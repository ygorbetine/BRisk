import { afterEach, describe, expect, it, vi } from "vitest";
import { openConfiguredMicrophone, stabilizeMicrophoneGain } from "./microphone";
import type { VoiceVideoSettings } from "./settings";

const settings: VoiceVideoSettings = {
  microphoneDeviceId: "usb-mic-123",
  noiseSuppression: "standard",
  echoCancellation: true,
  excludeBriksAudioFromScreenShare: true,
  noiseGateEnabled: false,
  noiseGateThresholdDb: -45,
};

const fakeStream = {} as MediaStream;

afterEach(() => vi.unstubAllGlobals());

describe("openConfiguredMicrophone", () => {
  it("solicita exatamente o microfone salvo", async () => {
    const getUserMedia = vi.fn(async () => fakeStream);
    vi.stubGlobal("navigator", { mediaDevices: { getUserMedia } });

    await expect(openConfiguredMicrophone(settings)).resolves.toBe(fakeStream);
    expect(getUserMedia).toHaveBeenCalledWith({
      audio: {
        deviceId: { exact: "usb-mic-123" },
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: false,
        channelCount: 1,
      },
      video: false,
    });
  });

  it("usa o microfone padrão se o dispositivo salvo não existir mais", async () => {
    const unavailable = Object.assign(new Error("device missing"), { name: "NotFoundError" });
    const getUserMedia = vi.fn()
      .mockRejectedValueOnce(unavailable)
      .mockResolvedValueOnce(fakeStream);
    vi.stubGlobal("navigator", { mediaDevices: { getUserMedia } });

    await expect(openConfiguredMicrophone(settings)).resolves.toBe(fakeStream);
    expect(getUserMedia).toHaveBeenCalledTimes(2);
    expect(getUserMedia).toHaveBeenLastCalledWith({
      audio: {
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: false,
        channelCount: 1,
      },
      video: false,
    });
  });

  it("reaplica ganho estável sem apagar as demais constraints", async () => {
    const applyConstraints = vi.fn(async () => undefined);
    const track = {
      readyState: "live",
      getConstraints: () => ({ echoCancellation: true, noiseSuppression: true, channelCount: 1 }),
      applyConstraints,
    } as unknown as MediaStreamTrack;
    const stream = { getAudioTracks: () => [track] } as unknown as MediaStream;

    await stabilizeMicrophoneGain(stream);

    expect(applyConstraints).toHaveBeenCalledWith({
      echoCancellation: true,
      noiseSuppression: true,
      channelCount: 1,
      autoGainControl: false,
    });
  });
});
