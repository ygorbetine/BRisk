/**
 * Atalhos da chamada. Mutar, ensurdecer e transmitir viram atalhos globais do Electron (só durante a chamada).
 * O push-to-talk lê o estado da tecla pelo backend (aperta = fala, solta = muta), sem bloquear a tecla para os outros programas.
 */
export type ShortcutAction = "mute" | "deafen" | "stream";
export const SHORTCUT_ACTIONS: ReadonlyArray<{ id: ShortcutAction; label: string }> = [
  { id: "mute", label: "Mutar / desmutar microfone" },
  { id: "deafen", label: "Ensurdecer" },
  { id: "stream", label: "Transmitir / parar" },
];

/** Tecla para o push-to-talk: código do Windows (virtual key) + nome para exibir. */
export type PttKey = { vk: number; label: string };
export type ShortcutSettings = {
  /** Acelerador do Electron (ex.: "Control+Shift+M"); `null` = sem atalho. */
  accelerators: Record<ShortcutAction, string | null>;
  pushToTalk: { enabled: boolean; key: PttKey | null; releaseDelayMs: number };
};

const KEY = "briks.shortcuts.v1";
const CHANGE_EVENT = "briks:shortcut-settings";

export function defaultShortcutSettings(): ShortcutSettings {
  return { accelerators: { mute: "Control+Shift+M", deafen: "Control+Shift+D", stream: null }, pushToTalk: { enabled: false, key: null, releaseDelayMs: 200 } };
}

const ACCELERATOR_PATTERN = /^(?:(?:Control|Alt|Shift|Super)\+){0,3}[A-Za-z0-9`\-=[\];',./\\]{1,12}$/;

export function isValidAccelerator(value: unknown): value is string {
  return typeof value === "string" && value.length <= 40 && ACCELERATOR_PATTERN.test(value);
}

function validPttKey(value: unknown): PttKey | null {
  if (!value || typeof value !== "object") return null;
  const key = value as Record<string, unknown>;
  if (typeof key.vk !== "number" || !isAllowedVk(key.vk) || typeof key.label !== "string") return null;
  return { vk: key.vk, label: key.label.slice(0, 24) };
}

export function loadShortcutSettings(): ShortcutSettings {
  const fallback = defaultShortcutSettings();
  try {
    const raw = JSON.parse(window.localStorage.getItem(KEY) ?? "null") as Partial<ShortcutSettings> | null;
    if (!raw || typeof raw !== "object") return fallback;
    const accelerators = { ...fallback.accelerators };
    for (const { id } of SHORTCUT_ACTIONS) {
      const value = raw.accelerators?.[id];
      if (value === null || isValidAccelerator(value)) accelerators[id] = value;
    }
    const ptt = raw.pushToTalk;
    const delay = typeof ptt?.releaseDelayMs === "number" && Number.isFinite(ptt.releaseDelayMs) ? Math.min(Math.max(Math.round(ptt.releaseDelayMs), 0), 1_000) : fallback.pushToTalk.releaseDelayMs;
    return { accelerators, pushToTalk: { enabled: ptt?.enabled === true, key: validPttKey(ptt?.key), releaseDelayMs: delay } };
  } catch { return fallback; }
}

export function saveShortcutSettings(settings: ShortcutSettings): ShortcutSettings {
  try { window.localStorage.setItem(KEY, JSON.stringify(settings)); } catch { /* vale só nesta sessão */ }
  window.dispatchEvent(new CustomEvent(CHANGE_EVENT, { detail: settings }));
  return settings;
}

export function watchShortcutSettings(callback: (settings: ShortcutSettings) => void): () => void {
  const listener = (event: Event) => callback((event as CustomEvent<ShortcutSettings>).detail);
  window.addEventListener(CHANGE_EVENT, listener);
  return () => window.removeEventListener(CHANGE_EVENT, listener);
}

type KeyLike = { code: string; ctrlKey: boolean; altKey: boolean; shiftKey: boolean; metaKey: boolean };

const MODIFIER_CODES = new Set(["ControlLeft", "ControlRight", "AltLeft", "AltRight", "ShiftLeft", "ShiftRight", "MetaLeft", "MetaRight"]);
const PUNCTUATION: Record<string, string> = {
  Backquote: "`", Minus: "-", Equal: "=", BracketLeft: "[", BracketRight: "]", Semicolon: ";", Quote: "'", Comma: ",", Period: ".", Slash: "/", Backslash: "\\",
};

function keyName(code: string): string | null {
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit[0-9]$/.test(code)) return code.slice(5);
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(code)) return code;
  if (/^Numpad[0-9]$/.test(code)) return `num${code.slice(6)}`;
  return PUNCTUATION[code] ?? null;
}

/**
 * Converte a tecla apertada num acelerador global. Exige Ctrl, Alt ou Win (exceto F1–F24),
 * para um atalho global não engolir uma letra comum enquanto a pessoa digita em outro programa.
 */
export function acceleratorFromKey(event: KeyLike): string | null {
  if (MODIFIER_CODES.has(event.code)) return null;
  const key = keyName(event.code);
  if (!key) return null;
  const isFunctionKey = /^F\d+$/.test(key);
  if (!isFunctionKey && !event.ctrlKey && !event.altKey && !event.metaKey) return null;
  const parts = [event.ctrlKey && "Control", event.altKey && "Alt", event.shiftKey && "Shift", event.metaKey && "Super", key].filter(Boolean) as string[];
  const accelerator = parts.join("+");
  return isValidAccelerator(accelerator) ? accelerator : null;
}

export function formatAccelerator(accelerator: string | null): string {
  if (!accelerator) return "Sem atalho";
  return accelerator.replace("Control", "Ctrl").replace("Super", "Win").replace(/num(\d)/, "Num $1").split("+").join(" + ");
}

const VK_BY_CODE: Record<string, number> = {
  Space: 0x20, Tab: 0x09, CapsLock: 0x14, Backquote: 0xc0,
  ShiftLeft: 0xa0, ShiftRight: 0xa1, ControlLeft: 0xa2, ControlRight: 0xa3, AltLeft: 0xa4, AltRight: 0xa5,
  Minus: 0xbd, Equal: 0xbb, BracketLeft: 0xdb, BracketRight: 0xdd, Semicolon: 0xba, Quote: 0xde, Comma: 0xbc, Period: 0xbe, Slash: 0xbf, Backslash: 0xdc,
};

const LABEL_BY_CODE: Record<string, string> = {
  Space: "Espaço", Tab: "Tab", CapsLock: "Caps Lock", ShiftLeft: "Shift esq.", ShiftRight: "Shift dir.", ControlLeft: "Ctrl esq.", ControlRight: "Ctrl dir.", AltLeft: "Alt", AltRight: "Alt Gr",
};

/** Teclas de mouse aceitas: botão do meio e os dois laterais. Esquerdo e direito ficam de fora. */
const MOUSE_VK: Record<number, PttKey> = { 1: { vk: 0x04, label: "Botão do meio" }, 3: { vk: 0x05, label: "Mouse 4" }, 4: { vk: 0x06, label: "Mouse 5" } };

export function isAllowedVk(vk: number): boolean {
  if (!Number.isInteger(vk)) return false;
  if (vk >= 0x04 && vk <= 0x06) return true;
  return (vk >= 0x08 && vk <= 0xfe) && vk !== 0x5b && vk !== 0x5c;
}

export function pttKeyFromKeyboard(event: { code: string }): PttKey | null {
  const code = event.code;
  let vk: number | undefined = VK_BY_CODE[code];
  if (vk === undefined && /^Key[A-Z]$/.test(code)) vk = code.charCodeAt(3);
  if (vk === undefined && /^Digit[0-9]$/.test(code)) vk = 0x30 + Number(code.slice(5));
  if (vk === undefined && /^Numpad[0-9]$/.test(code)) vk = 0x60 + Number(code.slice(6));
  const fn = /^F([1-9]|1[0-9]|2[0-4])$/.exec(code);
  if (vk === undefined && fn) vk = 0x6f + Number(fn[1]);
  if (vk === undefined || !isAllowedVk(vk)) return null;
  return { vk, label: LABEL_BY_CODE[code] ?? keyName(code) ?? code };
}

export function pttKeyFromMouse(button: number): PttKey | null {
  return MOUSE_VK[button] ?? null;
}
