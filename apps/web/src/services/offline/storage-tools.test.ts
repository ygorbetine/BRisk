import { describe, expect, it } from "vitest";
import { approximateBytes, breakdownShares, exportFileName } from "./storage-tools";

describe("ferramentas de armazenamento", () => {
  it("estima o tamanho em bytes", () => {
    expect(approximateBytes({ a: 1 })).toBe(7);
    expect(approximateBytes(undefined)).toBe(0);
  });
  it("calcula as fatias da barra", () => {
    const shares = breakdownShares({ messages: 25, groups: 25, attachments: 50 });
    expect(shares).toEqual({ messages: 25, groups: 25, attachments: 50 });
    expect(breakdownShares({ messages: 0, groups: 0, attachments: 0 })).toEqual({ messages: 0, groups: 0, attachments: 0 });
  });
  it("nomeia o arquivo pela data", () => {
    expect(exportFileName(new Date("2026-11-08T12:00:00Z"))).toBe("briks-dados-2026-11-08.json");
  });
});
