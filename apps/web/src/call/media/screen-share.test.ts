import { afterEach, describe, expect, it, vi } from "vitest";
import type { ScreenShareProvider } from "@briks/rtc";
import { startScreenCapture } from "./screen-share";

afterEach(() => vi.unstubAllGlobals());

describe("startScreenCapture", () => {
  it("no desktop Linux captura vídeo imediatamente e respeita áudio desligado", async () => {
    const stream = {} as MediaStream;
    const chooseScreenSource = vi.fn(async () => "screen:1");
    const selectScreenSource = vi.fn(async () => undefined);
    const getDisplayMedia = vi.fn(async () => stream);
    const provider = { startScreenShare: vi.fn() } as unknown as ScreenShareProvider;
    vi.stubGlobal("window", { desktop: { chooseScreenSource, selectScreenSource } });
    vi.stubGlobal("navigator", { userAgent: "Linux", mediaDevices: { getDisplayMedia } });

    await expect(startScreenCapture(provider, undefined, false, true)).resolves.toEqual({
      stream,
      desktopAudio: null,
    });
    expect(chooseScreenSource).toHaveBeenCalledOnce();
    expect(selectScreenSource).toHaveBeenCalledWith("screen:1");
    expect(getDisplayMedia).toHaveBeenCalledWith({ video: true, audio: false });
    expect(provider.startScreenShare).not.toHaveBeenCalled();
  });

  it("no navegador delega a escolha sem áudio ao provider", async () => {
    const stream = {} as MediaStream;
    const startScreenShare = vi.fn(async () => stream);
    const provider = { startScreenShare } as unknown as ScreenShareProvider;
    vi.stubGlobal("window", {});
    vi.stubGlobal("navigator", { userAgent: "Firefox", mediaDevices: {} });

    await expect(startScreenCapture(provider, "window:2", false, false)).resolves.toEqual({
      stream,
      desktopAudio: null,
    });
    expect(startScreenShare).toHaveBeenCalledWith("window:2", false);
  });
});
