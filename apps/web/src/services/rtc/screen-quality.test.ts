import { describe, expect, it, vi } from "vitest";
import { applyScreenCaptureQuality, SCREEN_QUALITY_PROFILES, screenVideoPublication } from "./screen-quality";

describe("qualidade da transmissão de tela", () => {
  it("configura 1080p60 com orçamento para conteúdo em movimento", () => {
    const profile = SCREEN_QUALITY_PROFILES["1080p60"];
    expect(profile).toMatchObject({ fps: 60, maxBitrate: 9_000_000, contentHint: "motion" });
    expect(screenVideoPublication(profile)).toEqual({
      source: "screen",
      maxBitrate: 9_000_000,
      maxFramerate: 60,
      targetWidth: 1920,
      targetHeight: 1080,
      degradationPreference: "balanced",
    });
  });

  it("aplica resolução e FPS antes da publicação", async () => {
    const applyConstraints = vi.fn(async () => undefined);
    const track = { contentHint: "", applyConstraints } as unknown as MediaStreamTrack;
    await applyScreenCaptureQuality(track, SCREEN_QUALITY_PROFILES["1080p30"]);
    expect(track.contentHint).toBe("motion");
    expect(applyConstraints).toHaveBeenCalledWith({
      width: { ideal: 1920, max: 1920 },
      height: { ideal: 1080, max: 1080 },
      frameRate: { ideal: 30, max: 30 },
      resizeMode: "crop-and-scale",
    });
  });

  it("mantém o limite de FPS se o capturador recusar as dimensões", async () => {
    const applyConstraints = vi.fn()
      .mockRejectedValueOnce(new DOMException("unsupported", "OverconstrainedError"))
      .mockResolvedValueOnce(undefined);
    const track = { contentHint: "", applyConstraints } as unknown as MediaStreamTrack;
    await applyScreenCaptureQuality(track, SCREEN_QUALITY_PROFILES["720p60"]);
    expect(applyConstraints).toHaveBeenLastCalledWith({ frameRate: { ideal: 60, max: 60 } });
  });
});

describe("secondScreenPublication", () => {
  it("limita a segunda tela a 4 Mbps e ignora bitrate forçado", async () => {
    const { secondScreenPublication, SCREEN_QUALITY_PROFILES } = await import("./screen-quality");
    const options = secondScreenPublication({ ...SCREEN_QUALITY_PROFILES["1080p60"], forceBitrate: true, maxBitrate: 30_000_000 });
    expect(options.maxBitrate).toBe(4_000_000);
    expect(options.forceBitrate).toBe(false);
    expect(options.source).toBe("screen");
  });
});
