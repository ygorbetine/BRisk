import type { ScreenQualityProfile } from "./screen-quality";

/**
 * Qualidade da transmissão com resolução, FPS e bitrate independentes.
 * O bitrate é automático por padrão; os níveis Alto/Máximo/Personalizado
 * deixam o usuário "forçar a barra" em favor da qualidade.
 */

export type ScreenResolution = "720p" | "900p" | "1080p" | "1440p" | "2160p";
export type ScreenFps = 15 | 24 | 30 | 60;
export type ScreenBitrateMode = "auto" | "high" | "max" | "custom";
/** O que sacrificar quando a rede não aguenta. */
export type ScreenDegradation = "balanced" | "resolution" | "framerate";

export type ScreenQualitySettings = {
  resolution: ScreenResolution;
  fps: ScreenFps;
  bitrateMode: ScreenBitrateMode;
  /** Usado só em `custom`, em Mbps. */
  customMbps: number;
  degradation: ScreenDegradation;
};

export const SCREEN_RESOLUTIONS: Record<ScreenResolution, { label: string; width: number; height: number }> = {
  "720p": { label: "720p", width: 1280, height: 720 },
  "900p": { label: "900p", width: 1600, height: 900 },
  "1080p": { label: "1080p", width: 1920, height: 1080 },
  "1440p": { label: "1440p (2K)", width: 2560, height: 1440 },
  "2160p": { label: "2160p (4K)", width: 3840, height: 2160 },
};

export const SCREEN_FPS_OPTIONS: readonly ScreenFps[] = [15, 24, 30, 60];

export const SCREEN_BITRATE_MODES: Record<ScreenBitrateMode, { label: string; hint: string }> = {
  auto: { label: "Automático", hint: "Ajusta sozinho à resolução, ao FPS e à rede." },
  high: { label: "Alto", hint: "50% acima do automático para mais nitidez." },
  max: { label: "Máximo", hint: "Prioriza a qualidade; usa muita banda de envio." },
  custom: { label: "Personalizado", hint: "Você escolhe o bitrate em Mbps." },
};

export const SCREEN_DEGRADATION_OPTIONS: Record<ScreenDegradation, string> = {
  balanced: "Equilibrado",
  resolution: "Manter resolução (cai o FPS)",
  framerate: "Manter fluidez (cai a resolução)",
};

export const DEFAULT_SCREEN_QUALITY_SETTINGS: ScreenQualitySettings = {
  resolution: "1080p",
  fps: 30,
  bitrateMode: "auto",
  customMbps: 8,
  degradation: "balanced",
};

export const SCREEN_QUALITY_PRESETS: Array<{ id: string; label: string; settings: Partial<ScreenQualitySettings> }> = [
  { id: "720p30", label: "720p30", settings: { resolution: "720p", fps: 30 } },
  { id: "1080p30", label: "1080p30", settings: { resolution: "1080p", fps: 30 } },
  { id: "1080p60", label: "1080p60", settings: { resolution: "1080p", fps: 60 } },
  { id: "1440p60", label: "1440p60", settings: { resolution: "1440p", fps: 60 } },
];

const MIN_BITRATE = 750_000;
const AUTO_CEILING = 20_000_000;
const FORCED_CEILING = 50_000_000;
/** Bits por pixel por quadro: ponto de partida para codecs modernos em tela. */
const BITS_PER_PIXEL = 0.07;
const HIGH_FACTOR = 1.5;
const MAX_FACTOR = 2.5;
export const MIN_CUSTOM_MBPS = 1;
export const MAX_CUSTOM_MBPS = 50;

export function automaticBitrate(width: number, height: number, fps: number): number {
  const estimate = Math.round(width * height * fps * BITS_PER_PIXEL);
  return Math.min(AUTO_CEILING, Math.max(MIN_BITRATE, estimate));
}

export function resolveScreenBitrate(settings: ScreenQualitySettings): number {
  const { width, height } = SCREEN_RESOLUTIONS[settings.resolution];
  const auto = automaticBitrate(width, height, settings.fps);
  switch (settings.bitrateMode) {
    case "auto":
      return auto;
    case "high":
      return Math.min(FORCED_CEILING, Math.round(auto * HIGH_FACTOR));
    case "max":
      return Math.min(FORCED_CEILING, Math.round(auto * MAX_FACTOR));
    case "custom": {
      const mbps = Math.min(MAX_CUSTOM_MBPS, Math.max(MIN_CUSTOM_MBPS, settings.customMbps));
      return Math.round(mbps * 1_000_000);
    }
  }
}

/** Bitrate acima do automático: o sender não aplica o teto de proteção da malha. */
export function isBitrateForced(settings: ScreenQualitySettings): boolean {
  return settings.bitrateMode !== "auto";
}

export function formatMbps(bitsPerSecond: number): string {
  const mbps = bitsPerSecond / 1_000_000;
  return `${mbps >= 10 ? Math.round(mbps) : Math.round(mbps * 10) / 10} Mbps`.replace(".", ",");
}

export function buildScreenQualityProfile(settings: ScreenQualitySettings): ScreenQualityProfile & {
  forceBitrate: boolean;
  degradation: ScreenDegradation;
} {
  const { width, height, label } = SCREEN_RESOLUTIONS[settings.resolution];
  const maxBitrate = resolveScreenBitrate(settings);
  const mode = settings.bitrateMode === "auto" ? "auto" : formatMbps(maxBitrate);
  return {
    label: `${label} · ${settings.fps} FPS · ${mode}`,
    width,
    height,
    fps: settings.fps,
    maxBitrate,
    contentHint: settings.fps >= 30 ? "motion" : "detail",
    forceBitrate: isBitrateForced(settings),
    degradation: settings.degradation,
  };
}

/** Upload estimado: um fluxo por espectador (malha) ou um só (SFU). */
export function estimatedUploadBitrate(settings: ScreenQualitySettings, viewers: number, sfu = false): number {
  return resolveScreenBitrate(settings) * (sfu ? 1 : Math.max(1, viewers));
}

const STORAGE_KEY = "brisk.screenQualitySettings";
const LEGACY_KEY = "risk.screenShareQuality";

export function sanitizeScreenQualitySettings(value: unknown): ScreenQualitySettings {
  const input = (value && typeof value === "object" ? value : {}) as Record<string, unknown>;
  const defaults = DEFAULT_SCREEN_QUALITY_SETTINGS;
  const resolution = typeof input.resolution === "string" && input.resolution in SCREEN_RESOLUTIONS
    ? input.resolution as ScreenResolution : defaults.resolution;
  const fps = SCREEN_FPS_OPTIONS.includes(input.fps as ScreenFps) ? input.fps as ScreenFps : defaults.fps;
  const bitrateMode = typeof input.bitrateMode === "string" && input.bitrateMode in SCREEN_BITRATE_MODES
    ? input.bitrateMode as ScreenBitrateMode : defaults.bitrateMode;
  const customRaw = typeof input.customMbps === "number" && Number.isFinite(input.customMbps) ? input.customMbps : defaults.customMbps;
  const degradation = typeof input.degradation === "string" && input.degradation in SCREEN_DEGRADATION_OPTIONS
    ? input.degradation as ScreenDegradation : defaults.degradation;
  return {
    resolution,
    fps,
    bitrateMode,
    customMbps: Math.min(MAX_CUSTOM_MBPS, Math.max(MIN_CUSTOM_MBPS, customRaw)),
    degradation,
  };
}

export function loadScreenQualitySettings(storage: Pick<Storage, "getItem"> = localStorage): ScreenQualitySettings {
  try {
    const raw = storage.getItem(STORAGE_KEY);
    if (raw) return sanitizeScreenQualitySettings(JSON.parse(raw));
    // Migra a escolha antiga de preset (ex.: "1080p60").
    const legacy = storage.getItem(LEGACY_KEY);
    const match = legacy ? /^(\d{3,4}p)(\d{2})$/.exec(legacy) : null;
    if (match) return sanitizeScreenQualitySettings({ resolution: match[1], fps: Number(match[2]) });
  } catch {
    // armazenamento indisponível
  }
  return { ...DEFAULT_SCREEN_QUALITY_SETTINGS };
}

export function saveScreenQualitySettings(settings: ScreenQualitySettings, storage: Pick<Storage, "setItem"> = localStorage): void {
  try {
    storage.setItem(STORAGE_KEY, JSON.stringify(settings));
  } catch {
    // não crítico
  }
}
