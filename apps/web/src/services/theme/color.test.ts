import { describe, expect, it } from "vitest";
import { hexToHsv, hsvToHex, hueFromPoint, normalizeHex, pointFromSv, svFromPoint, triangleVertices } from "./color";

describe("cores", () => {
  it("normaliza hex", () => {
    expect(normalizeHex("#B01FCF")).toBe("#b01fcf");
    expect(normalizeHex("b01fcf")).toBe("#b01fcf");
    expect(normalizeHex("#abc")).toBeNull();
    expect(normalizeHex("zzzzzz")).toBeNull();
  });
  it("converte hex e hsv sem perder a cor", () => {
    for (const hex of ["#b01fcf", "#6d5cf6", "#ffffff", "#000000", "#00ff00", "#123456"]) {
      expect(hsvToHex(hexToHsv(hex))).toBe(hex);
    }
  });
  it("matiz de cores puras", () => {
    expect(Math.round(hexToHsv("#ff0000").h)).toBe(0);
    expect(Math.round(hexToHsv("#00ff00").h)).toBe(120);
    expect(Math.round(hexToHsv("#0000ff").h)).toBe(240);
  });
});

describe("triângulo de cor", () => {
  const center = { x: 100, y: 100 };
  it("ida e volta entre ponto e saturação/brilho", () => {
    for (const hue of [0, 75, 200, 310]) {
      const vertices = triangleVertices(center, 70, hue);
      for (const [s, v] of [[1, 1], [0, 1], [0.5, 0.5], [0.8, 0.3]] as const) {
        const back = svFromPoint(pointFromSv(s, v, vertices), vertices);
        expect(back.v).toBeCloseTo(v, 5);
        expect(back.s).toBeCloseTo(s, 5);
      }
    }
  });
  it("o vértice puro é saturação e brilho máximos", () => {
    const vertices = triangleVertices(center, 70, 40);
    const sv = svFromPoint(vertices.pure, vertices);
    expect(sv.s).toBeCloseTo(1, 5);
    expect(sv.v).toBeCloseTo(1, 5);
  });
  it("ponto fora do triângulo gruda na borda", () => {
    const sv = svFromPoint({ x: 400, y: -300 }, triangleVertices(center, 70, 0));
    expect(sv.s).toBeGreaterThanOrEqual(0); expect(sv.s).toBeLessThanOrEqual(1);
    expect(sv.v).toBeGreaterThanOrEqual(0); expect(sv.v).toBeLessThanOrEqual(1);
  });
  it("matiz a partir do ponto", () => {
    expect(Math.round(hueFromPoint({ x: 150, y: 100 }, center))).toBe(0);
    expect(Math.round(hueFromPoint({ x: 100, y: 150 }, center))).toBe(90);
    expect(Math.round(hueFromPoint({ x: 50, y: 100 }, center))).toBe(180);
  });
});
