import { afterEach, describe, expect, it, vi } from "vitest";
import { WebScreenShareProvider } from "./index";

function fakeStream(): MediaStream {
  return {
    getAudioTracks: () => [],
    getTracks: () => [],
  } as unknown as MediaStream;
}

describe("WebScreenShareProvider restrictOwnAudio", () => {
  afterEach(() => vi.unstubAllGlobals());

  it("envia restrictOwnAudio na chamada inicial de getDisplayMedia", async () => {
    const getDisplayMedia = vi.fn(async () => fakeStream());
    vi.stubGlobal("navigator", { mediaDevices: { getDisplayMedia } });
    vi.stubGlobal("__briksMediaCaptureOptions", { restrictOwnAudio: true });

    await new WebScreenShareProvider().startScreenShare();

    expect(getDisplayMedia).toHaveBeenCalledWith({
      video: true,
      audio: { restrictOwnAudio: true },
    });
  });

  it("mantém loopback padrão quando a exclusão está desativada", async () => {
    const getDisplayMedia = vi.fn(async () => fakeStream());
    vi.stubGlobal("navigator", { mediaDevices: { getDisplayMedia } });
    vi.stubGlobal("__briksMediaCaptureOptions", { restrictOwnAudio: false });

    await new WebScreenShareProvider().startScreenShare();

    expect(getDisplayMedia).toHaveBeenCalledWith({ video: true, audio: true });
  });
});
