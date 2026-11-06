import type { RealtimeChannel, SupabaseClient } from "@supabase/supabase-js";
import type { PeerState } from "@briks/protocol";
import { getSupabaseRealtimeClient } from "./client";
import {
  P2P_CLOCK_SKEW_WARNING_MS,
  p2pClockSkewMs,
  withinP2PClockTolerance,
} from "../p2p-clock";
import { isValidPeer, parseAnswer, parseIceCandidate, parseOffer, parsePeerState } from "../signaling/validation";
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
} from "../signaling/types";

type CallbackMap = {
  peerJoined: (peer: SignalingPeer) => void;
  peerLeft: (peerId: string) => void;
  offer: (message: OfferMessage) => void;
  answer: (message: AnswerMessage) => void;
  ice: (message: IceCandidateMessage) => void;
  peerState: (message: PeerStateMessage) => void;
  status: (status: SignalingStatus) => void;
};

type CallbackSets = { [Key in keyof CallbackMap]: Set<CallbackMap[Key]> };
type RateWindow = { startedAt: number; count: number };
type ConnectionWaiter = { resolve: () => void; reject: (error: Error) => void; timer: ReturnType<typeof setTimeout> };

const DEBUG = import.meta.env.VITE_DEBUG_SIGNALING === "true";
const CLIENT_VERSION = import.meta.env.VITE_BRIKS_APP_VERSION ?? "0.2.1";
const PRESENCE_LEAVE_GRACE_MS = 10_000;
const SESSION_TIMESTAMP_TOLERANCE_MS = 1_500;
const SIGNALING_RECONNECT_SEND_TIMEOUT_MS = 20_000;
const SIGNALING_CHANNEL_CLEANUP_TIMEOUT_MS = 3_000;

export class SupabaseSignalingProvider implements SignalingProvider {
  private readonly callbacks: CallbackSets = {
    peerJoined: new Set(), peerLeft: new Set(), offer: new Set(), answer: new Set(),
    ice: new Set(), peerState: new Set(), status: new Set(),
  };
  private readonly presencePeers = new Map<string, SignalingPeer>();
  private readonly processedMessageIds = new Map<string, number>();
  private readonly rateWindows = new Map<string, RateWindow>();
  private channel?: RealtimeChannel;
  private client?: SupabaseClient;
  private roomId?: string;
  private peerId?: string;
  private status: SignalingStatus = "disconnected";
  private channelStatus = "CLOSED";
  private disconnecting = false;
  private channelName?: string;
  private reconnectTimer?: ReturnType<typeof setTimeout>;
  private reconnectAttempts = 0;
  private sessionStartedAt = 0;
  private readonly missingPeers = new Set<string>();
  private readonly peerLeaveTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private readonly peerDepartedAt = new Map<string, number>();
  private readonly connectionWaiters = new Set<ConnectionWaiter>();
  private lastObservedClockSkewMs?: number;
  private clockSkewRejectedMessages = 0;

  async connect(roomId: string, peerId: string, namespace: SignalingNamespace = "room"): Promise<void> {
    if (this.channel) await this.disconnect();
    this.disconnecting = false;
    this.sessionStartedAt = Date.now();
    this.missingPeers.clear();
    this.peerDepartedAt.clear();
    this.lastObservedClockSkewMs = undefined;
    this.clockSkewRejectedMessages = 0;
    this.peerLeaveTimers.forEach((timer) => clearTimeout(timer));
    this.peerLeaveTimers.clear();
    this.setStatus("connecting");
    this.client = getSupabaseRealtimeClient();
    this.roomId = await secureRoomId(`${namespace}:${roomId}`);
    this.peerId = peerId;
    this.channelName = `briks:${namespace}:${this.roomId.slice(0, 32)}`;
    await this.dropStaleChannels(this.channelName);
    this.channel = this.client.channel(this.channelName, {
      config: { presence: { key: peerId }, broadcast: { self: false, ack: true } },
    });
    this.registerChannelListeners(this.channel);
    const activeChannel = this.channel;
    await new Promise<void>((resolve, reject) => {
      let settled = false;
      const timeout = window.setTimeout(() => {
        if (!settled) { settled = true; this.setStatus("error"); reject(new Error("Tempo esgotado ao conectar ao Supabase Realtime.")); }
      }, 15_000);
      activeChannel.subscribe(async (status) => {
        if (this.channel !== activeChannel || this.disconnecting) return;
        this.channelStatus = status;
        if (status === "SUBSCRIBED") {
          if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
          this.reconnectTimer = undefined;
          this.reconnectAttempts = 0;
          this.setStatus("connected");
          try {
            await activeChannel.track({ peerId, joinedAt: this.sessionStartedAt, clientVersion: CLIENT_VERSION });
            this.reconcilePresence();
            if (!settled) { settled = true; window.clearTimeout(timeout); resolve(); }
          } catch (error) {
            if (!settled) { settled = true; window.clearTimeout(timeout); reject(error); }
          }
        } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT") {
          this.setStatus(settled ? "reconnecting" : "error");
          if (settled) this.scheduleReconnect();
          if (!settled) { settled = true; window.clearTimeout(timeout); reject(new Error(`Falha no canal Supabase Realtime: ${status}`)); }
        } else if (status === "CLOSED" && !this.disconnecting) {
          this.setStatus(settled ? "reconnecting" : "disconnected");
          if (settled) this.scheduleReconnect();
        }
      });
    });
  }

  async disconnect(): Promise<void> {
    this.disconnecting = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = undefined;
    this.reconnectAttempts = 0;
    this.rejectConnectionWaiters(new Error("Signaling Realtime foi desconectado."));
    this.peerLeaveTimers.forEach((timer) => clearTimeout(timer));
    this.peerLeaveTimers.clear();
    this.missingPeers.clear();
    this.peerDepartedAt.clear();
    const channel = this.channel;
    this.channel = undefined;
    if (channel) {
      await this.settleChannelOperation(() => channel.untrack());
      await this.removeChannelBestEffort(channel);
    }
    for (const peerId of this.presencePeers.keys()) this.emit("peerLeft", peerId);
    this.presencePeers.clear();
    this.processedMessageIds.clear();
    this.rateWindows.clear();
    this.roomId = undefined;
    this.peerId = undefined;
    this.channelStatus = "CLOSED";
    this.channelName = undefined;
    this.sessionStartedAt = 0;
    this.setStatus("disconnected");
    this.disconnecting = false;
  }

  sendOffer(targetPeerId: string, offer: RTCSessionDescriptionInit): Promise<void> {
    return this.broadcast("webrtc.offer", targetPeerId, { sdp: offer });
  }
  sendAnswer(targetPeerId: string, answer: RTCSessionDescriptionInit): Promise<void> {
    return this.broadcast("webrtc.answer", targetPeerId, { sdp: answer });
  }
  sendIceCandidate(targetPeerId: string, candidate: RTCIceCandidateInit): Promise<void> {
    return this.broadcast("webrtc.ice-candidate", targetPeerId, { candidate });
  }
  sendPeerState(state: PeerState): Promise<void> { return this.broadcast("peer.state", undefined, { state }); }

  onPeerJoined(callback: CallbackMap["peerJoined"]): () => void { return this.addCallback("peerJoined", callback); }
  onPeerLeft(callback: CallbackMap["peerLeft"]): () => void { return this.addCallback("peerLeft", callback); }
  onOffer(callback: CallbackMap["offer"]): () => void { return this.addCallback("offer", callback); }
  onAnswer(callback: CallbackMap["answer"]): () => void { return this.addCallback("answer", callback); }
  onIceCandidate(callback: CallbackMap["ice"]): () => void { return this.addCallback("ice", callback); }
  onPeerState(callback: CallbackMap["peerState"]): () => void { return this.addCallback("peerState", callback); }
  onStatusChange(callback: CallbackMap["status"]): () => void { return this.addCallback("status", callback); }

  getDiagnostics(): SignalingDiagnostics {
    return {
      status: this.status, channelStatus: this.channelStatus, peerId: this.peerId ?? null,
      roomId: this.roomId ?? null, connectedPeers: [...this.presencePeers.keys()],
      presencePeers: [...this.presencePeers.keys()], processedMessages: this.processedMessageIds.size,
      clockSkewMs: this.lastObservedClockSkewMs ?? null,
      clockSkewRejectedMessages: this.clockSkewRejectedMessages,
    };
  }

  private registerChannelListeners(channel: RealtimeChannel): void {
    channel
      .on("presence", { event: "sync" }, () => this.reconcilePresence())
      .on("broadcast", { event: "webrtc.offer" }, ({ payload }) => this.receive("offer", parseOffer(payload)))
      .on("broadcast", { event: "webrtc.answer" }, ({ payload }) => this.receive("answer", parseAnswer(payload)))
      .on("broadcast", { event: "webrtc.ice-candidate" }, ({ payload }) => this.receive("ice", parseIceCandidate(payload)))
      .on("broadcast", { event: "peer.state" }, ({ payload }) => this.receive("peerState", parsePeerState(payload)));
  }

  private scheduleReconnect(): void {
    if (this.disconnecting || this.reconnectTimer || !this.client || !this.channelName || !this.peerId) return;
    const delay = Math.min(30_000, 1_000 * 2 ** Math.min(this.reconnectAttempts, 5));
    this.reconnectAttempts += 1;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = undefined;
      void this.reopenChannel();
    }, delay);
  }

  private async reopenChannel(): Promise<void> {
    if (this.disconnecting || !this.client || !this.channelName || !this.peerId) return;
    const previous = this.channel;
    if (previous) await this.removeChannelBestEffort(previous);
    if (this.disconnecting || !this.client || !this.channelName || !this.peerId) return;
    await this.dropStaleChannels(this.channelName);
    let channel: RealtimeChannel;
    try {
      channel = this.client.channel(this.channelName, { config: { presence: { key: this.peerId }, broadcast: { self: false, ack: true } } });
    } catch {
      this.setStatus("reconnecting");
      this.scheduleReconnect();
      return;
    }
    this.channel = channel;
    this.registerChannelListeners(channel);
    channel.subscribe(async (status) => {
      if (channel !== this.channel || this.disconnecting) return;
      this.channelStatus = status;
      if (status === "SUBSCRIBED") {
        try {
          await channel.track({ peerId: this.peerId!, joinedAt: this.sessionStartedAt, clientVersion: CLIENT_VERSION });
          if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
          this.reconnectTimer = undefined;
          this.reconnectAttempts = 0;
          this.reconcilePresence();
          this.setStatus("connected");
        } catch {
          this.setStatus("reconnecting");
          this.scheduleReconnect();
        }
      } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT" || status === "CLOSED") {
        this.setStatus("reconnecting");
        this.scheduleReconnect();
      }
    });
  }

  /** O cliente devolve o canal já existente com o mesmo nome; remover os antigos evita adicionar callbacks depois de subscribe(). */
  private async dropStaleChannels(name: string): Promise<void> {
    const client = this.client;
    if (!client) return;
    const topic = `realtime:${name}`;
    for (const stale of (client.getChannels?.() ?? []).filter((item) => item.topic === topic && item !== this.channel)) {
      await this.removeChannelBestEffort(stale);
    }
  }

  private async removeChannelBestEffort(channel: RealtimeChannel): Promise<void> {
    const client = this.client;
    if (!client) return;
    await this.settleChannelOperation(() => client.removeChannel(channel));
  }

  private async settleChannelOperation(operation: () => Promise<unknown>): Promise<void> {
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
      await Promise.race([
        Promise.resolve().then(operation).catch(() => undefined),
        new Promise<void>((resolve) => {
          timer = setTimeout(resolve, SIGNALING_CHANNEL_CLEANUP_TIMEOUT_MS);
        }),
      ]);
    } finally {
      if (timer) clearTimeout(timer);
    }
  }

  private reconcilePresence(): void {
    const channel = this.channel;
    const ownPeerId = this.peerId;
    if (!channel || !ownPeerId) return;
    const next = new Map<string, SignalingPeer>();
    const state = channel.presenceState();
    for (const entries of Object.values(state)) {
      for (const entry of entries) {
        if (isValidPeer(entry) && entry.peerId !== ownPeerId) next.set(entry.peerId, entry);
      }
    }

    for (const [peerId, peer] of next) {
      const pendingLeave = this.peerLeaveTimers.get(peerId);
      if (pendingLeave) clearTimeout(pendingLeave);
      this.peerLeaveTimers.delete(peerId);
      this.missingPeers.delete(peerId);

      const existing = this.presencePeers.get(peerId);
      if (!existing) {
        this.presencePeers.set(peerId, peer);
        this.log("peer joined", peerId);
        this.emit("peerJoined", peer);
        continue;
      }

      if (Math.abs(existing.joinedAt - peer.joinedAt) > SESSION_TIMESTAMP_TOLERANCE_MS) {
        this.peerDepartedAt.set(peerId, Math.max(Date.now(), peer.joinedAt));
        this.presencePeers.set(peerId, peer);
        this.log("peer session replaced", peerId);
        this.emit("peerLeft", peerId);
        this.emit("peerJoined", peer);
        continue;
      }
      this.presencePeers.set(peerId, peer);
    }

    for (const peerId of this.presencePeers.keys()) {
      if (next.has(peerId) || this.peerLeaveTimers.has(peerId)) continue;
      this.missingPeers.add(peerId);
      const timer = setTimeout(() => {
        this.peerLeaveTimers.delete(peerId);
        if (!this.missingPeers.delete(peerId)) return;
        const peer = this.presencePeers.get(peerId);
        if (!peer) return;
        this.presencePeers.delete(peerId);
        this.peerDepartedAt.set(peerId, Date.now());
        this.log("peer left after grace", peerId);
        this.emit("peerLeft", peerId);
      }, PRESENCE_LEAVE_GRACE_MS);
      this.peerLeaveTimers.set(peerId, timer);
    }
  }

  private receive<Key extends "offer" | "answer" | "ice" | "peerState">(key: Key, message: Parameters<CallbackMap[Key]>[0] | null): void {
    if (!message || !this.acceptMessage(message)) return;
    this.log(`${key} received`, message.fromPeerId);
    this.emit(key, message);
  }

  private acceptMessage(message: OfferMessage | AnswerMessage | IceCandidateMessage | PeerStateMessage): boolean {
    if (!this.peerId || !this.roomId || message.roomId !== this.roomId || message.fromPeerId === this.peerId) return false;
    if (message.targetPeerId !== undefined && message.targetPeerId !== this.peerId) return false;
    const now = Date.now();
    const clockSkew = p2pClockSkewMs(message.timestamp, now);
    if (clockSkew !== null && Math.abs(clockSkew) >= P2P_CLOCK_SKEW_WARNING_MS) {
      this.lastObservedClockSkewMs = clockSkew;
    }
    if (!withinP2PClockTolerance(message.timestamp, now)) {
      this.clockSkewRejectedMessages += 1;
      console.warn("Signaling WebRTC rejeitado por diferença excessiva entre os relógios.", {
        fromPeerId: message.fromPeerId,
        clockSkewMs: clockSkew,
        type: message.type,
      });
      return false;
    }

    if (!this.presencePeers.has(message.fromPeerId)) this.reconcilePresence();
    const presentPeer = this.presencePeers.get(message.fromPeerId);
    if (presentPeer && message.timestamp + SESSION_TIMESTAMP_TOLERANCE_MS < presentPeer.joinedAt) {
      this.log("discarding signaling from older peer session", message.fromPeerId);
      return false;
    }
    const lastDeparture = this.peerDepartedAt.get(message.fromPeerId);
    if (!presentPeer && lastDeparture && message.timestamp <= lastDeparture) {
      this.log("discarding signaling sent before peer departure", message.fromPeerId);
      return false;
    }

    if (!presentPeer) {
      const targetedWebRtcMessage = message.targetPeerId === this.peerId
        && (message.type === "webrtc.offer" || message.type === "webrtc.answer" || message.type === "webrtc.ice-candidate");
      if (!targetedWebRtcMessage) return false;
      this.log("accepting fresh WebRTC signaling before presence sync", message.fromPeerId);
    }

    this.pruneCaches();
    if (this.processedMessageIds.has(message.messageId)) return false;
    if (!this.withinRateLimit(message.fromPeerId, message.type)) return false;
    this.processedMessageIds.set(message.messageId, now);
    return true;
  }

  private withinRateLimit(peerId: string, type: string): boolean {
    const key = `${peerId}:${type}`; const now = Date.now(); const existing = this.rateWindows.get(key);
    const limit = type === "webrtc.ice-candidate" ? 200 : 20;
    if (!existing || now - existing.startedAt > 10_000) { this.rateWindows.set(key, { startedAt: now, count: 1 }); return true; }
    existing.count += 1; return existing.count <= limit;
  }

  private pruneCaches(): void {
    const now = Date.now();
    const threshold = now - 120_000;
    for (const [id, timestamp] of this.processedMessageIds) if (timestamp < threshold) this.processedMessageIds.delete(id);
    for (const [key, window] of this.rateWindows) if (now - window.startedAt > 60_000) this.rateWindows.delete(key);
    if (this.processedMessageIds.size > 2_048) {
      const overflow = this.processedMessageIds.size - 2_048;
      [...this.processedMessageIds.keys()].slice(0, overflow).forEach((id) => this.processedMessageIds.delete(id));
    }
  }

  private async broadcast<Type extends string, Payload>(type: Type, targetPeerId: string | undefined, payload: Payload): Promise<void> {
    if (!this.roomId || !this.peerId || this.disconnecting) throw new Error("Signaling Realtime não está conectado.");
    if (!this.channel || this.status !== "connected") await this.waitForConnected();

    for (let attempt = 0; attempt < 2; attempt += 1) {
      const channel = this.channel;
      const roomId = this.roomId;
      const peerId = this.peerId;
      if (!channel || !roomId || !peerId || this.status !== "connected") {
        await this.waitForConnected();
        continue;
      }
      const envelope: SignalingEnvelope<Type, Payload> = {
        version: 1, roomId, fromPeerId: peerId, targetPeerId,
        messageId: crypto.randomUUID(), timestamp: Date.now(), type, payload,
      };
      try {
        const result = await channel.send({ type: "broadcast", event: type, payload: envelope });
        if (result === "ok") return;
      } catch {
        // Transient websocket failures are retried after reopening the channel.
      }
      if (attempt === 0 && !this.disconnecting) {
        this.setStatus("reconnecting");
        this.scheduleReconnect();
        await this.waitForConnected();
      }
    }
    throw new Error(`Falha ao enviar ${type} pelo Supabase Realtime após reconectar.`);
  }

  private waitForConnected(timeoutMs = SIGNALING_RECONNECT_SEND_TIMEOUT_MS): Promise<void> {
    if (this.channel && this.status === "connected") return Promise.resolve();
    if (this.disconnecting || !this.client || !this.channelName || !this.peerId || !this.roomId) {
      return Promise.reject(new Error("Signaling Realtime não está conectado."));
    }
    this.scheduleReconnect();
    return new Promise<void>((resolve, reject) => {
      const waiter = {} as ConnectionWaiter;
      waiter.resolve = () => {
        clearTimeout(waiter.timer);
        this.connectionWaiters.delete(waiter);
        resolve();
      };
      waiter.reject = (error) => {
        clearTimeout(waiter.timer);
        this.connectionWaiters.delete(waiter);
        reject(error);
      };
      waiter.timer = setTimeout(() => {
        waiter.reject(new Error("Tempo esgotado ao reconectar ao Supabase Realtime."));
      }, timeoutMs);
      this.connectionWaiters.add(waiter);
    });
  }

  private resolveConnectionWaiters(): void {
    [...this.connectionWaiters].forEach((waiter) => waiter.resolve());
  }

  private rejectConnectionWaiters(error: Error): void {
    [...this.connectionWaiters].forEach((waiter) => waiter.reject(error));
  }

  private addCallback<Key extends keyof CallbackMap>(key: Key, callback: CallbackMap[Key]): () => void {
    const callbacks = this.callbacks[key] as Set<CallbackMap[Key]>; callbacks.add(callback);
    return () => callbacks.delete(callback);
  }
  private emit<Key extends keyof CallbackMap>(key: Key, value: Parameters<CallbackMap[Key]>[0]): void {
    const callbacks = this.callbacks[key] as Set<(item: Parameters<CallbackMap[Key]>[0]) => void>;
    callbacks.forEach((callback) => callback(value));
  }
  private setStatus(status: SignalingStatus): void {
    if (this.status === status) return;
    this.status = status;
    if (status === "connected") this.resolveConnectionWaiters();
    this.log(status);
    this.emit("status", status);
  }
  private log(event: string, peerId?: string): void { if (DEBUG) console.info(`[signaling] ${event}`, peerId ? { peerId } : undefined); }
}

async function secureRoomId(roomId: string): Promise<string> {
  const bytes = new TextEncoder().encode(roomId);
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((value) => value.toString(16).padStart(2, "0")).join("");
}
