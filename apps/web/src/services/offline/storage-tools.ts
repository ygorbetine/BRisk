import type { StoredAttachmentRecord } from "../attachments/indexeddb-storage";
import { OFFLINE_STORES, clearStore, getAllFromStore } from "./database";
import { desktopConfig } from "./desktop-backend-client";

export type StorageBreakdown = { messages: number; groups: number; attachments: number };

/** Tamanho aproximado, em bytes, de um valor guardado como JSON. */
export function approximateBytes(value: unknown): number {
  try { return new TextEncoder().encode(JSON.stringify(value) ?? "").length; } catch { return 0; }
}

/** Fatias (0 a 100) para a barra por tipo; nada vira zero em todas. */
export function breakdownShares(breakdown: StorageBreakdown): StorageBreakdown {
  const total = breakdown.messages + breakdown.groups + breakdown.attachments;
  if (total <= 0) return { messages: 0, groups: 0, attachments: 0 };
  const pct = (value: number) => (value / total) * 100;
  return { messages: pct(breakdown.messages), groups: pct(breakdown.groups), attachments: pct(breakdown.attachments) };
}

function uniqueAttachments(records: StoredAttachmentRecord[]): Map<string, number> {
  const bySize = new Map<string, number>();
  for (const record of records) bySize.set(record.attachmentId, Math.max(bySize.get(record.attachmentId) ?? 0, record.totalBytes || record.manifest?.size || 0));
  return bySize;
}

export async function measureLocalData(): Promise<StorageBreakdown> {
  if (typeof indexedDB === "undefined") return { messages: 0, groups: 0, attachments: 0 };
  const [messages, groups, attachments] = await Promise.all([
    getAllFromStore(OFFLINE_STORES.messages),
    getAllFromStore(OFFLINE_STORES.groups),
    getAllFromStore<StoredAttachmentRecord>(OFFLINE_STORES.attachments),
  ]);
  let attachmentBytes = 0;
  for (const size of uniqueAttachments(attachments).values()) attachmentBytes += size;
  return { messages: approximateBytes(messages), groups: approximateBytes(groups), attachments: attachmentBytes };
}

/** Apaga os anexos guardados neste computador (cópias locais); as mensagens continuam. */
export async function clearLocalAttachments(): Promise<void> {
  const records = await getAllFromStore<StoredAttachmentRecord>(OFFLINE_STORES.attachments);
  const ids = [...uniqueAttachments(records).keys()];
  await clearStore(OFFLINE_STORES.attachmentChunks);
  await clearStore(OFFLINE_STORES.attachments);
  const config = await desktopConfig().catch(() => null);
  if (!config) return;
  for (const attachmentId of ids) {
    const headers = new Headers();
    if (config.token) headers.set("x-briks-desktop-token", config.token);
    await fetch(`${config.baseUrl}/p2p/attachments/content/${encodeURIComponent(attachmentId)}/discard`, { method: "POST", headers }).catch(() => undefined);
  }
}

/** Apaga o histórico de mensagens deste computador. Grupos, amigos e identidade ficam. */
export async function clearLocalMessages(): Promise<void> {
  await clearStore(OFFLINE_STORES.messages);
  await clearStore(OFFLINE_STORES.chatEvents);
  await clearStore(OFFLINE_STORES.outbox);
}

export type LocalDataExport = {
  app: "Briks";
  exportedAt: string;
  messages: unknown[];
  groups: unknown[];
  friends: unknown[];
  attachments: Array<{ attachmentId: string; name?: string; size?: number; channelId: string; state: string }>;
};

/** Monta a cópia dos dados. Não inclui chave privada, identidade nem o conteúdo dos anexos. */
export async function buildLocalDataExport(now = new Date()): Promise<LocalDataExport> {
  const [messages, groups, friends, attachments] = await Promise.all([
    getAllFromStore(OFFLINE_STORES.messages),
    getAllFromStore(OFFLINE_STORES.groups),
    getAllFromStore(OFFLINE_STORES.friends),
    getAllFromStore<StoredAttachmentRecord>(OFFLINE_STORES.attachments),
  ]);
  return {
    app: "Briks",
    exportedAt: now.toISOString(),
    messages,
    groups,
    friends,
    attachments: [...new Map(attachments.map((record) => [record.attachmentId, record])).values()].map((record) => ({
      attachmentId: record.attachmentId,
      name: (record.manifest as { name?: string } | undefined)?.name,
      size: record.totalBytes,
      channelId: record.channelId,
      state: record.state,
    })),
  };
}

export function exportFileName(now = new Date()): string {
  return `briks-dados-${now.toISOString().slice(0, 10)}.json`;
}

export async function downloadLocalDataExport(): Promise<void> {
  const payload = await buildLocalDataExport();
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = exportFileName();
  document.body.appendChild(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

