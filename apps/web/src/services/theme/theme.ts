import { normalizeHex } from "./color";

export type ThemeColors = { c1: string; c2: string; c3: string };
export type ThemePreset = { id: string; name: string; colors: ThemeColors };

export const DEFAULT_THEME: ThemeColors = { c1: "#b01fcf", c2: "#6d5cf6", c3: "#8b5cf6" };

export const THEME_PRESETS: ThemePreset[] = [
  { id: "briks", name: "Briks", colors: DEFAULT_THEME },
  { id: "oceano", name: "Oceano", colors: { c1: "#0ea5c6", c2: "#2f6bff", c3: "#5aa9ff" } },
  { id: "por-do-sol", name: "Pôr do sol", colors: { c1: "#ff5e7e", c2: "#ff8a3d", c3: "#ffb454" } },
  { id: "floresta", name: "Floresta", colors: { c1: "#1fbf75", c2: "#0e9f9a", c3: "#6fdc8c" } },
  { id: "rosa", name: "Rosa neon", colors: { c1: "#ff2bd6", c2: "#8a3dff", c3: "#ff7ad9" } },
];

const STORAGE_KEY = "briks.theme.v1";

export function sanitizeTheme(value: unknown): ThemeColors {
  const item = (value && typeof value === "object" ? value : {}) as Record<string, unknown>;
  const pick = (key: "c1" | "c2" | "c3") => (typeof item[key] === "string" ? normalizeHex(item[key] as string) : null) ?? DEFAULT_THEME[key];
  return { c1: pick("c1"), c2: pick("c2"), c3: pick("c3") };
}

export function loadTheme(): ThemeColors {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? sanitizeTheme(JSON.parse(raw)) : DEFAULT_THEME;
  } catch {
    return DEFAULT_THEME;
  }
}

export function saveTheme(colors: ThemeColors): void {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(colors)); } catch { /* sem armazenamento: o tema vale só nesta sessão */ }
}

/** Aplica as três cores como variáveis CSS; todo o gradiente e os destaques derivam delas. */
export function applyTheme(colors: ThemeColors, root: HTMLElement = document.documentElement): void {
  root.style.setProperty("--briks-c1", colors.c1);
  root.style.setProperty("--briks-c2", colors.c2);
  root.style.setProperty("--briks-c3", colors.c3);
}

export function applyStoredTheme(): void {
  if (typeof document !== "undefined") applyTheme(loadTheme());
}
