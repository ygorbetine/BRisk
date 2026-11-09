import { beforeEach, describe, expect, it, vi } from "vitest";
import { cleanNickInput, rememberLogin, resolveLoginIdentifier } from "./login-hints";

describe("login-hints", () => {
  beforeEach(() => {
    const store = new Map<string, string>();
    vi.stubGlobal("localStorage", { getItem: (k: string) => store.get(k) ?? null, setItem: (k: string, v: string) => void store.set(k, v), removeItem: (k: string) => void store.delete(k) });
  });

  it("aceita e-mail direto", () => {
    expect(resolveLoginIdentifier(" ana@x.com ")).toEqual({ email: "ana@x.com" });
  });
  it("resolve @nick lembrado, com ou sem @", () => {
    rememberLogin("Ana", "Ana@X.com");
    expect(resolveLoginIdentifier("@ana")).toEqual({ email: "ana@x.com" });
    expect(resolveLoginIdentifier("ANA")).toEqual({ email: "ana@x.com" });
  });
  it("explica quando o @nick é desconhecido ou vazio", () => {
    expect(resolveLoginIdentifier("@fulano")).toHaveProperty("error");
    expect(resolveLoginIdentifier("  ")).toHaveProperty("error");
    expect(cleanNickInput("@@Beto ")).toBe("beto");
  });
});
