import { P2P_CLOCK_SKEW_TOLERANCE_MS } from "../services/p2p-clock";
import {
  validBriksPeerCapabilities,
  type BriksPeerCapabilities,
} from "../services/protocol-compatibility";
import type { LocalChatMessage } from "../services/offline/chat-storage";
import { validAvatarDataUrl } from "../services/offline/profile";
import {
  validGroupAdministratorGrant,
  validGroupRevocationCertificate,
  type GroupAdministratorGrant,
  type GroupRevocationCertificate,
  type LocalGroupChannel,
  type PublicPeerIdentity,
} from "../services/offline/social-storage";

export type LegacyChatWireMessage = {
  version: 1;
  type: "chat.message";
  channelId: string;
  id: string;
  author: string;
  content: string;
  timestamp: number;
};

export type SignedChatWireMessage = {
  version: 2;
  type: "chat.message";
  channelId: string;
  id: string;
  authorPeerId: string;
  author: string;
  content: string;
  timestamp: number;
  signature: string;
};

export type ChatEventAction =
  | "reply"
  | "edit"
  | "delete"
  | "reaction.add"
  | "reaction.remove"
  | "pin"
  | "unpin";

export type SignedChatEventWireMessage = {
  version: 3;
  type: "chat.event";
  channelId: string;
  id: string;
  targetMessageId: string;
  actorPeerId: string;
  action: ChatEventAction;
  content?: string;
  referenceMessageId?: string;
  emoji?: string;
  timestamp: number;
  signature: string;
};

export type ChatTypingWireMessage = {
  version: 3;
  type: "chat.typing";
  channelId: string;
  actorPeerId: string;
  active: boolean;
  timestamp: number;
};

export type IdentityChallengeWireMessage = {
  version: 2;
  type: "chat.identity.challenge";
  channelId: string;
  fromPeerId: string;
  nonce: string;
  timestamp: number;
  capabilities: BriksPeerCapabilities;
};

export type IdentityProofWireMessage = {
  version: 2;
  type: "chat.identity.proof";
  channelId: string;
  fromPeerId: string;
  toPeerId: string;
  nonce: string;
  timestamp: number;
  capabilities: BriksPeerCapabilities;
  signature: string;
};

export type HistoryRequestWireMessage = {
  version: 2;
  type: "chat.history.request";
  channelId: string;
  requestId: string;
  knownIds: string[];
  knownEventIds?: string[];
  messageBefore?: string;
  messageBeforeId?: string;
  eventBeforeTimestamp?: number;
  eventBeforeId?: string;
  messagesDone?: boolean;
  eventsDone?: boolean;
};

export type HistoryChunkWireMessage = {
  version: 2;
  type: "chat.history.chunk";
  channelId: string;
  requestId: string;
  messages: SignedChatWireMessage[];
  events?: SignedChatEventWireMessage[];
};

export type HistoryCompleteWireMessage = {
  version: 2;
  type: "chat.history.complete";
  channelId: string;
  requestId: string;
  nextMessageBefore?: string;
  nextMessageBeforeId?: string;
  nextEventBeforeTimestamp?: number;
  nextEventBeforeId?: string;
};

export type GroupMembersWireMessage = {
  version: 2;
  type: "chat.members.snapshot";
  channelId: string;
  groupId: string;
  senderPeerId: string;
  ownerPeerId: string;
  membershipVersion: number;
  manifestVersion: number;
  manifestActorPeerId: string;
  manifestOperationId: string;
  administratorEpoch: number;
  administratorGrants: GroupAdministratorGrant[];
  name: string;
  avatar?: string;
  channels: LocalGroupChannel[];
  administratorPeerIds: string[];
  removedPeerIds: string[];
  removedMembers: PublicPeerIdentity[];
  revocations: GroupRevocationCertificate[];
  rendezvousVersion: number;
  rendezvousSecret: string;
  members: PublicPeerIdentity[];
  timestamp: number;
  signature: string;
};

export type MessageAckWireMessage = {
  version: 2;
  type: "chat.message.ack";
  channelId: string;
  messageId: string;
};

export type GroupRevocationWireMessage = {
  version: 2;
  type: "chat.group.revocation";
  channelId: string;
  certificate: GroupRevocationCertificate;
};

export type ProfileUpdateWireMessage = {
  version: 2;
  type: "chat.profile.update";
  channelId: string;
  identity: PublicPeerIdentity;
  timestamp: number;
  signature: string;
};

export type ChatWireMessage = LegacyChatWireMessage | SignedChatWireMessage;
export type ChatWireEnvelope =
  | ChatWireMessage
  | IdentityChallengeWireMessage
  | IdentityProofWireMessage
  | HistoryRequestWireMessage
  | HistoryChunkWireMessage
  | HistoryCompleteWireMessage
  | GroupMembersWireMessage
  | ProfileUpdateWireMessage
  | GroupRevocationWireMessage
  | MessageAckWireMessage
  | SignedChatEventWireMessage
  | ChatTypingWireMessage;

export const MAX_WIRE_BYTES = 64 * 1024;
export const MAX_HISTORY_IDS = 200;
export const HISTORY_CHUNK_MESSAGES = 8;
export const HISTORY_CHUNK_EVENTS = 16;
export const MAX_GROUP_SYNC_MEMBERS = 48;
export const MAX_GROUP_SYNC_REVOCATIONS = 48;

const LEGACY_MESSAGE_MAX_AGE_MS = 120_000;
const LEGACY_FUTURE_CLOCK_SKEW_MS = 30_000;
const QUEUED_MESSAGE_MAX_AGE_MS = 30 * 24 * 60 * 60_000;

export function parseChatWireMessage(raw: string, channelId?: string): ChatWireMessage | null {
  const envelope = parseChatWireEnvelope(raw, channelId);
  return envelope?.type === "chat.message" ? envelope : null;
}

export function parseChatWireEnvelope(raw: string, channelId?: string): ChatWireEnvelope | null {
  if (new TextEncoder().encode(raw).byteLength > MAX_WIRE_BYTES) return null;
  let value: unknown;
  try { value = JSON.parse(raw); } catch { return null; }
  if (!value || typeof value !== "object") return null;
  const message = value as Record<string, unknown>;
  if (message.channelId !== channelId) return null;

  if (message.type === "chat.message") {
    if (message.version === 1) return parseLegacyMessage(message, channelId);
    if (message.version === 2) return parseSignedMessageObject(message, channelId, false);
    return null;
  }
  if (message.version === 3 && message.type === "chat.event") return parseSignedEventObject(message, channelId, false);
  if (message.version === 3 && message.type === "chat.typing") return parseTypingMessage(message, channelId);
  if (message.version === 2 && message.type === "chat.identity.challenge") return parseIdentityChallenge(message, channelId);
  if (message.version === 2 && message.type === "chat.identity.proof") return parseIdentityProof(message, channelId);
  if (message.version === 2 && message.type === "chat.members.snapshot") return parseGroupMembership(message, channelId);
  if (message.version === 2 && message.type === "chat.group.revocation" && validGroupRevocationCertificate(message.certificate)) {
    return { version: 2, type: "chat.group.revocation", channelId: channelId!, certificate: message.certificate };
  }
  if (message.version === 2 && message.type === "chat.profile.update") return parseProfileUpdate(message, channelId);
  if (message.version === 2 && message.type === "chat.message.ack" && validWireId(message.messageId)) {
    return { version: 2, type: "chat.message.ack", channelId: channelId!, messageId: message.messageId };
  }

  if (message.version !== 2 || !validWireId(message.requestId)) return null;
  const requestId = message.requestId as string;
  if (message.type === "chat.history.request") {
    if (!Array.isArray(message.knownIds) || message.knownIds.length > MAX_HISTORY_IDS || !message.knownIds.every(validWireId)) return null;
    if (message.knownEventIds !== undefined && (!Array.isArray(message.knownEventIds) || message.knownEventIds.length > MAX_HISTORY_IDS || !message.knownEventIds.every(validWireId))) return null;
    if (!validOptionalHistoryCursor(message.messageBefore, message.messageBeforeId)
      || !validOptionalEventCursor(message.eventBeforeTimestamp, message.eventBeforeId)
      || (message.messagesDone !== undefined && typeof message.messagesDone !== "boolean")
      || (message.eventsDone !== undefined && typeof message.eventsDone !== "boolean")) return null;
    return {
      version: 2,
      type: "chat.history.request",
      channelId: channelId!,
      requestId,
      knownIds: [...new Set(message.knownIds as string[])],
      ...(Array.isArray(message.knownEventIds) ? { knownEventIds: [...new Set(message.knownEventIds as string[])] } : {}),
      ...(typeof message.messageBefore === "string" ? { messageBefore: message.messageBefore, messageBeforeId: message.messageBeforeId as string } : {}),
      ...(typeof message.eventBeforeTimestamp === "number" ? { eventBeforeTimestamp: message.eventBeforeTimestamp, eventBeforeId: message.eventBeforeId as string } : {}),
      ...(typeof message.messagesDone === "boolean" ? { messagesDone: message.messagesDone } : {}),
      ...(typeof message.eventsDone === "boolean" ? { eventsDone: message.eventsDone } : {}),
    };
  }
  if (message.type === "chat.history.chunk") {
    if (!Array.isArray(message.messages) || message.messages.length > HISTORY_CHUNK_MESSAGES) return null;
    const messages = message.messages.map((item) => parseSignedMessageObject(item, channelId, true));
    if (messages.some((item) => !item)) return null;
    if (message.events !== undefined && (!Array.isArray(message.events) || message.events.length > HISTORY_CHUNK_EVENTS)) return null;
    const events = Array.isArray(message.events)
      ? message.events.map((item) => parseSignedEventObject(item, channelId, true))
      : [];
    if (events.some((item) => !item)) return null;
    return {
      version: 2,
      type: "chat.history.chunk",
      channelId: channelId!,
      requestId,
      messages: messages as SignedChatWireMessage[],
      ...(Array.isArray(message.events) ? { events: events as SignedChatEventWireMessage[] } : {}),
    };
  }
  if (message.type === "chat.history.complete") {
    if (!validOptionalHistoryCursor(message.nextMessageBefore, message.nextMessageBeforeId)
      || !validOptionalEventCursor(message.nextEventBeforeTimestamp, message.nextEventBeforeId)) return null;
    return {
      version: 2,
      type: "chat.history.complete",
      channelId: channelId!,
      requestId,
      ...(typeof message.nextMessageBefore === "string" ? { nextMessageBefore: message.nextMessageBefore, nextMessageBeforeId: message.nextMessageBeforeId as string } : {}),
      ...(typeof message.nextEventBeforeTimestamp === "number" ? { nextEventBeforeTimestamp: message.nextEventBeforeTimestamp, nextEventBeforeId: message.nextEventBeforeId as string } : {}),
    };
  }
  return null;
}

function validOptionalHistoryCursor(timestamp: unknown, id: unknown): boolean {
  if (timestamp === undefined && id === undefined) return true;
  return typeof timestamp === "string"
    && timestamp.length <= 64
    && !Number.isNaN(Date.parse(timestamp))
    && validWireId(id);
}

function validOptionalEventCursor(timestamp: unknown, id: unknown): boolean {
  if (timestamp === undefined && id === undefined) return true;
  return typeof timestamp === "number"
    && Number.isSafeInteger(timestamp)
    && timestamp > 0
    && validWireId(id);
}

export async function privateConversationId(peerA: string, peerB: string): Promise<string> {
  if (!validWireId(peerA) || !validWireId(peerB) || peerA === peerB) throw new Error("Peers inválidos para conversa privada.");
  const pair = [peerA, peerB].sort().join(":");
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`briks-dm-v1:${pair}`));
  return `dm-${[...new Uint8Array(digest)].map((value) => value.toString(16).padStart(2, "0")).join("")}`;
}

export function canonicalSignedMessage(message: Omit<SignedChatWireMessage, "signature"> | SignedChatWireMessage): string {
  return JSON.stringify({
    version: 2,
    type: "chat.message",
    channelId: message.channelId,
    id: message.id,
    authorPeerId: message.authorPeerId,
    author: message.author,
    content: message.content,
    timestamp: message.timestamp,
  });
}

export function canonicalChatEvent(message: Omit<SignedChatEventWireMessage, "signature"> | SignedChatEventWireMessage): string {
  return JSON.stringify({
    version: 3,
    type: "chat.event",
    channelId: message.channelId,
    id: message.id,
    targetMessageId: message.targetMessageId,
    actorPeerId: message.actorPeerId,
    action: message.action,
    content: message.content ?? null,
    referenceMessageId: message.referenceMessageId ?? null,
    emoji: message.emoji ?? null,
    timestamp: message.timestamp,
  });
}

export function canonicalIdentityProof(message: Omit<IdentityProofWireMessage, "signature"> | IdentityProofWireMessage): string {
  return JSON.stringify({
    version: 2,
    type: "chat.identity.proof",
    channelId: message.channelId,
    fromPeerId: message.fromPeerId,
    toPeerId: message.toPeerId,
    nonce: message.nonce,
    timestamp: message.timestamp,
    capabilities: message.capabilities,
  });
}

export function canonicalGroupMembership(message: Omit<GroupMembersWireMessage, "signature"> | GroupMembersWireMessage): string {
  return JSON.stringify({
    version: 2,
    type: "chat.members.snapshot",
    channelId: message.channelId,
    groupId: message.groupId,
    senderPeerId: message.senderPeerId,
    ownerPeerId: message.ownerPeerId,
    membershipVersion: message.membershipVersion,
    manifestVersion: message.manifestVersion,
    manifestActorPeerId: message.manifestActorPeerId,
    manifestOperationId: message.manifestOperationId,
    administratorEpoch: message.administratorEpoch,
    administratorGrants: [...message.administratorGrants].sort((left, right) => left.administratorPeerId.localeCompare(right.administratorPeerId)),
    name: message.name,
    avatar: message.avatar ?? null,
    channels: [...message.channels].sort((left, right) => left.id.localeCompare(right.id)),
    administratorPeerIds: [...message.administratorPeerIds].sort(),
    removedPeerIds: [...message.removedPeerIds].sort(),
    removedMembers: canonicalPeerIdentities(message.removedMembers),
    revocations: [...message.revocations].sort((left, right) => left.messageId.localeCompare(right.messageId)),
    rendezvousVersion: message.rendezvousVersion,
    rendezvousSecret: message.rendezvousSecret,
    members: canonicalPeerIdentities(message.members),
    timestamp: message.timestamp,
  });
}

export function canonicalProfileUpdate(message: Omit<ProfileUpdateWireMessage, "signature"> | ProfileUpdateWireMessage): string {
  return JSON.stringify({
    version: 2,
    type: "chat.profile.update",
    channelId: message.channelId,
    identity: {
      peerId: message.identity.peerId,
      displayName: message.identity.displayName,
      avatar: message.identity.avatar ?? null,
      publicKey: {
        kty: message.identity.publicKey.kty ?? null,
        crv: message.identity.publicKey.crv ?? null,
        x: message.identity.publicKey.x ?? null,
        y: message.identity.publicKey.y ?? null,
      },
    },
    timestamp: message.timestamp,
  });
}

export function legacyToLocal(message: LegacyChatWireMessage, author: string): LocalChatMessage {
  return { id: message.id, channelId: message.channelId, author, content: message.content, createdAt: new Date(message.timestamp).toISOString() };
}

export function signedToLocal(message: SignedChatWireMessage): LocalChatMessage {
  return {
    id: message.id,
    channelId: message.channelId,
    author: message.author,
    content: message.content,
    createdAt: new Date(message.timestamp).toISOString(),
    authorPeerId: message.authorPeerId,
    signature: message.signature,
  };
}

export function localToSignedWire(message: LocalChatMessage): SignedChatWireMessage | null {
  const timestamp = Date.parse(message.createdAt);
  if (!message.authorPeerId || !message.signature || !Number.isFinite(timestamp)) return null;
  return {
    version: 2,
    type: "chat.message",
    channelId: message.channelId,
    id: message.id,
    authorPeerId: message.authorPeerId,
    author: message.author,
    content: message.content,
    timestamp,
    signature: message.signature,
  };
}

export function samePeerPublicKey(left: PublicPeerIdentity, right: PublicPeerIdentity): boolean {
  return left.publicKey.kty === right.publicKey.kty
    && left.publicKey.crv === right.publicKey.crv
    && left.publicKey.x === right.publicKey.x
    && left.publicKey.y === right.publicKey.y;
}

export function validWireId(value: unknown): value is string {
  return typeof value === "string" && /^[A-Za-z0-9_-]{8,128}$/.test(value);
}

export function bytesToBase64Url(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

export function base64UrlToArrayBuffer(value: string): ArrayBuffer {
  const base64 = value.replace(/-/g, "+").replace(/_/g, "/").padEnd(Math.ceil(value.length / 4) * 4, "=");
  const binary = atob(base64);
  const buffer = new ArrayBuffer(binary.length);
  const bytes = new Uint8Array(buffer);
  for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
  return buffer;
}

function parseLegacyMessage(message: Record<string, unknown>, channelId?: string): LegacyChatWireMessage | null {
  const now = Date.now();
  return message.version === 1 && message.type === "chat.message" && message.channelId === channelId
    && typeof message.id === "string" && /^[0-9a-f-]{36}$/i.test(message.id)
    && typeof message.author === "string" && message.author.trim().length >= 2 && message.author.length <= 80
    && typeof message.content === "string" && message.content.trim().length > 0 && message.content.length <= 4_000
    && typeof message.timestamp === "number" && Number.isFinite(message.timestamp)
    && message.timestamp >= now - LEGACY_MESSAGE_MAX_AGE_MS && message.timestamp <= now + LEGACY_FUTURE_CLOCK_SKEW_MS
    ? message as unknown as LegacyChatWireMessage : null;
}

function parseSignedMessageObject(value: unknown, channelId?: string, allowHistorical = false): SignedChatWireMessage | null {
  if (!value || typeof value !== "object") return null;
  const message = value as Record<string, unknown>;
  const now = Date.now();
  const timestampValid = typeof message.timestamp === "number" && Number.isFinite(message.timestamp)
    && message.timestamp > 0 && message.timestamp <= now + P2P_CLOCK_SKEW_TOLERANCE_MS
    && (allowHistorical || message.timestamp >= now - QUEUED_MESSAGE_MAX_AGE_MS);
  return message.version === 2 && message.type === "chat.message" && message.channelId === channelId
    && validWireId(message.id) && validWireId(message.authorPeerId)
    && typeof message.author === "string" && message.author.trim().length >= 2 && message.author.length <= 80
    && typeof message.content === "string" && message.content.trim().length > 0 && message.content.length <= 4_000
    && typeof message.signature === "string" && /^[A-Za-z0-9_-]{16,256}$/.test(message.signature)
    && timestampValid
    ? message as unknown as SignedChatWireMessage : null;
}

function parseSignedEventObject(value: unknown, channelId?: string, allowHistorical = false): SignedChatEventWireMessage | null {
  if (!value || typeof value !== "object") return null;
  const message = value as Record<string, unknown>;
  const now = Date.now();
  const action = message.action as ChatEventAction;
  const timestampValid = typeof message.timestamp === "number" && Number.isFinite(message.timestamp)
    && message.timestamp > 0 && message.timestamp <= now + P2P_CLOCK_SKEW_TOLERANCE_MS
    && (allowHistorical || message.timestamp >= now - QUEUED_MESSAGE_MAX_AGE_MS);
  const contentValid = action === "edit"
    ? typeof message.content === "string" && message.content.trim().length > 0 && message.content.length <= 4_000
    : message.content === undefined;
  const referenceValid = action === "reply"
    ? validWireId(message.referenceMessageId)
    : message.referenceMessageId === undefined;
  const emojiValid = action === "reaction.add" || action === "reaction.remove"
    ? typeof message.emoji === "string" && message.emoji.trim().length > 0 && [...message.emoji].length <= 16
    : message.emoji === undefined;
  return message.version === 3 && message.type === "chat.event" && message.channelId === channelId
    && validWireId(message.id) && validWireId(message.targetMessageId) && validWireId(message.actorPeerId)
    && ["reply", "edit", "delete", "reaction.add", "reaction.remove", "pin", "unpin"].includes(action)
    && contentValid && referenceValid && emojiValid && timestampValid
    && typeof message.signature === "string" && /^[A-Za-z0-9_-]{16,256}$/.test(message.signature)
    ? message as unknown as SignedChatEventWireMessage : null;
}

function parseTypingMessage(message: Record<string, unknown>, channelId?: string): ChatTypingWireMessage | null {
  return message.version === 3 && message.type === "chat.typing" && message.channelId === channelId
    && validWireId(message.actorPeerId) && typeof message.active === "boolean" && freshTimestamp(message.timestamp)
    ? message as unknown as ChatTypingWireMessage : null;
}

function parseIdentityChallenge(message: Record<string, unknown>, channelId?: string): IdentityChallengeWireMessage | null {
  if (!validWireId(message.fromPeerId) || !validWireId(message.nonce) || !freshTimestamp(message.timestamp) || !validBriksPeerCapabilities(message.capabilities)) return null;
  return {
    version: 2,
    type: "chat.identity.challenge",
    channelId: channelId!,
    fromPeerId: message.fromPeerId,
    nonce: message.nonce,
    timestamp: message.timestamp,
    capabilities: message.capabilities,
  } as IdentityChallengeWireMessage;
}

function parseIdentityProof(message: Record<string, unknown>, channelId?: string): IdentityProofWireMessage | null {
  if (!validWireId(message.fromPeerId) || !validWireId(message.toPeerId) || !validWireId(message.nonce) || !freshTimestamp(message.timestamp) || !validBriksPeerCapabilities(message.capabilities)) return null;
  if (typeof message.signature !== "string" || !/^[A-Za-z0-9_-]{16,256}$/.test(message.signature)) return null;
  return {
    version: 2,
    type: "chat.identity.proof",
    channelId: channelId!,
    fromPeerId: message.fromPeerId,
    toPeerId: message.toPeerId,
    nonce: message.nonce,
    timestamp: message.timestamp,
    capabilities: message.capabilities,
    signature: message.signature,
  } as IdentityProofWireMessage;
}

function parseGroupMembership(message: Record<string, unknown>, channelId?: string): GroupMembersWireMessage | null {
  if (!validWireId(message.groupId) || !validWireId(message.senderPeerId)) return null;
  if (!validWireId(message.ownerPeerId) || !Number.isSafeInteger(message.membershipVersion) || Number(message.membershipVersion) < 1) return null;
  if (!Array.isArray(message.members) || message.members.length === 0 || message.members.length > MAX_GROUP_SYNC_MEMBERS) return null;
  if (!Number.isSafeInteger(message.manifestVersion) || Number(message.manifestVersion) < 1) return null;
  if (!validWireId(message.manifestActorPeerId) || !validWireId(message.manifestOperationId)) return null;
  if (!Number.isSafeInteger(message.administratorEpoch) || Number(message.administratorEpoch) < 1) return null;
  if (!Array.isArray(message.administratorGrants) || message.administratorGrants.length > MAX_GROUP_SYNC_MEMBERS || !message.administratorGrants.every(validGroupAdministratorGrant)) return null;
  if (typeof message.name !== "string" || message.name.trim().length < 2 || message.name.length > 80) return null;
  if (message.avatar !== undefined && !validAvatarDataUrl(message.avatar)) return null;
  if (!Array.isArray(message.channels) || message.channels.length > 100 || !message.channels.every(isLocalGroupChannel)) return null;
  if (!Array.isArray(message.administratorPeerIds) || message.administratorPeerIds.length > 64 || !message.administratorPeerIds.every(validWireId)) return null;
  if (!Array.isArray(message.removedPeerIds) || message.removedPeerIds.length > 256 || !message.removedPeerIds.every(validWireId)) return null;
  if (!Array.isArray(message.removedMembers) || message.removedMembers.length > MAX_GROUP_SYNC_MEMBERS || !message.removedMembers.every(isPublicPeerIdentity)) return null;
  if (!Array.isArray(message.revocations) || message.revocations.length > MAX_GROUP_SYNC_REVOCATIONS || !message.revocations.every(validGroupRevocationCertificate)) return null;
  if (!Number.isSafeInteger(message.rendezvousVersion) || Number(message.rendezvousVersion) < 1 || !validWireId(message.rendezvousSecret)) return null;
  if (!message.members.every(isPublicPeerIdentity) || !freshTimestamp(message.timestamp)) return null;
  if (typeof message.signature !== "string" || !/^[A-Za-z0-9_-]{16,256}$/.test(message.signature)) return null;
  return {
    version: 2,
    type: "chat.members.snapshot",
    channelId: channelId!,
    groupId: message.groupId,
    senderPeerId: message.senderPeerId,
    ownerPeerId: message.ownerPeerId,
    membershipVersion: Number(message.membershipVersion),
    manifestVersion: Number(message.manifestVersion),
    manifestActorPeerId: message.manifestActorPeerId,
    manifestOperationId: message.manifestOperationId,
    administratorEpoch: Number(message.administratorEpoch),
    administratorGrants: message.administratorGrants,
    name: message.name,
    avatar: typeof message.avatar === "string" ? message.avatar : undefined,
    channels: message.channels,
    administratorPeerIds: [...new Set(message.administratorPeerIds as string[])],
    removedPeerIds: [...new Set(message.removedPeerIds as string[])],
    removedMembers: message.removedMembers,
    revocations: message.revocations,
    rendezvousVersion: Number(message.rendezvousVersion),
    rendezvousSecret: message.rendezvousSecret,
    members: message.members,
    timestamp: message.timestamp,
    signature: message.signature,
  } as GroupMembersWireMessage;
}

function parseProfileUpdate(message: Record<string, unknown>, channelId?: string): ProfileUpdateWireMessage | null {
  if (!isPublicPeerIdentity(message.identity) || !freshTimestamp(message.timestamp)) return null;
  if (typeof message.signature !== "string" || !/^[A-Za-z0-9_-]{16,256}$/.test(message.signature)) return null;
  return { version: 2, type: "chat.profile.update", channelId: channelId!, identity: message.identity, timestamp: message.timestamp, signature: message.signature };
}

function canonicalPeerIdentities(members: PublicPeerIdentity[]): object[] {
  return [...members].sort((left, right) => left.peerId.localeCompare(right.peerId)).map((member) => ({
    peerId: member.peerId,
    displayName: member.displayName,
    avatar: member.avatar ?? null,
    publicKey: {
      kty: member.publicKey.kty ?? null,
      crv: member.publicKey.crv ?? null,
      x: member.publicKey.x ?? null,
      y: member.publicKey.y ?? null,
    },
  }));
}

function isPublicPeerIdentity(value: unknown): value is PublicPeerIdentity {
  if (!value || typeof value !== "object") return false;
  const identity = value as Record<string, unknown>;
  if (!validWireId(identity.peerId)) return false;
  if (typeof identity.displayName !== "string" || identity.displayName.trim().length < 2 || identity.displayName.length > 80) return false;
  if (identity.avatar !== undefined && !validAvatarDataUrl(identity.avatar)) return false;
  if (!identity.publicKey || typeof identity.publicKey !== "object") return false;
  const key = identity.publicKey as JsonWebKey;
  return key.kty === "EC" && key.crv === "P-256"
    && typeof key.x === "string" && /^[A-Za-z0-9_-]{1,128}$/.test(key.x)
    && typeof key.y === "string" && /^[A-Za-z0-9_-]{1,128}$/.test(key.y);
}

function freshTimestamp(value: unknown): value is number {
  const now = Date.now();
  return typeof value === "number" && Number.isFinite(value)
    && value >= now - P2P_CLOCK_SKEW_TOLERANCE_MS
    && value <= now + P2P_CLOCK_SKEW_TOLERANCE_MS;
}

function isLocalGroupChannel(value: unknown): value is LocalGroupChannel {
  if (!value || typeof value !== "object") return false;
  const channel = value as Record<string, unknown>;
  return validWireId(channel.id) && typeof channel.name === "string" && channel.name.trim().length >= 1 && channel.name.length <= 80
    && (channel.kind === "text" || channel.kind === "voice")
    && (channel.voiceRoomId === undefined || channel.voiceRoomId === null || validWireId(channel.voiceRoomId));
}
