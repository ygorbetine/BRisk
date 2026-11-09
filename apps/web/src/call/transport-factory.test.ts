import { describe, expect, it, vi } from "vitest";
import { createSfuConnectionProvider, SFU_LOGIN_REQUIRED_MESSAGE } from "./transport-factory";

const session = async () => "user-jwt";

describe("provedor de conexão SFU", () => {
  it("envia a sessão do usuário e o peerId, nunca uma identidade livre", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify({ token: "jwt" }), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const info = await createSfuConnectionProvider("https://t.example/token", "wss://sfu", "anon-public", session)({ roomId: "r", localPeerId: "p" });
    expect(info).toEqual({ url: "wss://sfu", token: "jwt" });
    const [, init] = fetchMock.mock.calls[0] as unknown as [string, RequestInit];
    expect(JSON.parse(init.body as string)).toEqual({ room: "r", peerId: "p", identity: "p" });
    const headers = init.headers as Record<string, string>;
    expect(headers.authorization).toBe("Bearer user-jwt");
    expect(headers.apikey).toBe("anon-public");
    vi.unstubAllGlobals();
  });

  it("falha com mensagem clara, sem chamar a rede, quando não há sessão", async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal("fetch", fetchMock);
    await expect(createSfuConnectionProvider("https://t.example/token", "wss://sfu", undefined, async () => null)({ roomId: "r", localPeerId: "p" }))
      .rejects.toThrow(SFU_LOGIN_REQUIRED_MESSAGE);
    expect(fetchMock).not.toHaveBeenCalled();
    vi.unstubAllGlobals();
  });

  it("traduz 401 em pedido de login e repassa a mensagem do servidor nos demais erros", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("{}", { status: 401 })));
    await expect(createSfuConnectionProvider("https://t.example/token", "wss://sfu", undefined, session)({ roomId: "r", localPeerId: "p" })).rejects.toThrow("Entre na sua conta");
    vi.stubGlobal("fetch", vi.fn(async () => new Response(JSON.stringify({ message: "Esta instalação não está registrada na sua conta." }), { status: 403 })));
    await expect(createSfuConnectionProvider("https://t.example/token", "wss://sfu", undefined, session)({ roomId: "r", localPeerId: "p" })).rejects.toThrow("não está registrada");
    vi.unstubAllGlobals();
  });
});
