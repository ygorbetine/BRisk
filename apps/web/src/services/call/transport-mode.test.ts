import { describe, expect, it } from "vitest";
import { normalizeTransportMode } from "./transport-mode";

describe("modo da chamada", () => {
  it("aceita mesh e sfu, o resto vira automático", () => {
    expect(normalizeTransportMode("mesh")).toBe("mesh");
    expect(normalizeTransportMode("sfu")).toBe("sfu");
    expect(normalizeTransportMode("qualquer")).toBe("auto");
    expect(normalizeTransportMode(null)).toBe("auto");
  });
});
