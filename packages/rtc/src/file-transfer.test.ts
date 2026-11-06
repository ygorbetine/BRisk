import { describe, expect, it, vi } from "vitest";
import { BRIKS_ATTACHMENT_PROTOCOL_VERSION, type AttachmentManifest } from "@briks/protocol/attachments";
import { AttachmentTransferSender } from "./file-transfer";

describe("AttachmentTransferSender controls", () => {
  it("does not send a chunk when paused while waiting for backpressure", async () => {
    let releaseBuffer!: () => void;
    const bufferPending = new Promise<void>((resolve) => { releaseBuffer = resolve; });
    const sendChunk = vi.fn(async () => undefined);
    const waitForBufferedAmountLow = vi.fn(() => bufferPending);
    const sender = new AttachmentTransferSender(
      vi.fn(async () => undefined),
      sendChunk,
      {
        getBufferedAmount: () => 8 * 1024 * 1024,
        waitForBufferedAmountLow,
      },
    );
    const source = new Blob([Uint8Array.from([1, 2])], { type: "application/octet-stream" });
    const manifest: AttachmentManifest = {
      protocolVersion: BRIKS_ATTACHMENT_PROTOCOL_VERSION,
      id: "a".repeat(64),
      channelId: "channel_12345678",
      senderPeerId: "peer_12345678",
      filename: "arquivo.bin",
      mimeType: "application/octet-stream",
      kind: "other",
      size: source.size,
      contentHash: "a".repeat(64),
      chunkSize: source.size,
      chunkCount: 1,
      createdAt: new Date(0).toISOString(),
    };

    const transferId = await sender.offer("peer_12345678", source, manifest);
    await sender.acceptAndStart(transferId);
    await vi.waitFor(() => expect(waitForBufferedAmountLow).toHaveBeenCalledOnce());
    sender.pause(transferId);
    releaseBuffer();
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(sendChunk).not.toHaveBeenCalled();

    sender.resume(transferId);
    await vi.waitFor(() => expect(sendChunk).toHaveBeenCalledOnce());
  });

  it("drains pending transfer data before announcing completion", async () => {
    let releaseDrain!: () => void;
    const drainPending = new Promise<void>((resolve) => { releaseDrain = resolve; });
    const controls: string[] = [];
    const sender = new AttachmentTransferSender(
      async (_peerId, message) => { controls.push(message.type); },
      async () => undefined,
      { waitForPendingData: () => drainPending },
    );
    const source = new Blob([Uint8Array.from([1, 2])], { type: "application/octet-stream" });
    const manifest: AttachmentManifest = {
      protocolVersion: BRIKS_ATTACHMENT_PROTOCOL_VERSION,
      id: "a".repeat(64),
      channelId: "channel_12345678",
      senderPeerId: "peer_12345678",
      filename: "arquivo.bin",
      mimeType: "application/octet-stream",
      kind: "other",
      size: source.size,
      contentHash: "a".repeat(64),
      chunkSize: source.size,
      chunkCount: 1,
      createdAt: new Date(0).toISOString(),
    };

    const transferId = await sender.offer("peer_12345678", source, manifest);
    await sender.acceptAndStart(transferId);
    await new Promise((resolve) => setTimeout(resolve, 20));
    expect(controls).not.toContain("file.complete");

    releaseDrain();
    await vi.waitFor(() => expect(controls).toContain("file.complete"));
  });
});
