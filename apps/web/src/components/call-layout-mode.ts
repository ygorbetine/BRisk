export type CallLayoutMode = "weight" | "focus";

const KEY = "briks.call-layout.v1";

export function loadCallLayoutMode(): CallLayoutMode {
  try { return localStorage.getItem(KEY) === "focus" ? "focus" : "weight"; } catch { return "weight"; }
}

export function saveCallLayoutMode(mode: CallLayoutMode): void {
  try { localStorage.setItem(KEY, mode); } catch { /* preferência é opcional */ }
}

/**
 * Grade por peso: 4 colunas, cada live ocupa 2x2 células e cada pessoa 1x1.
 * Devolve quantas linhas a grade precisa (mínimo 2, para a live caber).
 */
export function weightedRows(liveCount: number, personCount: number): number {
  const cells = liveCount * 4 + personCount;
  return Math.max(2, Math.ceil(cells / 4));
}
