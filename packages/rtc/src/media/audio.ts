import { MediaDeviceError } from "@briks/shared";

export async function openMicrophone(deviceId?: string): Promise<MediaStreamTrack> {
  try {
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: deviceId
        ? { deviceId: { exact: deviceId }, echoCancellation: true, noiseSuppression: true }
        : true,
    });
    return stream.getAudioTracks()[0]!;
  } catch (error) {
    throw new MediaDeviceError("Não foi possível acessar o microfone", error);
  }
}
