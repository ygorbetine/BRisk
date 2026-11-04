import { describe, expect, it } from "vitest";
import { MESH_HARD_MAX_PARTICIPANTS, MESH_PREFERRED_MAX_PARTICIPANTS, selectCallTransport } from "./selection";

describe("selectCallTransport", () => {
  it("mantém chamadas pequenas no Mesh", () => {
    expect(selectCallTransport({ participantCount: 4, sfuAvailable: true })).toMatchObject({
      selected: "mesh",
      recommended: "mesh",
      reason: "small-call",
    });
  });

  it("seleciona SFU automaticamente acima do limite preferido do Mesh", () => {
    expect(selectCallTransport({ participantCount: MESH_PREFERRED_MAX_PARTICIPANTS + 1, sfuAvailable: true })).toMatchObject({
      selected: "sfu",
      recommended: "sfu",
      reason: "participant-threshold",
    });
  });

  it("recomenda SFU sem quebrar o fallback quando o provedor ainda não existe", () => {
    const result = selectCallTransport({ participantCount: MESH_HARD_MAX_PARTICIPANTS + 1, sfuAvailable: false });
    expect(result).toMatchObject({ selected: "mesh", recommended: "sfu", reason: "sfu-unavailable" });
    expect(result.label).toContain("não configurado");
  });

  it("considera qualidade ruim mesmo em uma chamada pequena", () => {
    expect(selectCallTransport({
      participantCount: 3,
      sfuAvailable: true,
      network: { roundTripTimeMs: 351 },
    })).toMatchObject({ selected: "sfu", reason: "degraded-network", networkDegraded: true });
  });

  it("respeita escolha manual e recusa SFU indisponível", () => {
    const manualMesh = selectCallTransport({ participantCount: 8, preference: "mesh", sfuAvailable: true });
    expect(manualMesh.selected).toBe("mesh");
    expect(manualMesh.label).toContain("manual");
    expect(selectCallTransport({ participantCount: 2, preference: "sfu", sfuAvailable: false })).toMatchObject({
      selected: "mesh",
      recommended: "sfu",
      reason: "sfu-unavailable",
    });
  });

  it("recusa quantidade inválida", () => {
    expect(() => selectCallTransport({ participantCount: 0, sfuAvailable: false })).toThrow("participantCount");
  });

  it("distingue recomendação do limite físico do Mesh", () => {
    expect(selectCallTransport({ participantCount: MESH_HARD_MAX_PARTICIPANTS + 1, sfuAvailable: false })).toMatchObject({
      selected: "mesh",
      recommended: "sfu",
      meshCapacityExceeded: true,
    });
  });
});
