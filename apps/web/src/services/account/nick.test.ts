import { describe, expect, it } from "vitest";
import { checkNick, friendlyAccountError, normalizeNick } from "./nick";

describe("@nick", () => {
  it("normaliza @, espaços e maiúsculas", () => {
    expect(normalizeNick("  @Ygor_B ")).toBe("ygor_b");
  });
  it("aceita nicks válidos", () => {
    expect(checkNick("@Sticker_01")).toEqual({ ok: true, nick: "sticker_01" });
  });
  it("recusa curto, longo, acento, símbolo e reservado", () => {
    for (const bad of ["ab", "a".repeat(21), "joão", "a-b-c", "ola mundo", "@admin", "briks"]) {
      expect(checkNick(bad).ok).toBe(false);
    }
  });
  it("traduz erros do servidor", () => {
    expect(friendlyAccountError(new Error("nick_taken"))).toContain("já está em uso");
    expect(friendlyAccountError({ message: "Invalid login credentials" })).toContain("incorretos");
  });
});
