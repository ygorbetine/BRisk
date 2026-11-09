import { describe, expect, it } from "vitest";
import { DEFAULT_OVERLAY_SETTINGS, EMPTY_OVERLAY_STATE, type OverlayPerson } from "../services/overlay/settings";
import { buildOverlayState, dragPosition, nextLiveId, pickLiveId, pickScreenStream, steppedScale, visibleWidgets } from "./overlay-model";

const person = (id: string, live = false): OverlayPerson => ({ id, name: id, speaking: false, muted: false, live });

describe("overlay-model", () => {
  it("arrasta sem sair da tela", () => {
    expect(dragPosition(10, 100, 1000)).toBe(20);
    expect(dragPosition(10, -500, 1000)).toBe(0);
    expect(dragPosition(90, 900, 1000)).toBe(96);
    expect(dragPosition(10, 50, 0)).toBe(10);
  });

  it("muda a escala em passos e respeita os limites", () => {
    expect(steppedScale(1, 1)).toBe(1.1);
    expect(steppedScale(0.5, -1)).toBe(0.5);
    expect(steppedScale(2, 1)).toBe(2);
    expect(steppedScale(1.1, 1)).toBe(1.2);
  });

  it("mostra só o que tem dados fora da edição e tudo que está ligado na edição", () => {
    const on = (id: keyof typeof DEFAULT_OVERLAY_SETTINGS.widgets) => ({ ...DEFAULT_OVERLAY_SETTINGS.widgets[id], on: true });
    const settings = { ...DEFAULT_OVERLAY_SETTINGS, widgets: { live: on("live"), voice: on("voice"), status: on("status"), mute: on("mute") } };
    expect(visibleWidgets(settings, EMPTY_OVERLAY_STATE, false)).toEqual([]);
    expect(visibleWidgets(settings, { ...EMPTY_OVERLAY_STATE, liveId: "a", people: [person("a", true)], micMuted: true, roomName: "Sala" }, false)).toEqual(["live", "voice", "status", "mute"]);
    expect(visibleWidgets(settings, EMPTY_OVERLAY_STATE, true)).toEqual(["live", "voice", "status", "mute"]);
  });

  it("escolhe e alterna a live", () => {
    const people = [person("a", true), person("b"), person("c", true)];
    expect(pickLiveId(people, "c")).toBe("c");
    expect(pickLiveId(people, "b")).toBe("a");
    expect(pickLiveId([person("b")], null)).toBeNull();
    expect(nextLiveId(people, "a")).toBe("c");
    expect(nextLiveId(people, "c")).toBe("a");
    expect(nextLiveId([person("a", true)], "a")).toBeNull();
  });

  it("acha o stream de tela sem pegar a câmera", () => {
    const video = (id: string) => ({ id, getVideoTracks: () => [1] });
    const audio = (id: string) => ({ id, getVideoTracks: () => [] as number[] });
    const streams = { cam: video("cam"), scr: video("scr"), mic: audio("mic") };
    expect(pickScreenStream(streams, { screenStreamId: "scr" })?.id).toBe("scr");
    expect(pickScreenStream(streams, { cameraStreamId: "cam" })?.id).toBe("scr");
    expect(pickScreenStream({ mic: audio("mic") }, {})).toBeNull();
    expect(pickScreenStream(undefined, {})).toBeNull();
  });

  it("monta o estado, sem oferecer a própria live", () => {
    const state = buildOverlayState({
      active: true, roomName: "Geral", groupName: "Grupo", selfName: "Eu", micOn: false, selfLive: true, selfSpeaking: true,
      participants: [{ peerId: "p1", displayName: "Ana", state: { microphone: true, screenShare: true } }],
      speaking: new Set(["p1"]), preferredLive: null,
    });
    expect(state.micMuted).toBe(true);
    expect(state.people[0]).toMatchObject({ id: "self", speaking: false, muted: true, live: true });
    expect(state.people[1]).toMatchObject({ id: "p1", speaking: true, live: true });
    expect(state.liveId).toBe("p1");
  });
});
