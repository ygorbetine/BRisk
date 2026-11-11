// Edge Function: emite tokens do LiveKit para usuários logados. O segredo fica só no servidor.
// O token também diz quem é o DONO da chamada (marca na `metadata`, ver ../_shared/owner.ts); o corpo aceita `previous`
// (o token anterior, ao reentrar depois de uma queda) para o dono continuar o mesmo.
// Secrets: LIVEKIT_API_KEY, LIVEKIT_API_SECRET, LIVEKIT_URL. Opcionais: ALLOWED_ORIGINS, LIVEKIT_TOKEN_TTL_SECONDS (máx. 3600).
// Deploy (JWT do gateway ligado, sem --no-verify-jwt): supabase functions deploy livekit-token
//
// LIMITAÇÃO CONHECIDA: as salas são derivadas de um segredo do grupo que só existe nos dispositivos dos membros
// (groupRendezvousId); o servidor não tem a lista de membros, então NÃO consegue provar que o usuário pertence à sala.
// Melhor checagem possível sem tabela nova: sessão válida, identity = peerId registrado e não revogado do próprio usuário
// (profile_keys), formato de sala validado, TTL de 1 h e limite de requisições. Quem não sabe o nome da sala (segredo
// de ~122 bits) não entra nela. Uma tabela de membros por sala fecharia essa lacuna no futuro.
import { AccessToken } from "npm:livekit-server-sdk@2";
import { authenticate, peerBelongsToUser } from "../_shared/auth.ts";
import { persistentRateLimit } from "../_shared/rate-limit.ts";
import { listParticipants } from "../_shared/livekit-admin.ts";
import { OWNER_METADATA, grantsOwner } from "../_shared/owner.ts";
import { RateLimiter, clampTtl, combineWaits, clientIp, corsHeaders, isValidPeerId, isValidRoom, originAllowed, parseAllowedOrigins } from "../_shared/guard.ts";

const allowedOrigins = parseAllowedOrigins(Deno.env.get("ALLOWED_ORIGINS"));
const perUser = new RateLimiter(30, 10 * 60_000);
const perIp = new RateLimiter(90, 10 * 60_000);

Deno.serve(async (request) => {
  const origin = request.headers.get("origin");
  const cors = corsHeaders(origin, allowedOrigins, "POST, OPTIONS");
  const json = (body: unknown, status = 200, extra: Record<string, string> = {}) =>
    new Response(JSON.stringify(body), { status, headers: { ...cors, ...extra, "content-type": "application/json", "cache-control": "no-store" } });
  if (!originAllowed(origin, allowedOrigins)) return json({ message: "Origem não permitida." }, 403);
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: cors });
  if (request.method !== "POST") return json({ message: "Método não permitido." }, 405);

  // Limite em duas camadas: memória (rápida, por instância) e banco (vale entre instâncias).
  const ip = clientIp(request.headers);
  const ipWait = combineWaits(perIp.check(ip), (ip === "desconhecido" ? 0 : await persistentRateLimit("livekit:ip", ip, 90, 600)));
  if (ipWait) return json({ message: "Muitas requisições. Tente de novo em instantes." }, 429, { "retry-after": String(ipWait) });
  const user = await authenticate(request);
  if (!user) return json({ message: "Entre na sua conta Briks para usar o servidor de chamadas." }, 401);
  const userWait = combineWaits(perUser.check(user.id), await persistentRateLimit("livekit:user", user.id, 30, 600));
  if (userWait) return json({ message: "Muitas requisições. Tente de novo em instantes." }, 429, { "retry-after": String(userWait) });

  const body = await request.json().catch(() => ({})) as { room?: unknown; peerId?: unknown; previous?: unknown };
  if (!isValidRoom(body.room)) return json({ message: "Nome de sala inválido." }, 400);
  // A identidade nunca vem do corpo livremente: é o usuário, ou um peerId (UUID) registrado para ele.
  let identity = user.id;
  if (body.peerId !== undefined) {
    if (!isValidPeerId(body.peerId)) return json({ message: "peerId inválido." }, 400);
    if (!(await peerBelongsToUser(user, body.peerId))) return json({ message: "Esta instalação não está registrada na sua conta." }, 403);
    identity = body.peerId;
  }

  const key = Deno.env.get("LIVEKIT_API_KEY");
  const secret = Deno.env.get("LIVEKIT_API_SECRET");
  if (!key || !secret) return json({ message: "LiveKit não configurado." }, 503);
  const ttl = clampTtl(Deno.env.get("LIVEKIT_TOKEN_TTL_SECONDS"));
  // Dono fixo: a marca vai no próprio token (regra em ../_shared/owner.ts). Se não der para perguntar ao servidor de chamadas
  // quem está na sala, o token sai sem a marca (vale "o mais antigo") em vez de a entrada falhar.
  const livekitUrl = Deno.env.get("LIVEKIT_URL");
  const participants = livekitUrl ? await listParticipants(livekitUrl, key, secret, body.room).catch(() => null) : null;
  const owner = await grantsOwner({ room: body.room, identity, participants, previous: body.previous, apiSecret: secret });
  const token = new AccessToken(key, secret, owner ? { identity, ttl, metadata: OWNER_METADATA } : { identity, ttl });
  token.addGrant({ room: body.room, roomJoin: true, canPublish: true, canSubscribe: true, canPublishData: true });
  return json({ token: await token.toJwt(), url: livekitUrl, ttl, owner });
});
