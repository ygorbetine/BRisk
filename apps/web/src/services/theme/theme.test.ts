import { describe, expect, it } from "vitest";
import { DEFAULT_THEME, THEME_PRESETS, sanitizeTheme } from "./theme";

describe("tema", () => {
  it("usa o padrão para valores inválidos", () => {
    expect(sanitizeTheme(null)).toEqual(DEFAULT_THEME);
    expect(sanitizeTheme({ c1: "vermelho", c2: 3 })).toEqual(DEFAULT_THEME);
  });
  it("normaliza cores válidas", () => {
    expect(sanitizeTheme({ c1: "#FF0000", c2: "00ff00", c3: "#0000FF" })).toEqual({ c1: "#ff0000", c2: "#00ff00", c3: "#0000ff" });
  });
  it("todas as predefinições são válidas", () => {
    for (const preset of THEME_PRESETS) expect(sanitizeTheme(preset.colors)).toEqual(preset.colors);
  });
});

import { afterEach, vi } from "vitest";
import { cleanThemeName, deleteCustomTheme, sameColors, sanitizeCustomThemes, saveCustomTheme } from "./theme";

describe("temas personalizados", () => {
  afterEach(() => vi.unstubAllGlobals());
  const colors = { c1: "#112233", c2: "#445566", c3: "#778899" };

  it("limpa o nome", () => {
    expect(cleanThemeName("  Meu   tema  ")).toBe("Meu tema");
    expect(cleanThemeName("x".repeat(60)).length).toBe(24);
  });
  it("descarta itens inválidos", () => {
    expect(sanitizeCustomThemes("nada")).toEqual([]);
    expect(sanitizeCustomThemes([{ id: "a", name: "", colors }, { id: "b", name: "Ok", colors }, { id: "b", name: "Repetido", colors }]).map((theme) => theme.name)).toEqual(["Ok"]);
  });
  it("salva, atualiza pelo nome e apaga", () => {
    vi.stubGlobal("localStorage", { getItem: () => null, setItem: () => undefined });
    const first = saveCustomTheme("Noite", colors, []);
    expect(first).toHaveLength(1);
    const updated = saveCustomTheme("noite", { ...colors, c1: "#ffffff" }, first);
    expect(updated).toHaveLength(1);
    expect(updated[0]?.colors.c1).toBe("#ffffff");
    expect(deleteCustomTheme(updated[0]!.id, updated)).toEqual([]);
  });
  it("ignora nome vazio", () => {
    expect(saveCustomTheme("   ", colors, [])).toEqual([]);
  });
  it("compara cores sem diferenciar maiúsculas", () => {
    expect(sameColors(colors, { c1: "#112233", c2: "#445566", c3: "#778899" })).toBe(true);
    expect(sameColors(colors, { ...colors, c3: "#000000" })).toBe(false);
  });
});
