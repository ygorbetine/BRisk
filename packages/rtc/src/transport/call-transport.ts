import type { IceCandidatePayload } from "@briks/protocol";
import type { NetworkInterfaceDescriptor, SelectedConnectionPath } from "../connection-path";
import type { VideoPublicationOptions } from "../video-encoding";
import type { RtcNetworkPreference } from "./network-policy";

export type CallTransportKind = "mesh" | "sfu";
export type CallTransportPreference = "auto" | CallTransportKind;

export type CallTransportJoinOptions = {
  roomId: string;
  localPeerId: string;
  accessToken?: string;
};

export interface TransportEvents {
  sendOffer(peerId: string, description: RTCSessionDescriptionInit): void | Promise<void>;
  sendAnswer(peerId: string, description: RTCSessionDescriptionInit): void | Promise<void>;
  sendIce(peerId: string, candidate: IceCandidatePayload): void | Promise<void>;
  onRemoteStream(peerId: string, stream: MediaStream): void;
  onConnectionState(peerId: string, state: RTCPeerConnectionState): void;
  onNegotiationError?(peerId: string, error: unknown): void;
  onPeerReset?(peerId: string): void;
  onDataMessage?(peerId: string, data: string): void;
  onDataState?(peerId: string, state: RTCDataChannelState): void;
  onTransferMessage?(peerId: string, data: ArrayBuffer): void;
  onTransferState?(peerId: string, state: RTCDataChannelState): void;
}

export type PeerConnectionDiagnostics = {
  peerId: string;
  connectionState: RTCPeerConnectionState;
  iceConnectionState: RTCIceConnectionState;
  signalingState: RTCSignalingState;
  pendingIceCandidates: number;
  dataChannelState: RTCDataChannelState | "unavailable";
  transferDataChannelState: RTCDataChannelState | "unavailable";
  roundTripTimeMs?: number;
  packetsLost?: number;
  packetLossPercent?: number;
  jitterMs?: number;
  outboundBitrateKbps?: number;
  selectedConnectionPath: SelectedConnectionPath;
};

/**
 * Contrato estável entre a chamada e sua topologia de mídia. Uma implementação
 * SFU pode gerenciar `connectPeer` como admissão/assinatura sem expor SDP à UI.
 */
export interface CallTransport {
  readonly kind: CallTransportKind;
  join(options: CallTransportJoinOptions): Promise<void>;
  leave(): Promise<void>;
  connectPeer(peerId: string, initiator: boolean): Promise<void>;
  disconnectPeer(peerId: string): Promise<void>;
  recoverPeer(peerId: string): Promise<void>;
  publishTrack(track: MediaStreamTrack, stream: MediaStream, options?: VideoPublicationOptions): Promise<void>;
  configurePublishedVideoTrack(track: MediaStreamTrack, options: VideoPublicationOptions): Promise<void>;
  unpublishTrack(track: MediaStreamTrack): Promise<void>;
  replaceTrack(kind: "audio" | "video", track: MediaStreamTrack | null): Promise<void>;
  replacePublishedTrack(previousTrack: MediaStreamTrack, nextTrack: MediaStreamTrack, stream: MediaStream): Promise<void>;
  sendData(data: string, targetPeerId?: string): number;
  requireMediaAuthorization(): void;
  authorizePeerMedia(peerId: string): Promise<void>;
  revokePeerMedia(peerId: string): void;
  getDiagnostics(): PeerConnectionDiagnostics[];
  collectDiagnostics(): Promise<PeerConnectionDiagnostics[]>;
}

/** Operações SDP/ICE exclusivas do transporte P2P Mesh. */
export interface MeshCallTransport extends CallTransport {
  readonly kind: "mesh";
  acceptOffer(peerId: string, description: RTCSessionDescriptionInit): Promise<void>;
  acceptAnswer(peerId: string, description: RTCSessionDescriptionInit): Promise<void>;
  addIceCandidate(peerId: string, candidate: RTCIceCandidateInit): Promise<void>;
  restartIce(peerId: string): Promise<void>;
  /** Reinicia o ICE de todos os peers após uma mudança de rede local. */
  notifyNetworkChange?(): Promise<void>;
}

/** Contrato nominal para a implementação do provedor SFU escolhido no futuro. */
export interface SfuCallTransport extends CallTransport {
  readonly kind: "sfu";
}

export function isMeshCallTransport(transport: CallTransport): transport is MeshCallTransport {
  return transport.kind === "mesh";
}

export function isSfuCallTransport(transport: CallTransport): transport is SfuCallTransport {
  return transport.kind === "sfu";
}

export type CallTransportCreationContext = {
  localPeerId: string;
  iceServers: RTCIceServer[];
  events: TransportEvents;
  networkInterfaces?: readonly NetworkInterfaceDescriptor[];
  networkPreference?: RtcNetworkPreference;
};

export type CallTransportCreator = (context: CallTransportCreationContext) => CallTransport;

export class CallTransportRegistry {
  private readonly creators: ReadonlyMap<CallTransportKind, CallTransportCreator>;

  constructor(creators: Partial<Record<CallTransportKind, CallTransportCreator>>) {
    this.creators = new Map(
      (Object.entries(creators) as Array<[CallTransportKind, CallTransportCreator | undefined]>)
        .filter((entry): entry is [CallTransportKind, CallTransportCreator] => typeof entry[1] === "function"),
    );
    if (!this.creators.has("mesh")) throw new Error("O registro de transportes precisa oferecer Mesh como fallback.");
  }

  has(kind: CallTransportKind): boolean {
    return this.creators.has(kind);
  }

  create(kind: CallTransportKind, context: CallTransportCreationContext): CallTransport {
    const creator = this.creators.get(kind);
    if (!creator) throw new Error(`Transporte ${kind.toUpperCase()} não está configurado.`);
    const transport = creator(context);
    if (transport.kind !== kind) throw new Error(`A fábrica ${kind.toUpperCase()} retornou um transporte incompatível.`);
    return transport;
  }
}
