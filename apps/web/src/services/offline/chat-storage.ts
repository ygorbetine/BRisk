export type LocalChatMessage = {
  id: string;
  channelId: string;
  author: string;
  content: string;
  createdAt: string;
  authorPeerId?: string | null;
  signature?: string | null;
  replyToId?: string | null;
  editedContent?: string | null;
  editedAt?: string | null;
  deletedAt?: string | null;
  pinnedAt?: string | null;
  pinnedByPeerId?: string | null;
  reactions?: Record<string, string[]>;
};

import { OFFLINE_STORES, getFromStore, openBriksDatabase } from "./database";
import { backendRequest, desktopConfig, resetDesktopBackendClient } from "./desktop-backend-client";
const STORE = OFFLINE_STORES.messages;
const migratedChannels = new Set<string>();

export function resetChatStorageRuntime(): void {
  resetDesktopBackendClient();
}

export type MessagePageOptions = { before?: string; beforeId?: string; limit?: number };

export async function loadLocalMessages(channelId: string, options: MessagePageOptions = {}): Promise<LocalChatMessage[]> {
  const limit = Math.max(1, Math.min(200, Math.trunc(options.limit ?? 100)));
  const config = await desktopConfig();
  if (!config) return loadLegacyMessages(channelId, options.before, options.beforeId, limit);
  const query = `?limit=${limit}${options.before ? `&before=${encodeURIComponent(options.before)}` : ""}${options.beforeId ? `&beforeId=${encodeURIComponent(options.beforeId)}` : ""}`;
  const current = await backendRequest<LocalChatMessage[]>(config, "messages", channelId, { method: "GET" }, query);
  if (options.before) return current;
  if (!migratedChannels.has(channelId)) {
    migratedChannels.add(channelId);
    const legacy = await loadAllLegacyMessages(channelId);
    const missing = legacy.filter((message) => !current.some((item) => item.id === message.id));
    for (const message of missing) {
      // A migração pode reencontrar uma mensagem antiga que já existe fora da
      // primeira página do SQLite. INSERT OR IGNORE preserva sua projeção de
      // edição/exclusão/reação em vez de restaurar os campos legados vazios.
      await backendRequest(config, "messages", channelId, { method: "POST", body: JSON.stringify(message) }, "?insertOnly=true");
    }
    if (missing.length) return backendRequest<LocalChatMessage[]>(config, "messages", channelId, { method: "GET" }, query);
  }
  return current;
}

export async function saveLocalMessage(message: LocalChatMessage): Promise<void> {
  const config = await desktopConfig();
  if (config) {
    await backendRequest(config, "messages", message.channelId, { method: "POST", body: JSON.stringify(message) });
    return;
  }
  await saveLegacyMessage(message);
}

export async function loadLocalMessage(channelId: string, messageId: string): Promise<LocalChatMessage | undefined> {
  const config = await desktopConfig();
  if (config) {
    const messages = await backendRequest<LocalChatMessage[]>(
      config,
      "messages",
      channelId,
      { method: "GET" },
      `?messageId=${encodeURIComponent(messageId)}&limit=1`,
    );
    return messages[0];
  }
  const message = await getFromStore<LocalChatMessage>(STORE, messageId);
  return message?.channelId === channelId ? message : undefined;
}

async function loadLegacyMessages(channelId: string, before: string | undefined, beforeId: string | undefined, limit: number): Promise<LocalChatMessage[]> {
  const database = await openBriksDatabase();
  return new Promise((resolve, reject) => {
    const transaction = database.transaction(STORE, "readonly");
    const index = transaction.objectStore(STORE).index("channelId");
    const request = index.getAll(IDBKeyRange.only(channelId));
    request.onsuccess = () => resolve((request.result as LocalChatMessage[])
      .filter((message) => !before || message.createdAt < before || (message.createdAt === before && Boolean(beforeId) && message.id < beforeId!))
      .sort((a, b) => a.createdAt.localeCompare(b.createdAt) || a.id.localeCompare(b.id)).slice(-limit));
    request.onerror = () => reject(request.error ?? new Error("Falha ao ler o histórico local."));
    transaction.oncomplete = () => database.close();
  });
}

async function loadAllLegacyMessages(channelId: string): Promise<LocalChatMessage[]> {
  const database = await openBriksDatabase();
  return new Promise((resolve, reject) => {
    const transaction = database.transaction(STORE, "readonly");
    const request = transaction.objectStore(STORE).index("channelId").getAll(IDBKeyRange.only(channelId));
    request.onsuccess = () => resolve((request.result as LocalChatMessage[])
      .sort((left, right) => left.createdAt.localeCompare(right.createdAt)));
    request.onerror = () => reject(request.error ?? new Error("Falha ao migrar o histórico local completo."));
    transaction.oncomplete = () => database.close();
  });
}

async function saveLegacyMessage(message: LocalChatMessage): Promise<void> {
  const database = await openBriksDatabase();
  return new Promise((resolve, reject) => {
    const transaction = database.transaction(STORE, "readwrite");
    transaction.objectStore(STORE).put(message);
    transaction.oncomplete = () => { database.close(); resolve(); };
    transaction.onerror = () => { database.close(); reject(transaction.error ?? new Error("Falha ao salvar a mensagem local.")); };
  });
}
