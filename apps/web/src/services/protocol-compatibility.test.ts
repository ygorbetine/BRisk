import { describe, expect, it } from "vitest";
import {
  compatibleAppVersion,
  compatibleCallPeer,
  compatibleChatPeer,
  LOCAL_BRIKS_CAPABILITIES,
  negotiateCapabilities,
  peerSupportsCapability,
  validBriksPeerCapabilities,
} from "./protocol-compatibility";

describe("negociação de versão P2P", () => {
  it("aceita a versão e os protocolos locais", () => {
    expect(compatibleAppVersion(LOCAL_BRIKS_CAPABILITIES.appVersion)).toBe(true);
    expect(compatibleCallPeer(LOCAL_BRIKS_CAPABILITIES)).toBe(true);
    expect(compatibleChatPeer(LOCAL_BRIKS_CAPABILITIES)).toBe(true);
  });

  it("recusa versões ou protocolos incompatíveis antes da mídia", () => {
    expect(compatibleAppVersion("99.0.0")).toBe(false);
    expect(compatibleCallPeer({ ...LOCAL_BRIKS_CAPABILITIES, callProtocolVersion: 1 })).toBe(false);
    expect(compatibleChatPeer({ ...LOCAL_BRIKS_CAPABILITIES, chatProtocolVersion: 1 })).toBe(false);
    expect(compatibleChatPeer({ ...LOCAL_BRIKS_CAPABILITIES, groupManifestVersion: 1 })).toBe(false);
  });

  it("permite versões menores diferentes quando os protocolos são compatíveis", () => {
    const remote = { ...LOCAL_BRIKS_CAPABILITIES, appVersion: "0.4.0", client: "0.4.0" };
    expect(compatibleAppVersion(remote.appVersion)).toBe(true);
    expect(compatibleCallPeer(remote)).toBe(true);
    expect(compatibleChatPeer(remote)).toBe(true);
  });

  it("negocia recursos granulares sem presumir suporte", () => {
    const remote = { ...LOCAL_BRIKS_CAPABILITIES, capabilities: ["chat-events-v1", "unknown-future-feature"] };
    expect(peerSupportsCapability(remote, "chat-events-v1")).toBe(true);
    expect(peerSupportsCapability(remote, "screen-audio")).toBe(false);
    expect(negotiateCapabilities(remote)).toEqual(["chat-events-v1"]);
  });

  it("valida envelopes sem aceitar campos ausentes", () => {
    expect(validBriksPeerCapabilities(LOCAL_BRIKS_CAPABILITIES)).toBe(true);
    expect(validBriksPeerCapabilities({ appVersion: "0.2.0" })).toBe(false);
    expect(validBriksPeerCapabilities(null)).toBe(false);
  });
});
