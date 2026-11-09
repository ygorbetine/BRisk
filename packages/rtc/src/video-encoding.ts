/** H.264 primeiro: é o codec que as GPUs (NVENC, AMF, Quick Sync) codificam em hardware. */
export const PREFERRED_VIDEO_CODECS = ["video/h264", "video/vp9", "video/vp8", "video/av1"] as const;

type CodecLike = { mimeType: string };

/**
 * Reordena os codecs para o SDP oferecer o preferido primeiro. Nada é removido, então quem não
 * suporta H.264 ainda negocia VP9/VP8. Codecs auxiliares (rtx, red, fec) ficam depois, na ordem original.
 */
export function orderVideoCodecs<T extends CodecLike>(codecs: readonly T[], preferred: readonly string[] = PREFERRED_VIDEO_CODECS): T[] {
  const rank = (codec: T): number => {
    const index = preferred.indexOf(codec.mimeType.toLowerCase());
    return index >= 0 ? index : preferred.length;
  };
  return codecs.map((codec, order) => ({ codec, order })).sort((a, b) => rank(a.codec) - rank(b.codec) || a.order - b.order).map((item) => item.codec);
}

export type VideoTrackSource = "camera" | "screen";

export type VideoPublicationOptions = {
  source: VideoTrackSource;
  maxBitrate?: number;
  maxFramerate?: number;
  targetWidth?: number;
  targetHeight?: number;
  degradationPreference?: RTCDegradationPreference;
  /** Tipo de conteúdo da tela: "motion" (jogo, padrão) ou "detail" (texto/código). */
  contentHint?: "motion" | "detail";
  /** O usuário escolheu o bitrate: sem o teto de proteção da malha, até 50 Mbps. */
  forceBitrate?: boolean;
};

export type VideoSenderPolicy = {
  maxBitrate: number;
  maxFramerate?: number;
  scaleResolutionDownBy?: number;
  degradationPreference: RTCDegradationPreference;
  priority: RTCPriorityType;
};

// O teto é agregado por origem. Dois receptores ainda recebem os 9 Mbps
// completos de 1080p60; em grupos maiores o custo do mesh é diluído.
const MAX_SCREEN_MESH_BITRATE = 18_000_000;
const MIN_SCREEN_BITRATE_PER_PEER = 1_800_000;
const FORCED_SCREEN_BITRATE_CEILING = 50_000_000;

export function resolveVideoSenderPolicy(
  options: VideoPublicationOptions | undefined,
  activePeers: number,
  capture: Pick<MediaTrackSettings, "width" | "height"> = {},
  screenPublished = false,
): VideoSenderPolicy {
  const peers = Math.max(1, Math.floor(activePeers));
  if (options?.source === "screen") {
    const requestedBitrate = clamp(
      options.maxBitrate ?? 6_000_000,
      750_000,
      options.forceBitrate ? FORCED_SCREEN_BITRATE_CEILING : 12_000_000,
    );
    const meshBitrate = Math.max(MIN_SCREEN_BITRATE_PER_PEER, Math.floor(MAX_SCREEN_MESH_BITRATE / peers));
    return {
      maxBitrate: options.forceBitrate ? requestedBitrate : Math.min(requestedBitrate, meshBitrate),
      maxFramerate: options.maxFramerate ? clamp(options.maxFramerate, 1, 60) : undefined,
      scaleResolutionDownBy: resolutionScale(capture, options),
      degradationPreference: options.degradationPreference ?? "balanced",
      priority: "high",
    };
  }

  const adaptiveCameraBitrate = peers <= 1 ? 2_500_000 : peers <= 3 ? 1_200_000 : 700_000;
  return {
    maxBitrate: screenPublished ? Math.min(adaptiveCameraBitrate, 600_000) : adaptiveCameraBitrate,
    maxFramerate: options?.maxFramerate ? clamp(options.maxFramerate, 1, 60) : undefined,
    degradationPreference: screenPublished ? "balanced" : "maintain-framerate",
    priority: screenPublished ? "low" : "medium",
  };
}

function resolutionScale(
  capture: Pick<MediaTrackSettings, "width" | "height">,
  options: VideoPublicationOptions,
): number {
  const widthScale = capture.width && options.targetWidth ? capture.width / options.targetWidth : 1;
  const heightScale = capture.height && options.targetHeight ? capture.height / options.targetHeight : 1;
  return Math.round(Math.max(1, widthScale, heightScale) * 100) / 100;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.min(maximum, Math.max(minimum, value));
}
