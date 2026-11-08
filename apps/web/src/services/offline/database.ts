const DATABASE = "risk-offline";
const VERSION = 5;

export const OFFLINE_STORES = {
  messages: "chat-messages",
  identity: "identity",
  friends: "friends",
  groups: "groups",
  attachments: "attachments",
  attachmentChunks: "attachment-chunks",
  syncCheckpoints: "sync-checkpoints",
  outbox: "chat-outbox",
  chatEvents: "chat-events",
} as const;

export function openBriksDatabase(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DATABASE, VERSION);
    request.onupgradeneeded = () => {
      const database = request.result;
      const messages = database.objectStoreNames.contains(OFFLINE_STORES.messages)
        ? request.transaction!.objectStore(OFFLINE_STORES.messages)
        : database.createObjectStore(OFFLINE_STORES.messages, { keyPath: "id" });
      if (!messages.indexNames.contains("channelId")) messages.createIndex("channelId", "channelId", { unique: false });
      if (!database.objectStoreNames.contains(OFFLINE_STORES.identity)) database.createObjectStore(OFFLINE_STORES.identity, { keyPath: "id" });
      if (!database.objectStoreNames.contains(OFFLINE_STORES.friends)) database.createObjectStore(OFFLINE_STORES.friends, { keyPath: "peerId" });
      if (!database.objectStoreNames.contains(OFFLINE_STORES.groups)) database.createObjectStore(OFFLINE_STORES.groups, { keyPath: "groupId" });

      const attachments = database.objectStoreNames.contains(OFFLINE_STORES.attachments)
        ? request.transaction!.objectStore(OFFLINE_STORES.attachments)
        : database.createObjectStore(OFFLINE_STORES.attachments, { keyPath: "recordId" });
      if (!attachments.indexNames.contains("channelId")) attachments.createIndex("channelId", "channelId", { unique: false });
      if (!attachments.indexNames.contains("attachmentId")) attachments.createIndex("attachmentId", "attachmentId", { unique: false });
      if (!attachments.indexNames.contains("transferId")) attachments.createIndex("transferId", "transferId", { unique: false });

      const chunks = database.objectStoreNames.contains(OFFLINE_STORES.attachmentChunks)
        ? request.transaction!.objectStore(OFFLINE_STORES.attachmentChunks)
        : database.createObjectStore(OFFLINE_STORES.attachmentChunks, { keyPath: "id" });
      if (!chunks.indexNames.contains("attachmentId")) chunks.createIndex("attachmentId", "attachmentId", { unique: false });

      const checkpoints = database.objectStoreNames.contains(OFFLINE_STORES.syncCheckpoints)
        ? request.transaction!.objectStore(OFFLINE_STORES.syncCheckpoints)
        : database.createObjectStore(OFFLINE_STORES.syncCheckpoints, { keyPath: "id" });
      if (!checkpoints.indexNames.contains("channelId")) checkpoints.createIndex("channelId", "channelId", { unique: false });
      if (!checkpoints.indexNames.contains("remotePeerId")) checkpoints.createIndex("remotePeerId", "remotePeerId", { unique: false });

      const outbox = database.objectStoreNames.contains(OFFLINE_STORES.outbox)
        ? request.transaction!.objectStore(OFFLINE_STORES.outbox)
        : database.createObjectStore(OFFLINE_STORES.outbox, { keyPath: "key" });
      if (!outbox.indexNames.contains("channelId")) outbox.createIndex("channelId", "channelId", { unique: false });

      const chatEvents = database.objectStoreNames.contains(OFFLINE_STORES.chatEvents)
        ? request.transaction!.objectStore(OFFLINE_STORES.chatEvents)
        : database.createObjectStore(OFFLINE_STORES.chatEvents, { keyPath: "id" });
      if (!chatEvents.indexNames.contains("channelId")) chatEvents.createIndex("channelId", "channelId", { unique: false });
      if (!chatEvents.indexNames.contains("targetMessageId")) chatEvents.createIndex("targetMessageId", "targetMessageId", { unique: false });
    };
    request.onsuccess = () => {
      const database = request.result;
      database.onversionchange = () => database.close();
      resolve(database);
    };
    request.onblocked = () => reject(new Error("Atualização do armazenamento local bloqueada por outra janela do Briks."));
    request.onerror = () => reject(request.error ?? new Error("IndexedDB indisponível."));
  });
}

export function getAllFromStore<T>(storeName: string): Promise<T[]> {
  return withStore<T[]>(storeName, "readonly", (store) => store.getAll());
}

export function getFromStore<T>(storeName: string, key: IDBValidKey): Promise<T | undefined> {
  return withStore<T | undefined>(storeName, "readonly", (store) => store.get(key));
}

export function getAllByIndex<T>(storeName: string, indexName: string, key: IDBValidKey): Promise<T[]> {
  return withStore<T[]>(storeName, "readonly", (store) => store.index(indexName).getAll(key));
}

export async function getAllPrimaryKeysByIndex(storeName: string, indexName: string, key: IDBValidKey): Promise<IDBValidKey[]> {
  const database = await openBriksDatabase();
  try {
    const transaction = database.transaction(storeName, "readonly");
    const done = transactionDone(transaction);
    const keys = await new Promise<IDBValidKey[]>((resolve, reject) => {
      const result: IDBValidKey[] = [];
      const request = transaction.objectStore(storeName).index(indexName).openKeyCursor(IDBKeyRange.only(key));
      request.onerror = () => reject(request.error ?? new Error("Falha ao listar chaves do armazenamento local."));
      request.onsuccess = () => {
        const cursor = request.result;
        if (!cursor) { resolve(result); return; }
        result.push(cursor.primaryKey);
        cursor.continue();
      };
    });
    await done;
    return keys;
  } finally { database.close(); }
}

export function putInStore<T>(storeName: string, value: T): Promise<void> {
  return withStore<void>(storeName, "readwrite", (store) => store.put(value));
}

export function deleteFromStore(storeName: string, key: IDBValidKey): Promise<void> {
  return withStore<void>(storeName, "readwrite", (store) => store.delete(key));
}

export async function deleteAllByIndex(storeName: string, indexName: string, key: IDBValidKey): Promise<void> {
  const database = await openBriksDatabase();
  try {
    const transaction = database.transaction(storeName, "readwrite");
    const done = transactionDone(transaction);
    const store = transaction.objectStore(storeName);
    const index = store.index(indexName);
    await new Promise<void>((resolve, reject) => {
      const request = index.openKeyCursor(IDBKeyRange.only(key));
      request.onerror = () => reject(request.error ?? new Error("Falha ao limpar armazenamento local."));
      request.onsuccess = () => {
        const cursor = request.result;
        if (!cursor) { resolve(); return; }
        store.delete(cursor.primaryKey);
        cursor.continue();
      };
    });
    await done;
  } finally { database.close(); }
}

function asyncRequest<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("Falha no armazenamento local."));
  });
}

function transactionDone(transaction: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    transaction.oncomplete = () => resolve();
    transaction.onabort = () => reject(transaction.error ?? new Error("Transação IndexedDB cancelada."));
    transaction.onerror = () => reject(transaction.error ?? new Error("Falha na transação IndexedDB."));
  });
}

async function withStore<T>(storeName: string, mode: IDBTransactionMode, operation: (store: IDBObjectStore) => IDBRequest): Promise<T> {
  const database = await openBriksDatabase();
  try {
    const transaction = database.transaction(storeName, mode);
    const done = transactionDone(transaction);
    const result = await asyncRequest(operation(transaction.objectStore(storeName))) as T;
    await done;
    return result;
  } finally { database.close(); }

}

export function clearStore(storeName: string): Promise<void> {
  return withStore<void>(storeName, "readwrite", (store) => store.clear());
}
