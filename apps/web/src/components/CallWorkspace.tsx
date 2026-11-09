import { BriksMark } from "./BriksMark";
import { Modal } from "./Modal";
import { SettingsView } from "../views/SettingsView";
import { useVideoZoom } from "./video-zoom";
import { isWindowsDesktop, listAudioApps, type AudioApp } from "../call/media/process-audio";
import {
  audioAppsForCapture,
  loadScreenAudioChoice,
  saveScreenAudioChoice,
  toggleAudioApp,
  type ScreenAudioChoice,
} from "../services/audio/screen-audio-apps";
import { loadCallLayoutMode, saveCallLayoutMode, weightedRows, bestFitLayout, type CallLayoutMode } from "./call-layout-mode";
import React, { useEffect, useMemo, useRef, useState, type CSSProperties, type FormEvent, type RefObject } from "react";
import {
  Activity,
  Copy,
  Hash,
  LayoutGrid,
  PanelRight,
  Maximize2,
  MessageCircle,
  Mic,
  MicOff,
  MonitorUp,
  PanelLeft,
  PhoneOff,
  Settings,
  Settings2,
  Sparkles,
  Video,
  VideoOff,
  Volume2,
  VolumeX,
  X,
  Check,
} from "lucide-react";
import { formatRemaining, idleStatus } from "../services/call/idle";
import { loadPersonAudio, savePersonAudio, type PersonAudio } from "../services/audio/person-audio";
import type { ChatMessage } from "../application/contracts";
import { useBriksApplication } from "../application/ApplicationContext";
import type { CallController, CallDiagnostics } from "../call";
import { buildTextReport, peerHealth } from "../call/diagnostics-report";
import type {
  ChatAttachmentProgress,
  ChatAttachmentRecord,
  ChatConnectionStatus,
  ChatController,
} from "../chat";
import { loadLocalGroups } from "../services/offline/social-storage";
import { incompatiblePeerMessage } from "../services/protocol-compatibility";
import { observeVoiceActivity } from "../services/audio/voice-activity";
import { networkPreferenceLabel } from "../services/network/settings";
import {
  buildScreenQualityProfile,
  loadScreenQualitySettings,
  saveScreenQualitySettings,
  type ScreenQualitySettings,
} from "../services/rtc/screen-quality-settings";
import { ScreenQualityPanel } from "./ScreenQualityPanel";
import { useCallStore, type Participant } from "../store";
import { ConversationTimeline } from "./ConversationTimeline";
import { responsiveCallGrid } from "./call-layout";
import { InCallAudioSettings } from "./InCallAudioSettings";
import { MessageComposer } from "./MessageComposer";
import { ProfileAvatar } from "./ProfileAvatar";
import "./call-workspace.css";
import "./screen-picker.css";

type ViewMode = "call" | "chat";

const SCREEN_AUDIO_KEY = "risk.screenShareAudio";

function initialScreenAudio(): boolean {
  return localStorage.getItem(SCREEN_AUDIO_KEY) !== "false";
}

function RemoteAudio({ stream, volume }: { stream: MediaStream; volume: number }) {
  const contextRef = useRef<AudioContext | null>(null);
  const gainRef = useRef<GainNode | null>(null);

  useEffect(() => {
    const context = new AudioContext();
    const source = context.createMediaStreamSource(stream);
    const gain = context.createGain();
    gain.gain.value = volume / 100;
    source.connect(gain).connect(context.destination);
    contextRef.current = context;
    gainRef.current = gain;
    void context.resume().catch(() => undefined);
    return () => {
      source.disconnect();
      gain.disconnect();
      void context.close();
      contextRef.current = null;
      gainRef.current = null;
    };
  }, [stream]);

  useEffect(() => {
    const gain = gainRef.current;
    if (gain) gain.gain.setTargetAtTime(volume / 100, gain.context.currentTime, 0.015);
    if (contextRef.current?.state === "suspended") void contextRef.current.resume().catch(() => undefined);
  }, [volume]);

  return null;
}

function useVoiceActivity(stream: MediaStream | null | undefined, enabled: boolean): boolean {
  const [speaking, setSpeaking] = useState(false);
  const audioTrackKey = stream?.getAudioTracks().map((track) => `${track.id}:${track.readyState}`).join(":") ?? "";

  useEffect(() => {
    setSpeaking(false);
    if (!enabled || !stream || !audioTrackKey) return;
    return observeVoiceActivity(stream, setSpeaking);
  }, [audioTrackKey, enabled, stream]);

  return enabled && speaking;
}

/**
 * Depois de um clique que começou no painel de volume, bloqueia o click/dblclick seguinte.
 * Sem isso, soltar o mouse fora do painel (arrastando o slider) faz o clique ir para o bloco
 * da transmissão e maximizar/focar a tela.
 */
function shieldClicksFromPanel(): void {
  const block = (event: Event) => { event.stopPropagation(); event.preventDefault(); };
  window.addEventListener("click", block, true);
  window.addEventListener("dblclick", block, true);
  const release = () => {
    window.removeEventListener("pointerup", release, true);
    window.removeEventListener("pointercancel", release, true);
    window.setTimeout(() => {
      window.removeEventListener("click", block, true);
      window.removeEventListener("dblclick", block, true);
    }, 400);
  };
  window.addEventListener("pointerup", release, true);
  window.addEventListener("pointercancel", release, true);
}

function VolumeControl({ label, value, onChange }: { label: string; value: number; onChange(value: number): void }) {
  return <label className="volume-control" title={`${label}: ${value}%`}>
    {value === 0 ? <VolumeX size={15}/> : <Volume2 size={15}/>}<span>{label}</span>
    <input type="range" min="0" max="200" step="5" value={value} onChange={(event) => onChange(Number(event.target.value))} aria-label={`Volume de ${label}`}/>
    <b>{value}%</b>
  </label>;
}

function FullscreenButton({ target }: { target: RefObject<HTMLElement | null> }) {
  return <button
    type="button"
    className="tile-fullscreen"
    title="Tela cheia"
    aria-label="Abrir transmissão em tela cheia"
    onClick={(event) => {
      event.stopPropagation();
      const element = target.current;
      if (element) void element.requestFullscreen().catch(() => undefined);
    }}
  ><Maximize2 size={16}/></button>;
}

function VideoTile({
  participant,
  tileId,
  focused,
  compact,
  deafened,
  watching,
  onFocus,
}: {
  participant: Participant;
  tileId: string;
  focused: boolean;
  compact: boolean;
  deafened: boolean;
  /** Modo Transmissão: só aí a tela compartilhada (vídeo e áudio) é exibida. */
  watching: boolean;
  onFocus(tileId: string): void;
}) {
  const streams = Object.values(participant.streams ?? {});
  const screenStream = participant.state.screenStreamId ? participant.streams?.[participant.state.screenStreamId] : undefined;
  const secondId = participant.state.secondScreenStreamId;
  const cameraStream = (participant.state.cameraStreamId ? participant.streams?.[participant.state.cameraStreamId] : undefined)
    ?? streams.find((stream) => stream.id !== screenStream?.id && stream.id !== secondId && stream.getVideoTracks().length > 0)
    ?? streams.find((stream) => stream.id !== screenStream?.id && stream.id !== secondId);
  const microphoneStream = streams.find((stream) =>
    stream.id !== screenStream?.id
    && stream.id !== secondId
    && stream.getAudioTracks().some((track) => track.readyState === "live"),
  ) ?? (cameraStream?.getAudioTracks().length ? cameraStream : undefined);
  const [source, setSource] = useState<"camera" | "screen">("camera");
  const [audio, setAudio] = useState<PersonAudio>(() => loadPersonAudio(participant.displayName));
  const updateAudio = (patch: Partial<PersonAudio>) => setAudio((current) => {
    const next = { ...current, ...patch };
    savePersonAudio(participant.displayName, next);
    return next;
  });
  const userVolume = audio.user;
  const screenVolume = audio.screen;
  const videoRef = useRef<HTMLVideoElement>(null);
  const articleRef = useRef<HTMLElement>(null);

  useEffect(() => {
    if (watching && participant.state.screenShare && screenStream) setSource("screen");
    else if (!watching || !participant.state.screenShare) setSource("camera");
  }, [watching, participant.state.screenShare, screenStream]);

  const selected = source === "screen" ? screenStream : cameraStream;
  const trackKey = selected?.getVideoTracks().map((track) => track.id).join(":") ?? "";
  useEffect(() => {
    if (videoRef.current) {
      videoRef.current.srcObject = selected ?? null;
      void videoRef.current.play().catch(() => undefined);
    }
  }, [selected, trackKey]);

  const hasVideo = Boolean(selected?.getVideoTracks().length);
  const canSwitch = watching && participant.state.camera && Boolean(cameraStream && screenStream);
  // Quem está mutado não deve ser ouvido mesmo que o servidor (SFU) ainda repasse o áudio, p.ex. versões antigas do remetente.
  const remoteMuted = Boolean(participant.stateReceived) && !participant.state.microphone;
  const userHasAudio = Boolean(microphoneStream?.getAudioTracks().some((track) => track.readyState === "live"));
  const screenHasAudio = watching && Boolean(screenStream?.getAudioTracks().some((track) => track.readyState === "live"));
  const speaking = useVoiceActivity(microphoneStream, participant.state.microphone && userHasAudio);
  const zoom = useVideoZoom(articleRef, source === "screen" && hasVideo && !compact);

  return <article
    ref={articleRef}
    className={`tile ${source} ${zoom.zoomed ? "zoomed" : ""} ${participant.connection === "connected" ? "online" : ""} ${speaking ? "speaking" : ""} ${focused ? "focused" : ""} ${compact ? "thumbnail" : ""}`}
    onClick={(event) => {
      if ((event.target as HTMLElement).closest("button,input,.volume-panel")) return;
      onFocus(tileId);
    }}
    onDoubleClick={(event) => {
      if ((event.target as HTMLElement).closest("button,input,.volume-panel")) return;
      void articleRef.current?.requestFullscreen().catch(() => undefined);
    }}
  >
    <video ref={videoRef} autoPlay playsInline muted className={hasVideo ? source : "hidden-video"} style={zoom.videoStyle}/>
    {zoom.badge}
    {!hasVideo && <div className="video-off"><ProfileAvatar displayName={participant.displayName} avatar={participant.avatar} className="call-profile-avatar"/><span>Vídeo desligado</span></div>}
    {userHasAudio && <RemoteAudio stream={microphoneStream!} volume={deafened || remoteMuted || audio.muted ? 0 : userVolume}/>}
    {screenHasAudio && <RemoteAudio stream={screenStream!} volume={deafened || audio.muted ? 0 : screenVolume}/>}
    <FullscreenButton target={articleRef}/>
    {canSwitch && <div className="source-switch">
      <button className={source === "camera" ? "selected" : ""} onClick={(event) => { event.stopPropagation(); setSource("camera"); }}><Video size={14}/> Câmera</button>
      <button className={source === "screen" ? "selected" : ""} onClick={(event) => { event.stopPropagation(); setSource("screen"); }}><MonitorUp size={14}/> Tela</button>
    </div>}
    {!compact && <button type="button" className={`volume-toggle ${audio.muted ? "muted" : ""}`} aria-label={audio.muted ? `Ouvir ${participant.displayName}` : `Silenciar ${participant.displayName}`} aria-pressed={audio.muted} title={audio.muted ? "Silenciado só para você. Clique para ouvir" : "Clique para silenciar só para você"} onClick={(event) => { event.stopPropagation(); updateAudio({ muted: !audio.muted }); }} onDoubleClick={(event) => event.stopPropagation()}>{audio.muted ? <VolumeX size={15}/> : <Volume2 size={15}/>}</button>}
    {(userHasAudio || screenHasAudio) && !compact && <div className="volume-panel" onClick={(event) => event.stopPropagation()} onDoubleClick={(event) => event.stopPropagation()} onMouseDown={(event) => event.stopPropagation()} onPointerDown={(event) => { event.stopPropagation(); shieldClicksFromPanel(); }}>
      {userHasAudio && <VolumeControl label="Usuário" value={userVolume} onChange={(value) => updateAudio({ user: value })}/>} 
      {screenHasAudio && <VolumeControl label="Transmissão" value={screenVolume} onChange={(value) => updateAudio({ screen: value })}/>} 
    </div>}
    <div className="tile-label"><span>{participant.displayName}</span>{!participant.state.microphone && <MicOff size={15}/>}{audio.muted && <VolumeX size={15} className="muted-person"/>} {watching && participant.state.screenAudio && !compact && <em>ÁUDIO DA TELA</em>}</div>
  </article>;
}

function RemoteSecondScreenTile({ participant, stream, tileId, focused, compact, onFocus }: {
  participant: Participant;
  stream: MediaStream;
  tileId: string;
  focused: boolean;
  compact: boolean;
  onFocus(tileId: string): void;
}) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const articleRef = useRef<HTMLElement>(null);
  const trackKey = stream.getVideoTracks().map((track) => track.id).join(":");
  useEffect(() => {
    if (videoRef.current) {
      videoRef.current.srcObject = stream;
      void videoRef.current.play().catch(() => undefined);
    }
  }, [stream, trackKey]);
  const zoom = useVideoZoom(articleRef, !compact);
  return <article
    ref={articleRef}
    className={`tile screen online ${zoom.zoomed ? "zoomed" : ""} ${focused ? "focused" : ""} ${compact ? "thumbnail" : ""}`}
    onClick={(event) => {
      if ((event.target as HTMLElement).closest("button")) return;
      onFocus(tileId);
    }}
    onDoubleClick={() => { void articleRef.current?.requestFullscreen().catch(() => undefined); }}
  >
    <video ref={videoRef} autoPlay playsInline muted className="screen" style={zoom.videoStyle}/>
    {zoom.badge}
    <FullscreenButton target={articleRef}/>
    <div className="tile-label"><span>{participant.displayName} · 2ª tela</span></div>
  </article>;
}

function LocalProfileTile({ displayName, avatar, microphone, microphoneStream, tileId, focused, compact, onFocus }: {
  displayName: string;
  avatar?: string;
  microphone: boolean;
  microphoneStream: MediaStream | null;
  tileId: string;
  focused: boolean;
  compact: boolean;
  onFocus(tileId: string): void;
}) {
  const speaking = useVoiceActivity(microphoneStream, microphone);
  return <article
    className={`tile local online profile-only ${speaking ? "speaking" : ""} ${focused ? "focused" : ""} ${compact ? "thumbnail" : ""}`}
    onClick={() => onFocus(tileId)}
  >
    <div className="video-off"><ProfileAvatar displayName={displayName} avatar={avatar} className="call-profile-avatar"/></div>
    <div className="tile-label"><span>Você · {displayName}</span>{!microphone && <MicOff size={15}/>}</div>
  </article>;
}

function LocalVideoTile({
  stream,
  tileId,
  label,
  mirrored,
  microphone,
  microphoneStream,
  focused,
  compact,
  onFocus,
}: {
  stream: MediaStream;
  tileId: string;
  label: string;
  mirrored: boolean;
  microphone: boolean;
  microphoneStream?: MediaStream | null;
  focused: boolean;
  compact: boolean;
  onFocus(tileId: string): void;
}) {
  const videoRef = useRef<HTMLVideoElement>(null);
  const articleRef = useRef<HTMLElement>(null);
  const source = mirrored ? "camera" : "screen";
  const speaking = useVoiceActivity(microphoneStream, microphone && Boolean(microphoneStream));
  useEffect(() => {
    if (videoRef.current) {
      videoRef.current.srcObject = stream;
      void videoRef.current.play().catch(() => undefined);
    }
  }, [stream]);
  const zoom = useVideoZoom(articleRef, !mirrored && !compact);

  return <article
    ref={articleRef}
    className={`tile local online ${source} ${zoom.zoomed ? "zoomed" : ""} ${speaking ? "speaking" : ""} ${focused ? "focused" : ""} ${compact ? "thumbnail" : ""}`}
    onClick={(event) => {
      if ((event.target as HTMLElement).closest("button")) return;
      onFocus(tileId);
    }}
    onDoubleClick={() => { void articleRef.current?.requestFullscreen().catch(() => undefined); }}
  >
    <video ref={videoRef} autoPlay playsInline muted className={`${source}${mirrored ? " mirrored" : ""}`} style={zoom.videoStyle}/>
    {zoom.badge}
    <FullscreenButton target={articleRef}/>
    <div className="tile-label" title={`Você · ${label}`}><span>Você · {compact ? label.split(" · ")[0] : label}</span>{!microphone && <MicOff size={15}/>} {!compact && <em>PRÉVIA</em>}</div>
  </article>;
}

function upsertAttachment(current: ChatAttachmentRecord[], record: ChatAttachmentRecord): ChatAttachmentRecord[] {
  const index = current.findIndex((item) => item.attachmentId === record.attachmentId);
  if (index < 0) return [...current, record].sort((left, right) => left.createdAt.localeCompare(right.createdAt));
  const existing = current[index]!;
  const replacement = record.updatedAt >= existing.updatedAt ? record : existing;
  const next = [...current];
  next[index] = replacement;
  return next.sort((left, right) => left.createdAt.localeCompare(right.createdAt));
}

const appIconCache = new Map<string, Promise<string | null>>();

/** Ícone do executável (via Electron). Sem ícone ou fora do desktop, cai no alto-falante. */
function AppIcon({ path }: { path?: string }) {
  const [src, setSrc] = useState<string | null>(null);
  useEffect(() => {
    if (!path || !window.desktop?.getAppIcon) return undefined;
    let alive = true;
    let pending = appIconCache.get(path);
    if (!pending) {
      pending = window.desktop.getAppIcon(path).catch(() => null);
      appIconCache.set(path, pending);
    }
    void pending.then((value) => { if (alive) setSrc(value); });
    return () => { alive = false; };
  }, [path]);
  return src ? <img src={src} alt="" draggable={false}/> : <Volume2 size={15}/>;
}

export function ScreenSourcePicker({
  sources,
  loading,
  includeAudio,
  onAudioChange,
  onChoose,
  onClose,
}: {
  sources: BriksDesktopSource[];
  loading: boolean;
  includeAudio: boolean;
  onAudioChange(includeAudio: boolean): void;
  onChoose(source: BriksDesktopSource, audioApps?: string[]): void;
  onClose(): void;
}) {
  const windowsAudio = isWindowsDesktop();
  const [choice, setChoice] = useState(loadScreenAudioChoice);
  const [audioApps, setAudioApps] = useState<AudioApp[]>([]);
  const audioMode: "source" | "apps" | "none" = !includeAudio ? "none" : choice.appsMode ? "apps" : "source";
  useEffect(() => {
    if (!windowsAudio || audioMode !== "apps") return undefined;
    let alive = true;
    const load = async () => {
      const next = await listAudioApps();
      if (alive) setAudioApps(next);
    };
    void load();
    const timer = window.setInterval(() => void load(), 2000);
    return () => { alive = false; window.clearInterval(timer); };
  }, [windowsAudio, audioMode]);
  const updateChoice = (next: ScreenAudioChoice) => { setChoice(next); saveScreenAudioChoice(next); };
  const changeMode = (mode: "source" | "apps" | "none") => {
    if (mode === "none") { onAudioChange(false); return; }
    onAudioChange(true);
    updateChoice({ ...choice, appsMode: mode === "apps" });
  };
  // Apps marcados que não estão abertos agora continuam na lista, para o Spotify fechado não sumir da escolha.
  const appRows: AudioApp[] = [
    ...audioApps,
    ...choice.names.filter((name) => !audioApps.some((app) => app.name === name)).map((name) => ({ name, label: name.replace(/\.exe$/i, "").replace(/^./, (first) => first.toUpperCase()), playing: false, closed: true })),
  ];
  const confirm = (source: BriksDesktopSource) => onChoose(source, audioAppsForCapture(includeAudio, choice));
  const screens = sources.filter((source) => source.id.startsWith("screen:"));
  const windows = sources.filter((source) => !source.id.startsWith("screen:"));
  const [tab, setTab] = useState<"screens" | "windows">("screens");
  const active = tab === "screens" && screens.length === 0 && windows.length > 0 ? "windows" : tab;
  const visible = active === "screens" ? screens : windows;
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const selected = visible.find((source) => source.id === selectedId) ?? visible[0];
  return <div className="sp-backdrop" onMouseDown={onClose}>
    <section className="sp" role="dialog" aria-modal="true" aria-label="Compartilhar tela" onMouseDown={(event) => event.stopPropagation()}>
      <div className="sp-head">
        <span className="sp-head-icon"><MonitorUp size={20}/></span>
        <div className="sp-head-text"><h2>Compartilhar tela</h2><p>Escolha a fonte e confirme</p></div>
        <button className="sp-close" onClick={onClose} aria-label="Fechar"><X size={18}/></button>
      </div>
      <div className="sp-bar">
        <div className="sp-tabs" role="tablist" aria-label="Tipo de fonte">
          <button role="tab" aria-selected={active === "screens"} className={active === "screens" ? "on" : ""} onClick={() => setTab("screens")}>Telas <small>{screens.length}</small></button>
          <button role="tab" aria-selected={active === "windows"} className={active === "windows" ? "on" : ""} onClick={() => setTab("windows")}>Janelas <small>{windows.length}</small></button>
        </div>
      </div>
      {loading ? <div className="sp-state"><Sparkles/><span>Buscando telas e janelas…</span></div> :
        visible.length === 0 ? <div className="sp-state"><span>{active === "screens" ? "Nenhuma tela encontrada." : "Nenhuma janela aberta encontrada."}</span></div> :
        <div className="sp-grid">
          {visible.map((source) => <button
            key={source.id}
            className={`sp-card${selected?.id === source.id ? " sel" : ""}`}
            aria-pressed={selected?.id === source.id}
            onClick={() => setSelectedId(source.id)}
            onDoubleClick={() => confirm(source)}
          >
            <span className="sp-thumb">{source.thumbnail ? <img src={source.thumbnail} alt="" draggable={false}/> : <MonitorUp/>}<span className="sp-check" aria-hidden="true"><Check size={14}/></span></span>
            <span className="sp-name" title={source.name}>{source.name}</span>
          </button>)}
        </div>}
      {windowsAudio && <div className="sp-audio-panel">
        <div className="sp-audio-row">
          <strong>Áudio da transmissão</strong>
          <div className="sp-seg" role="group" aria-label="Origem do áudio">
            <button className={audioMode === "source" ? "on" : ""} aria-pressed={audioMode === "source"} onClick={() => changeMode("source")}>Da fonte</button>
            <button className={audioMode === "apps" ? "on" : ""} aria-pressed={audioMode === "apps"} onClick={() => changeMode("apps")}>Escolher apps</button>
            <button className={audioMode === "none" ? "on" : ""} aria-pressed={audioMode === "none"} onClick={() => changeMode("none")}>Sem áudio</button>
          </div>
        </div>
        {audioMode === "apps" && <>
          {appRows.length === 0
            ? <p className="sp-apps-empty">Nenhum app com áudio aberto. Abra o Spotify ou o jogo e ele aparece aqui.</p>
            : <div className="sp-apps">
              {appRows.map((app) => {
                const on = choice.names.includes(app.name);
                const closed = "closed" in app;
                return <button
                  key={app.name}
                  className={`sp-app${on ? " on" : ""}${app.playing ? " playing" : ""}`}
                  role="switch"
                  aria-checked={on}
                  onClick={() => updateChoice({ ...choice, names: toggleAudioApp(choice.names, app.name) })}
                >
                  <span className="sp-app-icon" aria-hidden="true"><AppIcon path={app.path}/></span>
                  <span className="sp-app-text"><b title={app.name}>{app.label}</b><small>{closed ? "Fechado agora" : app.playing ? "Tocando agora" : "Em pausa"}</small></span>
                  <i aria-hidden="true"/>
                </button>;
              })}
            </div>}
          <p className="sp-apps-note">{choice.names.length === 0 ? "Nenhum app marcado: a transmissão sai sem áudio. " : ""}O Discord nunca entra na transmissão. Para mudar os apps depois, pare e inicie a transmissão.</p>
        </>}
      </div>}
      <div className="sp-actions">
        {!windowsAudio && <button
          className={`sp-audio ${includeAudio ? "on" : ""}`}
          onClick={() => onAudioChange(!includeAudio)}
          role="switch"
          aria-checked={includeAudio}
          title={includeAudio ? "O áudio será transmitido" : "Só o vídeo será transmitido"}
        >{includeAudio ? <Volume2 size={16}/> : <VolumeX size={16}/>}<span>{includeAudio ? "Com áudio" : "Sem áudio"}</span><i aria-hidden="true"/></button>}
        <button className="sp-go" onClick={() => selected && confirm(selected)} title={selected ? `Transmitir ${selected.name}` : undefined}>{selected ? `Transmitir ${selected.name}` : "Transmitir"}</button>
      </div>
      <div className="sp-foot">{audioMode === "apps" ? "Só o som dos apps marcados entra na transmissão." : "Janela: só o som dela, sem o do Discord. Abas do navegador levam o som do navegador inteiro."}</div>
    </section>
  </div>;
}

export function CallWorkspace({ call, chat, onMinimize }: { call: CallController; chat: ChatController; onMinimize(): void }) {
  const { gateway: api } = useBriksApplication();
  const token = useCallStore((state) => state.token)!;
  const roomId = useCallStore((state) => state.roomId)!;
  const callContext = useCallStore((state) => state.callContext);
  const setCallContext = useCallStore((state) => state.setCallContext);
  const participants = useCallStore((state) => state.participants);
  const localPreviews = useCallStore((state) => state.localPreviews);
  const localState = useCallStore((state) => state.localState);
  const callError = useCallStore((state) => state.error);
  const setError = useCallStore((state) => state.setError);
  const setRoom = useCallStore((state) => state.setRoom);
  const peers = Object.values(participants);
  const [view, setView] = useState<ViewMode>("call");
  // A chamada abre em modo Call (só as pessoas); as transmissões só aparecem no modo Transmissão.
  const [stageMode, setStageMode] = useState<"call" | "stream">("call");
  const streaming = stageMode === "stream";
  // O que está indo no áudio da transmissão: nomes dos apps escolhidos, "áudio da fonte" ou nenhum.
  const [sharedAudioApps, setSharedAudioApps] = useState<string[] | null>(null);
  const [focusedTile, setFocusedTile] = useState<string | null>(null);
  const stageRef = useRef<HTMLElement>(null);
  const [stageSize, setStageSize] = useState({ width: 0, height: 0 });
  const [layoutMode, setLayoutMode] = useState<CallLayoutMode>(loadCallLayoutMode);
  const [chatStatus, setChatStatus] = useState<ChatConnectionStatus>("disconnected");
  const [messages, setMessages] = useState<ChatMessage[]>([]);
  const [attachments, setAttachments] = useState<ChatAttachmentRecord[]>([]);
  const [attachmentProgress, setAttachmentProgress] = useState<Record<string, ChatAttachmentProgress | undefined>>({});
  const [qualitySettings, setQualitySettings] = useState<ScreenQualitySettings>(() => loadScreenQualitySettings());
  const [qualityPanelOpen, setQualityPanelOpen] = useState(false);
  const qualityProfile = useMemo(() => buildScreenQualityProfile(qualitySettings), [qualitySettings]);
  const [screenAudioEnabled, setScreenAudioEnabled] = useState(initialScreenAudio);
  const [pickingSecond, setPickingSecond] = useState(false);
  const [audioSettingsOpen, setAudioSettingsOpen] = useState(false);
  const [appSettingsOpen, setAppSettingsOpen] = useState(false);
  const [sourcePickerOpen, setSourcePickerOpen] = useState(false);
  const [screenSources, setScreenSources] = useState<BriksDesktopSource[]>([]);
  const [screenSourcesLoading, setScreenSourcesLoading] = useState(false);
  const [networkQuality, setNetworkQuality] = useState("Boa");
  const [deafened, setDeafened] = useState(false);
  const [diagnosticsOpen, setDiagnosticsOpen] = useState(false);
  const [diagnostics, setDiagnostics] = useState<CallDiagnostics | null>(null);
  const [replyingTo, setReplyingTo] = useState<ChatMessage | null>(null);
  const [typingUsers, setTypingUsers] = useState<string[]>([]);

  const context = callContext;

  useEffect(() => {
    if (context) return;
    let alive = true;
    void (async () => {
      try {
        const [localGroups, profile] = await Promise.all([loadLocalGroups(), api.me(token)]);
        if (!alive) return;
        const localGroup = localGroups.find((item) => item.channels.some((channel) => channel.kind === "voice" && channel.voiceRoomId === roomId));
        if (localGroup) {
          const voiceChannel = localGroup.channels.find((channel) => channel.kind === "voice" && channel.voiceRoomId === roomId) ?? null;
          const textChannel = localGroup.channels.find((channel) => channel.kind === "text") ?? null;
          setCallContext({
            groupId: localGroup.groupId,
            groupName: localGroup.name,
            voiceChannelId: voiceChannel?.id ?? null,
            voiceChannelName: voiceChannel?.name ?? null,
            textChannelId: textChannel?.id ?? null,
            textChannelName: textChannel?.name ?? null,
            displayName: profile.displayName,
            avatar: profile.avatar,
          });
          return;
        }

        const communities = await api.communities(token).catch(() => []);
        for (const community of communities) {
          if (!alive) return;
          const channels = await api.channels(token, community.id).catch(() => []);
          const voiceChannel = channels.find((channel) => channel.kind === "voice" && channel.voiceRoomId === roomId);
          if (!voiceChannel) continue;
          const textChannel = channels.find((channel) => channel.kind === "text") ?? null;
          setCallContext({
            groupId: community.id,
            groupName: community.name,
            voiceChannelId: voiceChannel.id,
            voiceChannelName: voiceChannel.name,
            textChannelId: textChannel?.id ?? null,
            textChannelName: textChannel?.name ?? null,
            displayName: profile.displayName,
            avatar: profile.avatar,
          });
          return;
        }
      } catch {
        // A chamada continua funcionando mesmo se não conseguirmos resolver o chat.
      }
    })();
    return () => { alive = false; };
  }, [context, roomId, setCallContext, token]);

  useEffect(() => {
    const channelId = context?.textChannelId;
    if (!channelId) {
      setChatStatus("disconnected");
      return;
    }
    let alive = true;
    const offMessage = chat.onMessage((message) => {
      if (!alive || message.channelId !== channelId) return;
      setMessages((current) => {
        const index = current.findIndex((item) => item.id === message.id);
        if (index < 0) return [...current, message].sort((left, right) => left.createdAt.localeCompare(right.createdAt));
        const next = [...current];
        next[index] = message;
        return next;
      });
    });
    const offTyping = chat.onTyping((participants) => {
      if (alive) setTypingUsers(participants.map((participant) => participant.displayName));
    });
    const offStatus = chat.onStatus((status) => {
      if (!alive) return;
      setChatStatus(status);
      if (status === "incompatible") setError(incompatiblePeerMessage());
    });
    const offAttachment = chat.onAttachment((record) => {
      if (alive && record.channelId === channelId) setAttachments((current) => upsertAttachment(current, record));
    });
    const offProgress = chat.onAttachmentProgress((progress) => {
      if (!alive || progress.record.channelId !== channelId) return;
      setAttachmentProgress((current) => ({ ...current, [progress.record.attachmentId]: progress }));
      setAttachments((current) => upsertAttachment(current, progress.record));
    });

    void (async () => {
      try {
        await chat.disconnect().catch(() => undefined);
        const [{ iceServers }, history, storedAttachments] = await Promise.all([
          api.turnCredentials(token),
          chat.history(channelId),
          chat.attachmentHistory(channelId),
        ]);
        if (!alive) return;
        setMessages([...history].sort((left, right) => left.createdAt.localeCompare(right.createdAt)));
        setAttachments(storedAttachments.reduce<ChatAttachmentRecord[]>((items, record) => upsertAttachment(items, record), []));
        await chat.connect(channelId, context.displayName, iceServers);
      } catch (cause) {
        if (alive) setError(cause instanceof Error ? cause.message : "Não foi possível conectar o chat do grupo.");
      }
    })();

    return () => {
      alive = false;
      offMessage();
      offTyping();
      offStatus();
      offAttachment();
      offProgress();
      setReplyingTo(null);
      setTypingUsers([]);
      void chat.disconnect();
    };
  }, [chat, context?.displayName, context?.textChannelId, setError, token]);

  useEffect(() => {
    saveScreenQualitySettings(qualitySettings);
    void call.updateScreenQuality(qualityProfile).catch((cause) => {
      setError(cause instanceof Error ? cause.message : "Não foi possível atualizar a qualidade da transmissão.");
    });
  }, [call, qualityProfile, qualitySettings, setError]);

  useEffect(() => {
    localStorage.setItem(SCREEN_AUDIO_KEY, String(screenAudioEnabled));
  }, [screenAudioEnabled]);

  useEffect(() => {
    let alive = true;
    const update = async () => {
      const diagnostics = await call.getLiveDiagnostics().catch(() => null);
      if (!alive || !diagnostics) return;
      setDiagnostics(diagnostics);
      const degraded = diagnostics.peerConnections.some((peer) => (peer.roundTripTimeMs ?? 0) > 350 || (peer.jitterMs ?? 0) > 60 || (peer.packetsLost ?? 0) > 20);
      const disconnected = diagnostics.peerConnections.some((peer) => !["connected", "connecting", "new"].includes(peer.connectionState));
      setNetworkQuality(disconnected ? "Instável" : degraded ? "Limitada" : "Boa");
    };
    void update();
    const timer = window.setInterval(() => void update(), 3_000);
    return () => { alive = false; window.clearInterval(timer); };
  }, [call]);

  const sharingScreen = Boolean(localPreviews.screen || localPreviews.screen2);
  useEffect(() => {
    const node = stageRef.current;
    if (!node || typeof ResizeObserver === "undefined") return;
    const measure = () => {
      const style = getComputedStyle(node);
      const width = node.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
      const height = node.clientHeight - parseFloat(style.paddingTop) - parseFloat(style.paddingBottom);
      setStageSize((current) => Math.abs(current.width - width) < 1 && Math.abs(current.height - height) < 1 ? current : { width, height });
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(node);
    return () => observer.disconnect();
  }, [view]);

  const tileIds = useMemo(() => {
    const ids: string[] = [];
    if (!localPreviews.camera && !(sharingScreen && streaming)) ids.push("local-profile");
    if (localPreviews.camera) ids.push("local-camera");
    if (streaming && localPreviews.screen) ids.push("local-screen");
    if (streaming && localPreviews.screen2) ids.push("local-screen2");
    peers.forEach((peer) => {
      ids.push(`peer-${peer.peerId}`);
      if (streaming && peer.state.secondScreenStreamId && peer.streams?.[peer.state.secondScreenStreamId]) ids.push(`peer2-${peer.peerId}`);
    });
    return ids;
  }, [localPreviews.camera, sharingScreen, streaming, localPreviews.screen, localPreviews.screen2, peers]);

  useEffect(() => {
    if (focusedTile && !tileIds.includes(focusedTile)) setFocusedTile(null);
  }, [focusedTile, tileIds]);

  async function showScreenPicker(second = false): Promise<void> {
    setPickingSecond(second);
    if (second) {
      if (localPreviews.screen2) { await call.toggleSecondScreen(roomId); return; }
      if (!window.desktop?.listScreenSources) { await call.toggleSecondScreen(roomId, undefined, qualityProfile); return; }
    } else if (localPreviews.screen) {
      await call.toggleScreen(roomId);
      return;
    }
    if (!window.desktop?.listScreenSources) {
      await call.toggleScreen(roomId, undefined, screenAudioEnabled, qualityProfile);
      return;
    }
    setSourcePickerOpen(true);
    setScreenSourcesLoading(true);
    try {
      setScreenSources(await window.desktop.listScreenSources());
    } catch (cause) {
      setSourcePickerOpen(false);
      setError(cause instanceof Error ? cause.message : "Não foi possível listar as telas disponíveis.");
    } finally {
      setScreenSourcesLoading(false);
    }
  }

  async function chooseScreen(source: BriksDesktopSource, audioApps?: string[]): Promise<void> {
    setSourcePickerOpen(false);
    try {
      if (pickingSecond) await call.toggleSecondScreen(roomId, source.id, qualityProfile);
      else {
        // Modo "escolher apps" sem nenhum app marcado: a transmissão sai sem áudio.
        const includeAudio = screenAudioEnabled && audioApps?.length !== 0;
        setSharedAudioApps(includeAudio ? (audioApps?.length ? audioApps : []) : null);
        await call.toggleScreen(roomId, source.id, includeAudio, qualityProfile, audioApps?.length ? audioApps : undefined);
      }
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível iniciar o compartilhamento.");
    }
  }

  async function submitMessage(event: FormEvent<HTMLFormElement>): Promise<void> {
    event.preventDefault();
    const input = event.currentTarget.elements.namedItem("message") as HTMLInputElement;
    const content = input.value.trim();
    if (!content) return;
    try {
      await chat.send(content, replyingTo ? { replyToId: replyingTo.id } : {});
      input.value = "";
      chat.setTyping(false);
      setReplyingTo(null);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Falha ao enviar mensagem.");
    }
  }

  async function sendFiles(files: File[]): Promise<void> {
    try {
      for (const file of files) await chat.sendAttachment(file);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Falha ao enviar arquivo.");
    }
  }

  async function attachmentAction(action: (record: ChatAttachmentRecord) => Promise<void>, record: ChatAttachmentRecord): Promise<void> {
    try { await action(record); }
    catch (cause) { setError(cause instanceof Error ? cause.message : "Falha na operação com o arquivo."); }
  }

  async function messageAction(action: () => Promise<void>, fallback: string): Promise<void> {
    try { await action(); }
    catch (cause) { setError(cause instanceof Error ? cause.message : fallback); }
  }

  const allLiveTileIds = [
    ...(localPreviews.screen ? ["local-screen"] : []),
    ...(localPreviews.screen2 ? ["local-screen2"] : []),
    ...peers.flatMap((peer) => [
      ...(peer.state.secondScreenStreamId && peer.streams?.[peer.state.secondScreenStreamId] ? [`peer2-${peer.peerId}`] : []),
      ...(peer.state.screenShare && peer.state.screenStreamId && peer.streams?.[peer.state.screenStreamId] ? [`peer-${peer.peerId}`] : []),
    ]),
  ];
  const sharedAudioText = !localState.screenAudio
    ? "sem áudio"
    : sharedAudioApps && sharedAudioApps.length > 0
      ? sharedAudioApps.map((name) => name.replace(/\.exe$/i, "").replace(/^./, (first) => first.toUpperCase())).join(", ")
      : "áudio da fonte";
  const liveTileIds = streaming ? allLiveTileIds : [];
  const liveCount = allLiveTileIds.length;
  useEffect(() => { if (streaming && liveCount === 0) setStageMode("call"); }, [streaming, liveCount]);
  // Ao começar a transmitir a própria tela, entra sozinho no modo Transmissão.
  const wasSharingRef = useRef(false);
  useEffect(() => {
    if (sharingScreen && !wasSharingRef.current) setStageMode("stream");
    wasSharingRef.current = sharingScreen;
  }, [sharingScreen]);
  const broadcasterNames = [...new Set([
    ...(sharingScreen ? ["Você"] : []),
    ...peers.filter((peer) => (peer.state.screenShare && peer.state.screenStreamId && peer.streams?.[peer.state.screenStreamId]) || (peer.state.secondScreenStreamId && peer.streams?.[peer.state.secondScreenStreamId])).map((peer) => peer.displayName),
  ])];
  const broadcasterText = broadcasterNames.length === 0 ? "" : broadcasterNames.length === 1
    ? (broadcasterNames[0] === "Você" ? "Você está transmitindo a tela" : `${broadcasterNames[0]} está transmitindo a tela`)
    : `${broadcasterNames.slice(0, -1).join(", ")} e ${broadcasterNames[broadcasterNames.length - 1]} estão transmitindo a tela`;

  // Inatividade: 1 h sem falar desconecta; nos 5 minutos finais aparece o aviso com "Continuar".
  const localSpeaking = useVoiceActivity(localPreviews.microphone, localState.microphone && Boolean(localPreviews.microphone));
  const lastActiveRef = useRef(Date.now());
  const [idleRemaining, setIdleRemaining] = useState<number | null>(null);
  useEffect(() => { if (localSpeaking) lastActiveRef.current = Date.now(); }, [localSpeaking]);
  const leaveCall = () => {
    setAudioSettingsOpen(false);
    setRoom(null);
    setCallContext(null);
    void chat.disconnect();
    void call.leave(roomId);
  };
  useEffect(() => {
    const timer = window.setInterval(() => {
      // Transmitir a tela conta como presença ativa.
      if (sharingScreen) lastActiveRef.current = Date.now();
      const status = idleStatus(Date.now() - lastActiveRef.current);
      if (status.phase === "expired") { setIdleRemaining(null); leaveCall(); return; }
      setIdleRemaining(status.phase === "warning" ? status.remainingMs : null);
    }, 1_000);
    return () => window.clearInterval(timer);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sharingScreen, roomId]);
  const manualFocus = focusedTile && tileIds.includes(focusedTile) ? focusedTile : null;
  const focused = manualFocus ?? (layoutMode === "focus" ? (liveTileIds[0] ?? null) : null);
  const publishFocusedTile = useCallStore((state) => state.setFocusedTile);
  // A miniatura "Ao vivo" da barra lateral usa a tela maximizada aqui; fica guardada ao minimizar a chamada.
  useEffect(() => { publishFocusedTile(focused); }, [focused, publishFocusedTile]);
  const weighted = !focused && liveTileIds.length > 0;
  const livesOnly = weighted && liveTileIds.length <= 2 && tileIds.length === liveTileIds.length;
  function chooseLayout(mode: CallLayoutMode): void {
    setLayoutMode(mode);
    saveCallLayoutMode(mode);
    setFocusedTile(null);
  }
  const responsiveGrid = responsiveCallGrid(tileIds.length);
  const fit = livesOnly && stageSize.width > 0 && stageSize.height > 0 ? bestFitLayout(liveTileIds.length, stageSize.width, stageSize.height) : null;
  const stageStyle = {
    ...(fit ? {
      gridTemplateColumns: `repeat(${fit.columns}, ${Math.floor(fit.tileWidth)}px)`,
      gridAutoRows: `${Math.floor(fit.tileHeight)}px`,
      justifyContent: "center",
      alignContent: "center",
    } : {}),
    "--weight-rows": weightedRows(liveTileIds.length, Math.max(0, tileIds.length - liveTileIds.length)),
    "--call-portrait-columns": responsiveGrid.portrait.columns,
    "--call-portrait-rows": responsiveGrid.portrait.rows,
    "--call-compact-columns": responsiveGrid.compactLandscape.columns,
    "--call-compact-rows": responsiveGrid.compactLandscape.rows,
  } as CSSProperties;
  const timeline = <ConversationTimeline
    messages={messages}
    attachments={attachments}
    progress={attachmentProgress}
    connected={chatStatus === "ready"}
    localPeerId={chat.localPeerId()}
    typingUsers={typingUsers}
    loadBlob={(record) => chat.attachmentBlob(record)}
    onDownload={(record) => attachmentAction((item) => chat.downloadAttachment(item), record)}
    onRequest={(record) => attachmentAction((item) => chat.requestAttachment(item), record)}
    onPause={(record) => attachmentAction((item) => chat.pauseAttachment(item), record)}
    onResume={(record) => attachmentAction((item) => chat.resumeAttachment(item), record)}
    onCancel={(record) => attachmentAction((item) => chat.cancelAttachment(item), record)}
    onReply={setReplyingTo}
    onEdit={(message, content) => messageAction(() => chat.editMessage(message.id, content), "Falha ao editar a mensagem.")}
    onDelete={(message) => messageAction(() => chat.deleteMessage(message.id), "Falha ao excluir a mensagem.")}
    onReaction={(message, emoji, active) => messageAction(() => chat.setReaction(message.id, emoji, active), "Falha ao reagir à mensagem.")}
    onPin={(message, pinned) => messageAction(() => chat.setPinned(message.id, pinned), "Falha ao fixar a mensagem.")}
  />;

  return <main className="room call-workspace">
    <header>
      <div className="brand"><BriksMark size={26}/> Briks</div>
      <div><strong>{context?.voiceChannelName ?? "Sala ao vivo"}</strong><span>{context?.groupName ? `${context.groupName} · ` : ""}{roomId}</span></div>
      <div className="call-header-actions">
        <button className="minimize-call" onClick={() => {
          setAudioSettingsOpen(false);
          setSourcePickerOpen(false);
          onMinimize();
        }} title="Voltar para o Briks"><PanelLeft size={17}/><span>Voltar ao menu</span></button>
        <button className="minimize-call settings-shortcut" onClick={() => setAppSettingsOpen(true)} title="Abrir as configurações do Briks" aria-label="Configurações"><Settings size={17}/><span>Configurações</span></button>
        {view === "call" && <div className="call-view-tabs stage-mode-tabs" role="group" aria-label="Modo da chamada">
          <button className={!streaming ? "active" : ""} onClick={() => setStageMode("call")} title="Só as pessoas, sem exibir transmissões"><span>Call</span></button>
          <button className={streaming ? "active" : ""} onClick={() => { if (liveCount > 0) setStageMode("stream"); }} disabled={liveCount === 0} title={liveCount > 0 ? "Assistir às transmissões" : "Ninguém está transmitindo"}><MonitorUp size={16}/><span>Transmissão{liveCount > 0 ? ` · ${liveCount}` : ""}</span></button>
        </div>}
        {view === "call" && streaming && <div className="call-view-tabs layout-tabs" role="group" aria-label="Disposição das telas">
          <button className={layoutMode === "weight" ? "active" : ""} onClick={() => chooseLayout("weight")} title="Grade: lives grandes e pessoas pequenas"><LayoutGrid size={16}/><span>Grade</span></button>
          <button className={layoutMode === "focus" ? "active" : ""} onClick={() => chooseLayout("focus")} title="Foco: uma live grande e miniaturas ao lado"><PanelRight size={16}/><span>Foco</span></button>
        </div>}
        <div className="call-view-tabs">
          <button className={view === "call" ? "active" : ""} onClick={() => setView("call")}><Video size={16}/><span>Chamada</span></button>
          <button className={view === "chat" ? "active" : ""} onClick={() => setView("chat")} disabled={!context?.textChannelId}><MessageCircle size={16}/><span>Chat</span></button>
        </div>
        <button
          className={`network-header-button network-${networkQuality.toLocaleLowerCase()}`}
          title="Abrir diagnóstico da conexão"
          onClick={() => { void call.getLiveDiagnostics().then((value) => { setDiagnostics(value); setDiagnosticsOpen(true); }); }}
        ><Activity size={17}/><span>Rede</span><small>{networkQuality} · {peers.length + 1}</small></button>
      </div>
    </header>

    {callError && <div className="global-error" onClick={() => setError(null)}>{callError}</div>}

    <section className={`call-view ${view === "call" ? "" : "is-hidden"}`}>
      {!streaming && liveCount > 0 && <div className="call-stage-banner"><span>{broadcasterText}</span><button type="button" onClick={() => setStageMode("stream")}>Assistir</button></div>}
      <section ref={stageRef} className={`stage ${focused ? "stage-focused" : ""} ${weighted ? "stage-weighted" : ""} ${livesOnly ? `stage-lives-only lives-${liveTileIds.length}` : ""}`} style={stageStyle} onMouseDown={(event) => {
        if (event.target === event.currentTarget) setFocusedTile(null);
      }}>
        {!localPreviews.camera && !(sharingScreen && streaming) && <LocalProfileTile
          displayName={context?.displayName ?? "Você"}
          avatar={context?.avatar}
          microphone={localState.microphone}
          microphoneStream={localPreviews.microphone}
          tileId="local-profile"
          focused={focused === "local-profile"}
          compact={Boolean(focused && focused !== "local-profile")}
          onFocus={(id) => setFocusedTile((current) => current === id ? null : id)}
        />}
        {localPreviews.camera && <LocalVideoTile
          stream={localPreviews.camera}
          tileId="local-camera"
          label="Câmera"
          mirrored
          microphone={localState.microphone}
          microphoneStream={localPreviews.microphone}
          focused={focused === "local-camera"}
          compact={Boolean(focused && focused !== "local-camera")}
          onFocus={(id) => setFocusedTile((current) => current === id ? null : id)}
        />}
        {streaming && localPreviews.screen && <LocalVideoTile
          stream={localPreviews.screen}
          tileId="local-screen"
          label={`Tela · ${localState.screenAudio ? "áudio: " : ""}${sharedAudioText} · ${qualityProfile.label}`}
          mirrored={false}
          microphone={localState.microphone}
          focused={focused === "local-screen"}
          compact={Boolean(focused && focused !== "local-screen")}
          onFocus={(id) => setFocusedTile((current) => current === id ? null : id)}
        />}
        {streaming && localPreviews.screen2 && <LocalVideoTile
          stream={localPreviews.screen2}
          tileId="local-screen2"
          label="2ª tela · sem áudio"
          mirrored={false}
          microphone={localState.microphone}
          focused={focused === "local-screen2"}
          compact={Boolean(focused && focused !== "local-screen2")}
          onFocus={(id) => setFocusedTile((current) => current === id ? null : id)}
        />}
        {peers.map((participant) => {
          const id = `peer-${participant.peerId}`;
          const secondStream = participant.state.secondScreenStreamId ? participant.streams?.[participant.state.secondScreenStreamId] : undefined;
          const id2 = `peer2-${participant.peerId}`;
          return <React.Fragment key={participant.peerId}>{streaming && secondStream && <RemoteSecondScreenTile participant={participant} stream={secondStream} tileId={id2} focused={focused === id2} compact={Boolean(focused && focused !== id2)} onFocus={(next) => setFocusedTile((current) => current === next ? null : next)}/>}<VideoTile
            key={participant.peerId}
            participant={participant}
            tileId={id}
            focused={focused === id}
            compact={Boolean(focused && focused !== id)}
            deafened={deafened}
            watching={streaming}
            onFocus={(next) => setFocusedTile((current) => current === next ? null : next)}
          /></React.Fragment>;
        })}
      </section>
    </section>

    <section className={`call-chat-view ${view === "chat" ? "" : "is-hidden"}`}>
      <header className="call-chat-header">
        <div><Hash/><span><strong>#{context?.textChannelName ?? "chat"}</strong><small>{context?.groupName ?? "Grupo atual"}</small></span></div>
        <div className={`call-chat-status ${chatStatus}`}><i/>{chatStatus === "ready" ? "P2P conectado" : chatStatus === "connected" ? "Aguardando peers" : chatStatus === "connecting" ? "Conectando" : chatStatus === "incompatible" ? "Versão incompatível" : "Offline"}</div>
        <button onClick={() => setView("call")}><Video size={16}/> Voltar para chamada</button>
      </header>
      <div className="messages call-chat-messages">
        {timeline}
        {!messages.length && !attachments.length && <div className="channel-welcome"><MessageCircle/><h2>Chat da chamada</h2><p>O chat P2P conecta automaticamente enquanto você permanece na sala de voz.</p></div>}
      </div>
      <MessageComposer
        placeholder={context?.textChannelName ? `Conversar em #${context.textChannelName}` : "Chat indisponível"}
        canAttach={chatStatus === "ready"}
        onSubmit={submitMessage}
        onFiles={sendFiles}
        replyingTo={replyingTo}
        onCancelReply={() => setReplyingTo(null)}
        onTyping={(active) => chat.setTyping(active)}
        mentionCandidates={[context?.displayName ?? "", ...peers.map((peer) => peer.displayName)]}
      />
    </section>

    <footer>
      <button className={localState.microphone ? "" : "off"} onClick={() => void call.toggleMicrophone(roomId)}>{localState.microphone ? <Mic/> : <MicOff/>}<span>Microfone</span></button>
      <button className={deafened ? "off" : ""} onClick={() => setDeafened((value) => !value)} title="Silenciar todo o áudio recebido">{deafened ? <VolumeX/> : <Volume2/>}<span>Ensurdecer</span></button>
      <button className={audioSettingsOpen ? "active" : ""} onClick={() => setAudioSettingsOpen((current) => !current)} title="Configurações de áudio"><Settings2/><span>Áudio</span></button>
      <button className={localState.camera ? "active" : ""} onClick={() => void call.toggleCamera(roomId)}>{localState.camera ? <Video/> : <VideoOff/>}<span>Câmera</span></button>
      <div className="share-control">
        <button className={localPreviews.screen ? "active" : ""} onClick={() => void showScreenPicker()}><MonitorUp/><span>{localPreviews.screen ? "Parar transmissão" : "Compartilhar"}</span></button>
        {localPreviews.screen && <button className={localPreviews.screen2 ? "active" : ""} onClick={() => void showScreenPicker(true)} title={localPreviews.screen2 ? "Parar a segunda tela" : "Transmitir outro monitor ao mesmo tempo (sem áudio, até 4 Mbps)"}><MonitorUp/><span>{localPreviews.screen2 ? "Parar 2ª tela" : "+ 2ª tela"}</span></button>}
        <button
          type="button"
          className={`share-audio-toggle${screenAudioEnabled ? " on" : ""}`}
          onClick={() => setScreenAudioEnabled((current) => !current)}
          disabled={localState.screenShare}
          aria-pressed={screenAudioEnabled}
          aria-label="Áudio da transmissão de tela"
          title={localState.screenShare ? `Áudio da transmissão: ${sharedAudioText}. Pare a transmissão para mudar` : screenAudioEnabled ? "O áudio do sistema será transmitido. Clique para transmitir sem áudio" : "Só o vídeo será transmitido. Clique para incluir o áudio"}
        >{screenAudioEnabled ? <Volume2 size={17}/> : <VolumeX size={17}/>}<span>{localState.screenShare ? (localState.screenAudio ? `Áudio: ${sharedAudioText}` : "Sem áudio") : screenAudioEnabled ? "Com áudio" : "Sem áudio"}</span></button>
        <button className={qualityPanelOpen ? "quality-trigger active" : "quality-trigger"} onClick={() => setQualityPanelOpen((current) => !current)} title="Resolução, FPS e bitrate da transmissão" aria-expanded={qualityPanelOpen}>
          {qualityProfile.label}
        </button>
      </div>
      <button className="hangup" onClick={leaveCall}><PhoneOff/><span>Sair</span></button>
    </footer>

    {idleRemaining !== null && <div className="idle-warning" role="alert"><span>Você está há muito tempo sem falar. A chamada será encerrada em {formatRemaining(idleRemaining)}.</span><button type="button" onClick={() => { lastActiveRef.current = Date.now(); setIdleRemaining(null); }}>Continuar</button></div>}
    {appSettingsOpen && <Modal title="Configurações" onClose={() => setAppSettingsOpen(false)}><SettingsView/></Modal>}
    {audioSettingsOpen && <InCallAudioSettings call={call} onClose={() => setAudioSettingsOpen(false)}/>}
    {qualityPanelOpen && <ScreenQualityPanel settings={qualitySettings} viewers={Math.max(1, peers.length)} onChange={setQualitySettings} readDiagnostics={() => call.getLiveDiagnostics()} onClose={() => setQualityPanelOpen(false)}/>} 
    {diagnosticsOpen && <section className="network-diagnostics" role="dialog" aria-label="Diagnóstico da conexão">
      <header><strong>Diagnóstico da conexão</strong><button onClick={() => setDiagnosticsOpen(false)}><X/></button></header>
      <p>Signaling: <b>{diagnostics?.signaling?.status ?? "indisponível"}</b></p>
      <p>Canal Supabase: <b>{diagnostics?.signaling?.channelStatus ?? "indisponível"}</b></p>
      <p>Peers presentes: <b>{diagnostics?.signaling?.presencePeers.length ?? 0}</b></p>
      <p>Transporte: <b>{diagnostics?.transport.label ?? "verificando"}</b></p>
      {diagnostics?.transport.migrationRecommended && <p>
        Mudança de topologia recomendada: <b>{diagnostics.transport.active?.toUpperCase()} → {diagnostics.transport.selected.toUpperCase()}</b>
      </p>}
      {diagnostics?.signaling?.clockSkewMs !== null && diagnostics?.signaling?.clockSkewMs !== undefined && <p>
        Diferença de relógio: <b>{Math.round(Math.abs(diagnostics.signaling.clockSkewMs) / 1_000)}s ({diagnostics.signaling.clockSkewMs > 0 ? "peer adiantado" : "peer atrasado"})</b>
        {diagnostics.signaling.clockSkewRejectedMessages > 0 ? ` · ${diagnostics.signaling.clockSkewRejectedMessages} mensagens rejeitadas` : ""}
      </p>}
      <p>Conectividade: <b>{diagnostics?.connectivity.label ?? "verificando"}</b></p>
      <p>Preferência de rede: <b>{diagnostics ? networkPreferenceLabel(diagnostics.network.preference) : "verificando"}</b></p>
      <p>VPN local: <b>{diagnostics?.network.vpnProviders.length ? diagnostics.network.vpnProviders.map((provider) => provider === "zerotier" ? "ZeroTier" : provider === "tailscale" ? "Tailscale" : provider === "wireguard" ? "WireGuard" : "VPN").join(", ") : "não detectada"}</b></p>
      {(diagnostics?.peerConnections ?? []).map((peer) => {
        const path = peer.selectedConnectionPath;
        const routeLabel = path.kind === "unknown" && ["new", "connecting"].includes(peer.connectionState)
          ? "Conectando..."
          : path.label;
        const candidateTypes = [path.localCandidateType, path.remoteCandidateType].filter(Boolean).join(" → ");
        return <article key={peer.peerId}>
          <strong><i className={`sx-dot health-${peerHealth(peer).level}`} aria-hidden="true"/>{participants[peer.peerId]?.displayName ?? peer.peerId.slice(0, 8)} <em className="peer-health">{peerHealth(peer).label}</em></strong>
          <span>WebRTC {peer.connectionState} · ICE {peer.iceConnectionState}</span>
          <span>{routeLabel}{path.protocol ? ` · ${path.protocol.toLocaleUpperCase()}` : ""}</span>
          {candidateTypes && <small>Candidates {candidateTypes}{path.provider ? ` · VPN ${path.provider}` : ""}</small>}
          <small>RTT {peer.roundTripTimeMs ?? 0} ms · jitter {peer.jitterMs ?? 0} ms · perdas {peer.packetsLost ?? 0}</small>
        </article>;
      })}
      <button onClick={() => void navigator.clipboard.writeText(buildTextReport(diagnostics, (id) => participants[id]?.displayName ?? "Participante"))}><Copy/> Copiar relatório</button>
    </section>}
    {sourcePickerOpen && <ScreenSourcePicker
      sources={screenSources}
      loading={screenSourcesLoading}
      includeAudio={screenAudioEnabled}
      onAudioChange={setScreenAudioEnabled}
      onChoose={(source, audioApps) => void chooseScreen(source, audioApps)}
      onClose={() => setSourcePickerOpen(false)}
    />}
  </main>;
}
