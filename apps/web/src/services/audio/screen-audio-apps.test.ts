import { describe, expect, it } from "vitest";
import { audioAppsForCapture, sanitizeChoice, toggleAudioApp } from "./screen-audio-apps";
import { parseAudioApps, resolveProcessAudioRequest } from "../../call/media/process-audio";

describe("screen-audio-apps", () => {
  it("limpa a escolha guardada: minúsculas, sem repetidos, sem lixo", () => {
    expect(sanitizeChoice({ appsMode: true, names: ["Spotify.exe", "spotify.exe", 5, "", "  chrome.exe "] }))
      .toEqual({ appsMode: true, names: ["spotify.exe", "chrome.exe"] });
    expect(sanitizeChoice(null)).toEqual({ appsMode: false, names: [] });
    expect(sanitizeChoice({ appsMode: "sim", names: "x" })).toEqual({ appsMode: false, names: [] });
  });

  it("marca e desmarca apps", () => {
    expect(toggleAudioApp([], "Spotify.exe")).toEqual(["spotify.exe"]);
    expect(toggleAudioApp(["spotify.exe"], "SPOTIFY.EXE")).toEqual([]);
    expect(toggleAudioApp(["a.exe"], "  ")).toEqual(["a.exe"]);
  });

  it("decide o que a captura recebe", () => {
    const choice = { appsMode: true, names: ["spotify.exe"] };
    expect(audioAppsForCapture(true, choice)).toEqual(["spotify.exe"]);
    expect(audioAppsForCapture(true, { appsMode: true, names: [] })).toEqual([]);
    expect(audioAppsForCapture(true, { appsMode: false, names: ["spotify.exe"] })).toBeUndefined();
    expect(audioAppsForCapture(false, choice)).toBeUndefined();
  });
});

describe("process-audio com apps", () => {
  it("valida a lista vinda do backend", () => {
    expect(parseAudioApps([{ name: "Spotify.exe", label: "Spotify", playing: true }, { name: "" }, null, { label: "x" }, { name: "a.exe" }]))
      .toEqual([{ name: "spotify.exe", label: "Spotify", playing: true }, { name: "a.exe", label: "a.exe", playing: false }]);
    expect(parseAudioApps({})).toEqual([]);
  });

  it("apps escolhidos têm prioridade sobre a fonte de vídeo", () => {
    expect(resolveProcessAudioRequest("screen:1:0", ["Spotify.exe"])).toEqual({ mode: "apps", includeNames: ["spotify.exe"] });
    expect(resolveProcessAudioRequest("window:42:0", [])).toEqual({ mode: "window", hwnd: 42 });
    expect(resolveProcessAudioRequest("screen:1:0")).toEqual({ mode: "screen" });
  });
});
