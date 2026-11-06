import { describe, expect, it } from "vitest";
import { candidateKind, classifyConnection } from "./connection-test";

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
