export type PersonAudio = { user: number; screen: number; muted: boolean };

const KEY = "briks.personAudio.v1";
const MAX_PEOPLE = 500;
export const DEFAULT_PERSON_AUDIO: PersonAudio = { user: 100, screen: 100, muted: false };

function clampVolume(value: unknown): number {
  const number = typeof value === "number" && Number.isFinite(value) ? Math.round(value) : 100;
  return Math.min(200, Math.max(0, number));
}

/** A chave é o nome exibido, em minúsculas: o peerId muda a cada chamada, o nome não. */
export function personKey(displayName: string): string {
  return displayName.trim().toLowerCase().slice(0, 80);
}

export function sanitizePersonAudio(value: unknown): PersonAudio {
  if (!value || typeof value !== "object") return { ...DEFAULT_PERSON_AUDIO };
  const { user, screen, muted } = value as Record<string, unknown>;
  return { user: clampVolume(user), screen: clampVolume(screen), muted: muted === true };
}

function readAll(): Record<string, PersonAudio> {
  try {
    const raw = localStorage.getItem(KEY);
    const parsed: unknown = raw ? JSON.parse(raw) : {};
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return {};
    const result: Record<string, PersonAudio> = {};
    for (const [name, entry] of Object.entries(parsed as Record<string, unknown>).slice(0, MAX_PEOPLE)) {
      result[name] = sanitizePersonAudio(entry);
    }
    return result;
  } catch {
    return {};
  }
}

export function loadPersonAudio(displayName: string): PersonAudio {
  return readAll()[personKey(displayName)] ?? { ...DEFAULT_PERSON_AUDIO };
}

/** Guarda só o que foge do padrão, para a lista não crescer com todo mundo que já entrou numa call. */
export function savePersonAudio(displayName: string, audio: PersonAudio): void {
  const key = personKey(displayName);
  if (!key) return;
  try {
    const all = readAll();
    const clean = sanitizePersonAudio(audio);
    if (clean.user === 100 && clean.screen === 100 && !clean.muted) delete all[key];
    else all[key] = clean;
    localStorage.setItem(KEY, JSON.stringify(all));
  } catch {
    // Sem storage disponível, o ajuste vale só até fechar a chamada.
  }
}
