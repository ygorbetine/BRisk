// Emissor de credenciais TURN temporárias (modo REST do Coturn). Exige usuário logado no Supabase.
// Secrets necessários: TURN_HOST, TURN_SECRET (o mesmo do Coturn). Opcionais: TURN_PORT, TURN_TLS_PORT, TURN_TLS_ALT_PORT,
// TURN_TTL_SECONDS (máx. 3600), ALLOWED_ORIGINS (origens web permitidas, separadas por vírgula).
// Publicar COM a verificação de JWT do gateway ligada (padrão). NÃO use --no-verify-jwt:
//   supabase functions deploy turn-credentials
// A função também valida a sessão do usuário (role "authenticated") por conta própria.
import { authenticate } from "../_shared/auth.ts";
import { RateLimiter, clampTtl, clientIp, corsHeaders, originAllowed, parseAllowedOrigins } from "../_shared/guard.ts";

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

Deno.serve(async (request: Request) => {
  const origin = request.headers.get("origin");
  const cors = corsHeaders(origin, allowedOrigins, "GET, OPTIONS");
  const json = (body: unknown, status = 200, extra: Record<string, string> = {}) =>
    new Response(JSON.stringify(body), { status, headers: { ...cors, ...extra, "content-type": "application/json", "cache-control": "no-store" } });
  if (!originAllowed(origin, allowedOrigins)) return json({ message: "Origem não permitida." }, 403);
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: cors });
  if (request.method !== "GET") return json({ message: "Método não permitido." }, 405);

  const ipWait = perIp.check(clientIp(request.headers));
  if (ipWait) return json({ message: "Muitas requisições. Tente de novo em instantes." }, 429, { "retry-after": String(ipWait) });
  const user = await authenticate(request);
  if (!user) return json({ message: "Entre na sua conta Briks para usar o relay TURN." }, 401);
  const userWait = perUser.check(user.id);
  if (userWait) return json({ message: "Muitas requisições. Tente de novo em instantes." }, 429, { "retry-after": String(userWait) });

  const host = Deno.env.get("TURN_HOST")?.trim();
  const secret = Deno.env.get("TURN_SECRET")?.trim();
  if (!host || !secret) return json({ message: "TURN não configurado." }, 503);
  const port = Deno.env.get("TURN_PORT") ?? "3478";
  const tlsPort = Deno.env.get("TURN_TLS_PORT") ?? "5349";
  const altPort = Deno.env.get("TURN_TLS_ALT_PORT") ?? "443";
  const ttl = clampTtl(Deno.env.get("TURN_TTL_SECONDS"));
  const expiresAt = Math.floor(Date.now() / 1000) + ttl;
  const username = `${expiresAt}:${user.id}`;
  const credential = await hmacBase64(secret, username);
  const urls = [
    `turn:${host}:${port}?transport=udp`,
    `turn:${host}:${port}?transport=tcp`,
    `turns:${host}:${tlsPort}?transport=tcp`,
    `turns:${host}:${altPort}?transport=tcp`,
  ];
  return json({ username, credential, ttl, expiresAt, urls, iceServers: [{ urls: [`stun:${host}:${port}`] }, { urls, username, credential }] });
});
