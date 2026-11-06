import {
  MAX_WIRE_BYTES,
  bytesToBase64Url,
  canonicalIdentityProof,
  type IdentityChallengeWireMessage,
  type IdentityProofWireMessage,
} from "./MessageProtocol";
import {
  compatibleChatPeer,
  LOCAL_BRIKS_CAPABILITIES,
  validBriksPeerCapabilities,
  peerSupportsCapability,
  type BriksCapability,
  type BriksPeerCapabilities,
} from "../services/protocol-compatibility";
import type { LocalIdentity } from "../services/offline/social-storage";

const IDENTITY_HANDSHAKE_RETRY_MS = 1_200;
const IDENTITY_HANDSHAKE_TIMEOUT_MS = 12_000;

export type ChatSyncContext = {
  identity(): LocalIdentity | undefined;
  channelId(): string | undefined;
  localPeerId(): string | undefined;
  sessionToken(): object | undefined;
  send(remotePeerId: string, wire: string): number;
  isDataPeer(remotePeerId: string): boolean;
  isOpenPeer(remotePeerId: string): boolean;
  isAllowed(remotePeerId: string): boolean;
  isTrusted(remotePeerId: string): boolean;
  isRevoked(remotePeerId: string): boolean;
  verify(remotePeerId: string, signature: string, canonical: string): Promise<boolean>;
  presencePeers(): string[];
  onReady(remotePeerId: string): void;
  onCapabilities?(remotePeerId: string, capabilities: BriksPeerCapabilities): void;
  onRevoked(remotePeerId: string): void;
  onFailure(remotePeerId: string): void;
};

export class SyncService {
  private readonly pendingChallenges = new Map<string, string>();
  private readonly timers = new Map<string, ReturnType<typeof setTimeout>>();
  private readonly startedAt = new Map<string, number>();
  private readonly failedPeers = new Set<string>();
  private readonly peerCapabilities = new Map<string, BriksPeerCapabilities>();

  constructor(private readonly context: ChatSyncContext) {}

  resetSession(): void {
    for (const timer of this.timers.values()) clearTimeout(timer);
    this.timers.clear();
    this.pendingChallenges.clear();
    this.startedAt.clear();
    this.failedPeers.clear();
    this.peerCapabilities.clear();
  }

  forgetPeer(remotePeerId: string): void {
    this.clear(remotePeerId);
    this.pendingChallenges.delete(remotePeerId);
    this.failedPeers.delete(remotePeerId);
    this.peerCapabilities.delete(remotePeerId);
  }

  failedPeerIds(): string[] { return [...this.failedPeers]; }
  didFail(remotePeerId: string): boolean { return this.failedPeers.has(remotePeerId); }
  clearFailure(remotePeerId: string): void { this.failedPeers.delete(remotePeerId); }
  capabilities(remotePeerId: string): BriksPeerCapabilities | undefined { return this.peerCapabilities.get(remotePeerId); }
  supports(remotePeerId: string, capability: BriksCapability): boolean {
    return peerSupportsCapability(this.peerCapabilities.get(remotePeerId), capability);
  }

  rejectIncompatibleEnvelope(raw: string): { incompatible: boolean; remoteVersion?: string } {
    if (new TextEncoder().encode(raw).byteLength > MAX_WIRE_BYTES) return { incompatible: false };
    let parsed: unknown;
    try { parsed = JSON.parse(raw); } catch { return { incompatible: false }; }
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return { incompatible: false };
    const message = parsed as Record<string, unknown>;
    if (message.type !== "chat.identity.challenge" && message.type !== "chat.identity.proof") return { incompatible: false };
    if (!validBriksPeerCapabilities(message.capabilities)) return { incompatible: true };
    return compatibleChatPeer(message.capabilities)
      ? { incompatible: false }
      : { incompatible: true, remoteVersion: message.capabilities.appVersion };
  }

  async begin(remotePeerId: string): Promise<void> {
    const identity = this.context.identity();
    const channelId = this.context.channelId();
    if (!identity || !channelId || !this.context.isDataPeer(remotePeerId)
      || !this.context.isAllowed(remotePeerId) || this.context.isOpenPeer(remotePeerId)
      || this.failedPeers.has(remotePeerId)) return;
    const now = Date.now();
    const startedAt = this.startedAt.get(remotePeerId) ?? now;
    this.startedAt.set(remotePeerId, startedAt);
    if (now - startedAt >= IDENTITY_HANDSHAKE_TIMEOUT_MS) {
      this.fail(remotePeerId, "timeout");
      return;
    }
    const nonce = this.pendingChallenges.get(remotePeerId) ?? crypto.randomUUID();
    const challenge: IdentityChallengeWireMessage = {
      version: 2,
      type: "chat.identity.challenge",
      channelId,
      fromPeerId: identity.peerId,
      nonce,
      timestamp: now,
      capabilities: LOCAL_BRIKS_CAPABILITIES,
    };
    this.pendingChallenges.set(remotePeerId, nonce);
    this.context.send(remotePeerId, JSON.stringify(challenge));
    this.schedule(remotePeerId);
  }

  async respond(remotePeerId: string, challenge: IdentityChallengeWireMessage): Promise<void> {
    const sessionToken = this.context.sessionToken();
    const identity = this.context.identity();
    const channelId = this.context.channelId();
    if (!sessionToken || !identity || !channelId || challenge.fromPeerId !== remotePeerId) return;
    const unsigned: Omit<IdentityProofWireMessage, "signature"> = {
      version: 2,
      type: "chat.identity.proof",
      channelId,
      fromPeerId: identity.peerId,
      toPeerId: remotePeerId,
      nonce: challenge.nonce,
      timestamp: Date.now(),
      capabilities: LOCAL_BRIKS_CAPABILITIES,
    };
    const signature = await crypto.subtle.sign(
      { name: "ECDSA", hash: "SHA-256" },
      identity.privateKey,
      new TextEncoder().encode(canonicalIdentityProof(unsigned)),
    );
    if (this.context.sessionToken() !== sessionToken || this.context.identity() !== identity || !this.context.isDataPeer(remotePeerId)) return;
    const proof: IdentityProofWireMessage = { ...unsigned, signature: bytesToBase64Url(new Uint8Array(signature)) };
    this.context.send(remotePeerId, JSON.stringify(proof));
    if (!this.context.isOpenPeer(remotePeerId) && !this.pendingChallenges.has(remotePeerId) && !this.failedPeers.has(remotePeerId)) {
      void this.begin(remotePeerId);
    }
  }

  async accept(remotePeerId: string, proof: IdentityProofWireMessage): Promise<void> {
    const sessionToken = this.context.sessionToken();
    const identity = this.context.identity();
    if (!sessionToken || !identity || proof.fromPeerId !== remotePeerId || proof.toPeerId !== identity.peerId) return;
    const expectedNonce = this.pendingChallenges.get(remotePeerId);
    if (!expectedNonce || proof.nonce !== expectedNonce) return;
    const valid = await this.context.verify(remotePeerId, proof.signature, canonicalIdentityProof(proof));
    if (this.context.sessionToken() !== sessionToken || this.context.identity() !== identity
      || this.pendingChallenges.get(remotePeerId) !== expectedNonce) return;
    if (!valid) {
      this.fail(remotePeerId, "invalid-proof");
      return;
    }
    this.pendingChallenges.delete(remotePeerId);
    this.clear(remotePeerId);
    this.failedPeers.delete(remotePeerId);
    this.peerCapabilities.set(remotePeerId, proof.capabilities);
    this.context.onCapabilities?.(remotePeerId, proof.capabilities);
    if (this.context.isRevoked(remotePeerId)) this.context.onRevoked(remotePeerId);
    else this.context.onReady(remotePeerId);
  }

  private schedule(remotePeerId: string): void {
    this.clearTimer(remotePeerId);
    if (!this.context.identity() || !this.context.isDataPeer(remotePeerId)
      || this.context.isOpenPeer(remotePeerId) || this.failedPeers.has(remotePeerId)) return;
    const startedAt = this.startedAt.get(remotePeerId) ?? Date.now();
    this.startedAt.set(remotePeerId, startedAt);
    const remaining = IDENTITY_HANDSHAKE_TIMEOUT_MS - (Date.now() - startedAt);
    if (remaining <= 0) {
      this.fail(remotePeerId, "timeout");
      return;
    }
    const timer = setTimeout(() => {
      this.timers.delete(remotePeerId);
      if (!this.context.isDataPeer(remotePeerId) || this.context.isOpenPeer(remotePeerId)) return;
      if (Date.now() - startedAt >= IDENTITY_HANDSHAKE_TIMEOUT_MS) {
        this.fail(remotePeerId, "timeout");
        return;
      }
      void this.begin(remotePeerId);
    }, Math.min(IDENTITY_HANDSHAKE_RETRY_MS, remaining));
    this.timers.set(remotePeerId, timer);
  }

  private clearTimer(remotePeerId: string): void {
    const timer = this.timers.get(remotePeerId);
    if (timer) clearTimeout(timer);
    this.timers.delete(remotePeerId);
  }

  private clear(remotePeerId: string): void {
    this.clearTimer(remotePeerId);
    this.startedAt.delete(remotePeerId);
  }

  private fail(remotePeerId: string, reason: "timeout" | "invalid-proof"): void {
    this.clear(remotePeerId);
    this.pendingChallenges.delete(remotePeerId);
    this.failedPeers.add(remotePeerId);
    console.warn("Autenticação de identidade do chat P2P não foi concluída", {
      reason,
      remotePeerId,
      localPeerId: this.context.localPeerId(),
      channelId: this.context.channelId(),
      trustedPeer: this.context.isTrusted(remotePeerId),
      presencePeers: this.context.presencePeers(),
    });
    this.context.onFailure(remotePeerId);
  }
}
