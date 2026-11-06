import {
  validateAttachmentManifest,
  type AttachmentChunkFrame,
  type AttachmentManifest,
  type AttachmentTransferProgress,
  type FileControlMessage,
  type FileOfferMessage,
} from "@briks/protocol/attachments";
import { verifyAttachmentChunk, type TransferSendControl } from "./file-transfer";

export interface AttachmentChunkSink {
  prepare(transferId: string, manifest: AttachmentManifest): Promise<void>;
  hasChunk(transferId: string, index: number): Promise<boolean>;
  writeChunk(transferId: string, frame: AttachmentChunkFrame): Promise<void>;
  finalize(transferId: string, manifest: AttachmentManifest): Promise<{ contentHash: string }>;
  discard(transferId: string): Promise<void>;
  listMissingChunks?(transferId: string, chunkCount: number): Promise<number[]>;
}

export type AttachmentTransferReceiverOptions = {
  maxIncomingTransfers?: number;
  maxConcurrentChunkWrites?: number;
  maxFileSizeBytes?: number;
  autoAccept?: (peerId: string, manifest: AttachmentManifest) => boolean | Promise<boolean>;
  onOffer?: (peerId: string, transferId: string, manifest: AttachmentManifest) => void | Promise<void>;
};

type IncomingTransfer = {
  peerId: string;
  transferId: string;
  manifest: AttachmentManifest;
  receivedChunks: Set<number>;
  bytesTransferred: number;
  state: AttachmentTransferProgress["state"];
  startedAt?: string;
  retryCount: number;
  missingChunks?: Set<number>;
  processingChunks: Set<number>;
};

const DEFAULT_MAX_INCOMING_TRANSFERS = 4;
const DEFAULT_MAX_CONCURRENT_CHUNK_WRITES = 8;
const DEFAULT_MAX_FILE_SIZE_BYTES = 20 * 1024 * 1024 * 1024;
const MAX_MISSING_CHUNKS_PER_REQUEST = 2_048;

export class AttachmentTransferReceiver extends EventTarget {
  private readonly transfers = new Map<string, IncomingTransfer>();
  private readonly maxIncomingTransfers: number;
  private readonly maxFileSizeBytes: number;
  private readonly chunkWriteSemaphore: AsyncSemaphore;
  private readonly inFlightWrites = new Map<string, Set<Promise<void>>>();

  constructor(
    private readonly sink: AttachmentChunkSink,
    private readonly sendControl: TransferSendControl,
    private readonly options: AttachmentTransferReceiverOptions = {},
  ) {
    super();
    this.maxIncomingTransfers = options.maxIncomingTransfers ?? DEFAULT_MAX_INCOMING_TRANSFERS;
    this.maxFileSizeBytes = options.maxFileSizeBytes ?? DEFAULT_MAX_FILE_SIZE_BYTES;
    this.chunkWriteSemaphore = new AsyncSemaphore(options.maxConcurrentChunkWrites ?? DEFAULT_MAX_CONCURRENT_CHUNK_WRITES);
  }

  async handleControl(peerId: string, message: FileControlMessage): Promise<void> {
    switch (message.type) {
      case "file.offer":
        await this.handleOffer(peerId, message);
        return;
      case "file.manifest":
        await this.ensurePrepared(peerId, message.transferId, message.manifest);
        return;
      case "file.pause":
        this.updateState(message.transferId, "paused");
        return;
      case "file.resume":
        this.updateState(message.transferId, "transferring");
        return;
      case "file.cancel":
        await this.cancel(message.transferId);
        return;
      case "file.complete":
        await this.complete(peerId, message.transferId, message.contentHash);
        return;
      default:
        return;
    }
  }

  async handleChunk(peerId: string, frame: AttachmentChunkFrame): Promise<void> {
    const transfer = this.transfers.get(frame.transferId);
    if (!transfer || transfer.peerId !== peerId) throw new Error("Chunk recebido para transferência desconhecida.");
    if (transfer.state !== "accepted" && transfer.state !== "transferring") return;
    if (frame.attachmentId !== transfer.manifest.id) throw new Error("attachmentId divergente no chunk.");
    if (frame.index < 0 || frame.index >= transfer.manifest.chunkCount) throw new Error("Índice de chunk fora do manifesto.");
    if (transfer.receivedChunks.has(frame.index) || transfer.processingChunks.has(frame.index)) return;

    transfer.processingChunks.add(frame.index);
    const operation = this.chunkWriteSemaphore.run(() => this.processChunk(peerId, frame));
    const writes = this.inFlightWrites.get(frame.transferId) ?? new Set<Promise<void>>();
    writes.add(operation);
    this.inFlightWrites.set(frame.transferId, writes);
    try {
      await operation;
    } finally {
      transfer.processingChunks.delete(frame.index);
      writes.delete(operation);
      if (writes.size === 0) this.inFlightWrites.delete(frame.transferId);
    }
  }

  private async processChunk(peerId: string, frame: AttachmentChunkFrame): Promise<void> {
    const transfer = this.transfers.get(frame.transferId);
    if (!transfer || transfer.peerId !== peerId || !this.isReceiving(transfer)) return;

    const expectedHash = transfer.manifest.chunkHashes?.[frame.index];
    if (expectedHash && expectedHash.toLowerCase() !== frame.hash.toLowerCase()) {
      transfer.retryCount += 1;
      await this.requestMissing(peerId, transfer, [frame.index]);
      return;
    }
    if (!(await verifyAttachmentChunk(frame))) {
      transfer.retryCount += 1;
      await this.requestMissing(peerId, transfer, [frame.index]);
      return;
    }
    if (!this.isReceiving(transfer)) return;
    if (transfer.missingChunks && !transfer.missingChunks.has(frame.index)) {
      transfer.receivedChunks.add(frame.index);
      return;
    }
    if (!transfer.missingChunks) {
      const alreadyStored = await this.sink.hasChunk(frame.transferId, frame.index);
      if (!this.isReceiving(transfer)) return;
      if (alreadyStored) {
        transfer.receivedChunks.add(frame.index);
        return;
      }
    }

    await this.sink.writeChunk(frame.transferId, frame);
    if (this.transfers.get(frame.transferId) !== transfer) {
      await this.sink.discard(frame.transferId).catch(() => undefined);
      return;
    }
    if (this.hasState(transfer, "cancelled")) return;
    transfer.receivedChunks.add(frame.index);
    transfer.missingChunks?.delete(frame.index);
    transfer.bytesTransferred += frame.size;
    if (!this.hasState(transfer, "paused")) transfer.state = "transferring";
    transfer.startedAt ??= new Date().toISOString();
    this.emitProgress(transfer);
  }

  async accept(transferId: string): Promise<void> {
    const transfer = this.requireTransfer(transferId);
    await this.sink.prepare(transferId, transfer.manifest);
    transfer.state = "accepted";
    const missing = await this.getMissingChunks(transfer);
    transfer.missingChunks = new Set(missing);
    if (missing.length === transfer.manifest.chunkCount) {
      await this.sendControl(transfer.peerId, { type: "file.accept", transferId });
    } else {
      await this.requestMissing(transfer.peerId, transfer, missing);
    }
    transfer.state = "transferring";
    this.emitProgress(transfer);
  }

  canAccept(transferId: string): boolean {
    return this.transfers.get(transferId)?.state === "offered";
  }

  async reject(transferId: string, reason?: string): Promise<void> {
    const transfer = this.requireTransfer(transferId);
    await this.sendControl(transfer.peerId, { type: "file.reject", transferId, reason });
    await this.sink.discard(transferId);
    transfer.state = "cancelled";
    this.emitProgress(transfer);
    this.transfers.delete(transferId);
  }

  async cancel(transferId: string): Promise<void> {
    const transfer = this.transfers.get(transferId);
    if (!transfer) return;
    transfer.state = "cancelled";
    this.emitProgress(transfer);
    await this.waitForInFlightWrites(transferId);
    await this.sink.discard(transferId);
    this.transfers.delete(transferId);
  }

  async getMissingChunksForResume(transferId: string): Promise<number[]> {
    const transfer = this.requireTransfer(transferId);
    return transfer.missingChunks
      ? [...transfer.missingChunks]
      : this.getMissingChunks(transfer);
  }

  private async handleOffer(peerId: string, message: FileOfferMessage): Promise<void> {
    if (this.transfers.has(message.transferId)) return;
    if (this.transfers.size >= this.maxIncomingTransfers) {
      const deferred = [...this.transfers.values()].find((transfer) => transfer.state === "offered");
      if (deferred) {
        await this.sendControl(deferred.peerId, {
          type: "file.reject",
          transferId: deferred.transferId,
          reason: "too_many_transfers",
        });
        await this.sink.discard(deferred.transferId);
        deferred.state = "cancelled";
        this.emitProgress(deferred);
        this.transfers.delete(deferred.transferId);
      }
      else {
        await this.sendControl(peerId, { type: "file.reject", transferId: message.transferId, reason: "too_many_transfers" });
        return;
      }
    }
    const errors = validateAttachmentManifest(message.manifest);
    if (errors.length > 0 || message.manifest.size > this.maxFileSizeBytes) {
      await this.sendControl(peerId, {
        type: "file.reject",
        transferId: message.transferId,
        reason: errors.length > 0 ? errors.join(",") : "file_too_large",
      });
      return;
    }

    const transfer: IncomingTransfer = {
      peerId,
      transferId: message.transferId,
      manifest: message.manifest,
      receivedChunks: new Set(),
      bytesTransferred: 0,
      state: "offered",
      retryCount: 0,
      processingChunks: new Set(),
    };
    this.transfers.set(message.transferId, transfer);
    this.emitOffer(transfer);
    try {
      await this.options.onOffer?.(peerId, message.transferId, message.manifest);
    } catch {
      this.transfers.delete(message.transferId);
      await this.sendControl(peerId, { type: "file.reject", transferId: message.transferId, reason: "storage_unavailable" });
      return;
    }

    let shouldAutoAccept = false;
    try {
      shouldAutoAccept = Boolean(this.options.autoAccept && await this.options.autoAccept(peerId, message.manifest));
    } catch {
      this.transfers.delete(message.transferId);
      await this.sink.discard(message.transferId).catch(() => undefined);
      await this.sendControl(peerId, { type: "file.reject", transferId: message.transferId, reason: "storage_unavailable" });
      return;
    }
    if (shouldAutoAccept) {
      try {
        await this.accept(message.transferId);
      } catch {
        await this.sink.discard(message.transferId).catch(() => undefined);
        this.transfers.delete(message.transferId);
        await this.sendControl(peerId, { type: "file.reject", transferId: message.transferId, reason: "storage_unavailable" });
      }
    }
  }

  private async ensurePrepared(peerId: string, transferId: string, manifest: AttachmentManifest): Promise<void> {
    const existing = this.transfers.get(transferId);
    if (existing) {
      if (existing.peerId !== peerId || existing.manifest.contentHash !== manifest.contentHash) {
        throw new Error("Manifesto divergente para transferência existente.");
      }
      return;
    }
    await this.handleOffer(peerId, { type: "file.offer", transferId, manifest });
  }

  private async complete(peerId: string, transferId: string, announcedHash: string): Promise<void> {
    const transfer = this.requireTransfer(transferId);
    if (transfer.peerId !== peerId) throw new Error("Peer divergente ao finalizar transferência.");
    if (transfer.state !== "accepted" && transfer.state !== "transferring") return;
    await this.waitForInFlightWrites(transferId);
    if (this.transfers.get(transferId) !== transfer || !this.isReceiving(transfer)) return;
    const missing = transfer.missingChunks
      ? [...transfer.missingChunks]
      : await this.getMissingChunks(transfer);
    transfer.missingChunks ??= new Set(missing);
    if (missing.length > 0) {
      await this.requestMissing(peerId, transfer, missing);
      return;
    }

    transfer.state = "verifying";
    this.emitProgress(transfer);
    try {
      const result = await this.sink.finalize(transferId, transfer.manifest);
      if (this.transfers.get(transferId) !== transfer || transfer.state !== "verifying") {
        await this.sink.discard(transferId).catch(() => undefined);
        return;
      }
      if (result.contentHash.toLowerCase() !== transfer.manifest.contentHash.toLowerCase() || result.contentHash.toLowerCase() !== announcedHash.toLowerCase()) {
        throw new Error("Hash final do arquivo não confere com o manifesto.");
      }
      transfer.bytesTransferred = transfer.manifest.size;
      transfer.state = "completed";
      this.emitProgress(transfer);
      await this.sendControl(peerId, { type: "file.complete", transferId, contentHash: result.contentHash });
      this.transfers.delete(transferId);
    } catch (error) {
      if (this.transfers.get(transferId) !== transfer || this.hasState(transfer, "cancelled")) return;
      transfer.state = "failed";
      this.emitProgress(transfer, error instanceof Error ? error.message : String(error));
      await this.sendControl(peerId, {
        type: "file.error",
        transferId,
        code: "integrity_check_failed",
        message: error instanceof Error ? error.message : String(error),
        retryable: true,
      });
    }
  }

  private isReceiving(transfer: IncomingTransfer): boolean {
    return this.transfers.get(transfer.transferId) === transfer
      && (transfer.state === "accepted" || transfer.state === "transferring");
  }

  private hasState(transfer: IncomingTransfer, state: AttachmentTransferProgress["state"]): boolean {
    return transfer.state === state;
  }

  private async waitForInFlightWrites(transferId: string): Promise<void> {
    for (;;) {
      const writes = this.inFlightWrites.get(transferId);
      if (!writes?.size) return;
      await Promise.allSettled([...writes]);
    }
  }

  private async requestMissing(peerId: string, transfer: IncomingTransfer, missingChunks: number[]): Promise<void> {
    await this.sendControl(peerId, {
      type: "file.need",
      transferId: transfer.transferId,
      missingChunks: missingChunks.slice(0, MAX_MISSING_CHUNKS_PER_REQUEST),
    });
  }

  private async getMissingChunks(transfer: IncomingTransfer): Promise<number[]> {
    if (this.sink.listMissingChunks) {
      return this.sink.listMissingChunks(transfer.transferId, transfer.manifest.chunkCount);
    }
    const missing: number[] = [];
    for (let index = 0; index < transfer.manifest.chunkCount; index += 1) {
      if (transfer.receivedChunks.has(index) || await this.sink.hasChunk(transfer.transferId, index)) continue;
      missing.push(index);
    }
    return missing;
  }

  private updateState(transferId: string, state: AttachmentTransferProgress["state"]): void {
    const transfer = this.transfers.get(transferId);
    if (!transfer) return;
    transfer.state = state;
    this.emitProgress(transfer);
  }

  private requireTransfer(transferId: string): IncomingTransfer {
    const transfer = this.transfers.get(transferId);
    if (!transfer) throw new Error(`Transferência recebida desconhecida: ${transferId}`);
    return transfer;
  }

  private emitOffer(transfer: IncomingTransfer): void {
    this.dispatchEvent(new CustomEvent("offer", {
      detail: { transferId: transfer.transferId, peerId: transfer.peerId, manifest: transfer.manifest },
    }));
    this.emitProgress(transfer);
  }

  private emitProgress(transfer: IncomingTransfer, lastError?: string): void {
    const detail: AttachmentTransferProgress = {
      transferId: transfer.transferId,
      attachmentId: transfer.manifest.id,
      peerId: transfer.peerId,
      state: transfer.state,
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

class AsyncSemaphore {
  private active = 0;
  private readonly waiters: Array<() => void> = [];

  constructor(private readonly limit: number) {
    if (!Number.isSafeInteger(limit) || limit < 1) throw new Error("maxConcurrentChunkWrites deve ser maior que zero.");
  }

  async run<T>(operation: () => Promise<T>): Promise<T> {
    if (this.active >= this.limit) await new Promise<void>((resolve) => this.waiters.push(resolve));
    this.active += 1;
    try {
      return await operation();
    } finally {
      this.active -= 1;
      this.waiters.shift()?.();
    }
  }
}
