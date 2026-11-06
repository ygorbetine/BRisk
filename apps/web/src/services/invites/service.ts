import { MeshWebRTCTransport, type TransportEvents } from "@briks/rtc";
import { SupabaseSignalingProvider } from "../supabase/signaling";
import type { SignalingProvider } from "../signaling/types";
import {
  addLocalGroupMember,
  loadLocalGroups,
  mergeLocalGroupManifest,
  publicIdentity,
  saveLocalFriend,
  saveLocalGroup,
  type GroupInviteMetadata,
  type LocalIdentity,
  type PublicPeerIdentity,
} from "../offline/social-storage";
import {
  deriveInviteRendezvousId,
  generateBriksInviteCode,
  InviteAttemptLimiter,
  normalizeBriksInviteCode,
  type InviteType,
  validateBriksInviteCode,
} from "./code";
import {
  createSignedInviteMessage,
  parseAndVerifyInviteMessage,
  type SignedInviteMessage,
} from "./protocol";

export const DEFAULT_INVITE_TTL_MS = 10 * 60 * 1000;
const CANDIDATE_CONNECT_TIMEOUT_MS = 20_000;
const DECISION_ACK_TIMEOUT_MS = 15_000;
const FINAL_ACK_GRACE_MS = 400;
const CREATOR_RETRY_DELAY_MS = 1_000;
const sharedAttemptLimiter = new InviteAttemptLimiter();

export type InviteStatus =
  | "idle"
  | "waiting"
  | "connecting"
  | "connected"
  | "approval"
  | "confirming"
  | "accepted"
  | "rejected"
  | "expired"
  | "cancelled"
  | "error";

export type InviteSnapshot = {
  type: InviteType;
  role: "creator" | "joiner";
  code: string;
  createdAt: number;
  expiresAt: number;
  status: InviteStatus;
  message: string;
  remoteIdentity?: PublicPeerIdentity;
};

export type IncomingInviteRequest = {
  requestId: string;
  identity: PublicPeerIdentity;
  type: InviteType;
};

export type InviteTransport = {
  connect(peerId: string, initiator: boolean): Promise<void>;
  acceptOffer(peerId: string, description: RTCSessionDescriptionInit): Promise<void>;
  acceptAnswer(peerId: string, description: RTCSessionDescriptionInit): Promise<void>;
  addIceCandidate(peerId: string, candidate: RTCIceCandidateInit): Promise<void>;
  sendData(data: string, targetPeerId?: string): number;
  disconnect(peerId?: string): Promise<void>;
};

export type InviteDependencies = {
  createSignaling(): SignalingProvider;
  createTransport(
    peerId: string,
    iceServers: RTCIceServer[],
    events: TransportEvents,
  ): InviteTransport | Promise<InviteTransport>;
  now(): number;
  setTimer(callback: () => void, delay: number): ReturnType<typeof setTimeout>;
  clearTimer(timer: ReturnType<typeof setTimeout>): void;
};

const defaults: InviteDependencies = {
  createSignaling: () => new SupabaseSignalingProvider(),
  createTransport: (peerId, iceServers, events) =>
    new MeshWebRTCTransport(peerId, iceServers, events),
  now: () => Date.now(),
  setTimer: (callback, delay) => setTimeout(callback, delay),
  clearTimer: (timer) => clearTimeout(timer),
};

export class InviteService {
  private signaling?: SignalingProvider;
  private transport?: InviteTransport;
  private snapshot?: InviteSnapshot;
  private localPeerId?: string;
  private candidatePeerId?: string;
  private request?: IncomingInviteRequest;
  private requestId?: string;
  private group?: GroupInviteMetadata;
  private membershipCommittedRequestId?: string;
  private pendingDecision?: "accept" | "reject";
  private expiryTimer?: ReturnType<typeof setTimeout>;
  private availabilityTimer?: ReturnType<typeof setTimeout>;
  private candidateTimer?: ReturnType<typeof setTimeout>;
  private decisionTimer?: ReturnType<typeof setTimeout>;
  private creatorRetryTimer?: ReturnType<typeof setTimeout>;
  private unsubscribers: Array<() => void> = [];
  private readonly stateListeners = new Set<(snapshot: InviteSnapshot) => void>();
  private readonly requestListeners = new Set<(request: IncomingInviteRequest) => void>();
  private cleaned = false;

  constructor(
    private readonly identity: LocalIdentity,
    private readonly iceServers: RTCIceServer[],
    private readonly dependencies: InviteDependencies = defaults,
  ) {}

  onState(callback: (snapshot: InviteSnapshot) => void): () => void {
    this.stateListeners.add(callback);
    if (this.snapshot) callback(this.snapshot);
    return () => this.stateListeners.delete(callback);
  }

  onRequest(callback: (request: IncomingInviteRequest) => void): () => void {
    this.requestListeners.add(callback);
    if (this.request) callback(this.request);
    return () => this.requestListeners.delete(callback);
  }

  get state(): InviteSnapshot | undefined {
    return this.snapshot;
  }

  async createInvite(
    type: InviteType,
    group?: GroupInviteMetadata,
    ttlMs = DEFAULT_INVITE_TTL_MS,
  ): Promise<InviteSnapshot> {
    await this.cancel(false);
    this.cleaned = false;
    if (type === "group" && !group) {
      throw new Error("Selecione um grupo para criar o convite.");
    }
    this.group = group;
    return this.start(type, "creator", generateBriksInviteCode(), ttlMs);
  }

  async joinInvite(
    type: InviteType,
    input: string,
    ttlMs = DEFAULT_INVITE_TTL_MS,
  ): Promise<InviteSnapshot> {
    if (!sharedAttemptLimiter.consume()) {
      throw new Error("Muitas tentativas. Aguarde um minuto antes de tentar novamente.");
    }
    const code = normalizeBriksInviteCode(input);
    if (!validateBriksInviteCode(code)) throw new Error("Digite um código Briks válido.");
    await this.cancel(false);
    this.cleaned = false;
    return this.start(type, "joiner", code, ttlMs);
  }

  async accept(): Promise<void> {
    if (this.snapshot?.role !== "creator" || !this.request || !this.candidatePeerId) {
      throw new Error("Não há solicitação aguardando aprovação.");
    }
    await this.sendDecision("accept");
  }

  async reject(): Promise<void> {
    if (this.snapshot?.role !== "creator" || !this.request || !this.candidatePeerId) return;
    await this.sendDecision("reject");
  }

  async cancel(markCancelled = true): Promise<void> {
    if (
      markCancelled
      && this.snapshot
      && !isTerminalStatus(this.snapshot.status)
    ) {
      this.update("cancelled", "Convite cancelado");
    }
    await this.cleanup();
  }

  private async start(
    type: InviteType,
    role: "creator" | "joiner",
    code: string,
    ttlMs: number,
  ): Promise<InviteSnapshot> {
    const now = this.dependencies.now();
    this.snapshot = {
      type,
      role,
      code,
      createdAt: now,
      expiresAt: now + ttlMs,
      status: role === "creator" ? "waiting" : "connecting",
      message: role === "creator" ? "Aguardando alguém entrar…" : "Procurando convite…",
    };
    this.pendingDecision = undefined;
    this.membershipCommittedRequestId = undefined;
    this.request = undefined;
    this.requestId = undefined;
    this.emitState();
    try {
      this.localPeerId = crypto.randomUUID();
      this.signaling = this.dependencies.createSignaling();
      this.transport = await this.dependencies.createTransport(
        this.localPeerId,
        this.iceServers,
        this.transportEvents(),
      );
      this.bindSignaling();
      const rendezvous = await deriveInviteRendezvousId(type, code);
      await this.signaling.connect(rendezvous, this.localPeerId, type);

      this.expiryTimer = this.dependencies.setTimer(() => {
        if (!this.snapshot || isTerminalStatus(this.snapshot.status)) return;
        this.update("expired", "Convite expirado");
        void this.cleanup();
      }, ttlMs);

      if (role === "joiner") {
        this.availabilityTimer = this.dependencies.setTimer(() => {
          if (!this.candidatePeerId && this.snapshot?.status === "connecting") {
            this.update("error", "Convite não encontrado, expirado ou o criador está offline.");
            void this.cleanup();
          }
        }, Math.min(12_000, ttlMs));
      }

      this.reconcilePresentCandidates();
      return this.snapshot;
    } catch (error) {
      this.update("error", "Não foi possível iniciar o serviço de convites.");
      await this.cleanup();
      throw error;
    }
  }

  private bindSignaling(): void {
    const signaling = this.signaling!;
    this.unsubscribers.push(
      signaling.onPeerJoined((peer) => {
        this.considerCandidate(peer.peerId);
      }),
      signaling.onPeerLeft((peerId) => {
        if (
          peerId === this.candidatePeerId
          && this.snapshot
          && !isTerminalStatus(this.snapshot.status)
        ) {
          void this.failCandidate(peerId);
        }
      }),
      signaling.onOffer((message) => {
        if (this.acceptCandidate(message.fromPeerId)) {
          void this.transport!
            .acceptOffer(message.fromPeerId, message.payload.sdp)
            .catch(() => this.failCandidate(message.fromPeerId));
        }
      }),
      signaling.onAnswer((message) => {
        if (this.acceptCandidate(message.fromPeerId)) {
          void this.transport!
            .acceptAnswer(message.fromPeerId, message.payload.sdp)
            .catch(() => this.failCandidate(message.fromPeerId));
        }
      }),
      signaling.onIceCandidate((message) => {
        if (this.acceptCandidate(message.fromPeerId)) {
          void this.transport!
            .addIceCandidate(message.fromPeerId, message.payload.candidate)
            .catch(() => undefined);
        }
      }),
    );
  }

  private transportEvents(): TransportEvents {
    return {
      sendOffer: (peerId, description) => this.signaling!.sendOffer(peerId, description),
      sendAnswer: (peerId, description) => this.signaling!.sendAnswer(peerId, description),
      sendIce: (peerId, candidate) => this.signaling!.sendIceCandidate(peerId, candidate),
      onRemoteStream: () => undefined,
      onConnectionState: (peerId, state) => {
        if ((state === "failed" || state === "closed") && this.snapshot && !isTerminalStatus(this.snapshot.status)) {
          void this.failCandidate(peerId);
        }
      },
      onDataState: (peerId, state) => {
        if (peerId !== this.candidatePeerId || !this.snapshot || isTerminalStatus(this.snapshot.status)) return;
        if (state === "open") {
          this.handleDataChannelOpen(peerId);
          return;
        }
        if (state === "closed") void this.failCandidate(peerId);
      },
      onDataMessage: (peerId, data) => {
        if (peerId === this.candidatePeerId) {
          void this.receive(data).catch((error: unknown) => {
            const message = error instanceof Error ? error.message : "Falha ao processar resposta do convite.";
            this.failCurrent(message);
          });
        }
      },
    };
  }

  private handleDataChannelOpen(peerId: string): void {
    if (!this.snapshot || peerId !== this.candidatePeerId || isTerminalStatus(this.snapshot.status)) return;
    this.clearCandidateTimeout();
    if (this.snapshot.role === "creator") {
      if (["waiting", "connecting"].includes(this.snapshot.status)) {
        this.update("connected", "Conexão P2P estabelecida");
      }
      return;
    }

    const requestId = this.requestId ?? crypto.randomUUID();
    this.requestId = requestId;
    if (this.snapshot.status === "connecting") {
      this.update("connected", "Conexão P2P estabelecida");
    }
    const type = this.snapshot.type === "friend" ? "friend.request" : "group.join.request";
    void this.send(type, requestId)
      .then(() => {
        if (!this.snapshot || this.snapshot.role !== "joiner" || this.requestId !== requestId || isTerminalStatus(this.snapshot.status)) return;
        this.update("approval", "Solicitação enviada. Aguardando aprovação…");
      })
      .catch(() => this.failCurrent("Não foi possível enviar a solicitação."));
  }

  private async receive(raw: string): Promise<void> {
    const message = await parseAndVerifyInviteMessage(raw, this.dependencies.now());
    if (!message || !this.snapshot || isTerminalStatus(this.snapshot.status)) return;
    const expectedRequest = this.snapshot.type === "friend" ? "friend.request" : "group.join.request";

    if (this.snapshot.role === "creator") {
      if (
        message.type === "invite.ack"
        && this.request
        && message.requestId === this.request.requestId
        && this.pendingDecision
      ) {
        const decision = this.pendingDecision;
        this.pendingDecision = undefined;
        this.clearDecisionTimeout();
        if (decision === "accept") {
          const remote = this.request.identity;
          if (this.snapshot.type === "friend") {
            await saveLocalFriend({ ...remote, addedAt: this.dependencies.now() });
          } else if (this.membershipCommittedRequestId !== message.requestId) {
            if (!this.group) throw new Error("Metadados do grupo indisponíveis.");
            await addLocalGroupMember(this.group, remote, publicIdentity(this.identity));
          }
          this.snapshot = { ...this.snapshot, remoteIdentity: remote };
          await this.finishWithStatus(
            "accepted",
            this.snapshot.type === "friend" ? "Amizade concluída" : "Entrada no grupo concluída",
          );
        } else {
          await this.finishWithStatus("rejected", "Solicitação recusada");
        }
        return;
      }

      if (message.type !== expectedRequest || this.pendingDecision) return;
      if (this.request) {
        if (this.request.requestId === message.requestId && this.request.identity.peerId === message.identity.peerId) return;
        return;
      }
      this.request = {
        requestId: message.requestId,
        identity: message.identity,
        type: this.snapshot.type,
      };
      this.snapshot = { ...this.snapshot, remoteIdentity: message.identity };
      this.update(
        "approval",
        this.snapshot.type === "friend"
          ? `${message.identity.displayName} quer adicionar você`
          : `${message.identity.displayName} quer entrar no grupo`,
      );
      this.requestListeners.forEach((callback) => callback(this.request!));
      return;
    }

    if (!this.requestId || message.requestId !== this.requestId) return;
    const accepted = this.snapshot.type === "friend"
      ? message.type === "friend.accept"
      : message.type === "group.join.accept";
    const rejected = this.snapshot.type === "friend"
      ? message.type === "friend.reject"
      : message.type === "group.join.reject";

    if (accepted) {
      if (this.snapshot.type === "friend") {
        await saveLocalFriend({ ...message.identity, addedAt: this.dependencies.now() });
      } else if (message.group) {
        const ownerIdentity = message.group.ownerIdentity ?? (message.identity.peerId === message.group.ownerPeerId ? message.identity : undefined);
        if (!ownerIdentity) throw new Error("A identidade do proprietário não veio no convite.");
        const membersByPeerId = new Map<string, PublicPeerIdentity>();
        // O snapshot pode conter um perfil antigo do proprietário ou do
        // administrador. As identidades autenticadas pelo convite prevalecem.
        for (const member of [...(message.group.members ?? []), ownerIdentity, message.identity, publicIdentity(this.identity)]) {
          membersByPeerId.set(member.peerId, member);
        }
        const members = [...membersByPeerId.values()];
        const incomingGroup = {
          ...message.group,
          members,
          joinedAt: this.dependencies.now(),
        };
        const existing = (await loadLocalGroups()).find((group) => group.groupId === incomingGroup.groupId);
        if (existing) {
          if (existing.ownerPeerId !== incomingGroup.ownerPeerId) {
            throw new Error("Já existe um grupo local com o mesmo identificador e outro proprietário.");
          }
          await mergeLocalGroupManifest({ ...incomingGroup, joinedAt: existing.joinedAt }, message.identity.peerId);
        } else {
          await saveLocalGroup(incomingGroup);
        }
      } else {
        return;
      }
      this.snapshot = { ...this.snapshot, remoteIdentity: message.identity };
      await this.sendAck(this.requestId);
      await this.finishWithStatus(
        "accepted",
        this.snapshot.type === "friend" ? "Amizade concluída" : "Entrada no grupo concluída",
      );
    } else if (rejected) {
      await this.sendAck(this.requestId);
      await this.finishWithStatus("rejected", "Solicitação recusada");
    }
  }

  private async sendDecision(decision: "accept" | "reject"): Promise<void> {
    if (this.snapshot?.role !== "creator" || !this.request || !this.candidatePeerId) {
      throw new Error("Não há solicitação aguardando aprovação.");
    }
    if (this.pendingDecision) throw new Error("Já existe uma decisão aguardando confirmação.");

    const type = decision === "accept"
      ? this.snapshot.type === "friend" ? "friend.accept" : "group.join.accept"
      : this.snapshot.type === "friend" ? "friend.reject" : "group.join.reject";
    const requestId = this.request.requestId;
    if (decision === "reject" && this.membershipCommittedRequestId === requestId) {
      throw new Error("Esta entrada já foi aprovada localmente. Reenvie o aceite para concluir no outro dispositivo.");
    }
    if (decision === "accept" && this.snapshot.type === "group" && this.membershipCommittedRequestId !== requestId) {
      if (!this.group) throw new Error("Metadados do grupo indisponíveis.");
      await addLocalGroupMember(this.group, this.request.identity, publicIdentity(this.identity));
      this.membershipCommittedRequestId = requestId;
      const updated = (await loadLocalGroups()).find((group) => group.groupId === this.group!.groupId);
      if (updated) this.group = updated;
    }
    this.pendingDecision = decision;
    this.update(
      "confirming",
      decision === "accept"
        ? "Aceite enviado. Aguardando confirmação do outro dispositivo…"
        : "Recusa enviada. Aguardando confirmação…",
    );
    this.armDecisionTimeout();

    try {
      await this.send(
        type,
        requestId,
        decision === "accept" && this.snapshot.type === "group" ? this.group : undefined,
      );
    } catch (error) {
      this.clearDecisionTimeout();
      this.pendingDecision = undefined;
      if (this.snapshot && !isTerminalStatus(this.snapshot.status)) {
        this.update("approval", "Não foi possível entregar a decisão. Tente novamente.");
      }
      throw error;
    }
  }

  private async send(
    type: SignedInviteMessage["type"],
    requestId: string,
    group?: GroupInviteMetadata,
  ): Promise<void> {
    if (!this.transport || !this.candidatePeerId) {
      throw new Error("Conexão P2P indisponível.");
    }
    const message = await createSignedInviteMessage(this.identity, {
      type,
      requestId,
      timestamp: this.dependencies.now(),
      group,
    });
    if (this.transport.sendData(JSON.stringify(message), this.candidatePeerId) !== 1) {
      throw new Error("DataChannel indisponível.");
    }
  }

  private async sendAck(requestId: string): Promise<void> {
    let lastError: unknown;
    for (let attempt = 0; attempt < 3; attempt += 1) {
      try {
        await this.send("invite.ack", requestId);
        return;
      } catch (error) {
        lastError = error;
        if (attempt < 2) await new Promise((resolve) => setTimeout(resolve, 150));
      }
    }
    throw lastError instanceof Error
      ? lastError
      : new Error("Não foi possível confirmar o convite.");
  }

  private considerCandidate(peerId: string): void {
    if (!this.snapshot || !this.transport || isTerminalStatus(this.snapshot.status)) return;
    if (this.candidatePeerId === peerId || this.candidatePeerId) return;
    if (this.snapshot.role === "creator" && !["waiting", "connecting"].includes(this.snapshot.status)) return;
    if (this.snapshot.role === "joiner" && this.snapshot.status !== "connecting") return;

    this.candidatePeerId = peerId;
    this.clearAvailabilityTimeout();
    this.update("connecting", "Negociando conexão P2P…");
    this.armCandidateTimeout(peerId);
    void this.transport
      .connect(peerId, this.snapshot.role === "creator")
      .catch(() => this.failCandidate(peerId));
  }

  private reconcilePresentCandidates(): void {
    if (!this.signaling || this.candidatePeerId || !this.snapshot || isTerminalStatus(this.snapshot.status)) return;
    const peerId = this.signaling.getDiagnostics().presencePeers[0];
    if (peerId) this.considerCandidate(peerId);
  }

  private armCandidateTimeout(peerId: string): void {
    this.clearCandidateTimeout();
    const remaining = Math.max(
      1,
      (this.snapshot?.expiresAt ?? this.dependencies.now()) - this.dependencies.now(),
    );
    this.candidateTimer = this.dependencies.setTimer(() => {
      if (
        this.candidatePeerId === peerId
        && this.snapshot
        && ["connecting", "waiting"].includes(this.snapshot.status)
      ) {
        void this.failCandidate(peerId);
      }
    }, Math.min(CANDIDATE_CONNECT_TIMEOUT_MS, remaining));
  }

  private clearCandidateTimeout(): void {
    if (this.candidateTimer) this.dependencies.clearTimer(this.candidateTimer);
    this.candidateTimer = undefined;
  }

  private clearAvailabilityTimeout(): void {
    if (this.availabilityTimer) this.dependencies.clearTimer(this.availabilityTimer);
    this.availabilityTimer = undefined;
  }

  private armDecisionTimeout(): void {
    this.clearDecisionTimeout();
    this.decisionTimer = this.dependencies.setTimer(() => {
      if (this.snapshot?.status !== "confirming") return;
      this.pendingDecision = undefined;
      if (this.request && this.candidatePeerId) {
        this.update("approval", "O outro dispositivo não confirmou. Você pode tentar novamente.");
      } else {
        this.failCurrent("A conexão foi perdida antes da confirmação.");
      }
    }, DECISION_ACK_TIMEOUT_MS);
  }

  private clearDecisionTimeout(): void {
    if (this.decisionTimer) this.dependencies.clearTimer(this.decisionTimer);
    this.decisionTimer = undefined;
  }

  private acceptCandidate(peerId: string): boolean {
    if (!this.snapshot || !this.transport || isTerminalStatus(this.snapshot.status)) return false;
    if (!this.candidatePeerId) {
      this.candidatePeerId = peerId;
      this.clearAvailabilityTimeout();
      if (["waiting", "connecting"].includes(this.snapshot.status)) {
        this.update("connecting", "Negociando conexão P2P…");
      }
      this.armCandidateTimeout(peerId);
    }
    return this.candidatePeerId === peerId;
  }

  private async failCandidate(peerId: string): Promise<void> {
    if (peerId !== this.candidatePeerId || !this.snapshot) return;
    if (isTerminalStatus(this.snapshot.status)) return;
    this.clearCandidateTimeout();
    this.clearDecisionTimeout();
    await this.transport?.disconnect(peerId).catch(() => undefined);
    this.candidatePeerId = undefined;
    this.request = undefined;
    this.requestId = undefined;
    this.pendingDecision = undefined;
    if (this.snapshot.role === "creator" && this.dependencies.now() < this.snapshot.expiresAt) {
      this.update("waiting", "Conexão perdida. Aguardando o participante reconectar…");
      this.scheduleCreatorRetry();
    } else {
      this.update("error", "Não foi possível estabelecer conexão.");
      await this.cleanup();
    }
  }

  private scheduleCreatorRetry(): void {
    if (this.creatorRetryTimer) this.dependencies.clearTimer(this.creatorRetryTimer);
    this.creatorRetryTimer = this.dependencies.setTimer(() => {
      this.creatorRetryTimer = undefined;
      this.reconcilePresentCandidates();
    }, CREATOR_RETRY_DELAY_MS);
  }

  private failCurrent(message: string): void {
    if (!this.snapshot || isTerminalStatus(this.snapshot.status)) return;
    this.update("error", message);
    void this.cleanup();
  }

  private update(status: InviteStatus, message: string): void {
    if (!this.snapshot) return;
    this.snapshot = { ...this.snapshot, status, message };
    this.emitState();
  }

  private emitState(): void {
    if (this.snapshot) this.stateListeners.forEach((callback) => callback(this.snapshot!));
  }

  private async finishWithStatus(status: "accepted" | "rejected", message: string): Promise<void> {
    this.clearDecisionTimeout();
    this.update(status, message);
    await new Promise((resolve) => setTimeout(resolve, FINAL_ACK_GRACE_MS));
    await this.cleanup();
    if (typeof window !== "undefined") {
      window.dispatchEvent(new Event("briks:social-updated"));
    }
  }

  private async cleanup(): Promise<void> {
    if (this.cleaned) return;
    this.cleaned = true;
    if (this.expiryTimer) this.dependencies.clearTimer(this.expiryTimer);
    if (this.availabilityTimer) this.dependencies.clearTimer(this.availabilityTimer);
    if (this.candidateTimer) this.dependencies.clearTimer(this.candidateTimer);
    if (this.decisionTimer) this.dependencies.clearTimer(this.decisionTimer);
    if (this.creatorRetryTimer) this.dependencies.clearTimer(this.creatorRetryTimer);
    this.expiryTimer = undefined;
    this.availabilityTimer = undefined;
    this.candidateTimer = undefined;
    this.decisionTimer = undefined;
    this.creatorRetryTimer = undefined;
    this.unsubscribers.splice(0).forEach((unsubscribe) => unsubscribe());
    await this.transport?.disconnect().catch(() => undefined);
    await this.signaling?.disconnect().catch(() => undefined);
    this.transport = undefined;
    this.signaling = undefined;
    this.localPeerId = undefined;
    this.candidatePeerId = undefined;
    this.request = undefined;
    this.requestId = undefined;
    this.pendingDecision = undefined;
    this.membershipCommittedRequestId = undefined;
  }
}

function isTerminalStatus(status: InviteStatus): boolean {
  return ["accepted", "rejected", "expired", "cancelled", "error"].includes(status);
}

export class FriendInviteService extends InviteService {
  createFriendInvite(ttlMs?: number): Promise<InviteSnapshot> {
    return this.createInvite("friend", undefined, ttlMs);
  }

  joinFriendInvite(code: string, ttlMs?: number): Promise<InviteSnapshot> {
    return this.joinInvite("friend", code, ttlMs);
  }
}

export class GroupInviteService extends InviteService {
  createGroupInvite(group: GroupInviteMetadata, ttlMs?: number): Promise<InviteSnapshot> {
    return this.createInvite("group", group, ttlMs);
  }

  joinGroupInvite(code: string, ttlMs?: number): Promise<InviteSnapshot> {
    return this.joinInvite("group", code, ttlMs);
  }
}
