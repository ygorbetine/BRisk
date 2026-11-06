import { describe, expect, it } from "vitest";
import { parseChatWireEnvelope } from "./MessageProtocol";

describe("chat.members.request", () => {
  const base = { version: 2, type: "chat.members.request", channelId: "channel-0001", requestId: "request-0001" };

  it("aceita um pedido leve de lista de membros", () => {
    expect(parseChatWireEnvelope(JSON.stringify(base), "channel-0001")).toEqual(base);
  });

  it("recusa pedido sem requestId ou de outro canal", () => {
    expect(parseChatWireEnvelope(JSON.stringify({ ...base, requestId: undefined }), "channel-0001")).toBeNull();
    expect(parseChatWireEnvelope(JSON.stringify(base), "outro-canal")).toBeNull();
  });
});
