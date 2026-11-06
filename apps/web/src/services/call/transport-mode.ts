import type { CallTransportPreference } from "@briks/rtc";

/** Padrão: SFU. Se o servidor não estiver configurado, a seleção cai para P2P sozinha. */
export const DEFAULT_TRANSPORT_MODE: CallTransportPreference = "sfu";

const STORAGE_KEY = "briks.call-transport-mode.v1";

export function normalizeTransportMode(value: unknown): CallTransportPreference {
  return value === "mesh" || value === "sfu" ? value : "auto";
}

export function loadCallTransportMode(): CallTransportPreference {
  try {
    const stored = localStorage.getItem(STORAGE_KEY);
    return stored === null ? DEFAULT_TRANSPORT_MODE : normalizeTransportMode(stored);
  } catch {
    return DEFAULT_TRANSPORT_MODE;
  }
}

export function saveCallTransportMode(mode: CallTransportPreference): void {
  try { localStorage.setItem(STORAGE_KEY, mode); } catch { /* sem armazenamento: vale só nesta sessão */ }
}
