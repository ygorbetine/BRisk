import type { AttachmentChunkFrame } from "@briks/protocol/attachments";

const MAGIC = 0x5249534b; // BRIKS
const VERSION = 1;
const FIXED_HEADER_BYTES = 16;
const MAX_METADATA_BYTES = 16 * 1024;
const MAX_PAYLOAD_BYTES = 256 * 1024;

export type AttachmentChunkMetadata = Omit<AttachmentChunkFrame, "payload">;

export function encodeAttachmentChunkFrame(frame: AttachmentChunkFrame): ArrayBuffer {
  validateFrame(frame);
  const metadata: AttachmentChunkMetadata = {
    transferId: frame.transferId,
    attachmentId: frame.attachmentId,
    index: frame.index,
    offset: frame.offset,
    size: frame.size,
    hash: frame.hash,
  };
  const metadataBytes = new TextEncoder().encode(JSON.stringify(metadata));
  if (metadataBytes.byteLength > MAX_METADATA_BYTES) throw new Error("Metadados do chunk excedem o limite do protocolo.");
  if (frame.payload.byteLength > MAX_PAYLOAD_BYTES) throw new Error("Payload do chunk excede o limite do protocolo.");

  const output = new ArrayBuffer(FIXED_HEADER_BYTES + metadataBytes.byteLength + frame.payload.byteLength);
  const view = new DataView(output);
  view.setUint32(0, MAGIC, false);
  view.setUint8(4, VERSION);
  view.setUint8(5, 0);
  view.setUint16(6, FIXED_HEADER_BYTES, false);
  view.setUint32(8, metadataBytes.byteLength, false);
  view.setUint32(12, frame.payload.byteLength, false);

  const bytes = new Uint8Array(output);
  bytes.set(metadataBytes, FIXED_HEADER_BYTES);
  bytes.set(new Uint8Array(frame.payload), FIXED_HEADER_BYTES + metadataBytes.byteLength);
  return output;
}

export function decodeAttachmentChunkFrame(data: ArrayBuffer | ArrayBufferView): AttachmentChunkFrame {
  const buffer = toExactArrayBuffer(data);
  if (buffer.byteLength < FIXED_HEADER_BYTES) throw new Error("Frame binário de anexo truncado.");
  const view = new DataView(buffer);
  if (view.getUint32(0, false) !== MAGIC) throw new Error("Frame binário de anexo inválido.");
  if (view.getUint8(4) !== VERSION) throw new Error("Versão do frame binário não suportada.");
  const headerBytes = view.getUint16(6, false);
  if (headerBytes !== FIXED_HEADER_BYTES) throw new Error("Cabeçalho binário incompatível.");
  const metadataLength = view.getUint32(8, false);
  const payloadLength = view.getUint32(12, false);
  if (metadataLength > MAX_METADATA_BYTES || payloadLength > MAX_PAYLOAD_BYTES) throw new Error("Frame binário excede os limites do protocolo.");
  if (FIXED_HEADER_BYTES + metadataLength + payloadLength !== buffer.byteLength) throw new Error("Tamanho do frame binário inconsistente.");

  const bytes = new Uint8Array(buffer);
  const metadataText = new TextDecoder().decode(bytes.subarray(FIXED_HEADER_BYTES, FIXED_HEADER_BYTES + metadataLength));
  let metadata: AttachmentChunkMetadata;
  try {
    metadata = JSON.parse(metadataText) as AttachmentChunkMetadata;
  } catch {
    throw new Error("Metadados do chunk não são JSON válido.");
  }
  const payloadStart = FIXED_HEADER_BYTES + metadataLength;
  const payload = buffer.slice(payloadStart, payloadStart + payloadLength);
  const frame: AttachmentChunkFrame = { ...metadata, payload };
  validateFrame(frame);
  if (frame.size !== payloadLength) throw new Error("Tamanho anunciado do chunk não confere com o payload.");
  return frame;
}

export function isAttachmentChunkFrame(data: ArrayBuffer | ArrayBufferView): boolean {
  const buffer = toExactArrayBuffer(data);
  return buffer.byteLength >= FIXED_HEADER_BYTES && new DataView(buffer).getUint32(0, false) === MAGIC;
}

function validateFrame(frame: AttachmentChunkFrame): void {
  if (!frame.transferId || frame.transferId.length > 128) throw new Error("transferId inválido no chunk.");
  if (!frame.attachmentId || frame.attachmentId.length > 128) throw new Error("attachmentId inválido no chunk.");
  if (!Number.isSafeInteger(frame.index) || frame.index < 0) throw new Error("Índice inválido no chunk.");
  if (!Number.isSafeInteger(frame.offset) || frame.offset < 0) throw new Error("Offset inválido no chunk.");
  if (!Number.isSafeInteger(frame.size) || frame.size < 0 || frame.size !== frame.payload.byteLength) throw new Error("Tamanho inválido no chunk.");
  if (!/^[a-f0-9]{64}$/i.test(frame.hash)) throw new Error("Hash inválido no chunk.");
}

function toExactArrayBuffer(data: ArrayBuffer | ArrayBufferView): ArrayBuffer {
  if (data instanceof ArrayBuffer) return data;
  return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength) as ArrayBuffer;
}
