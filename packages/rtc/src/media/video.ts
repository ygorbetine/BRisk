import { MediaDeviceError } from "@briks/shared";

export * from "../video-encoding";

export async function openCamera(deviceId?: string): Promise<MediaStreamTrack> {
  try {
    const stream = await navigator.mediaDevices.getUserMedia({
      video: deviceId
        ? { deviceId: { exact: deviceId }, width: { ideal: 1280 }, height: { ideal: 720 } }
        : true,
    });
    return stream.getVideoTracks()[0]!;
  } catch (error) {
    throw new MediaDeviceError("Não foi possível acessar a câmera", error);
  }
}
