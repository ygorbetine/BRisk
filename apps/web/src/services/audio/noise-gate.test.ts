import { describe, expect, it } from "vitest";
import { clampGateThreshold } from "./noise-gate";

describe("clampGateThreshold", () => {
  it("limita e arredonda", () => {
    expect(clampGateThreshold(-100)).toBe(-70);
    expect(clampGateThreshold(0)).toBe(-20);
    expect(clampGateThreshold(-44.6)).toBe(-45);
    expect(clampGateThreshold("x")).toBe(-45);
    expect(clampGateThreshold(NaN)).toBe(-45);
  });
});
