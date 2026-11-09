/** Avisos de amigos: ficou online / começou a transmitir. Regras puras + preferências locais. */
export type FriendNotifySettings = { online: boolean; streaming: boolean; muted: string[] };
export type FriendEvent = { peerId: string; kind: "online" | "streaming" };

const KEY = "briks.friendNotify.v1";
const CHANGE_EVENT = "briks:friend-notify-settings";
/** Ao abrir o app a presença chega aos poucos: quem já estava online não deve gerar aviso. */
export const STARTUP_GRACE_MS = 20_000;

export function defaultNotifySettings(): FriendNotifySettings { return { online: true, streaming: true, muted: [] }; }

export function loadNotifySettings(): FriendNotifySettings {
  try {
    const raw = JSON.parse(window.localStorage.getItem(KEY) ?? "null") as Partial<FriendNotifySettings> | null;
    if (!raw || typeof raw !== "object") return defaultNotifySettings();
    return {
      online: raw.online !== false,
      streaming: raw.streaming !== false,
      muted: Array.isArray(raw.muted) ? raw.muted.filter((item): item is string => typeof item === "string" && item.length <= 128).slice(0, 500) : [],
    };
  } catch { return defaultNotifySettings(); }
}

export function saveNotifySettings(settings: FriendNotifySettings): FriendNotifySettings {
  try { window.localStorage.setItem(KEY, JSON.stringify(settings)); } catch { /* vale só nesta sessão */ }
  window.dispatchEvent(new CustomEvent(CHANGE_EVENT, { detail: settings }));
  return settings;
}

export function watchNotifySettings(callback: (settings: FriendNotifySettings) => void): () => void {
  const listener = (event: Event) => callback((event as CustomEvent<FriendNotifySettings>).detail);
  window.addEventListener(CHANGE_EVENT, listener);
  return () => window.removeEventListener(CHANGE_EVENT, listener);
}

export function toggleMuted(settings: FriendNotifySettings, peerId: string): FriendNotifySettings {
  const muted = settings.muted.includes(peerId) ? settings.muted.filter((id) => id !== peerId) : [...settings.muted, peerId];
  return { ...settings, muted };
}

export type FriendSnapshot = { online: ReadonlySet<string>; streaming: ReadonlySet<string> };

/**
 * Compara dois retratos da presença e devolve os avisos a mostrar. Nada é avisado durante a carência
 * de abertura, durante uma chamada, para amigos silenciados ou para quem não é amigo.
 * Quem fica online já transmitindo gera só o aviso de transmissão.
 */
export function friendEvents(previous: FriendSnapshot, next: FriendSnapshot, context: { settings: FriendNotifySettings; friends: ReadonlySet<string>; inCall: boolean; sinceStartMs: number }): FriendEvent[] {
  const { settings, friends, inCall, sinceStartMs } = context;
  if (inCall || sinceStartMs < STARTUP_GRACE_MS) return [];
  const events: FriendEvent[] = [];
  for (const peerId of friends) {
    if (settings.muted.includes(peerId)) continue;
    const startedStreaming = next.streaming.has(peerId) && !previous.streaming.has(peerId);
    if (startedStreaming && settings.streaming) events.push({ peerId, kind: "streaming" });
    else if (next.online.has(peerId) && !previous.online.has(peerId) && settings.online) events.push({ peerId, kind: "online" });
  }
  return events;
}
