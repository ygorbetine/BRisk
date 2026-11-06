import type { CallTransportPreference } from "@briks/rtc";

const STORAGE_KEY = "briks.call-transport-mode.v1";

export function normalizeTransportMode(value: unknown): CallTransportPreference {
  return value === "mesh" || value === "sfu" ? value : "auto";
}

export function loadCallTransportMode(): CallTransportPreference {
  try {
    return normalizeTransportMode(localStorage.getItem(STORAGE_KEY));
  } catch {
    return "auto";
  }
}

export function saveCallTransportMode(mode: CallTransportPreference): void {
  try { localStorage.setItem(STORAGE_KEY, mode); } catch { /* sem armazenamento: vale só nesta sessão */ }
}
