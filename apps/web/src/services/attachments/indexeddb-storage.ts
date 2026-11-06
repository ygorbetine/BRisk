import type { AttachmentChunkFrame, AttachmentManifest, AttachmentTransferProgress } from "@briks/protocol/attachments";
import type { AttachmentChunkSink } from "@briks/rtc/file-receiver";
import { sha256Hex } from "@briks/rtc/file-transfer";
import {
  OFFLINE_STORES,
  deleteAllByIndex,
  deleteFromStore,
  getAllByIndex,
  getAllPrimaryKeysByIndex,
  getFromStore,
  putInStore,
} from "../offline/database";
import { StreamingSha256 } from "./streaming-sha256";

export type AttachmentDirection = "incoming" | "outgoing";

export type StoredAttachmentRecord = {
  recordId: string;
  attachmentId: string;
  transferId: string;
  channelId: string;
  peerId: string;
  direction: AttachmentDirection;
  sourcePersisted?: boolean;
  manifest: AttachmentManifest;
  state: AttachmentTransferProgress["state"];
  bytesTransferred: number;
  totalBytes: number;
  retryCount: number;
  createdAt: string;
  updatedAt: string;
  completedAt?: string;
  lastError?: string;
};

type StoredAttachmentChunk = {
  id: string;
  attachmentId: string;
  index: number;
  hash: string;
  size: number;
  payload: ArrayBuffer;
};

export class IndexedDbAttachmentStorage implements AttachmentChunkSink {
  async prepare(transferId: string, manifest: AttachmentManifest): Promise<void> {
    const existing = await this.findByTransferId(transferId);
    if (existing) return;
    await this.saveRecord({
      recordId: recordId(manifest.channelId ?? "unknown", manifest.id, transferId),
      attachmentId: manifest.id,
      transferId,
      channelId: manifest.channelId ?? "unknown",
      peerId: manifest.senderPeerId,
      direction: "incoming",
      manifest,
      state: "accepted",
      bytesTransferred: 0,
      totalBytes: manifest.size,
      retryCount: 0,
      createdAt: manifest.createdAt,
      updatedAt: new Date().toISOString(),
    });
  }

  async hasChunk(transferId: string, index: number): Promise<boolean> {
    const record = await this.requireTransfer(transferId);
    return Boolean(await getFromStore<StoredAttachmentChunk>(OFFLINE_STORES.attachmentChunks, chunkId(record.attachmentId, index)));
  }

  async listMissingChunks(transferId: string, chunkCount: number): Promise<number[]> {
    const record = await this.requireTransfer(transferId);
    const prefix = `${record.attachmentId}:`;
    const present = new Set((await getAllPrimaryKeysByIndex(OFFLINE_STORES.attachmentChunks, "attachmentId", record.attachmentId))
      .filter((key): key is string => typeof key === "string" && key.startsWith(prefix))
      .map((key) => Number(key.slice(prefix.length)))
      .filter((index) => Number.isSafeInteger(index) && index >= 0 && index < chunkCount));
    const missing: number[] = [];
    for (let index = 0; index < chunkCount; index += 1) if (!present.has(index)) missing.push(index);
    return missing;
  }

  async writeChunk(transferId: string, frame: AttachmentChunkFrame): Promise<void> {
    const record = await this.requireTransfer(transferId);
    if (record.attachmentId !== frame.attachmentId) throw new Error("Chunk não pertence ao anexo armazenado.");
    const payload = frame.payload.slice(0);
    await putInStore(OFFLINE_STORES.attachmentChunks, {
      id: chunkId(record.attachmentId, frame.index),
      attachmentId: record.attachmentId,
      index: frame.index,
      hash: frame.hash,
      size: frame.size,
      payload,
    } satisfies StoredAttachmentChunk);
  }

  async finalize(transferId: string, manifest: AttachmentManifest): Promise<{ contentHash: string }> {
    const record = await this.requireTransfer(transferId);
    const hasher = new StreamingSha256();
    let total = 0;
    for (let index = 0; index < manifest.chunkCount; index += 1) {
      const chunk = await getFromStore<StoredAttachmentChunk>(OFFLINE_STORES.attachmentChunks, chunkId(manifest.id, index));
      if (!chunk) throw new Error(`Chunk ${index} ausente durante a finalização.`);
      hasher.update(chunk.payload);
      total += chunk.size;
    }
    if (total !== manifest.size) throw new Error("Tamanho final do anexo não confere com o manifesto.");
    const contentHash = hasher.digestHex();
    const now = new Date().toISOString();
    await this.saveRecord({ ...record, state: "completed", bytesTransferred: manifest.size, updatedAt: now, completedAt: now });
    return { contentHash };
  }

  async discard(transferId: string): Promise<void> {
    const record = await this.findByTransferId(transferId);
    if (!record) return;
    await deleteFromStore(OFFLINE_STORES.attachments, record.recordId);
    const remaining = await getAllByIndex<StoredAttachmentRecord>(OFFLINE_STORES.attachments, "attachmentId", record.attachmentId);
    if (remaining.length === 0) await deleteAllByIndex(OFFLINE_STORES.attachmentChunks, "attachmentId", record.attachmentId);
  }

  async persistOutgoingSource(
    transferId: string,
    channelId: string,
    peerId: string,
    source: Blob,
    manifest: AttachmentManifest,
  ): Promise<StoredAttachmentRecord> {
    const record = await this.registerOutgoing(transferId, channelId, peerId, manifest);
    for (let index = 0; index < manifest.chunkCount; index += 1) {
      const offset = index * manifest.chunkSize;
      const payload = await source.slice(offset, Math.min(source.size, offset + manifest.chunkSize)).arrayBuffer();
      const hash = manifest.chunkHashes?.[index] ?? await sha256Hex(payload);
      await putInStore(OFFLINE_STORES.attachmentChunks, {
        id: chunkId(manifest.id, index),
        attachmentId: manifest.id,
        index,
        hash,
        size: payload.byteLength,
        payload,
      } satisfies StoredAttachmentChunk);
    }
    const persisted = { ...record, sourcePersisted: true, updatedAt: new Date().toISOString() };
    await this.saveRecord(persisted);
    return persisted;
  }

  async registerOutgoing(
    transferId: string,
    channelId: string,
    peerId: string,
    manifest: AttachmentManifest,
  ): Promise<StoredAttachmentRecord> {
    const record: StoredAttachmentRecord = {
      recordId: recordId(channelId, manifest.id, transferId),
      attachmentId: manifest.id,
      transferId,
      channelId,
      peerId,
      direction: "outgoing",
      sourcePersisted: false,
      manifest,
      state: "offered",
      bytesTransferred: 0,
      totalBytes: manifest.size,
      retryCount: 0,
      createdAt: manifest.createdAt,
      updatedAt: new Date().toISOString(),
    };
    await this.saveRecord(record);
    return record;
  }

  async registerSyncedMetadata(peerId: string, manifest: AttachmentManifest): Promise<StoredAttachmentRecord> {
    const existing = await this.findAnyByAttachmentId(manifest.id);
    if (existing) return existing;
    const transferId = `sync:${peerId}:${manifest.id}`;
    const record: StoredAttachmentRecord = {
      recordId: recordId(manifest.channelId ?? "unknown", manifest.id, transferId),
      attachmentId: manifest.id,
      transferId,
      channelId: manifest.channelId ?? "unknown",
      peerId,
      direction: "incoming",
      manifest,
      state: "waiting",
      bytesTransferred: 0,
      totalBytes: manifest.size,
      retryCount: 0,
      createdAt: manifest.createdAt,
      updatedAt: new Date().toISOString(),
    };
    await this.saveRecord(record);
    return record;
  }

  async updateProgress(progress: AttachmentTransferProgress): Promise<StoredAttachmentRecord | undefined> {
    const record = await this.findByTransferId(progress.transferId);
    if (!record) return undefined;
    const updated: StoredAttachmentRecord = {
      ...record,
      state: progress.state,
      bytesTransferred: progress.bytesTransferred,
      totalBytes: progress.totalBytes,
      retryCount: progress.retryCount,
      updatedAt: progress.updatedAt,
      completedAt: progress.state === "completed" ? progress.updatedAt : record.completedAt,
      lastError: progress.lastError,
    };
    await this.saveRecord(updated);
    return updated;
  }

  async listChannel(channelId: string): Promise<StoredAttachmentRecord[]> {
    return (await getAllByIndex<StoredAttachmentRecord>(OFFLINE_STORES.attachments, "channelId", channelId))
      .sort((left, right) => left.createdAt.localeCompare(right.createdAt));
  }

  async findByTransferId(transferId: string): Promise<StoredAttachmentRecord | undefined> {
    return (await getAllByIndex<StoredAttachmentRecord>(OFFLINE_STORES.attachments, "transferId", transferId))[0];
  }

  async findAnyByAttachmentId(attachmentId: string): Promise<StoredAttachmentRecord | undefined> {
    return (await getAllByIndex<StoredAttachmentRecord>(OFFLINE_STORES.attachments, "attachmentId", attachmentId))[0];
  }

  async findCompletedByAttachmentId(attachmentId: string): Promise<StoredAttachmentRecord | undefined> {
    const records = await getAllByIndex<StoredAttachmentRecord>(OFFLINE_STORES.attachments, "attachmentId", attachmentId);
    return records.find((record) => record.direction === "incoming"
      ? record.state === "completed"
      : record.sourcePersisted === true);
  }

  async getBlob(attachmentId: string, manifest?: AttachmentManifest): Promise<Blob> {
    const record = manifest ? undefined : await this.findCompletedByAttachmentId(attachmentId);
    const resolvedManifest = manifest ?? record?.manifest;
    if (!resolvedManifest) throw new Error("Anexo concluído não encontrado no armazenamento local.");
    const parts: ArrayBuffer[] = [];
    for (let index = 0; index < resolvedManifest.chunkCount; index += 1) {
      const chunk = await getFromStore<StoredAttachmentChunk>(OFFLINE_STORES.attachmentChunks, chunkId(attachmentId, index));
      if (!chunk) throw new Error(`Chunk ${index} não está disponível localmente.`);
      parts.push(chunk.payload);
    }
    return new Blob(parts, { type: resolvedManifest.mimeType || "application/octet-stream" });
  }

  async saveRecord(record: StoredAttachmentRecord): Promise<void> {
    await putInStore(OFFLINE_STORES.attachments, record);
  }

  private async requireTransfer(transferId: string): Promise<StoredAttachmentRecord> {
    const record = await this.findByTransferId(transferId);
    if (!record) throw new Error(`Transferência ${transferId} não está registrada no IndexedDB.`);
    return record;
  }
}

function recordId(channelId: string, attachmentId: string, transferId: string): string {
  return `${channelId}:${attachmentId}:${transferId}`;
}

function chunkId(attachmentId: string, index: number): string {
  return `${attachmentId}:${index}`;
}
