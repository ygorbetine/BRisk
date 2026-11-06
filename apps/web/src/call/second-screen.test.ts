import { describe, expect, it } from "vitest";
import { reconcileRemoteMediaState } from "./ParticipantManager";

const stream = (id: string) => ({ id, getVideoTracks: () => [{}], getAudioTracks: () => [] }) as unknown as MediaStream;

describe("segunda tela remota", () => {
  it("não confunde a segunda tela com a câmera nem com a primeira tela", () => {
    const streams = { cam: stream("cam"), s1: stream("s1"), s2: stream("s2") };
    const state = reconcileRemoteMediaState(streams, { microphone: true, camera: true, screenShare: true, cameraStreamId: "cam", screenStreamId: "s1", secondScreenStreamId: "s2" });
    expect(state.cameraStreamId).toBe("cam");
    expect(state.screenStreamId).toBe("s1");
    expect(state.secondScreenStreamId).toBe("s2");
  });
  it("quando só há a primeira tela e a segunda, a primeira continua sendo a tela principal", () => {
    const streams = { s1: stream("s1"), s2: stream("s2") };
    const state = reconcileRemoteMediaState(streams, { microphone: true, camera: false, screenShare: true, secondScreenStreamId: "s2" });
    expect(state.screenStreamId).toBe("s1");
  });
});
