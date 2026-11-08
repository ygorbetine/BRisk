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

export type FitLayout = { columns: number; rows: number; tileWidth: number; tileHeight: number };

/**
 * Escolhe quantas colunas usar para encaixar `count` telas (todas com a mesma proporção)
 * na área disponível, escolhendo a disposição em que cada tela fica maior.
 * Para duas telas isso decide entre lado a lado e empilhadas.
 */
export function bestFitLayout(count: number, width: number, height: number, aspect = 16 / 9, gap = 10): FitLayout {
  const n = Math.max(1, Math.floor(count));
  let best: FitLayout = { columns: 1, rows: n, tileWidth: 0, tileHeight: 0 };
  for (let columns = 1; columns <= n; columns++) {
    const rows = Math.ceil(n / columns);
    const maxWidth = (width - gap * (columns - 1)) / columns;
    const maxHeight = (height - gap * (rows - 1)) / rows;
    const tileWidth = Math.max(0, Math.min(maxWidth, maxHeight * aspect));
    if (tileWidth > best.tileWidth + 0.5) best = { columns, rows, tileWidth, tileHeight: tileWidth / aspect };
  }
  return best;
}
