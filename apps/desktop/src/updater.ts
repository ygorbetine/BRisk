import { app, BrowserWindow } from "electron";
import electronUpdater from "electron-updater";

const CHECK_INTERVAL_MS = 4 * 60 * 60 * 1000;

export type UpdateState = {
  phase: "idle" | "checking" | "downloading" | "ready" | "error";
  version?: string;
  percent?: number;
  message?: string;
};

let state: UpdateState = { phase: "idle" };
let beforeInstallHook: () => void = () => undefined;
let wired = false;

function setState(next: UpdateState): void {
  state = next;
  for (const window of BrowserWindow.getAllWindows()) {
    if (!window.isDestroyed()) window.webContents.send("updater:state", state);
  }
}

export function getUpdateState(): UpdateState {
  return state;
}

/** Reinicia o app e instala a versão já baixada. */
export function installUpdateNow(): boolean {
  if (state.phase !== "ready") return false;
  beforeInstallHook();
  electronUpdater.autoUpdater.quitAndInstall(false, true);
  return true;
}

/**
 * Atualização automática pelo GitHub Releases: baixa em segundo plano e, quando pronta,
 * avisa na interface (Sobre e tela principal), onde há o botão "Reiniciar e instalar". Só roda no app
 * instalado (empacotado) e pode ser desligada com BRIKS_DISABLE_UPDATE=1.
 */
export function startAutoUpdate(_getWindow: () => BrowserWindow | undefined, beforeInstall: () => void): void {
  if (!app.isPackaged || process.env.BRIKS_DISABLE_UPDATE === "1") return;
  const { autoUpdater } = electronUpdater;
  beforeInstallHook = beforeInstall;
  autoUpdater.autoDownload = true;
  autoUpdater.autoInstallOnAppQuit = true;
  autoUpdater.allowPrerelease = false;
  wired = true;

  autoUpdater.on("checking-for-update", () => { if (state.phase !== "downloading" && state.phase !== "ready") setState({ phase: "checking" }); });
  autoUpdater.on("update-not-available", () => { if (state.phase !== "ready") setState({ phase: "idle" }); });
  autoUpdater.on("update-available", (info) => setState({ phase: "downloading", version: info.version, percent: 0 }));
  autoUpdater.on("download-progress", (progress) => setState({ phase: "downloading", version: state.version, percent: Math.round(progress.percent) }));
  autoUpdater.on("error", (error) => {
    console.warn("[updater] falha ao verificar atualização:", error?.message ?? error);
    if (state.phase !== "ready") setState({ phase: "error", message: error?.message ?? "Falha na atualização." });
  });
  autoUpdater.on("update-downloaded", (info) => {
    setState({ phase: "ready", version: info.version, percent: 100 });
  });

  const check = () => { void autoUpdater.checkForUpdates().catch(() => undefined); };
  setTimeout(check, 15_000);
  setInterval(check, CHECK_INTERVAL_MS).unref();
}

export type UpdateCheckOutcome = { status: "disabled" | "current" | "available" | "error"; version?: string; message?: string };

/** Verificação manual (botão em Sobre). A baixada continua automática e o estado chega pelo evento updater:state. */
export async function checkForUpdatesNow(): Promise<UpdateCheckOutcome> {
  if (!app.isPackaged || process.env.BRIKS_DISABLE_UPDATE === "1" || !wired) {
    return { status: "disabled", message: "A atualização só funciona no app instalado." };
  }
  if (state.phase === "ready") return { status: "available", version: state.version };
  try {
    const result = await electronUpdater.autoUpdater.checkForUpdates();
    const latest = result?.updateInfo?.version;
    if (latest && latest !== app.getVersion()) return { status: "available", version: latest };
    return { status: "current", version: app.getVersion() };
  } catch (error) {
    return { status: "error", message: error instanceof Error ? error.message : "Falha ao verificar." };
  }
}
