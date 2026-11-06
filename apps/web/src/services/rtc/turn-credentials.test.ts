import { describe, expect, it, vi } from "vitest";
import {
  parseTemporaryTurnCredentials,
  TemporaryTurnCredentialClient,
  validateTurnCredentialsEndpoint,
} from "./turn-credentials";

const NOW = 1_800_000_000_000;

function response(ttl = 3600, nowMs = NOW): object {
  const expiresAt = Math.floor(nowMs / 1000) + ttl;
  return {
    username: `${expiresAt}:peer-id`,
    credential: "temporary-hmac",
    ttl,
    expiresAt,
    urls: [
      "turn:turn.briks.test:3478?transport=udp",
      "turn:turn.briks.test:3478?transport=tcp",
      "turns:turn.briks.test:5349?transport=tcp",
      "turns:turn.briks.test:443?transport=tcp",
    ],
  };
}

describe("credenciais TURN temporárias", () => {
  it("aceita o contrato compacto e cria um RTCIceServer autenticado", () => {
    const parsed = parseTemporaryTurnCredentials(response(), NOW);
    expect(parsed.ttl).toBe(3600);
    expect(parsed.urls).toHaveLength(4);
    expect(parsed.iceServers[0]).toMatchObject({
      username: "1800003600:peer-id",
      credential: "temporary-hmac",
    });
  });

  it("aceita o contrato expandido usado pelo backend do Briks", () => {
    const compact = response() as { username: string; credential: string; ttl: number; expiresAt: number; urls: string[] };
    const parsed = parseTemporaryTurnCredentials({
      ...compact,
      iceServers: [
        { urls: ["stun:turn.briks.test:3478"] },
        { urls: compact.urls, username: compact.username, credential: compact.credential },
      ],
    }, NOW);
    expect(parsed.iceServers).toHaveLength(2);
    expect(parsed.urls).toContain("turns:turn.briks.test:443?transport=tcp");
  });

  it("rejeita endpoint inseguro, credenciais permanentes e TTL excessivo", () => {
    expect(() => validateTurnCredentialsEndpoint("http://turn.briks.test/credentials")).toThrow(/HTTPS/);
    expect(() => validateTurnCredentialsEndpoint("http://127.0.0.1:8080/credentials", true)).not.toThrow();
    expect(() => parseTemporaryTurnCredentials({ ...response(), ttl: 0 }, NOW)).toThrow(/ttl/);
    expect(() => parseTemporaryTurnCredentials({ ...response(), ttl: 90_000 }, NOW)).toThrow(/ttl/);
    expect(() => parseTemporaryTurnCredentials({ ttl: 3600, urls: ["turn:turn.briks.test:3478"] }, NOW)).toThrow(/username/);
  });

  it("envia bearer somente em memória e reutiliza credenciais ainda válidas", async () => {
    let observedInit: RequestInit | undefined;
    const fetcher = vi.fn(async (_input: RequestInfo | URL, init?: RequestInit) => {
      observedInit = init;
      return new Response(JSON.stringify(response()), {
        status: 200,
        headers: { "content-type": "application/json" },
      });
    }) as typeof fetch;
    const client = new TemporaryTurnCredentialClient("https://auth.briks.test/rtc/credentials", {
      fetcher,
      now: () => NOW,
    });
    const first = await client.get("session-token");
    first.urls.length = 0;
    const second = await client.get("session-token");

    expect(second.urls).toHaveLength(4);
    expect(fetcher).toHaveBeenCalledOnce();
    expect(new Headers(observedInit?.headers).get("authorization")).toBe("Bearer session-token");
    expect(observedInit).toMatchObject({ cache: "no-store", credentials: "omit", referrerPolicy: "no-referrer" });
  });

  it("não envia o token local a um emissor público", async () => {
    let observedInit: RequestInit | undefined;
    const fetcher = vi.fn(async (_input: RequestInfo | URL, init?: RequestInit) => {
      observedInit = init;
      return new Response(JSON.stringify(response()), { status: 200 });
    }) as typeof fetch;
    const client = new TemporaryTurnCredentialClient("https://auth.briks.test/rtc/credentials", { fetcher, now: () => NOW });
    await client.get();
    expect(new Headers(observedInit?.headers).has("authorization")).toBe(false);
  });

  it("renova o cache antes que a credencial expire", async () => {
    let clock = NOW;
    const fetcher = vi.fn(async () => new Response(JSON.stringify(response(3600, clock)), { status: 200 })) as typeof fetch;
    const client = new TemporaryTurnCredentialClient("https://auth.briks.test/rtc/credentials", {
      fetcher,
      now: () => clock,
    });
    await client.get();
    clock += 3_550_000;
    await client.get();
    expect(fetcher).toHaveBeenCalledTimes(2);
  });
});
