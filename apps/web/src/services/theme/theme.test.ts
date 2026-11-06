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
