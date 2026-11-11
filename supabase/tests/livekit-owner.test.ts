// Regra do dono fixo (supabase/functions/_shared/owner.ts) e da expulsão que a respeita, com simulações: sem Supabase, sem
// servidor de chamadas e sem publicar nada. O segredo aqui é de mentira.
//   node --experimental-transform-types --test supabase/tests/livekit-owner.test.ts
// O teste `owner_rule_of_the_token_function` do briks-core roda este arquivo quando `BRIKS_NODE_BIN` está definido.
import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { test } from "node:test";
import { handleKick, roomOwner } from "../functions/_shared/kick.ts";
import { listParticipants } from "../functions/_shared/livekit-admin.ts";
import { OWNER_METADATA, PREVIOUS_TOKEN_MAX_AGE_SECONDS, grantsOwner, hasOwnerMark, readOwnToken } from "../functions/_shared/owner.ts";

const SECRET = "segredo-de-teste";
const ROOM = "room:0123456789abcdef0123456789abcdef";
const ANA = "11111111-1111-4111-8111-111111111111";
const BETO = "22222222-2222-4222-8222-222222222222";
const DUDA = "33333333-3333-4333-8333-333333333333";
const NOW = 1_800_000_000;

const b64 = (value: unknown) => Buffer.from(JSON.stringify(value)).toString("base64url");
/** Token como o que a `livekit-token` emite (mesmo formato do livekit-server-sdk). */
function token(identity: string, o: { owner?: boolean; room?: string; exp?: number; secret?: string; alg?: string; video?: Record<string, unknown> } = {}): string {
  const head = b64({ alg: o.alg ?? "HS256", typ: "JWT" });
  const body = b64({ iss: "chave", sub: identity, nbf: NOW - 10, exp: o.exp ?? NOW + 3600, ...(o.owner ? { metadata: OWNER_METADATA } : {}),
    video: o.video ?? { room: o.room ?? ROOM, roomJoin: true, canPublish: true, canSubscribe: true, canPublishData: true } });
  return `${head}.${body}.${createHmac("sha256", o.secret ?? SECRET).update(`${head}.${body}`).digest("base64url")}`;
}
const ask = (identity: string, participants: { identity: string; joinedAt: number; owner?: boolean }[] | null, previous?: unknown) =>
  grantsOwner({ room: ROOM, identity, participants, previous, apiSecret: SECRET, nowSeconds: NOW });

test("a marca de dono só é lida de uma metadata bem formada", () => {
  assert.equal(hasOwnerMark(OWNER_METADATA), true);
  for (const bad of ["", "{}", "{\"briks\":{}}", "{\"briks\":{\"owner\":\"true\"}}", "{\"briks\":{\"owner\":1}}", "{\"owner\":true}", "não é json", "null", 7, null, undefined, { briks: { owner: true } }, " ".repeat(3000)]) {
    assert.equal(hasOwnerMark(bad), false, String(bad));
  }
});

test("token anterior: só vale com a assinatura do nosso segredo e no formato de entrada em sala", async () => {
  assert.deepEqual(await readOwnToken(token(ANA, { owner: true }), SECRET), { identity: ANA, room: ROOM, owner: true, exp: NOW + 3600 });
  assert.deepEqual(await readOwnToken(token(BETO), SECRET), { identity: BETO, room: ROOM, owner: false, exp: NOW + 3600 });
  assert.equal(await readOwnToken(token(ANA, { owner: true, secret: "outro-segredo" }), SECRET), null, "assinado com outro segredo");
  const forged = token(BETO).split(".");
  forged[1] = b64({ sub: BETO, exp: NOW + 3600, metadata: OWNER_METADATA, video: { room: ROOM, roomJoin: true } });
  assert.equal(await readOwnToken(forged.join("."), SECRET), null, "corpo trocado sem refazer a assinatura");
  assert.equal(await readOwnToken(token(ANA, { owner: true, alg: "none" }), SECRET), null, "algoritmo diferente");
  assert.equal(await readOwnToken(token("briks-kick", { video: { room: ROOM, roomAdmin: true } }), SECRET), null, "token de administração não é prova de entrada");
  for (const bad of ["", "a.b", "a.b.c", "x".repeat(5000), 12, null, {}]) assert.equal(await readOwnToken(bad, SECRET), null);
});

test("quem começa a chamada (sala vazia) recebe a marca; quem chega depois, não", async () => {
  assert.equal(await ask(ANA, []), true);
  assert.equal(await ask(BETO, [{ identity: ANA, joinedAt: 100, owner: true }]), false);
  assert.equal(await ask(BETO, [{ identity: ANA, joinedAt: 100 }]), false, "sala com gente sem marca: não vira dono só por pedir");
});

test("reentrada depois de uma queda: o dono continua dono e quem não era não vira", async () => {
  // o servidor de chamadas reiniciou: a sala está vazia e Beto volta primeiro
  assert.equal(await ask(BETO, [], token(BETO)), false, "Beto voltou primeiro mas não era o dono");
  assert.equal(await ask(ANA, [{ identity: BETO, joinedAt: 500 }, { identity: DUDA, joinedAt: 501 }], token(ANA, { owner: true })), true, "a dona volta por último e continua dona");
  // token já vencido (a chamada durou mais que 1 h) ainda prova a reentrada
  assert.equal(await ask(ANA, [{ identity: BETO, joinedAt: 500 }], token(ANA, { owner: true, exp: NOW - 3 * 3600 })), true);
  assert.equal(await ask(ANA, [{ identity: BETO, joinedAt: 500 }], token(ANA, { owner: true, exp: NOW - PREVIOUS_TOKEN_MAX_AGE_SECONDS - 60 })), false, "velho demais: conta como entrada nova");
});

test("só existe um dono por vez: com outro marcado na sala, nem o token anterior de dono vale", async () => {
  assert.equal(await ask(ANA, [{ identity: BETO, joinedAt: 500, owner: true }], token(ANA, { owner: true })), false);
});

test("token anterior de outra pessoa, de outra sala ou forjado conta como entrada nova (não dá a marca numa sala ocupada)", async () => {
  const busy = [{ identity: DUDA, joinedAt: 100 }];
  assert.equal(await ask(BETO, busy, token(ANA, { owner: true })), false, "token da dona usado por outro");
  assert.equal(await ask(ANA, busy, token(ANA, { owner: true, room: "room:ffffffffffffffffffffffffffffffff" })), false, "dona de OUTRA sala");
  assert.equal(await ask(ANA, busy, token(ANA, { owner: true, secret: "outro" })), false, "assinatura de outro segredo");
  assert.equal(await ask(ANA, busy, "lixo"), false);
  assert.equal(await ask(ANA, busy, 42), false);
});

test("sem resposta do servidor de chamadas ninguém recebe a marca (vale o mais antigo)", async () => {
  assert.equal(await ask(ANA, null), false);
  assert.equal(await ask(ANA, null, token(ANA, { owner: true })), false);
});

test("a expulsão segue o dono marcado, mesmo que ele não seja o mais antigo na sala", async () => {
  // depois de uma queda a dona (Ana) voltou por último: é a mais nova na sala, mas é a marcada
  const room = [{ identity: BETO, joinedAt: 500 }, { identity: DUDA, joinedAt: 501 }, { identity: ANA, joinedAt: 502, owner: true }];
  assert.equal(roomOwner(room), ANA);
  assert.equal(roomOwner(room.map((p) => ({ identity: p.identity, joinedAt: p.joinedAt }))), BETO, "sem marcado: o mais antigo, como antes");
  assert.equal(roomOwner([{ identity: BETO, joinedAt: 9, owner: true }, { identity: ANA, joinedAt: 9, owner: true }]), ANA, "dois marcados (corrida na criação): desempate determinístico");
  assert.equal(roomOwner([]), null);

  const removed: string[] = [];
  const run = (requester: string, target: string) => handleKick(
    new Request("http://local/livekit-kick", { method: "POST", headers: { authorization: "Bearer x", "content-type": "application/json" }, body: JSON.stringify({ room: ROOM, identity: target }) }),
    { allowedOrigins: [], authenticate: () => Promise.resolve({ id: requester }), rateLimit: () => Promise.resolve(0), peerBelongsToUser: () => Promise.resolve(false),
      listParticipants: () => Promise.resolve(room), removeParticipant: (_r, identity) => { removed.push(identity); return Promise.resolve(); } });
  assert.equal((await run(BETO, ANA)).status, 403, "o mais antigo não expulsa a dona");
  assert.deepEqual(removed, []);
  assert.equal((await run(ANA, BETO)).status, 200, "a dona marcada expulsa");
  assert.deepEqual(removed, [BETO]);
});

test("ListParticipants lê a marca da metadata que o servidor de chamadas devolve", async () => {
  const fetcher = (() => Promise.resolve(new Response(JSON.stringify({ participants: [
    { identity: ANA, joinedAt: "100", metadata: OWNER_METADATA }, { identity: BETO, joinedAt: "101", metadata: "" }, { identity: DUDA, joinedAt: "102" },
  ] }), { status: 200 }))) as typeof fetch;
  assert.deepEqual(await listParticipants("wss://lk.exemplo", "chave", SECRET, ROOM, fetcher), [
    { identity: ANA, joinedAt: 100, owner: true }, { identity: BETO, joinedAt: 101 }, { identity: DUDA, joinedAt: 102 },
  ]);
});
