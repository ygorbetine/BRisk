// Regra da expulsão de verdade (tirar do servidor de mídia), separada do `Deno.serve` para poder ser testada com simulações.
// Quem pode: só o DONO da chamada. O servidor não tem tabela de salas nem de membros, então o dono é definido pelo que o
// próprio LiveKit registra:
//   1. quem entrou com a marca de dono no token (emitida pela `livekit-token`, ver ./owner.ts): o dono fixo, que continua
//      sendo o mesmo depois de uma queda ou reinício do servidor de chamadas;
//   2. sem ninguém marcado na sala (o dono saiu de vez, ou tokens antigos): quem está há mais tempo na sala.
// Nos dois casos o desempate é o menor `joinedAt` do servidor e depois a menor identidade: nada disso o cliente forja.
// É a mesma regra que o app usa para mostrar quem é o dono.
import { clientIp, corsHeaders, isValidPeerId, isValidRoom, originAllowed } from "./guard.ts";
import type { Participant } from "./livekit-admin.ts";

export type KickUser = { id: string };

export type KickDeps = {
  allowedOrigins: string[];
  authenticate(request: Request): Promise<KickUser | null>;
  /** segundos a esperar (0 = pode seguir) */
  rateLimit(scope: "ip" | "user", key: string): Promise<number>;
  /** a instalação (peerId) pertence a este usuário? Só é chamada se o corpo trouxer `peerId`. */
  peerBelongsToUser(user: KickUser, peerId: string): Promise<boolean>;
  listParticipants(room: string): Promise<Participant[]>;
  removeParticipant(room: string, identity: string): Promise<void>;
};

const IDENTITY_PATTERN = /^[A-Za-z0-9_-]{8,128}$/;
export function isValidIdentity(value: unknown): value is string { return typeof value === "string" && IDENTITY_PATTERN.test(value); }

/** Dono segundo o servidor: o marcado no token; sem marcado, quem está há mais tempo na sala. `null` = sala vazia. */
export function roomOwner(participants: Participant[]): string | null {
  const marked = participants.filter((p) => p.owner === true);
  let best: Participant | null = null;
  for (const p of marked.length ? marked : participants) {
    if (!best || p.joinedAt < best.joinedAt || (p.joinedAt === best.joinedAt && p.identity < best.identity)) best = p;
  }
  return best ? best.identity : null;
}

export async function handleKick(request: Request, deps: KickDeps): Promise<Response> {
  const origin = request.headers.get("origin");
  const cors = corsHeaders(origin, deps.allowedOrigins, "POST, OPTIONS");
  const json = (body: unknown, status = 200, extra: Record<string, string> = {}) =>
    new Response(JSON.stringify(body), { status, headers: { ...cors, ...extra, "content-type": "application/json", "cache-control": "no-store" } });
  if (!originAllowed(origin, deps.allowedOrigins)) return json({ message: "Origem não permitida." }, 403);
  if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: cors });
  if (request.method !== "POST") return json({ message: "Método não permitido." }, 405);

  const ip = clientIp(request.headers);
  const ipWait = ip === "desconhecido" ? 0 : await deps.rateLimit("ip", ip);
  if (ipWait) return json({ message: "Muitas requisições. Tente de novo em instantes." }, 429, { "retry-after": String(ipWait) });
  const user = await deps.authenticate(request);
  if (!user) return json({ message: "Entre na sua conta Briks para moderar a chamada." }, 401);
  const userWait = await deps.rateLimit("user", user.id);
  if (userWait) return json({ message: "Muitas requisições. Tente de novo em instantes." }, 429, { "retry-after": String(userWait) });

  const raw: unknown = await request.json().catch(() => null);
  const body = (raw && typeof raw === "object" ? raw : {}) as { room?: unknown; identity?: unknown; peerId?: unknown };
  if (!isValidRoom(body.room)) return json({ message: "Nome de sala inválido." }, 400);
  if (!isValidIdentity(body.identity)) return json({ message: "Participante inválido." }, 400);
  // Quem pede é sempre o usuário logado (ou uma instalação registrada dele): nunca uma identidade livre vinda do corpo.
  let requester = user.id;
  if (body.peerId !== undefined) {
    if (!isValidPeerId(body.peerId)) return json({ message: "peerId inválido." }, 400);
    if (!(await deps.peerBelongsToUser(user, body.peerId))) return json({ message: "Esta instalação não está registrada na sua conta." }, 403);
    requester = body.peerId;
  }
  if (body.identity === requester) return json({ message: "Para sair da chamada, use o botão Sair." }, 400);

  let participants: Participant[];
  try { participants = await deps.listParticipants(body.room); }
  catch { return json({ message: "Servidor de chamadas indisponível." }, 502); }
  if (!participants.some((p) => p.identity === requester)) return json({ message: "Você não está nesta chamada." }, 403);
  if (roomOwner(participants) !== requester) return json({ message: "Só o dono da chamada pode expulsar." }, 403);
  if (!participants.some((p) => p.identity === body.identity)) return json({ message: "Essa pessoa não está na chamada." }, 404);
  try { await deps.removeParticipant(body.room, body.identity); }
  catch { return json({ message: "Servidor de chamadas indisponível." }, 502); }
  return json({ removed: body.identity });
}
