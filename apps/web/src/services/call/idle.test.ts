import { describe, expect, it } from "vitest";
import { formatRemaining, IDLE_LIMIT_MS, IDLE_WARNING_MS, idleStatus } from "./idle";

describe("idle", () => {
  it("fica ativo antes dos últimos 5 minutos", () => {
    expect(idleStatus(0).phase).toBe("active");
    expect(idleStatus(IDLE_LIMIT_MS - IDLE_WARNING_MS - 1).phase).toBe("active");
  });
  it("avisa nos últimos 5 minutos e expira em 1 hora", () => {
    expect(idleStatus(IDLE_LIMIT_MS - IDLE_WARNING_MS)).toEqual({ phase: "warning", remainingMs: IDLE_WARNING_MS });
    expect(idleStatus(IDLE_LIMIT_MS).phase).toBe("expired");
    expect(idleStatus(IDLE_LIMIT_MS * 2).phase).toBe("expired");
  });
  it("formata o tempo restante", () => {
    expect(formatRemaining(299_000)).toBe("4:59");
    expect(formatRemaining(5_000)).toBe("0:05");
  });
});
