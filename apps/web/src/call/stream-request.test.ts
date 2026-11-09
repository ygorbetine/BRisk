import { describe, expect, it } from "vitest";
import { parseStreamRequestMessage, streamRequestMessage, StreamRequestLimiter } from "./stream-request";

describe("pedido de transmissão", () => {
  it("aceita só a mensagem exata", () => {
    expect(parseStreamRequestMessage(JSON.stringify(streamRequestMessage()))).toEqual(streamRequestMessage());
    expect(parseStreamRequestMessage(JSON.stringify({ version: 2, type: "call.stream.request" }))).toBeNull();
    expect(parseStreamRequestMessage(JSON.stringify({ version: 1, type: "call.profile" }))).toBeNull();
    expect(parseStreamRequestMessage("não é json")).toBeNull();
    expect(parseStreamRequestMessage(JSON.stringify({ version: 1, type: "call.stream.request", pad: "x".repeat(300) }))).toBeNull();
  });
  it("limita um pedido por pessoa a cada 30 s", () => {
    const limiter = new StreamRequestLimiter(30_000);
    expect(limiter.take("a", 0)).toBe(true);
    expect(limiter.take("a", 10_000)).toBe(false);
    expect(limiter.remaining("a", 10_000)).toBe(20);
    expect(limiter.take("b", 10_000)).toBe(true);
    expect(limiter.take("a", 30_000)).toBe(true);
    expect(limiter.remaining("c")).toBe(0);
  });
});
