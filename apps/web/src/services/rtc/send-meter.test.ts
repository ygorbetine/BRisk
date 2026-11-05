import { describe, expect, it } from "vitest";
import type { PeerConnectionDiagnostics } from "@risk/rtc";
import { summarizeSendMeter } from "./send-meter";

const peer = (extra: Partial<PeerConnectionDiagnostics>) => ({ peerId: "p", ...extra }) as PeerConnectionDiagnostics;

describe("medidor de envio", () => {
  it("soma o bitrate e pega os piores valores", () => {
    const meter = summarizeSendMeter([
      peer({ outboundBitrateKbps: 4000, roundTripTimeMs: 40, packetLossPercent: 0.2 }),
      peer({ outboundBitrateKbps: 3500.4, roundTripTimeMs: 90, packetLossPercent: 1 }),
    ]);
    expect(meter).toMatchObject({ peers: 2, outboundKbps: 7500, worstRttMs: 90, worstLossPercent: 1, health: "good" });
  });

  it("classifica rede degradada e ruim", () => {
    expect(summarizeSendMeter([peer({ packetLossPercent: 5 })]).health).toBe("degraded");
    expect(summarizeSendMeter([peer({ roundTripTimeMs: 700 })]).health).toBe("poor");
  });

  it("lida com ausência de peers", () => {
    expect(summarizeSendMeter([])).toMatchObject({ peers: 0, outboundKbps: 0, health: "good" });
  });
});
