import {
  isMeshCallTransport,
  MESH_HARD_MAX_PARTICIPANTS,
  type CallTransport,
  type CallTransportDecision,
  type CallTransportPreference,
  type CallTransportRegistry,
  type PeerConnectionDiagnostics,
  type RtcNetworkPreference,
  type TransportEvents,
} from "@briks/rtc";
import type { PeerState } from "@briks/protocol";
import { useCallStore } from "../store";
import type { BriksGateway } from "../application/contracts";
import { loadVoiceVideoSettings, type VoiceVideoSettings } from "../services/audio/settings";
import {
  CallPresenceSoundState,
  playCallSound,
  type CallSound,
} from "../services/audio/call-sounds";
import { validAvatarDataUrl } from "../services/offline/profile";
import {
  applyGroupRevocationCertificate,
  getOrCreateLocalIdentity,
  groupRendezvousId,
  loadLocalGroups,
  validGroupRevocationCertificate,
  type GroupRevocationCertificate,
  type LocalIdentity,
  type PublicPeerIdentity,
} from "../services/offline/social-storage";
import type { SignalingProvider } from "../services/signaling/types";
import {
  compatibleAppVersion,
  incompatiblePeerMessage,
} from "../services/protocol-compatibility";
import type { ScreenQualityProfile } from "../services/rtc/screen-quality";
import { loadRtcNetworkContext } from "../services/network/runtime";
import { createCallTransport, defaultCallTransportRegistry } from "./transport-factory";
import { AuthenticationManager, type CallAuthMessage } from "./auth/AuthenticationManager";
import { callGroupRevocationMessage, parseCallGroupRevocationMessage } from "./auth/revocation";
import {
  connectivityDiagnostics,
  callNetworkHealth,
  hasTurnServer,
  loadCachedCallNetworkHealth,
  saveCachedCallNetworkHealth,
  transportDiagnostics,
  type CallDiagnostics,
} from "./CallDiagnostics";
import { CallSession, type CallJoinOptions } from "./CallSession";
import { ConnectionRecovery } from "./ConnectionRecovery";
import {
  ParticipantManager,
  parseCallProfileMessage,
  type CallProfileMessage,
} from "./ParticipantManager";
import { MediaManager } from "./MediaManager";
import { bindCallSignaling } from "./signaling/call-signaling";

const AUTH_CHALLENGE_TIMEOUT_MS = 8_000;
const AUTH_CONNECTION_WATCHDOG_MS = 20_000;
const AUTHENTICATED_DATA_CHANNEL_RECOVERY_DELAY_MS = 500;
const MAX_AUTH_CHALLENGES_PER_CONNECTION = 3;
const UNKNOWN_PEERS_ERROR_PREFIX = "Há pessoas na chamada que não são membros reconhecidos deste grupo";
const MESH_CAPACITY_ERROR = `A sala atingiu o limite seguro de ${MESH_HARD_MAX_PARTICIPANTS} participantes no Mesh. Configure um transporte SFU para ampliar a chamada.`;
const missingSignaling = (): SignalingProvider => {
  throw new Error("CallController precisa receber um adapter de signaling no composition root.");
};
const missingProfileGateway: Pick<BriksGateway, "me"> = {
  me: async () => { throw new Error("CallController precisa receber o gateway de perfil no composition root."); },
};

export class CallController {
  private readonly session = new CallSession();
  private signaling?: SignalingProvider;
  private transport?: CallTransport;
  private transportDecision?: CallTransportDecision;
  private transportPreference: CallTransportPreference = "auto";
  private networkPreference: RtcNetworkPreference = "auto";
  private vpnProviders: string[] = [];
  private expectedParticipantCount = 1;
  private readonly participants = new ParticipantManager();
  private readonly authentication = new AuthenticationManager();
  private readonly recovery = new ConnectionRecovery();
  private readonly media = new MediaManager({
    getTransport: () => this.transport,
    currentLifecycle: () => this.lifecycleId,
    isActive: (lifecycle) => this.isActive(lifecycle),
    sendState: (state, failureMessage) => {
      void this.signaling?.sendPeerState(state).catch((error) => this.reportError(error, failureMessage));
    },
    reportError: (error, fallback) => this.reportError(error, fallback),
  });
  private displayName = "Participante";
  private avatar?: string;
  private signalingUnsubscribers: Array<() => void> = [];
  private turnAvailable = false;
  private identity?: LocalIdentity;
  private readonly trustedPeers = new Map<string, PublicPeerIdentity>();
  private readonly revokedPeers = new Map<string, PublicPeerIdentity>();
  private revocations: GroupRevocationCertificate[] = [];
  private groupId?: string;
  private readonly authChallenges = new Map<string, string>();
  private readonly authTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private readonly authChallengeAttempts = new Map<string, number>();
  private readonly authenticatedPeers = new Set<string>();
  private readonly pendingPeerStates = new Map<string, PeerState>();
  private readonly remoteIdentityPeerIds = new Map<string, string>();
  private readonly revocationOnlyPeers = new Set<string>();
  private readonly pendingRevokedPeers = new Set<string>();
  private mediaAuthenticationRequired = false;
  private transportReady = false;
  private readonly presenceSounds = new CallPresenceSoundState();

  // Accessors mantêm os métodos focados enquanto CallSession centraliza a
  // geração usada para cancelar operações assíncronas antigas.
  private get lifecycleId(): number { return this.session.lifecycleId; }
  private set lifecycleId(value: number) { this.session.lifecycleId = value; }
  private get roomId(): string | undefined { return this.session.roomId; }
  private set roomId(value: string | undefined) { this.session.roomId = value; }
  private get rendezvousId(): string | undefined { return this.session.rendezvousId; }
  private set rendezvousId(value: string | undefined) { this.session.rendezvousId = value; }
  private get peerId(): string | undefined { return this.session.peerId; }
  private set peerId(value: string | undefined) { this.session.peerId = value; }

  constructor(
    private readonly createSignaling: () => SignalingProvider = missingSignaling,
    private readonly profileGateway: Pick<BriksGateway, "me"> = missingProfileGateway,
    private readonly transportRegistry: CallTransportRegistry = defaultCallTransportRegistry,
  ) {}

  async join(token: string, roomId: string, iceServers: RTCIceServer[], options: CallJoinOptions = {}): Promise<MediaStream> {
    if (this.roomId) await this.leave(this.roomId);
    this.presenceSounds.reset();
    const voiceSettings = loadVoiceVideoSettings();
    const localPeerId = options.requireIdentityAuthentication && options.identity
      ? options.identity.peerId
      : crypto.randomUUID();
    const rendezvousId = options.rendezvousId ?? roomId;
    const lifecycle = this.session.begin(roomId, localPeerId, rendezvousId);
    try {
    this.transportPreference = options.transportPreference ?? "auto";
    this.transportReady = false;
    this.expectedParticipantCount = 1;
    this.media.reset();
    this.turnAvailable = hasTurnServer(iceServers);
    this.recovery.reset();
    this.identity = undefined;
    this.trustedPeers.clear();
    this.revokedPeers.clear();
    this.revocations = [];
    this.groupId = options.groupId;
    this.authChallenges.clear();
    this.authTimers.forEach((timer) => clearTimeout(timer));
    this.authTimers.clear();
    this.authChallengeAttempts.clear();
    this.authenticatedPeers.clear();
    this.pendingPeerStates.clear();
    this.remoteIdentityPeerIds.clear();
    this.revocationOnlyPeers.clear();
    this.pendingRevokedPeers.clear();
    this.mediaAuthenticationRequired = false;
    this.identity = options.identity;
    this.trustedPeers.clear();
    options.trustedPeers?.forEach((peer) => this.trustedPeers.set(peer.peerId, peer));
    options.revokedPeers?.forEach((peer) => this.revokedPeers.set(peer.peerId, peer));
    this.revocations = (options.revocations ?? []).filter(validGroupRevocationCertificate);
    this.mediaAuthenticationRequired = options.requireIdentityAuthentication === true || Boolean(options.identity && options.trustedPeers?.length);
    if (this.mediaAuthenticationRequired && !options.identity) {
      throw new Error("A identidade P2P é obrigatória para entrar em uma chamada de grupo.");
    }
    this.authChallenges.clear();
    const store = useCallStore.getState();
    store.setError(null);
    store.setSelf(localPeerId);

    const { networkInterfaces, preference: networkPreference } = await loadRtcNetworkContext();
    this.networkPreference = networkPreference;
    this.vpnProviders = [...new Set(networkInterfaces
      .filter((item) => item.provider !== "unknown")
      .map((item) => item.provider))];
    this.recovery.begin(this.turnAvailable, networkInterfaces);

    const signaling = this.createSignaling();
    this.signaling = signaling;
    const transportEvents = {
      sendOffer: (targetPeerId, description) => signaling.sendOffer(targetPeerId, description),
      sendAnswer: (targetPeerId, description) => signaling.sendAnswer(targetPeerId, description),
      sendIce: (targetPeerId, candidate) => signaling.sendIceCandidate(targetPeerId, candidate),
      onRemoteStream: (remotePeerId, stream) => this.participants.remoteStream(remotePeerId, stream),
      onConnectionState: (remotePeerId, connection) => {
        const store = useCallStore.getState();
        this.participants.connection(remotePeerId, connection);
        if (connection === "failed") {
          store.setError(this.recovery.failed(remotePeerId));
        } else if (connection === "connected" || connection === "closed") {
          if (this.recovery.finish(remotePeerId, store.error)) store.setError(null);
        }
      },
      onPeerReset: (remotePeerId) => {
        this.authenticatedPeers.delete(remotePeerId);
        this.remoteIdentityPeerIds.delete(remotePeerId);
        this.pendingPeerStates.delete(remotePeerId);
        this.authChallenges.delete(remotePeerId);
        const timer = this.authTimers.get(remotePeerId); if (timer) clearTimeout(timer);
        this.authTimers.delete(remotePeerId);
        this.authChallengeAttempts.delete(remotePeerId);
        if (this.mediaAuthenticationRequired && this.isAdmittedCallPeer(remotePeerId)) {
          this.schedulePeerAuthenticationCheck(remotePeerId, AUTH_CONNECTION_WATCHDOG_MS);
        }
        // Preserva nome/avatar autenticados na UI durante a recuperação, mas o
        // transporte bloqueia mídia até uma nova prova ECDSA pelo DataChannel.
        this.participants.reconnecting(remotePeerId, true);
      },
      onNegotiationError: (remotePeerId, error) => {
        useCallStore.getState().setError(`Falha ao negociar mídia com ${remotePeerId.slice(0, 8)}: ${error instanceof Error ? error.message : String(error)}`);
      },
      onDataMessage: (remotePeerId, data) => {
        if (this.mediaAuthenticationRequired) {
          if (this.handleAuthMessage(remotePeerId, data)) return;
          void this.handleGroupRevocationMessage(remotePeerId, data);
          return;
        }
        const message = parseCallProfileMessage(data);
        if (!message || this.mediaAuthenticationRequired) return;
        this.participants.profile(remotePeerId, message.payload.displayName, message.payload.avatar);
      },
      onDataState: (remotePeerId, state) => this.handleDataChannelState(remotePeerId, state),
    } satisfies TransportEvents;
    await signaling.connect(rendezvousId, localPeerId);
    if (this.lifecycleId !== lifecycle) throw new DOMException("Entrada na chamada cancelada.", "AbortError");
    this.expectedParticipantCount = signaling.getDiagnostics().presencePeers.length + 1;
    const { transport, decision } = createCallTransport({
      localPeerId,
      iceServers,
      events: transportEvents,
      networkInterfaces,
      networkPreference,
      participantCount: this.expectedParticipantCount,
      preference: this.transportPreference,
      network: loadCachedCallNetworkHealth(),
      registry: this.transportRegistry,
    });
    this.transportDecision = decision;
    if (this.mediaAuthenticationRequired) transport.requireMediaAuthorization();
    this.transport = transport;
    this.bindSignaling(signaling, localPeerId);
    if (this.groupId && typeof window !== "undefined") {
      const refreshSecurity = () => { void this.refreshGroupSecurity(); };
      window.addEventListener("briks:social-updated", refreshSecurity);
      this.signalingUnsubscribers.push(() => window.removeEventListener("briks:social-updated", refreshSecurity));
    }
    if (typeof window !== "undefined") {
      // Troca de rede (Wi-Fi, VPN, volta da internet): reinicia o ICE sem esperar a queda.
      let networkTimer: ReturnType<typeof setTimeout> | undefined;
      const onNetworkChange = () => {
        if (networkTimer) clearTimeout(networkTimer);
        networkTimer = setTimeout(() => {
          const current = this.transport;
          if (current && isMeshCallTransport(current)) void current.notifyNetworkChange?.();
        }, 1_500);
      };
      const connection = (navigator as Navigator & { connection?: EventTarget }).connection;
      window.addEventListener("online", onNetworkChange);
      connection?.addEventListener("change", onNetworkChange);
      this.signalingUnsubscribers.push(() => {
        if (networkTimer) clearTimeout(networkTimer);
        window.removeEventListener("online", onNetworkChange);
        connection?.removeEventListener("change", onNetworkChange);
      });
    }

      await transport.join({ roomId: rendezvousId, localPeerId, accessToken: token });
      this.transportReady = true;
      const profile = await this.profileGateway.me(token);
      if (!this.isActive(lifecycle)) throw new DOMException("Entrada na chamada cancelada.", "AbortError");
      this.displayName = profile.displayName;
      this.avatar = profile.avatar;
      this.identity ??= await getOrCreateLocalIdentity(profile.displayName);

      const localStream = await this.media.initializeMicrophone(voiceSettings, transport, lifecycle);
      if (!this.isActive(lifecycle)) throw new DOMException("Entrada na chamada cancelada.", "AbortError");
      await this.connectPresentAdmittedCallPeers(lifecycle);
      await signaling.sendPeerState(this.media.state);
      this.presenceSounds.enable();
      return localStream;
    } catch (error) {
      if (this.lifecycleId === lifecycle) await this.cleanup();
      throw error;
    }
  }

  async toggleMicrophone(_roomId: string): Promise<void> {
    await this.media.toggleMicrophone();
  }

  async updateVoiceInput(settings: VoiceVideoSettings): Promise<void> {
    await this.media.updateVoiceInput(settings);
  }

  async toggleCamera(_roomId: string): Promise<void> {
    await this.media.toggleCamera();
  }

  async toggleScreen(
    _roomId: string,
    sourceId?: string,
    includeAudio = true,
    qualityProfile?: ScreenQualityProfile,
  ): Promise<void> {
    await this.media.toggleScreen(sourceId, includeAudio, qualityProfile);
  }

  async toggleSecondScreen(_roomId: string, sourceId?: string, qualityProfile?: ScreenQualityProfile): Promise<void> {
    await this.media.toggleSecondScreen(sourceId, qualityProfile);
  }

  async updateScreenQuality(profile: ScreenQualityProfile): Promise<void> {
    await this.media.updateScreenQuality(profile);
  }

  async leave(_roomId: string): Promise<void> { await this.cleanup(); }

  updateProfile(displayName: string, avatar?: string): void {
    const normalized = displayName.trim();
    if (normalized.length < 2 || normalized.length > 80) return;
    if (avatar !== undefined && !validAvatarDataUrl(avatar)) return;
    this.displayName = normalized;
    this.avatar = avatar;
    this.sendProfile();
  }

  private sendAuthChallenge(remotePeerId: string): boolean {
    if (!this.identity || !this.transport || this.authenticatedPeers.has(remotePeerId)) return false;
    const message = this.authentication.createChallenge(this.identity.peerId);
    this.authChallenges.set(remotePeerId, message.nonce);
    const sent = this.transport.sendData(JSON.stringify(message), remotePeerId);
    if (sent < 1) {
      if (this.authChallenges.get(remotePeerId) === message.nonce) this.authChallenges.delete(remotePeerId);
      return false;
    }
    this.authChallengeAttempts.set(remotePeerId, (this.authChallengeAttempts.get(remotePeerId) ?? 0) + 1);
    this.schedulePeerAuthenticationCheck(remotePeerId, AUTH_CHALLENGE_TIMEOUT_MS);
    return true;
  }

  private handleDataChannelState(remotePeerId: string, state: RTCDataChannelState): void {
    if (state === "open") {
      if (this.mediaAuthenticationRequired) this.sendAuthChallenge(remotePeerId);
      else this.sendProfile(remotePeerId);
      // Uma conexão recriada não gera novo evento de presença. Reenvia o
      // estado para que câmera/tela/microfone sejam restaurados junto com a
      // identidade, sem depender de o usuário alternar algum controle.
      void this.signaling?.sendPeerState(this.media.state).catch((error) => {
        console.warn("Não foi possível reenviar o estado após reabrir o DataChannel.", { remotePeerId, error });
      });
      return;
    }
    if (!this.mediaAuthenticationRequired || !this.isAdmittedCallPeer(remotePeerId)) return;

    if (this.authenticatedPeers.delete(remotePeerId)) {
      this.authChallenges.delete(remotePeerId);
      this.authChallengeAttempts.delete(remotePeerId);
      const timer = this.authTimers.get(remotePeerId);
      if (timer) clearTimeout(timer);
      this.authTimers.delete(remotePeerId);
      // Mantém o nome autenticado visível, mas sinaliza imediatamente que o canal
      // de controle precisa ser recriado e exige uma nova prova de identidade.
      this.participants.reconnecting(remotePeerId);
      this.schedulePeerAuthenticationCheck(remotePeerId, AUTHENTICATED_DATA_CHANNEL_RECOVERY_DELAY_MS);
      return;
    }

    this.schedulePeerAuthenticationCheck(remotePeerId, AUTH_CHALLENGE_TIMEOUT_MS);
  }

  private schedulePeerAuthenticationCheck(remotePeerId: string, delayMs: number, replace = true): void {
    if (!this.mediaAuthenticationRequired || this.authenticatedPeers.has(remotePeerId)) return;
    const existing = this.authTimers.get(remotePeerId);
    if (existing && !replace) return;
    if (existing) clearTimeout(existing);
    const lifecycle = this.lifecycleId;
    const timer = setTimeout(() => {
      if (this.authTimers.get(remotePeerId) !== timer) return;
      this.authTimers.delete(remotePeerId);
      if (!this.isActive(lifecycle)) return;
      void this.recoverPeerAuthentication(remotePeerId);
    }, delayMs);
    this.authTimers.set(remotePeerId, timer);
  }

  private async recoverPeerAuthentication(remotePeerId: string): Promise<void> {
    const lifecycle = this.lifecycleId;
    const signaling = this.signaling;
    const transport = this.transport;
    const localPeerId = this.peerId;
    if (!signaling || !transport || !localPeerId || !this.isActive(lifecycle)
      || this.authenticatedPeers.has(remotePeerId)
      || !this.isAdmittedCallPeer(remotePeerId)
      || !signaling.getDiagnostics().presencePeers.includes(remotePeerId)) return;

    const attempts = this.authChallengeAttempts.get(remotePeerId) ?? 0;
    if (attempts < MAX_AUTH_CHALLENGES_PER_CONNECTION && this.sendAuthChallenge(remotePeerId)) return;

    this.authChallenges.delete(remotePeerId);
    this.authChallengeAttempts.delete(remotePeerId);
    console.warn("Autenticação do peer não foi concluída; recriando somente esta conexão.", {
      remotePeerId,
      challengesSent: attempts,
    });
    this.participants.reconnecting(remotePeerId, true);
    try {
      await transport.recoverPeer(remotePeerId);
    } catch (error) {
      console.warn("Falha ao recriar peer após timeout de autenticação; tentando uma nova conexão.", { remotePeerId, error });
      await transport.disconnectPeer(remotePeerId).catch(() => undefined);
      if (this.isActive(lifecycle) && this.transport === transport) {
        await transport.connectPeer(remotePeerId, true).catch((cause) => {
          console.warn("Falha ao reconectar peer ainda presente na chamada.", { remotePeerId, error: cause });
        });
      }
    } finally {
      if (this.isActive(lifecycle) && this.transport === transport && !this.authenticatedPeers.has(remotePeerId)) {
        this.schedulePeerAuthenticationCheck(remotePeerId, AUTH_CONNECTION_WATCHDOG_MS);
      }
    }
  }

  private handleAuthMessage(remotePeerId: string, raw: string): boolean {
    const parsed = this.authentication.parse(raw);
    if (parsed.status === "not-auth") return false;
    if (parsed.status === "incompatible") {
      this.rejectIncompatibleCallPeer(remotePeerId, parsed.remoteVersion);
      return true;
    }
    if (parsed.status === "expired") return true;
    if (parsed.message.type === "call.auth.challenge") void this.respondAuthChallenge(remotePeerId, parsed.message);
    if (parsed.message.type === "call.auth.proof") void this.acceptAuthProof(remotePeerId, parsed.message);
    return true;
  }

  private async respondAuthChallenge(remotePeerId: string, message: Partial<Extract<CallAuthMessage, { type: "call.auth.challenge" }>>): Promise<void> {
    const lifecycle = this.lifecycleId;
    const identity = this.identity;
    const transport = this.transport;
    const localPeerId = this.peerId;
    const roomId = this.roomId;
    if (!identity || !transport || typeof message.nonce !== "string" || !localPeerId || !roomId) return;
    const proof = await this.authentication.createProof(identity, { roomId, localPeerId, remotePeerId }, message.nonce);
    if (!this.isActive(lifecycle) || this.transport !== transport || this.identity !== identity || this.peerId !== localPeerId || this.roomId !== roomId) return;
    transport.sendData(JSON.stringify(proof), remotePeerId);
  }

  private async acceptAuthProof(remotePeerId: string, message: Partial<Extract<CallAuthMessage, { type: "call.auth.proof" }>>): Promise<void> {
    const lifecycle = this.lifecycleId;
    const transport = this.transport;
    const identity = this.identity;
    const expectedNonce = this.authChallenges.get(remotePeerId);
    const remoteIdentity = message.identity;
    const localPeerId = this.peerId;
    const roomId = this.roomId;
    if (!transport || !identity || !expectedNonce || message.nonce !== expectedNonce || !remoteIdentity
      || typeof message.signature !== "string" || !message.capabilities || !localPeerId || !roomId
      || !this.authentication.validRemoteIdentity(remotePeerId, remoteIdentity)) return;
    const trusted = this.trustedPeers.get(remoteIdentity.peerId) ?? this.revokedPeers.get(remoteIdentity.peerId);
    if (!trusted) return;
    try {
      const valid = await this.authentication.verifyProof(
        trusted,
        remoteIdentity,
        { roomId, localPeerId, remotePeerId },
        expectedNonce,
        message.capabilities,
        message.signature,
      );
      if (!valid || !this.isActive(lifecycle) || this.transport !== transport || this.identity !== identity || this.authChallenges.get(remotePeerId) !== expectedNonce) return;
      this.authChallenges.delete(remotePeerId);
      const timer = this.authTimers.get(remotePeerId); if (timer) clearTimeout(timer);
      this.authTimers.delete(remotePeerId);
      this.authChallengeAttempts.delete(remotePeerId);
      this.remoteIdentityPeerIds.set(remotePeerId, remoteIdentity.peerId);
      this.authenticatedPeers.add(remotePeerId);
      if (this.revokedPeers.has(remoteIdentity.peerId)) {
        this.pendingRevokedPeers.delete(remotePeerId);
        this.revocationOnlyPeers.add(remotePeerId);
        transport.revokePeerMedia(remotePeerId);
        this.participants.remove(remotePeerId);
        this.pendingPeerStates.delete(remotePeerId);
        await this.sendCallRevocations(remotePeerId, remoteIdentity.peerId);
        return;
      }
      this.playPresenceSound(this.presenceSounds.accept(remotePeerId));
      this.participants.profile(remotePeerId, remoteIdentity.displayName, remoteIdentity.avatar);
      await transport.authorizePeerMedia(remotePeerId);
      if (!this.isActive(lifecycle) || this.transport !== transport) return;
      const pendingState = this.pendingPeerStates.get(remotePeerId);
      if (pendingState) {
        this.pendingPeerStates.delete(remotePeerId);
        this.participants.state(remotePeerId, pendingState);
      }
    } catch { /* prova externa inválida */ }
  }

  private async handleGroupRevocationMessage(remotePeerId: string, raw: string): Promise<void> {
    const lifecycle = this.lifecycleId;
    const groupId = this.groupId;
    const identity = this.identity;
    if (!groupId || !identity) return;
    const message = parseCallGroupRevocationMessage(raw);
    if (!message) return;
    if (message.certificate.groupId !== groupId || !(await applyGroupRevocationCertificate(message.certificate))) return;
    if (!this.isActive(lifecycle) || this.groupId !== groupId || this.identity !== identity) return;
    if (message.certificate.targetPeerId === identity.peerId) {
      await this.cleanup();
      return;
    }
    await this.refreshGroupSecurity();
  }

  private async sendCallRevocations(remotePeerId: string, identityPeerId: string): Promise<void> {
    if (!this.transport || !this.revocationOnlyPeers.has(remotePeerId)) return;
    for (const certificate of this.revocations.filter((item) => item.targetPeerId === identityPeerId)) {
      const message = callGroupRevocationMessage(certificate);
      this.transport.sendData(JSON.stringify(message), remotePeerId);
    }
  }

  private async refreshGroupSecurity(): Promise<void> {
    const lifecycle = this.lifecycleId;
    const groupId = this.groupId;
    if (!groupId) return;
    const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
    if (!group || !this.isActive(lifecycle) || this.groupId !== groupId) return;
    this.trustedPeers.clear();
    this.revokedPeers.clear();
    (group.members ?? []).forEach((peer) => this.trustedPeers.set(peer.peerId, peer));
    (group.removedMembers ?? []).forEach((peer) => this.revokedPeers.set(peer.peerId, peer));
    this.revocations = (group.revocations ?? []).filter(validGroupRevocationCertificate);

    for (const [remotePeerId, identityPeerId] of [...this.remoteIdentityPeerIds]) {
      if (!this.isActive(lifecycle)) return;
      if (this.revokedPeers.has(identityPeerId)) {
        this.revocationOnlyPeers.add(remotePeerId);
        this.pendingPeerStates.delete(remotePeerId);
        this.transport?.revokePeerMedia(remotePeerId);
        this.participants.remove(remotePeerId);
        await this.sendCallRevocations(remotePeerId, identityPeerId);
      } else if (!this.trustedPeers.has(identityPeerId)) {
        this.authenticatedPeers.delete(remotePeerId);
        this.remoteIdentityPeerIds.delete(remotePeerId);
        this.transport?.revokePeerMedia(remotePeerId);
        this.participants.remove(remotePeerId);
        await this.transport?.disconnectPeer(remotePeerId);
      }
    }
    if (!this.isActive(lifecycle)) return;
    const nextRendezvousId = this.roomId ? groupRendezvousId(group, "voice", this.roomId) : undefined;
    const signaling = this.signaling;
    const peerId = this.peerId;
    if (nextRendezvousId && this.rendezvousId && nextRendezvousId !== this.rendezvousId && signaling && peerId) {
      this.rendezvousId = nextRendezvousId;
      await signaling.connect(nextRendezvousId, peerId);
      if (!this.isActive(lifecycle) || this.signaling !== signaling) return;
    }
    await this.connectPresentAdmittedCallPeers(lifecycle);
  }

  private async connectPresentAdmittedCallPeers(lifecycle: number): Promise<void> {
    const signaling = this.signaling;
    const transport = this.transport;
    const localPeerId = this.peerId;
    if (!signaling || !transport || !localPeerId || !this.transportReady || !this.isActive(lifecycle)) return;
    const present = signaling.getDiagnostics().presencePeers.filter((remotePeerId) => remotePeerId !== localPeerId);
    this.reportUnknownPeers(present.filter((remotePeerId) => !this.isAdmittedCallPeer(remotePeerId)).length);
    const remotePeerIds = present
      .filter((remotePeerId) => this.isAdmittedCallPeer(remotePeerId)
        && this.enforceMeshCapacity(remotePeerId));
    const store = useCallStore.getState();
    await Promise.all(remotePeerIds.map(async (remotePeerId) => {
      // Um peer pode já estar na presença e só se tornar admitido após a
      // sincronização do grupo; nesse caso ele também precisa entrar no ciclo
      // normal de autenticação e sons.
      this.presenceSounds.observe(remotePeerId);
      this.schedulePeerAuthenticationCheck(remotePeerId, AUTH_CONNECTION_WATCHDOG_MS, false);
      this.participants.ensure(remotePeerId);
      await transport.connectPeer(remotePeerId, localPeerId < remotePeerId).catch((error) => store.setError(String(error)));
    }));
    if (!this.isActive(lifecycle) || this.signaling !== signaling) return;
    if (remotePeerIds.length) void signaling.sendPeerState(this.media.state).catch((error) => store.setError(String(error)));
  }

  private rejectIncompatibleCallPeer(remotePeerId: string, remoteVersion?: string): void {
    const message = incompatiblePeerMessage(remoteVersion);
    console.warn(message, { remotePeerId });
    useCallStore.getState().setError(message);
    this.pendingRevokedPeers.delete(remotePeerId);
    this.authChallenges.delete(remotePeerId);
    this.authChallengeAttempts.delete(remotePeerId);
    const timer = this.authTimers.get(remotePeerId); if (timer) clearTimeout(timer);
    this.authTimers.delete(remotePeerId);
    this.participants.remove(remotePeerId);
    void this.transport?.disconnectPeer(remotePeerId);
  }

  getDiagnostics(): CallDiagnostics {
    const peerConnections = this.transport?.getDiagnostics() ?? [];
    return {
      signaling: this.signaling?.getDiagnostics() ?? null,
      peerConnections,
      connectivity: connectivityDiagnostics(this.turnAvailable),
      network: { preference: this.networkPreference, vpnProviders: this.vpnProviders },
      transport: this.buildTransportDiagnostics(peerConnections),
    };
  }

  private bindSignaling(signaling: SignalingProvider, peerId: string): void {
    this.signalingUnsubscribers.push(bindCallSignaling(signaling, {
      peerJoined: (peer) => {
        if (this.mediaAuthenticationRequired && peer.clientVersion && !compatibleAppVersion(peer.clientVersion)) {
          this.rejectIncompatibleCallPeer(peer.peerId, peer.clientVersion);
          return;
        }
        if (this.mediaAuthenticationRequired && !this.isAdmittedCallPeer(peer.peerId)) return;
        if (!this.enforceMeshCapacity(peer.peerId)) return;
        if (!this.transportReady) return;
        this.presenceSounds.observe(peer.peerId);
        if (!this.mediaAuthenticationRequired) this.playPresenceSound(this.presenceSounds.accept(peer.peerId));
        else this.schedulePeerAuthenticationCheck(peer.peerId, AUTH_CONNECTION_WATCHDOG_MS, false);
        const store = useCallStore.getState();
        this.participants.ensure(peer.peerId);
        void this.transport?.connectPeer(peer.peerId, peerId < peer.peerId).catch((error) => store.setError(String(error)));
        void signaling.sendPeerState(this.media.state).catch((error) => store.setError(String(error)));
      },
      peerLeft: (remotePeerId) => {
        this.playPresenceSound(this.presenceSounds.leave(remotePeerId));
        const store = useCallStore.getState();
        if (this.recovery.finish(remotePeerId, store.error)) store.setError(null);
        this.participants.remove(remotePeerId);
        this.authenticatedPeers.delete(remotePeerId);
        this.pendingPeerStates.delete(remotePeerId);
        this.remoteIdentityPeerIds.delete(remotePeerId);
        this.revocationOnlyPeers.delete(remotePeerId);
        this.pendingRevokedPeers.delete(remotePeerId);
        this.authChallenges.delete(remotePeerId);
        const timer = this.authTimers.get(remotePeerId); if (timer) clearTimeout(timer);
        this.authTimers.delete(remotePeerId);
        this.authChallengeAttempts.delete(remotePeerId);
        void this.transport?.disconnectPeer(remotePeerId);
        void this.connectPresentAdmittedCallPeers(this.lifecycleId);
      },
      offer: (message) => {
        if (this.mediaAuthenticationRequired && !this.isAdmittedCallPeer(message.fromPeerId)) return;
        if (!this.enforceMeshCapacity(message.fromPeerId)) return;
        const transport = this.transport;
        if (!transport || !isMeshCallTransport(transport)) return;
        void transport.acceptOffer(message.fromPeerId, message.payload.sdp).catch((error) => useCallStore.getState().setError(String(error)));
      },
      answer: (message) => {
        if (this.mediaAuthenticationRequired && !this.isAdmittedCallPeer(message.fromPeerId)) return;
        if (!this.enforceMeshCapacity(message.fromPeerId)) return;
        const transport = this.transport;
        if (!transport || !isMeshCallTransport(transport)) return;
        void transport.acceptAnswer(message.fromPeerId, message.payload.sdp).catch((error) => useCallStore.getState().setError(String(error)));
      },
      iceCandidate: (message) => {
        if (this.mediaAuthenticationRequired && !this.isAdmittedCallPeer(message.fromPeerId)) return;
        if (!this.enforceMeshCapacity(message.fromPeerId)) return;
        const transport = this.transport;
        if (!transport || !isMeshCallTransport(transport)) return;
        void transport.addIceCandidate(message.fromPeerId, message.payload.candidate).catch((error) => useCallStore.getState().setError(String(error)));
      },
      peerState: (message) => {
        if (this.mediaAuthenticationRequired && !this.isAdmittedCallPeer(message.fromPeerId)) return;
        if (this.revocationOnlyPeers.has(message.fromPeerId)) return;
        if (this.mediaAuthenticationRequired && !this.authenticatedPeers.has(message.fromPeerId)) {
          this.pendingPeerStates.set(message.fromPeerId, message.payload.state);
          return;
        }
        this.participants.state(message.fromPeerId, message.payload.state);
      },
      statusChange: (status) => {
        if (status === "error") useCallStore.getState().setError("Falha no signaling Supabase Realtime.");
      },
    }));
  }

  private isActive(lifecycle: number): boolean {
    return this.session.isActive(lifecycle, Boolean(this.transport && this.signaling));
  }

  private reportError(error: unknown, fallback: string): void {
    const message = error instanceof Error ? error.message : fallback;
    useCallStore.getState().setError(message || fallback);
  }

  private playPresenceSound(sound: CallSound | null): void {
    if (sound) playCallSound(sound);
  }

  private async cleanup(): Promise<void> {
    this.session.end();
    const signaling = this.signaling;
    const transport = this.transport;
    const unsubscribers = this.signalingUnsubscribers.splice(0);
    const localLeaveSound = this.presenceSounds.disable();

    // O estado é destacado antes dos awaits para impedir que um cleanup antigo
    // apague a sessão criada por um join posterior.
    this.signaling = undefined;
    this.transport = undefined;
    this.transportReady = false;
    this.transportDecision = undefined;
    this.transportPreference = "auto";
    this.networkPreference = "auto";
    this.vpnProviders = [];
    this.expectedParticipantCount = 1;
    this.displayName = "Participante";
    this.avatar = undefined;
    this.turnAvailable = false;
    this.recovery.reset();
    this.identity = undefined;
    this.trustedPeers.clear();
    this.revokedPeers.clear();
    this.revocations = [];
    this.groupId = undefined;
    this.authChallenges.clear();
    this.authTimers.forEach((timer) => clearTimeout(timer));
    this.authTimers.clear();
    this.authChallengeAttempts.clear();
    this.authenticatedPeers.clear();
    this.pendingPeerStates.clear();
    this.remoteIdentityPeerIds.clear();
    this.revocationOnlyPeers.clear();
    this.pendingRevokedPeers.clear();
    this.mediaAuthenticationRequired = false;

    this.playPresenceSound(localLeaveSound);
    unsubscribers.forEach((unsubscribe) => unsubscribe());
    const mediaCleanup = this.media.cleanup();

    this.participants.clear();

    await signaling?.disconnect().catch(() => undefined);
    await transport?.leave().catch(() => undefined);
    await mediaCleanup.catch(() => undefined);
  }

  async getLiveDiagnostics(): Promise<CallDiagnostics> {
    const peerConnections = await this.transport?.collectDiagnostics() ?? [];
    if (peerConnections.length) saveCachedCallNetworkHealth(callNetworkHealth(peerConnections));
    return {
      signaling: this.signaling?.getDiagnostics() ?? null,
      peerConnections,
      connectivity: connectivityDiagnostics(this.turnAvailable),
      network: { preference: this.networkPreference, vpnProviders: this.vpnProviders },
      transport: this.buildTransportDiagnostics(peerConnections),
    };
  }

  private buildTransportDiagnostics(peerConnections: PeerConnectionDiagnostics[]): CallDiagnostics["transport"] {
    return transportDiagnostics({
      peerConnections,
      presencePeers: this.signaling?.getDiagnostics().presencePeers.length ?? 0,
      expectedParticipantCount: this.expectedParticipantCount,
      preference: this.transportPreference,
      registry: this.transportRegistry,
      active: this.transport?.kind ?? null,
      initialDecision: this.transportDecision,
    });
  }

  private enforceMeshCapacity(candidatePeerId?: string): boolean {
    const transport = this.transport;
    const signaling = this.signaling;
    const localPeerId = this.peerId;
    if (!signaling || !localPeerId) return true;
    const present = [...new Set([localPeerId, ...signaling.getDiagnostics().presencePeers])].sort();
    this.expectedParticipantCount = present.length;
    if (!transport || !isMeshCallTransport(transport)) return true;
    if (present.length <= MESH_HARD_MAX_PARTICIPANTS) {
      const store = useCallStore.getState();
      if (store.error === MESH_CAPACITY_ERROR) store.setError(null);
      return true;
    }
    const admitted = new Set(present.slice(0, MESH_HARD_MAX_PARTICIPANTS));
    for (const peer of transport.getDiagnostics()) {
      if (admitted.has(peer.peerId) && admitted.has(localPeerId)) continue;
      this.participants.remove(peer.peerId);
      void transport.disconnectPeer(peer.peerId);
    }
    useCallStore.getState().setError(MESH_CAPACITY_ERROR);
    return admitted.has(localPeerId) && (!candidatePeerId || admitted.has(candidatePeerId));
  }

  private sendProfile(targetPeerId?: string): void {
    const message: CallProfileMessage = {
      version: 1,
      type: "call.profile",
      payload: { displayName: this.displayName, avatar: this.avatar },
    };
    this.transport?.sendData(JSON.stringify(message), targetPeerId);
  }

  /** Em chamadas de grupo só quem está na lista de membros é admitido; avisa quando alguém fica de fora. */
  private lastMembershipResyncAt = 0;
  private unknownRetryTimer?: ReturnType<typeof setTimeout>;

  private reportUnknownPeers(count: number): void {
    const store = useCallStore.getState();
    const current = store.error;
    if (count > 0 && this.mediaAuthenticationRequired) {
      // Reenvio automático: enquanto houver gente não reconhecida, pede/reenvia a lista de membros (no máximo a cada 5 s).
      const now = Date.now();
      if (typeof window !== "undefined" && now - this.lastMembershipResyncAt > 5000) {
        this.lastMembershipResyncAt = now;
        window.dispatchEvent(new Event("briks:group-resync"));
      }
      if (!this.unknownRetryTimer) {
        const lifecycle = this.lifecycleId;
        this.unknownRetryTimer = setTimeout(() => {
          this.unknownRetryTimer = undefined;
          if (this.isActive(lifecycle)) void this.connectPresentAdmittedCallPeers(lifecycle).catch(() => undefined);
        }, 5500);
      }
      if (!current) {
        store.setError(`${UNKNOWN_PEERS_ERROR_PREFIX} (${count}). Quem entrou precisa ser convidado pelo ícone + de Membros para que todos se vejam.`);
      }
    } else if (current?.startsWith(UNKNOWN_PEERS_ERROR_PREFIX)) {
      store.setError(null);
    }
  }

  private isAdmittedCallPeer(remotePeerId: string): boolean {
    if (this.trustedPeers.has(remotePeerId)) return true;
    if (!this.revokedPeers.has(remotePeerId)) return false;
    if (this.pendingRevokedPeers.has(remotePeerId) || this.revocationOnlyPeers.has(remotePeerId)) return true;
    if (this.pendingRevokedPeers.size + this.revocationOnlyPeers.size >= 1) return false;
    this.pendingRevokedPeers.add(remotePeerId);
    return true;
  }
}
