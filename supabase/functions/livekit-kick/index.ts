// Edge Function: o dono da chamada tira alguém do servidor de mídia (LiveKit RemoveParticipant).
// A expulsão assinada entre os apps (camada de dados do SFU) faz os clientes honestos ignorarem o expulso; esta função é o
// que realmente derruba a conexão dele no servidor. A regra (quem é o dono, o que é recusado) está em ../_shared/kick.ts.
// Secrets: LIVEKIT_API_KEY, LIVEKIT_API_SECRET, LIVEKIT_URL (os mesmos da livekit-token). Opcional: ALLOWED_ORIGINS.
// Deploy (JWT do gateway ligado, sem --no-verify-jwt): supabase functions deploy livekit-kick
// O dono é quem entrou com a marca de dono no token (../_shared/owner.ts); sem marcado na sala, o mais antigo.
import { authenticate, peerBelongsToUser } from "../_shared/auth.ts";
import type { AuthUser } from "../_shared/auth.ts";
import { persistentRateLimit } from "../_shared/rate-limit.ts";
import { RateLimiter, combineWaits, parseAllowedOrigins } from "../_shared/guard.ts";
import { handleKick } from "../_shared/kick.ts";
import { listParticipants, removeParticipant } from "../_shared/livekit-admin.ts";

const allowedOrigins = parseAllowedOrigins(Deno.env.get("ALLOWED_ORIGINS"));
const perUser = new RateLimiter(20, 10 * 60_000);
const perIp = new RateLimiter(60, 10 * 60_000);

Deno.serve((request) => {
  const url = Deno.env.get("LIVEKIT_URL");
  const key = Deno.env.get("LIVEKIT_API_KEY");
  const secret = Deno.env.get("LIVEKIT_API_SECRET");
  if (!url || !key || !secret) {
    return new Response(JSON.stringify({ message: "LiveKit não configurado." }), { status: 503, headers: { "content-type": "application/json", "cache-control": "no-store" } });
  }
  return handleKick(request, {
    allowedOrigins,
    authenticate,
    rateLimit: async (scope, value) => scope === "ip"
      ? combineWaits(perIp.check(value), await persistentRateLimit("livekit-kick:ip", value, 60, 600))
      : combineWaits(perUser.check(value), await persistentRateLimit("livekit-kick:user", value, 20, 600)),
    peerBelongsToUser: (user, peerId) => peerBelongsToUser(user as AuthUser, peerId),
    listParticipants: (room) => listParticipants(url, key, secret, room),
    removeParticipant: (room, identity) => removeParticipant(url, key, secret, room, identity),
  });
});
