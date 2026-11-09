import type { Participant } from "../store";

type LocalPreviews = { screen: MediaStream | null; screen2?: MediaStream | null };

export type LivePreviewSource = { id: string; stream: MediaStream };

/**
 * Escolhe qual tela aparece na miniatura "Ao vivo" da barra lateral: primeiro a tela
 * maximizada na chamada (se for uma tela), depois qualquer tela de outra pessoa e, por
 * último, a sua própria transmissão. Os ids seguem os das peças da chamada.
 */
export function pickLivePreview(input: {
  focusedTile: string | null;
  participants: Participant[];
  localPreviews: LocalPreviews;
  localScreenShare: boolean;
}): LivePreviewSource | null {
  const candidates: LivePreviewSource[] = [];
  for (const peer of input.participants) {
    const first = peer.state.screenShare && peer.state.screenStreamId ? peer.streams?.[peer.state.screenStreamId] : undefined;
    if (first && first.getVideoTracks().length > 0) candidates.push({ id: `peer-${peer.peerId}`, stream: first });
    const second = peer.state.secondScreenStreamId ? peer.streams?.[peer.state.secondScreenStreamId] : undefined;
    if (second && second.getVideoTracks().length > 0) candidates.push({ id: `peer2-${peer.peerId}`, stream: second });
  }
  if (input.localScreenShare && input.localPreviews.screen) candidates.push({ id: "local-screen", stream: input.localPreviews.screen });
  if (input.localPreviews.screen2) candidates.push({ id: "local-screen2", stream: input.localPreviews.screen2 });
  if (!candidates.length) return null;
  const focused = input.focusedTile ? candidates.find((item) => item.id === input.focusedTile) : undefined;
  if (focused) return focused;
  // Sem tela maximizada: prefere a de outra pessoa; a sua só quando não há outra.
  return candidates.find((item) => !item.id.startsWith("local-")) ?? candidates[0]!;
}
