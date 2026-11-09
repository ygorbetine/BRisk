import { useCallback, useEffect, useRef, useState, type CSSProperties, type RefObject } from "react";
import { ZoomOut } from "lucide-react";
import { NO_ZOOM, clampZoom, panBy, wheelFactor, zoomAt, zoomLabel, type ZoomState } from "./zoom-model";

const CONTROLS = "button,input,.volume-panel,.source-switch";

/**
 * Zoom na transmissão: roda do mouse (ou pinça no trackpad) aproxima no ponto do cursor,
 * arrastar move a imagem. Funciona dentro e fora da tela cheia.
 */
export function useVideoZoom(articleRef: RefObject<HTMLElement | null>, enabled: boolean) {
  const [zoom, setZoom] = useState<ZoomState>(NO_ZOOM);
  const zoomRef = useRef(zoom);
  zoomRef.current = zoom;

  useEffect(() => {
    if (!enabled) { setZoom(NO_ZOOM); return undefined; }
    const element = articleRef.current;
    if (!element) return undefined;

    const onWheel = (event: WheelEvent) => {
      if ((event.target as HTMLElement).closest(CONTROLS)) return;
      event.preventDefault();
      const rect = element.getBoundingClientRect();
      const factor = wheelFactor(event.deltaY, event.ctrlKey);
      setZoom((current) => zoomAt(current, factor, event.clientX - rect.left - rect.width / 2, event.clientY - rect.top - rect.height / 2, rect.width, rect.height));
    };

    let drag: { id: number; x: number; y: number; moved: number } | null = null;
    let swallowClick = false;
    const onDown = (event: PointerEvent) => {
      if (event.button !== 0 || zoomRef.current.scale <= 1.001 || (event.target as HTMLElement).closest(CONTROLS)) return;
      drag = { id: event.pointerId, x: event.clientX, y: event.clientY, moved: 0 };
      element.setPointerCapture(event.pointerId);
    };
    const onMove = (event: PointerEvent) => {
      if (!drag || drag.id !== event.pointerId) return;
      const dx = event.clientX - drag.x;
      const dy = event.clientY - drag.y;
      drag.x = event.clientX;
      drag.y = event.clientY;
      drag.moved += Math.abs(dx) + Math.abs(dy);
      const rect = element.getBoundingClientRect();
      setZoom((current) => panBy(current, dx, dy, rect.width, rect.height));
    };
    const onUp = (event: PointerEvent) => {
      if (!drag || drag.id !== event.pointerId) return;
      swallowClick = drag.moved > 4;
      drag = null;
      if (element.hasPointerCapture(event.pointerId)) element.releasePointerCapture(event.pointerId);
    };
    // Soltar depois de arrastar não pode contar como clique (que muda o foco da transmissão).
    const onClickCapture = (event: MouseEvent) => {
      if (!swallowClick) return;
      swallowClick = false;
      event.stopPropagation();
      event.preventDefault();
    };
    const refit = () => requestAnimationFrame(() => {
      const rect = element.getBoundingClientRect();
      setZoom((current) => clampZoom(current, rect.width, rect.height));
    });

    element.addEventListener("wheel", onWheel, { passive: false });
    element.addEventListener("pointerdown", onDown);
    element.addEventListener("pointermove", onMove);
    element.addEventListener("pointerup", onUp);
    element.addEventListener("pointercancel", onUp);
    element.addEventListener("click", onClickCapture, true);
    document.addEventListener("fullscreenchange", refit);
    window.addEventListener("resize", refit);
    return () => {
      element.removeEventListener("wheel", onWheel);
      element.removeEventListener("pointerdown", onDown);
      element.removeEventListener("pointermove", onMove);
      element.removeEventListener("pointerup", onUp);
      element.removeEventListener("pointercancel", onUp);
      element.removeEventListener("click", onClickCapture, true);
      document.removeEventListener("fullscreenchange", refit);
      window.removeEventListener("resize", refit);
    };
  }, [articleRef, enabled]);

  const reset = useCallback(() => setZoom(NO_ZOOM), []);
  const zoomed = zoom.scale > 1.001;
  const videoStyle: CSSProperties | undefined = zoomed
    ? { transform: `translate(${zoom.x}px, ${zoom.y}px) scale(${zoom.scale})`, transformOrigin: "center", cursor: "grab" }
    : undefined;
  const badge = zoomed
    ? <button type="button" className="zoom-badge" title="Voltar ao tamanho normal" aria-label={`Zoom ${zoomLabel(zoom)}. Voltar ao tamanho normal`} onClick={(event) => { event.stopPropagation(); reset(); }} onDoubleClick={(event) => event.stopPropagation()}><ZoomOut size={14}/>{zoomLabel(zoom)}</button>
    : null;
  const hint = enabled && !zoomed ? <span className="zoom-hint" aria-hidden="true">Role o mouse para dar zoom</span> : null;
  return { videoStyle, badge: badge ?? hint, zoomed };
}
