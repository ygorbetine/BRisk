/**
 * Histórico de chamadas, só neste computador (o servidor não guarda nada disso).
 * Guarda quando, onde, quanto tempo e quem apareceu; nunca áudio, vídeo, mensagens ou endereços.
 */
export type CallHistoryEntry = {
  id: string;
  startedAt: number;
  endedAt: number;
  groupName: string;
  channelName: string;
  /** Nomes de quem apareceu na chamada (sem você), na ordem em que entraram. */
  participants: string[];
  /** Você transmitiu a tela em algum momento. */
  streamed: boolean;
};

const KEY = "briks.callHistory.v1";
const ENABLED_KEY = "briks.callHistory.enabled";
const CHANGE_EVENT = "briks:call-history";
export const MAX_ENTRIES = 200;
/** Chamada de menos de 5 s (entrou e saiu, falhou ao conectar) não vira registro. */
export const MIN_DURATION_MS = 5_000;

const cleanName = (value: unknown, max: number) => typeof value === "string" ? [...value.replace(/[\u0000-\u001f\u007f-\u009f]/g, "").trim()].slice(0, max).join("") : "";

export function parseEntries(raw: unknown): CallHistoryEntry[] {
  if (!Array.isArray(raw)) return [];
  const entries: CallHistoryEntry[] = [];
  for (const item of raw.slice(0, MAX_ENTRIES)) {
    if (!item || typeof item !== "object") continue;
    const value = item as Record<string, unknown>;
    if (typeof value.id !== "string" || typeof value.startedAt !== "number" || typeof value.endedAt !== "number" || value.endedAt < value.startedAt) continue;
    entries.push({
      id: value.id.slice(0, 64),
      startedAt: value.startedAt,
      endedAt: value.endedAt,
      groupName: cleanName(value.groupName, 80),
      channelName: cleanName(value.channelName, 80),
      participants: Array.isArray(value.participants) ? value.participants.map((name) => cleanName(name, 80)).filter(Boolean).slice(0, 50) : [],
      streamed: value.streamed === true,
    });
  }
  return entries;
}

export function loadCallHistory(): CallHistoryEntry[] {
  try { return parseEntries(JSON.parse(window.localStorage.getItem(KEY) ?? "[]")); } catch { return []; }
}

function save(entries: CallHistoryEntry[]): void {
  try { window.localStorage.setItem(KEY, JSON.stringify(entries)); } catch { /* sem espaço: o histórico fica como estava */ }
  window.dispatchEvent(new Event(CHANGE_EVENT));
}

/** Coloca o registro no topo e corta os mais antigos. Função pura. */
export function prependEntry(entries: readonly CallHistoryEntry[], entry: CallHistoryEntry): CallHistoryEntry[] {
  return [entry, ...entries.filter((item) => item.id !== entry.id)].slice(0, MAX_ENTRIES);
}

export function addCallHistoryEntry(entry: CallHistoryEntry): void {
  if (!isCallHistoryEnabled() || entry.endedAt - entry.startedAt < MIN_DURATION_MS) return;
  save(prependEntry(loadCallHistory(), entry));
}

export function clearCallHistory(): void { save([]); }

export function isCallHistoryEnabled(): boolean {
  try { return window.localStorage.getItem(ENABLED_KEY) !== "false"; } catch { return true; }
}

export function setCallHistoryEnabled(enabled: boolean): void {
  try { window.localStorage.setItem(ENABLED_KEY, String(enabled)); } catch { /* vale só nesta sessão */ }
  if (!enabled) clearCallHistory();
  else window.dispatchEvent(new Event(CHANGE_EVENT));
}

export function watchCallHistory(callback: () => void): () => void {
  window.addEventListener(CHANGE_EVENT, callback);
  return () => window.removeEventListener(CHANGE_EVENT, callback);
}

export function formatDuration(ms: number): string {
  const totalMinutes = Math.round(ms / 60_000);
  if (totalMinutes < 1) return "menos de 1 min";
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  return hours ? `${hours} h${minutes ? ` ${minutes} min` : ""}` : `${minutes} min`;
}

type CallSnapshot = {
  roomId: string | null;
  groupName?: string;
  channelName?: string;
  participants: ReadonlyArray<{ peerId: string; displayName: string }>;
  streaming: boolean;
};

/**
 * Acompanha o estado da chamada e fecha um registro quando ela termina. Sem dependência de React/store:
 * recebe retratos do estado (testável) e chama `onFinish` com o registro pronto.
 */
export class CallHistoryRecorder {
  private current?: { roomId: string; startedAt: number; groupName: string; channelName: string; names: Map<string, string>; streamed: boolean };

  constructor(private readonly onFinish: (entry: CallHistoryEntry) => void, private readonly now: () => number = Date.now) {}

  update(snapshot: CallSnapshot): void {
    if (this.current && snapshot.roomId !== this.current.roomId) this.finish();
    if (!snapshot.roomId) return;
    if (!this.current) {
      this.current = { roomId: snapshot.roomId, startedAt: this.now(), groupName: "", channelName: "", names: new Map(), streamed: false };
    }
    const current = this.current;
    if (snapshot.groupName) current.groupName = snapshot.groupName;
    if (snapshot.channelName) current.channelName = snapshot.channelName;
    for (const person of snapshot.participants) {
      // Ignora o nome provisório ("Peer abc123") enquanto o perfil não chega.
      if (person.displayName && !/^Peer \S{1,8}$/.test(person.displayName)) current.names.set(person.peerId, person.displayName);
    }
    if (snapshot.streaming) current.streamed = true;
  }

  finish(): void {
    const current = this.current;
    this.current = undefined;
    if (!current) return;
    this.onFinish({
      id: `${current.startedAt}-${current.roomId.slice(0, 12)}`,
      startedAt: current.startedAt,
      endedAt: this.now(),
      groupName: current.groupName,
      channelName: current.channelName,
      participants: [...new Set(current.names.values())],
      streamed: current.streamed,
    });
  }
}
