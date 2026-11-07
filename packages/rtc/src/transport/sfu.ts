import {
  ConnectionState,
  Room,
  RoomEvent,
  Track,
  type LocalTrackPublication,
  type RemoteParticipant,
  type RemoteTrack,
  type TrackPublishOptions,
} from "livekit-client";
import type { VideoPublicationOptions } from "../video-encoding";
import type {
  CallTransportJoinOptions,
  PeerConnectionDiagnostics,
  SfuCallTransport,
  TransportEvents,
} from "./call-transport";

export type SfuConnectionInfo = { url: string; token: string };

/** Obtém URL e token do servidor SFU. O segredo da API nunca fica no cliente. */
export type SfuConnectionProvider = (options: CallTransportJoinOptions) => Promise<SfuConnectionInfo>;

const DEFAULT_SCREEN_BITRATE = 6_000_000;
const DEFAULT_CAMERA_BITRATE = 1_500_000;

/**
 * Transporte SFU baseado em LiveKit. Cada participante publica uma vez e o
 * servidor distribui; o upload deixa de crescer com o número de espectadores.
 * O `peerId` do Briks é a identidade do participante no LiveKit.
 */
export class LiveKitTransport implements SfuCallTransport {
  readonly kind = "sfu" as const;
  private room: Room | null = null;
  private readonly streams = new Map<string, MediaStream>();
  private readonly published = new Map<MediaStreamTrack, LocalTrackPublication>();
  private readonly publishedStreams = new Map<MediaStreamTrack, MediaStream>();
  private readonly encoder = new TextEncoder();
  private readonly decoder = new TextDecoder();

  constructor(
    readonly localPeerId: string,
    private readonly events: TransportEvents,
    private readonly provider: SfuConnectionProvider,
  ) {}

  async join(options: CallTransportJoinOptions): Promise<void> {
    if (this.room) return;
    const { url, token } = await this.provider(options);
    const room = new Room({ adaptiveStream: true, dynacast: true });
    this.bind(room);
    this.room = room;
    try {
      await room.connect(url, token);
    } catch (error) {
      this.room = null;
      throw error;
    }
    for (const participant of room.remoteParticipants.values()) this.adoptParticipant(participant);
  }

  async leave(): Promise<void> {
    const room = this.room;
    this.room = null;
    this.published.clear();
    this.streams.clear();
    if (room) await room.disconnect();
  }

  // Admissão/assinatura é feita pelo servidor: conectar um peer é só reportar o estado.
  async connectPeer(peerId: string): Promise<void> {
    const participant = this.room?.remoteParticipants.get(peerId);
    if (participant) this.adoptParticipant(participant);
  }

  async disconnectPeer(peerId: string): Promise<void> {
    this.dropStreams(peerId);
    this.events.onPeerReset?.(peerId);
  }

  async recoverPeer(peerId: string): Promise<void> {
    const participant = this.room?.remoteParticipants.get(peerId);
    if (participant) {
      for (const publication of participant.trackPublications.values()) {
        if (publication.isSubscribed === false) publication.setSubscribed(true);
      }
    }
  }

  async publishTrack(track: MediaStreamTrack, stream: MediaStream, options?: VideoPublicationOptions): Promise<void> {
    const room = this.requireRoom();
    if (this.published.has(track)) return;
    this.publishedStreams.set(track, stream);
    const publication = await room.localParticipant.publishTrack(track, { ...this.publishOptions(track, options), name: stream.id });
    this.published.set(track, publication);
  }

  async configurePublishedVideoTrack(track: MediaStreamTrack, options: VideoPublicationOptions): Promise<void> {
    // O LiveKit fixa a codificação na publicação; republica com os novos limites.
    const publication = this.published.get(track);
    if (!publication) return;
    await this.unpublishTrack(track);
    await this.publishTrack(track, this.publishedStreams.get(track) ?? new MediaStream([track]), options);
  }

  async unpublishTrack(track: MediaStreamTrack): Promise<void> {
    const room = this.room;
    const publication = this.published.get(track);
    this.published.delete(track);
    if (!room || !publication) return;
    await room.localParticipant.unpublishTrack(track, false);
  }

  async replaceTrack(kind: "audio" | "video", track: MediaStreamTrack | null): Promise<void> {
    for (const [previous, publication] of this.published) {
      if (previous.kind !== kind) continue;
      if (track) await publication.track?.replaceTrack(track);
      return;
    }
  }

  async setMicrophoneMuted(muted: boolean): Promise<void> {
    for (const [track, publication] of this.published) {
      if (track.kind !== "audio" || publication.source !== Track.Source.Microphone) continue;
      if (muted) await publication.mute(); else await publication.unmute();
    }
  }

  async replacePublishedTrack(previousTrack: MediaStreamTrack, nextTrack: MediaStreamTrack, _stream: MediaStream): Promise<void> {
    const publication = this.published.get(previousTrack);
    if (!publication?.track) return;
    await publication.track.replaceTrack(nextTrack);
    this.published.delete(previousTrack);
    this.published.set(nextTrack, publication);
  }

  sendData(data: string, targetPeerId?: string): number {
    const room = this.room;
    if (!room || room.state !== ConnectionState.Connected) return 0;
    const payload = this.encoder.encode(data);
    void room.localParticipant
      .publishData(payload, { reliable: true, destinationIdentities: targetPeerId ? [targetPeerId] : undefined })
      .catch(() => undefined);
    return payload.byteLength;
  }

  // Autorização de mídia por peer é do servidor (token por sala); nada a fazer aqui.
  requireMediaAuthorization(): void {}
  async authorizePeerMedia(): Promise<void> {}
  revokePeerMedia(): void {}

  getDiagnostics(): PeerConnectionDiagnostics[] {
    const room = this.room;
    if (!room) return [];
    return [...room.remoteParticipants.keys()].map((peerId) => this.diagnostic(peerId, room.state));
  }

  async collectDiagnostics(): Promise<PeerConnectionDiagnostics[]> {
    return this.getDiagnostics();
  }

  private requireRoom(): Room {
    if (!this.room) throw new Error("O transporte SFU não está conectado.");
    return this.room;
  }

  private publishOptions(track: MediaStreamTrack, options?: VideoPublicationOptions): TrackPublishOptions {
    if (track.kind === "audio") {
      return { source: options?.source === "screen" ? Track.Source.ScreenShareAudio : Track.Source.Microphone, dtx: true };
    }
    const screen = options?.source === "screen";
    return {
      source: screen ? Track.Source.ScreenShare : Track.Source.Camera,
      simulcast: true,
      degradationPreference: options?.degradationPreference,
      videoEncoding: {
        maxBitrate: options?.maxBitrate ?? (screen ? DEFAULT_SCREEN_BITRATE : DEFAULT_CAMERA_BITRATE),
        maxFramerate: options?.maxFramerate ?? (screen ? 30 : 30),
      },
      ...(screen ? { screenShareSimulcastLayers: [] } : {}),
    };
  }

  private bind(room: Room): void {
    room
      .on(RoomEvent.ParticipantConnected, (participant) => this.adoptParticipant(participant))
      .on(RoomEvent.ParticipantDisconnected, (participant) => {
        this.dropStreams(participant.identity);
        this.events.onConnectionState(participant.identity, "closed");
      })
      .on(RoomEvent.TrackSubscribed, (track, publication, participant) => this.attach(track, participant, publication.trackName))
      .on(RoomEvent.TrackUnsubscribed, (track, publication, participant) => this.detach(track, participant, publication.trackName))
      .on(RoomEvent.DataReceived, (payload, participant) => {
        if (participant) this.events.onDataMessage?.(participant.identity, this.decoder.decode(payload));
      })
      .on(RoomEvent.Reconnecting, () => {
        for (const peerId of room.remoteParticipants.keys()) this.events.onConnectionState(peerId, "connecting");
      })
      .on(RoomEvent.Reconnected, () => {
        for (const peerId of room.remoteParticipants.keys()) this.events.onConnectionState(peerId, "connected");
      });
  }

  private adoptParticipant(participant: RemoteParticipant): void {
    this.events.onConnectionState(participant.identity, "connected");
    this.events.onDataState?.(participant.identity, "open");
    for (const publication of participant.trackPublications.values()) {
      if (publication.track) this.attach(publication.track, participant, publication.trackName);
    }
  }

  // Cada fluxo publicado (câmera+mic, tela, segunda tela) vira um MediaStream próprio com o mesmo id do emissor,
  // porque o receptor separa tela e segunda tela pelo id do fluxo anunciado no estado do peer.
  private streamFor(peerId: string, name: string): MediaStream {
    const key = `${peerId}|${name}`;
    let stream = this.streams.get(key);
    if (!stream) {
      stream = new MediaStream();
      if (name) Object.defineProperty(stream, "id", { value: name, configurable: true });
      this.streams.set(key, stream);
    }
    return stream;
  }

  private dropStreams(peerId: string): void {
    for (const key of [...this.streams.keys()]) if (key.startsWith(`${peerId}|`)) this.streams.delete(key);
  }

  private attach(track: RemoteTrack, participant: RemoteParticipant, name = ""): void {
    const peerId = participant.identity;
    const stream = this.streamFor(peerId, name);
    const mediaTrack = track.mediaStreamTrack;
    if (!stream.getTracks().includes(mediaTrack)) stream.addTrack(mediaTrack);
    this.events.onRemoteStream(peerId, stream);
  }

  private detach(track: RemoteTrack, participant: RemoteParticipant, name = ""): void {
    const stream = this.streams.get(`${participant.identity}|${name}`);
    if (!stream) return;
    stream.removeTrack(track.mediaStreamTrack);
    this.events.onRemoteStream(participant.identity, stream);
  }

  private diagnostic(peerId: string, state: ConnectionState): PeerConnectionDiagnostics {
    const connected = state === ConnectionState.Connected;
    return {
      peerId,
      connectionState: connected ? "connected" : "connecting",
      iceConnectionState: connected ? "connected" : "checking",
      signalingState: "stable",
      pendingIceCandidates: 0,
      dataChannelState: connected ? "open" : "connecting",
      transferDataChannelState: "unavailable",
      selectedConnectionPath: { kind: "unknown", label: "Servidor SFU" },
    };
  }
}
