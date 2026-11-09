import { describe, expect, it } from "vitest";
import { DEFAULT_OVERLAY_SETTINGS, sanitizeOverlaySettings } from "./settings";

describe("sanitizeOverlaySettings", () => {
  it("usa os padrões para dados inválidos", () => {
    expect(sanitizeOverlaySettings("lixo")).toEqual(DEFAULT_OVERLAY_SETTINGS);
    expect(sanitizeOverlaySettings(null).enabled).toBe(false);
  });
  it("limita posição, escala e opacidade", () => {
    const result = sanitizeOverlaySettings({ enabled: true, opacity: 5, widgets: { live: { on: false, x: 500, y: -4, scale: 9 } } });
    expect(result.enabled).toBe(true);
    expect(result.opacity).toBe(1);
    expect(result.widgets.live).toEqual({ on: false, x: 96, y: 0, scale: 2 });
    expect(result.widgets.voice).toEqual(DEFAULT_OVERLAY_SETTINGS.widgets.voice);
  });
});
