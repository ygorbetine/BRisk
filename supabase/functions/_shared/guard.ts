// Lógica pura e compartilhada das Edge Functions (sem APIs do Deno, testável no Vitest).
// Sem estado persistente: o limitador vive na memória de cada instância (isolate) da função.

export const MAX_TTL_SECONDS = 3600;
export const ROOM_PATTERN = /^[A-Za-z0-9:_.\-]{1,160}$/;
const UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export function bearerToken(header: string | null | undefined): string | null {
  const match = /^Bearer\s+([A-Za-z0-9._\-]{20,4096})$/.exec((header ?? "").trim());
  return match?.[1] ?? null;
}

/** O JWT público `anon` tem role=anon: nunca vale como sessão de usuário. */
export function jwtRole(token: string): string | null {
  try {
    const payload = (token.split(".")[1] ?? "").replace(/-/g, "+").replace(/_/g, "/");
    const json = JSON.parse(atob(payload.padEnd(Math.ceil(payload.length / 4) * 4, "=")));
    return typeof json.role === "string" ? json.role : null;
  } catch { return null; }
}

export function isValidRoom(room: unknown): room is string {
  return typeof room === "string" && ROOM_PATTERN.test(room);
}

export function isValidPeerId(peerId: unknown): peerId is string {
  return typeof peerId === "string" && UUID_PATTERN.test(peerId);
}

/** TTL em segundos: padrão 1 h, nunca acima de 1 h e nunca abaixo de 5 min. */
export function clampTtl(raw: unknown, fallback = MAX_TTL_SECONDS): number {
  const value = Number(raw);
  const base = Number.isFinite(value) && value > 0 ? Math.floor(value) : fallback;
  return Math.min(MAX_TTL_SECONDS, Math.max(300, base));
}

/** ALLOWED_ORIGINS: lista separada por vírgula. */
export function parseAllowedOrigins(raw: string | undefined | null): string[] {
  return (raw ?? "").split(",").map((item) => item.trim().replace(/\/$/, "")).filter(Boolean);
}

/** Sem lista configurada, só origens locais/Electron são aceitas. Sem cabeçalho Origin (cliente nativo), CORS não se aplica. */
export function originAllowed(origin: string | null, allowed: string[]): boolean {
  if (!origin) return true;
  if (allowed.length) return allowed.includes(origin);
  return origin === "null" || /^https?:\/\/(localhost|127\.0\.0\.1|\[::1\])(:\d+)?$/.test(origin) || origin.startsWith("app://");
}

export function corsHeaders(origin: string | null, allowed: string[], methods: string): Record<string, string> {
  const base: Record<string, string> = {
    "access-control-allow-methods": methods,
    "access-control-allow-headers": "authorization, content-type, apikey, accept, x-client-info",
    "access-control-max-age": "600",
    vary: "origin",
  };
  if (origin && originAllowed(origin, allowed)) base["access-control-allow-origin"] = origin;
  return base;
}

export function clientIp(headers: { get(name: string): string | null }): string {
  const forwarded = headers.get("cf-connecting-ip") ?? headers.get("x-real-ip") ?? headers.get("x-forwarded-for") ?? "";
  return (forwarded.split(",")[0] ?? "").trim().slice(0, 64) || "desconhecido";
}

/** Janela deslizante simples por chave. */
export class RateLimiter {
  private readonly hits = new Map<string, number[]>();
  constructor(private readonly limit: number, private readonly windowMs: number, private readonly maxKeys = 5000) {}

  /** Devolve segundos até poder tentar de novo, ou 0 se liberado. */
  check(key: string, now = Date.now()): number {
    const recent = (this.hits.get(key) ?? []).filter((time) => now - time < this.windowMs);
    if (recent.length >= this.limit) {
      this.hits.set(key, recent);
      return Math.max(1, Math.ceil((this.windowMs - (now - (recent[0] ?? now))) / 1000));
    }
    recent.push(now);
    this.hits.delete(key);
    this.hits.set(key, recent);
    if (this.hits.size > this.maxKeys) {
      const oldest = this.hits.keys().next().value;
      if (oldest !== undefined) this.hits.delete(oldest);
    }
    return 0;
  }
}

/** Hash SHA-256 (hex) da chave do limite: o banco nunca vê o id do usuário nem o IP em claro. */
export async function hashRateKey(scope: string, value: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${scope}:${value}`));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

/** Maior espera entre as fontes de limite (memória e banco). */
export function combineWaits(...waits: number[]): number {
  return Math.max(0, ...waits.filter((wait) => Number.isFinite(wait)));
}
