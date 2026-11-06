import { describe, expect, it } from "vitest";
import { deriveInviteRendezvousId, generateBriksInviteCode, InviteAttemptLimiter, normalizeBriksInviteCode, validateBriksInviteCode } from "./code";

describe("códigos de convite Briks", () => {
  it("gera códigos criptográficos no formato amigável e sem caracteres ambíguos", () => {
    const codes = new Set(Array.from({ length: 100 }, generateBriksInviteCode));
    expect(codes.size).toBe(100);
    codes.forEach((code) => expect(code).toMatch(/^briks-(?:[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}-){3}[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}$/));
  });

  it("normaliza caixa, espaços, código cru e deep links", () => {
    expect(normalizeBriksInviteCode(" Briks-7h4k-m9p2-q8rf-a3dx ")).toBe("briks-7H4K-M9P2-Q8RF-A3DX");
    expect(normalizeBriksInviteCode("7h4km9p2q8rfa3dx")).toBe("briks-7H4K-M9P2-Q8RF-A3DX");
    expect(normalizeBriksInviteCode("briks://friend/briks-7h4k-m9p2-q8rf-a3dx")).toBe("briks-7H4K-M9P2-Q8RF-A3DX");
    expect(normalizeBriksInviteCode("briks://group/7h4k-m9p2-q8rf-a3dx")).toBe("briks-7H4K-M9P2-Q8RF-A3DX");
    expect(validateBriksInviteCode("briks-123")).toBe(false);
  });

  it("aceita códigos copiados de mensagens com wrappers e caracteres invisíveis", () => {
    expect(normalizeBriksInviteCode("`briks-7H4K-M9P2-Q8RF-A3DX`")).toBe("briks-7H4K-M9P2-Q8RF-A3DX");
    expect(normalizeBriksInviteCode("Meu código é briks-7H4K-M9P2-Q8RF-A3DX, entra aí")).toBe("briks-7H4K-M9P2-Q8RF-A3DX");
    expect(normalizeBriksInviteCode("briks-7H4K–M9P2–Q8RF–A3DX")).toBe("briks-7H4K-M9P2-Q8RF-A3DX");
    expect(normalizeBriksInviteCode("briks-7H4K-M9P2\u200B-Q8RF-A3DX")).toBe("briks-7H4K-M9P2-Q8RF-A3DX");
    expect(validateBriksInviteCode("Compartilhe: `briks-7h4k-m9p2-q8rf-a3dx`" )).toBe(true);
  });

  it("deriva rendezvous determinístico com namespaces separados", async () => {
    const code = "briks-7H4K-M9P2-Q8RF-A3DX";
    expect(await deriveInviteRendezvousId("friend", code)).toBe(await deriveInviteRendezvousId("friend", code.toLowerCase()));
    expect(await deriveInviteRendezvousId("friend", code)).not.toBe(await deriveInviteRendezvousId("group", code));
  });

  it("limita tentativas locais em uma janela", () => {
    const limiter = new InviteAttemptLimiter(2, 1_000);
    expect(limiter.consume(0)).toBe(true); expect(limiter.consume(10)).toBe(true); expect(limiter.consume(20)).toBe(false); expect(limiter.consume(1_100)).toBe(true);
  });
});
