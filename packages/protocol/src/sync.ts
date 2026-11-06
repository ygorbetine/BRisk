export const BRIKS_SYNC_PROTOCOL_VERSION = 3 as const;

export type SyncEntityKind = "message" | "attachment" | "message-tombstone" | "reaction" | "group-members";

export type SyncScope = {
  channelId: string;
  entityKinds: SyncEntityKind[];
};

export type SyncDescriptor = {
  id: string;
  kind: SyncEntityKind;
  revision: number;
  contentHash: string;
  updatedAt: string;
};

export type SyncCheckpoint = {
  id: string;
  remotePeerId: string;
  channelId: string;
  revision: number;
  cursor?: string;
  stateHash?: string;
  lastSyncedAt: string;
};

export type SyncHelloMessage = {
  version: typeof BRIKS_SYNC_PROTOCOL_VERSION;
  type: "sync.hello";
  requestId: string;
  fromPeerId: string;
  scope: SyncScope;
  checkpoint?: Pick<SyncCheckpoint, "revision" | "cursor" | "stateHash" | "lastSyncedAt">;
};

export type SyncManifestMessage = {
  version: typeof BRIKS_SYNC_PROTOCOL_VERSION;
  type: "sync.manifest";
  requestId: string;
  channelId: string;
  page: number;
  hasMore: boolean;
  cursor?: string;
  descriptors: SyncDescriptor[];
  stateHash: string;
};

export type SyncNeedMessage = {
  version: typeof BRIKS_SYNC_PROTOCOL_VERSION;
  type: "sync.need";
  requestId: string;
  channelId: string;
  ids: string[];
};

export type SyncItem = {
  descriptor: SyncDescriptor;
  payload: unknown;
};

export type SyncItemsMessage = {
  version: typeof BRIKS_SYNC_PROTOCOL_VERSION;
  type: "sync.items";
  requestId: string;
  channelId: string;
  items: SyncItem[];
};

export type SyncCheckpointMessage = {
  version: typeof BRIKS_SYNC_PROTOCOL_VERSION;
  type: "sync.checkpoint";
  requestId: string;
  channelId: string;
  revision: number;
  cursor?: string;
  stateHash: string;
  timestamp: string;
};

export type SyncCompleteMessage = {
  version: typeof BRIKS_SYNC_PROTOCOL_VERSION;
  type: "sync.complete";
  requestId: string;
  channelId: string;
  stateHash: string;
  timestamp: string;
};

export type SyncWireMessage =
  | SyncHelloMessage
  | SyncManifestMessage
  | SyncNeedMessage
  | SyncItemsMessage
  | SyncCheckpointMessage
  | SyncCompleteMessage;

export const MAX_SYNC_DESCRIPTORS_PER_PAGE = 96;
export const MAX_SYNC_NEED_IDS = 128;
// Attachment manifests may include hundreds of chunk hashes. One item per frame keeps
// sync.items safely below the 64 KiB control-channel ceiling without fragmenting JSON.
export const MAX_SYNC_ITEMS_PER_MESSAGE = 1;

export function isSyncWireMessage(value: unknown): value is SyncWireMessage {
  if (!value || typeof value !== "object") return false;
  const candidate = value as { version?: unknown; type?: unknown; channelId?: unknown };
  if (candidate.version !== BRIKS_SYNC_PROTOCOL_VERSION || typeof candidate.type !== "string") return false;
  return candidate.type === "sync.hello"
    || candidate.type === "sync.manifest"
    || candidate.type === "sync.need"
    || candidate.type === "sync.items"
    || candidate.type === "sync.checkpoint"
    || candidate.type === "sync.complete";
}

export function syncCheckpointId(remotePeerId: string, channelId: string): string {
  return `${remotePeerId}:${channelId}`;
}
