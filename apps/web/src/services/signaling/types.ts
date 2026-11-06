import type { PeerState } from "@briks/protocol";

export type SignalingStatus = "connecting" | "connected" | "reconnecting" | "disconnected" | "error";
export type SignalingNamespace = "room" | "chat" | "friend" | "group";

export type SignalingPeer = {
  peerId: string;
  joinedAt: number;
  clientVersion?: string;
};

export type OfferPayload = { sdp: RTCSessionDescriptionInit };
export type AnswerPayload = { sdp: RTCSessionDescriptionInit };
export type IceCandidatePayload = { candidate: RTCIceCandidateInit };
export type PeerStatePayload = { state: PeerState };

export type SignalingEnvelope<Type extends string, Payload> = {
  version: 1;
  roomId: string;
  fromPeerId: string;
  targetPeerId?: string;
  messageId: string;
  timestamp: number;
  type: Type;
  payload: Payload;
};

export type OfferMessage = SignalingEnvelope<"webrtc.offer", OfferPayload>;
export type AnswerMessage = SignalingEnvelope<"webrtc.answer", AnswerPayload>;
export type IceCandidateMessage = SignalingEnvelope<"webrtc.ice-candidate", IceCandidatePayload>;
export type PeerStateMessage = SignalingEnvelope<"peer.state", PeerStatePayload>;

export type SignalingDiagnostics = {
  status: SignalingStatus;
  channelStatus: string;
  peerId: string | null;
  roomId: string | null;
  connectedPeers: string[];
  presencePeers: string[];
  processedMessages: number;
  clockSkewMs: number | null;
  clockSkewRejectedMessages: number;
};

export interface SignalingProvider {
  connect(roomId: string, peerId: string, namespace?: SignalingNamespace): Promise<void>;
  disconnect(): Promise<void>;
  sendOffer(targetPeerId: string, offer: RTCSessionDescriptionInit): Promise<void>;
  sendAnswer(targetPeerId: string, answer: RTCSessionDescriptionInit): Promise<void>;
  sendIceCandidate(targetPeerId: string, candidate: RTCIceCandidateInit): Promise<void>;
  sendPeerState(state: PeerState): Promise<void>;
  onPeerJoined(callback: (peer: SignalingPeer) => void): () => void;
  onPeerLeft(callback: (peerId: string) => void): () => void;
  onOffer(callback: (message: OfferMessage) => void): () => void;
  onAnswer(callback: (message: AnswerMessage) => void): () => void;
  onIceCandidate(callback: (message: IceCandidateMessage) => void): () => void;
  onPeerState(callback: (message: PeerStateMessage) => void): () => void;
  onStatusChange(callback: (status: SignalingStatus) => void): () => void;
  getDiagnostics(): SignalingDiagnostics;
}
