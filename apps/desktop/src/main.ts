import { app, BrowserWindow, dialog, type Tray } from "electron";
import { existsSync, mkdirSync, renameSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { DesktopBackendManager } from "./backend.js";
import { configurePlatformRuntime } from "./gpu.js";
import { registerDesktopIpc } from "./ipc/index.js";
import { registerPermissionPolicy } from "./permissions.js";
import {
  PACKAGED_ENTRY_URL,
  PACKAGED_HOST,
  PACKAGED_ORIGIN,
  PACKAGED_SCHEME,
  registerPackagedProtocol,
  registerBriksScheme,
} from "./protocol.js";
import { ScreenCaptureController } from "./screen-capture.js";
import { createApplicationTray, isTrayAvailable } from "./tray.js";
import { createMainWindow } from "./window.js";
import { startAutoUpdate } from "./updater.js";

/**
 * O pacote foi renomeado (@risk/desktop -> @briks/desktop), e a pasta de dados do Electron segue o nome.
 * Move a pasta antiga para a nova, preservando banco local, identidade e configurações; se não der,
 * continua usando a pasta antiga em vez de perder dados.
 */
function migrateLegacyUserData(): void {
  try {
    const appData = app.getPath("appData");
    const legacy = path.join(appData, "@risk", "desktop");
    const next = path.join(appData, "@briks", "desktop");
    if (existsSync(next) || !existsSync(legacy)) return;
    mkdirSync(path.dirname(next), { recursive: true });
    renameSync(legacy, next);
    console.info("[desktop] dados migrados para", next);
  } catch (error) {
    console.warn("[desktop] migração de dados falhou; mantendo a pasta antiga.", error);
    app.setPath("userData", path.join(app.getPath("appData"), "@risk", "desktop"));
  }
}

migrateLegacyUserData();
configurePlatformRuntime();
registerBriksScheme();

const rootDirectory = path.dirname(fileURLToPath(import.meta.url));
const DEVELOPMENT_ORIGINS = new Set(["http://localhost:5173", "http://127.0.0.1:5173"]);
const PACKAGED_ORIGIN_REPORT_FILE = process.env.BRIKS_PACKAGED_ORIGIN_REPORT_FILE?.trim();
const DEV_BACKEND_BRIDGE_FILE = path.resolve(rootDirectory, "../../../.briks/dev-backend.json");
const APP_ICON_PATH = app.isPackaged
  ? path.join(process.resourcesPath, "icon.png")
  : path.resolve(rootDirectory, "../build/icon.png");

let pageUrl = "http://localhost:5173";
let isQuitting = false;
let mainWindow: BrowserWindow | undefined;
let tray: Tray | undefined;

function isTrustedRendererUrl(value: string): boolean {
  try {
    const parsed = new URL(value);
    if (app.isPackaged) return parsed.protocol === `${PACKAGED_SCHEME}:` && parsed.host === PACKAGED_HOST;
    return DEVELOPMENT_ORIGINS.has(parsed.origin);
  } catch {
    return false;
  }
}

const backend = new DesktopBackendManager(
  rootDirectory,
  DEV_BACKEND_BRIDGE_FILE,
  () => isQuitting,
);
const screenCapture = new ScreenCaptureController(isTrustedRendererUrl);
registerDesktopIpc(backend, isTrustedRendererUrl);

function openMainWindow(): BrowserWindow {
  if (mainWindow && !mainWindow.isDestroyed()) return mainWindow;
  const window = createMainWindow({
    pageUrl,
    rootDirectory,
    iconPath: APP_ICON_PATH,
    packagedOriginReportFile: PACKAGED_ORIGIN_REPORT_FILE,
    isQuitting: () => isQuitting,
    trayAvailable: () => isTrayAvailable(tray),
    isTrustedRendererUrl,
    onClosed: (closedWindow) => {
      if (mainWindow === closedWindow) mainWindow = undefined;
    },
  });
  mainWindow = window;
  return window;
}

function showMainWindow(): void {
  const window = openMainWindow();
  if (window.isMinimized()) window.restore();
  if (!window.isVisible()) window.show();
  window.focus();
}

function quitApplication(): void {
  isQuitting = true;
  app.quit();
}

function restartApplication(): void {
  isQuitting = true;
  app.relaunch();
  app.quit();
}

const hasSingleInstanceLock = app.requestSingleInstanceLock();
if (!hasSingleInstanceLock) app.quit();

if (hasSingleInstanceLock) {
  app.on("second-instance", () => {
    if (app.isReady()) showMainWindow();
  });

  app.whenReady().then(async () => {
    if (app.isPackaged) {
      await registerPackagedProtocol(path.resolve(process.resourcesPath, "web"));
      pageUrl = PACKAGED_ENTRY_URL;
    } else {
      await backend.clearDevBridge();
      pageUrl = "http://localhost:5173";
    }

    const webOrigin = app.isPackaged ? PACKAGED_ORIGIN : pageUrl;
    await backend.start(webOrigin);
    registerPermissionPolicy(isTrustedRendererUrl);
    screenCapture.register();
    tray = createApplicationTray(APP_ICON_PATH, {
      show: showMainWindow,
      restart: restartApplication,
      quit: quitApplication,
    });
    openMainWindow();
    app.on("activate", showMainWindow);
    startAutoUpdate(() => BrowserWindow.getAllWindows()[0], () => { isQuitting = true; });
  }).catch((error) => {
    console.error("Falha ao iniciar o Briks", error);
    dialog.showErrorBox("Briks não conseguiu iniciar", error instanceof Error ? error.message : String(error));
    void backend.clearDevBridge();
    backend.stop();
    app.quit();
  });
}

app.on("before-quit", () => {
  isQuitting = true;
  tray?.destroy();
  tray = undefined;
  screenCapture.clear();
  void backend.clearDevBridge();
  backend.stop();
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin" && (!isTrayAvailable(tray) || Boolean(PACKAGED_ORIGIN_REPORT_FILE))) app.quit();
});
