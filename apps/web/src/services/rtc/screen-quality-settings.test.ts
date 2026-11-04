import { describe, expect, it } from "vitest";
import {
  automaticBitrate,
  buildScreenQualityProfile,
  DEFAULT_SCREEN_QUALITY_SETTINGS,
  estimatedUploadBitrate,
  loadScreenQualitySettings,
  resolveScreenBitrate,
  sanitizeScreenQualitySettings,
  type ScreenQualitySettings,
} from "./screen-quality-settings";

const base: ScreenQualitySettings = { ...DEFAULT_SCREEN_QUALITY_SETTINGS };

describe("qualidade da transmissão", () => {
  it("calcula o bitrate automático pela resolução e pelo FPS", () => {
    expect(automaticBitrate(1920, 1080, 30)).toBeGreaterThan(4_000_000);
    expect(automaticBitrate(1920, 1080, 60)).toBeGreaterThan(automaticBitrate(1920, 1080, 30));
    expect(automaticBitrate(1280, 720, 15)).toBeGreaterThanOrEqual(750_000);
    expect(automaticBitrate(3840, 2160, 60)).toBe(20_000_000);
  });

  it("resolução, FPS e bitrate são independentes", () => {
    const low = buildScreenQualityProfile({ ...base, resolution: "1440p", fps: 30 });
    const high = buildScreenQualityProfile({ ...base, resolution: "720p", fps: 60 });
    expect([low.width, low.height, low.fps]).toEqual([2560, 1440, 30]);
    expect([high.width, high.height, high.fps]).toEqual([1280, 720, 60]);
  });

  it("os níveis forçam o bitrate acima do automático", () => {
    const auto = resolveScreenBitrate(base);
    const high = resolveScreenBitrate({ ...base, bitrateMode: "high" });
    const max = resolveScreenBitrate({ ...base, bitrateMode: "max" });
    expect(high).toBeGreaterThan(auto);
    expect(max).toBeGreaterThan(high);
    expect(buildScreenQualityProfile(base).forceBitrate).toBe(false);
    expect(buildScreenQualityProfile({ ...base, bitrateMode: "max" }).forceBitrate).toBe(true);
  });

  it("aceita bitrate personalizado dentro dos limites", () => {
    expect(resolveScreenBitrate({ ...base, bitrateMode: "custom", customMbps: 12 })).toBe(12_000_000);
    expect(resolveScreenBitrate({ ...base, bitrateMode: "custom", customMbps: 500 })).toBe(50_000_000);
    expect(resolveScreenBitrate({ ...base, bitrateMode: "custom", customMbps: 0 })).toBe(1_000_000);
  });

  it("estima o upload por espectador na malha e fixo no SFU", () => {
    expect(estimatedUploadBitrate(base, 4)).toBe(resolveScreenBitrate(base) * 4);
    expect(estimatedUploadBitrate(base, 4, true)).toBe(resolveScreenBitrate(base));
  });

  it("sanitiza valores inválidos do armazenamento", () => {
    expect(sanitizeScreenQualitySettings({ resolution: "8k", fps: 144, bitrateMode: "x", customMbps: "a" })).toEqual(DEFAULT_SCREEN_QUALITY_SETTINGS);
    expect(sanitizeScreenQualitySettings({ resolution: "1440p", fps: 60 }).resolution).toBe("1440p");
  });

  it("migra a escolha antiga de preset", () => {
    const storage = { getItem: (key: string) => (key === "risk.screenShareQuality" ? "720p60" : null) };
    expect(loadScreenQualitySettings(storage)).toMatchObject({ resolution: "720p", fps: 60 });
  });
});
