import { describe, expect, it } from "vitest";
import { MAX_ZOOM, NO_ZOOM, clampZoom, panBy, wheelFactor, zoomAt, zoomLabel } from "./zoom-model";

const W = 800;
const H = 450;

describe("zoom-model", () => {
  it("não passa dos limites de escala e volta ao estado neutro em 100%", () => {
    expect(zoomAt(NO_ZOOM, 100, 0, 0, W, H).scale).toBe(MAX_ZOOM);
    expect(zoomAt(NO_ZOOM, 0.1, 0, 0, W, H)).toEqual(NO_ZOOM);
    expect(zoomAt({ scale: 2, x: 50, y: 20 }, 0.5, 10, 10, W, H)).toEqual(NO_ZOOM);
  });

  it("mantém fixo o ponto sob o cursor", () => {
    // Ponto a 100px à direita do centro: continua no mesmo lugar da tela depois do zoom.
    const next = zoomAt(NO_ZOOM, 2, 100, 0, W, H);
    const before = 100;
    const after = next.x + next.scale * ((before - NO_ZOOM.x) / NO_ZOOM.scale);
    expect(after).toBeCloseTo(100);
  });

  it("não deixa a imagem sair do quadro", () => {
    const zoomed = { scale: 2, x: 0, y: 0 };
    expect(panBy(zoomed, 9999, -9999, W, H)).toEqual({ scale: 2, x: W / 2, y: -H / 2 });
    expect(panBy(NO_ZOOM, 50, 50, W, H)).toEqual(NO_ZOOM);
  });

  it("ajusta o deslocamento quando o quadro muda de tamanho", () => {
    expect(clampZoom({ scale: 2, x: 400, y: 200 }, 400, 200)).toEqual({ scale: 2, x: 200, y: 100 });
  });

  it("roda para cima aproxima e para baixo afasta; pinça é mais forte", () => {
    expect(wheelFactor(-100, false)).toBeGreaterThan(1);
    expect(wheelFactor(100, false)).toBeLessThan(1);
    expect(wheelFactor(-5, true)).toBeGreaterThan(wheelFactor(-5, false));
  });

  it("mostra a escala em porcentagem", () => {
    expect(zoomLabel({ scale: 1.5, x: 0, y: 0 })).toBe("150%");
  });
});
