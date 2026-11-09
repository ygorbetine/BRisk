import { describe, expect, it } from "vitest";
import {
  RateLimiter, bearerToken, clampTtl, isValidPeerId, isValidRoom, jwtRole, originAllowed, parseAllowedOrigins,
} from "../../../../../supabase/functions/_shared/guard";

const jwt = (role: string) => `h.${btoa(JSON.stringify({ role })).replace(/=+$/, "")}.s`;

describe("validação das Edge Functions", () => {
  it("aceita só sessão authenticated, não a chave anon", () => {
    expect(jwtRole(jwt("authenticated"))).toBe("authenticated");
    expect(jwtRole(jwt("anon"))).toBe("anon");
    expect(jwtRole("lixo")).toBeNull();
    expect(bearerToken("Bearer abcdefghijklmnopqrstu.v.w")).toBe("abcdefghijklmnopqrstu.v.w");
    expect(bearerToken("Basic xxx")).toBeNull();
  });
  it("valida sala e peerId", () => {
    expect(isValidRoom("risk-rendezvous-v1:abc-123:voice:def")).toBe(true);
    expect(isValidRoom("sala com espaço")).toBe(false);
    expect(isValidRoom("a".repeat(161))).toBe(false);
    expect(isValidPeerId("123e4567-e89b-42d3-a456-426614174000")).toBe(true);
    expect(isValidPeerId("admin")).toBe(false);
  });
  it("limita o TTL a 1 h", () => {
    expect(clampTtl(undefined)).toBe(3600);
    expect(clampTtl("86400")).toBe(3600);
    expect(clampTtl(10)).toBe(300);
    expect(clampTtl(1800)).toBe(1800);
  });
  it("restringe CORS", () => {
    expect(originAllowed("https://evil.example", [])).toBe(false);
    expect(originAllowed("http://localhost:5173", [])).toBe(true);
    expect(originAllowed(null, [])).toBe(true);
    const list = parseAllowedOrigins("https://briks.app/, https://x.example");
    expect(originAllowed("https://briks.app", list)).toBe(true);
    expect(originAllowed("http://localhost:5173", list)).toBe(false);
  });
  it("limita requisições por janela", () => {
    const limiter = new RateLimiter(2, 1000);
    expect(limiter.check("u", 0)).toBe(0);
    expect(limiter.check("u", 10)).toBe(0);
    expect(limiter.check("u", 20)).toBeGreaterThan(0);
    expect(limiter.check("outro", 20)).toBe(0);
    expect(limiter.check("u", 1500)).toBe(0);
  });
});
