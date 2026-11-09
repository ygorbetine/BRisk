import { useEffect, useRef } from "react";
import { friendEvents, loadNotifySettings, watchNotifySettings, type FriendSnapshot } from "./friend-notifications";

type Directory = {
  onOnlineChange(callback: (online: ReadonlySet<string>) => void): () => void;
  onStreamingChange(callback: (streaming: ReadonlySet<string>) => void): () => void;
  peerStreaming(): ReadonlySet<string>;
};

/** Mostra notificações do sistema quando um amigo fica online ou começa a transmitir. */
export function useFriendNotifications(directory: Directory, friends: ReadonlyArray<{ id: string; displayName: string }>, inCall: boolean): void {
  const state = useRef({ friends, inCall, settings: loadNotifySettings() });
  state.current.friends = friends;
  state.current.inCall = inCall;

  useEffect(() => watchNotifySettings((settings) => { state.current.settings = settings; }), []);

  useEffect(() => {
    const startedAt = Date.now();
    let snapshot: FriendSnapshot = { online: new Set(), streaming: new Set() };
    const update = (next: FriendSnapshot) => {
      const { friends: list, inCall: busy, settings } = state.current;
      const events = friendEvents(snapshot, next, { settings, friends: new Set(list.map((friend) => friend.id)), inCall: busy, sinceStartMs: Date.now() - startedAt });
      snapshot = next;
      if (!events.length || !("Notification" in window) || Notification.permission !== "granted") return;
      for (const event of events.slice(0, 3)) {
        const name = list.find((friend) => friend.id === event.peerId)?.displayName ?? "Um amigo";
        const notice = new Notification(event.kind === "streaming" ? `${name} começou a transmitir` : `${name} está online`, {
          body: event.kind === "streaming" ? "Entre na chamada para assistir." : "Agora dá para chamar.",
          tag: `friend-${event.kind}-${event.peerId}`,
          silent: false,
        });
        notice.onclick = () => { window.focus(); notice.close(); };
      }
    };
    // Lê os dois juntos: quem fica online já transmitindo gera um aviso só.
    const offOnline = directory.onOnlineChange((online) => update({ online, streaming: directory.peerStreaming() }));
    const offStreaming = directory.onStreamingChange((streaming) => update({ online: snapshot.online, streaming }));
    return () => { offOnline(); offStreaming(); };
  }, [directory]);
}
