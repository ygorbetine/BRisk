import { describe, expect, it } from "vitest";
import { pickLivePreview } from "./live-preview-source";
import type { Participant } from "../store";

const stream = (id: string) => ({ id, getVideoTracks: () => [{}] }) as unknown as MediaStream;
const peer = (peerId: string, screenStreamId?: string): Participant => ({
  peerId,
  displayName: peerId,
  state: { microphone: true, camera: false, screenShare: Boolean(screenStreamId), screenStreamId },
  streams: screenStreamId ? { [screenStreamId]: stream(screenStreamId) } : {},
});
const none = { screen: null };

describe("pickLivePreview", () => {
  it("sem nenhuma tela compartilhada não há miniatura", () => {
    expect(pickLivePreview({ focusedTile: null, participants: [peer("a")], localPreviews: none, localScreenShare: false })).toBeNull();
  });

  it("mostra a tela de outra pessoa mesmo sem você transmitir", () => {
    expect(pickLivePreview({ focusedTile: null, participants: [peer("a", "s1")], localPreviews: none, localScreenShare: false })?.id).toBe("peer-a");
  });

  it("prefere a tela maximizada, inclusive a sua", () => {
    const local = { screen: stream("mine") };
    const input = { participants: [peer("a", "s1"), peer("b", "s2")], localPreviews: local, localScreenShare: true };
    expect(pickLivePreview({ ...input, focusedTile: "peer-b" })?.id).toBe("peer-b");
    expect(pickLivePreview({ ...input, focusedTile: "local-screen" })?.id).toBe("local-screen");
  });

  it("sem maximizada prefere a de outra pessoa à sua; ignora foco que sumiu", () => {
    const input = { participants: [peer("a", "s1")], localPreviews: { screen: stream("mine") }, localScreenShare: true };
    expect(pickLivePreview({ ...input, focusedTile: null })?.id).toBe("peer-a");
    expect(pickLivePreview({ ...input, focusedTile: "peer-zzz" })?.id).toBe("peer-a");
  });

  it("só a sua tela quando ninguém mais transmite", () => {
    expect(pickLivePreview({ focusedTile: null, participants: [peer("a")], localPreviews: { screen: stream("mine") }, localScreenShare: true })?.id).toBe("local-screen");
  });
});
