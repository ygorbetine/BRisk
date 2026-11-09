import { useSyncExternalStore } from "react";
import type { CustomStatus } from "./status";

/** Ponte entre a presença (Home) e a chamada: guarda o status dos amigos por peerId. */
let current: ReadonlyMap<string, CustomStatus> = new Map();
const listeners = new Set<() => void>();

export function setPeerStatuses(next: ReadonlyMap<string, CustomStatus>): void {
  current = next;
  for (const listener of listeners) listener();
}

function subscribe(listener: () => void): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function usePeerStatus(peerId: string | undefined): CustomStatus | undefined {
  return useSyncExternalStore(subscribe, () => (peerId ? current.get(peerId) : undefined));
}
