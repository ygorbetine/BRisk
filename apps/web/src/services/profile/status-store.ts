import { isStatusActive, parseRemoteStatus, type CustomStatus } from "./status";

const KEY = "briks.customStatus.v1";

export function loadLocalStatus(now = Date.now()): CustomStatus | null {
  try {
    const raw = window.localStorage.getItem(KEY);
    if (!raw) return null;
    const status = parseRemoteStatus(JSON.parse(raw), now);
    if (!status) window.localStorage.removeItem(KEY);
    return status;
  } catch { return null; }
}

export function saveLocalStatus(status: CustomStatus | null): void {
  try {
    if (isStatusActive(status)) window.localStorage.setItem(KEY, JSON.stringify(status));
    else window.localStorage.removeItem(KEY);
  } catch { /* sem armazenamento: o status vale só nesta sessão */ }
}
