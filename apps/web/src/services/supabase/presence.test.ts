import { describe, expect, it } from "vitest";
import { parsePresence } from "./voice-activity";

describe("parsePresence", () => {
  it("aceita presença online sem sala de voz", () => {
    expect(parsePresence({ peerId: "peer-abc-1", joinedAt: Date.now() })).toEqual({ peerId: "peer-abc-1", joinedAt: expect.any(Number) });
  });
  it("aceita presença com sala de voz", () => {
    const parsed = parsePresence({ peerId: "peer-abc-1", joinedAt: Date.now(), roomId: "room-1", channelId: "chan-1" });
    expect(parsed?.voice).toEqual({ roomId: "room-1", channelId: "chan-1" });
  });
  it("recusa peerId vazio/enorme e horário absurdo", () => {
    expect(parsePresence({ peerId: "", joinedAt: Date.now() })).toBeNull();
    expect(parsePresence({ peerId: "x".repeat(200), joinedAt: Date.now() })).toBeNull();
    expect(parsePresence({ peerId: "peer-abc-1", joinedAt: Date.now() - 90_000_000, roomId: "r", channelId: "c" })).toBeNull();
    expect(parsePresence({ peerId: "peer-abc-1", joinedAt: Date.now() - 90_000_000 })).not.toBeNull();
    expect(parsePresence(null)).toBeNull();
  });
});
