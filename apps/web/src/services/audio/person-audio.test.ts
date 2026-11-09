import { beforeEach, describe, expect, it } from "vitest";
import { loadPersonAudio, personKey, sanitizePersonAudio, savePersonAudio } from "./person-audio";

describe("person-audio", () => {
  beforeEach(() => {
    const data = new Map<string, string>();
    Object.defineProperty(globalThis, "localStorage", {
      configurable: true,
      value: { getItem: (k: string) => data.get(k) ?? null, setItem: (k: string, v: string) => void data.set(k, v), removeItem: (k: string) => void data.delete(k) },
    });
  });

  it("usa o nome em minúsculas como chave", () => {
    expect(personKey("  Ana ")).toBe("ana");
  });

  it("guarda e devolve volume e silêncio por pessoa", () => {
    savePersonAudio("Ana", { user: 150, screen: 40, muted: true });
    expect(loadPersonAudio("ana")).toEqual({ user: 150, screen: 40, muted: true });
    expect(loadPersonAudio("Davi")).toEqual({ user: 100, screen: 100, muted: false });
  });

  it("volta ao padrão sem guardar nada quando tudo está normal", () => {
    savePersonAudio("Ana", { user: 150, screen: 100, muted: false });
    savePersonAudio("Ana", { user: 100, screen: 100, muted: false });
    expect(localStorage.getItem("briks.personAudio.v1")).toBe("{}");
  });

  it("limita volumes inválidos", () => {
    expect(sanitizePersonAudio({ user: 999, screen: -5, muted: "sim" })).toEqual({ user: 200, screen: 0, muted: false });
    expect(sanitizePersonAudio(null)).toEqual({ user: 100, screen: 100, muted: false });
  });
});
