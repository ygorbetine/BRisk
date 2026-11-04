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
