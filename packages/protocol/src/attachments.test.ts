import { describe, expect, it } from "vitest";
import {
  BRIKS_ATTACHMENT_PROTOCOL_VERSION,
  classifyAttachment,
  inferAttachmentMimeType,
  isFileControlMessage,
  sanitizeAttachmentFilename,
  validateAttachmentManifest,
  type AttachmentManifest,
} from "./attachments";

function manifest(overrides: Partial<AttachmentManifest> = {}): AttachmentManifest {
  return {
    protocolVersion: BRIKS_ATTACHMENT_PROTOCOL_VERSION,
    id: "a".repeat(64),
    senderPeerId: "peer_12345678",
    filename: "arquivo.zip",
    mimeType: "application/zip",
    kind: "archive",
    size: 64,
    contentHash: "a".repeat(64),
    chunkSize: 64,
    chunkCount: 1,
    createdAt: new Date(0).toISOString(),
    ...overrides,
  };
}

describe("attachment protocol validation", () => {
  it("accepts a bounded valid manifest", () => {
    expect(validateAttachmentManifest(manifest())).toEqual([]);
  });

  it("rejects invalid hashes and chunk counts", () => {
    expect(validateAttachmentManifest(manifest({ contentHash: "nope", chunkCount: -1 }))).toEqual(
      expect.arrayContaining(["invalid_content_hash", "invalid_chunk_count"]),
    );
  });

  it("rejects divergent ids, oversized chunks and inconsistent layouts", () => {
    expect(validateAttachmentManifest(manifest({
      id: "b".repeat(64),
      chunkSize: 256 * 1024 + 1,
      chunkCount: 2,
    }))).toEqual(expect.arrayContaining([
      "attachment_id_hash_mismatch",
      "invalid_chunk_size",
      "invalid_chunk_layout",
    ]));
  });

  it("does not accept malformed control envelopes by type alone", () => {
    expect(isFileControlMessage({ type: "file.offer" })).toBe(false);
    expect(isFileControlMessage({ type: "file.need", transferId: "transfer_12345678", missingChunks: new Array(2_049).fill(0) })).toBe(false);
    expect(isFileControlMessage({ type: "file.offer", transferId: "transfer_12345678", manifest: manifest() })).toBe(true);
    expect(isFileControlMessage({ type: "peer.capabilities", protocolVersion: 999, capabilities: [] })).toBe(false);
  });

  it("removes path traversal and reserved filename characters", () => {
    expect(sanitizeAttachmentFilename("../../bad<name>.exe")).toBe("bad_name_.exe");
  });

  it("infers image MIME and kind from extension when the picker omits MIME", () => {
    expect(inferAttachmentMimeType("", "captura.PNG")).toBe("image/png");
    expect(inferAttachmentMimeType("application/octet-stream", "foto.jpeg")).toBe("image/jpeg");
    expect(classifyAttachment("application/octet-stream", "foto.jpeg")).toBe("image");
  });

  it("keeps a specific MIME supplied by the platform", () => {
    expect(inferAttachmentMimeType("image/webp", "foto.bin")).toBe("image/webp");
    expect(classifyAttachment("image/webp", "foto.bin")).toBe("image");
  });
});
