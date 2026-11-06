import { openConfiguredMicrophone } from "../../services/audio/microphone";
import { createRnnoiseMicrophone, type RnnoiseMicrophone } from "../../services/audio/rnnoise";
import type { VoiceVideoSettings } from "../../services/audio/settings";

export type MicrophoneSession = {
  inputStream: MediaStream;
  track: MediaStreamTrack;
  rnnoise?: RnnoiseMicrophone;
};

export async function createMicrophoneSession(settings: VoiceVideoSettings): Promise<MicrophoneSession> {
  const inputStream = await openConfiguredMicrophone(settings);
  const inputTrack = inputStream.getAudioTracks()[0];
  if (!inputTrack) {
    inputStream.getTracks().forEach((track) => track.stop());
    throw new Error("Nenhum microfone foi disponibilizado pelo navegador.");
  }

  console.info("Briks microphone capture", {
    requestedDeviceId: settings.microphoneDeviceId || "default",
    deviceId: inputTrack.getSettings().deviceId ?? "unknown",
    label: inputTrack.label || "unknown",
    noiseSuppression: settings.noiseSuppression,
    echoCancellation: settings.echoCancellation,
  });

  if (settings.noiseSuppression !== "rnnoise") return { inputStream, track: inputTrack };
  try {
    const rnnoise = await createRnnoiseMicrophone(inputStream);
    return { inputStream, track: rnnoise.track, rnnoise };
  } catch (error) {
    console.warn("RNNoise indisponível; usando supressão de ruído padrão do WebRTC.", error);
    await inputTrack.applyConstraints({ noiseSuppression: true }).catch(() => undefined);
    return { inputStream, track: inputTrack };
  }
}

export async function stopMicrophoneSession(session: MicrophoneSession): Promise<void> {
  session.inputStream.getTracks().forEach((track) => track.stop());
  session.track.stop();
  await session.rnnoise?.stop().catch(() => undefined);
}
