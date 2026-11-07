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
