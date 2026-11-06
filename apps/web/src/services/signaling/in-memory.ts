import type { PeerState } from "@briks/protocol";
import type {
  AnswerMessage,
  IceCandidateMessage,
  OfferMessage,
  PeerStateMessage,
  SignalingDiagnostics,
  SignalingEnvelope,
  SignalingPeer,
  SignalingProvider,
  SignalingNamespace,
  SignalingStatus,
} from "./types";
import { BRIKS_APP_VERSION } from "../protocol-compatibility";

type TestEnvelope = OfferMessage | AnswerMessage | IceCandidateMessage | PeerStateMessage;
type Listener<T> = (value: T) => void;

export class InMemorySignalingHub {
  private readonly rooms = new Map<string, Set<InMemorySignalingProvider>>();
  lastEnvelope?: TestEnvelope;

  join(roomId: string, provider: InMemorySignalingProvider): InMemorySignalingProvider[] {
    const room = this.rooms.get(roomId) ?? new Set<InMemorySignalingProvider>();
    const existing = [...room];
    room.add(provider);
    this.rooms.set(roomId, room);
    return existing;
  }

  leave(roomId: string, provider: InMemorySignalingProvider): void {
    const room = this.rooms.get(roomId);
    if (!room) return;
    room.delete(provider);
    room.forEach((peer) => peer.receivePeerLeft(provider.currentPeerId));
    if (room.size === 0) this.rooms.delete(roomId);
  }

  broadcast(roomId: string, sender: InMemorySignalingProvider, envelope: TestEnvelope): void {
    this.lastEnvelope = envelope;
    this.rooms.get(roomId)?.forEach((provider) => {
      if (provider !== sender) provider.receiveEnvelope(envelope);
    });
  }

  replayLast(roomId: string): void {
    if (!this.lastEnvelope) return;
    this.rooms.get(roomId)?.forEach((provider) => provider.receiveEnvelope(this.lastEnvelope!));
  }

  roomSize(roomId: string): number { return this.rooms.get(roomId)?.size ?? 0; }
}

export class InMemorySignalingProvider implements SignalingProvider {
  private roomId?: string;
  private peerId?: string;
  private status: SignalingStatus = "disconnected";
  private readonly peers = new Map<string, SignalingPeer>();
  private readonly processed = new Set<string>();
  private readonly peerJoined = new Set<Listener<SignalingPeer>>();
  private readonly peerLeft = new Set<Listener<string>>();
  private readonly offers = new Set<Listener<OfferMessage>>();
  private readonly answers = new Set<Listener<AnswerMessage>>();
  private readonly ice = new Set<Listener<IceCandidateMessage>>();
  private readonly peerStates = new Set<Listener<PeerStateMessage>>();
  private readonly statuses = new Set<Listener<SignalingStatus>>();

  constructor(
    private readonly hub: InMemorySignalingHub,
    private readonly clientVersion = BRIKS_APP_VERSION,
  ) {}

  get currentPeerId(): string { return this.peerId ?? ""; }

  async connect(roomId: string, peerId: string, namespace: SignalingNamespace = "room"): Promise<void> {
    if (this.roomId) await this.disconnect();
    this.roomId = namespace === "room" ? roomId : `${namespace}:${roomId}`;
    this.peerId = peerId;
    this.setStatus("connecting");
    const existing = this.hub.join(this.roomId, this);
    const me = this.asPeer();
    this.setStatus("connected");
    existing.forEach((provider) => {
      const peer = provider.asPeer();
      this.receivePeerJoined(peer);
      provider.receivePeerJoined(me);
    });
  }

  async disconnect(): Promise<void> {
    if (this.roomId) this.hub.leave(this.roomId, this);
    this.peers.clear();
    this.processed.clear();
    this.roomId = undefined;
    this.peerId = undefined;
    this.setStatus("disconnected");
  }

  sendOffer(targetPeerId: string, offer: RTCSessionDescriptionInit): Promise<void> {
    return this.send("webrtc.offer", targetPeerId, { sdp: offer });
  }
  sendAnswer(targetPeerId: string, answer: RTCSessionDescriptionInit): Promise<void> {
    return this.send("webrtc.answer", targetPeerId, { sdp: answer });
  }
  sendIceCandidate(targetPeerId: string, candidate: RTCIceCandidateInit): Promise<void> {
    return this.send("webrtc.ice-candidate", targetPeerId, { candidate });
  }
  sendPeerState(state: PeerState): Promise<void> { return this.send("peer.state", undefined, { state }); }

  onPeerJoined(callback: Listener<SignalingPeer>): () => void { return add(this.peerJoined, callback); }
  onPeerLeft(callback: Listener<string>): () => void { return add(this.peerLeft, callback); }
  onOffer(callback: Listener<OfferMessage>): () => void { return add(this.offers, callback); }
  onAnswer(callback: Listener<AnswerMessage>): () => void { return add(this.answers, callback); }
  onIceCandidate(callback: Listener<IceCandidateMessage>): () => void { return add(this.ice, callback); }
  onPeerState(callback: Listener<PeerStateMessage>): () => void { return add(this.peerStates, callback); }
  onStatusChange(callback: Listener<SignalingStatus>): () => void { return add(this.statuses, callback); }

  getDiagnostics(): SignalingDiagnostics {
    return {
      status: this.status, channelStatus: this.status === "connected" ? "SUBSCRIBED" : "CLOSED",
      peerId: this.peerId ?? null, roomId: this.roomId ?? null,
      connectedPeers: [...this.peers.keys()], presencePeers: [...this.peers.keys()],
      processedMessages: this.processed.size,
      clockSkewMs: null,
      clockSkewRejectedMessages: 0,
    };
  }

  receivePeerJoined(peer: SignalingPeer): void {
    if (peer.peerId === this.peerId || this.peers.has(peer.peerId)) return;
    this.peers.set(peer.peerId, peer);
    this.peerJoined.forEach((callback) => callback(peer));
  }

  receivePeerLeft(peerId: string): void {
    if (!this.peers.delete(peerId)) return;
    this.peerLeft.forEach((callback) => callback(peerId));
  }

  receiveEnvelope(envelope: TestEnvelope): void {
    if (!this.peerId || !this.roomId || envelope.roomId !== this.roomId || envelope.fromPeerId === this.peerId) return;
    if (envelope.targetPeerId !== undefined && envelope.targetPeerId !== this.peerId) return;
    if (this.processed.has(envelope.messageId)) return;
    this.processed.add(envelope.messageId);
    switch (envelope.type) {
      case "webrtc.offer": this.offers.forEach((callback) => callback(envelope)); break;
      case "webrtc.answer": this.answers.forEach((callback) => callback(envelope)); break;
      case "webrtc.ice-candidate": this.ice.forEach((callback) => callback(envelope)); break;
      case "peer.state": this.peerStates.forEach((callback) => callback(envelope)); break;
    }
  }

  private asPeer(): SignalingPeer { return { peerId: this.peerId!, joinedAt: Date.now(), clientVersion: this.clientVersion }; }

  private async send<Type extends TestEnvelope["type"], Payload>(type: Type, targetPeerId: string | undefined, payload: Payload): Promise<void> {
    if (!this.roomId || !this.peerId || this.status !== "connected") throw new Error("Provider em memória desconectado.");
    const envelope: SignalingEnvelope<Type, Payload> = {
      version: 1, roomId: this.roomId, fromPeerId: this.peerId, targetPeerId,
      messageId: crypto.randomUUID(), timestamp: Date.now(), type, payload,
    };
    this.hub.broadcast(this.roomId, this, envelope as TestEnvelope);
  }

  private setStatus(status: SignalingStatus): void {
    this.status = status;
    this.statuses.forEach((callback) => callback(status));
  }
}

function add<T>(listeners: Set<Listener<T>>, callback: Listener<T>): () => void {
  listeners.add(callback);
  return () => listeners.delete(callback);
}
