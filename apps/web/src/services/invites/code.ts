export type InviteType = "friend" | "group";

const ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
const CODE_PATTERN = /^briks-(?:[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}-){3}[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}$/;
const CODE_CHUNK = `[${ALPHABET}]{4}`;
const PREFIXED_CODE_PATTERN = new RegExp(`BRIKS\\s*-\\s*(${CODE_CHUNK}(?:[\\s-]*${CODE_CHUNK}){3})`);
const DEEP_LINK_PATTERN = /BRIKS:\/\/(?:INVITE|FRIEND|GROUP)\/([^\s?#]+)/;
const UNICODE_DASHES = /[\u2010-\u2015\u2212\uFE58\uFE63\uFF0D]/g;
const ZERO_WIDTH = /[\u200B-\u200D\u2060\uFEFF]/g;

export function generateBriksInviteCode(): string {
  const values = new Uint8Array(16);
  crypto.getRandomValues(values);
  const body = [...values].map((value) => ALPHABET[value % ALPHABET.length]).join("");
  return `briks-${body.match(/.{4}/g)!.join("-")}`;
}

export function normalizeBriksInviteCode(input: string): string {
  let value = input
    .normalize("NFKC")
    .replace(ZERO_WIDTH, "")
    .replace(UNICODE_DASHES, "-")
    .trim()
    .toUpperCase();

  // Se o código veio dentro de uma mensagem (`briks-...`, por exemplo entre
  // crases), extraímos apenas o token válido e ignoramos o texto ao redor.
  const embedded = value.match(PREFIXED_CODE_PATTERN);
  if (embedded) return formatInviteBody(embedded[1]!);

  // Deep links podem carregar o código com ou sem o prefixo `briks-`.
  const deepLink = value.match(DEEP_LINK_PATTERN);
  if (deepLink) value = deepLink[1]!;

  value = value.replace(/^BRIKS\s*-?\s*/, "");
  return formatInviteBody(value);
}

function formatInviteBody(value: string): string {
  const compact = value.replace(/[\s-]+/g, "");
  const groups = compact.match(/.{1,4}/g)?.join("-") ?? "";
  return `briks-${groups}`;
}

export function validateBriksInviteCode(input: string): boolean {
  return CODE_PATTERN.test(normalizeBriksInviteCode(input));
}

export async function deriveInviteRendezvousId(type: InviteType, input: string): Promise<string> {
  const code = normalizeBriksInviteCode(input);
  if (!validateBriksInviteCode(code)) throw new Error("Código de convite inválido.");
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`briks:${type}:${code}`));
  return [...new Uint8Array(digest)].map((value) => value.toString(16).padStart(2, "0")).join("");
}

export class InviteAttemptLimiter {
  private attempts: number[] = [];
  constructor(private readonly maximum = 8, private readonly windowMs = 60_000) {}
  consume(now = Date.now()): boolean {
    this.attempts = this.attempts.filter((timestamp) => now - timestamp < this.windowMs);
    if (this.attempts.length >= this.maximum) return false;
    this.attempts.push(now);
    return true;
  }
}
