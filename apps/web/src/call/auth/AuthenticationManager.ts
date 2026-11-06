import { withinP2PClockTolerance } from "../../services/p2p-clock";
import {
  compatibleCallPeer,
  LOCAL_BRIKS_CAPABILITIES,
  validBriksPeerCapabilities,
  type BriksPeerCapabilities,
} from "../../services/protocol-compatibility";
import { validAvatarDataUrl } from "../../services/offline/profile";
import { publicIdentity, type LocalIdentity, type PublicPeerIdentity } from "../../services/offline/social-storage";

export type CallAuthMessage =
  | { version: 1; type: "call.auth.challenge"; identityPeerId: string; nonce: string; timestamp: number; capabilities: BriksPeerCapabilities }
  | { version: 1; type: "call.auth.proof"; identity: PublicPeerIdentity; nonce: string; timestamp: number; capabilities: BriksPeerCapabilities; signature: string };

export type ParsedCallAuthMessage =
  | { status: "not-auth" }
  | { status: "incompatible"; remoteVersion?: string }
  | { status: "expired" }
  | { status: "valid"; message: Partial<CallAuthMessage> };

export type CallAuthContext = {
  roomId: string;
  localPeerId: string;
  remotePeerId: string;
};

export class AuthenticationManager {
  createChallenge(identityPeerId: string): Extract<CallAuthMessage, { type: "call.auth.challenge" }> {
    return {
      version: 1,
      type: "call.auth.challenge",
      identityPeerId,
      nonce: crypto.randomUUID(),
      timestamp: Date.now(),
      capabilities: LOCAL_BRIKS_CAPABILITIES,
    };
  }

  parse(raw: string): ParsedCallAuthMessage {
    let value: unknown;
    try { value = JSON.parse(raw); } catch { return { status: "not-auth" }; }
    if (!value || typeof value !== "object" || Array.isArray(value)) return { status: "not-auth" };
    const message = value as Partial<CallAuthMessage>;
    if (message.version !== 1 || typeof message.type !== "string" || !message.type.startsWith("call.auth.")) {
      return { status: "not-auth" };
    }
    if (!validBriksPeerCapabilities(message.capabilities) || !compatibleCallPeer(message.capabilities)) {
      return {
        status: "incompatible",
        remoteVersion: validBriksPeerCapabilities(message.capabilities) ? message.capabilities.appVersion : undefined,
      };
    }
    if (!withinP2PClockTolerance(message.timestamp)) return { status: "expired" };
    return { status: "valid", message };
  }

  async createProof(
    identity: LocalIdentity,
    context: CallAuthContext,
    nonce: string,
  ): Promise<Extract<CallAuthMessage, { type: "call.auth.proof" }>> {
    const profile = publicIdentity(identity);
    const signature = await crypto.subtle.sign(
      { name: "ECDSA", hash: "SHA-256" },
      identity.privateKey,
      new TextEncoder().encode(this.canonical(context, nonce, profile)),
    );
    return {
      version: 1,
      type: "call.auth.proof",
      identity: profile,
      nonce,
      timestamp: Date.now(),
      capabilities: LOCAL_BRIKS_CAPABILITIES,
      signature: bytesToBase64Url(new Uint8Array(signature)),
    };
  }

  async verifyProof(
    trusted: PublicPeerIdentity,
    remoteIdentity: PublicPeerIdentity,
    context: CallAuthContext,
    nonce: string,
    capabilities: BriksPeerCapabilities,
    signature: string,
  ): Promise<boolean> {
    if (!sameCallPublicKey(trusted.publicKey, remoteIdentity.publicKey)) return false;
    try {
      const key = await crypto.subtle.importKey("jwk", trusted.publicKey, { name: "ECDSA", namedCurve: "P-256" }, false, ["verify"]);
      return await crypto.subtle.verify(
        { name: "ECDSA", hash: "SHA-256" },
        key,
        base64UrlToArrayBuffer(signature),
        new TextEncoder().encode(this.canonical(context, nonce, remoteIdentity, capabilities)),
      );
    } catch {
      return false;
    }
  }

  validRemoteIdentity(remotePeerId: string, identity: PublicPeerIdentity): boolean {
    return identity.peerId === remotePeerId
      && /^[A-Za-z0-9_-]{8,128}$/.test(identity.peerId)
      && identity.displayName.trim().length >= 2
      && identity.displayName.length <= 80
      && (identity.avatar === undefined || validAvatarDataUrl(identity.avatar));
  }

  canonical(
    context: CallAuthContext,
    nonce: string,
    identity: PublicPeerIdentity,
    capabilities: BriksPeerCapabilities = LOCAL_BRIKS_CAPABILITIES,
  ): string {
    const peers = [context.localPeerId, context.remotePeerId].sort().join(":");
    return JSON.stringify({ protocol: "briks-call-auth-v2", roomId: context.roomId, peers, nonce, identity, capabilities });
  }
}

export function sameCallPublicKey(left: JsonWebKey, right: JsonWebKey): boolean {
  return left.kty === right.kty
    && left.crv === right.crv
    && left.x === right.x
    && left.y === right.y;
}

function bytesToBase64Url(bytes: Uint8Array): string {
  let binary = "";
  bytes.forEach((byte) => { binary += String.fromCharCode(byte); });
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function base64UrlToArrayBuffer(value: string): ArrayBuffer {
  const normalized = value.replace(/-/g, "+").replace(/_/g, "/").padEnd(Math.ceil(value.length / 4) * 4, "=");
  const binary = atob(normalized);
  return Uint8Array.from(binary, (character) => character.charCodeAt(0)).buffer;
}
