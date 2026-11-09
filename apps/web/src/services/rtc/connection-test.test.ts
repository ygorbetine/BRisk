import { describe, expect, it } from "vitest";
import { applyPathMetrics, candidateKind, classifyConnection, summarizeRoundTrips } from "./connection-test";

describe("teste de conexão", () => {
  it("lê o tipo do candidato", () => {
    expect(candidateKind("candidate:1 1 udp 2122 10.0.0.2 5000 typ host")).toBe("host");
    expect(candidateKind("candidate:2 1 udp 1686 200.1.1.1 6000 typ srflx raddr 10.0.0.2")).toBe("srflx");
    expect(candidateKind("candidate:3 1 udp 41 1.2.3.4 7000 typ relay raddr 0.0.0.0")).toBe("relay");
    expect(candidateKind("lixo")).toBeNull();
  });
  it("relay funcionando é conexão boa", () => {
    expect(classifyConnection(["host", "srflx", "relay"], true).level).toBe("boa");
    expect(classifyConnection(["host", "relay"], true).summary).toMatch(/relay/i);
  });
  it("sem relay mas com STUN é limitada e avisa do TURN", () => {
    const sem = classifyConnection(["host", "srflx"], false);
    expect(sem.level).toBe("limitada");
    expect(sem.tips.join(" ")).toMatch(/Nenhum servidor TURN/);
    expect(classifyConnection(["host", "srflx"], true).tips.join(" ")).toMatch(/não respondeu/);
  });
  it("só host é ruim e sem nada também", () => {
    expect(classifyConnection(["host"], false).level).toBe("ruim");
    expect(classifyConnection([], false).level).toBe("ruim");
  });
});

describe("medições do teste de rede", () => {
  it("resume idas e voltas: mediana, variação e perda", () => {
    expect(summarizeRoundTrips([10, 20, null, 30])).toEqual({ rttMs: 20, jitterMs: 10, lossPct: 25 });
    expect(summarizeRoundTrips([40, 60])).toEqual({ rttMs: 50, jitterMs: 20, lossPct: 0 });
    expect(summarizeRoundTrips([])).toBeNull();
    expect(summarizeRoundTrips([null, null])?.lossPct).toBe(100);
  });
  it("perda e latência alta rebaixam o nível", () => {
    const base = classifyConnection(["host", "srflx", "relay"], true);
    expect(applyPathMetrics(base, { relayRttMs: 40, relayJitterMs: 5, relayLossPct: 0 }).level).toBe("boa");
    expect(applyPathMetrics(base, { relayRttMs: 200, relayLossPct: 0 }).level).toBe("limitada");
    expect(applyPathMetrics(base, { relayRttMs: 40, relayLossPct: 12 }).level).toBe("ruim");
    expect(applyPathMetrics(base, { relayRttMs: 40, relayLossPct: 4 }).tips.join(" ")).toMatch(/4%/);
  });
  it("não sobe um nível que já era ruim", () => {
    const ruim = classifyConnection(["host"], false);
    expect(applyPathMetrics(ruim, { relayLossPct: 4 }).level).toBe("ruim");
  });
  it("avisa quando a chamada vai precisar do relay", () => {
    const soRelay = applyPathMetrics(classifyConnection(["host", "relay"], true), {});
    expect(soRelay.needsRelay).toBe(true);
    expect(soRelay.tips.join(" ")).toMatch(/passar pelo relay/);
    expect(applyPathMetrics(classifyConnection(["host", "srflx"], false), {}).needsRelay).toBe(false);
  });
});
