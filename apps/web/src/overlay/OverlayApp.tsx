import { useCallback, useEffect, useRef, useState, type CSSProperties, type PointerEvent as ReactPointerEvent, type ReactNode } from "react";
import {
  EMPTY_OVERLAY_STATE,
  OVERLAY_WIDGETS,
  loadOverlaySettings,
  saveOverlaySettings,
  watchOverlaySettings,
  type OverlayPerson,
  type OverlayWidget as OverlayWidgetSettings,
  MAX_OVERLAY_LIVES,
  type OverlaySettings,
  type OverlayState,
  type OverlayWidgetId,
} from "../services/overlay/settings";
import { dragPosition, liveTile, steppedScale, shownLiveIds, visibleWidgets } from "./overlay-model";
import "./overlay.css";

const PLACEHOLDER_LIVE = "__placeholder__";

const DEMO_STATE: OverlayState = {
  active: true,
  roomName: "Geral",
  groupName: "UFFCraft",
  micMuted: true,
  liveIds: ["ana", "beto"],
  people: [
    { id: "self", name: "Você", speaking: false, muted: true, live: false },
    { id: "ana", name: "Ana", speaking: true, muted: false, live: true },
    { id: "beto", name: "Beto", speaking: false, muted: false, live: true },
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
    liveIds: Array.isArray(raw.liveIds) ? raw.liveIds.filter((id): id is string => typeof id === "string").slice(0, 32) : [],
  };
}

export function OverlayApp({ demo = false }: { demo?: boolean }) {
  const overlay = window.desktop?.overlay;
  const [settings, setSettings] = useState<OverlaySettings>(loadOverlaySettings);
  const [state, setState] = useState<OverlayState>(demo ? DEMO_STATE : EMPTY_OVERLAY_STATE);
  const [editing, setEditing] = useState(demo);
  const [frameUrls, setFrameUrls] = useState<Record<string, string>>({});
  const frameUrlsRef = useRef<Record<string, string>>({});

  useEffect(() => watchOverlaySettings(setSettings), []);

  useEffect(() => {
    if (!overlay) return undefined;
    const offState = overlay.onState((value) => setState(asState(value)));
    const offEditing = overlay.onEditing(setEditing);
    const offFrame = overlay.onFrame((buffer, peerId) => {
      const url = URL.createObjectURL(new Blob([buffer], { type: "image/jpeg" }));
      const previous = frameUrlsRef.current[peerId];
      frameUrlsRef.current = { ...frameUrlsRef.current, [peerId]: url };
      setFrameUrls(frameUrlsRef.current);
      if (previous) URL.revokeObjectURL(previous);
    });
    return () => { offState(); offEditing(); offFrame(); };
  }, [overlay]);

  // Quem deixou de ser exibido perde o último quadro, para não reaparecer com a imagem velha.
  const shownIds = shownLiveIds(settings, state.liveIds);
  const shownKey = shownIds.join("|");
  useEffect(() => {
    const keep = new Set(shownKey ? shownKey.split("|") : []);
    const next: Record<string, string> = {};
    for (const [peerId, url] of Object.entries(frameUrlsRef.current)) {
      if (keep.has(peerId)) next[peerId] = url; else URL.revokeObjectURL(url);
    }
    if (Object.keys(next).length === Object.keys(frameUrlsRef.current).length) return;
    frameUrlsRef.current = next;
    setFrameUrls(next);
  }, [shownKey]);

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

  const patchWidget = (id: OverlayWidgetId, patch: Partial<OverlayWidgetSettings>, persist: boolean) => {
    update({ ...settings, widgets: { ...settings.widgets, [id]: { ...settings.widgets[id], ...patch } } }, persist);
  };
  const patchLive = (peerId: string, patch: Partial<OverlayWidgetSettings>, persist: boolean) => {
    const index = Math.max(0, shownIds.indexOf(peerId));
    const current = liveTile(settings, peerId, index);
    update({ ...settings, lives: { ...settings.lives, [peerId]: { ...current, ...patch } } }, persist);
  };

  const anyOn = OVERLAY_WIDGETS.some((widget) => settings.widgets[widget.id].on);
  const toggleAll = (on: boolean) => {
    const widgets = { ...settings.widgets };
    for (const widget of OVERLAY_WIDGETS) widgets[widget.id] = { ...widgets[widget.id], on };
    update({ ...settings, widgets }, true);
  };
  const shown = visibleWidgets(settings, state, editing);
  const personName = (id: string) => state.people.find((person) => person.id === id)?.name ?? "Transmissão";
  // Em edição sem ninguém transmitindo, mostra uma miniatura de exemplo para poder posicionar.
  const placeholderLive = editing && settings.widgets.live.on && state.liveIds.length === 0;
  const liveTiles = shownIds.map((id, index) => ({ id, tile: liveTile(settings, id, index) }));
  const liveTileFor = (id: string): ReactNode => <div className="ov-live">
    {frameUrls[id] ? <img src={frameUrls[id]} alt="" draggable={false}/> : <span className="ov-live-empty">{id === PLACEHOLDER_LIVE ? "Transmissão" : "Carregando transmissão…"}</span>}
    <span className="ov-badge"><i/>Ao vivo</span>
    <span className="ov-live-name">{id === PLACEHOLDER_LIVE ? "Sem transmissão" : personName(id)}</span>
  </div>;

  const content: Record<Exclude<OverlayWidgetId, "live">, ReactNode> = {
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
    {liveTiles.map(({ id, tile }) => <OverlayWidget
      key={`live:${id}`}
      className="ov-live-tile"
      label={personName(id)}
      widget={tile}
      opacity={settings.opacity}
      editing={editing}
      onPatch={(patch, persist) => patchLive(id, patch, persist)}
    >{liveTileFor(id)}</OverlayWidget>)}
    {placeholderLive && <OverlayWidget
      className="ov-live-tile"
      label="Transmissão"
      widget={settings.widgets.live}
      opacity={settings.opacity}
      editing={editing}
      onPatch={(patch, persist) => patchWidget("live", patch, persist)}
    >{liveTileFor(PLACEHOLDER_LIVE)}</OverlayWidget>}
    {shown.map((id) => id === "live" ? null : <OverlayWidget
      key={id}
      className={`ov-${id}`}
      label={OVERLAY_WIDGETS.find((widget) => widget.id === id)?.label ?? id}
      widget={settings.widgets[id]}
      opacity={settings.opacity}
      editing={editing}
      onPatch={(patch, persist) => patchWidget(id, patch, persist)}
    >{content[id as Exclude<OverlayWidgetId, "live">]}</OverlayWidget>)}

    {editing && <p className="ov-hint">Arraste para mover · roda do mouse ou − e + mudam o tamanho · Esc ou Ctrl+Shift+O fixa</p>}
    {editing && <div className="ov-toolbar" role="toolbar" aria-label="Editar sobreposição">
      <button type="button" className="ov-all" aria-pressed={anyOn} onClick={() => toggleAll(!anyOn)}>{anyOn ? "Desativar tudo" : "Ativar tudo"}</button>
      <span className="ov-sep" aria-hidden="true"/>
      <div className="ov-chips">
        {OVERLAY_WIDGETS.map((widget) => <button
          key={widget.id}
          type="button"
          className={settings.widgets[widget.id].on ? "on" : ""}
          aria-pressed={settings.widgets[widget.id].on}
          onClick={() => patchWidget(widget.id, { on: !settings.widgets[widget.id].on }, true)}
        >{widget.label}</button>)}
        {settings.widgets.live.on && state.liveIds.map((id) => {
          const on = settings.lives[id]?.on !== false;
          return <button
            key={id}
            type="button"
            className={on && shownIds.includes(id) ? "on" : ""}
            aria-pressed={on}
            title={on && !shownIds.includes(id) ? `No máximo ${MAX_OVERLAY_LIVES} transmissões ao mesmo tempo` : undefined}
            onClick={() => patchLive(id, { on: !on }, true)}
          >Live de {personName(id)}</button>;
        })}
      </div>
      <span className="ov-sep" aria-hidden="true"/>
      <div className="ov-controls">
        <label>Opacidade
          <input type="range" className="sx-range" style={{ "--fill": `${Math.round(settings.opacity * 100)}%`, width: 140 } as CSSProperties} min={0} max={100} step={1} value={Math.round(settings.opacity * 100)} aria-label="Opacidade" onChange={(event) => update({ ...settings, opacity: Number(event.target.value) / 100 }, true)}/>
        </label>
        <button type="button" className="primary" onClick={finishEditing}>Concluir</button>
      </div>
    </div>}
  </div>;
}

function OverlayWidget({ className, label, widget, opacity, editing, onPatch, children }: {
  className: string;
  label: string;
  widget: OverlayWidgetSettings;
  opacity: number;
  editing: boolean;
  onPatch(patch: Partial<OverlayWidgetSettings>, persist: boolean): void;
  children: ReactNode;
}) {
  const drag = useRef<{ startX: number; startY: number; x: number; y: number } | null>(null);

  const down = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!editing || (event.target as HTMLElement).closest("button")) return;
    event.currentTarget.setPointerCapture(event.pointerId);
    drag.current = { startX: event.clientX, startY: event.clientY, x: widget.x, y: widget.y };
  };
  const move = (event: ReactPointerEvent<HTMLDivElement>) => {
    const start = drag.current;
    if (!start) return;
    onPatch({
      x: dragPosition(start.x, event.clientX - start.startX, window.innerWidth),
      y: dragPosition(start.y, event.clientY - start.startY, window.innerHeight),
    }, false);
  };
  const up = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!drag.current) return;
    drag.current = null;
    event.currentTarget.releasePointerCapture(event.pointerId);
    onPatch({}, true);
  };

  const style = {
    left: `${widget.x}%`,
    top: `${widget.y}%`,
    opacity: editing ? Math.max(opacity, 0.4) : opacity,
    transform: `scale(${widget.scale})`,
  } as CSSProperties;

  return <div
    className={`ov-widget ${className}${editing ? " editable" : ""}`}
    style={style}
    onPointerDown={down}
    onPointerMove={move}
    onPointerUp={up}
    onPointerCancel={up}
    onDragStart={(event) => event.preventDefault()}
    onWheel={(event) => { if (editing) onPatch({ scale: steppedScale(widget.scale, event.deltaY < 0 ? 1 : -1) }, true); }}
  >
    {editing && <div className="ov-handle">
      <span>{label}</span>
      <button type="button" aria-label={`Diminuir ${label}`} onClick={() => onPatch({ scale: steppedScale(widget.scale, -1) }, true)}>−</button>
      <span className="ov-scale">{Math.round(widget.scale * 100)}%</span>
      <button type="button" aria-label={`Aumentar ${label}`} onClick={() => onPatch({ scale: steppedScale(widget.scale, 1) }, true)}>+</button>
      <button type="button" aria-label={`Ocultar ${label}`} onClick={() => onPatch({ on: false }, true)}>×</button>
    </div>}
    {children}
  </div>;
}
