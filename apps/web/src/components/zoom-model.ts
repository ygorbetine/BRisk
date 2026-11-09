/** Zoom e deslocamento de uma transmissão. A imagem usa `translate(x, y) scale(scale)` com origem no centro do quadro. */
export type ZoomState = { scale: number; x: number; y: number };

export const MIN_ZOOM = 1;
export const MAX_ZOOM = 8;
export const NO_ZOOM: ZoomState = { scale: 1, x: 0, y: 0 };

const clamp = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value));

/** Mantém a escala nos limites e a imagem sempre cobrindo o quadro (sem bordas vazias). */
export function clampZoom(state: ZoomState, width: number, height: number): ZoomState {
  const scale = clamp(state.scale, MIN_ZOOM, MAX_ZOOM);
  if (scale <= 1.001) return NO_ZOOM;
  const maxX = (width * (scale - 1)) / 2;
  const maxY = (height * (scale - 1)) / 2;
  return { scale, x: clamp(state.x, -maxX, maxX), y: clamp(state.y, -maxY, maxY) };
}

/** Zoom por um fator, mantendo fixo o ponto sob o cursor. `px`/`py` são relativos ao centro do quadro. */
export function zoomAt(state: ZoomState, factor: number, px: number, py: number, width: number, height: number): ZoomState {
  const scale = clamp(state.scale * factor, MIN_ZOOM, MAX_ZOOM);
  const ratio = scale / state.scale;
  return clampZoom({ scale, x: px - (px - state.x) * ratio, y: py - (py - state.y) * ratio }, width, height);
}

export function panBy(state: ZoomState, dx: number, dy: number, width: number, height: number): ZoomState {
  if (state.scale <= 1.001) return NO_ZOOM;
  return clampZoom({ ...state, x: state.x + dx, y: state.y + dy }, width, height);
}

/** Fator de zoom de um evento de roda. Pinça no trackpad chega como roda com Ctrl, com passos menores. */
export function wheelFactor(deltaY: number, pinch: boolean): number {
  return Math.exp(-deltaY * (pinch ? 0.01 : 0.0015));
}

export function zoomLabel(state: ZoomState): string {
  return `${Math.round(state.scale * 100)}%`;
}
