const { contextBridge, ipcRenderer } = require("electron") as typeof import("electron");

export type DesktopSource = {
  id: string;
  name: string;
  displayId: string;
  thumbnail: string;
};

export type DesktopBackendConfig = {
  baseUrl: string;
  token: string;
};

export type DesktopBackendStatus = {
  state: "restarting" | "recovered" | "failed";
  message: string;
};

export type DesktopNetworkInterface = {
  name: string;
  address: string;
  family: "IPv4" | "IPv6";
  netmask?: string;
  cidr?: string;
  provider: "zerotier" | "tailscale" | "wireguard" | "vpn" | "unknown";
};

const MIN_FULLSCREEN_ZOOM = 0.5;
const MAX_FULLSCREEN_ZOOM = 5;
const FULLSCREEN_ZOOM_FACTOR = 1.12;
const SWALLOW_CLICK_MS = 900;
let activeFullscreenTile: HTMLElement | null = null;
let activeFullscreenWorkspace: HTMLElement | null = null;
let activeFullscreenZoom = 1;
let swallowFullscreenClickUntil = 0;
let fullscreenTransition: Promise<void> = Promise.resolve();
let htmlFullscreenFallback = false;


function closestFromEventTarget(target: EventTarget | null, selector: string): HTMLElement | null {
  if (!target) return null;
  const candidate = target as EventTarget & { closest?: (value: string) => Element | null };
  if (typeof candidate.closest !== "function") return null;
  return candidate.closest(selector) as HTMLElement | null;
}

function clearFullscreenZoomStyles(tile: HTMLElement | null): void {
  activeFullscreenZoom = 1;
  if (!tile) return;
  tile.style.removeProperty("--briks-fullscreen-zoom");
  tile.style.removeProperty("--briks-fullscreen-origin-x");
  tile.style.removeProperty("--briks-fullscreen-origin-y");
}

function setFullscreenZoom(tile: HTMLElement, zoom: number, originX: number, originY: number): void {
  activeFullscreenZoom = zoom;
  tile.style.setProperty("--briks-fullscreen-zoom", zoom.toFixed(3));
  tile.style.setProperty("--briks-fullscreen-origin-x", `${originX.toFixed(2)}%`);
  tile.style.setProperty("--briks-fullscreen-origin-y", `${originY.toFixed(2)}%`);
}

function adjustFullscreenZoom(event: WheelEvent): void {
  const tile = activeFullscreenTile;
  if (!tile || event.deltaY === 0) return;

  event.preventDefault();
  event.stopPropagation();
  event.stopImmediatePropagation();

  const factor = event.deltaY < 0 ? FULLSCREEN_ZOOM_FACTOR : 1 / FULLSCREEN_ZOOM_FACTOR;
  const nextZoom = Math.min(MAX_FULLSCREEN_ZOOM, Math.max(MIN_FULLSCREEN_ZOOM, activeFullscreenZoom * factor));
  if (Math.abs(nextZoom - activeFullscreenZoom) < 0.001) return;

  const rect = tile.getBoundingClientRect();
  const relativeX = rect.width > 0 ? (event.clientX - rect.left) / rect.width : 0.5;
  const relativeY = rect.height > 0 ? (event.clientY - rect.top) / rect.height : 0.5;
  const originX = Math.min(1, Math.max(0, relativeX)) * 100;
  const originY = Math.min(1, Math.max(0, relativeY)) * 100;

  setFullscreenZoom(tile, nextZoom, originX, originY);
}

function cleanupFullscreenDom(): void {
  clearFullscreenZoomStyles(activeFullscreenTile);
  activeFullscreenTile?.removeAttribute("data-briks-native-fullscreen");
  activeFullscreenWorkspace?.removeAttribute("data-briks-native-fullscreen-active");
  document.documentElement.classList.remove("briks-native-stream-fullscreen");
  document.body.classList.remove("briks-native-stream-fullscreen");
  activeFullscreenTile = null;
  activeFullscreenWorkspace = null;
}

async function exitHtmlFullscreen(): Promise<void> {
  if (!document.fullscreenElement) return;
  await document.exitFullscreen().catch(() => undefined);
}

async function exitNativeStreamFullscreen(): Promise<void> {
  htmlFullscreenFallback = false;
  await exitHtmlFullscreen();
  cleanupFullscreenDom();
  await ipcRenderer.invoke("window:fullscreen", false).catch(() => undefined);
}

async function performEnterNativeStreamFullscreen(tile: HTMLElement): Promise<void> {
  if (activeFullscreenTile === tile && !htmlFullscreenFallback) {
    await exitNativeStreamFullscreen();
    return;
  }

  if (activeFullscreenTile) await exitNativeStreamFullscreen();
  htmlFullscreenFallback = false;
  await exitHtmlFullscreen();

  const workspace = tile.closest<HTMLElement>(".call-workspace");
  if (!workspace || !tile.isConnected) return;

  activeFullscreenTile = tile;
  activeFullscreenWorkspace = workspace;
  setFullscreenZoom(tile, 1, 50, 50);
  tile.setAttribute("data-briks-native-fullscreen", "true");
  workspace.setAttribute("data-briks-native-fullscreen-active", "true");
  document.documentElement.classList.add("briks-native-stream-fullscreen");
  document.body.classList.add("briks-native-stream-fullscreen");

  try {
    await ipcRenderer.invoke("window:fullscreen", true);
  } catch (error) {
    cleanupFullscreenDom();
    htmlFullscreenFallback = true;
    try {
      await tile.requestFullscreen();
    } catch {
      htmlFullscreenFallback = false;
      console.warn("Briks não conseguiu entrar em tela cheia.", error);
    }
  }
}

function enterNativeStreamFullscreen(tile: HTMLElement): Promise<void> {
  fullscreenTransition = fullscreenTransition
    .catch(() => undefined)
    .then(() => performEnterNativeStreamFullscreen(tile));
  return fullscreenTransition;
}

function tileFromFullscreenButton(event: Event): HTMLElement | null {
  const button = closestFromEventTarget(event.target, ".tile-fullscreen");
  if (!button) return null;
  return button.closest<HTMLElement>(".call-workspace .tile");
}

function suppressEvent(event: Event): void {
  event.preventDefault();
  event.stopPropagation();
  event.stopImmediatePropagation();
}

function installNativeStreamFullscreenController(): void {
  // Começa no pointerdown e engole o click posterior. Assim o requestFullscreen()
  // legado do React nunca concorre com o fullscreen nativo da BrowserWindow.
  // A busca por closest() é deliberadamente duck-typed: o preload roda em um
  // isolated world e instanceof Element/HTMLElement não é confiável entre mundos.
  window.addEventListener("pointerdown", (event) => {
    const tile = tileFromFullscreenButton(event);
    if (!tile) return;
    suppressEvent(event);
    swallowFullscreenClickUntil = Date.now() + SWALLOW_CLICK_MS;
    void enterNativeStreamFullscreen(tile);
  }, true);

  window.addEventListener("click", (event) => {
    const tile = tileFromFullscreenButton(event);
    if (!tile) return;
    suppressEvent(event);
    if (Date.now() <= swallowFullscreenClickUntil) return;
    // Ativação por teclado não gera pointerdown; ainda deve funcionar.
    void enterNativeStreamFullscreen(tile);
  }, true);

  window.addEventListener("dblclick", (event) => {
    const tile = closestFromEventTarget(event.target, ".call-workspace .tile");
    if (!tile) return;
    suppressEvent(event);
    void enterNativeStreamFullscreen(tile);
  }, true);

  window.addEventListener("wheel", (event) => {
    if (!activeFullscreenTile) return;
    adjustFullscreenZoom(event);
  }, { capture: true, passive: false });

  window.addEventListener("keydown", (event) => {
    if (event.key !== "Escape" || !activeFullscreenTile) return;
    suppressEvent(event);
    fullscreenTransition = fullscreenTransition
      .catch(() => undefined)
      .then(() => exitNativeStreamFullscreen());
  }, true);

  // Se algum requestFullscreen() HTML legado ainda escapar, convertemos para o
  // modo nativo. Quando o HTML fullscreen é o fallback deliberado porque o IPC
  // falhou, não tentamos converter novamente para evitar um loop.
  document.addEventListener("fullscreenchange", () => {
    if (htmlFullscreenFallback) return;
    const tile = closestFromEventTarget(document.fullscreenElement, ".call-workspace .tile");
    if (!tile || document.fullscreenElement !== tile) return;
    void exitHtmlFullscreen().then(() => enterNativeStreamFullscreen(tile));
  });

  const observer = new MutationObserver(() => {
    if (activeFullscreenTile && !activeFullscreenTile.isConnected) {
      fullscreenTransition = fullscreenTransition
        .catch(() => undefined)
        .then(() => exitNativeStreamFullscreen());
    }
  });
  observer.observe(document.documentElement, { childList: true, subtree: true });
}

if (document.readyState === "loading") {
  window.addEventListener("DOMContentLoaded", installNativeStreamFullscreenController, { once: true });
} else {
  installNativeStreamFullscreenController();
}

contextBridge.exposeInMainWorld("desktop", {
  listScreenSources: (): Promise<DesktopSource[]> => ipcRenderer.invoke("screen:list"),
  chooseScreenSource: (): Promise<string | null> => ipcRenderer.invoke("screen:choose"),
  selectScreenSource: (sourceId: string): Promise<void> => ipcRenderer.invoke("screen:select", sourceId),
  setWindowFullscreen: (enabled: boolean): Promise<{ fullscreen: boolean }> => ipcRenderer.invoke("window:fullscreen", enabled),
  getBackendConfig: (): Promise<DesktopBackendConfig> => ipcRenderer.invoke("backend:config"),
  getNetworkInterfaces: (): Promise<DesktopNetworkInterface[]> => ipcRenderer.invoke("network:interfaces"),
  showEmojiPanel: (): Promise<boolean> => ipcRenderer.invoke("app:emoji-panel"),
  getAppIcon: (path: string): Promise<string | null> => ipcRenderer.invoke("app:icon", path),
  checkForUpdates: (): Promise<{ status: "disabled" | "current" | "available" | "error"; version?: string; message?: string }> => ipcRenderer.invoke("updater:check"),
  getUpdateState: (): Promise<{ phase: "idle" | "checking" | "downloading" | "ready" | "error"; version?: string; percent?: number; message?: string }> => ipcRenderer.invoke("updater:state"),
  installUpdate: (): Promise<boolean> => ipcRenderer.invoke("updater:install"),
  onUpdateState: (callback: (state: { phase: "idle" | "checking" | "downloading" | "ready" | "error"; version?: string; percent?: number; message?: string }) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, state: { phase: "idle" | "checking" | "downloading" | "ready" | "error"; version?: string; percent?: number; message?: string }) => callback(state);
    ipcRenderer.on("updater:state", listener);
    return () => ipcRenderer.removeListener("updater:state", listener);
  },
  openExternal: (url: string): Promise<void> => ipcRenderer.invoke("shell:open-external", url),
  setCallShortcuts: (accelerators: Record<string, string | null>): Promise<{ failed: string[] }> => ipcRenderer.invoke("shortcuts:set", accelerators),
  onCallShortcut: (callback: (action: string) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, action: unknown) => { if (typeof action === "string") callback(action); };
    ipcRenderer.on("shortcuts:action", listener);
    return () => ipcRenderer.removeListener("shortcuts:action", listener);
  },
  overlay: {
    setVisible: (visible: boolean): Promise<void> => ipcRenderer.invoke("overlay:set-visible", visible),
    setEditing: (editing: boolean): Promise<void> => ipcRenderer.invoke("overlay:set-editing", editing),
    pushState: (state: unknown): void => ipcRenderer.send("overlay:state", state),
    pushFrame: (frame: ArrayBuffer, peerId: string): void => ipcRenderer.send("overlay:frame", frame, peerId),
    sendCommand: (command: unknown): void => ipcRenderer.send("overlay:command", command),
    onState: (callback: (state: unknown) => void): (() => void) => {
      const listener = (_event: Electron.IpcRendererEvent, state: unknown) => callback(state);
      ipcRenderer.on("overlay:state", listener);
      return () => ipcRenderer.removeListener("overlay:state", listener);
    },
    onFrame: (callback: (frame: ArrayBuffer, peerId: string) => void): (() => void) => {
      const listener = (_event: Electron.IpcRendererEvent, frame: ArrayBuffer | Uint8Array, peerId: unknown) => {
        if (typeof peerId !== "string") return;
        callback(frame instanceof Uint8Array ? (frame.buffer.slice(frame.byteOffset, frame.byteOffset + frame.byteLength) as ArrayBuffer) : frame, peerId);
      };
      ipcRenderer.on("overlay:frame", listener);
      return () => ipcRenderer.removeListener("overlay:frame", listener);
    },
    onEditing: (callback: (editing: boolean) => void): (() => void) => {
      const listener = (_event: Electron.IpcRendererEvent, editing: boolean) => callback(Boolean(editing));
      ipcRenderer.on("overlay:editing", listener);
      return () => ipcRenderer.removeListener("overlay:editing", listener);
    },
    onCommand: (callback: (command: unknown) => void): (() => void) => {
      const listener = (_event: Electron.IpcRendererEvent, command: unknown) => callback(command);
      ipcRenderer.on("overlay:command", listener);
      return () => ipcRenderer.removeListener("overlay:command", listener);
    },
  },
  onBackendStatus: (callback: (status: DesktopBackendStatus) => void): (() => void) => {
    const listener = (_event: Electron.IpcRendererEvent, status: DesktopBackendStatus) => callback(status);
    ipcRenderer.on("backend:status", listener);
    return () => ipcRenderer.removeListener("backend:status", listener);
  },
});
