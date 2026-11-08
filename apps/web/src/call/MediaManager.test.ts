import { afterAll, beforeAll, describe, expect, it, vi } from "vitest";
import type { CallTransport, ScreenShareProvider } from "@briks/rtc";
import type { VoiceVideoSettings } from "../services/audio/settings";

type MediaManagerConstructor = typeof import("./MediaManager")["MediaManager"];
let MediaManager: MediaManagerConstructor;

class FakeMediaStream {
  readonly id = "local-media";
  constructor(private readonly tracks: MediaStreamTrack[] = []) {}
  addTrack(track: MediaStreamTrack): void { this.tracks.push(track); }
  removeTrack(track: MediaStreamTrack): void {
    const index = this.tracks.indexOf(track);
    if (index >= 0) this.tracks.splice(index, 1);
  }
  getTracks(): MediaStreamTrack[] { return [...this.tracks]; }
  getAudioTracks(): MediaStreamTrack[] { return this.tracks.filter((track) => track.kind === "audio"); }
  getVideoTracks(): MediaStreamTrack[] { return this.tracks.filter((track) => track.kind === "video"); }
}

beforeAll(async () => {
  const values = new Map<string, string>();
  vi.stubGlobal("sessionStorage", {
    getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => { values.set(key, value); },
    removeItem: (key: string) => { values.delete(key); },
  });
  vi.stubGlobal("MediaStream", FakeMediaStream);
  vi.stubGlobal("window", {});
  ({ MediaManager } = await import("./MediaManager"));
});

afterAll(() => vi.unstubAllGlobals());

describe("MediaManager", () => {
  it("publica o microfone e mantém mute e estado local sincronizados", async () => {
    const stop = vi.fn();
    const microphone = {
      kind: "audio",
      enabled: true,
      muted: false,
      readyState: "live",
      label: "Mic de teste",
      stop,
      applyConstraints: vi.fn(async () => undefined),
      getConstraints: () => ({ echoCancellation: true, noiseSuppression: true, autoGainControl: false }),
      getSettings: () => ({ deviceId: "mic-test" }),
    } as unknown as MediaStreamTrack;
    const inputStream = new FakeMediaStream([microphone]) as unknown as MediaStream;
    vi.stubGlobal("navigator", { mediaDevices: { getUserMedia: vi.fn(async () => inputStream) } });

    const publishTrack = vi.fn(async () => undefined);
    const unpublishTrack = vi.fn(async () => undefined);
    const transport = { publishTrack, unpublishTrack } as unknown as CallTransport;
    const sendState = vi.fn();
    const screenTrack = {
      kind: "video",
      readyState: "live",
      contentHint: "",
      addEventListener: vi.fn(),
      applyConstraints: vi.fn(async () => undefined),
      stop: vi.fn(),
    } as unknown as MediaStreamTrack;
    const screenStream = new FakeMediaStream([screenTrack]) as unknown as MediaStream;
    const screen = {
      getSources: vi.fn(async () => []),
      startScreenShare: vi.fn(async () => screenStream),
      stopScreenShare: vi.fn(async () => undefined),
    } as unknown as ScreenShareProvider;
    const manager = new MediaManager({
      getTransport: () => transport,
      currentLifecycle: () => 1,
      isActive: () => true,
      sendState,
      reportError: vi.fn(),
    }, screen);
    const settings: VoiceVideoSettings = {
      microphoneDeviceId: "",
      noiseSuppression: "standard",
      echoCancellation: true,
      excludeBriksAudioFromScreenShare: true,
      noiseGateEnabled: false,
      noiseGateThresholdDb: -45,
    };

    await expect(manager.initializeMicrophone(settings, transport, 1)).resolves.toBe(manager.localStream);
    expect(publishTrack).toHaveBeenCalledWith(microphone, manager.localStream);
    expect(manager.state.cameraStreamId).toBe("local-media");

    await manager.toggleScreen(undefined, false);
    expect(microphone.enabled).toBe(false);
    expect(microphone.applyConstraints).toHaveBeenCalledWith(expect.objectContaining({ autoGainControl: false }));
    expect(publishTrack).toHaveBeenLastCalledWith(microphone, manager.localStream);
    await manager.toggleScreen(undefined, false);
    expect(unpublishTrack).toHaveBeenCalledWith(screenTrack);

    await manager.toggleMicrophone();
    expect(microphone.enabled).toBe(true);
    await manager.toggleMicrophone();
    expect(microphone.enabled).toBe(false);
    expect(sendState).toHaveBeenCalledWith(expect.objectContaining({ microphone: false }), expect.any(String));

    await manager.cleanup();
    expect(stop).toHaveBeenCalled();
    expect(manager.state).toEqual({ microphone: false, camera: false, screenShare: false });
  });
});
