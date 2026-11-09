import { app, BrowserWindow, screen } from "electron";
import { writeFile } from "node:fs/promises";
import path from "node:path";
import { desktopWindowBounds, shouldHideWindowOnClose } from "./desktop-lifecycle.js";

export type MainWindowOptions = {
  pageUrl: string;
  rootDirectory: string;
  iconPath: string;
  packagedOriginReportFile?: string;
  isQuitting: () => boolean;
  trayAvailable: () => boolean;
  isTrustedRendererUrl: (value: string) => boolean;
  onClosed: (window: BrowserWindow) => void;
};

type PackagedSmokeResult = {
  origin?: unknown;
  href?: unknown;
  rootChildren?: unknown;
  rootTextLength?: unknown;
  backendHealth?: unknown;
  backendLoopback?: unknown;
  storageWritable?: unknown;
  indexedDbWritable?: unknown;
};

export function createMainWindow(options: MainWindowOptions): BrowserWindow {
  const reportingPackagedOrigin = app.isPackaged && Boolean(options.packagedOriginReportFile);
  let rendererRecoveryAttempted = false;
  const bounds = desktopWindowBounds(screen.getPrimaryDisplay().workAreaSize);
  const window = new BrowserWindow({
    width: bounds.width,
    height: bounds.height,
    minWidth: bounds.minWidth,
    minHeight: bounds.minHeight,
    backgroundColor: "#12041a",
    icon: options.iconPath,
    show: false,
    autoHideMenuBar: false,
    webPreferences: {
      preload: path.join(options.rootDirectory, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      backgroundThrottling: false,
    },
  });

  // Sem barra de menu (Alt não deve mostrar File/Edit/View/Window); copiar e colar continuam funcionando nos campos.
  window.removeMenu();
  window.setMenuBarVisibility(false);

  window.on("close", (event) => {
    if (!shouldHideWindowOnClose({
      isQuitting: options.isQuitting(),
      trayAvailable: options.trayAvailable(),
      automatedRun: reportingPackagedOrigin,
    })) return;
    event.preventDefault();
    window.hide();
  });
  window.on("closed", () => options.onClosed(window));
  window.webContents.setWebRTCIPHandlingPolicy("default");
  window.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
  window.webContents.on("will-navigate", (event, targetUrl) => {
    if (!options.isTrustedRendererUrl(targetUrl)) event.preventDefault();
  });
  window.webContents.on("render-process-gone", (_event, details) => {
    console.error("Renderer do Briks encerrou inesperadamente", details);
    if (rendererRecoveryAttempted || window.isDestroyed() || options.isQuitting()) return;
    rendererRecoveryAttempted = true;
    setTimeout(() => { if (!window.isDestroyed()) void window.loadURL(options.pageUrl); }, 500);
  });
  window.webContents.on("did-fail-load", (_event, errorCode, errorDescription, validatedUrl, isMainFrame) => {
    if (!isMainFrame || errorCode === -3 || rendererRecoveryAttempted || window.isDestroyed() || options.isQuitting()) return;
    console.error("Falha ao carregar renderer do Briks", { errorCode, errorDescription, validatedUrl });
    rendererRecoveryAttempted = true;
    setTimeout(() => { if (!window.isDestroyed()) void window.loadURL(options.pageUrl); }, 500);
  });
  window.webContents.once("did-finish-load", () => {
    if (!reportingPackagedOrigin || !options.packagedOriginReportFile) return;
    void reportPackagedRenderer(window, options.packagedOriginReportFile);
  });
  window.once("ready-to-show", () => {
    if (!reportingPackagedOrigin) window.show();
  });
  void window.loadURL(options.pageUrl).catch((error) => {
    console.error("Falha ao carregar a interface do Briks", error);
    if (!reportingPackagedOrigin) window.show();
    else app.exit(1);
  });
  return window;
}

async function reportPackagedRenderer(window: BrowserWindow, reportFile: string): Promise<void> {
  try {
    const location = await window.webContents.executeJavaScript(
      `(async () => {
        const deadline = Date.now() + 10_000;
        while ((document.getElementById("root")?.childElementCount ?? 0) === 0 && Date.now() < deadline) {
          await new Promise((resolve) => setTimeout(resolve, 50));
        }
        const root = document.getElementById("root");
        let backendHealth = false;
        let backendLoopback = false;
        let storageWritable = false;
        let indexedDbWritable = false;
        try {
          const config = await window.desktop.getBackendConfig();
          const backendUrl = new URL(config.baseUrl);
          backendLoopback = backendUrl.protocol === "http:" && backendUrl.hostname === "127.0.0.1" && config.token.length >= 32;
          const normalizedBaseUrl = config.baseUrl.endsWith("/") ? config.baseUrl.slice(0, -1) : config.baseUrl;
          const response = await fetch(normalizedBaseUrl + "/health");
          backendHealth = response.ok;
        } catch {}
        try {
          const marker = "briks-packaged-smoke";
          localStorage.setItem(marker, "ok");
          storageWritable = localStorage.getItem(marker) === "ok";
          localStorage.removeItem(marker);
        } catch {}
        try {
          indexedDbWritable = await new Promise((resolve) => {
            const request = indexedDB.open("briks-packaged-smoke", 1);
            request.onupgradeneeded = () => request.result.createObjectStore("health");
            request.onerror = () => resolve(false);
            request.onsuccess = () => {
              request.result.close();
              const deletion = indexedDB.deleteDatabase("briks-packaged-smoke");
              deletion.onerror = () => resolve(false);
              deletion.onsuccess = () => resolve(true);
            };
          });
        } catch {}
        return {
          origin: window.location.origin,
          href: window.location.href,
          rootChildren: root?.childElementCount ?? 0,
          rootTextLength: root?.textContent?.trim().length ?? 0,
          backendHealth,
          backendLoopback,
          storageWritable,
          indexedDbWritable,
        };
      })()`,
      true,
    ) as PackagedSmokeResult;
    await writeFile(reportFile, JSON.stringify({ packaged: app.isPackaged, ...location }), "utf8");
    app.quit();
  } catch (error) {
    console.error("Falha ao verificar a origem do renderer empacotado", error);
    app.exit(1);
  }
}
