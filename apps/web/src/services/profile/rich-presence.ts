/** Rich presence: o programa em primeiro plano (jogo ou trabalho), com ícone. Separado do status personalizado. */
export type RichApp = { name: string; label: string; icon?: string };
export type RichPresenceSettings = { enabled: boolean; ignored: string[] };

const KEY = "briks.richPresence.v1";
export const MAX_ICON_BYTES = 8_192;
const ICON_PATTERN = /^data:image\/png;base64,[A-Za-z0-9+/=]+$/;

export function defaultSettings(): RichPresenceSettings { return { enabled: false, ignored: [] }; }

export function loadRichSettings(): RichPresenceSettings {
  try {
    const raw = JSON.parse(window.localStorage.getItem(KEY) ?? "null") as Partial<RichPresenceSettings> | null;
    if (!raw || typeof raw !== "object") return defaultSettings();
    const ignored = Array.isArray(raw.ignored) ? raw.ignored.filter((item): item is string => typeof item === "string").map((item) => item.toLowerCase()).slice(0, 100) : [];
    return { enabled: raw.enabled === true, ignored };
  } catch { return defaultSettings(); }
}

export function saveRichSettings(settings: RichPresenceSettings): void {
  try { window.localStorage.setItem(KEY, JSON.stringify(settings)); } catch { /* vale só nesta sessão */ }
}

export function validIcon(value: unknown): value is string {
  return typeof value === "string" && value.length <= MAX_ICON_BYTES * 1.4 && ICON_PATTERN.test(value);
}

/** Valida o que vem da rede: nome curto, sem controles, ícone só PNG pequeno. */
export function parseRemoteApp(raw: unknown): RichApp | null {
  if (!raw || typeof raw !== "object") return null;
  const value = raw as Record<string, unknown>;
  const clean = (input: unknown, max: number) => typeof input === "string" ? [...input.replace(/[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁦-⁩]/g, "").trim()].slice(0, max).join("") : "";
  const name = clean(value.name, 60).toLowerCase();
  const label = clean(value.label, 40);
  if (!name || !label) return null;
  return { name, label, ...(validIcon(value.icon) ? { icon: value.icon } : {}) };
}

export function isIgnored(settings: RichPresenceSettings, name: string): boolean {
  return settings.ignored.includes(name.toLowerCase());
}

/** Reduz o ícone do Windows (normalmente 32×32) para 24×24 PNG, pequeno o bastante para a presença. */
export async function shrinkIcon(dataUrl: string): Promise<string | undefined> {
  try {
    const image = new Image();
    image.src = dataUrl;
    await image.decode();
    const canvas = document.createElement("canvas");
    canvas.width = 24;
    canvas.height = 24;
    const context = canvas.getContext("2d");
    if (!context) return undefined;
    context.drawImage(image, 0, 0, 24, 24);
    const result = canvas.toDataURL("image/png");
    return validIcon(result) ? result : undefined;
  } catch { return undefined; }
}

export async function readForeground(): Promise<{ name: string; label: string; path?: string } | null> {
  if (!window.desktop?.getBackendConfig) return null;
  try {
    const config = await window.desktop.getBackendConfig();
    const response = await fetch(`${config.baseUrl.replace(/\/$/, "")}/presence/foreground`, { headers: { "x-briks-desktop-token": config.token } });
    if (!response.ok) return null;
    const body = await response.json() as { name?: unknown; label?: unknown; path?: unknown } | null;
    if (!body || typeof body.name !== "string" || typeof body.label !== "string") return null;
    return { name: body.name, label: body.label, ...(typeof body.path === "string" && body.path.length <= 1_024 ? { path: body.path } : {}) };
  } catch { return null; }
}
