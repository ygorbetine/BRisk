import { describe, expect, it, vi } from "vitest";
import { createSfuConnectionProvider } from "./transport-factory";

describe("provedor de conexão SFU", () => {
  it("pede o token ao serviço sem expor segredos", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify({ token: "jwt" }), { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);
    const info = await createSfuConnectionProvider("https://t.example/token", "wss://sfu")({ roomId: "r", localPeerId: "p", accessToken: "a" });
    expect(info).toEqual({ url: "wss://sfu", token: "jwt" });
    const [, init] = fetchMock.mock.calls[0] as unknown as [string, RequestInit];
    expect(JSON.parse(init.body as string)).toEqual({ room: "r", identity: "p" });
    vi.unstubAllGlobals();
  });

  it("falha quando o serviço recusa", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => new Response("no", { status: 401 })));
    await expect(createSfuConnectionProvider("https://t.example/token", "wss://sfu")({ roomId: "r", localPeerId: "p" })).rejects.toThrow("401");
    vi.unstubAllGlobals();
  });
});
