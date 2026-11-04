import type { IceCandidatePayload, PeerState } from "@risk/protocol";
import { logger } from "@risk/shared";
import {
  classifySelectedConnectionPath,
  type NetworkInterfaceDescriptor,
} from "../connection-path";
import {
  CONTROL_CHANNEL_LABEL,
  MAX_CONTROL_MESSAGE_BYTES,
  MAX_DATA_BUFFER_BYTES,
  MAX_TRANSFER_FRAME_BYTES,
  TRANSFER_BUFFER_WAIT_TIMEOUT_MS,
  TRANSFER_CHANNEL_LABEL,
  TRANSFER_HIGH_WATER_MARK_BYTES,
  TRANSFER_LOW_WATER_MARK_BYTES,
  encodedMessageSize,
  exactArrayBuffer,
} from "../data/messages";
import { connectionPathSignature, summarizePeerStats, type OutboundBytesSample } from "../diagnostics/stats";
import type {
  CallTransportJoinOptions,
  MeshCallTransport,
  PeerConnectionDiagnostics,
  TransportEvents,
} from "./call-transport";
import { resolveVideoSenderPolicy, type VideoPublicationOptions } from "../video-encoding";
import { isMLineOrderMismatch } from "./negotiation";
import { createMeshPeerEntry, type MeshPeerEntry as PeerEntry } from "./peer";
import { addOrQueueIceCandidate, flushPendingIceCandidates } from "./ice";
import {
  prepareLocalIceCandidate,
  prepareLocalSessionDescription,
  type MeshTransportOptions,
  type RtcNetworkPreference,
} from "./network-policy";

const DEFAULT_MAX_REMOTE_PEERS = 5;
const DISCONNECTED_RECOVERY_DELAY_MS = 2_500;
/** Tempo máximo esperando uma conexão nova ficar "connected" antes de reiniciar o ICE. */
const CONNECT_WATCHDOG_MS = 15_000;
/** O lado de id maior só assume a recuperação se o lado de id menor não agir. */
const NON_INITIATOR_RECOVERY_EXTRA_MS = 6_000;
/** Pré-coleta de candidatos: conecta mais rápido, principalmente com TURN. */
const ICE_CANDIDATE_POOL_SIZE = 4;
const MAX_RECOVERY_DELAY_MS = 30_000;
const RECREATE_PEER_EVERY_ATTEMPTS = 3;

export class MeshWebRTCTransport implements MeshCallTransport {
  readonly kind = "mesh" as const;
  private readonly peers = new Map<string, PeerEntry>();
  private readonly localTracks = new Map<string, { track: MediaStreamTrack; stream: MediaStream; video?: VideoPublicationOptions }>();
  private readonly mediaAuthorizedPeers = new Set<string>();
  private readonly pendingRemoteStreams = new Map<string, Map<string, MediaStream>>();
  private readonly activeRemoteStreams = new Map<string, Map<string, MediaStream>>();
  private readonly recoveryTimers = new Map<string, ReturnType<typeof setTimeout>>();
  private readonly connectWatchdogs = new Map<string, ReturnType<typeof setTimeout>>();
  private readonly recoveryAttempts = new Map<string, number>();
  private mediaAuthorizationRequired = false;
  private readonly previousOutboundBytes = new Map<string, OutboundBytesSample>();
  private readonly previousConnectionPaths = new Map<string, string>();
  private readonly maxRemotePeers: number;
  private readonly networkInterfaces: readonly NetworkInterfaceDescriptor[];
  private readonly networkPreference: RtcNetworkPreference;
  private videoParameterUpdate: Promise<void> = Promise.resolve();
  private joinedRoomId?: string;

  constructor(
    private readonly localPeerId: string,
    private readonly iceServers: RTCIceServer[],
    private readonly events: TransportEvents,
    options: number | readonly NetworkInterfaceDescriptor[] | MeshTransportOptions = DEFAULT_MAX_REMOTE_PEERS,
  ) {
    const normalized = normalizeMeshTransportOptions(options);
    this.maxRemotePeers = normalized.maxRemotePeers ?? DEFAULT_MAX_REMOTE_PEERS;
    this.networkInterfaces = normalized.networkInterfaces ?? [];
    this.networkPreference = normalized.networkPreference ?? "auto";
    if (!Number.isInteger(this.maxRemotePeers) || this.maxRemotePeers < 1) throw new Error("maxRemotePeers deve ser maior que zero.");
  }

  async join(options: CallTransportJoinOptions): Promise<void> {
    if (options.localPeerId !== this.localPeerId) throw new Error("O transporte Mesh foi criado para outro peer local.");
    if (this.joinedRoomId && this.joinedRoomId !== options.roomId) {
      throw new Error("O transporte Mesh já está associado a outra chamada.");
    }
    this.joinedRoomId = options.roomId;
  }

  async leave(): Promise<void> {
    this.joinedRoomId = undefined;
    await this.disconnect();
  }

  connectPeer(peerId: string, initiator: boolean): Promise<void> {
    return this.connect(peerId, initiator);
  }

  disconnectPeer(peerId: string): Promise<void> {
    return this.disconnect(peerId);
  }

  async connect(peerId: string, initiator: boolean): Promise<void> {
    const entry = this.peers.get(peerId) ?? this.createPeer(peerId);
    if (initiator) { entry.canNegotiate = true; entry.initiator = true; }
    if (initiator && this.events.onDataMessage && !entry.dataChannel) {
      this.bindControlDataChannel(peerId, entry, entry.pc.createDataChannel(CONTROL_CHANNEL_LABEL, { ordered: true }));
    }
    if (initiator) {
      entry.needsNegotiation = true;
      await this.negotiateIfNeeded(peerId, entry);
    }
    await this.applyAdaptiveVideoParameters();
  }

  requireMediaAuthorization(): void {
    if (this.peers.size > 0) throw new Error("A autorização de mídia precisa ser ativada antes de conectar peers.");
    this.mediaAuthorizationRequired = true;
  }

  async authorizePeerMedia(peerId: string): Promise<void> {
    this.mediaAuthorizedPeers.add(peerId);
    const entry = this.requirePeer(peerId);
    const pendingStreams = this.pendingRemoteStreams.get(peerId);
    if (pendingStreams) {
      this.pendingRemoteStreams.delete(peerId);
      for (const stream of pendingStreams.values()) {
        stream.getTracks().forEach((track) => { track.enabled = true; });
        this.rememberActiveRemoteStream(peerId, stream);
        this.events.onRemoteStream(peerId, stream);
      }
    }
    for (const { track, stream } of this.localTracks.values()) {
      if (!entry.pc.getSenders().some((sender) => sender.track?.id === track.id)) entry.pc.addTrack(track, stream);
    }
    entry.needsNegotiation = true;
    await this.negotiateIfNeeded(peerId, entry);
    await this.applyAdaptiveVideoParameters();
  }

  revokePeerMedia(peerId: string): void {
    this.mediaAuthorizedPeers.delete(peerId);
    this.pendingRemoteStreams.get(peerId)?.forEach((stream) => stream.getTracks().forEach((track) => { track.enabled = false; }));
    this.pendingRemoteStreams.delete(peerId);
    this.activeRemoteStreams.get(peerId)?.forEach((stream) => stream.getTracks().forEach((track) => { track.enabled = false; }));
    const entry = this.peers.get(peerId);
    if (entry) {
      for (const sender of entry.pc.getSenders()) {
        if (sender.track) entry.pc.removeTrack(sender);
      }
    }
    void this.applyAdaptiveVideoParameters();
  }

  async acceptOffer(peerId: string, description: RTCSessionDescriptionInit): Promise<void> {
    if (description.type !== "offer") throw new Error("Descrição WebRTC não é uma offer.");
    await this.acceptDescription(peerId, description);
    await this.applyAdaptiveVideoParameters();
  }

  async acceptAnswer(peerId: string, description: RTCSessionDescriptionInit): Promise<void> {
    if (description.type !== "answer") throw new Error("Descrição WebRTC não é uma answer.");
    await this.acceptDescription(peerId, description);
    await this.applyAdaptiveVideoParameters();
  }

  async addIceCandidate(peerId: string, candidate: RTCIceCandidateInit): Promise<void> {
    const entry = this.peers.get(peerId) ?? this.createPeer(peerId);
    await addOrQueueIceCandidate(entry, candidate);
  }

  async publishTrack(track: MediaStreamTrack, stream: MediaStream, options?: VideoPublicationOptions): Promise<void> {
    this.localTracks.set(track.id, { track, stream, video: track.kind === "video" ? options : undefined });
    const negotiations: Promise<void>[] = [];
    for (const [peerId, entry] of this.peers) {
      if (this.mediaAuthorizationRequired && !this.mediaAuthorizedPeers.has(peerId)) continue;
      if (entry.pc.getSenders().some((sender) => sender.track?.id === track.id)) continue;
      entry.pc.addTrack(track, stream);
      entry.needsNegotiation = true;
      negotiations.push(this.negotiateIfNeeded(peerId, entry));
    }
    await Promise.all(negotiations);
    await this.applyAdaptiveVideoParameters();
  }

  async configurePublishedVideoTrack(track: MediaStreamTrack, options: VideoPublicationOptions): Promise<void> {
    if (track.kind !== "video") throw new Error("Somente faixas de vídeo aceitam parâmetros de codificação.");
    const published = this.localTracks.get(track.id);
    if (!published) throw new Error(`Track publicada não encontrada: ${track.id}`);
    this.localTracks.set(track.id, { ...published, video: options });
    await this.applyAdaptiveVideoParameters();
  }

  async unpublishTrack(track: MediaStreamTrack): Promise<void> {
    this.localTracks.delete(track.id);
    const negotiations: Promise<void>[] = [];
    for (const [peerId, entry] of this.peers) {
      const sender = entry.pc.getSenders().find((item) => item.track === track || item.track?.id === track.id);
      if (!sender) continue;
      entry.pc.removeTrack(sender);
      entry.needsNegotiation = true;
      negotiations.push(this.negotiateIfNeeded(peerId, entry));
    }
    await Promise.all(negotiations);
    await this.applyAdaptiveVideoParameters();
  }

  async replaceTrack(kind: "audio" | "video", track: MediaStreamTrack | null): Promise<void> {
    await Promise.all([...this.peers.values()].map(async ({ pc }) => {
      const sender = pc.getTransceivers().find((item) => item.sender.track?.kind === kind || item.receiver.track.kind === kind)?.sender;
      if (sender) await sender.replaceTrack(track);
    }));
  }

  async replacePublishedTrack(previousTrack: MediaStreamTrack, nextTrack: MediaStreamTrack, stream: MediaStream): Promise<void> {
    if (previousTrack.kind !== nextTrack.kind) throw new Error("A track substituta precisa ter o mesmo tipo da track publicada.");
    if (!this.localTracks.has(previousTrack.id)) throw new Error(`Track publicada não encontrada: ${previousTrack.id}`);

    const senders = [...this.peers.values()]
      .map(({ pc }) => pc.getSenders().find((sender) => sender.track === previousTrack || sender.track?.id === previousTrack.id))
      .filter((sender): sender is RTCRtpSender => Boolean(sender));
    const replaced: RTCRtpSender[] = [];
    try {
      for (const sender of senders) {
        await sender.replaceTrack(nextTrack);
        replaced.push(sender);
      }
    } catch (error) {
      await Promise.all(replaced.map((sender) => sender.replaceTrack(previousTrack).catch(() => undefined)));
      throw error;
    }

    const previousPublication = this.localTracks.get(previousTrack.id);
    this.localTracks.delete(previousTrack.id);
    this.localTracks.set(nextTrack.id, { track: nextTrack, stream, video: previousPublication?.video });
    if (nextTrack.kind === "video") await this.applyAdaptiveVideoParameters();
  }

  sendData(data: string, targetPeerId?: string): number {
    if (encodedMessageSize(data) > MAX_CONTROL_MESSAGE_BYTES) throw new Error("Mensagem DataChannel excede 64 KiB.");
    let sent = 0;
    for (const [peerId, entry] of this.peers) {
      if (targetPeerId && peerId !== targetPeerId) continue;
      const channel = entry.dataChannel;
      if (channel?.readyState !== "open") continue;
      if (channel.bufferedAmount > MAX_DATA_BUFFER_BYTES) {
        logger.warn("DataChannel de controle congestionado; mensagem não enviada", { peerId, bufferedAmount: channel.bufferedAmount });
        continue;
      }
      channel.send(data);
      sent += 1;
    }
    return sent;
  }

  sendTransferData(data: ArrayBuffer | ArrayBufferView, targetPeerId?: string): number {
    const payload = exactArrayBuffer(data);
    if (payload.byteLength > MAX_TRANSFER_FRAME_BYTES) throw new Error("Frame do risk.transfer excede o limite permitido.");
    let sent = 0;
    for (const [peerId, entry] of this.peers) {
      if (targetPeerId && peerId !== targetPeerId) continue;
      const channel = entry.transferDataChannel;
      if (channel?.readyState !== "open") continue;
      if (channel.bufferedAmount > TRANSFER_HIGH_WATER_MARK_BYTES) continue;
      channel.send(payload);
      sent += 1;
    }
    return sent;
  }

  getTransferBufferedAmount(peerId: string): number {
    const channel = this.peers.get(peerId)?.transferDataChannel;
    return channel?.readyState === "open" ? channel.bufferedAmount : Number.POSITIVE_INFINITY;
  }

  isTransferChannelOpen(peerId: string): boolean {
    const channel = this.peers.get(peerId)?.transferDataChannel;
    return channel?.readyState === "open";
  }

  ensureTransferChannel(peerId: string): void {
    const entry = this.requirePeer(peerId);
    if (!this.events.onTransferMessage || entry.transferDataChannel) return;
    if (this.localPeerId > peerId) return;
    this.bindTransferDataChannel(peerId, entry, entry.pc.createDataChannel(TRANSFER_CHANNEL_LABEL, { ordered: true }));
  }

  async waitForTransferBufferedAmountLow(peerId: string, threshold = TRANSFER_LOW_WATER_MARK_BYTES): Promise<void> {
    const channel = this.requirePeer(peerId).transferDataChannel;
    if (!channel || channel.readyState !== "open") throw new Error(`Canal ${TRANSFER_CHANNEL_LABEL} indisponível para ${peerId}.`);
    if (channel.bufferedAmount <= threshold) return;
    channel.bufferedAmountLowThreshold = Math.max(0, threshold);
    await new Promise<void>((resolve, reject) => {
      let timeout: ReturnType<typeof setTimeout> | undefined;
      const cleanup = () => {
        if (timeout) clearTimeout(timeout);
        channel.removeEventListener("bufferedamountlow", onLow);
        channel.removeEventListener("close", onClosed);
        channel.removeEventListener("error", onClosed);
      };
      const onLow = () => { cleanup(); resolve(); };
      const onClosed = () => { cleanup(); reject(new Error(`Canal ${TRANSFER_CHANNEL_LABEL} foi fechado durante a transferência.`)); };
      timeout = setTimeout(() => {
        cleanup();
        reject(new Error(`Canal ${TRANSFER_CHANNEL_LABEL} permaneceu congestionado por mais de ${TRANSFER_BUFFER_WAIT_TIMEOUT_MS / 1000}s.`));
      }, TRANSFER_BUFFER_WAIT_TIMEOUT_MS);
      channel.addEventListener("bufferedamountlow", onLow, { once: true });
      channel.addEventListener("close", onClosed, { once: true });
      channel.addEventListener("error", onClosed, { once: true });
    });
  }

  /**
   * Chamado quando a rede local muda (Wi-Fi trocou, voltou a ficar online, VPN).
   * Reinicia o ICE de todos os peers para achar o novo caminho sem esperar a queda.
   */
  async notifyNetworkChange(): Promise<void> {
    const ids = [...this.peers.keys()];
    await Promise.all(ids.map(async (peerId) => {
      const entry = this.peers.get(peerId);
      if (!entry || entry.pc.connectionState === "closed") return;
      try {
        await this.restartIce(peerId);
      } catch (error) {
        logger.warn("ICE restart after network change failed", { peerId, error: String(error) });
      }
    }));
  }

  async restartIce(peerId: string): Promise<void> {
    const entry = this.requirePeer(peerId);
    entry.needsIceRestart = true;
    entry.pc.restartIce();
    await this.negotiateIfNeeded(peerId, entry);
  }

  /**
   * Recria somente a conexão de um peer que ficou parcialmente conectado
   * (por exemplo, mídia/ICE ativos mas DataChannel de controle travado).
   */
  async recoverPeer(peerId: string): Promise<void> {
    const entry = this.requirePeer(peerId);
    const attempt = (this.recoveryAttempts.get(peerId) ?? 0) + 1;
    await this.recreatePeerForRecovery(peerId, entry, attempt);
  }

  async disconnect(peerId?: string): Promise<void> {
    const ids = peerId ? [peerId] : [...this.peers.keys()];
    ids.forEach((id) => {
      const entry = this.peers.get(id);
      if (!entry) return;
      entry.pc.onicecandidate = null;
      entry.pc.ontrack = null;
      entry.pc.onnegotiationneeded = null;
      entry.pc.onconnectionstatechange = null;
      entry.pendingIceCandidates.length = 0;
      entry.dataChannel?.close();
      entry.transferDataChannel?.close();
      entry.pc.close();
      this.peers.delete(id);
      this.mediaAuthorizedPeers.delete(id);
      const pendingStreams = this.pendingRemoteStreams.get(id);
      pendingStreams?.forEach((stream) => stream.getTracks().forEach((track) => { track.enabled = false; }));
      this.pendingRemoteStreams.delete(id);
      this.activeRemoteStreams.delete(id);
      this.previousOutboundBytes.delete(id);
      this.previousConnectionPaths.delete(id);
      const timer = this.recoveryTimers.get(id);
      if (timer) clearTimeout(timer);
      this.recoveryTimers.delete(id);
      this.clearConnectWatchdog(id);
      this.recoveryAttempts.delete(id);
    });
    await this.applyAdaptiveVideoParameters();
  }

  getDiagnostics(): PeerConnectionDiagnostics[] {
    return [...this.peers].map(([peerId, entry]) => ({
      peerId,
      connectionState: entry.pc.connectionState,
      iceConnectionState: entry.pc.iceConnectionState,
      signalingState: entry.pc.signalingState,
      pendingIceCandidates: entry.pendingIceCandidates.length,
      dataChannelState: entry.dataChannel?.readyState ?? "unavailable",
      transferDataChannelState: entry.transferDataChannel?.readyState ?? "unavailable",
      selectedConnectionPath: classifySelectedConnectionPath(undefined),
    }));
  }

  async collectDiagnostics(): Promise<PeerConnectionDiagnostics[]> {
    return Promise.all([...this.peers].map(async ([peerId, entry]) => {
      const base = this.getDiagnostics().find((item) => item.peerId === peerId)!;
      const reports = await entry.pc.getStats();
      const summary = summarizePeerStats(reports, this.previousOutboundBytes.get(peerId), this.networkInterfaces);
      if (summary.outboundSample) this.previousOutboundBytes.set(peerId, summary.outboundSample);
      const pathSignature = connectionPathSignature(summary.selectedConnectionPath);
      if (this.previousConnectionPaths.get(peerId) !== pathSignature) {
        this.previousConnectionPaths.set(peerId, pathSignature);
        logger.info("[rtc] selected connection path", {
          peerId,
          kind: summary.selectedConnectionPath.kind,
          provider: summary.selectedConnectionPath.provider,
          protocol: summary.selectedConnectionPath.protocol,
        });
      }
      const { outboundSample: _outboundSample, ...diagnostics } = summary;
      return { ...base, ...diagnostics };
    }));
  }

  private createPeer(peerId: string): PeerEntry {
    const existing = this.peers.get(peerId);
    if (existing) return existing;
    if (this.peers.size >= this.maxRemotePeers) {
      throw new Error(`Sala cheia: este cliente aceita no máximo ${this.maxRemotePeers + 1} participantes no Mesh.`);
    }
    const pc = new RTCPeerConnection({ iceServers: this.iceServers, iceTransportPolicy: "all", iceCandidatePoolSize: ICE_CANDIDATE_POOL_SIZE });
    const entry = createMeshPeerEntry(pc);
    this.peers.set(peerId, entry);
    if (!this.mediaAuthorizationRequired || this.mediaAuthorizedPeers.has(peerId)) this.localTracks.forEach(({ track, stream }) => pc.addTrack(track, stream));
    pc.onicecandidate = ({ candidate }) => {
      if (!candidate) return;
      const prepared = prepareLocalIceCandidate(
        candidate.toJSON() as IceCandidatePayload,
        this.networkPreference,
        this.networkInterfaces,
      );
      if (!prepared) {
        logger.info("[rtc] VPN candidate skipped by direct-internet preference", { peerId });
        return;
      }
      void Promise.resolve(this.events.sendIce(peerId, prepared)).catch((error) => logger.warn("ICE signaling failed", { peerId, error: String(error) }));
    };
    pc.ontrack = ({ streams }) => {
      const stream = streams[0];
      if (!stream) return;
      if (!this.mediaAuthorizationRequired || this.mediaAuthorizedPeers.has(peerId)) {
        this.rememberActiveRemoteStream(peerId, stream);
        this.events.onRemoteStream(peerId, stream);
        return;
      }
      // Mídia recebida de um peer ainda não autenticado nunca chega à UI nem ao
      // mixer. Desabilitar as tracks também impede reprodução acidental enquanto
      // a prova bilateral percorre o DataChannel de controle.
      stream.getTracks().forEach((track) => { track.enabled = false; });
      const pending = this.pendingRemoteStreams.get(peerId) ?? new Map<string, MediaStream>();
      pending.set(stream.id, stream);
      this.pendingRemoteStreams.set(peerId, pending);
    };
    pc.ondatachannel = ({ channel }) => {
      if (channel.label === TRANSFER_CHANNEL_LABEL) this.bindTransferDataChannel(peerId, entry, channel);
      else if (channel.label === CONTROL_CHANNEL_LABEL || channel.label === "risk.control") this.bindControlDataChannel(peerId, entry, channel);
      else channel.close();
    };
    pc.onnegotiationneeded = () => {
      if (!entry.canNegotiate) {
        entry.needsNegotiation = true;
        return;
      }
      if (entry.makingOffer || entry.pc.signalingState !== "stable") {
        entry.needsNegotiation = true;
        return;
      }
      entry.needsNegotiation = true;
      void this.negotiateIfNeeded(peerId, entry).catch((error) => {
        logger.warn("WebRTC negotiationneeded failed", { peerId, error: String(error) });
        this.events.onNegotiationError?.(peerId, error);
      });
    };
    // Vigia da conexão inicial: se nunca chegar a "connected", reinicia o ICE.
    this.armConnectWatchdog(peerId, entry);
    pc.onconnectionstatechange = () => {
      this.events.onConnectionState(peerId, pc.connectionState);
      if (pc.connectionState === "connecting") this.armConnectWatchdog(peerId, entry);
      if (pc.connectionState === "connected") this.clearPeerRecovery(peerId);
      if (pc.connectionState === "disconnected" || pc.connectionState === "failed") {
        this.schedulePeerRecovery(
          peerId,
          entry,
          pc.connectionState === "failed" ? 0 : DISCONNECTED_RECOVERY_DELAY_MS,
        );
      }
    };
    return entry;
  }

  private schedulePeerRecovery(peerId: string, entry: PeerEntry, delayMs: number): void {
    // O lado de id menor inicia a recuperação. O de id maior só entra depois de
    // uma folga, caso o outro lado não perceba a queda (estado "connected" preso).
    // O glare é resolvido pela negociação perfeita (peer educado/impolido).
    if (this.peers.get(peerId) !== entry
      || entry.pc.connectionState === "connected"
      || entry.pc.connectionState === "closed"
      || this.recoveryTimers.has(peerId)) return;
    const effectiveDelay = delayMs + (this.localPeerId >= peerId ? NON_INITIATOR_RECOVERY_EXTRA_MS : 0);
    const timer = setTimeout(() => {
      this.recoveryTimers.delete(peerId);
      void this.recoverPeerConnection(peerId, entry).finally(() => {
        const current = this.peers.get(peerId);
        if (!current || current.pc.connectionState === "connected" || current.pc.connectionState === "closed") return;
        const attempts = this.recoveryAttempts.get(peerId) ?? 0;
        const retryDelay = Math.min(MAX_RECOVERY_DELAY_MS, 2_000 * 2 ** Math.min(attempts, 4));
        this.schedulePeerRecovery(peerId, current, retryDelay);
      });
    }, effectiveDelay);
    this.recoveryTimers.set(peerId, timer);
  }

  private async recoverPeerConnection(peerId: string, observedEntry: PeerEntry): Promise<void> {
    const entry = this.peers.get(peerId);
    if (!entry || entry !== observedEntry || entry.pc.connectionState === "connected" || entry.pc.connectionState === "closed") return;
    const attempt = (this.recoveryAttempts.get(peerId) ?? 0) + 1;
    this.recoveryAttempts.set(peerId, attempt);
    try {
      if (attempt % RECREATE_PEER_EVERY_ATTEMPTS === 0) {
        await this.recreatePeerForRecovery(peerId, entry, attempt);
      } else {
        await this.restartIce(peerId);
      }
    } catch (error) {
      logger.warn("WebRTC automatic recovery attempt failed", { peerId, attempt, error: String(error) });
    }
  }

  private async recreatePeerForRecovery(peerId: string, entry: PeerEntry, attempt: number): Promise<void> {
    if (this.peers.get(peerId) !== entry) return;
    logger.warn("Recreating stalled WebRTC peer for recovery", { peerId, attempt });
    const hadTransferChannel = Boolean(entry.transferDataChannel);
    this.disposePeerEntry(peerId, entry, true);
    this.recoveryAttempts.set(peerId, attempt);
    const replacement = this.createPeer(peerId);
    replacement.initiator = true;
    replacement.canNegotiate = true;
    if (this.events.onDataMessage && !replacement.dataChannel) {
      this.bindControlDataChannel(peerId, replacement, replacement.pc.createDataChannel(CONTROL_CHANNEL_LABEL, { ordered: true }));
    }
    if (hadTransferChannel && this.events.onTransferMessage && !replacement.transferDataChannel) {
      this.bindTransferDataChannel(peerId, replacement, replacement.pc.createDataChannel(TRANSFER_CHANNEL_LABEL, { ordered: true }));
    }
    this.events.onPeerReset?.(peerId);
    replacement.needsIceRestart = true;
    replacement.pc.restartIce();
    await this.negotiateIfNeeded(peerId, replacement);
    await this.applyAdaptiveVideoParameters();
  }

  private armConnectWatchdog(peerId: string, entry: PeerEntry): void {
    if (this.connectWatchdogs.has(peerId)) return;
    const timer = setTimeout(() => {
      this.connectWatchdogs.delete(peerId);
      const state = entry.pc.connectionState;
      if (this.peers.get(peerId) !== entry || state === "connected" || state === "closed") return;
      this.schedulePeerRecovery(peerId, entry, 0);
    }, CONNECT_WATCHDOG_MS);
    this.connectWatchdogs.set(peerId, timer);
  }

  private clearConnectWatchdog(peerId: string): void {
    const timer = this.connectWatchdogs.get(peerId);
    if (timer) clearTimeout(timer);
    this.connectWatchdogs.delete(peerId);
  }

  private clearPeerRecovery(peerId: string): void {
    this.clearConnectWatchdog(peerId);
    const timer = this.recoveryTimers.get(peerId);
    if (timer) clearTimeout(timer);
    this.recoveryTimers.delete(peerId);
    this.recoveryAttempts.delete(peerId);
  }

  private applyAdaptiveVideoParameters(): Promise<void> {
    const operation = this.videoParameterUpdate.then(() => this.applyAdaptiveVideoParametersNow());
    this.videoParameterUpdate = operation.catch(() => undefined);
    return operation;
  }

  private async applyAdaptiveVideoParametersNow(): Promise<void> {
    const activePeers = Math.max(1, this.mediaAuthorizationRequired ? this.mediaAuthorizedPeers.size : this.peers.size);
    const screenPublished = [...this.localTracks.values()].some(({ track, video }) => track.kind === "video" && video?.source === "screen");
    await Promise.all([...this.peers.values()].flatMap(({ pc }) => pc.getSenders().filter((sender) => sender.track?.kind === "video").map(async (sender) => {
      const track = sender.track;
      if (!track) return;
      const publication = this.localTracks.get(track.id);
      const capture = typeof track.getSettings === "function" ? track.getSettings() : {};
      const policy = resolveVideoSenderPolicy(publication?.video, activePeers, capture, screenPublished);
      try {
        const parameters = sender.getParameters();
        parameters.encodings = parameters.encodings?.length ? parameters.encodings : [{}];
        const encoding = parameters.encodings[0]!;
        encoding.maxBitrate = policy.maxBitrate;
        encoding.priority = policy.priority;
        encoding.networkPriority = policy.priority;
        if (policy.maxFramerate !== undefined) encoding.maxFramerate = policy.maxFramerate;
        else delete encoding.maxFramerate;
        if (policy.scaleResolutionDownBy !== undefined) encoding.scaleResolutionDownBy = policy.scaleResolutionDownBy;
        else delete encoding.scaleResolutionDownBy;
        parameters.degradationPreference = policy.degradationPreference;
        await sender.setParameters(parameters);
      } catch (error) {
        logger.warn("Não foi possível aplicar os parâmetros adaptativos de vídeo", {
          trackId: track.id,
          source: publication?.video?.source ?? "camera",
          error: String(error),
        });
      }
    })));
  }

  private bindControlDataChannel(peerId: string, entry: PeerEntry, channel: RTCDataChannel): void {
    const previous = entry.dataChannel;
    entry.dataChannel = channel;
    if (previous && previous !== channel) previous.close();
    const notifyState = () => {
      // O fechamento de um canal substituído não pode derrubar a autenticação do
      // canal novo. Só a geração atualmente associada ao peer publica seu estado.
      if (this.peers.get(peerId) === entry && entry.dataChannel === channel) {
        this.events.onDataState?.(peerId, channel.readyState);
      }
    };
    channel.onopen = notifyState;
    channel.onclose = notifyState;
    channel.onerror = notifyState;
    channel.onmessage = ({ data }) => {
      if (typeof data === "string" && encodedMessageSize(data) <= MAX_CONTROL_MESSAGE_BYTES) this.events.onDataMessage?.(peerId, data);
    };
  }

  private bindTransferDataChannel(peerId: string, entry: PeerEntry, channel: RTCDataChannel): void {
    const previous = entry.transferDataChannel;
    entry.transferDataChannel = channel;
    if (previous && previous !== channel) previous.close();
    channel.binaryType = "arraybuffer";
    channel.bufferedAmountLowThreshold = TRANSFER_LOW_WATER_MARK_BYTES;
    const notifyState = () => {
      if (this.peers.get(peerId) === entry && entry.transferDataChannel === channel) {
        this.events.onTransferState?.(peerId, channel.readyState);
      }
    };
    channel.onopen = notifyState;
    channel.onclose = notifyState;
    channel.onerror = notifyState;
    channel.onmessage = ({ data }) => {
      if (data instanceof ArrayBuffer) {
        if (data.byteLength <= MAX_TRANSFER_FRAME_BYTES) this.events.onTransferMessage?.(peerId, data);
        return;
      }
      if (data instanceof Blob && data.size <= MAX_TRANSFER_FRAME_BYTES) {
        void data.arrayBuffer().then((buffer) => this.events.onTransferMessage?.(peerId, buffer)).catch(() => undefined);
      }
    };
  }

  private async acceptDescription(peerId: string, description: RTCSessionDescriptionInit): Promise<void> {
    const entry = this.peers.get(peerId) ?? this.createPeer(peerId);
    const operation = entry.descriptionChain.then(async () => {
      if (this.peers.get(peerId) !== entry) return;
      await this.acceptDescriptionNow(peerId, entry, description);
    });
    entry.descriptionChain = operation.catch(() => undefined);
    await operation;
  }

  private async acceptDescriptionNow(peerId: string, entry: PeerEntry, description: RTCSessionDescriptionInit): Promise<void> {
    const { pc } = entry;
    const readyForOffer = !entry.makingOffer && (pc.signalingState === "stable" || entry.settingRemoteAnswer);
    const offerCollision = description.type === "offer" && !readyForOffer;
    const polite = this.localPeerId > peerId;
    entry.ignoreOffer = !polite && offerCollision;
    if (entry.ignoreOffer) return;
    entry.settingRemoteAnswer = description.type === "answer";
    try {
      try {
        if (offerCollision && pc.signalingState !== "stable") {
          await Promise.all([pc.setLocalDescription({ type: "rollback" }), pc.setRemoteDescription(description)]);
        } else {
          await pc.setRemoteDescription(description);
        }
      } catch (error) {
        if (description.type === "offer" && isMLineOrderMismatch(error) && this.peers.get(peerId) === entry) {
          await this.recoverPeerFromMLineMismatch(peerId, entry, description);
          return;
        }
        throw error;
      }
    } finally { entry.settingRemoteAnswer = false; }
    await flushPendingIceCandidates(entry);
    entry.canNegotiate = true;
    if (description.type === "offer") {
      entry.needsNegotiation = false;
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      if (pc.localDescription) await this.events.sendAnswer(peerId, this.prepareLocalDescription(pc.localDescription.toJSON()));
    } else {
      await this.negotiateIfNeeded(peerId, entry);
    }
  }

  private async recoverPeerFromMLineMismatch(peerId: string, entry: PeerEntry, offer: RTCSessionDescriptionInit): Promise<void> {
    logger.warn("Resetting one WebRTC peer after stale SDP m-line order", { peerId });
    const initiator = entry.initiator;
    const hadTransferChannel = Boolean(entry.transferDataChannel);
    this.disposePeerEntry(peerId, entry);
    const replacement = this.createPeer(peerId);
    replacement.initiator = initiator;
    replacement.canNegotiate = true;
    if (this.events.onDataMessage && !replacement.dataChannel) {
      this.bindControlDataChannel(peerId, replacement, replacement.pc.createDataChannel(CONTROL_CHANNEL_LABEL, { ordered: true }));
    }
    if (hadTransferChannel && this.events.onTransferMessage && !replacement.transferDataChannel) {
      this.bindTransferDataChannel(peerId, replacement, replacement.pc.createDataChannel(TRANSFER_CHANNEL_LABEL, { ordered: true }));
    }
    this.events.onPeerReset?.(peerId);
    await replacement.pc.setRemoteDescription(offer);
    await flushPendingIceCandidates(replacement);
    replacement.needsNegotiation = false;
    const answer = await replacement.pc.createAnswer();
    await replacement.pc.setLocalDescription(answer);
    if (replacement.pc.localDescription) await this.events.sendAnswer(peerId, this.prepareLocalDescription(replacement.pc.localDescription.toJSON()));
  }

  private disposePeerEntry(peerId: string, entry: PeerEntry, preserveRecovery = false): void {
    if (this.peers.get(peerId) === entry) this.peers.delete(peerId);
    entry.pc.onicecandidate = null;
    entry.pc.ontrack = null;
    entry.pc.onnegotiationneeded = null;
    entry.pc.onconnectionstatechange = null;
    entry.pendingIceCandidates.length = 0;
    entry.dataChannel?.close();
    entry.transferDataChannel?.close();
    entry.pc.close();
    this.mediaAuthorizedPeers.delete(peerId);
    const pendingStreams = this.pendingRemoteStreams.get(peerId);
    pendingStreams?.forEach((stream) => stream.getTracks().forEach((track) => { track.enabled = false; }));
    this.pendingRemoteStreams.delete(peerId);
    const activeStreams = this.activeRemoteStreams.get(peerId);
    activeStreams?.forEach((stream) => stream.getTracks().forEach((track) => { track.enabled = false; }));
    this.activeRemoteStreams.delete(peerId);
    this.previousOutboundBytes.delete(peerId);
    this.previousConnectionPaths.delete(peerId);
    const timer = this.recoveryTimers.get(peerId);
    if (timer) clearTimeout(timer);
    this.recoveryTimers.delete(peerId);
    this.clearConnectWatchdog(peerId);
    if (!preserveRecovery) this.recoveryAttempts.delete(peerId);
  }

  private requirePeer(peerId: string): PeerEntry {
    const entry = this.peers.get(peerId);
    if (!entry) throw new Error(`Peer desconhecido: ${peerId}`);
    return entry;
  }

  private rememberActiveRemoteStream(peerId: string, stream: MediaStream): void {
    const streams = this.activeRemoteStreams.get(peerId) ?? new Map<string, MediaStream>();
    streams.set(stream.id, stream);
    this.activeRemoteStreams.set(peerId, streams);
  }

  private async negotiateIfNeeded(peerId: string, entry: PeerEntry): Promise<void> {
    if ((!entry.needsNegotiation && !entry.needsIceRestart)
      || !entry.canNegotiate
      || entry.makingOffer
      || entry.pc.signalingState !== "stable") return;
    const needsNegotiation = entry.needsNegotiation;
    const needsIceRestart = entry.needsIceRestart;
    entry.needsNegotiation = false;
    entry.needsIceRestart = false;
    try {
      const sent = await this.negotiate(peerId, entry, needsIceRestart);
      if (!sent) {
        entry.needsNegotiation ||= needsNegotiation;
        entry.needsIceRestart ||= needsIceRestart;
      }
    } catch (error) {
      entry.needsNegotiation ||= needsNegotiation;
      entry.needsIceRestart ||= needsIceRestart;
      throw error;
    }
  }

  private async negotiate(peerId: string, entry: PeerEntry, iceRestart = false): Promise<boolean> {
    if (entry.makingOffer || entry.pc.signalingState !== "stable") {
      return false;
    }
    entry.makingOffer = true;
    try {
      const offer = await entry.pc.createOffer({ iceRestart });
      if (entry.pc.signalingState !== "stable") {
        return false;
      }
      await entry.pc.setLocalDescription(offer);
      if (entry.pc.localDescription) await this.events.sendOffer(peerId, this.prepareLocalDescription(entry.pc.localDescription.toJSON()));
      return true;
    } catch (error) {
      logger.warn("WebRTC renegotiation failed", { peerId, error: String(error) });
      throw error;
    } finally { entry.makingOffer = false; }
  }

  private prepareLocalDescription(description: RTCSessionDescriptionInit): RTCSessionDescriptionInit {
    return prepareLocalSessionDescription(description, this.networkPreference, this.networkInterfaces);
  }
}

export function defaultPeerState(): PeerState { return { microphone: true, camera: false, screenShare: false }; }

// Nome curto para novos consumidores; o nome anterior continua exportado para
// chat, convites e integrações já existentes.
export { MeshWebRTCTransport as MeshTransport };

function normalizeMeshTransportOptions(
  options: number | readonly NetworkInterfaceDescriptor[] | MeshTransportOptions,
): MeshTransportOptions {
  if (typeof options === "number") return { maxRemotePeers: options };
  if (Array.isArray(options)) return { networkInterfaces: options as readonly NetworkInterfaceDescriptor[] };
  return options as MeshTransportOptions;
}
