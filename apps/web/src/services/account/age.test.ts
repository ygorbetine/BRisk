import { describe, expect, it } from "vitest";
import { checkAdult } from "./age";

const today = new Date(2026, 10, 8); // 8 de novembro de 2026

describe("checkAdult", () => {
  it("aceita quem faz 18 anos hoje e rejeita quem faz amanhã", () => {
    expect(checkAdult("2008-11-08", today)).toEqual({ ok: true });
    expect(checkAdult("2008-11-09", today)).toMatchObject({ ok: false });
  });

  it("aceita adultos e rejeita menores", () => {
    expect(checkAdult("1990-02-15", today)).toEqual({ ok: true });
    expect(checkAdult("2015-06-01", today)).toMatchObject({ ok: false, message: expect.stringContaining("18") });
  });

  it("rejeita vazio, formato errado, data inexistente, futuro e idade absurda", () => {
    expect(checkAdult("", today)).toMatchObject({ ok: false });
    expect(checkAdult("08/11/2000", today)).toMatchObject({ ok: false });
    expect(checkAdult("2001-02-30", today)).toMatchObject({ ok: false });
    expect(checkAdult("2030-01-01", today)).toMatchObject({ ok: false });
    expect(checkAdult("1800-01-01", today)).toMatchObject({ ok: false });
  });
});
