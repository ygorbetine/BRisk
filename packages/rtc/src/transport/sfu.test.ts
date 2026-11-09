import { describe, expect, it, vi } from "vitest";
import { LiveKitTransport } from "./sfu";
import type { TransportEvents } from "./call-transport";

const events = (): TransportEvents => ({
  sendOffer: vi.fn(), sendAnswer: vi.fn(), sendIce: vi.fn(),
  onRemoteStream: vi.fn(), onConnectionState: vi.fn(),
});

describe("LiveKitTransport", () => {
  it("é um transporte SFU e não expõe diagnóstico sem sala", async () => {
    const transport = new LiveKitTransport("a", events(), async () => ({ url: "wss://x", token: "t" }));
    expect(transport.kind).toBe("sfu");
    expect(transport.getDiagnostics()).toEqual([]);
    expect(await transport.collectDiagnostics()).toEqual([]);
  });

  it("mede bitrate, latência e perda reais pela conexão de envio (soma as camadas simulcast)", async () => {
    const transport = new LiveKitTransport("a", events(), async () => ({ url: "wss://x", token: "t" }));
    let bytes = 0;
    let at = 1000;
    const stats = () => new Map<string, unknown>([
      ["o1", { type: "outbound-rtp", kind: "video", bytesSent: bytes, timestamp: at }],
      ["o2", { type: "outbound-rtp", kind: "video", bytesSent: bytes, timestamp: at }],
    ]);
    (transport as unknown as { room: unknown }).room = {
      state: "connected",
      remoteParticipants: new Map([["p1", {}], ["p2", {}]]),
      engine: { pcManager: { publisher: { getStats: async () => stats() } } },
    };
    await transport.collectDiagnostics();
    bytes = 500_000; at = 2000; // 2 camadas x 500 kB em 1 s = 8000 kbps
    const diagnostics = await transport.collectDiagnostics();
    expect(diagnostics.map((item) => item.outboundBitrateKbps)).toEqual([8000, undefined]);
  });

  it("recusa publicar antes de entrar e não envia dados desconectado", async () => {
    const transport = new LiveKitTransport("a", events(), async () => ({ url: "wss://x", token: "t" }));
    await expect(transport.publishTrack({ kind: "video" } as MediaStreamTrack, {} as MediaStream)).rejects.toThrow();
    expect(transport.sendData("oi")).toBe(0);
  });

  it("propaga falha do provedor de token", async () => {
    const transport = new LiveKitTransport("a", events(), async () => { throw new Error("sem token"); });
    await expect(transport.join({ roomId: "r", localPeerId: "a" })).rejects.toThrow("sem token");
  });
});
