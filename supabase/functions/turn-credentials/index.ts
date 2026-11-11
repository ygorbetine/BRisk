// Emissor de credenciais TURN temporárias. Exige usuário logado no Supabase.
// Dois provedores, escolhidos pelos secrets (o primeiro configurado vence):
//   1) Cloudflare Realtime TURN: CF_TURN_KEY_ID e CF_TURN_API_TOKEN (sem servidor próprio, franquia grátis grande).
//   2) Coturn próprio (modo REST): TURN_HOST e TURN_SECRET (o mesmo do Coturn). Opcionais: TURN_PORT, TURN_TLS_PORT, TURN_TLS_ALT_PORT.
// Opcionais comuns: TURN_TTL_SECONDS (máx. 3600), ALLOWED_ORIGINS (origens web permitidas, separadas por vírgula).
// Publicar COM a verificação de JWT do gateway ligada (padrão). NÃO use --no-verify-jwt:
//   supabase functions deploy turn-credentials
// A função também valida a sessão do usuário (role "authenticated") por conta própria.
import { authenticate } from "../_shared/auth.ts";
import { persistentRateLimit } from "../_shared/rate-limit.ts";
import { RateLimiter, clampTtl, combineWaits, clientIp, corsHeaders, originAllowed, parseAllowedOrigins } from "../_shared/guard.ts";

const allowedOrigins = parseAllowedOrigins(Deno.env.get("ALLOWED_ORIGINS"));
const perUser = new RateLimiter(20, 10 * 60_000);
const perIp = new RateLimiter(60, 10 * 60_000);

async function hmacBase64(secret: string, message: string): Promise<string> {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(secret), { name: "HMAC", hash: "SHA-1" }, false, ["sign"]);
  const signature = new Uint8Array(await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(message)));
  let binary = "";
  for (const byte of signature) binary += String.fromCharCode(byte);
  return btoa(binary);
}

type Issued = { username: string; credential: string; urls: string[]; stun: string[] };

/** Cloudflare Realtime TURN: gera um par usuário/senha temporário pela API (o token da API nunca sai daqui). */
async function issueCloudflare(keyId: string, apiToken: string, ttl: number): Promise<Issued | null> {
  try {
    const response = await fetch(`https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(keyId)}/credentials/generate`, {
      method: "POST",
      headers: { authorization: `Bearer ${apiToken}`, "content-type": "application/json" },
      body: JSON.stringify({ ttl }),
      signal: AbortSignal.timeout(5_000),
    });
    if (!response.ok) return null;
    const data = await response.json() as { iceServers?: { urls?: unknown; username?: unknown; credential?: unknown } | { urls?: unknown; username?: unknown; credential?: unknown }[] };
    const entry = Array.isArray(data.iceServers) ? data.iceServers.find((item) => typeof item.username === "string") : data.iceServers;
    if (!entry || typeof entry.username !== "string" || typeof entry.credential !== "string") return null;
    const all = (Array.isArray(entry.urls) ? entry.urls : [entry.urls]).filter((url): url is string => typeof url === "string");
    return {
      username: entry.username,
      credential: entry.credential,
      urls: all.filter((url) => /^turns?:/.test(url)),
      stun: all.filter((url) => /^stun:/.test(url)),
    };
  } catch { return null; }
}

Deno.serve(async (request: Request) => {
  const origin = request.headers.get("origin");
  const cors = corsHeaders(origin, allowedOrigins, "GET, OPTIONS");
  const json = (body: unknown, status = 200, extra: Record<string, string> = {}) =>
    new Response(JSON.stringify(body), { status, headers: { ...cors, ...extra, "content-type": "application/json", "cache-control": "no-store" } });
  if (!originAllowed(origin, allowedOrigins)) return json({ message: "Origem não permitida." }, 403);
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: cors });
  if (request.method !== "GET") return json({ message: "Método não permitido." }, 405);

  // Limite em duas camadas: memória (rápida, por instância) e banco (vale entre instâncias).
  const ip = clientIp(request.headers);
  const ipWait = combineWaits(perIp.check(ip), (ip === "desconhecido" ? 0 : await persistentRateLimit("turn:ip", ip, 60, 600)));
  if (ipWait) return json({ message: "Muitas requisições. Tente de novo em instantes." }, 429, { "retry-after": String(ipWait) });
  const user = await authenticate(request);
  if (!user) return json({ message: "Entre na sua conta Briks para usar o relay TURN." }, 401);
  const userWait = combineWaits(perUser.check(user.id), await persistentRateLimit("turn:user", user.id, 20, 600));
  if (userWait) return json({ message: "Muitas requisições. Tente de novo em instantes." }, 429, { "retry-after": String(userWait) });

  const ttl = clampTtl(Deno.env.get("TURN_TTL_SECONDS"));
  const expiresAt = Math.floor(Date.now() / 1000) + ttl;
  let issued: Issued | null = null;

  const cfKey = Deno.env.get("CF_TURN_KEY_ID")?.trim();
  const cfToken = Deno.env.get("CF_TURN_API_TOKEN")?.trim();
  if (cfKey && cfToken) {
    issued = await issueCloudflare(cfKey, cfToken, ttl);
    if (!issued) return json({ message: "Relay TURN indisponível." }, 502);
  } else {
    const host = Deno.env.get("TURN_HOST")?.trim();
    const secret = Deno.env.get("TURN_SECRET")?.trim();
    if (!host || !secret) return json({ message: "TURN não configurado." }, 503);
    const port = Deno.env.get("TURN_PORT") ?? "3478";
    const tlsPort = Deno.env.get("TURN_TLS_PORT") ?? "5349";
    const altPort = Deno.env.get("TURN_TLS_ALT_PORT") ?? "443";
    const username = `${expiresAt}:${user.id}`;
    issued = {
      username,
      credential: await hmacBase64(secret, username),
      urls: [
        `turn:${host}:${port}?transport=udp`,
        `turn:${host}:${port}?transport=tcp`,
        `turns:${host}:${tlsPort}?transport=tcp`,
        `turns:${host}:${altPort}?transport=tcp`,
      ],
      stun: [`stun:${host}:${port}`],
    };
  }

  const iceServers = [
    ...(issued.stun.length ? [{ urls: issued.stun }] : []),
    { urls: issued.urls, username: issued.username, credential: issued.credential },
  ];
  return json({ username: issued.username, credential: issued.credential, ttl, expiresAt, urls: issued.urls, iceServers });
});
