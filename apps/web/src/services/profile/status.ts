/** Status personalizado: emoji + texto curto, com validade opcional. Viaja na presença do Supabase Realtime. */
export const STATUS_MAX_LENGTH = 60;
export const STATUS_EXPIRY_OPTIONS = ["1h", "4h", "today", "never"] as const;
export type StatusExpiry = (typeof STATUS_EXPIRY_OPTIONS)[number];

export type CustomStatus = { emoji: string; text: string; expiresAt: number | null };

export const STATUS_PRESETS: ReadonlyArray<{ emoji: string; text: string }> = [
  { emoji: "🎮", text: "Jogando" },
  { emoji: "📚", text: "Estudando" },
  { emoji: "💼", text: "Trabalhando" },
  { emoji: "🎧", text: "Ouvindo música" },
  { emoji: "😴", text: "Ausente" },
];

const HOUR = 3_600_000;

/** Remove caracteres de controle e espaços repetidos; limita o tamanho pelo número de caracteres visíveis. */
export function cleanStatusText(raw: string): string {
  const compact = raw.replace(/[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁦-⁩]/g, "").replace(/\s+/g, " ").trim();
  return [...compact].slice(0, STATUS_MAX_LENGTH).join("");
}

/** Aceita só um emoji (um grafema); qualquer outra coisa vira vazio. */
export function cleanStatusEmoji(raw: string): string {
  const value = raw.trim();
  if (!value) return "";
  const Segmenter = (Intl as { Segmenter?: new (locale?: string, options?: { granularity: "grapheme" }) => { segment(input: string): Iterable<{ segment: string }> } }).Segmenter;
  const graphemes = Segmenter ? [...new Segmenter(undefined, { granularity: "grapheme" }).segment(value)].map((part) => part.segment) : [...value];
  const first = graphemes[0] ?? "";
  return /\p{Extended_Pictographic}|\p{Regional_Indicator}|[#*0-9]️?⃣/u.test(first) ? first : "";
}

export function expiryToTimestamp(expiry: StatusExpiry, now: number): number | null {
  if (expiry === "1h") return now + HOUR;
  if (expiry === "4h") return now + 4 * HOUR;
  if (expiry === "today") {
    const end = new Date(now);
    end.setHours(24, 0, 0, 0);
    return end.getTime();
  }
  return null;
}

export function buildStatus(emoji: string, text: string, expiry: StatusExpiry, now = Date.now()): CustomStatus | null {
  const cleanText = cleanStatusText(text);
  const cleanEmoji = cleanStatusEmoji(emoji);
  if (!cleanText && !cleanEmoji) return null;
  return { emoji: cleanEmoji, text: cleanText, expiresAt: expiryToTimestamp(expiry, now) };
}

export function isStatusActive(status: CustomStatus | null | undefined, now = Date.now()): status is CustomStatus {
  if (!status) return false;
  if (!status.text && !status.emoji) return false;
  return status.expiresAt === null || status.expiresAt > now;
}

/** Valida o que chega da rede: nunca confiar no formato vindo de outro usuário. */
export function parseRemoteStatus(raw: unknown, now = Date.now()): CustomStatus | null {
  if (!raw || typeof raw !== "object") return null;
  const value = raw as Record<string, unknown>;
  const emoji = typeof value.emoji === "string" ? cleanStatusEmoji(value.emoji) : "";
  const text = typeof value.text === "string" ? cleanStatusText(value.text) : "";
  const expiresAt = typeof value.expiresAt === "number" && Number.isFinite(value.expiresAt) ? value.expiresAt : null;
  const status = { emoji, text, expiresAt };
  return isStatusActive(status, now) ? status : null;
}

export function formatStatus(status: CustomStatus): string {
  return [status.emoji, status.text].filter(Boolean).join(" ");
}
