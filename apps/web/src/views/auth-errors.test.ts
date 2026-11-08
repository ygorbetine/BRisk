import { describe, expect, it } from "vitest";
import { friendlyAuthError } from "./auth-errors";

describe("friendlyAuthError", () => {
  it("explica credenciais inválidas", () => {
    expect(friendlyAuthError(Object.assign(new Error("Não autorizado"), { status: 401 }), false)).toMatch(/E-mail ou senha/);
  });
  it("trata falha de rede", () => {
    expect(friendlyAuthError(new TypeError("Failed to fetch"), false)).toMatch(/servidor/);
  });
  it("trata conta duplicada e erro 500", () => {
    expect(friendlyAuthError(Object.assign(new Error("x"), { status: 409 }), true)).toMatch(/Já existe/);
    expect(friendlyAuthError(Object.assign(new Error("x"), { status: 503 }), false)).toMatch(/problema/);
  });
});
