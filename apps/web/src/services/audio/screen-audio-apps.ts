/** Escolha de quais apps entram no áudio da transmissão (Windows). Guarda só nomes de executáveis. */
export type ScreenAudioChoice = { appsMode: boolean; names: string[] };

const KEY = "briks.screenAudioApps.v1";
const MAX_NAMES = 16;

export const DEFAULT_SCREEN_AUDIO_CHOICE: ScreenAudioChoice = { appsMode: false, names: [] };

export function sanitizeChoice(value: unknown): ScreenAudioChoice {
  if (!value || typeof value !== "object") return DEFAULT_SCREEN_AUDIO_CHOICE;
  const raw = value as { appsMode?: unknown; names?: unknown };
  const names = Array.isArray(raw.names)
    ? [...new Set(raw.names.filter((name): name is string => typeof name === "string").map((name) => name.trim().toLowerCase()).filter((name) => name && name.length <= 128))].slice(0, MAX_NAMES)
    : [];
  return { appsMode: raw.appsMode === true, names };
}

export function loadScreenAudioChoice(): ScreenAudioChoice {
  try {
    const stored = localStorage.getItem(KEY);
    return stored ? sanitizeChoice(JSON.parse(stored)) : DEFAULT_SCREEN_AUDIO_CHOICE;
  } catch {
    return DEFAULT_SCREEN_AUDIO_CHOICE;
  }
}

export function saveScreenAudioChoice(choice: ScreenAudioChoice): void {
  try { localStorage.setItem(KEY, JSON.stringify(sanitizeChoice(choice))); } catch { /* sem armazenamento */ }
}

export function toggleAudioApp(names: string[], name: string): string[] {
  const key = name.trim().toLowerCase();
  if (!key) return names;
  return names.includes(key) ? names.filter((item) => item !== key) : [...names, key].slice(-MAX_NAMES);
}

/**
 * O que enviar à captura. `undefined` = o áudio segue a fonte de vídeo; lista = só esses apps;
 * `[]` = modo apps sem nenhum app marcado (a transmissão sai sem áudio).
 */
export function audioAppsForCapture(includeAudio: boolean, choice: ScreenAudioChoice): string[] | undefined {
  if (!includeAudio || !choice.appsMode) return undefined;
  return choice.names;
}
