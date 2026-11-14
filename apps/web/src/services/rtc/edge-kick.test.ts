import { createHmac } from "node:crypto";
import { describe, expect, it, vi } from "vitest";
import { handleKick, isValidIdentity, roomOwner, type KickDeps } from "../../../../../supabase/functions/_shared/kick";
import { adminToken, httpBase, listParticipants, removeParticipant } from "../../../../../supabase/functions/_shared/livekit-admin";

const OWNER = "11111111-1111-4111-8111-111111111111";
const GUEST = "22222222-2222-4222-8222-222222222222";
const OTHER = "33333333-3333-4333-8333-333333333333";
const ROOM = "room:0123456789abcdef0123456789abcdef";

type Overrides = Partial<KickDeps> & { user?: string | null };
function deps(o: Overrides = {}) {
  const d = {
    allowedOrigins: [] as string[],
    authenticate: vi.fn(async () => (o.user === null ? null : { id: o.user ?? OWNER })),
    rateLimit: vi.fn(async () => 0),
    peerBelongsToUser: vi.fn(async () => false),
    listParticipants: vi.fn(async () => [{ identity: GUEST, joinedAt: 200 }, { identity: OWNER, joinedAt: 100 }, { identity: OTHER, joinedAt: 300 }]),
    removeParticipant: vi.fn(async () => {}),
    ...o,
  };
  return d;
}
const post = (body: unknown, headers: Record<string, string> = {}) =>
  new Request("https://x.supabase.co/functions/v1/livekit-kick", { method: "POST", headers: { "content-type": "application/json", authorization: "Bearer sessao", ...headers }, body: JSON.stringify(body) });
const message = async (r: Response) => (await r.json() as { message?: string }).message ?? "";

describe("livekit-kick: quem é o dono", () => {
  it("é quem está há mais tempo na sala, segundo o servidor", () => {
    expect(roomOwner([{ identity: "b", joinedAt: 5 }, { identity: "a", joinedAt: 9 }, { identity: "c", joinedAt: 7 }])).toBe("b");
    expect(roomOwner([{ identity: "b", joinedAt: 5 }, { identity: "a", joinedAt: 5 }])).toBe("a");   // empate: determinístico
    expect(roomOwner([])).toBeNull();
  });
  it("quem entrou com a marca de dono no token vence o mais antigo; sem marcado, vale o mais antigo", () => {
    expect(roomOwner([{ identity: "b", joinedAt: 5 }, { identity: "a", joinedAt: 9, owner: true }, { identity: "c", joinedAt: 7 }])).toBe("a");
    expect(roomOwner([{ identity: "b", joinedAt: 5, owner: true }, { identity: "a", joinedAt: 5, owner: true }])).toBe("a");   // dois marcados: determinístico
    expect(roomOwner([{ identity: "b", joinedAt: 5, owner: false }, { identity: "a", joinedAt: 9 }])).toBe("b");
  });
  it("valida a identidade pedida", () => {
    expect(isValidIdentity(GUEST)).toBe(true);
    for (const bad of ["", "curto", "com espaço aqui", "a".repeat(129), 42, null, undefined, { identity: GUEST }]) expect(isValidIdentity(bad)).toBe(false);
  });
});

describe("livekit-kick: regra da função", () => {
  it("o dono expulsa um participante: chama RemoveParticipant uma vez, com a sala e o alvo certos", async () => {
    const d = deps();
    const r = await handleKick(post({ room: ROOM, identity: GUEST }), d);
    expect(r.status).toBe(200);
    expect(await r.json()).toEqual({ removed: GUEST });
    expect(d.listParticipants).toHaveBeenCalledWith(ROOM);
    expect(d.removeParticipant).toHaveBeenCalledTimes(1);
    expect(d.removeParticipant).toHaveBeenCalledWith(ROOM, GUEST);
    expect(r.headers.get("cache-control")).toBe("no-store");
  });

  it("qualquer participante da chamada pode expulsar outro (regra atual do Briks, desde a 1.5.16)", async () => {
    const d = deps({ user: GUEST });
    const r = await handleKick(post({ room: ROOM, identity: OWNER }), d);
    expect(r.status).toBe(200);
    expect(d.removeParticipant).toHaveBeenCalledWith(ROOM, OWNER);
  });

  it("quem nem está na chamada é recusado (mesmo sabendo o nome da sala)", async () => {
    const d = deps({ user: "44444444-4444-4444-8444-444444444444" });
    const r = await handleKick(post({ room: ROOM, identity: GUEST }), d);
    expect(r.status).toBe(403);
    expect(await message(r)).toContain("não está nesta chamada");
    expect(d.removeParticipant).not.toHaveBeenCalled();
  });

  it("sem sessão: 401, sem consultar nem tocar o LiveKit", async () => {
    const d = deps({ user: null });
    const r = await handleKick(post({ room: ROOM, identity: GUEST }), d);
    expect(r.status).toBe(401);
    expect(d.listParticipants).not.toHaveBeenCalled();
    expect(d.removeParticipant).not.toHaveBeenCalled();
  });

  it("a identidade de quem pede vem da sessão, nunca do corpo", async () => {
    const d = deps({ user: "44444444-4444-4444-8444-444444444444" });
    // quem nem está na chamada tenta se passar por participante mandando campos a mais: vale a sessão, não o corpo
    const r = await handleKick(post({ room: ROOM, identity: OTHER, requester: OWNER, user: OWNER, owner: GUEST }), d);
    expect(r.status).toBe(403);
    expect(d.removeParticipant).not.toHaveBeenCalled();
  });

  it("peerId só vale se a instalação for do usuário", async () => {
    const peer = "55555555-5555-4555-8555-555555555555";
    const parts = async () => [{ identity: peer, joinedAt: 1 }, { identity: GUEST, joinedAt: 2 }];
    const no = deps({ listParticipants: vi.fn(parts) });
    expect((await handleKick(post({ room: ROOM, identity: GUEST, peerId: peer }), no)).status).toBe(403);
    expect(no.removeParticipant).not.toHaveBeenCalled();
    const yes = deps({ listParticipants: vi.fn(parts), peerBelongsToUser: vi.fn(async () => true) });
    expect((await handleKick(post({ room: ROOM, identity: GUEST, peerId: peer }), yes)).status).toBe(200);
    expect(yes.removeParticipant).toHaveBeenCalledWith(ROOM, GUEST);
    expect((await handleKick(post({ room: ROOM, identity: GUEST, peerId: "nao-e-uuid" }), deps())).status).toBe(400);
  });

  it("entradas inválidas: 400 antes de qualquer chamada ao LiveKit", async () => {
    for (const body of [{}, { room: ROOM }, { identity: GUEST }, { room: "sala com espaço", identity: GUEST }, { room: ROOM, identity: "x" }, { room: ROOM, identity: OWNER }, "texto", null]) {
      const d = deps();
      const r = await handleKick(post(body), d);
      expect(r.status, JSON.stringify(body)).toBe(400);
      expect(d.listParticipants).not.toHaveBeenCalled();
      expect(d.removeParticipant).not.toHaveBeenCalled();
    }
  });

  it("alvo que não está na sala: 404; sala vazia: 403", async () => {
    const d = deps();
    expect((await handleKick(post({ room: ROOM, identity: "66666666-6666-4666-8666-666666666666" }), d)).status).toBe(404);
    expect(d.removeParticipant).not.toHaveBeenCalled();
    expect((await handleKick(post({ room: ROOM, identity: GUEST }), deps({ listParticipants: vi.fn(async () => []) }))).status).toBe(403);
  });

  it("LiveKit fora do ar: 502 sem vazar o erro interno", async () => {
    const boom = () => Promise.reject(new Error("segredo-interno http://10.0.0.1"));
    for (const d of [deps({ listParticipants: vi.fn(boom) }), deps({ removeParticipant: vi.fn(boom) })]) {
      const r = await handleKick(post({ room: ROOM, identity: GUEST }), d);
      expect(r.status).toBe(502);
      expect(await r.text()).not.toContain("segredo-interno");
    }
  });

  it("limite de requisições, método e origem", async () => {
    const limited = deps({ rateLimit: vi.fn(async (scope: string) => (scope === "user" ? 42 : 0)) });
    const r = await handleKick(post({ room: ROOM, identity: GUEST }), limited);
    expect(r.status).toBe(429);
    expect(r.headers.get("retry-after")).toBe("42");
    expect(limited.listParticipants).not.toHaveBeenCalled();
    const byIp = deps({ rateLimit: vi.fn(async (scope: string) => (scope === "ip" ? 7 : 0)) });
    const r2 = await handleKick(post({ room: ROOM, identity: GUEST }, { "x-forwarded-for": "203.0.113.9" }), byIp);
    expect(r2.status).toBe(429);
    expect(byIp.authenticate).not.toHaveBeenCalled();
    expect((await handleKick(new Request("https://x/f", { method: "GET" }), deps())).status).toBe(405);
    expect((await handleKick(new Request("https://x/f", { method: "OPTIONS" }), deps())).status).toBe(204);
    const evil = await handleKick(post({ room: ROOM, identity: GUEST }, { origin: "https://evil.example" }), deps());
    expect(evil.status).toBe(403);
    const d = deps({ allowedOrigins: ["https://app.briks.example"] });
    const ok = await handleKick(post({ room: ROOM, identity: GUEST }, { origin: "https://app.briks.example" }), d);
    expect(ok.status).toBe(200);
    expect(ok.headers.get("access-control-allow-origin")).toBe("https://app.briks.example");
  });
});

describe("livekit-kick: cliente de administração do LiveKit", () => {
  it("token de administração: HS256 assinado com o segredo, restrito à sala e de vida curta", async () => {
    const token = await adminToken("chave-teste", "segredo-teste", ROOM, 60, 1_700_000_000);
    const [h, p, s] = token.split(".");
    expect(createHmac("sha256", "segredo-teste").update(`${h}.${p}`).digest("base64url")).toBe(s);
    const payload = JSON.parse(Buffer.from(p, "base64url").toString());
    expect(payload).toMatchObject({ iss: "chave-teste", exp: 1_700_000_060, video: { room: ROOM, roomAdmin: true } });
    expect(payload.video.roomJoin).toBeUndefined();
    expect(token).not.toContain("segredo-teste");
  });

  it("monta as chamadas Twirp e lê o instante de entrada do servidor", async () => {
    expect(httpBase("wss://briks.livekit.cloud/")).toBe("https://briks.livekit.cloud");
    expect(httpBase("ws://127.0.0.1:7880")).toBe("http://127.0.0.1:7880");
    const calls: { url: string; init: RequestInit }[] = [];
    const fetcher = (async (url: string, init: RequestInit) => {
      calls.push({ url, init });
      return new Response(JSON.stringify({ participants: [{ identity: OWNER, joinedAt: "1700000000", joinedAtMs: "1700000000250" }, { identity: GUEST, joinedAt: "1700000005" }, { sid: "sem identidade" }] }), { status: 200 });
    }) as unknown as typeof fetch;
    const list = await listParticipants("wss://lk.example", "k", "s", ROOM, fetcher);
    expect(list).toEqual([{ identity: OWNER, joinedAt: 1700000000.25 }, { identity: GUEST, joinedAt: 1700000005 }]);
    expect(calls[0].url).toBe("https://lk.example/twirp/livekit.RoomService/ListParticipants");
    expect(JSON.parse(calls[0].init.body as string)).toEqual({ room: ROOM });
    expect((calls[0].init.headers as Record<string, string>).authorization).toMatch(/^Bearer [\w-]+\.[\w-]+\.[\w-]+$/);
    await removeParticipant("wss://lk.example", "k", "s", ROOM, GUEST, fetcher);
    expect(calls[1].url).toBe("https://lk.example/twirp/livekit.RoomService/RemoveParticipant");
    expect(JSON.parse(calls[1].init.body as string)).toEqual({ room: ROOM, identity: GUEST });
  });

  it("sala inexistente é sala vazia; outros erros sobem", async () => {
    const status = (code: number) => (async () => new Response("{}", { status: code })) as unknown as typeof fetch;
    expect(await listParticipants("wss://lk.example", "k", "s", ROOM, status(404))).toEqual([]);
    await expect(listParticipants("wss://lk.example", "k", "s", ROOM, status(401))).rejects.toThrow("401");
    await expect(removeParticipant("wss://lk.example", "k", "s", ROOM, GUEST, status(500))).rejects.toThrow("500");
  });
});
