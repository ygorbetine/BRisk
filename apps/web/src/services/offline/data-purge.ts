import type { StoredAttachmentRecord } from "../attachments/indexeddb-storage";
import {
  OFFLINE_STORES,
  deleteAllByIndex,
  getAllByIndex,
} from "./database";
import { backendRequest, desktopConfig } from "./desktop-backend-client";

/** Remove todos os dados locais cujo ciclo de vida pertence a um canal. */
export async function purgeLocalChannelData(channelId: string): Promise<void> {
  const attachments = typeof indexedDB === "undefined"
    ? []
    : await getAllByIndex<StoredAttachmentRecord>(OFFLINE_STORES.attachments, "channelId", channelId);

  const orphanedAttachmentIds = new Set<string>();
  if (typeof indexedDB !== "undefined") {
    await Promise.all([
      deleteAllByIndex(OFFLINE_STORES.messages, "channelId", channelId),
      deleteAllByIndex(OFFLINE_STORES.chatEvents, "channelId", channelId),
      deleteAllByIndex(OFFLINE_STORES.outbox, "channelId", channelId),
      deleteAllByIndex(OFFLINE_STORES.syncCheckpoints, "channelId", channelId),
      deleteAllByIndex(OFFLINE_STORES.attachments, "channelId", channelId),
    ]);
    for (const attachmentId of new Set(attachments.map((record) => record.attachmentId))) {
      const remaining = await getAllByIndex<StoredAttachmentRecord>(OFFLINE_STORES.attachments, "attachmentId", attachmentId);
      if (remaining.length === 0) {
        orphanedAttachmentIds.add(attachmentId);
        await deleteAllByIndex(OFFLINE_STORES.attachmentChunks, "attachmentId", attachmentId);
      }
    }
  }

  const config = await desktopConfig();
  if (!config) return;
  await backendRequest(config, "channels", channelId, { method: "POST" });
  for (const attachmentId of orphanedAttachmentIds) {
    const headers = new Headers();
    if (config.token) headers.set("x-briks-desktop-token", config.token);
    const response = await fetch(`${config.baseUrl}/p2p/attachments/content/${encodeURIComponent(attachmentId)}/discard`, {
      method: "POST",
      headers,
    });
    if (!response.ok) throw new Error(`Não foi possível remover o conteúdo órfão ${attachmentId.slice(0, 12)} (HTTP ${response.status}).`);
  }
}
