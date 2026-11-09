import { globalShortcut, ipcMain, type BrowserWindow } from "electron";

export const CALL_SHORTCUT_ACTIONS = ["mute", "deafen", "stream"] as const;
export type CallShortcutAction = (typeof CALL_SHORTCUT_ACTIONS)[number];

const ACCELERATOR_PATTERN = /^(?:(?:Control|Alt|Shift|Super)\+){0,3}[A-Za-z0-9`\-=[\];',./\\]{1,12}$/;

/** Valida o pedido do renderer: só as três ações conhecidas, cada uma com acelerador curto ou `null`. */
export function parseShortcutRequest(value: unknown): Partial<Record<CallShortcutAction, string>> | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const input = value as Record<string, unknown>;
  const result: Partial<Record<CallShortcutAction, string>> = {};
  for (const [key, accelerator] of Object.entries(input)) {
    if (!(CALL_SHORTCUT_ACTIONS as readonly string[]).includes(key)) return null;
    if (accelerator === null) continue;
    if (typeof accelerator !== "string" || accelerator.length > 40 || !ACCELERATOR_PATTERN.test(accelerator)) return null;
    // Uma letra sem Ctrl/Alt/Win engoliria a tecla em todos os programas.
    if (!/^F\d+$/.test(accelerator.split("+").pop()!) && !/(^|\+)(Control|Alt|Super)\+/.test(accelerator)) return null;
    result[key as CallShortcutAction] = accelerator;
  }
  return result;
}

/**
 * Atalhos globais da chamada (mutar, ensurdecer, transmitir). O renderer registra ao entrar na chamada
 * e limpa ao sair, para não segurar teclas quando não há chamada.
 */
export class CallShortcuts {
  private active: string[] = [];

  constructor(private readonly options: { getMainWindow: () => BrowserWindow | undefined; isTrustedRendererUrl: (value: string) => boolean }) {}

  register(): void {
    ipcMain.handle("shortcuts:set", (event, value: unknown) => {
      if (!this.options.isTrustedRendererUrl(event.sender.getURL())) throw new Error("Origem do renderer não autorizada.");
      const request = parseShortcutRequest(value);
      if (!request) throw new Error("Atalhos inválidos.");
      return this.apply(request);
    });
  }

  private apply(request: Partial<Record<CallShortcutAction, string>>): { failed: CallShortcutAction[] } {
    this.clear();
    const failed: CallShortcutAction[] = [];
    for (const action of CALL_SHORTCUT_ACTIONS) {
      const accelerator = request[action];
      if (!accelerator || this.active.includes(accelerator)) { if (accelerator) failed.push(action); continue; }
      try {
        if (globalShortcut.register(accelerator, () => this.fire(action))) this.active.push(accelerator);
        else failed.push(action);
      } catch { failed.push(action); }
    }
    return { failed };
  }

  private fire(action: CallShortcutAction): void {
    const window = this.options.getMainWindow();
    if (!window || window.isDestroyed()) return;
    // Transmitir abre o seletor de tela: a janela precisa aparecer.
    if (action === "stream") { if (window.isMinimized()) window.restore(); window.show(); window.focus(); }
    window.webContents.send("shortcuts:action", action);
  }

  clear(): void {
    for (const accelerator of this.active) globalShortcut.unregister(accelerator);
    this.active = [];
  }
}
