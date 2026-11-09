export type OverlayWidgetId = "live" | "voice" | "status" | "mute";

export type OverlayWidget = { on: boolean; x: number; y: number; scale: number };

export type OverlaySettings = {
  enabled: boolean;
  opacity: number;
  widgets: Record<OverlayWidgetId, OverlayWidget>;
};

export const OVERLAY_WIDGETS: Array<{ id: OverlayWidgetId; label: string; hint: string }> = [
  { id: "live", label: "Transmissão", hint: "Miniatura da live que você está assistindo" },
  { id: "voice", label: "Quem está na sala", hint: "Pessoas na chamada; quem fala fica com borda verde" },
  { id: "status", label: "Sala conectada", hint: "Nome da sala e quantas pessoas estão nela" },
  { id: "mute", label: "Aviso de mudo", hint: "Aparece só quando seu microfone está desligado" },
];

const STORAGE_KEY = "briks.overlay.v1";
export const OVERLAY_SETTINGS_EVENT = "briks:overlay-settings";
export const MIN_SCALE = 0.5;
export const MAX_SCALE = 2;

export const DEFAULT_OVERLAY_SETTINGS: OverlaySettings = {
  // Tudo desligado por padrão: a pessoa liga a sobreposição e escolhe o que aparece.
  enabled: false,
  opacity: 0.92,
  widgets: {
    live: { on: false, x: 80, y: 2, scale: 1 },
    voice: { on: false, x: 1, y: 28, scale: 1 },
    status: { on: false, x: 1, y: 5, scale: 1 },
    mute: { on: false, x: 2, y: 90, scale: 1 },
  },
};

const clamp = (value: unknown, min: number, max: number, fallback: number): number => {
  const n = typeof value === "number" && Number.isFinite(value) ? value : fallback;
  return Math.min(max, Math.max(min, n));
};

export function sanitizeOverlaySettings(raw: unknown): OverlaySettings {
  const source = (raw && typeof raw === "object" ? raw : {}) as Partial<OverlaySettings>;
  const widgets = {} as Record<OverlayWidgetId, OverlayWidget>;
  for (const { id } of OVERLAY_WIDGETS) {
    const fallback = DEFAULT_OVERLAY_SETTINGS.widgets[id];
    const item = (source.widgets?.[id] ?? {}) as Partial<OverlayWidget>;
    widgets[id] = {
      on: typeof item.on === "boolean" ? item.on : fallback.on,
      x: clamp(item.x, 0, 96, fallback.x),
      y: clamp(item.y, 0, 96, fallback.y),
      scale: clamp(item.scale, MIN_SCALE, MAX_SCALE, fallback.scale),
    };
  }
  return { enabled: source.enabled === true, opacity: clamp(source.opacity, 0, 1, DEFAULT_OVERLAY_SETTINGS.opacity), widgets };
}

export function loadOverlaySettings(): OverlaySettings {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return sanitizeOverlaySettings(raw ? JSON.parse(raw) : null);
  } catch {
    return sanitizeOverlaySettings(null);
  }
}

export function saveOverlaySettings(settings: OverlaySettings): OverlaySettings {
  const clean = sanitizeOverlaySettings(settings);
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(clean)); } catch { /* sem armazenamento */ }
  window.dispatchEvent(new CustomEvent(OVERLAY_SETTINGS_EVENT, { detail: clean }));
  return clean;
}

/** Ouve mudanças feitas nesta janela ou em outra (o overlay roda numa janela separada). */
export function watchOverlaySettings(callback: (settings: OverlaySettings) => void): () => void {
  const local = () => callback(loadOverlaySettings());
  const storage = (event: StorageEvent) => { if (event.key === STORAGE_KEY) local(); };
  window.addEventListener(OVERLAY_SETTINGS_EVENT, local);
  window.addEventListener("storage", storage);
  return () => {
    window.removeEventListener(OVERLAY_SETTINGS_EVENT, local);
    window.removeEventListener("storage", storage);
  };
}

export type OverlayPerson = { id: string; name: string; speaking: boolean; muted: boolean; live: boolean };
export type OverlayState = {
  active: boolean;
  roomName: string;
  groupName: string;
  micMuted: boolean;
  people: OverlayPerson[];
  liveId: string | null;
};

export const EMPTY_OVERLAY_STATE: OverlayState = { active: false, roomName: "", groupName: "", micMuted: false, people: [], liveId: null };
