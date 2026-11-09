import { describe, expect, it } from "vitest";
import { buildStatus, cleanStatusEmoji, cleanStatusText, expiryToTimestamp, formatStatus, isStatusActive, parseRemoteStatus, STATUS_MAX_LENGTH } from "./status";

const NOW = new Date(2026, 10, 9, 15, 0, 0).getTime();

describe("status personalizado", () => {
  it("limita o texto a 60 caracteres e remove controles", () => {
    expect(cleanStatusText("a".repeat(100))).toHaveLength(STATUS_MAX_LENGTH);
    expect(cleanStatusText("  oi\n‮  mundo\u0007 ")).toBe("oi mundo");
  });
  it("não corta emoji ao meio ao limitar", () => {
    expect([...cleanStatusText("😀".repeat(80))]).toHaveLength(STATUS_MAX_LENGTH);
  });
  it("aceita um emoji e recusa texto comum", () => {
    expect(cleanStatusEmoji("🛠️")).toBe("🛠️");
    expect(cleanStatusEmoji("a")).toBe("");
    expect(cleanStatusEmoji("")).toBe("");
  });
  it("calcula a validade", () => {
    expect(expiryToTimestamp("1h", NOW)).toBe(NOW + 3_600_000);
    expect(expiryToTimestamp("4h", NOW)).toBe(NOW + 14_400_000);
    expect(expiryToTimestamp("never", NOW)).toBeNull();
    expect(expiryToTimestamp("today", NOW)).toBe(new Date(2026, 10, 10, 0, 0, 0).getTime());
  });
  it("não cria status vazio e expira no horário", () => {
    expect(buildStatus("", "   ", "1h", NOW)).toBeNull();
    const status = buildStatus("🎮", "Jogando", "1h", NOW)!;
    expect(isStatusActive(status, NOW + 1)).toBe(true);
    expect(isStatusActive(status, NOW + 3_600_001)).toBe(false);
    expect(formatStatus(status)).toBe("🎮 Jogando");
  });
  it("valida dados vindos da rede", () => {
    expect(parseRemoteStatus(null)).toBeNull();
    expect(parseRemoteStatus("x")).toBeNull();
    expect(parseRemoteStatus({ emoji: "<script>", text: "ok", expiresAt: null }, NOW)).toEqual({ emoji: "", text: "ok", expiresAt: null });
    expect(parseRemoteStatus({ text: "x".repeat(500), expiresAt: NOW + 1000 }, NOW)?.text).toHaveLength(STATUS_MAX_LENGTH);
    expect(parseRemoteStatus({ text: "velho", expiresAt: NOW - 1 }, NOW)).toBeNull();
  });
});
