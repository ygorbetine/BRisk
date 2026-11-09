import { useCallback, useEffect, useRef, useState, type CSSProperties, type PointerEvent as ReactPointerEvent, type ReactNode } from "react";
import {
  EMPTY_OVERLAY_STATE,
  OVERLAY_WIDGETS,
  loadOverlaySettings,
  saveOverlaySettings,
  watchOverlaySettings,
  type OverlayPerson,
  type OverlaySettings,
  type OverlayState,
  type OverlayWidgetId,
} from "../services/overlay/settings";
import { dragPosition, nextLiveId, steppedScale, visibleWidgets } from "./overlay-model";
import "./overlay.css";

const DEMO_STATE: OverlayState = {
  active: true,
  roomName: "Geral",
  groupName: "UFFCraft",
  micMuted: true,
  liveId: "ana",
  people: [
    { id: "self", name: "Você", speaking: false, muted: true, live: false },
    { id: "ana", name: "Ana", speaking: true, muted: false, live: true },
    { id: "beto", name: "Beto", speaking: false, muted: false, live: false },
  ],
};

function asState(value: unknown): OverlayState {
  if (!value || typeof value !== "object") return EMPTY_OVERLAY_STATE;
  const raw = value as Partial<OverlayState>;
  const people = Array.isArray(raw.people)
    ? raw.people.filter((person): person is OverlayPerson => Boolean(person) && typeof (person as OverlayPerson).id === "string").slice(0, 32)
    : [];
  return {
    active: raw.active === true,
    roomName: typeof raw.roomName === "string" ? raw.roomName.slice(0, 80) : "",
    groupName: typeof raw.groupName === "string" ? raw.groupName.slice(0, 80) : "",
    micMuted: raw.micMuted === true,
    people,
    liveId: typeof raw.liveId === "string" ? raw.liveId : null,
  };
}

export function OverlayApp({ demo = false }: { demo?: boolean }) {
  const overlay = window.desktop?.overlay;
  const [settings, setSettings] = useState<OverlaySettings>(loadOverlaySettings);
  const [state, setState] = useState<OverlayState>(demo ? DEMO_STATE : EMPTY_OVERLAY_STATE);
  const [editing, setEditing] = useState(demo);
  const [frameUrl, setFrameUrl] = useState<string | null>(null);
  const frameUrlRef = useRef<string | null>(null);

  useEffect(() => watchOverlaySettings(setSettings), []);

  useEffect(() => {
    if (!overlay) return undefined;
    const offState = overlay.onState((value) => setState(asState(value)));
    const offEditing = overlay.onEditing(setEditing);
    const offFrame = overlay.onFrame((buffer) => {
      const url = URL.createObjectURL(new Blob([buffer], { type: "image/jpeg" }));
      const previous = frameUrlRef.current;
      frameUrlRef.current = url;
      setFrameUrl(url);
      if (previous) URL.revokeObjectURL(previous);
    });
    return () => { offState(); offEditing(); offFrame(); };
  }, [overlay]);

  // Trocou de live: descarta o último quadro para não mostrar a imagem de outra pessoa.
  useEffect(() => {
    const previous = frameUrlRef.current;
    frameUrlRef.current = null;
    setFrameUrl(null);
    if (previous) URL.revokeObjectURL(previous);
  }, [state.liveId]);

  useEffect(() => {
    document.documentElement.classList.add("overlay-mode");
    if (demo) document.documentElement.classList.add("overlay-demo");
    return () => { document.documentElement.classList.remove("overlay-mode", "overlay-demo"); };
  }, [demo]);

  const finishEditing = useCallback(() => {
    if (overlay) void overlay.setEditing(false); else setEditing(false);
  }, [overlay]);

  useEffect(() => {
    if (!editing) return undefined;
    const onKey = (event: KeyboardEvent) => { if (event.key === "Escape") finishEditing(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [editing, finishEditing]);

  const update = useCallback((next: OverlaySettings, persist: boolean) => {
    if (persist) setSettings(saveOverlaySettings(next)); else setSettings(next);
  }, []);

  const patchWidget = (id: OverlayWidgetId, patch: Partial<OverlaySettings["widgets"][OverlayWidgetId]>, persist: boolean) => {
    update({ ...settings, widgets: { ...settings.widgets, [id]: { ...settings.widgets[id], ...patch } } }, persist);
  };

  const shown = visibleWidgets(settings, state, editing);
  const livePerson = state.people.find((person) => person.id === state.liveId);
  const nextLive = nextLiveId(state.people.filter((person) => person.id !== "self"), state.liveId);

  const content: Record<OverlayWidgetId, ReactNode> = {
    live: <div className="ov-live">
      {frameUrl ? <img src={frameUrl} alt="" draggable={false}/> : <span className="ov-live-empty">{livePerson ? "Carregando transmissão…" : "Transmissão"}</span>}
      <span className="ov-badge"><i/>Ao vivo</span>
      <span className="ov-live-name">{livePerson?.name ?? "Sem transmissão"}</span>
    </div>,
    voice: <ul className="ov-voice">
      {(state.people.length || !editing ? state.people : DEMO_STATE.people).map((person) => <li key={person.id} className={person.speaking ? "speaking" : ""}>
        <span className="ov-avatar">{(person.name[0] ?? "?").toUpperCase()}</span>
        <span className="ov-name">{person.name}</span>
        {person.live && <span className="ov-badge small"><i/>Ao vivo</span>}
        {person.muted && <span className="ov-muted">mudo</span>}
      </li>)}
    </ul>,
    status: <div className="ov-status"><i/>{state.roomName || "Sala de voz"} · {Math.max(1, state.people.length)} {state.people.length === 1 ? "pessoa" : "pessoas"}</div>,
    mute: <div className="ov-mute">Microfone desligado</div>,
  };

  if (!demo && !state.active && !editing) return null;

  return <div className={`ov-root${editing ? " editing" : ""}`}>
    {shown.map((id) => <OverlayWidget
      key={id}
      id={id}
      label={OVERLAY_WIDGETS.find((widget) => widget.id === id)?.label ?? id}
      settings={settings}
      editing={editing}
      onPatch={patchWidget}
    >{content[id]}</OverlayWidget>)}

    {editing && <div className="ov-toolbar" role="toolbar" aria-label="Editar sobreposição">
      <p className="ov-hint">Arraste para mover · roda do mouse ou − e + mudam o tamanho · Esc ou Ctrl+Shift+O fixa</p>
      <div className="ov-chips">
        {OVERLAY_WIDGETS.map((widget) => <button
          key={widget.id}
          type="button"
          className={settings.widgets[widget.id].on ? "on" : ""}
          aria-pressed={settings.widgets[widget.id].on}
          onClick={() => patchWidget(widget.id, { on: !settings.widgets[widget.id].on }, true)}
        >{widget.label}</button>)}
      </div>
      <div className="ov-controls">
        <label>Opacidade
          <input type="range" className="sx-range" style={{ "--fill": `${Math.round(settings.opacity * 100)}%`, width: 200 } as CSSProperties} min={0} max={100} step={1} value={Math.round(settings.opacity * 100)} aria-label="Opacidade" onChange={(event) => update({ ...settings, opacity: Number(event.target.value) / 100 }, true)}/>
        </label>
        {nextLive && <button type="button" onClick={() => { if (overlay) overlay.sendCommand({ type: "select-live", peerId: nextLive }); else setState({ ...state, liveId: nextLive }); }}>Trocar transmissão</button>}
        <button type="button" className="primary" onClick={finishEditing}>Concluir</button>
      </div>
    </div>}
  </div>;
}

function OverlayWidget({ id, label, settings, editing, onPatch, children }: {
  id: OverlayWidgetId;
  label: string;
  settings: OverlaySettings;
  editing: boolean;
  onPatch(id: OverlayWidgetId, patch: Partial<OverlaySettings["widgets"][OverlayWidgetId]>, persist: boolean): void;
  children: ReactNode;
}) {
  const widget = settings.widgets[id];
  const drag = useRef<{ startX: number; startY: number; x: number; y: number } | null>(null);

  const down = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!editing || (event.target as HTMLElement).closest("button")) return;
    event.currentTarget.setPointerCapture(event.pointerId);
    drag.current = { startX: event.clientX, startY: event.clientY, x: widget.x, y: widget.y };
  };
  const move = (event: ReactPointerEvent<HTMLDivElement>) => {
    const start = drag.current;
    if (!start) return;
    onPatch(id, {
      x: dragPosition(start.x, event.clientX - start.startX, window.innerWidth),
      y: dragPosition(start.y, event.clientY - start.startY, window.innerHeight),
    }, false);
  };
  const up = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!drag.current) return;
    drag.current = null;
    event.currentTarget.releasePointerCapture(event.pointerId);
    onPatch(id, {}, true);
  };

  const style = {
    left: `${widget.x}%`,
    top: `${widget.y}%`,
    opacity: editing ? Math.max(settings.opacity, 0.4) : settings.opacity,
    transform: `scale(${widget.scale})`,
  } as CSSProperties;

  return <div
    className={`ov-widget ov-${id}${editing ? " editable" : ""}`}
    style={style}
    onPointerDown={down}
    onPointerMove={move}
    onPointerUp={up}
    onPointerCancel={up}
    onDragStart={(event) => event.preventDefault()}
    onWheel={(event) => { if (editing) onPatch(id, { scale: steppedScale(widget.scale, event.deltaY < 0 ? 1 : -1) }, true); }}
  >
    {editing && <div className="ov-handle">
      <span>{label}</span>
      <button type="button" aria-label={`Diminuir ${label}`} onClick={() => onPatch(id, { scale: steppedScale(widget.scale, -1) }, true)}>−</button>
      <span className="ov-scale">{Math.round(widget.scale * 100)}%</span>
      <button type="button" aria-label={`Aumentar ${label}`} onClick={() => onPatch(id, { scale: steppedScale(widget.scale, 1) }, true)}>+</button>
      <button type="button" aria-label={`Ocultar ${label}`} onClick={() => onPatch(id, { on: false }, true)}>×</button>
    </div>}
    {children}
  </div>;
}
