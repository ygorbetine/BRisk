// Dono fixo da chamada, sem tabela de salas e sem segredo novo: a marca de dono viaja no próprio token do LiveKit
// (`metadata` do participante), que só a função `livekit-token` assina. O cliente não consegue forjar nem mudar essa marca
// (o token não dá `canUpdateOwnMetadata`), e o servidor de chamadas a devolve em ListParticipants e nos avisos de participante.
//
// Quem recebe a marca ao pedir um token:
//   1. ninguém, se outra pessoa que está na sala agora já a tem (só existe um dono por vez);
//   2. quem REENTRA (manda o token anterior da mesma sala e da mesma identidade, com assinatura válida) mantém o que tinha:
//      o dono continua dono depois de uma queda ou reinício do servidor de chamadas, e quem não era dono não vira dono
//      por ter voltado primeiro;
//   3. quem entra numa sala vazia (começou a chamada).
// Sem ninguém marcado na sala (o dono saiu de vez, ou todos entraram com tokens antigos), vale a regra anterior: o mais antigo.
// LIMITE: um cliente modificado pode omitir o token anterior e pedir entrada "nova"; só vira dono se a sala estiver vazia
// naquele instante, que é o mesmo que começar uma chamada.
import type { Participant } from "./livekit-admin.ts";

export const OWNER_METADATA = JSON.stringify({ briks: { owner: true } });
/** Token anterior aceito como prova de reentrada até este tempo depois de vencido (a chamada pode durar mais que o token). */
export const PREVIOUS_TOKEN_MAX_AGE_SECONDS = 24 * 60 * 60;

/** A `metadata` de um participante (texto JSON vindo do token) traz a marca de dono? */
export function hasOwnerMark(metadata: unknown): boolean {
  if (typeof metadata !== "string" || !metadata || metadata.length > 2048) return false;
  try {
    const value = JSON.parse(metadata) as { briks?: { owner?: unknown } } | null;
    return !!value && typeof value === "object" && !!value.briks && value.briks.owner === true;
  } catch { return false; }
}

const fromB64url = (text: string): Uint8Array => {
  const padded = text.replace(/-/g, "+").replace(/_/g, "/") + "=".repeat((4 - (text.length % 4)) % 4);
  const raw = atob(padded);
  const bytes = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
  return bytes;
};

export type PreviousToken = { identity: string; room: string; owner: boolean; exp: number };

/** Lê um token do LiveKit emitido por nós. `null` se a assinatura (HS256 com o segredo do LiveKit) não confere ou o formato é outro. */
export async function readOwnToken(token: unknown, apiSecret: string): Promise<PreviousToken | null> {
  if (typeof token !== "string" || token.length < 20 || token.length > 4096) return null;
  const parts = token.split(".");
  if (parts.length !== 3) return null;
  try {
    const header = JSON.parse(new TextDecoder().decode(fromB64url(parts[0]))) as { alg?: unknown };
    if (header.alg !== "HS256") return null;
    const key = await crypto.subtle.importKey("raw", new TextEncoder().encode(apiSecret), { name: "HMAC", hash: "SHA-256" }, false, ["verify"]);
    const ok = await crypto.subtle.verify("HMAC", key, fromB64url(parts[2]), new TextEncoder().encode(`${parts[0]}.${parts[1]}`));
    if (!ok) return null;
    const payload = JSON.parse(new TextDecoder().decode(fromB64url(parts[1]))) as { sub?: unknown; exp?: unknown; metadata?: unknown; video?: { room?: unknown; roomJoin?: unknown } };
    if (typeof payload.sub !== "string" || !payload.sub || typeof payload.exp !== "number") return null;
    if (!payload.video || typeof payload.video.room !== "string" || payload.video.roomJoin !== true) return null;   // token de administração não vale
    return { identity: payload.sub, room: payload.video.room, owner: hasOwnerMark(payload.metadata), exp: payload.exp };
  } catch { return null; }
}

export type OwnerQuestion = {
  room: string;
  identity: string;
  /** quem está na sala agora, segundo o servidor de chamadas; `null` = não deu para perguntar (ninguém recebe a marca) */
  participants: Participant[] | null;
  /** token anterior mandado pelo cliente ao reentrar (opcional) */
  previous?: unknown;
  apiSecret: string;
  nowSeconds?: number;
};

/** O token que vai ser emitido para `identity` leva a marca de dono? */
export async function grantsOwner(q: OwnerQuestion): Promise<boolean> {
  if (!q.participants) return false;
  const others = q.participants.filter((p) => p.identity !== q.identity);
  if (others.some((p) => p.owner === true)) return false;
  const now = q.nowSeconds ?? Math.floor(Date.now() / 1000);
  const previous = q.previous === undefined ? null : await readOwnToken(q.previous, q.apiSecret);
  if (previous && previous.room === q.room && previous.identity === q.identity && now - previous.exp <= PREVIOUS_TOKEN_MAX_AGE_SECONDS) return previous.owner;
  return others.length === 0;
}
