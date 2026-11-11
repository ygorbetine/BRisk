// Cliente mínimo da API de administração do LiveKit (RoomService via Twirp/HTTP), sem dependências: só `fetch` e Web Crypto,
// que existem no Deno (Edge Functions) e no Node 20+ (testes). Usa a chave e o segredo do LiveKit, que ficam SÓ no servidor.
// Cada chamada leva um token de administração de vida curta, restrito à sala em questão.

import { hasOwnerMark } from "./owner.ts";

/** `owner` = o token dele traz a marca de dono (ver ./owner.ts); ausente em quem entrou com token sem a marca. */
export type Participant = { identity: string; joinedAt: number; owner?: boolean };

const b64url = (bytes: Uint8Array): string => {
  let text = "";
  for (const byte of bytes) text += String.fromCharCode(byte);
  return btoa(text).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
};
const utf8 = (text: string): Uint8Array => new TextEncoder().encode(text);

/** JWT HS256 de administração da sala (`roomAdmin`), válido por `ttlSeconds` (padrão 60 s). */
export async function adminToken(apiKey: string, apiSecret: string, room: string, ttlSeconds = 60, nowSeconds = Math.floor(Date.now() / 1000)): Promise<string> {
  const header = b64url(utf8(JSON.stringify({ alg: "HS256", typ: "JWT" })));
  const payload = b64url(utf8(JSON.stringify({ iss: apiKey, sub: "briks-kick", nbf: nowSeconds - 10, exp: nowSeconds + ttlSeconds, video: { room, roomAdmin: true } })));
  const key = await crypto.subtle.importKey("raw", utf8(apiSecret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const signature = new Uint8Array(await crypto.subtle.sign("HMAC", key, utf8(`${header}.${payload}`)));
  return `${header}.${payload}.${b64url(signature)}`;
}

/** `wss://host` (o que o cliente usa) -> `https://host` (API HTTP); `ws://` só aparece em desenvolvimento local. */
export function httpBase(livekitUrl: string): string {
  const trimmed = livekitUrl.trim().replace(/\/+$/, "");
  if (trimmed.startsWith("wss://")) return `https://${trimmed.slice(6)}`;
  if (trimmed.startsWith("ws://")) return `http://${trimmed.slice(5)}`;
  return trimmed;
}

export class LiveKitAdminError extends Error {
  status: number;
  constructor(status: number, message: string) { super(message); this.status = status; }
}

async function twirp(base: string, token: string, method: string, body: unknown, fetcher: typeof fetch): Promise<unknown> {
  const response = await fetcher(`${base}/twirp/livekit.RoomService/${method}`, {
    method: "POST",
    headers: { authorization: `Bearer ${token}`, "content-type": "application/json" },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(5_000),
  });
  if (!response.ok) throw new LiveKitAdminError(response.status, `LiveKit ${method}: HTTP ${response.status}`);
  return await response.json().catch(() => ({}));
}

/** Quem está na sala agora, com o instante de entrada REGISTRADO PELO SERVIDOR (segundos; o Twirp manda int64 como texto). */
export async function listParticipants(livekitUrl: string, apiKey: string, apiSecret: string, room: string, fetcher: typeof fetch = fetch): Promise<Participant[]> {
  const token = await adminToken(apiKey, apiSecret, room);
  let data: { participants?: unknown };
  try { data = await twirp(httpBase(livekitUrl), token, "ListParticipants", { room }, fetcher) as { participants?: unknown }; }
  catch (error) { if (error instanceof LiveKitAdminError && error.status === 404) return []; throw error; }   // sala que não existe = vazia
  const list = Array.isArray(data.participants) ? data.participants as Record<string, unknown>[] : [];
  return list.filter((p) => typeof p.identity === "string" && p.identity).map((p) => {
    const ms = Number(p.joinedAtMs ?? p.joined_at_ms);
    const s = Number(p.joinedAt ?? p.joined_at);
    const joinedAt = Number.isFinite(ms) && ms > 0 ? ms / 1000 : Number.isFinite(s) ? s : 0;
    return hasOwnerMark(p.metadata) ? { identity: p.identity as string, joinedAt, owner: true } : { identity: p.identity as string, joinedAt };
  });
}

/** Tira o participante da sala no servidor de mídia (o cliente dele recebe `Leave` com o motivo PARTICIPANT_REMOVED). */
export async function removeParticipant(livekitUrl: string, apiKey: string, apiSecret: string, room: string, identity: string, fetcher: typeof fetch = fetch): Promise<void> {
  const token = await adminToken(apiKey, apiSecret, room);
  await twirp(httpBase(livekitUrl), token, "RemoveParticipant", { room, identity }, fetcher);
}
