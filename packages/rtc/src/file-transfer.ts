import {
  DEFAULT_ATTACHMENT_CHUNK_SIZE,
  BRIKS_ATTACHMENT_PROTOCOL_VERSION,
  classifyAttachment,
  sanitizeAttachmentFilename,
  type AttachmentChunkFrame,
  type AttachmentManifest,
  type AttachmentTransferProgress,
  type FileControlMessage,
} from "@briks/protocol/attachments";
import { sha256BlobHex } from "./streaming-sha256";

export type TransferSendControl = (peerId: string, message: FileControlMessage) => void | Promise<void>;
export type TransferSendChunk = (peerId: string, frame: AttachmentChunkFrame) => void | Promise<void>;

export type AttachmentTransferSenderOptions = {
  maxConcurrentTransfers?: number;
  highWaterMarkBytes?: number;
  lowWaterMarkBytes?: number;
  retryLimit?: number;
  retryDelayMs?: number;
  getBufferedAmount?: (peerId: string) => number;
  waitForBufferedAmountLow?: (peerId: string) => Promise<void>;
  waitForPendingData?: (peerId: string) => Promise<void>;
};

export type TransferSource = Blob & { name?: string; type: string; lastModified?: number };

export type CreateAttachmentManifestOptions = {
  id?: string;
  messageId?: string;
  channelId?: string;
  senderPeerId: string;
  chunkSize?: number;
  createdAt?: string;
  includeChunkHashes?: boolean;
};

type QueuedTransfer = {
  transferId: string;
  peerId: string;
  source: TransferSource;
  manifest: AttachmentManifest;
  missingChunks?: Set<number>;
  paused: boolean;
  cancelled: boolean;
  awaitingVerification: boolean;
  retryCount: number;
  bytesTransferred: number;
  startedAt?: string;
};

const DEFAULT_MAX_CONCURRENT_TRANSFERS = 2;
const DEFAULT_HIGH_WATER_MARK_BYTES = 4 * 1024 * 1024;
const DEFAULT_LOW_WATER_MARK_BYTES = 1 * 1024 * 1024;
const DEFAULT_RETRY_LIMIT = 3;
const DEFAULT_RETRY_DELAY_MS = 500;

export async function sha256Hex(data: Blob | ArrayBuffer | ArrayBufferView): Promise<string> {
  if (data instanceof Blob) return sha256BlobHex(data);

  let bytes: ArrayBuffer | Uint8Array<ArrayBuffer>;
  if (data instanceof ArrayBuffer) {
    bytes = data;
  } else {
    // ArrayBufferView.buffer é ArrayBufferLike no TypeScript moderno e pode ser
    // um SharedArrayBuffer. Web Crypto não aceita SharedArrayBuffer como
    // BufferSource, então copiamos explicitamente apenas o intervalo visível da
    // view para memória respaldada por ArrayBuffer normal.
    const copy = new Uint8Array(data.byteLength);
    copy.set(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
    bytes = copy;
  }

  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((value) => value.toString(16).padStart(2, "0")).join("");
}

export async function createAttachmentManifest(
  source: TransferSource,
  options: CreateAttachmentManifestOptions,
): Promise<AttachmentManifest> {
  const chunkSize = options.chunkSize ?? DEFAULT_ATTACHMENT_CHUNK_SIZE;
  if (!Number.isSafeInteger(chunkSize) || chunkSize <= 0) throw new Error("chunkSize inválido.");

  const filename = sanitizeAttachmentFilename(source.name ?? "attachment");
  const mimeType = source.type || "application/octet-stream";
  const contentHash = await sha256Hex(source);
  const chunkCount = Math.ceil(source.size / chunkSize);
  const id = options.id ?? contentHash;
  const chunkHashes = options.includeChunkHashes === false
    ? undefined
    : await hashChunks(source, chunkSize, chunkCount);

  return {
    protocolVersion: BRIKS_ATTACHMENT_PROTOCOL_VERSION,
    id,
    messageId: options.messageId,
    channelId: options.channelId,
    senderPeerId: options.senderPeerId,
    filename,
    mimeType,
    extension: extensionOf(filename),
    kind: classifyAttachment(mimeType, filename),
    size: source.size,
    contentHash,
    chunkSize,
    chunkCount,
    chunkHashes,
    createdAt: options.createdAt ?? new Date().toISOString(),
  };
}

export async function* iterateAttachmentChunks(
  source: Blob,
  manifest: AttachmentManifest,
  onlyChunks?: ReadonlySet<number>,
): AsyncGenerator<AttachmentChunkFrame> {
  for (let index = 0; index < manifest.chunkCount; index += 1) {
    if (onlyChunks && !onlyChunks.has(index)) continue;
    const offset = index * manifest.chunkSize;
    const chunk = source.slice(offset, Math.min(source.size, offset + manifest.chunkSize));
    const payload = await chunk.arrayBuffer();
    const hash = manifest.chunkHashes?.[index] ?? await sha256Hex(payload);
    yield {
      transferId: "",
      attachmentId: manifest.id,
      index,
      offset,
      size: payload.byteLength,
      hash,
      payload,
    };
  }
}

export async function verifyAttachmentChunk(frame: AttachmentChunkFrame): Promise<boolean> {
  if (!Number.isSafeInteger(frame.index) || frame.index < 0) return false;
  if (!Number.isSafeInteger(frame.offset) || frame.offset < 0) return false;
  if (frame.payload.byteLength !== frame.size) return false;
  return (await sha256Hex(frame.payload)).toLowerCase() === frame.hash.toLowerCase();
}

export class AttachmentTransferSender extends EventTarget {
  private readonly queue: QueuedTransfer[] = [];
  private readonly active = new Map<string, QueuedTransfer>();
  private readonly maxConcurrentTransfers: number;
  private readonly highWaterMarkBytes: number;
  private readonly lowWaterMarkBytes: number;
  private readonly retryLimit: number;
  private readonly retryDelayMs: number;
  private pumping = false;

  constructor(
    private readonly sendControl: TransferSendControl,
    private readonly sendChunk: TransferSendChunk,
    private readonly options: AttachmentTransferSenderOptions = {},
  ) {
    super();
    this.maxConcurrentTransfers = positiveInteger(options.maxConcurrentTransfers, DEFAULT_MAX_CONCURRENT_TRANSFERS);
    this.highWaterMarkBytes = positiveInteger(options.highWaterMarkBytes, DEFAULT_HIGH_WATER_MARK_BYTES);
    this.lowWaterMarkBytes = positiveInteger(options.lowWaterMarkBytes, DEFAULT_LOW_WATER_MARK_BYTES);
    this.retryLimit = nonNegativeInteger(options.retryLimit, DEFAULT_RETRY_LIMIT);
    this.retryDelayMs = nonNegativeInteger(options.retryDelayMs, DEFAULT_RETRY_DELAY_MS);
    if (this.lowWaterMarkBytes > this.highWaterMarkBytes) throw new Error("lowWaterMarkBytes não pode exceder highWaterMarkBytes.");
  }

  async offer(peerId: string, source: TransferSource, manifest: AttachmentManifest): Promise<string> {
    const transferId = crypto.randomUUID();
    const transfer: QueuedTransfer = {
      transferId,
      peerId,
      source,
      manifest,
      paused: false,
      cancelled: false,
      awaitingVerification: false,
      retryCount: 0,
      bytesTransferred: 0,
    };
    this.queue.push(transfer);
    await this.sendControl(peerId, { type: "file.offer", transferId, manifest });
    this.emitProgress(transfer, "offered");
    return transferId;
  }

  async acceptAndStart(transferId: string, missingChunks?: number[]): Promise<void> {
    const transfer = this.findTransfer(transferId);
    transfer.missingChunks = missingChunks ? new Set(missingChunks) : undefined;
    transfer.paused = false;
    transfer.awaitingVerification = false;
    this.emitProgress(transfer, "queued");
    await this.pump();
  }

  pause(transferId: string): void {
    const transfer = this.findTransfer(transferId);
    transfer.paused = true;
    this.emitProgress(transfer, "paused");
  }

  resume(transferId: string, missingChunks?: number[]): void {
    const transfer = this.findTransfer(transferId);
    transfer.paused = false;
    transfer.awaitingVerification = false;
    transfer.missingChunks = missingChunks ? new Set(missingChunks) : transfer.missingChunks;
    this.emitProgress(transfer, "queued");
    void this.pump();
  }

  cancel(transferId: string): void {
    const transfer = this.findTransfer(transferId);
    transfer.cancelled = true;
    transfer.paused = false;
    transfer.awaitingVerification = false;
    this.emitProgress(transfer, "cancelled");
    if (!this.active.has(transferId)) this.removeTransfer(transfer);
  }

  confirm(transferId: string): void {
    const transfer = this.findTransfer(transferId);
    transfer.awaitingVerification = false;
    transfer.bytesTransferred = transfer.manifest.size;
    this.emitProgress(transfer, "completed");
    this.removeTransfer(transfer);
  }

  private async pump(): Promise<void> {
    if (this.pumping) return;
    this.pumping = true;
    try {
      while (this.active.size < this.maxConcurrentTransfers) {
        const next = this.queue.find((item) => !item.cancelled && !item.paused && !item.awaitingVerification && !this.active.has(item.transferId));
        if (!next) break;
        this.active.set(next.transferId, next);
        void this.run(next).finally(() => {
          this.active.delete(next.transferId);
          if (next.cancelled) this.removeTransfer(next);
          void this.pump();
        });
      }
    } finally {
      this.pumping = false;
    }
  }

  private async run(transfer: QueuedTransfer): Promise<void> {
    transfer.startedAt ??= new Date().toISOString();
    await this.sendControl(transfer.peerId, { type: "file.manifest", transferId: transfer.transferId, manifest: transfer.manifest });
    if (transfer.cancelled) return;
    this.emitProgress(transfer, transfer.paused ? "paused" : "transferring");

    try {
      for await (const frame of iterateAttachmentChunks(transfer.source, transfer.manifest, transfer.missingChunks)) {
        if (transfer.cancelled) return;
        while (transfer.paused) await delay(100);
        if (transfer.cancelled) return;
        await this.applyBackpressure(transfer.peerId);
        if (transfer.cancelled) return;
        frame.transferId = transfer.transferId;
        if (!(await this.sendWithRetry(transfer, frame))) return;
        transfer.bytesTransferred = Math.min(transfer.manifest.size, transfer.bytesTransferred + frame.size);
        if (transfer.cancelled) return;
        this.emitProgress(transfer, transfer.paused ? "paused" : "transferring");
      }
      if (transfer.cancelled) return;
      await this.options.waitForPendingData?.(transfer.peerId);
      if (transfer.cancelled) return;
      transfer.awaitingVerification = true;
      this.emitProgress(transfer, "verifying");
      await this.sendControl(transfer.peerId, {
        type: "file.complete",
        transferId: transfer.transferId,
        contentHash: transfer.manifest.contentHash,
      });
    } catch (error) {
      transfer.awaitingVerification = false;
      this.emitProgress(transfer, "failed", error instanceof Error ? error.message : String(error));
      await this.sendControl(transfer.peerId, {
        type: "file.error",
        transferId: transfer.transferId,
        code: "transfer_failed",
        message: error instanceof Error ? error.message : String(error),
        retryable: transfer.retryCount < this.retryLimit,
      });
    }
  }

  private async sendWithRetry(transfer: QueuedTransfer, frame: AttachmentChunkFrame): Promise<boolean> {
    for (;;) {
      if (transfer.cancelled) return false;
      while (transfer.paused) await delay(100);
      if (transfer.cancelled) return false;
      try {
        await this.sendChunk(transfer.peerId, frame);
        return true;
      } catch (error) {
        transfer.retryCount += 1;
        if (transfer.retryCount > this.retryLimit) throw error;
        await delay(this.retryDelayMs * transfer.retryCount);
      }
    }
  }

  private async applyBackpressure(peerId: string): Promise<void> {
    const getBufferedAmount = this.options.getBufferedAmount;
    if (!getBufferedAmount || getBufferedAmount(peerId) <= this.highWaterMarkBytes) return;
    if (this.options.waitForBufferedAmountLow) {
      await this.options.waitForBufferedAmountLow(peerId);
      return;
    }
    while (getBufferedAmount(peerId) > this.lowWaterMarkBytes) await delay(25);
  }

  private findTransfer(transferId: string): QueuedTransfer {
    const transfer = this.queue.find((item) => item.transferId === transferId) ?? this.active.get(transferId);
    if (!transfer) throw new Error(`Transferência desconhecida: ${transferId}`);
    return transfer;
  }

  private removeTransfer(transfer: QueuedTransfer): void {
    const index = this.queue.indexOf(transfer);
    if (index >= 0) this.queue.splice(index, 1);
  }

  private emitProgress(transfer: QueuedTransfer, state: AttachmentTransferProgress["state"], lastError?: string): void {
    const detail: AttachmentTransferProgress = {
      transferId: transfer.transferId,
      attachmentId: transfer.manifest.id,
      peerId: transfer.peerId,
      state,
      bytesTransferred: transfer.bytesTransferred,
      totalBytes: transfer.manifest.size,
      retryCount: transfer.retryCount,
      startedAt: transfer.startedAt,
      updatedAt: new Date().toISOString(),
      lastError,
    };
    this.dispatchEvent(new CustomEvent<AttachmentTransferProgress>("progress", { detail }));
  }
}

async function hashChunks(source: Blob, chunkSize: number, chunkCount: number): Promise<string[]> {
  const hashes: string[] = [];
  for (let index = 0; index < chunkCount; index += 1) {
    const offset = index * chunkSize;
    hashes.push(await sha256Hex(source.slice(offset, Math.min(source.size, offset + chunkSize))));
  }
  return hashes;
}

function extensionOf(filename: string): string | undefined {
  const index = filename.lastIndexOf(".");
  if (index <= 0 || index === filename.length - 1) return undefined;
  return filename.slice(index + 1).toLowerCase();
}

function positiveInteger(value: number | undefined, fallback: number): number {
  return Number.isSafeInteger(value) && (value ?? 0) > 0 ? value! : fallback;
}

function nonNegativeInteger(value: number | undefined, fallback: number): number {
  return Number.isSafeInteger(value) && (value ?? -1) >= 0 ? value! : fallback;
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
