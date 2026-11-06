import { describe, expect, it, vi } from "vitest";
import { SupabaseSignalingProvider } from "./signaling";

function internals(provider: SupabaseSignalingProvider): Record<string, any> {
  return provider as unknown as Record<string, any>;
}

describe("SupabaseSignalingProvider reconnect sends", () => {
  it("aguarda reconnect antes de enviar peer.state", async () => {
    const provider = new SupabaseSignalingProvider();
    const state = internals(provider);
    const send = vi.fn(async () => "ok");

    state.roomId = "room-test";
    state.peerId = "peer-test";
    state.channelName = "briks:room:test";
    state.client = { channel: vi.fn(), removeChannel: vi.fn(async () => undefined) };
    state.status = "reconnecting";

    const pending = provider.sendPeerState({ microphone: true, camera: false, screenShare: false });
    await Promise.resolve();
    expect(send).not.toHaveBeenCalled();

    if (state.reconnectTimer) clearTimeout(state.reconnectTimer);
    state.reconnectTimer = undefined;
    state.channel = { send };
    state.setStatus("connected");

    await expect(pending).resolves.toBeUndefined();
    expect(send).toHaveBeenCalledOnce();
  });

  it("tolera cinco minutos de diferença e registra rejeições maiores no diagnóstico", () => {
    vi.useFakeTimers();
    const now = new Date("2026-09-01T12:00:00.000Z");
    vi.setSystemTime(now);
    const provider = new SupabaseSignalingProvider();
    const state = internals(provider);
    const localPeerId = "00000000-0000-4000-8000-000000000001";
    const remotePeerId = "00000000-0000-4000-8000-000000000002";
    state.roomId = "room-clock-test";
    state.peerId = localPeerId;
    state.presencePeers.set(remotePeerId, {
      peerId: remotePeerId,
      joinedAt: now.getTime() - 10 * 60_000,
      clientVersion: "0.2.1",
    });

    const envelope = (timestamp: number, messageId: string) => ({
      version: 1,
      type: "webrtc.offer",
      roomId: "room-clock-test",
      fromPeerId: remotePeerId,
      targetPeerId: localPeerId,
      messageId,
      timestamp,
      payload: { sdp: { type: "offer", sdp: "offer" } },
    });

    expect(state.acceptMessage(envelope(now.getTime() - 4 * 60_000, "clock-within-limit"))).toBe(true);
    expect(provider.getDiagnostics().clockSkewMs).toBe(-4 * 60_000);
    expect(state.acceptMessage(envelope(now.getTime() - 6 * 60_000, "clock-outside-limit"))).toBe(false);
    expect(provider.getDiagnostics()).toMatchObject({
      clockSkewMs: -6 * 60_000,
      clockSkewRejectedMessages: 1,
    });
    vi.useRealTimers();
  });

  it("não deixa a reabertura presa quando removeChannel nunca resolve", async () => {
    vi.useFakeTimers();
    const provider = new SupabaseSignalingProvider();
    const state = internals(provider);
    const replacement = {
      on: vi.fn(function (this: unknown) { return replacement; }),
      subscribe: vi.fn(),
    };
    state.roomId = "room-test";
    state.peerId = "peer-test";
    state.channelName = "briks:room:test";
    state.channel = {};
    state.client = {
      removeChannel: vi.fn(() => new Promise(() => undefined)),
      channel: vi.fn(() => replacement),
    };

    const reopening = state.reopenChannel();
    await vi.advanceTimersByTimeAsync(3_100);
    await reopening;
    expect(state.channel).toBe(replacement);
    expect(replacement.subscribe).toHaveBeenCalledOnce();
    vi.useRealTimers();
  });
});
