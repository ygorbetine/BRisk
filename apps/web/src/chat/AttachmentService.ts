import type { MeshWebRTCTransport } from "@briks/rtc";
import {
  AttachmentService as P2PAttachmentService,
  type AttachmentRuntimeState,
} from "../services/attachments/attachment-service";
import { createAttachmentStorage } from "../services/attachments/desktop-storage";
import type { StoredAttachmentRecord } from "../services/attachments/indexeddb-storage";

/**
 * Adapta o serviço de transferência P2P ao ciclo de vida de uma sessão de chat.
 * O controller não precisa conhecer storage, eventos DOM ou regras de download.
 */
export class ChatAttachmentService {
  private active?: P2PAttachmentService;
  private generation = 0;
  private readonly attachmentCallbacks = new Set<(record: StoredAttachmentRecord) => void>();
  private readonly progressCallbacks = new Set<(progress: AttachmentRuntimeState) => void>();

  async connect(
    transport: MeshWebRTCTransport,
    channelId: string,
    localPeerId: string,
    peers: () => string[],
    isCurrent: () => boolean,
  ): Promise<number | undefined> {
    const storage = await createAttachmentStorage();
    if (!isCurrent()) return undefined;
    const service = new P2PAttachmentService(transport, channelId, localPeerId, peers, storage);
    this.generation += 1;
    this.active = service;
    service.addEventListener("attachment", (event) => {
      if (this.active !== service) return;
      const record = (event as CustomEvent<StoredAttachmentRecord>).detail;
      this.attachmentCallbacks.forEach((callback) => callback(record));
    });
    service.addEventListener("progress", (event) => {
      if (this.active !== service) return;
      const progress = (event as CustomEvent<AttachmentRuntimeState>).detail;
      this.progressCallbacks.forEach((callback) => callback(progress));
    });
    return this.generation;
  }

  clear(expectedGeneration?: number): void {
    if (expectedGeneration !== undefined && expectedGeneration !== this.generation) return;
    this.generation += 1;
    this.active = undefined;
  }
  isConnected(): boolean { return Boolean(this.active); }
  currentGeneration(): number { return this.generation; }

  async history(channelId: string, isCurrentChannel: boolean): Promise<StoredAttachmentRecord[]> {
    if (this.active && isCurrentChannel) return this.active.history();
    return (await createAttachmentStorage()).listChannel(channelId);
  }

  async send(file: File): Promise<void> {
    if (!this.active) throw new Error("Conecte e autentique o chat P2P antes de enviar arquivos.");
    await this.active.sendFile(file);
  }

  async request(record: StoredAttachmentRecord): Promise<void> {
    if (!this.active) throw new Error("Conecte ao peer para solicitar este arquivo.");
    await this.active.requestDownload(record);
  }

  async download(record: StoredAttachmentRecord): Promise<void> {
    if (this.active) {
      await this.active.download(record);
      return;
    }
    const locallyAvailable = record.direction === "outgoing"
      ? record.sourcePersisted === true
      : record.state === "completed";
    if (!locallyAvailable) throw new Error("Conecte ao peer para baixar este arquivo.");
    const blob = await (await createAttachmentStorage()).getBlob(record.attachmentId, record.manifest);
    triggerDownload(blob, record.manifest.filename);
  }

  async blob(record: StoredAttachmentRecord): Promise<Blob> {
    if (this.active) return this.active.getBlob(record);
    return (await createAttachmentStorage()).getBlob(record.attachmentId, record.manifest);
  }

  async pause(record: StoredAttachmentRecord): Promise<void> { await this.active?.pause(record); }
  async resume(record: StoredAttachmentRecord): Promise<void> { await this.active?.resume(record); }
  async cancel(record: StoredAttachmentRecord): Promise<void> { await this.active?.cancel(record); }
  async handleControl(remotePeerId: string, raw: string): Promise<boolean> {
    return this.active ? this.active.handleControlString(remotePeerId, raw) : false;
  }
  async handleTransfer(remotePeerId: string, data: ArrayBuffer): Promise<void> {
    await this.active?.handleTransferFrame(remotePeerId, data);
  }
  async peerReady(remotePeerId: string, expectedGeneration = this.generation): Promise<void> {
    if (expectedGeneration !== this.generation) return;
    await this.active?.peerReady(remotePeerId);
  }
  forgetPeer(remotePeerId: string): void { this.active?.forgetPeer(remotePeerId); }

  onAttachment(callback: (record: StoredAttachmentRecord) => void): () => void {
    this.attachmentCallbacks.add(callback);
    return () => this.attachmentCallbacks.delete(callback);
  }

  onProgress(callback: (progress: AttachmentRuntimeState) => void): () => void {
    this.progressCallbacks.add(callback);
    return () => this.progressCallbacks.delete(callback);
  }
}

function triggerDownload(blob: Blob, filename: string): void {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.rel = "noopener";
  anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 30_000);
}

export type { AttachmentRuntimeState, StoredAttachmentRecord };
