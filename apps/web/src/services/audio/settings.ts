export type NoiseSuppressionMode = "rnnoise" | "standard" | "off";

export type VoiceVideoSettings = {
  microphoneDeviceId: string;
  noiseSuppression: NoiseSuppressionMode;
  echoCancellation: boolean;
  excludeBriksAudioFromScreenShare: boolean;
  noiseGateEnabled: boolean;
  noiseGateThresholdDb: number;
};

type BriksMediaCaptureOptions = {
  restrictOwnAudio: boolean;
};

const STORAGE_KEY = "risk.voice-video-settings.v1";
export const VOICE_VIDEO_SETTINGS_EVENT = "briks:voice-video-settings";
const MAX_DEVICE_ID_LENGTH = 512;

export const DEFAULT_VOICE_VIDEO_SETTINGS: VoiceVideoSettings = {
  microphoneDeviceId: "",
  noiseSuppression: "standard",
  echoCancellation: true,
  excludeBriksAudioFromScreenShare: true,
  noiseGateEnabled: false,
  noiseGateThresholdDb: -45,
};

function syncMediaCaptureOptions(settings: VoiceVideoSettings): void {
  (globalThis as typeof globalThis & { __briksMediaCaptureOptions?: BriksMediaCaptureOptions }).__briksMediaCaptureOptions = {
    restrictOwnAudio: settings.excludeBriksAudioFromScreenShare,
  };
}

function clampGate(v: unknown): number {
  const n = typeof v === "number" && Number.isFinite(v) ? v : -45;
  return Math.min(-20, Math.max(-70, Math.round(n)));
}

function normalizeMicrophoneDeviceId(value: unknown): string {
  return typeof value === "string" && value.length <= MAX_DEVICE_ID_LENGTH ? value : "";
}

export function loadVoiceVideoSettings(): VoiceVideoSettings {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) {
      const settings = { ...DEFAULT_VOICE_VIDEO_SETTINGS };
      syncMediaCaptureOptions(settings);
      return settings;
    }
    const value = JSON.parse(raw) as Partial<VoiceVideoSettings>;
    const noiseSuppression: NoiseSuppressionMode = value.noiseSuppression === "rnnoise" || value.noiseSuppression === "off"
      ? value.noiseSuppression
      : "standard";
    const settings: VoiceVideoSettings = {
      microphoneDeviceId: normalizeMicrophoneDeviceId(value.microphoneDeviceId),
      noiseSuppression,
      echoCancellation: value.echoCancellation !== false,
      excludeBriksAudioFromScreenShare: value.excludeBriksAudioFromScreenShare !== false,
      noiseGateEnabled: value.noiseGateEnabled === true,
      noiseGateThresholdDb: clampGate(value.noiseGateThresholdDb),
    };
    syncMediaCaptureOptions(settings);
    return settings;
  } catch {
    const settings = { ...DEFAULT_VOICE_VIDEO_SETTINGS };
    syncMediaCaptureOptions(settings);
    return settings;
  }
}

export function saveVoiceVideoSettings(settings: VoiceVideoSettings): void {
  const normalized: VoiceVideoSettings = {
    ...settings,
    microphoneDeviceId: normalizeMicrophoneDeviceId(settings.microphoneDeviceId),
    noiseGateThresholdDb: clampGate(settings.noiseGateThresholdDb),
  };
  localStorage.setItem(STORAGE_KEY, JSON.stringify(normalized));
  syncMediaCaptureOptions(normalized);
  window.dispatchEvent(new CustomEvent<VoiceVideoSettings>(VOICE_VIDEO_SETTINGS_EVENT, { detail: normalized }));
}
