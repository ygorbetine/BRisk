import { describe, expect, it } from "vitest";
import {
  MAX_ATTACHMENT_CONTROL_WIRE_BYTES,
  type AttachmentManifest,
  type AttachmentTransferProgress,
} from "@briks/protocol/attachments";
import type { MeshWebRTCTransport } from "@briks/rtc";
import { AttachmentService, type AttachmentStorage } from "./attachment-service";
import type { StoredAttachmentRecord } from "./indexeddb-storage";

const attachmentId = "a".repeat(64);
const manifest: AttachmentManifest = {
  protocolVersion: 1,
  id: attachmentId,
  channelId: "channel_12345678",
  senderPeerId: "peer_12345678",
  filename: "arquivo.bin",
  mimeType: "application/octet-stream",
  kind: "other",
  size: 64,
  contentHash: attachmentId,
  chunkSize: 64,
  chunkCount: 1,
  createdAt: new Date(0).toISOString(),
};

function record(): StoredAttachmentRecord {
  return {
    recordId: `channel_12345678:${attachmentId}:sync:peer_12345678:${attachmentId}`,
    attachmentId,
    transferId: `sync:peer_12345678:${attachmentId}`,
    channelId: "channel_12345678",
    peerId: "peer_12345678",
    direction: "incoming",
    manifest,
    state: "waiting",
    bytesTransferred: 0,
    totalBytes: 64,
    retryCount: 0,
    createdAt: manifest.createdAt,
    updatedAt: manifest.createdAt,
  };
}

describe("AttachmentService request errors", () => {
  it("marks a synchronized attachment as failed when the source no longer has it", async () => {
    let current = record();
    const storage: AttachmentStorage = {
      prepare: async () => undefined,
      hasChunk: async () => false,
      writeChunk: async () => undefined,
      finalize: async () => ({ contentHash: attachmentId }),
      discard: async () => undefined,
      persistOutgoingSource: async () => current,
      registerOutgoing: async () => current,
      registerSyncedMetadata: async () => current,
      updateProgress: async () => current,
      listChannel: async () => [current],
      findByTransferId: async () => current,
      findAnyByAttachmentId: async () => current,
      findCompletedByAttachmentId: async () => undefined,
      getBlob: async () => new Blob(),
      saveRecord: async (next) => { current = next; },
    };
    const transport = {
      sendData: () => 1,
      sendTransferData: () => 1,
      waitForTransferBufferedAmountLow: async () => undefined,
      getTransferBufferedAmount: () => 0,
      isTransferChannelOpen: () => true,
      ensureTransferChannel: () => undefined,
    } as unknown as MeshWebRTCTransport;
    const service = new AttachmentService(transport, "channel_12345678", "self_12345678", () => ["peer_12345678"], storage);

    await service.handleControlString("peer_12345678", JSON.stringify({
      type: "file.error",
      transferId: `request:${attachmentId}`,
      code: "attachment_unavailable",
      message: "Arquivo removido no peer remoto.",
      retryable: false,
    }));

    expect(current.state).toBe("failed");
    expect(current.lastError).toBe("Arquivo removido no peer remoto.");
  });

  it("rejects oversized control messages before parsing JSON", async () => {
    const storage = {} as AttachmentStorage;
    const transport = {} as MeshWebRTCTransport;
    const service = new AttachmentService(transport, "channel_12345678", "self_12345678", () => ["peer_12345678"], storage);

    await expect(service.handleControlString("peer_12345678", " ".repeat(MAX_ATTACHMENT_CONTROL_WIRE_BYTES + 1))).resolves.toBe(false);
  });
});

describe("AttachmentService transfer controls", () => {
  it("persists pause, resume and cancellation in the order requested", async () => {
    const transferId = "transfer_12345678";
    let current: StoredAttachmentRecord | undefined = {
      ...record(),
      recordId: `channel_12345678:${attachmentId}:${transferId}`,
      transferId,
      state: "transferring",
    };
    const storage: AttachmentStorage = {
      prepare: async () => undefined,
      hasChunk: async () => false,
      writeChunk: async () => undefined,
      finalize: async () => ({ contentHash: attachmentId }),
      discard: async () => { current = undefined; },
      persistOutgoingSource: async () => current!,
      registerOutgoing: async () => current!,
      registerSyncedMetadata: async () => current!,
      updateProgress: async (progress) => {
        if (!current) return undefined;
        current = {
          ...current,
          state: progress.state,
          bytesTransferred: progress.bytesTransferred,
          updatedAt: progress.updatedAt,
        };
        return current;
      },
      listChannel: async () => current ? [current] : [],
      findByTransferId: async () => current,
      findAnyByAttachmentId: async () => current,
      findCompletedByAttachmentId: async () => undefined,
      getBlob: async () => new Blob(),
      saveRecord: async (next) => { current = next; },
    };
    const transport = {
      sendData: () => 1,
      sendTransferData: () => 1,
      waitForTransferBufferedAmountLow: async () => undefined,
      getTransferBufferedAmount: () => 0,
      isTransferChannelOpen: () => true,
      ensureTransferChannel: () => undefined,
    } as unknown as MeshWebRTCTransport;
    const service = new AttachmentService(transport, "channel_12345678", "self_12345678", () => ["peer_12345678"], storage);

    await service.handleControlString("peer_12345678", JSON.stringify({
      type: "file.offer",
      transferId,
      manifest,
    }));
    await service.pause(current!);
    expect(current?.state).toBe("paused");

    await service.resume(current!);
    expect(current?.state).toBe("transferring");

    await service.cancel(current!);
    expect(current?.state).toBe("cancelled");
  });

  it("uses a rolling window for speed and resets it across a pause", () => {
    const service = new AttachmentService({} as MeshWebRTCTransport, "channel_12345678", "self_12345678", () => [], {} as AttachmentStorage);
    const updateSpeed = (service as unknown as {
      updateTransferSpeed(progress: AttachmentTransferProgress, observedAt: number): number;
    }).updateTransferSpeed.bind(service);
    const progress = (bytesTransferred: number, state: AttachmentTransferProgress["state"] = "transferring"): AttachmentTransferProgress => ({
      transferId: "transfer_12345678",
      attachmentId,
      peerId: "peer_12345678",
      state,
      bytesTransferred,
      totalBytes: 10_000,
      retryCount: 0,
      updatedAt: new Date(0).toISOString(),
    });

    expect(updateSpeed(progress(0), 0)).toBe(0);
    expect(updateSpeed(progress(1_000), 1_000)).toBe(1_000);
    expect(updateSpeed(progress(3_000), 2_000)).toBe(1_500);
    expect(updateSpeed(progress(3_500), 3_000)).toBeCloseTo(1_166.67, 1);
    expect(updateSpeed(progress(3_500, "paused"), 4_000)).toBe(0);
    expect(updateSpeed(progress(3_500), 5_000)).toBe(0);
    expect(updateSpeed(progress(4_500), 6_000)).toBe(1_000);
  });
});
