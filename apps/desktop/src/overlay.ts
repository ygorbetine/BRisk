import { BrowserWindow, globalShortcut, ipcMain, screen } from "electron";
import path from "node:path";

const EDIT_SHORTCUT = "CommandOrControl+Shift+O";

export type OverlayCommand = { type: "select-live"; peerId: string } | { type: "exit-edit" };

export function isOverlayCommand(value: unknown): value is OverlayCommand {
  if (!value || typeof value !== "object") return false;
  const command = value as { type?: unknown; peerId?: unknown };
  if (command.type === "exit-edit") return true;
  return command.type === "select-live" && typeof command.peerId === "string" && command.peerId.length > 0 && command.peerId.length <= 200;
}

/**
 * Janela de sobreposição sobre jogos em janela ou janela sem bordas: transparente, sempre no topo,
 * sem foco e sem capturar o mouse (os cliques passam para o jogo). Só no modo de edição ela
 * recebe mouse e foco, para mover e ajustar os itens.
 */
export class OverlayController {
  private window: BrowserWindow | undefined;
  private editing = false;
  private lastState: unknown = null;

  constructor(
    private readonly options: {
      pageUrl: () => string;
      rootDirectory: string;
      getMainWindow: () => BrowserWindow | undefined;
      isTrustedRendererUrl: (value: string) => boolean;
    },
  ) {}

  register(): void {
    const trusted = (event: Electron.IpcMainEvent | Electron.IpcMainInvokeEvent) => {
      if (!this.options.isTrustedRendererUrl(event.sender.getURL())) throw new Error("Origem do renderer não autorizada.");
    };

    ipcMain.handle("overlay:set-visible", (event, visible: unknown) => {
      trusted(event);
      if (typeof visible !== "boolean") throw new Error("Estado do overlay inválido.");
      if (visible) this.show(); else this.hide();
    });

    ipcMain.handle("overlay:set-editing", (event, editing: unknown) => {
      trusted(event);
      if (typeof editing !== "boolean") throw new Error("Modo de edição inválido.");
      this.setEditing(editing);
    });

    ipcMain.on("overlay:state", (event, state: unknown) => {
      try { trusted(event); } catch { return; }
      this.lastState = state;
      this.sendToOverlay("overlay:state", state);
    });

    ipcMain.on("overlay:frame", (event, frame: unknown) => {
      try { trusted(event); } catch { return; }
      if (!(frame instanceof ArrayBuffer) && !ArrayBuffer.isView(frame)) return;
      const bytes = frame instanceof ArrayBuffer ? frame.byteLength : (frame as ArrayBufferView).byteLength;
      if (bytes > 2_000_000) return;
      this.sendToOverlay("overlay:frame", frame);
    });

    ipcMain.on("overlay:command", (event, command: unknown) => {
      try { trusted(event); } catch { return; }
      if (!isOverlayCommand(command)) return;
      if (command.type === "exit-edit") this.setEditing(false);
      this.options.getMainWindow()?.webContents.send("overlay:command", command);
    });
  }

  private sendToOverlay(channel: string, payload: unknown): void {
    const window = this.window;
    if (window && !window.isDestroyed()) window.webContents.send(channel, payload);
  }

  private show(): void {
    if (this.window && !this.window.isDestroyed()) return;
    const display = screen.getPrimaryDisplay();
    const window = new BrowserWindow({
      x: display.bounds.x,
      y: display.bounds.y,
      width: display.bounds.width,
      height: display.bounds.height,
      transparent: true,
      backgroundColor: "#00000000",
      frame: false,
      hasShadow: false,
      resizable: false,
      movable: false,
      minimizable: false,
      maximizable: false,
      fullscreenable: false,
      skipTaskbar: true,
      focusable: false,
      show: false,
      webPreferences: {
        preload: path.join(this.options.rootDirectory, "preload.cjs"),
        contextIsolation: true,
        nodeIntegration: false,
        sandbox: true,
        backgroundThrottling: false,
      },
    });
    this.window = window;
    window.setAlwaysOnTop(true, "screen-saver");
    window.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });
    window.setIgnoreMouseEvents(true, { forward: true });
    window.setMenuBarVisibility(false);
    window.webContents.setWindowOpenHandler(() => ({ action: "deny" }));
    window.webContents.on("will-navigate", (event) => event.preventDefault());
    window.webContents.once("did-finish-load", () => {
      if (window.isDestroyed()) return;
      if (this.lastState !== null) window.webContents.send("overlay:state", this.lastState);
      window.showInactive();
    });
    window.on("closed", () => {
      if (this.window === window) this.window = undefined;
      this.editing = false;
      globalShortcut.unregister(EDIT_SHORTCUT);
    });
    globalShortcut.unregister(EDIT_SHORTCUT);
    globalShortcut.register(EDIT_SHORTCUT, () => this.setEditing(!this.editing));
    void window.loadURL(`${this.options.pageUrl()}#overlay`).catch((error) => {
      console.error("Falha ao carregar o overlay", error);
      this.hide();
    });
  }

  private hide(): void {
    globalShortcut.unregister(EDIT_SHORTCUT);
    const window = this.window;
    this.window = undefined;
    this.editing = false;
    if (window && !window.isDestroyed()) window.destroy();
  }

  private setEditing(editing: boolean): void {
    const window = this.window;
    if (!window || window.isDestroyed()) return;
    this.editing = editing;
    if (editing) {
      window.setFocusable(true);
      window.setIgnoreMouseEvents(false);
      window.focus();
    } else {
      window.setIgnoreMouseEvents(true, { forward: true });
      window.setFocusable(false);
      window.blur();
    }
    window.webContents.send("overlay:editing", editing);
  }

  dispose(): void {
    this.hide();
  }
}
