import type { VideoPublicationOptions } from "@briks/rtc";

export type ScreenQuality = "720p30" | "720p60" | "1080p30" | "1080p60";

export type ScreenQualityProfile = {
  label: string;
  width: number;
  height: number;
  fps: number;
  maxBitrate: number;
  contentHint: "detail" | "motion";
  /** Bitrate escolhido pelo usuário (Alto/Máximo/Personalizado): ignora o teto de proteção da malha. */
  forceBitrate?: boolean;
  /** O que sacrificar quando a rede não aguenta. */
  degradation?: "balanced" | "resolution" | "framerate";
};

type ScreenCaptureConstraints = MediaTrackConstraints & { resizeMode?: "crop-and-scale" };

export const SCREEN_QUALITY_PROFILES: Record<ScreenQuality, ScreenQualityProfile> = {
  "720p30": { label: "720p · 30 FPS", width: 1280, height: 720, fps: 30, maxBitrate: 3_500_000, contentHint: "detail" },
  "720p60": { label: "720p · 60 FPS", width: 1280, height: 720, fps: 60, maxBitrate: 5_000_000, contentHint: "motion" },
  "1080p30": { label: "1080p · 30 FPS", width: 1920, height: 1080, fps: 30, maxBitrate: 6_000_000, contentHint: "motion" },
  "1080p60": { label: "1080p · 60 FPS", width: 1920, height: 1080, fps: 60, maxBitrate: 9_000_000, contentHint: "motion" },
};

export function isScreenQuality(value: string | null): value is ScreenQuality {
  return Boolean(value && value in SCREEN_QUALITY_PROFILES);
}

export function screenVideoPublication(profile: ScreenQualityProfile): VideoPublicationOptions {
  return {
    source: "screen",
    maxBitrate: profile.maxBitrate,
    maxFramerate: profile.fps,
    targetWidth: profile.width,
    targetHeight: profile.height,
    forceBitrate: profile.forceBitrate,
    degradationPreference: profile.degradation === "resolution"
      ? "maintain-resolution"
      : profile.degradation === "framerate"
        ? "maintain-framerate"
        : profile.contentHint === "motion" ? "balanced" : "maintain-resolution",
  };
}

/** A segunda tela divide o upload com a primeira: teto de 4 Mbps e prioridade normal de degradação. */
export const SECOND_SCREEN_MAX_BITRATE = 4_000_000;

export function secondScreenPublication(profile: ScreenQualityProfile): VideoPublicationOptions {
  return { ...screenVideoPublication(profile), maxBitrate: Math.min(profile.maxBitrate, SECOND_SCREEN_MAX_BITRATE), forceBitrate: false };
}

export async function applyScreenCaptureQuality(track: MediaStreamTrack, profile: ScreenQualityProfile): Promise<void> {
  try { track.contentHint = profile.contentHint; } catch { /* contentHint é opcional */ }
  try {
    const constraints: ScreenCaptureConstraints = {
      width: { ideal: profile.width, max: profile.width },
      height: { ideal: profile.height, max: profile.height },
      frameRate: { ideal: profile.fps, max: profile.fps },
      resizeMode: "crop-and-scale",
    };
    await track.applyConstraints(constraints);
  } catch {
    // Capturadores PipeWire podem recusar dimensões, mas normalmente permitem
    // limitar FPS. O sender ainda aplica scaleResolutionDownBy como salvaguarda.
    await track.applyConstraints({ frameRate: { ideal: profile.fps, max: profile.fps } }).catch(() => undefined);
  }
}
