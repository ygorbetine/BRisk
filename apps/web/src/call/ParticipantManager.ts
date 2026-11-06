import type { PeerState } from "@briks/protocol";
import { useCallStore, type Participant } from "../store";
import { validAvatarDataUrl } from "../services/offline/profile";

export type CallProfileMessage = {
  version: 1;
  type: "call.profile";
  payload: { displayName: string; avatar?: string };
};

export function parseCallProfileMessage(value: string): CallProfileMessage | null {
  if (new TextEncoder().encode(value).byteLength > 64 * 1024) return null;
  let parsed: unknown;
  try { parsed = JSON.parse(value); } catch { return null; }
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return null;
  const envelope = parsed as Record<string, unknown>;
  if (envelope.version !== 1 || envelope.type !== "call.profile" || !envelope.payload || typeof envelope.payload !== "object" || Array.isArray(envelope.payload)) return null;
  const payload = envelope.payload as Record<string, unknown>;
  const displayName = typeof payload.displayName === "string" ? payload.displayName.trim() : "";
  if (displayName.length < 2 || displayName.length > 80) return null;
  if (payload.avatar !== undefined && !validAvatarDataUrl(payload.avatar)) return null;
  return { version: 1, type: "call.profile", payload: { displayName, avatar: payload.avatar as string | undefined } };
}

export function placeholderParticipant(peerId: string): Participant {
  return {
    peerId,
    displayName: `Peer ${peerId.slice(0, 6)}`,
    state: { microphone: false, camera: false, screenShare: false },
    streams: {},
    connection: "new",
  };
}

export function reconcileRemoteMediaState(
  streamsById: Participant["streams"],
  state: PeerState,
): PeerState {
  const streams = Object.values(streamsById ?? {});
  const videoStreams = streams.filter((stream) => stream.getVideoTracks().length > 0 && stream.id !== state.secondScreenStreamId);
  if (!videoStreams.length) return state;

  const next = { ...state };
  const exactCamera = next.cameraStreamId ? streamsById?.[next.cameraStreamId] : undefined;
  const exactScreen = next.screenStreamId ? streamsById?.[next.screenStreamId] : undefined;

  // O msid/MediaStream.id pode mudar entre Firefox e Chromium. Quando não há
  // correspondência exata, o stream principal é tratado como câmera e o último
  // stream de vídeo publicado como compartilhamento de tela.
  if (next.screenShare && !exactScreen) {
    let candidate: MediaStream | undefined;
    if (exactCamera) candidate = [...videoStreams].reverse().find((stream) => stream.id !== exactCamera.id);
    else if (!next.camera || videoStreams.length >= 2) candidate = videoStreams.at(-1);
    if (candidate) next.screenStreamId = candidate.id;
  }

  const normalizedScreen = next.screenStreamId ? streamsById?.[next.screenStreamId] : undefined;
  if (next.camera && !exactCamera) {
    let candidate: MediaStream | undefined;
    if (normalizedScreen) candidate = videoStreams.find((stream) => stream.id !== normalizedScreen.id);
    else if (!next.screenShare || videoStreams.length >= 2) candidate = videoStreams[0];
    if (candidate) next.cameraStreamId = candidate.id;
  }
  return next;
}

export class ParticipantManager {
  remoteStream(peerId: string, stream: MediaStream): void {
    const store = useCallStore.getState();
    const participant = store.participants[peerId] ?? placeholderParticipant(peerId);
    const streams = { ...participant.streams, [stream.id]: stream };
    store.upsert({ ...participant, streams, state: reconcileRemoteMediaState(streams, participant.state) });
  }

  connection(peerId: string, connection: RTCPeerConnectionState): void {
    const store = useCallStore.getState();
    const participant = store.participants[peerId] ?? placeholderParticipant(peerId);
    store.upsert({ ...participant, connection });
  }

  ensure(peerId: string): void {
    const store = useCallStore.getState();
    const participant = store.participants[peerId] ?? placeholderParticipant(peerId);
    store.upsert({ ...participant, connection: participant.connection ?? "new" });
  }

  profile(peerId: string, displayName: string, avatar?: string): void {
    const store = useCallStore.getState();
    const participant = store.participants[peerId] ?? placeholderParticipant(peerId);
    store.upsert({ ...participant, displayName, avatar });
  }

  state(peerId: string, remoteState: PeerState): void {
    const store = useCallStore.getState();
    const participant = store.participants[peerId] ?? placeholderParticipant(peerId);
    store.upsert({ ...participant, state: reconcileRemoteMediaState(participant.streams, remoteState) });
  }

  reconnecting(peerId: string, clearStreams = false): void {
    const store = useCallStore.getState();
    const participant = store.participants[peerId] ?? placeholderParticipant(peerId);
    const streams = clearStreams ? {} : participant.streams;
    if (clearStreams) Object.values(participant.streams ?? {}).forEach((stream) => stream.getTracks().forEach((track) => { track.enabled = false; }));
    store.upsert({ ...participant, streams, connection: "connecting" });
  }

  remove(peerId: string): void {
    useCallStore.getState().remove(peerId);
  }

  clear(): void {
    useCallStore.getState().clearParticipants();
  }
}
