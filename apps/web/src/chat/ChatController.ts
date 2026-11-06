import { MeshWebRTCTransport } from "@briks/rtc";
import {
  ChatAttachmentService,
  type AttachmentRuntimeState,
  type StoredAttachmentRecord,
} from "./AttachmentService";
import {
  parseChatWireEnvelope,
  canonicalChatEvent,
  type GroupRevocationWireMessage,
  type HistoryRequestWireMessage,
  type MembersRequestWireMessage,
  type SignedChatEventWireMessage,
} from "./MessageProtocol";
import { HistoryService } from "./HistoryService";
import { GroupChatService, inferLocalGroupSecurity } from "./GroupChatService";
import { MessageService, type ChatEventInput, type ChatMessageChange } from "./MessageService";
import { OutboxService } from "./OutboxService";
import { SyncService } from "./SyncService";
import type { SignalingNamespace, SignalingProvider } from "../services/signaling/types";
import {
  compatibleAppVersion,
  incompatiblePeerMessage,
} from "../services/protocol-compatibility";
import type { LocalChatMessage, MessagePageOptions } from "../services/offline/chat-storage";
import { loadRtcNetworkContext } from "../services/network/runtime";
import {
  applyGroupRevocationCertificate,
  getOrCreateLocalIdentity,
  loadLocalGroups,
  groupRendezvousId,
  type GroupRevocationCertificate,
  type LocalIdentity,
  type PublicPeerIdentity,
} from "../services/offline/social-storage";

export type ChatConnectionStatus = "disconnected" | "connecting" | "connected" | "ready" | "incompatible" | "error";
export type ChatAttachmentRecord = StoredAttachmentRecord;
export type ChatAttachmentProgress = AttachmentRuntimeState;
export type ChatSendOptions = { replyToId?: string };
export type ChatTypingParticipant = { peerId: string; displayName: string };

export type ChatConnectionOptions = {
  identity?: LocalIdentity;
  trustedPeers?: PublicPeerIdentity[];
  revokedPeers?: PublicPeerIdentity[];
  revocations?: GroupRevocationCertificate[];
  groupId?: string;
  requireIdentityAuthentication?: boolean;
  rendezvousId?: string;
  namespace?: SignalingNamespace;
  maxRemotePeers?: number;
  /** Sessão de controle do grupo: sincroniza manifesto sem histórico/anexos. */
  membershipOnly?: boolean;
};

const CHAT_READY_TIMEOUT_MS = 15_000;
const missingSignaling = (): SignalingProvider => {
  throw new Error("ChatController precisa receber um adapter de signaling no composition root.");
};

export type MembershipReport = {
  status: ChatConnectionStatus;
  openPeers: number;
  authorizedOnline: boolean;
  localIsAuthorized: boolean;
  received: number;
  accepted: number;
  rejected: number;
  lastAcceptedAt?: number;
  members: number;
};

export class ChatController {
  private signaling?: SignalingProvider;
  private transport?: MeshWebRTCTransport;
  private readonly attachments = new ChatAttachmentService();
  private readonly messages = new MessageService();
  private readonly outbox = new OutboxService();
  private readonly groups = new GroupChatService();
  private readonly historySync = new HistoryService(
    this.messages,
    (message) => this.groups.verifySignedMessage(message),
    (event) => this.groups.verifyCanonical(event.actorPeerId, event.signature, canonicalChatEvent(event)),
  );
  private readonly sync = new SyncService({
    identity: () => this.identity,
    channelId: () => this.channelId,
    localPeerId: () => this.peerId,
    sessionToken: () => this.sessionToken,
    send: (remotePeerId, wire) => this.transport?.sendData(wire, remotePeerId) ?? 0,
    isDataPeer: (remotePeerId) => this.dataChannelPeers.has(remotePeerId),
    isOpenPeer: (remotePeerId) => this.openDataPeers.has(remotePeerId),
    isAllowed: (remotePeerId) => this.isTrustedRemote(remotePeerId),
    isTrusted: (remotePeerId) => this.groups.isTrusted(remotePeerId),
    isRevoked: (remotePeerId) => this.groups.isRevoked(remotePeerId),
    verify: (remotePeerId, signature, canonical) => this.groups.verifyCanonical(remotePeerId, signature, canonical),
    presencePeers: () => this.signaling?.getDiagnostics().presencePeers ?? [],
    onReady: (remotePeerId) => this.markPeerReady(remotePeerId),
    onRevoked: (remotePeerId) => this.markRevokedPeerReady(remotePeerId),
    onFailure: () => { if (this.openDataPeers.size === 0) this.setStatus("error"); },
  });
  private channelId?: string;
  private groupId?: string;
  private rendezvousId?: string;
  private signalingNamespace: SignalingNamespace = "chat";
  private peerId?: string;
  private displayName = "Participante";
  private identity?: LocalIdentity;
  private status: ChatConnectionStatus = "disconnected";
  private readonly membershipStats = { received: 0, accepted: 0, rejected: 0, lastAcceptedAt: undefined as number | undefined };
  private readonly dataChannelPeers = new Set<string>();
  private readonly openDataPeers = new Set<string>();
  private readonly revocationOnlyPeers = new Set<string>();
  private readonly statusCallbacks = new Set<(status: ChatConnectionStatus) => void>();
  private unsubscribers: Array<() => void> = [];
  private refreshingMembers?: Promise<void>;
  private pendingMemberRefreshBroadcast = false;
  private membershipOnly = false;
  private sessionToken?: object;
  private readyTimer?: ReturnType<typeof setTimeout>;
  private readonly typingCallbacks = new Set<(participants: ChatTypingParticipant[]) => void>();
  private readonly typingPeers = new Map<string, { displayName: string; timer: ReturnType<typeof setTimeout> }>();
  private localTypingActive = false;
  private lastTypingSentAt = 0;

  constructor(private readonly createSignaling: () => SignalingProvider = missingSignaling) {}

  history(channelId: string, options?: MessagePageOptions): Promise<LocalChatMessage[]> { return this.messages.history(channelId, options); }

  async attachmentHistory(channelId: string): Promise<StoredAttachmentRecord[]> {
    return this.attachments.history(channelId, this.channelId === channelId);
  }

  async connect(
    channelId: string,
    displayName: string,
    iceServers: RTCIceServer[],
    options: ChatConnectionOptions = {},
  ): Promise<void> {
    await this.disconnect();
    this.setStatus("connecting");
    const sessionToken = {};
    this.sessionToken = sessionToken;
    let signaling: SignalingProvider | undefined;
    let transport: MeshWebRTCTransport | undefined;

    try {
      const inferred = options.identity ? null : await inferLocalGroupSecurity(channelId, displayName);
      if (this.sessionToken !== sessionToken) throw new DOMException("Conexão do chat substituída por outra sessão.", "AbortError");
      const identity = options.identity ?? inferred?.identity;
      const trustedPeers = options.trustedPeers ?? inferred?.trustedPeers ?? [];
      const revokedPeers = options.revokedPeers ?? inferred?.revokedPeers ?? [];
      if (options.requireIdentityAuthentication && !identity) {
        throw new Error("A identidade P2P é obrigatória para conectar ao chat deste grupo.");
      }

      const localPeerId = identity?.peerId ?? crypto.randomUUID();
      const rendezvousId = options.rendezvousId ?? inferred?.rendezvousId ?? channelId;
      const namespace = options.namespace ?? "chat";
      this.channelId = channelId;
      this.groupId = options.groupId ?? inferred?.groupId;
      this.rendezvousId = rendezvousId;
      this.signalingNamespace = namespace;
      this.membershipOnly = options.membershipOnly === true;
      this.identity = identity;
      this.peerId = localPeerId;
      this.displayName = displayName.trim();
      this.groups.configure(identity, trustedPeers, revokedPeers, options.revocations ?? inferred?.revocations ?? []);

      const network = await loadRtcNetworkContext();
      if (this.sessionToken !== sessionToken) throw new DOMException("Conexão do chat substituída por outra sessão.", "AbortError");

      signaling = this.createSignaling();
      this.signaling = signaling;
      transport = new MeshWebRTCTransport(localPeerId, iceServers, {
        sendOffer: (targetPeerId, description) => signaling!.sendOffer(targetPeerId, description),
        sendAnswer: (targetPeerId, description) => signaling!.sendAnswer(targetPeerId, description),
        sendIce: (targetPeerId, candidate) => signaling!.sendIceCandidate(targetPeerId, candidate),
        onRemoteStream: () => undefined,
        onConnectionState: (remotePeerId, state) => {
          if (this.sessionToken !== sessionToken) return;
          if (state === "failed" || state === "closed") this.forgetPeerConnection(remotePeerId);
        },
        onNegotiationError: (remotePeerId, error) => {
          if (this.sessionToken !== sessionToken) return;
          console.warn("Falha de negociação WebRTC no chat", { remotePeerId, error });
          if (this.openDataPeers.size === 0) this.setStatus("error");
        },
        onDataMessage: (remotePeerId, data) => {
          if (this.sessionToken !== sessionToken) return;
          void this.receiveData(remotePeerId, data);
        },
        onDataState: (remotePeerId, state) => {
          if (this.sessionToken !== sessionToken) return;
          if (state === "open") {
            this.dataChannelPeers.add(remotePeerId);
            this.sync.clearFailure(remotePeerId);
            if (this.identity) void this.sync.begin(remotePeerId);
            else this.markPeerReady(remotePeerId);
          } else {
            this.forgetPeerConnection(remotePeerId);
          }
        },
        onTransferMessage: (remotePeerId, data) => {
          if (this.sessionToken !== sessionToken || !this.openDataPeers.has(remotePeerId)) return;
          void this.attachments.handleTransfer(remotePeerId, data).catch((error) => console.warn("Frame de anexo rejeitado", error));
        },
        onTransferState: (remotePeerId, state) => {
          if (this.sessionToken !== sessionToken) return;
          if (state === "open" && this.openDataPeers.has(remotePeerId) && !this.groups.isRevoked(remotePeerId)) {
            void this.attachments.peerReady(remotePeerId).catch((error) => console.warn("Falha ao preparar canal de anexos", { remotePeerId, error }));
          }
        },
      }, {
        maxRemotePeers: options.maxRemotePeers,
        networkInterfaces: network.networkInterfaces,
        networkPreference: network.preference,
      });
      this.transport = transport;

      const attachmentGeneration = !this.membershipOnly
        ? await this.attachments.connect(
          transport,
          channelId,
          localPeerId,
          () => this.sessionToken === sessionToken ? [...this.openDataPeers] : [],
          () => this.sessionToken === sessionToken && this.transport === transport,
        )
        : undefined;
      if (this.sessionToken !== sessionToken || this.transport !== transport) {
        if (attachmentGeneration !== undefined) this.attachments.clear(attachmentGeneration);
        await transport.disconnect().catch(() => undefined);
        throw new DOMException("Conexão do chat substituída por outra sessão.", "AbortError");
      }
      this.bindSignaling(signaling, localPeerId);
      if (this.groupId && typeof window !== "undefined") {
        const refresh = () => { if (this.sessionToken === sessionToken) void this.refreshGroupMembership(true); };
        window.addEventListener("briks:social-updated", refresh);
        this.unsubscribers.push(() => window.removeEventListener("briks:social-updated", refresh));
        const resync = () => { if (this.sessionToken === sessionToken) void this.resyncGroupMembership(); };
        window.addEventListener("briks:group-resync", resync);
        this.unsubscribers.push(() => window.removeEventListener("briks:group-resync", resync));
      }

      await signaling.connect(rendezvousId, localPeerId, namespace);
      if (this.sessionToken !== sessionToken || this.transport !== transport) {
        throw new DOMException("Conexão do chat substituída por outra sessão.", "AbortError");
      }
      this.setStatus("connected");
      this.armReadyTimeout(sessionToken);
      if (this.groupId) await this.refreshGroupMembership(false);
      else await this.connectPresentTrustedPeers();
    } catch (error) {
      if (this.sessionToken === sessionToken) {
        await this.disconnect();
        if (!(error instanceof DOMException && error.name === "AbortError")) this.setStatus("error");
      } else {
        await signaling?.disconnect().catch(() => undefined);
        await transport?.disconnect().catch(() => undefined);
      }
      throw error;
    }
  }

  async disconnect(): Promise<void> {
    const signaling = this.signaling;
    const transport = this.transport;
    const unsubscribers = this.unsubscribers.splice(0);

    // Desanexa o estado atual antes de aguardar rede/RTC. Assim um disconnect antigo
    // não consegue apagar um signaling/transport criado por uma conexão posterior.
    this.sessionToken = undefined;
    this.signaling = undefined;
    this.transport = undefined;
    this.attachments.clear();
    this.channelId = undefined;
    this.groupId = undefined;
    this.rendezvousId = undefined;
    this.signalingNamespace = "chat";
    this.peerId = undefined;
    this.identity = undefined;
    this.membershipOnly = false;
    this.pendingMemberRefreshBroadcast = false;
    this.refreshingMembers = undefined;
    this.clearReadyTimeout();
    this.dataChannelPeers.clear();
    this.openDataPeers.clear();
    this.revocationOnlyPeers.clear();
    for (const peer of this.typingPeers.values()) clearTimeout(peer.timer);
    this.typingPeers.clear();
    this.localTypingActive = false;
    this.lastTypingSentAt = 0;
    this.emitTyping();
    this.groups.reset();
    this.sync.resetSession();
    this.historySync.resetSession();
    this.messages.resetSession();
    this.setStatus("disconnected");

    unsubscribers.forEach((unsubscribe) => unsubscribe());
    await signaling?.disconnect().catch(() => undefined);
    await transport?.disconnect().catch(() => undefined);
  }

  async send(content: string, options: ChatSendOptions = {}): Promise<LocalChatMessage> {
    if (!this.channelId || !this.transport) throw new Error("Conecte o chat primeiro.");
    const trimmed = this.messages.validateContent(content);

    if (this.identity) {
      const wire = await this.messages.createSigned(this.identity, this.channelId, this.displayName, trimmed);
      const serialized = JSON.stringify(wire);
      await this.outbox.enqueue(wire);
      this.sendToAuthenticatedPeers(serialized);
      const message = await this.messages.persistSigned(wire);
      if (options.replyToId) await this.publishEvent(wire.id, { action: "reply", referenceMessageId: options.replyToId });
      return message;
    }

    const wire = this.messages.createLegacy(this.channelId, this.displayName, trimmed);
    if (this.transport.sendData(JSON.stringify(wire)) === 0) {
      throw new Error("Nenhuma conexão P2P disponível ou os canais estão congestionados.");
    }
    return this.messages.persistLegacy(wire, this.displayName);
  }

  async queue(channelId: string, content: string, displayName: string, options: ChatSendOptions = {}): Promise<LocalChatMessage> {
    const identity = await getOrCreateLocalIdentity(displayName);
    const wire = await this.messages.createSigned(identity, channelId, identity.displayName, content);
    await this.outbox.enqueue(wire);
    const message = await this.messages.persistSigned(wire);
    if (options.replyToId) {
      const event = await this.messages.createEvent(identity, channelId, wire.id, { action: "reply", referenceMessageId: options.replyToId });
      await this.outbox.enqueue(event);
      await this.messages.persistEvent(event);
    }
    return message;
  }

  async editMessage(messageId: string, content: string): Promise<void> {
    const message = await this.requireOwnMessage(messageId);
    if (message.deletedAt) throw new Error("Não é possível editar uma mensagem excluída.");
    await this.publishEvent(messageId, { action: "edit", content });
  }

  async deleteMessage(messageId: string): Promise<void> {
    await this.requireOwnMessage(messageId);
    await this.publishEvent(messageId, { action: "delete" });
  }

  async setReaction(messageId: string, emoji: string, active: boolean): Promise<void> {
    if (!this.channelId || !this.identity || !(await this.messages.find(this.channelId, messageId))) throw new Error("Mensagem não encontrada.");
    await this.publishEvent(messageId, { action: active ? "reaction.add" : "reaction.remove", emoji });
  }

  async setPinned(messageId: string, pinned: boolean): Promise<void> {
    if (!this.channelId || !this.identity || !(await this.messages.find(this.channelId, messageId))) throw new Error("Mensagem não encontrada.");
    await this.publishEvent(messageId, { action: pinned ? "pin" : "unpin" });
  }

  setTyping(active: boolean): void {
    if (!this.channelId || !this.identity || !this.transport || this.status !== "ready") return;
    const now = Date.now();
    if (active === this.localTypingActive && (!active || now - this.lastTypingSentAt < 1_500)) return;
    this.localTypingActive = active;
    this.lastTypingSentAt = now;
    const wire = JSON.stringify({
      version: 3,
      type: "chat.typing",
      channelId: this.channelId,
      actorPeerId: this.identity.peerId,
      active,
      timestamp: now,
    });
    this.sendToCapablePeers(wire, "typing-indicator-v1");
  }

  async sendAttachment(file: File): Promise<void> {
    if (!this.attachments.isConnected() || this.status !== "ready") throw new Error("Conecte e autentique o chat P2P antes de enviar arquivos.");
    await this.attachments.send(file);
  }

  async requestAttachment(record: StoredAttachmentRecord): Promise<void> {
    await this.attachments.request(record);
  }

  async downloadAttachment(record: StoredAttachmentRecord): Promise<void> {
    await this.attachments.download(record);
  }

  async attachmentBlob(record: StoredAttachmentRecord): Promise<Blob> {
    return this.attachments.blob(record);
  }

  async pauseAttachment(record: StoredAttachmentRecord): Promise<void> { await this.attachments.pause(record); }
  async resumeAttachment(record: StoredAttachmentRecord): Promise<void> { await this.attachments.resume(record); }
  async cancelAttachment(record: StoredAttachmentRecord): Promise<void> { await this.attachments.cancel(record); }

  localPeerId(): string | undefined { return this.peerId; }
  onMessage(callback: (message: LocalChatMessage, change: ChatMessageChange) => void): () => void { return this.messages.onMessage(callback); }
  onTyping(callback: (participants: ChatTypingParticipant[]) => void): () => void {
    this.typingCallbacks.add(callback);
    callback(this.typingParticipants());
    return () => this.typingCallbacks.delete(callback);
  }
  onStatus(callback: (status: ChatConnectionStatus) => void): () => void {
    this.statusCallbacks.add(callback);
    callback(this.status);
    return () => this.statusCallbacks.delete(callback);
  }
  onAttachment(callback: (record: StoredAttachmentRecord) => void): () => void { return this.attachments.onAttachment(callback); }
  onAttachmentProgress(callback: (progress: AttachmentRuntimeState) => void): () => void { return this.attachments.onProgress(callback); }

  private async receiveData(remotePeerId: string, raw: string): Promise<void> {
    if (this.identity) {
      const compatibility = this.sync.rejectIncompatibleEnvelope(raw);
      if (compatibility.incompatible) {
        this.rejectIncompatiblePeer(remotePeerId, compatibility.remoteVersion);
        return;
      }
    }
    if (this.openDataPeers.has(remotePeerId) && this.attachments.isConnected()) {
      try {
        if (await this.attachments.handleControl(remotePeerId, raw)) return;
      } catch (error) {
        console.warn("Controle de anexo/sync rejeitado", error);
        return;
      }
    }
    await this.receive(remotePeerId, raw);
  }

  private bindSignaling(signaling: SignalingProvider, peerId: string): void {
    this.unsubscribers.push(
      signaling.onPeerJoined((peer) => {
        if (this.identity && peer.clientVersion && !compatibleAppVersion(peer.clientVersion)) {
          this.rejectIncompatiblePeer(peer.peerId, peer.clientVersion);
          return;
        }
        if (!this.isTrustedRemote(peer.peerId)) {
          if (this.groupId) {
            void this.refreshGroupMembership(true).then(() => {
              if (this.isTrustedRemote(peer.peerId)) {
                void this.connectTrustedPeer(peer.peerId, peerId);
              } else if (this.openDataPeers.size === 0) {
                console.warn("Peer presente no chat não corresponde a uma identidade confiável do grupo", {
                  remotePeerId: peer.peerId,
                  localPeerId: this.peerId,
                  channelId: this.channelId,
                  trustedPeerIds: this.groups.trustedPeerIds(),
                });
              }
            });
          }
          return;
        }
        void this.connectTrustedPeer(peer.peerId, peerId);
      }),
      signaling.onPeerLeft((remotePeerId) => {
        if (!this.isTrustedRemote(remotePeerId)) return;
        this.forgetPeerConnection(remotePeerId);
        if (!this.identity) this.groups.removeDisplayName(remotePeerId);
        void this.transport?.disconnect(remotePeerId);
      }),
      signaling.onOffer((message) => {
        if (!this.isTrustedRemote(message.fromPeerId)) return;
        void this.transport?.acceptOffer(message.fromPeerId, message.payload.sdp).catch(() => this.setStatus(this.openDataPeers.size ? "ready" : "connected"));
      }),
      signaling.onAnswer((message) => {
        if (!this.isTrustedRemote(message.fromPeerId)) return;
        void this.transport?.acceptAnswer(message.fromPeerId, message.payload.sdp).catch(() => this.setStatus(this.openDataPeers.size ? "ready" : "connected"));
      }),
      signaling.onIceCandidate((message) => {
        if (!this.isTrustedRemote(message.fromPeerId)) return;
        void this.transport?.addIceCandidate(message.fromPeerId, message.payload.candidate).catch(() => undefined);
      }),
      signaling.onStatusChange((status) => {
        if (status === "connected" && this.openDataPeers.size === 0) {
          this.setStatus("connected");
          if (this.sessionToken) this.armReadyTimeout(this.sessionToken);
          void (this.groupId ? this.refreshGroupMembership(false) : this.connectPresentTrustedPeers());
        }
        if (status === "reconnecting" && this.openDataPeers.size === 0) this.setStatus("connecting");
        if (status === "error" && this.openDataPeers.size === 0) this.setStatus("error");
      }),
    );
  }

  private async connectTrustedPeer(remotePeerId: string, localPeerId = this.peerId): Promise<void> {
    if (!localPeerId || !this.transport || !this.isTrustedRemote(remotePeerId)) return;
    await this.transport.connect(remotePeerId, localPeerId < remotePeerId).catch((error) => {
      console.warn("Falha ao conectar peer confiável do chat", { remotePeerId, error });
      this.setStatus(this.openDataPeers.size ? "ready" : "connected");
      if (this.sessionToken && this.openDataPeers.size === 0) this.armReadyTimeout(this.sessionToken);
    });
  }

  private isTrustedRemote(remotePeerId: string): boolean {
    return this.groups.isTrustedRemote(remotePeerId, this.peerId, Boolean(this.identity));
  }

  private forgetPeerConnection(remotePeerId: string): void {
    this.updateTypingPeer(remotePeerId, false);
    this.sync.forgetPeer(remotePeerId);
    this.dataChannelPeers.delete(remotePeerId);
    this.openDataPeers.delete(remotePeerId);
    this.revocationOnlyPeers.delete(remotePeerId);
    this.historySync.forgetPeer(remotePeerId);
    this.attachments.forgetPeer(remotePeerId);
    this.setStatus(this.openDataPeers.size > 0 ? "ready" : "connected");
    if (this.sessionToken && this.openDataPeers.size === 0) this.armReadyTimeout(this.sessionToken);
  }

  private rejectIncompatiblePeer(remotePeerId: string, remoteVersion?: string): void {
    console.warn(incompatiblePeerMessage(remoteVersion), { remotePeerId });
    this.forgetPeerConnection(remotePeerId);
    if (this.openDataPeers.size === 0) {
      this.clearReadyTimeout();
      this.setStatus("incompatible");
    }
    void this.transport?.disconnect(remotePeerId);
  }

  private markRevokedPeerReady(remotePeerId: string): void {
    if (!this.dataChannelPeers.has(remotePeerId) || this.revocationOnlyPeers.has(remotePeerId)) return;
    this.sync.forgetPeer(remotePeerId);
    this.openDataPeers.delete(remotePeerId);
    this.revocationOnlyPeers.add(remotePeerId);
    this.clearReadyTimeout();
    void this.sendRevocations(remotePeerId);
  }

  private markPeerReady(remotePeerId: string): void {
    if (!this.dataChannelPeers.has(remotePeerId) || this.openDataPeers.has(remotePeerId)) return;
    if (this.groups.isRevoked(remotePeerId)) {
      this.markRevokedPeerReady(remotePeerId);
      return;
    }
    const sessionToken = this.sessionToken;
    if (!sessionToken) return;
    const attachmentGeneration = this.attachments.currentGeneration();
    this.openDataPeers.add(remotePeerId);
    this.clearReadyTimeout();
    this.setStatus("ready");
    void (async () => {
      await this.sendProfileUpdate(remotePeerId);
      if (!this.isSessionPeerActive(sessionToken, remotePeerId)) return;
      await this.sendGroupMembership(remotePeerId);
      this.requestGroupMembership(remotePeerId);
      if (this.membershipOnly) return;
      if (!this.isSessionPeerActive(sessionToken, remotePeerId)) return;
      await this.flushOutbox(remotePeerId);
      if (!this.isSessionPeerActive(sessionToken, remotePeerId)) return;
      await this.requestHistory(remotePeerId);
      if (!this.isSessionPeerActive(sessionToken, remotePeerId)) return;
      await this.attachments.peerReady(remotePeerId, attachmentGeneration);
    })().catch((error) => {
      if (this.sessionToken !== sessionToken) return;
      console.warn("Falha ao preparar peer autenticado do chat", { remotePeerId, error });
      if (this.openDataPeers.size === 0) this.setStatus("error");
    });
  }

  private isSessionPeerActive(sessionToken: object, remotePeerId: string): boolean {
    return this.sessionToken === sessionToken && Boolean(this.transport) && this.openDataPeers.has(remotePeerId);
  }

  private armReadyTimeout(sessionToken: object): void {
    this.clearReadyTimeout();
    if (this.openDataPeers.size > 0 || this.sessionToken !== sessionToken) return;
    this.readyTimer = setTimeout(() => {
      this.readyTimer = undefined;
      if (this.sessionToken !== sessionToken || this.openDataPeers.size > 0) return;
      const diagnostics = this.signaling?.getDiagnostics();
      console.warn("Tempo esgotado aguardando chat P2P ficar pronto", {
        channelId: this.channelId,
        localPeerId: this.peerId,
        presencePeers: diagnostics?.presencePeers ?? [],
        trustedPeerIds: this.groups.trustedPeerIds(),
        dataChannelPeers: [...this.dataChannelPeers],
        failedIdentityPeers: this.sync.failedPeerIds(),
      });
      // Estar sozinho não é uma falha: Presence e signaling permanecem ativos e
      // o chat pode ficar pronto assim que outro participante entrar. Reservamos
      // `error` para falhas reais de signaling/negociação.
      this.setStatus("connected");
    }, CHAT_READY_TIMEOUT_MS);
  }

  private clearReadyTimeout(): void {
    if (this.readyTimer) clearTimeout(this.readyTimer);
    this.readyTimer = undefined;
  }

  private async receive(remotePeerId: string, raw: string): Promise<void> {
    if (!this.isTrustedRemote(remotePeerId)) return;
    const envelope = parseChatWireEnvelope(raw, this.channelId);
    if (!envelope) return;

    if (envelope.type === "chat.identity.challenge") {
      if (!this.identity) return;
      await this.sync.respond(remotePeerId, envelope);
      return;
    }
    if (envelope.type === "chat.identity.proof") {
      if (!this.identity) return;
      await this.sync.accept(remotePeerId, envelope);
      return;
    }
    if (envelope.type === "chat.group.revocation") {
      if (!this.groupId || envelope.certificate.groupId !== this.groupId) return;
      if (await applyGroupRevocationCertificate(envelope.certificate)) {
        if (envelope.certificate.targetPeerId === this.identity?.peerId) await this.disconnect();
        else await this.refreshGroupMembership(false).catch(() => undefined);
      }
      return;
    }
    if (this.identity && !this.openDataPeers.has(remotePeerId) && !this.revocationOnlyPeers.has(remotePeerId)) return;
    // Uma identidade revogada só pode concluir o handshake e receber o
    // certificado assinado que explica sua remoção. Todo o restante é negado.
    if (this.revocationOnlyPeers.has(remotePeerId)) return;

    if (envelope.type === "chat.message.ack") {
      if (this.channelId) await this.outbox.acknowledge(this.channelId, envelope.messageId);
      return;
    }

    if (envelope.type === "chat.typing") {
      if (envelope.actorPeerId !== remotePeerId || !this.sync.supports(remotePeerId, "typing-indicator-v1")) return;
      this.updateTypingPeer(remotePeerId, envelope.active);
      return;
    }

    if (envelope.type === "chat.event") {
      if (envelope.actorPeerId !== remotePeerId || !this.sync.supports(remotePeerId, "chat-events-v1")) return;
      if (this.messages.hasProcessedEvent(envelope.id)) {
        this.sendMessageAck(remotePeerId, envelope.id);
        return;
      }
      if (!(await this.groups.verifyCanonical(remotePeerId, envelope.signature, canonicalChatEvent(envelope)))) return;
      await this.messages.persistEvent(envelope);
      this.sendMessageAck(remotePeerId, envelope.id);
      return;
    }

    if (envelope.type === "chat.message") {
      if (envelope.version === 2) {
        if (this.messages.hasProcessed(envelope.id)) {
          this.sendMessageAck(remotePeerId, envelope.id);
          return;
        }
        if (!this.identity || envelope.authorPeerId !== remotePeerId || !(await this.groups.verifySignedMessage(envelope))) return;
        await this.messages.persistSigned(envelope);
        this.sendMessageAck(remotePeerId, envelope.id);
        return;
      }
      if (this.messages.hasProcessed(envelope.id)) return;
      if (this.identity) return;
      const trustedDisplayName = this.groups.displayName(remotePeerId) ?? `Peer ${remotePeerId.slice(0, 6)}`;
      await this.messages.persistLegacy(envelope, trustedDisplayName);
      return;
    }

    if (!this.identity) return;
    if (envelope.type === "chat.members.snapshot") {
      if (!this.groupId) return;
      this.membershipStats.received += 1;
      const accepted = await this.groups.acceptMembership(this.groupId, this.identity, remotePeerId, envelope);
      if (!accepted) { this.membershipStats.rejected += 1; return; }
      this.membershipStats.accepted += 1;
      this.membershipStats.lastAcceptedAt = Date.now();
      this.installGroupPeers(accepted.merged.members ?? [], accepted.merged.removedMembers ?? [], accepted.merged.revocations ?? []);
      await this.connectPresentTrustedPeers();
      if (accepted.shouldReply) await this.sendGroupMembership(remotePeerId);
      return;
    }
    if (envelope.type === "chat.profile.update") {
      await this.groups.acceptProfile(remotePeerId, envelope);
      return;
    }
    if (envelope.type === "chat.members.request") {
      await this.sendGroupMembership(remotePeerId);
      return;
    }
    if (envelope.type === "chat.history.request") {
      await this.sendGroupMembership(remotePeerId);
      await this.respondHistory(remotePeerId, envelope);
      return;
    }
    if (envelope.type === "chat.history.chunk") {
      await this.historySync.acceptChunk(remotePeerId, envelope);
      return;
    }
    if (envelope.type === "chat.history.complete") {
      this.historySync.acceptComplete(remotePeerId, envelope);
    }
  }

  private async requestHistory(remotePeerId: string): Promise<void> {
    const sessionToken = this.sessionToken;
    const transport = this.transport;
    const channelId = this.channelId;
    if (!sessionToken || !this.identity || !channelId || !transport || !this.openDataPeers.has(remotePeerId)) return;
    await this.historySync.request({
      channelId,
      remotePeerId,
      send: (wire, peerId) => transport.sendData(wire, peerId),
      isActive: () => this.isSessionPeerActive(sessionToken, remotePeerId) && this.transport === transport,
      includeEvents: this.sync.supports(remotePeerId, "chat-events-v1"),
    });
  }

  private async respondHistory(remotePeerId: string, request: HistoryRequestWireMessage): Promise<void> {
    const sessionToken = this.sessionToken;
    const transport = this.transport;
    const channelId = this.channelId;
    if (!sessionToken || !channelId || !transport || !this.openDataPeers.has(remotePeerId)) return;
    await this.historySync.respond({
      channelId,
      remotePeerId,
      send: (wire, peerId) => transport.sendData(wire, peerId),
      isActive: () => this.isSessionPeerActive(sessionToken, remotePeerId) && this.transport === transport,
      includeEvents: this.sync.supports(remotePeerId, "chat-events-v1"),
    }, request);
  }

  private async refreshGroupMembership(broadcast: boolean): Promise<void> {
    const sessionToken = this.sessionToken;
    const groupId = this.groupId;
    const identity = this.identity;
    if (!sessionToken || !groupId || !identity) return;
    if (broadcast) this.pendingMemberRefreshBroadcast = true;
    if (this.refreshingMembers) return this.refreshingMembers;
    let task: Promise<void>;
    task = (async () => {
      do {
        const shouldBroadcast = this.pendingMemberRefreshBroadcast;
        this.pendingMemberRefreshBroadcast = false;
        const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
        if (!group || this.sessionToken !== sessionToken || this.groupId !== groupId || this.identity !== identity) return;
        const channelId = this.channelId;
        const nextRendezvousId = channelId ? groupRendezvousId(group, "chat", channelId) : undefined;
        const rendezvousChanged = Boolean(nextRendezvousId && this.rendezvousId && nextRendezvousId !== this.rendezvousId);
        this.installGroupPeers(group.members ?? [], group.removedMembers ?? [], group.revocations ?? []);
        await this.connectPresentTrustedPeers();
        if (this.sessionToken !== sessionToken) return;
        if (shouldBroadcast) await Promise.all([...this.openDataPeers].map((peerId) => this.sendGroupMembership(peerId)));
        if (this.sessionToken !== sessionToken) return;
        const signaling = this.signaling;
        const peerId = this.peerId;
        if (rendezvousChanged && nextRendezvousId && signaling && peerId) {
          this.rendezvousId = nextRendezvousId;
          this.setStatus("connecting");
          await signaling.connect(nextRendezvousId, peerId, this.signalingNamespace);
          if (this.sessionToken !== sessionToken || this.signaling !== signaling) return;
          this.setStatus("connected");
          await this.connectPresentTrustedPeers();
        }
      } while (this.pendingMemberRefreshBroadcast && this.sessionToken === sessionToken);
    })().finally(() => {
      if (this.refreshingMembers === task) this.refreshingMembers = undefined;
    });
    this.refreshingMembers = task;
    return task;
  }

  private installGroupPeers(activePeers: PublicPeerIdentity[], revokedPeers: PublicPeerIdentity[], revocations: GroupRevocationCertificate[]): void {
    const transitions = this.groups.install(activePeers, revokedPeers, revocations, [...this.dataChannelPeers]);
    for (const remotePeerId of transitions.revokedPeerIds) {
      this.openDataPeers.delete(remotePeerId);
      this.attachments.forgetPeer(remotePeerId);
      this.markRevokedPeerReady(remotePeerId);
    }
    for (const remotePeerId of transitions.unknownPeerIds) {
      this.forgetPeerConnection(remotePeerId);
      void this.transport?.disconnect(remotePeerId);
    }
  }

  private async connectPresentTrustedPeers(): Promise<void> {
    if (!this.signaling || !this.peerId) return;
    const present = this.signaling.getDiagnostics().presencePeers;
    await Promise.all(present.filter((peerId) => this.isTrustedRemote(peerId)).map((peerId) => this.connectTrustedPeer(peerId, this.peerId)));
  }

  /** Estado da sincronização de membros, para o botão "Recarregar lista" explicar o que aconteceu. */
  async getMembershipReport(): Promise<MembershipReport> {
    const group = this.groupId ? (await loadLocalGroups()).find((item) => item.groupId === this.groupId) : undefined;
    const authorized = new Set([group?.ownerPeerId, ...(group?.administratorPeerIds ?? [])].filter((id): id is string => Boolean(id)));
    const openPeerIds = [...this.openDataPeers];
    return {
      status: this.status,
      openPeers: openPeerIds.length,
      authorizedOnline: openPeerIds.some((peerId) => authorized.has(peerId)),
      localIsAuthorized: Boolean(this.identity && authorized.has(this.identity.peerId)),
      received: this.membershipStats.received,
      accepted: this.membershipStats.accepted,
      rejected: this.membershipStats.rejected,
      lastAcceptedAt: this.membershipStats.lastAcceptedAt,
      members: group?.members.length ?? 0,
    };
  }

  /** Pede a lista de membros a um peer (quem é dono/admin responde com o snapshot assinado). */
  private requestGroupMembership(remotePeerId: string): void {
    const channelId = this.channelId;
    const transport = this.transport;
    if (!this.groupId || !channelId || !transport || !this.openDataPeers.has(remotePeerId)) return;
    const message: MembersRequestWireMessage = { version: 2, type: "chat.members.request", channelId, requestId: crypto.randomUUID() };
    transport.sendData(JSON.stringify(message), remotePeerId);
  }

  /** Reenvia a lista (se for dono/admin) e pede a lista a todos os peers conectados. */
  private async resyncGroupMembership(): Promise<void> {
    await this.refreshGroupMembership(true).catch(() => undefined);
    for (const peerId of [...this.openDataPeers]) this.requestGroupMembership(peerId);
  }

  private async sendGroupMembership(remotePeerId: string): Promise<void> {
    const sessionToken = this.sessionToken;
    const groupId = this.groupId;
    const identity = this.identity;
    const channelId = this.channelId;
    const transport = this.transport;
    if (!sessionToken || !groupId || !identity || !channelId || !transport || !this.openDataPeers.has(remotePeerId)) return;
    const message = await this.groups.createMembershipSnapshot(groupId, channelId, identity);
    if (this.sessionToken !== sessionToken || this.transport !== transport || !this.openDataPeers.has(remotePeerId)) return;
    if (message) transport.sendData(JSON.stringify(message), remotePeerId);
  }

  private async sendRevocations(remotePeerId: string): Promise<void> {
    if (!this.channelId || !this.transport || !this.revocationOnlyPeers.has(remotePeerId)) return;
    for (const certificate of this.groups.revocationsFor(remotePeerId)) {
      const message: GroupRevocationWireMessage = {
        version: 2,
        type: "chat.group.revocation",
        channelId: this.channelId,
        certificate,
      };
      this.transport.sendData(JSON.stringify(message), remotePeerId);
    }
  }

  private async sendProfileUpdate(remotePeerId: string): Promise<void> {
    const sessionToken = this.sessionToken;
    const identity = this.identity;
    const channelId = this.channelId;
    const transport = this.transport;
    if (!sessionToken || !identity || !channelId || !transport || !this.openDataPeers.has(remotePeerId)) return;
    const message = await this.groups.createProfileUpdate(identity, channelId);
    if (this.sessionToken !== sessionToken || this.transport !== transport || !this.openDataPeers.has(remotePeerId)) return;
    transport.sendData(JSON.stringify(message), remotePeerId);
  }

  private sendToAuthenticatedPeers(data: string): number {
    if (!this.transport) return 0;
    let sent = 0;
    for (const peerId of this.openDataPeers) sent += this.transport.sendData(data, peerId);
    return sent;
  }

  private sendToCapablePeers(data: string, capability: "chat-events-v1" | "typing-indicator-v1"): number {
    if (!this.transport) return 0;
    let sent = 0;
    for (const peerId of this.openDataPeers) {
      if (this.sync.supports(peerId, capability)) sent += this.transport.sendData(data, peerId);
    }
    return sent;
  }

  private async flushOutbox(remotePeerId: string): Promise<void> {
    const sessionToken = this.sessionToken;
    const channelId = this.channelId;
    const transport = this.transport;
    if (!sessionToken || !channelId || !transport || !this.openDataPeers.has(remotePeerId)) return;
    await this.outbox.flush(
      channelId,
      remotePeerId,
      (wire, peerId) => transport.sendData(wire, peerId),
      () => this.isSessionPeerActive(sessionToken, remotePeerId) && this.transport === transport,
      (envelope) => envelope.type !== "chat.event" || this.sync.supports(remotePeerId, "chat-events-v1"),
    );
  }

  private sendMessageAck(remotePeerId: string, messageId: string): void {
    if (!this.channelId || !this.transport || !this.openDataPeers.has(remotePeerId)) return;
    const ack = this.outbox.createAck(this.channelId, messageId);
    this.transport.sendData(JSON.stringify(ack), remotePeerId);
  }

  private async publishEvent(targetMessageId: string, input: ChatEventInput): Promise<SignedChatEventWireMessage> {
    if (!this.channelId || !this.identity) throw new Error("Conecte e autentique o chat P2P primeiro.");
    const event = await this.messages.createEvent(this.identity, this.channelId, targetMessageId, input);
    await this.outbox.enqueue(event);
    this.sendToCapablePeers(JSON.stringify(event), "chat-events-v1");
    await this.messages.persistEvent(event);
    return event;
  }

  private async requireOwnMessage(messageId: string): Promise<LocalChatMessage> {
    if (!this.channelId || !this.identity) throw new Error("Conecte e autentique o chat P2P primeiro.");
    const message = await this.messages.find(this.channelId, messageId);
    if (!message) throw new Error("Mensagem não encontrada.");
    if (message.authorPeerId !== this.identity.peerId) throw new Error("Você só pode alterar suas próprias mensagens.");
    return message;
  }

  private updateTypingPeer(remotePeerId: string, active: boolean): void {
    const previous = this.typingPeers.get(remotePeerId);
    if (previous) clearTimeout(previous.timer);
    this.typingPeers.delete(remotePeerId);
    if (active) {
      const timer = setTimeout(() => {
        this.typingPeers.delete(remotePeerId);
        this.emitTyping();
      }, 5_000);
      this.typingPeers.set(remotePeerId, {
        displayName: this.groups.displayName(remotePeerId) ?? `Peer ${remotePeerId.slice(0, 6)}`,
        timer,
      });
    }
    this.emitTyping();
  }

  private typingParticipants(): ChatTypingParticipant[] {
    return [...this.typingPeers].map(([peerId, value]) => ({ peerId, displayName: value.displayName }));
  }

  private emitTyping(): void {
    const participants = this.typingParticipants();
    this.typingCallbacks.forEach((callback) => callback(participants));
  }

  private setStatus(status: ChatConnectionStatus): void { if (this.status === status) return; this.status = status; this.statusCallbacks.forEach((callback) => callback(status)); }
}
