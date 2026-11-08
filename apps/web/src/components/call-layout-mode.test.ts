import { afterEach, describe, expect, it, vi } from "vitest";
import { loadCallLayoutMode, saveCallLayoutMode, weightedRows } from "./call-layout-mode";

describe("weightedRows", () => {
  it("usa 3 linhas para duas lives e três pessoas", () => expect(weightedRows(2, 3)).toBe(3));
  it("nunca fica abaixo de 2 linhas", () => expect(weightedRows(1, 0)).toBe(2));
  it("cresce com mais pessoas", () => expect(weightedRows(1, 8)).toBe(3));
});

describe("modo de layout", () => {
  afterEach(() => vi.unstubAllGlobals());
  it("padrão é grade por peso e guarda a escolha", () => {
    const data = new Map<string, string>();
    vi.stubGlobal("localStorage", { getItem: (k: string) => data.get(k) ?? null, setItem: (k: string, v: string) => void data.set(k, v) });
    expect(loadCallLayoutMode()).toBe("weight");
    saveCallLayoutMode("focus");
    expect(loadCallLayoutMode()).toBe("focus");
  });
  it("sem storage cai no padrão", () => {
    vi.stubGlobal("localStorage", undefined);
    expect(loadCallLayoutMode()).toBe("weight");
  });
});

import { bestFitLayout } from "./call-layout-mode";

describe("bestFitLayout", () => {
  it("duas telas em janela larga ficam lado a lado", () => {
    expect(bestFitLayout(2, 1885, 830).columns).toBe(2);
  });
  it("duas telas em janela alta e estreita ficam empilhadas", () => {
    expect(bestFitLayout(2, 700, 1000).columns).toBe(1);
  });
  it("uma tela usa o maior tamanho possível", () => {
    const fit = bestFitLayout(1, 1000, 400);
    expect(Math.round(fit.tileHeight)).toBe(400);
  });
  it("nunca passa da área", () => {
    const fit = bestFitLayout(2, 1885, 830);
    expect(fit.tileWidth * fit.columns + 10 * (fit.columns - 1)).toBeLessThanOrEqual(1885);
    expect(fit.tileHeight * fit.rows + 10 * (fit.rows - 1)).toBeLessThanOrEqual(830);
  });
});
