import { describe, expect, it } from "vitest";
import { hwndFromSourceId, processAudioRequestForSource } from "./process-audio";

describe("áudio por processo", () => {
  it("extrai o HWND de ids de janela do Electron", () => {
    expect(hwndFromSourceId("window:197342:0")).toBe(197342);
    expect(hwndFromSourceId("window:abc:0")).toBeNull();
    expect(hwndFromSourceId("screen:0:0")).toBeNull();
  });

  it("monta o pedido conforme o tipo de fonte", () => {
    expect(processAudioRequestForSource("screen:1:0")).toEqual({ mode: "screen" });
    expect(processAudioRequestForSource("window:42:0")).toEqual({ mode: "window", hwnd: 42 });
    expect(processAudioRequestForSource("outra:coisa")).toBeNull();
  });
});
