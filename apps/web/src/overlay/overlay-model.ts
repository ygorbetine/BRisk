import {
  MAX_SCALE,
  MIN_SCALE,
  type OverlayPerson,
  type OverlaySettings,
  type OverlayState,
  type OverlayWidget,
  type OverlayWidgetId,
  MAX_OVERLAY_LIVES,
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
  const ids: OverlayWidgetId[] = ["voice", "status", "mute"];
  return ids.filter((id) => {
    if (!settings.widgets[id].on) return false;
    if (editing) return true;
    if (id === "voice") return state.people.length > 0;
    if (id === "status") return state.active || Boolean(state.roomName);
    return state.micMuted;
  });
}

/** Quem está transmitindo, na ordem recebida. */
export function livePeople(people: OverlayPerson[]): OverlayPerson[] {
  return people.filter((person) => person.live);
}

/** Lives exibidas: as que a pessoa não desligou, até o limite. Tudo desligado se o item "Transmissões" estiver desligado. */
export function shownLiveIds(settings: OverlaySettings, liveIds: string[]): string[] {
  if (!settings.widgets.live.on) return [];
  return liveIds.filter((id) => settings.lives[id]?.on !== false).slice(0, MAX_OVERLAY_LIVES);
}

const LIVE_STACK_STEP = 20;

/** Posição e tamanho de uma miniatura: os dela, ou a posição base escalonada para não ficarem uma sobre a outra. */
export function liveTile(settings: OverlaySettings, peerId: string, index: number): OverlayWidget {
  const saved = settings.lives[peerId];
  if (saved) return saved;
  const base = settings.widgets.live;
  return { on: true, x: base.x, y: Math.min(MAX_DRAG_PERCENT, base.y + index * LIVE_STACK_STEP), scale: base.scale };
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
  // As lives exibidas são sempre de outras pessoas: a própria tela você já vê no jogo.
  const watchable = people.filter((person) => person.id !== "self");
  return {
    active: input.active,
    roomName: input.roomName,
    groupName: input.groupName,
    micMuted: !input.micOn,
    people,
    liveIds: livePeople(watchable).map((person) => person.id),
  };
}

export function sameOverlayState(a: OverlayState, b: OverlayState): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}
