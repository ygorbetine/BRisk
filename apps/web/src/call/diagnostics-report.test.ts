import { describe, expect, it } from "vitest";
import { buildTextReport, peerHealth } from "./diagnostics-report";

const base = { connectionState: "connected", roundTripTimeMs: 40, jitterMs: 5, packetLossPercent: 0 } as never;

describe("diagnostics-report", () => {
  it("classifica a saúde da conexão", () => {
    expect(peerHealth(base).level).toBe("good");
    expect(peerHealth({ ...(base as object), roundTripTimeMs: 500 } as never).level).toBe("warn");
    expect(peerHealth({ ...(base as object), connectionState: "failed" } as never).level).toBe("bad");
  });
  it("gera relatório sem ids", () => {
    const report = buildTextReport({
      signaling: null, connectivity: { mode: "turn", label: "TURN" }, network: { preference: "auto", vpnProviders: [] },
      transport: { label: "P2P" },
      peerConnections: [{ ...(base as object), peerId: "secret-id", selectedConnectionPath: { label: "Direto" }, packetsLost: 2 }],
    } as never, () => "Ana");
    expect(report).toContain("- Ana: Boa");
    expect(report).not.toContain("secret-id");
  });
});
