export const BRIKS_ATTACHMENT_PROTOCOL_VERSION = 1 as const;

export const DEFAULT_ATTACHMENT_CHUNK_SIZE = 256 * 1024;
export const MAX_ATTACHMENT_CHUNK_SIZE = 256 * 1024;
export const MAX_ATTACHMENT_FILENAME_LENGTH = 255;
export const MAX_ATTACHMENT_MIME_LENGTH = 127;
export const MAX_ATTACHMENT_CHUNKS = 1_000_000;
export const MAX_ATTACHMENT_CONTROL_WIRE_BYTES = 64 * 1024;

export type BriksCapability =
  | "file-transfer-v1"
  | "attachment-sync-v1"
  | "transfer-resume-v1"
  | "multi-source-v1";

export type PeerCapabilitiesMessage = {
  type: "peer.capabilities";
  protocolVersion: number;
  capabilities: BriksCapability[];
};

export type AttachmentKind = "image" | "video" | "audio" | "document" | "archive" | "executable" | "other";

export type AttachmentMetadata = {
  width?: number;
  height?: number;
  durationMs?: number;
  thumbnailHash?: string;
  voiceMessage?: boolean;
};

export type AttachmentManifest = {
  protocolVersion: typeof BRIKS_ATTACHMENT_PROTOCOL_VERSION;
  id: string;
  messageId?: string;
  channelId?: string;
  senderPeerId: string;
  filename: string;
  mimeType: string;
  extension?: string;
  kind: AttachmentKind;
  size: number;
  contentHash: string;
  chunkSize: number;
  chunkCount: number;
  chunkHashes?: string[];
  createdAt: string;
  metadata?: AttachmentMetadata;
};

export type AttachmentTransferState =
  | "offered"
  | "waiting"
  | "accepted"
  | "queued"
  | "transferring"
  | "paused"
  | "verifying"
  | "completed"
  | "failed"
  | "cancelled";

export type AttachmentTransferProgress = {
  transferId: string;
  attachmentId: string;
  peerId: string;
  state: AttachmentTransferState;
  bytesTransferred: number;
  totalBytes: number;
  retryCount: number;
  startedAt?: string;
  updatedAt: string;
  lastError?: string;
};

export type FileOfferMessage = { type: "file.offer"; transferId: string; manifest: AttachmentManifest };
export type FileRequestMessage = { type: "file.request"; attachmentId: string };
export type FileAcceptMessage = { type: "file.accept"; transferId: string };
export type FileRejectMessage = { type: "file.reject"; transferId: string; reason?: string };
export type FileManifestMessage = { type: "file.manifest"; transferId: string; manifest: AttachmentManifest };
export type FileNeedMessage = { type: "file.need"; transferId: string; missingChunks: number[] };
export type FilePauseMessage = { type: "file.pause"; transferId: string };
export type FileResumeMessage = { type: "file.resume"; transferId: string; missingChunks?: number[] };
export type FileCancelMessage = { type: "file.cancel"; transferId: string; reason?: string };
export type FileCompleteMessage = { type: "file.complete"; transferId: string; contentHash: string };
export type FileErrorMessage = { type: "file.error"; transferId: string; code: string; message: string; retryable: boolean };
export type FileAvailabilityMessage = {
  type: "file.availability";
  attachmentId: string;
  chunkCount: number;
  availableChunks: number[];
  expiresAt: string;
};

export type FileControlMessage =
  | PeerCapabilitiesMessage
  | FileOfferMessage
  | FileRequestMessage
  | FileAcceptMessage
  | FileRejectMessage
  | FileManifestMessage
  | FileNeedMessage
  | FilePauseMessage
  | FileResumeMessage
  | FileCancelMessage
  | FileCompleteMessage
  | FileErrorMessage
  | FileAvailabilityMessage;

export type AttachmentChunkFrame = {
  transferId: string;
  attachmentId: string;
  index: number;
  offset: number;
  size: number;
  hash: string;
  payload: ArrayBuffer;
};

const MIME_BY_EXTENSION: Readonly<Record<string, string>> = {
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  jfif: "image/jpeg",
  gif: "image/gif",
  webp: "image/webp",
  avif: "image/avif",
  bmp: "image/bmp",
  ico: "image/x-icon",
  mp4: "video/mp4",
  webm: "video/webm",
  ogv: "video/ogg",
  mp3: "audio/mpeg",
  wav: "audio/wav",
  ogg: "audio/ogg",
  opus: "audio/ogg",
  m4a: "audio/mp4",
  aac: "audio/aac",
  flac: "audio/flac",
  pdf: "application/pdf",
};

export function inferAttachmentMimeType(mimeType: string, filename = ""): string {
  const normalized = mimeType.trim().toLowerCase();
  if (normalized && normalized !== "application/octet-stream") return normalized;
  const extension = filename.toLowerCase().split(".").pop() ?? "";
  return MIME_BY_EXTENSION[extension] ?? (normalized || "application/octet-stream");
}

export function classifyAttachment(mimeType: string, filename = ""): AttachmentKind {
  const mime = inferAttachmentMimeType(mimeType, filename);
  const extension = filename.toLowerCase().split(".").pop() ?? "";
  if (mime.startsWith("image/")) return "image";
  if (mime.startsWith("video/")) return "video";
  if (mime.startsWith("audio/")) return "audio";
  if (["zip", "rar", "7z", "tar", "gz", "bz2", "xz"].includes(extension)) return "archive";
  if (["exe", "msi", "bat", "cmd", "com", "ps1", "scr", "jar", "appimage", "deb", "rpm"].includes(extension)) return "executable";
  if (mime.startsWith("text/") || mime === "application/pdf" || mime.includes("document") || mime.includes("spreadsheet") || mime.includes("presentation")) return "document";
  return "other";
}

export function validateAttachmentManifest(manifest: unknown): string[] {
  if (!manifest || typeof manifest !== "object" || Array.isArray(manifest)) return ["invalid_manifest"];
  const value = manifest as Partial<AttachmentManifest>;
  const errors: string[] = [];
  if (value.protocolVersion !== BRIKS_ATTACHMENT_PROTOCOL_VERSION) errors.push("unsupported_protocol_version");
  if (typeof value.id !== "string" || !/^[a-f0-9]{64}$/i.test(value.id)) errors.push("missing_attachment_id");
  if (typeof value.senderPeerId !== "string" || !validWireIdentifier(value.senderPeerId)) errors.push("missing_sender_peer_id");
  if (typeof value.filename !== "string" || !value.filename || value.filename.length > MAX_ATTACHMENT_FILENAME_LENGTH) errors.push("invalid_filename");
  if (typeof value.mimeType !== "string" || value.mimeType.length > MAX_ATTACHMENT_MIME_LENGTH) errors.push("invalid_mime_type");
  if (!ATTACHMENT_KINDS.has(value.kind as AttachmentKind)) errors.push("invalid_kind");
  if (!Number.isSafeInteger(value.size) || Number(value.size) < 0) errors.push("invalid_size");
  if (!Number.isSafeInteger(value.chunkSize) || Number(value.chunkSize) <= 0 || Number(value.chunkSize) > MAX_ATTACHMENT_CHUNK_SIZE) errors.push("invalid_chunk_size");
  if (!Number.isSafeInteger(value.chunkCount) || Number(value.chunkCount) < 0 || Number(value.chunkCount) > MAX_ATTACHMENT_CHUNKS) errors.push("invalid_chunk_count");
  if (typeof value.contentHash !== "string" || !/^[a-f0-9]{64}$/i.test(value.contentHash)) errors.push("invalid_content_hash");
  if (typeof value.id === "string" && typeof value.contentHash === "string"
    && value.id.toLowerCase() !== value.contentHash.toLowerCase()) errors.push("attachment_id_hash_mismatch");
  if (Number.isSafeInteger(value.size) && Number(value.size) >= 0
    && Number.isSafeInteger(value.chunkSize) && Number(value.chunkSize) > 0
    && Number.isSafeInteger(value.chunkCount)
    && value.chunkCount !== Math.ceil(Number(value.size) / Number(value.chunkSize))) errors.push("invalid_chunk_layout");
  if (value.chunkHashes !== undefined && (!Array.isArray(value.chunkHashes)
    || value.chunkHashes.length !== value.chunkCount
    || value.chunkHashes.some((hash) => typeof hash !== "string" || !/^[a-f0-9]{64}$/i.test(hash)))) errors.push("invalid_chunk_hash_count");
  if (typeof value.createdAt !== "string" || value.createdAt.length > 64 || !Number.isFinite(Date.parse(value.createdAt))) errors.push("invalid_created_at");
  if (value.channelId !== undefined && (typeof value.channelId !== "string" || !validWireIdentifier(value.channelId))) errors.push("invalid_channel_id");
  if (value.messageId !== undefined && (typeof value.messageId !== "string" || !validWireIdentifier(value.messageId))) errors.push("invalid_message_id");
  if (value.extension !== undefined && (typeof value.extension !== "string" || value.extension.length > 32)) errors.push("invalid_extension");
  if (value.metadata !== undefined && !validAttachmentMetadata(value.metadata)) errors.push("invalid_metadata");
  return errors;
}

export function isFileControlMessage(value: unknown): value is FileControlMessage {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const message = value as Record<string, unknown>;
  const validTransfer = () => typeof message.transferId === "string" && validTransferIdentifier(message.transferId);
  const validOptionalReason = () => message.reason === undefined
    || (typeof message.reason === "string" && message.reason.length <= 500);
  switch (message.type) {
    case "peer.capabilities":
      return message.protocolVersion === BRIKS_ATTACHMENT_PROTOCOL_VERSION
        && Array.isArray(message.capabilities)
        && message.capabilities.length <= BRIKS_CAPABILITIES.size
        && message.capabilities.every((capability) => BRIKS_CAPABILITIES.has(capability as BriksCapability));
    case "file.offer":
    case "file.manifest":
      return validTransfer() && validateAttachmentManifest(message.manifest).length === 0;
    case "file.request":
      return typeof message.attachmentId === "string" && SHA256_PATTERN.test(message.attachmentId);
    case "file.accept":
    case "file.pause":
      return validTransfer();
    case "file.reject":
    case "file.cancel":
      return validTransfer() && validOptionalReason();
    case "file.need":
      return validTransfer() && validChunkList(message.missingChunks);
    case "file.resume":
      return validTransfer() && (message.missingChunks === undefined || validChunkList(message.missingChunks));
    case "file.complete":
      return validTransfer() && typeof message.contentHash === "string" && SHA256_PATTERN.test(message.contentHash);
    case "file.error":
      return validTransfer()
        && typeof message.code === "string" && /^[a-z0-9_-]{1,64}$/i.test(message.code)
        && typeof message.message === "string" && message.message.length <= 1_000
        && typeof message.retryable === "boolean";
    case "file.availability":
      return typeof message.attachmentId === "string" && SHA256_PATTERN.test(message.attachmentId)
        && Number.isSafeInteger(message.chunkCount) && Number(message.chunkCount) >= 0 && Number(message.chunkCount) <= MAX_ATTACHMENT_CHUNKS
        && validChunkList(message.availableChunks)
        && (message.availableChunks as number[]).every((index) => index < Number(message.chunkCount))
        && typeof message.expiresAt === "string" && message.expiresAt.length <= 64 && Number.isFinite(Date.parse(message.expiresAt));
    default:
      return false;
  }
}

const SHA256_PATTERN = /^[a-f0-9]{64}$/i;
const ATTACHMENT_KINDS = new Set<AttachmentKind>(["image", "video", "audio", "document", "archive", "executable", "other"]);
const BRIKS_CAPABILITIES = new Set<BriksCapability>(["file-transfer-v1", "attachment-sync-v1", "transfer-resume-v1", "multi-source-v1"]);

function validWireIdentifier(value: string): boolean {
  return value.length >= 8 && value.length <= 128 && /^[A-Za-z0-9_-]+$/.test(value);
}

function validTransferIdentifier(value: string): boolean {
  return value.length >= 8 && value.length <= 200 && /^[A-Za-z0-9:_-]+$/.test(value);
}

function validChunkList(value: unknown): value is number[] {
  return Array.isArray(value)
    && value.length <= 2_048
    && value.every((index) => Number.isSafeInteger(index) && index >= 0 && index < MAX_ATTACHMENT_CHUNKS);
}

function validAttachmentMetadata(value: unknown): value is AttachmentMetadata {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const metadata = value as Record<string, unknown>;
  const validPositiveNumber = (candidate: unknown) => candidate === undefined
    || (typeof candidate === "number" && Number.isFinite(candidate) && candidate >= 0);
  return validPositiveNumber(metadata.width)
    && validPositiveNumber(metadata.height)
    && validPositiveNumber(metadata.durationMs)
    && (metadata.thumbnailHash === undefined || (typeof metadata.thumbnailHash === "string" && SHA256_PATTERN.test(metadata.thumbnailHash)))
    && (metadata.voiceMessage === undefined || typeof metadata.voiceMessage === "boolean");
}

export function sanitizeAttachmentFilename(filename: string): string {
  const base = filename.replace(/\\/g, "/").split("/").pop() ?? "attachment";
  const cleaned = base.replace(/[\u0000-\u001f<>:"|?*]/g, "_").replace(/^\.+$/, "attachment").trim();
  return (cleaned || "attachment").slice(0, MAX_ATTACHMENT_FILENAME_LENGTH);
}
