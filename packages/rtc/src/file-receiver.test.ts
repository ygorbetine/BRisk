import { describe, expect, it, vi } from "vitest";
import {
  BRIKS_ATTACHMENT_PROTOCOL_VERSION,
  type AttachmentChunkFrame,
  type AttachmentManifest,
  type FileControlMessage,
} from "@briks/protocol/attachments";
import { AttachmentTransferReceiver, type AttachmentChunkSink } from "./file-receiver";
import { sha256Hex } from "./file-transfer";

function manifest(): AttachmentManifest {
  return {
    protocolVersion: BRIKS_ATTACHMENT_PROTOCOL_VERSION,
    id: "a".repeat(64),
    channelId: "channel_12345678",
    senderPeerId: "peer_12345678",
    filename: "arquivo.bin",
    mimeType: "application/octet-stream",
    kind: "other",
    size: 8,
    contentHash: "a".repeat(64),
    chunkSize: 2,
    chunkCount: 4,
    createdAt: new Date(0).toISOString(),
  };
}

function sink(missing: number[]): AttachmentChunkSink & { listMissingChunks: ReturnType<typeof vi.fn> } {
  return {
    prepare: vi.fn(async () => undefined),
    hasChunk: vi.fn(async () => false),
    writeChunk: vi.fn(async () => undefined),
    finalize: vi.fn(async () => ({ contentHash: "a".repeat(64) })),
    discard: vi.fn(async () => undefined),
    listMissingChunks: vi.fn(async () => missing),
  };
}

describe("AttachmentTransferReceiver", () => {
  it("keeps offers pending until explicitly accepted", async () => {
    const storage = sink([0, 1, 2, 3]);
    const send = vi.fn(async (_peerId: string, _message: FileControlMessage) => undefined);
    const onOffer = vi.fn(async () => undefined);
    const receiver = new AttachmentTransferReceiver(storage, send, { onOffer });

    await receiver.handleControl("peer_12345678", {
      type: "file.offer",
      transferId: "transfer_12345678",
      manifest: manifest(),
    });

    expect(receiver.canAccept("transfer_12345678")).toBe(true);
    expect(storage.prepare).not.toHaveBeenCalled();
    expect(onOffer).toHaveBeenCalledOnce();
  });

  it("loads the missing index once and updates it as chunks arrive", async () => {
    const storage = sink([1, 3]);
    const send = vi.fn(async (_peerId: string, _message: FileControlMessage) => undefined);
    const receiver = new AttachmentTransferReceiver(storage, send);
    await receiver.handleControl("peer_12345678", {
      type: "file.offer",
      transferId: "transfer_12345678",
      manifest: manifest(),
    });
    await receiver.accept("transfer_12345678");

    const payload = Uint8Array.from([1, 2]);
    const frame: AttachmentChunkFrame = {
      transferId: "transfer_12345678",
      attachmentId: "a".repeat(64),
      index: 1,
      offset: 2,
      size: payload.byteLength,
      hash: await sha256Hex(payload),
      payload: payload.buffer,
    };
    await receiver.handleChunk("peer_12345678", frame);

    expect(await receiver.getMissingChunksForResume("transfer_12345678")).toEqual([3]);
    expect(storage.listMissingChunks).toHaveBeenCalledOnce();
    expect(storage.hasChunk).not.toHaveBeenCalled();
    expect(storage.writeChunk).toHaveBeenCalledOnce();
    expect(send).toHaveBeenCalledWith("peer_12345678", {
      type: "file.need",
      transferId: "transfer_12345678",
      missingChunks: [1, 3],
    });
  });

  it("evicts only an unaccepted offer when the pending limit is reached", async () => {
    const storage = sink([0, 1, 2, 3]);
    const receiver = new AttachmentTransferReceiver(storage, vi.fn(async () => undefined), { maxIncomingTransfers: 1 });
    await receiver.handleControl("peer_12345678", { type: "file.offer", transferId: "transfer_first_123", manifest: manifest() });
    await receiver.handleControl("peer_12345678", { type: "file.offer", transferId: "transfer_second_123", manifest: manifest() });

    expect(receiver.canAccept("transfer_first_123")).toBe(false);
    expect(receiver.canAccept("transfer_second_123")).toBe(true);
    expect(storage.discard).toHaveBeenCalledWith("transfer_first_123");
  });

  it("does not overwrite a pause while a chunk write is in flight", async () => {
    let releaseWrite!: () => void;
    const writePending = new Promise<void>((resolve) => { releaseWrite = resolve; });
    const storage = sink([0, 1, 2, 3]);
    storage.writeChunk = vi.fn(() => writePending);
    const receiver = new AttachmentTransferReceiver(storage, vi.fn(async () => undefined));
    const states: string[] = [];
    receiver.addEventListener("progress", (event) => {
      states.push((event as CustomEvent<{ state: string }>).detail.state);
    });
    await receiver.handleControl("peer_12345678", { type: "file.offer", transferId: "transfer_12345678", manifest: manifest() });
    await receiver.accept("transfer_12345678");

    const payload = Uint8Array.from([1, 2]);
    const handling = receiver.handleChunk("peer_12345678", {
      transferId: "transfer_12345678",
      attachmentId: "a".repeat(64),
      index: 0,
      offset: 0,
      size: payload.byteLength,
      hash: await sha256Hex(payload),
      payload: payload.buffer,
    });
    await vi.waitFor(() => expect(storage.writeChunk).toHaveBeenCalledOnce());
    await receiver.handleControl("peer_12345678", { type: "file.pause", transferId: "transfer_12345678" });
    releaseWrite();
    await handling;

    expect(states.at(-1)).toBe("paused");
  });

  it("cleans up a chunk that finishes writing after cancellation", async () => {
    let releaseWrite!: () => void;
    const writePending = new Promise<void>((resolve) => { releaseWrite = resolve; });
    const storage = sink([0, 1, 2, 3]);
    storage.writeChunk = vi.fn(() => writePending);
    const receiver = new AttachmentTransferReceiver(storage, vi.fn(async () => undefined));
    const states: string[] = [];
    receiver.addEventListener("progress", (event) => {
      states.push((event as CustomEvent<{ state: string }>).detail.state);
    });
    await receiver.handleControl("peer_12345678", { type: "file.offer", transferId: "transfer_12345678", manifest: manifest() });
    await receiver.accept("transfer_12345678");

    const payload = Uint8Array.from([1, 2]);
    const handling = receiver.handleChunk("peer_12345678", {
      transferId: "transfer_12345678",
      attachmentId: "a".repeat(64),
      index: 0,
      offset: 0,
      size: payload.byteLength,
      hash: await sha256Hex(payload),
      payload: payload.buffer,
    });
    await vi.waitFor(() => expect(storage.writeChunk).toHaveBeenCalledOnce());
    const cancelling = receiver.cancel("transfer_12345678");
    releaseWrite();
    await Promise.all([handling, cancelling]);

    expect(states.at(-1)).toBe("cancelled");
    expect(storage.discard).toHaveBeenCalledOnce();
  });

  it("limits chunk writes while still processing more than one concurrently", async () => {
    let activeWrites = 0;
    let peakWrites = 0;
    const storage = sink([0, 1, 2, 3]);
    storage.writeChunk = vi.fn(async () => {
      activeWrites += 1;
      peakWrites = Math.max(peakWrites, activeWrites);
      await new Promise((resolve) => setTimeout(resolve, 20));
      activeWrites -= 1;
    });
    const receiver = new AttachmentTransferReceiver(storage, vi.fn(async () => undefined), { maxConcurrentChunkWrites: 2 });
    await receiver.handleControl("peer_12345678", { type: "file.offer", transferId: "transfer_12345678", manifest: manifest() });
    await receiver.accept("transfer_12345678");
    const frames = await Promise.all([0, 1, 2, 3].map(async (index): Promise<AttachmentChunkFrame> => {
      const payload = Uint8Array.from([index, index + 1]);
      return {
        transferId: "transfer_12345678",
        attachmentId: "a".repeat(64),
        index,
        offset: index * 2,
        size: payload.byteLength,
        hash: await sha256Hex(payload),
        payload: payload.buffer,
      };
    }));

    await Promise.all(frames.map((frame) => receiver.handleChunk("peer_12345678", frame)));

    expect(peakWrites).toBe(2);
    expect(storage.writeChunk).toHaveBeenCalledTimes(4);
  });

  it("waits for pending chunk writes before finalizing", async () => {
    let releaseWrite!: () => void;
    const storage = sink([0]);
    storage.writeChunk = vi.fn(() => new Promise<void>((resolve) => { releaseWrite = resolve; }));
    const oneChunk = { ...manifest(), size: 2, chunkCount: 1 };
    const receiver = new AttachmentTransferReceiver(storage, vi.fn(async () => undefined), { maxConcurrentChunkWrites: 2 });
    await receiver.handleControl("peer_12345678", { type: "file.offer", transferId: "transfer_12345678", manifest: oneChunk });
    await receiver.accept("transfer_12345678");
    const payload = Uint8Array.from([1, 2]);
    const handling = receiver.handleChunk("peer_12345678", {
      transferId: "transfer_12345678",
      attachmentId: "a".repeat(64),
      index: 0,
      offset: 0,
      size: payload.byteLength,
      hash: await sha256Hex(payload),
      payload: payload.buffer,
    });
    await vi.waitFor(() => expect(storage.writeChunk).toHaveBeenCalledOnce());

    const completing = receiver.handleControl("peer_12345678", {
      type: "file.complete",
      transferId: "transfer_12345678",
      contentHash: "a".repeat(64),
    });
    expect(storage.finalize).not.toHaveBeenCalled();
    releaseWrite();
    await Promise.all([handling, completing]);

    expect(storage.finalize).toHaveBeenCalledOnce();
  });
});
