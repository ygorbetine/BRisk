import type {
  AnswerMessage,
  IceCandidateMessage,
  OfferMessage,
  PeerStateMessage,
} from "./types";

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const ROOM = /^[0-9a-f]{64}$/;
const MAX_AGE_MS = 120_000;
const MAX_FUTURE_MS = 30_000;
const MAX_ENVELOPE_BYTES = 65_536;

type RecordValue = Record<string, unknown>;

function record(value: unknown): value is RecordValue {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function validBase(value: unknown, expectedType: string): value is RecordValue {
  if (!record(value)) return false;
  let size = MAX_ENVELOPE_BYTES + 1;
  try { size = new TextEncoder().encode(JSON.stringify(value)).byteLength; } catch { return false; }
  const now = Date.now();
  return size <= MAX_ENVELOPE_BYTES
    && value.version === 1
    && value.type === expectedType
    && typeof value.roomId === "string" && ROOM.test(value.roomId)
    && typeof value.fromPeerId === "string" && UUID.test(value.fromPeerId)
    && (value.targetPeerId === undefined || (typeof value.targetPeerId === "string" && UUID.test(value.targetPeerId)))
    && typeof value.messageId === "string" && UUID.test(value.messageId)
    && typeof value.timestamp === "number"
    && Number.isFinite(value.timestamp)
    && value.timestamp >= now - MAX_AGE_MS
    && value.timestamp <= now + MAX_FUTURE_MS
    && record(value.payload);
}

function validDescription(value: unknown, type: "offer" | "answer"): value is RTCSessionDescriptionInit {
  return record(value)
    && value.type === type
    && typeof value.sdp === "string"
    && value.sdp.length > 0
    && value.sdp.length <= 60_000;
}

export function parseOffer(value: unknown): OfferMessage | null {
  if (!validBase(value, "webrtc.offer") || typeof value.targetPeerId !== "string") return null;
  const payload = value.payload as RecordValue;
  return validDescription(payload.sdp, "offer") ? value as OfferMessage : null;
}

export function parseAnswer(value: unknown): AnswerMessage | null {
  if (!validBase(value, "webrtc.answer") || typeof value.targetPeerId !== "string") return null;
  const payload = value.payload as RecordValue;
  return validDescription(payload.sdp, "answer") ? value as AnswerMessage : null;
}

export function parseIceCandidate(value: unknown): IceCandidateMessage | null {
  if (!validBase(value, "webrtc.ice-candidate") || typeof value.targetPeerId !== "string") return null;
  const payload = value.payload as RecordValue;
  if (!record(payload.candidate)) return null;
  const candidate = payload.candidate;
  const valid = typeof candidate.candidate === "string"
    && candidate.candidate.length <= 4_096
    && (candidate.sdpMid === undefined || candidate.sdpMid === null || (typeof candidate.sdpMid === "string" && candidate.sdpMid.length <= 256))
    && (candidate.sdpMLineIndex === undefined || candidate.sdpMLineIndex === null || (Number.isInteger(candidate.sdpMLineIndex) && Number(candidate.sdpMLineIndex) >= 0))
    && (candidate.usernameFragment === undefined || candidate.usernameFragment === null || (typeof candidate.usernameFragment === "string" && candidate.usernameFragment.length <= 256));
  return valid ? value as IceCandidateMessage : null;
}

export function parsePeerState(value: unknown): PeerStateMessage | null {
  if (!validBase(value, "peer.state")) return null;
  const payload = value.payload as RecordValue;
  if (!record(payload.state)) return null;
  const state = payload.state;
  const valid = typeof state.microphone === "boolean"
    && typeof state.camera === "boolean"
    && typeof state.screenShare === "boolean"
    && (state.cameraStreamId === undefined || typeof state.cameraStreamId === "string")
    && (state.screenStreamId === undefined || typeof state.screenStreamId === "string")
    && (state.secondScreenStreamId === undefined || typeof state.secondScreenStreamId === "string")
    && (state.screenAudio === undefined || typeof state.screenAudio === "boolean");
  return valid ? value as PeerStateMessage : null;
}

export function isValidPeer(value: unknown): value is { peerId: string; joinedAt: number; clientVersion?: string } {
  if (!record(value)) return false;
  return typeof value.peerId === "string" && UUID.test(value.peerId)
    && typeof value.joinedAt === "number" && Number.isFinite(value.joinedAt)
    && (value.clientVersion === undefined || (typeof value.clientVersion === "string" && value.clientVersion.length <= 32));
}
