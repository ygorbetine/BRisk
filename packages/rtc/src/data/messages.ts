export const CONTROL_CHANNEL_LABEL = "briks.chat";
export const TRANSFER_CHANNEL_LABEL = "briks.transfer";
export const MAX_CONTROL_MESSAGE_BYTES = 64 * 1024;
export const MAX_DATA_BUFFER_BYTES = 512 * 1024;
export const MAX_TRANSFER_FRAME_BYTES = 320 * 1024;
export const TRANSFER_HIGH_WATER_MARK_BYTES = 4 * 1024 * 1024;
export const TRANSFER_LOW_WATER_MARK_BYTES = 1 * 1024 * 1024;
export const TRANSFER_BUFFER_WAIT_TIMEOUT_MS = 15_000;

export function exactArrayBuffer(data: ArrayBuffer | ArrayBufferView): ArrayBuffer {
  if (data instanceof ArrayBuffer) return data;
  return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength) as ArrayBuffer;
}

export function encodedMessageSize(message: string): number {
  return new TextEncoder().encode(message).byteLength;
}
