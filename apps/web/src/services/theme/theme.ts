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

export type CustomTheme = { id: string; name: string; colors: ThemeColors };

const CUSTOM_KEY = "briks.themes.custom.v1";
export const MAX_CUSTOM_THEMES = 24;
export const MAX_THEME_NAME = 24;

export function cleanThemeName(value: string): string {
  return value.replace(/\s+/g, " ").trim().slice(0, MAX_THEME_NAME);
}

export function sanitizeCustomThemes(value: unknown): CustomTheme[] {
  if (!Array.isArray(value)) return [];
  const seen = new Set<string>();
  const result: CustomTheme[] = [];
  for (const item of value) {
    if (!item || typeof item !== "object") continue;
    const record = item as Record<string, unknown>;
    const name = typeof record.name === "string" ? cleanThemeName(record.name) : "";
    const id = typeof record.id === "string" ? record.id.slice(0, 64) : "";
    if (!name || !id || seen.has(id)) continue;
    seen.add(id);
    result.push({ id, name, colors: sanitizeTheme(record.colors) });
    if (result.length >= MAX_CUSTOM_THEMES) break;
  }
  return result;
}

export function loadCustomThemes(): CustomTheme[] {
  try {
    const raw = localStorage.getItem(CUSTOM_KEY);
    return raw ? sanitizeCustomThemes(JSON.parse(raw)) : [];
  } catch {
    return [];
  }
}

function writeCustomThemes(themes: CustomTheme[]): void {
  try { localStorage.setItem(CUSTOM_KEY, JSON.stringify(themes)); } catch { /* sem armazenamento: os temas valem só nesta sessão */ }
}

/** Salva um tema com nome; se o nome já existir (sem diferenciar maiúsculas), substitui as cores desse. */
export function saveCustomTheme(name: string, colors: ThemeColors, current: CustomTheme[] = loadCustomThemes()): CustomTheme[] {
  const clean = cleanThemeName(name);
  if (!clean) return current;
  const existing = current.find((theme) => theme.name.toLocaleLowerCase() === clean.toLocaleLowerCase());
  const next = existing
    ? current.map((theme) => (theme === existing ? { ...theme, colors } : theme))
    : [...current, { id: `custom-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`, name: clean, colors }].slice(-MAX_CUSTOM_THEMES);
  writeCustomThemes(next);
  return next;
}

export function deleteCustomTheme(id: string, current: CustomTheme[] = loadCustomThemes()): CustomTheme[] {
  const next = current.filter((theme) => theme.id !== id);
  writeCustomThemes(next);
  return next;
}

export function sameColors(a: ThemeColors, b: ThemeColors): boolean {
  return a.c1.toLowerCase() === b.c1.toLowerCase() && a.c2.toLowerCase() === b.c2.toLowerCase() && a.c3.toLowerCase() === b.c3.toLowerCase();
}
