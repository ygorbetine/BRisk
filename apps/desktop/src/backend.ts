import { app, BrowserWindow } from "electron";
import { spawn, type ChildProcess } from "node:child_process";
import { randomBytes } from "node:crypto";
import { constants as fsConstants } from "node:fs";
import { access, mkdir, unlink, writeFile } from "node:fs/promises";
import path from "node:path";
import {
  MAX_BACKEND_RESTART_ATTEMPTS,
  shouldAttemptBackendRestart,
} from "./desktop-lifecycle.js";

export type BackendConfig = { baseUrl: string; token: string };
export type BackendStatus = { state: "restarting" | "recovered" | "failed"; message: string };

const BACKEND_STABLE_RESET_MS = 30_000;

export class DesktopBackendManager {
  private process: ChildProcess | undefined;
  private config: BackendConfig | undefined;
  private webOrigin = "";
  private restartAttempts = 0;
  private restarting = false;
  private stabilityTimer: ReturnType<typeof setTimeout> | undefined;

  constructor(
    private readonly rootDirectory: string,
    private readonly devBridgeFile: string,
    private readonly isQuitting: () => boolean,
  ) {}

  getConfig(): BackendConfig | undefined {
    return this.config;
  }

  async start(webOrigin: string): Promise<BackendConfig> {
    this.webOrigin = webOrigin;
    const config = await this.startProcess(webOrigin);
    this.config = config;
    await this.publishDevBridge(config);
    this.markHealthy();
    return config;
  }

  async clearDevBridge(): Promise<void> {
    if (app.isPackaged) return;
    await unlink(this.devBridgeFile).catch((error: NodeJS.ErrnoException) => {
      if (error.code !== "ENOENT") console.warn("Falha ao remover bridge temporário do backend", error);
    });
  }

  stop(): void {
    this.clearStabilityTimer();
    const child = this.process;
    this.process = undefined;
    this.config = undefined;
    if (!child || child.killed) return;
    child.stdin?.end();
    const forceTimer = setTimeout(() => {
      if (!child.killed) child.kill();
    }, 1_500);
    child.once("exit", () => clearTimeout(forceTimer));
  }

  private executableName(): string {
    return process.platform === "win32" ? "briks-desktop-backend.exe" : "briks-desktop-backend";
  }

  private executablePath(): string {
    const override = process.env.BRIKS_BACKEND_BIN?.trim();
    if (override) return path.resolve(override);
    if (app.isPackaged) return path.join(process.resourcesPath, "backend", this.executableName());
    return path.resolve(this.rootDirectory, "../../../desktop-backend/target/debug", this.executableName());
  }

  private clearStabilityTimer(): void {
    if (this.stabilityTimer) clearTimeout(this.stabilityTimer);
    this.stabilityTimer = undefined;
  }

  private markHealthy(): void {
    this.clearStabilityTimer();
    this.stabilityTimer = setTimeout(() => {
      this.restartAttempts = 0;
      this.stabilityTimer = undefined;
    }, BACKEND_STABLE_RESET_MS);
  }

  private async publishDevBridge(config: BackendConfig): Promise<void> {
    if (app.isPackaged) return;
    await mkdir(path.dirname(this.devBridgeFile), { recursive: true });
    await writeFile(this.devBridgeFile, JSON.stringify(config), { encoding: "utf8", mode: 0o600 });
  }

  private async startProcess(webOrigin: string): Promise<BackendConfig> {
    const executable = this.executablePath();
    await access(executable, process.platform === "win32" ? fsConstants.F_OK : fsConstants.X_OK).catch(() => {
      throw new Error(`Backend Rust não encontrado ou não executável em ${executable}`);
    });
    const token = randomBytes(32).toString("base64url");
    const child = spawn(executable, [], {
      windowsHide: true,
      stdio: ["pipe", "pipe", "pipe"],
      env: {
        ...process.env,
        BRIKS_DATA_DIR: app.getPath("userData"),
        BRIKS_LOCAL_TOKEN: token,
        BRIKS_WEB_ORIGIN: webOrigin,
        BRIKS_BACKEND_BIND: "127.0.0.1:0",
        RUST_LOG: process.env.RUST_LOG ?? "briks_desktop_backend=info,tower_http=warn",
      },
    });
    this.process = child;
    child.stderr?.setEncoding("utf8");
    child.stderr?.on("data", (chunk: string) => console.error(`[briks-backend] ${chunk.trimEnd()}`));

    const baseUrl = await this.waitUntilReady(child);
    try {
      const response = await fetch(`${baseUrl}/health`, { signal: AbortSignal.timeout(5_000) });
      if (!response.ok) throw new Error(`Healthcheck do backend falhou com HTTP ${response.status}.`);
    } catch (error) {
      if (this.process === child) this.process = undefined;
      child.stdin?.end();
      if (!child.killed) child.kill();
      throw error;
    }

    child.once("exit", (code, signal) => {
      if (this.process !== child) return;
      this.clearStabilityTimer();
      this.process = undefined;
      this.config = undefined;
      if (!this.isQuitting()) void this.recoverAfterCrash(code, signal);
    });
    return { baseUrl, token };
  }

  private async waitUntilReady(child: ChildProcess): Promise<string> {
    return await new Promise<string>((resolve, reject) => {
      let settled = false;
      let stdoutBuffer = "";
      const timeout = setTimeout(() => {
        if (settled) return;
        settled = true;
        reject(new Error("O backend local não ficou pronto dentro de 20 segundos."));
      }, 20_000);
      const cleanup = () => clearTimeout(timeout);
      child.once("error", (error) => {
        if (settled) return;
        settled = true;
        if (this.process === child) this.process = undefined;
        cleanup();
        reject(error);
      });
      child.once("exit", (code, signal) => {
        if (settled) return;
        settled = true;
        if (this.process === child) this.process = undefined;
        cleanup();
        reject(new Error(`Backend local encerrou antes do readiness (code=${code ?? "?"}, signal=${signal ?? "?"}).`));
      });
      child.stdout?.setEncoding("utf8");
      child.stdout?.on("data", (chunk: string) => {
        stdoutBuffer += chunk;
        let newline = stdoutBuffer.indexOf("\n");
        while (newline >= 0) {
          const line = stdoutBuffer.slice(0, newline).trim();
          stdoutBuffer = stdoutBuffer.slice(newline + 1);
          if (line.startsWith("BRIKS_BACKEND_READY ")) {
            try {
              const payload = JSON.parse(line.slice("BRIKS_BACKEND_READY ".length)) as { url?: unknown };
              if (typeof payload.url !== "string" || !payload.url.startsWith("http://127.0.0.1:")) {
                throw new Error("URL de readiness inválida.");
              }
              if (!settled) {
                settled = true;
                cleanup();
                resolve(payload.url);
              }
            } catch (error) {
              if (!settled) {
                settled = true;
                cleanup();
                reject(error);
              }
            }
          } else if (line) {
            console.log(`[briks-backend] ${line}`);
          }
          newline = stdoutBuffer.indexOf("\n");
        }
      });
    });
  }

  private broadcastStatus(payload: BackendStatus): void {
    for (const window of BrowserWindow.getAllWindows()) {
      if (!window.isDestroyed()) window.webContents.send("backend:status", payload);
    }
  }

  private async recoverAfterCrash(code: number | null, signal: NodeJS.Signals | null): Promise<void> {
    if (!shouldAttemptBackendRestart({
      isQuitting: this.isQuitting(),
      restarting: this.restarting,
      attempts: this.restartAttempts,
    })) {
      if (this.restarting || this.isQuitting()) return;
      const message = `O backend local encerrou inesperadamente (code=${code ?? "?"}, signal=${signal ?? "?"}) e não pôde ser reiniciado.`;
      console.error(message);
      this.broadcastStatus({ state: "failed", message });
      return;
    }
    this.restarting = true;
    let lastError = `code=${code ?? "?"}, signal=${signal ?? "?"}`;
    try {
      while (!this.isQuitting() && this.restartAttempts < MAX_BACKEND_RESTART_ATTEMPTS) {
        this.restartAttempts += 1;
        const attempt = this.restartAttempts;
        this.broadcastStatus({
          state: "restarting",
          message: `O backend local parou. Tentando recuperar a sessão (${attempt}/${MAX_BACKEND_RESTART_ATTEMPTS})…`,
        });
        await new Promise((resolve) => setTimeout(resolve, Math.min(3_000, 750 * attempt)));
        if (this.isQuitting()) return;
        try {
          const config = await this.startProcess(this.webOrigin);
          this.config = config;
          await this.publishDevBridge(config);
          if (!this.process || this.config !== config) throw new Error("O backend encerrou durante a tentativa de recuperação.");
          this.markHealthy();
          this.broadcastStatus({ state: "recovered", message: "Backend local recuperado. Você já pode continuar." });
          return;
        } catch (error) {
          lastError = error instanceof Error ? error.message : String(error);
          console.error(`Falha ao reiniciar o backend local (${attempt}/${MAX_BACKEND_RESTART_ATTEMPTS})`, error);
          this.stop();
        }
      }
      if (!this.isQuitting()) {
        this.broadcastStatus({ state: "failed", message: `Não foi possível recuperar o backend local: ${lastError}` });
      }
    } finally {
      this.restarting = false;
    }
  }
}
