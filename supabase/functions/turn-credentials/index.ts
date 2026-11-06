// Emissor mínimo de credenciais TURN temporárias (modo REST do Coturn).
// Secrets necessários: TURN_HOST, TURN_SECRET (o mesmo do Coturn). Opcionais: TURN_PORT, TURN_TLS_PORT, TURN_TLS_ALT_PORT, TURN_TTL_SECONDS.
// Publicar sem verificação de JWT (o app local não envia): supabase functions deploy turn-credentials --no-verify-jwt

const cors = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, OPTIONS",
  "access-control-allow-headers": "authorization, content-type, accept",
};

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { ...cors, "content-type": "application/json", "cache-control": "no-store" } });
}

async function hmacBase64(secret: string, message: string): Promise<string> {
  const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(secret), { name: "HMAC", hash: "SHA-1" }, false, ["sign"]);
  const signature = new Uint8Array(await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(message)));
  let binary = "";
  for (const byte of signature) binary += String.fromCharCode(byte);
  return btoa(binary);
}

Deno.serve(async (request: Request) => {
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: cors });
  if (request.method !== "GET") return json({ message: "Método não permitido." }, 405);
  const host = Deno.env.get("TURN_HOST")?.trim();
  const secret = Deno.env.get("TURN_SECRET")?.trim();
  if (!host || !secret) return json({ message: "TURN não configurado." }, 503);
  const port = Deno.env.get("TURN_PORT") ?? "3478";
  const tlsPort = Deno.env.get("TURN_TLS_PORT") ?? "5349";
  const altPort = Deno.env.get("TURN_TLS_ALT_PORT") ?? "443";
  const ttl = Math.min(86_400, Math.max(300, Number(Deno.env.get("TURN_TTL_SECONDS") ?? "3600") || 3600));
  const expiresAt = Math.floor(Date.now() / 1000) + ttl;
  const username = `${expiresAt}:${crypto.randomUUID()}`;
  const credential = await hmacBase64(secret, username);
  const urls = [
    `turn:${host}:${port}?transport=udp`,
    `turn:${host}:${port}?transport=tcp`,
    `turns:${host}:${tlsPort}?transport=tcp`,
    `turns:${host}:${altPort}?transport=tcp`,
  ];
  return json({ username, credential, ttl, expiresAt, urls, iceServers: [{ urls: [`stun:${host}:${port}`] }, { urls, username, credential }] });
});
