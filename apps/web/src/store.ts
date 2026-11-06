import { create } from "zustand";
import type { PeerState } from "@briks/protocol";

export type Participant = { peerId: string; displayName: string; avatar?: string; state: PeerState; streams?: Record<string, MediaStream>; connection?: RTCPeerConnectionState };
export type CallContext = {
  groupId: string;
  groupName: string;
  voiceChannelId: string | null;
  voiceChannelName: string | null;
  textChannelId: string | null;
  textChannelName: string | null;
  displayName: string;
  avatar?: string;
};
export type LocalMediaPreviews = {
  microphone: MediaStream | null;
  camera: MediaStream | null;
  screen: MediaStream | null;
  screen2?: MediaStream | null;
};

type CallState = {
  token: string | null; roomId: string | null; callContext: CallContext | null; callWorkspaceOpen: boolean; selfPeerId: string | null; participants: Record<string, Participant>; localPreviews: LocalMediaPreviews; localState: PeerState; error: string | null;
  setSession(token: string): void; setRoom(roomId: string | null): void; setCallContext(context: CallContext | null): void; setCallWorkspaceOpen(open: boolean): void; setSelf(peerId: string | null): void;
  setLocalMedia(previews: LocalMediaPreviews, state: PeerState): void; upsert(participant: Participant): void; remove(peerId: string): void; clearParticipants(): void; setError(error: string | null): void; reset(): void;
};

function storedAccessToken(): string | null {
  try {
    return typeof sessionStorage === "undefined" ? null : sessionStorage.getItem("accessToken");
  } catch {
    return null;
  }
}

function persistAccessToken(token: string | null): void {
  try {
    if (typeof sessionStorage === "undefined") return;
    if (token) sessionStorage.setItem("accessToken", token);
    else sessionStorage.removeItem("accessToken");
  } catch {
    // O estado em memória continua funcional em testes, SSR e navegadores que
    // bloqueiam storage por política de privacidade.
  }
}

export const useCallStore = create<CallState>((set) => ({
  token: storedAccessToken(), roomId: null, callContext: null, callWorkspaceOpen: false, selfPeerId: null, participants: {}, localPreviews: { microphone: null, camera: null, screen: null }, localState: { microphone: false, camera: false, screenShare: false }, error: null,
  setSession: (token) => { persistAccessToken(token); set({ token, error: null }); },
  setRoom: (roomId) => set({ roomId, callWorkspaceOpen: Boolean(roomId) }), setCallContext: (callContext) => set({ callContext }), setCallWorkspaceOpen: (callWorkspaceOpen) => set({ callWorkspaceOpen }), setSelf: (selfPeerId) => set({ selfPeerId }),
  setLocalMedia: (localPreviews, localState) => set({ localPreviews, localState: { ...localState } }),
  upsert: (participant) => set((state) => ({ participants: { ...state.participants, [participant.peerId]: { ...state.participants[participant.peerId], ...participant } } })),
  remove: (peerId) => set((state) => { const participants = { ...state.participants }; delete participants[peerId]; return { participants }; }),
  clearParticipants: () => set({ participants: {}, selfPeerId: null }),
  setError: (error) => set({ error }),
  reset: () => {
    persistAccessToken(null);
    set({ token: null, roomId: null, callContext: null, callWorkspaceOpen: false, selfPeerId: null, participants: {}, localPreviews: { microphone: null, camera: null, screen: null }, localState: { microphone: false, camera: false, screenShare: false }, error: null });
  },
}));
