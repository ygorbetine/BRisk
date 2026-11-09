import {
  MAX_SCALE,
  MIN_SCALE,
  type OverlayPerson,
  type OverlaySettings,
  type OverlayState,
  type OverlayWidgetId,
} from "../services/overlay/settings";

export const MAX_DRAG_PERCENT = 96;

/** Nova posição (em % da tela) depois de arrastar `deltaPx`, sem sair da tela. */
export function dragPosition(startPercent: number, deltaPx: number, screenPx: number): number {
  if (!Number.isFinite(screenPx) || screenPx <= 0) return startPercent;
  const next = startPercent + (deltaPx / screenPx) * 100;
  return Math.min(MAX_DRAG_PERCENT, Math.max(0, Math.round(next * 10) / 10));
}

/** Escala depois de um passo (+1 aumenta, -1 diminui), em saltos de 0,1. */
export function steppedScale(scale: number, direction: 1 | -1): number {
  const next = Math.round((scale + direction * 0.1) * 10) / 10;
  return Math.min(MAX_SCALE, Math.max(MIN_SCALE, next));
}

/** Quais widgets aparecem. Em edição todos os ligados aparecem, mesmo sem dados (para poder posicionar). */
export function visibleWidgets(settings: OverlaySettings, state: OverlayState, editing: boolean): OverlayWidgetId[] {
  const ids: OverlayWidgetId[] = ["live", "voice", "status", "mute"];
  return ids.filter((id) => {
    if (!settings.widgets[id].on) return false;
    if (editing) return true;
    if (id === "live") return Boolean(state.liveId);
    if (id === "voice") return state.people.length > 0;
    if (id === "status") return Boolean(state.roomName);
    return state.micMuted;
  });
}

/** Quem está transmitindo, na ordem recebida. */
export function livePeople(people: OverlayPerson[]): OverlayPerson[] {
  return people.filter((person) => person.live);
}

/** Escolhe a live exibida: a preferida, se ainda estiver ao vivo; senão a primeira. */
export function pickLiveId(people: OverlayPerson[], preferred: string | null): string | null {
  const lives = livePeople(people);
  if (preferred && lives.some((person) => person.id === preferred)) return preferred;
  return lives[0]?.id ?? null;
}

/** Próxima live ao trocar (circular). Devolve null se não há outra. */
export function nextLiveId(people: OverlayPerson[], current: string | null): string | null {
  const lives = livePeople(people);
  if (lives.length < 2) return null;
  const index = lives.findIndex((person) => person.id === current);
  return lives[(index + 1) % lives.length]?.id ?? null;
}

/** Escolhe o stream de tela de um participante: o id anunciado ou, na falta, o primeiro com vídeo que não é câmera. */
export function pickScreenStream<T extends { id: string; getVideoTracks(): unknown[] }>(
  streams: Record<string, T> | undefined,
  state: { screenStreamId?: string; cameraStreamId?: string },
): T | null {
  if (!streams) return null;
  const exact = state.screenStreamId ? streams[state.screenStreamId] : undefined;
  if (exact && exact.getVideoTracks().length > 0) return exact;
  for (const stream of Object.values(streams)) {
    if (stream.id === state.cameraStreamId) continue;
    if (stream.getVideoTracks().length > 0) return stream;
  }
  return null;
}

type ParticipantLike = { peerId: string; displayName: string; state: { microphone: boolean; screenShare: boolean } };

export function buildOverlayState(input: {
  active: boolean;
  roomName: string;
  groupName: string;
  selfName: string;
  micOn: boolean;
  selfLive: boolean;
  selfSpeaking: boolean;
  participants: ParticipantLike[];
  speaking: ReadonlySet<string>;
  preferredLive: string | null;
}): OverlayState {
  const people: OverlayPerson[] = [
    { id: "self", name: input.selfName || "Você", speaking: input.micOn && input.selfSpeaking, muted: !input.micOn, live: input.selfLive },
    ...input.participants.map((person) => ({
      id: person.peerId,
      name: person.displayName,
      speaking: person.state.microphone && input.speaking.has(person.peerId),
      muted: !person.state.microphone,
      live: person.state.screenShare,
    })),
  ];
  // A live exibida é sempre de outra pessoa: a própria tela você já vê no jogo.
  const watchable = people.filter((person) => person.id !== "self");
  return {
    active: input.active,
    roomName: input.roomName,
    groupName: input.groupName,
    micMuted: !input.micOn,
    people,
    liveId: pickLiveId(watchable, input.preferredLive),
  };
}

export function sameOverlayState(a: OverlayState, b: OverlayState): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}
