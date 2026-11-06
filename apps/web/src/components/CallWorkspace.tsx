import { BriksMark } from "./BriksMark";
import React, { useEffect, useMemo, useRef, useState, type CSSProperties, type FormEvent, type RefObject } from "react";
import {
  Activity,
  Copy,
  Hash,
  Maximize2,
  MessageCircle,
  Mic,
  MicOff,
  MonitorUp,
  PanelLeft,
  PhoneOff,
  Settings2,
  Sparkles,
  Video,
  VideoOff,
  Volume2,
  VolumeX,
  X,
} from "lucide-react";
import type { ChatMessage } from "../application/contracts";
import { useBriksApplication } from "../application/ApplicationContext";
import type { CallController, CallDiagnostics } from "../call";
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
  onFocus,
}: {
  participant: Participant;
  tileId: string;
  focused: boolean;
  compact: boolean;
  deafened: boolean;
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
  const [userVolume, setUserVolume] = useState(100);
  const [screenVolume, setScreenVolume] = useState(100);
  const videoRef = useRef<HTMLVideoElement>(null);
  const articleRef = useRef<HTMLElement>(null);

  useEffect(() => {
    if (participant.state.screenShare && screenStream) setSource("screen");
    else if (!participant.state.screenShare) setSource("camera");
  }, [participant.state.screenShare, screenStream]);

  const selected = source === "screen" ? screenStream : cameraStream;
  const trackKey = selected?.getVideoTracks().map((track) => track.id).join(":") ?? "";
  useEffect(() => {
    if (videoRef.current) {
      videoRef.current.srcObject = selected ?? null;
      void videoRef.current.play().catch(() => undefined);
    }
  }, [selected, trackKey]);

  const hasVideo = Boolean(selected?.getVideoTracks().length);
  const canSwitch = participant.state.camera && Boolean(cameraStream && screenStream);
  const userHasAudio = Boolean(microphoneStream?.getAudioTracks().some((track) => track.readyState === "live"));
  const screenHasAudio = Boolean(screenStream?.getAudioTracks().some((track) => track.readyState === "live"));
  const speaking = useVoiceActivity(microphoneStream, participant.state.microphone && userHasAudio);

  return <article
    ref={articleRef}
    className={`tile ${source} ${participant.connection === "connected" ? "online" : ""} ${speaking ? "speaking" : ""} ${focused ? "focused" : ""} ${compact ? "thumbnail" : ""}`}
    onClick={(event) => {
      if ((event.target as HTMLElement).closest("button,input,.volume-panel")) return;
      onFocus(tileId);
    }}
    onDoubleClick={(event) => {
      if ((event.target as HTMLElement).closest("button,input,.volume-panel")) return;
      void articleRef.current?.requestFullscreen().catch(() => undefined);
    }}
  >
    <video ref={videoRef} autoPlay playsInline muted className={hasVideo ? source : "hidden-video"}/>
    {!hasVideo && <div className="video-off"><ProfileAvatar displayName={participant.displayName} avatar={participant.avatar} className="call-profile-avatar"/><span>Vídeo desligado</span></div>}
    {userHasAudio && <RemoteAudio stream={microphoneStream!} volume={deafened ? 0 : userVolume}/>}
    {screenHasAudio && <RemoteAudio stream={screenStream!} volume={deafened ? 0 : screenVolume}/>}
    <FullscreenButton target={articleRef}/>
    {canSwitch && <div className="source-switch">
      <button className={source === "camera" ? "selected" : ""} onClick={(event) => { event.stopPropagation(); setSource("camera"); }}><Video size={14}/> Câmera</button>
      <button className={source === "screen" ? "selected" : ""} onClick={(event) => { event.stopPropagation(); setSource("screen"); }}><MonitorUp size={14}/> Tela</button>
    </div>}
    {(userHasAudio || screenHasAudio) && !compact && <div className="volume-panel" onClick={(event) => event.stopPropagation()} onDoubleClick={(event) => event.stopPropagation()} onMouseDown={(event) => event.stopPropagation()} onPointerDown={(event) => { event.stopPropagation(); shieldClicksFromPanel(); }}>
      {userHasAudio && <VolumeControl label="Usuário" value={userVolume} onChange={setUserVolume}/>} 
      {screenHasAudio && <VolumeControl label="Transmissão" value={screenVolume} onChange={setScreenVolume}/>} 
    </div>}
    <div className="tile-label"><span>{participant.displayName}</span>{!participant.state.microphone && <MicOff size={15}/>} {participant.state.screenAudio && !compact && <em>ÁUDIO DA TELA</em>}</div>
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
  return <article
    ref={articleRef}
    className={`tile screen online ${focused ? "focused" : ""} ${compact ? "thumbnail" : ""}`}
    onClick={(event) => {
      if ((event.target as HTMLElement).closest("button")) return;
      onFocus(tileId);
    }}
    onDoubleClick={() => { void articleRef.current?.requestFullscreen().catch(() => undefined); }}
  >
    <video ref={videoRef} autoPlay playsInline muted className="screen"/>
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

  return <article
    ref={articleRef}
    className={`tile local online ${source} ${speaking ? "speaking" : ""} ${focused ? "focused" : ""} ${compact ? "thumbnail" : ""}`}
    onClick={(event) => {
      if ((event.target as HTMLElement).closest("button")) return;
      onFocus(tileId);
    }}
    onDoubleClick={() => { void articleRef.current?.requestFullscreen().catch(() => undefined); }}
  >
    <video ref={videoRef} autoPlay playsInline muted className={`${source}${mirrored ? " mirrored" : ""}`}/>
    <FullscreenButton target={articleRef}/>
    <div className="tile-label"><span>Você · {label}</span>{!microphone && <MicOff size={15}/>} {!compact && <em>PRÉVIA</em>}</div>
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

function ScreenSourcePicker({
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
  onChoose(source: BriksDesktopSource): void;
  onClose(): void;
}) {
  return <div className="screen-picker-backdrop" onMouseDown={onClose}>
    <section className="screen-picker" onMouseDown={(event) => event.stopPropagation()}>
      <header>
        <div><MonitorUp/><span><strong>Compartilhar tela</strong><small>Escolha uma tela ou janela</small></span></div>
        <button
          className={`screen-audio-option ${includeAudio ? "active" : ""}`}
          onClick={() => onAudioChange(!includeAudio)}
          aria-pressed={includeAudio}
          title={includeAudio ? "O áudio do sistema será transmitido" : "Apenas o vídeo da tela será transmitido"}
        >{includeAudio ? <Volume2 size={17}/> : <VolumeX size={17}/>}<span>{includeAudio ? "Com áudio" : "Sem áudio"}</span></button>
        <button className="screen-picker-close" onClick={onClose} aria-label="Fechar"><X/></button>
      </header>
      <p className="screen-picker-note">Janela: só o som dela (sem Discord). Abas do navegador compartilham o som do navegador inteiro.</p>
      {loading ? <div className="screen-picker-loading"><Sparkles/><span>Buscando telas e janelas…</span></div> :
        <div className="screen-source-grid">
          {sources.map((source) => <button key={source.id} className="screen-source" onClick={() => onChoose(source)}>
            <div className="screen-source-preview">{source.thumbnail ? <img src={source.thumbnail} alt=""/> : <MonitorUp/>}</div>
            <span title={source.name}>{source.name}</span>
          </button>)}
          {!sources.length && <div className="screen-picker-empty">Nenhuma tela ou janela foi encontrada.</div>}
        </div>}
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
  const [focusedTile, setFocusedTile] = useState<string | null>(null);
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

  const tileIds = useMemo(() => {
    const ids: string[] = [];
    if (!localPreviews.camera) ids.push("local-profile");
    if (localPreviews.camera) ids.push("local-camera");
    if (localPreviews.screen) ids.push("local-screen");
    if (localPreviews.screen2) ids.push("local-screen2");
    peers.forEach((peer) => {
      ids.push(`peer-${peer.peerId}`);
      if (peer.state.secondScreenStreamId && peer.streams?.[peer.state.secondScreenStreamId]) ids.push(`peer2-${peer.peerId}`);
    });
    return ids;
  }, [localPreviews.camera, localPreviews.screen, localPreviews.screen2, peers]);

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

  async function chooseScreen(source: BriksDesktopSource): Promise<void> {
    setSourcePickerOpen(false);
    try {
      if (pickingSecond) await call.toggleSecondScreen(roomId, source.id, qualityProfile);
      else await call.toggleScreen(roomId, source.id, screenAudioEnabled, qualityProfile);
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

  const focused = focusedTile && tileIds.includes(focusedTile) ? focusedTile : null;
  const responsiveGrid = responsiveCallGrid(tileIds.length);
  const stageStyle = {
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
      <section className={`stage ${focused ? "stage-focused" : ""}`} style={stageStyle} onMouseDown={(event) => {
        if (event.target === event.currentTarget) setFocusedTile(null);
      }}>
        {!localPreviews.camera && <LocalProfileTile
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
        {localPreviews.screen && <LocalVideoTile
          stream={localPreviews.screen}
          tileId="local-screen"
          label={localState.screenAudio ? `Tela · ${qualityProfile.label}` : `Tela · sem áudio · ${qualityProfile.label}`}
          mirrored={false}
          microphone={localState.microphone}
          focused={focused === "local-screen"}
          compact={Boolean(focused && focused !== "local-screen")}
          onFocus={(id) => setFocusedTile((current) => current === id ? null : id)}
        />}
        {localPreviews.screen2 && <LocalVideoTile
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
          return <React.Fragment key={participant.peerId}>{secondStream && <RemoteSecondScreenTile participant={participant} stream={secondStream} tileId={id2} focused={focused === id2} compact={Boolean(focused && focused !== id2)} onFocus={(next) => setFocusedTile((current) => current === next ? null : next)}/>}<VideoTile
            key={participant.peerId}
            participant={participant}
            tileId={id}
            focused={focused === id}
            compact={Boolean(focused && focused !== id)}
            deafened={deafened}
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
        <select
          value={screenAudioEnabled ? "audio" : "silent"}
          onChange={(event) => setScreenAudioEnabled(event.target.value === "audio")}
          disabled={localState.screenShare}
          aria-label="Áudio da transmissão de tela"
          title={localState.screenShare ? "Pare a transmissão para mudar o áudio" : "Escolha se o áudio do sistema será transmitido"}
        >
          <option value="audio">Com áudio</option>
          <option value="silent">Sem áudio</option>
        </select>
        <button className={qualityPanelOpen ? "quality-trigger active" : "quality-trigger"} onClick={() => setQualityPanelOpen((current) => !current)} title="Resolução, FPS e bitrate da transmissão" aria-expanded={qualityPanelOpen}>
          {qualityProfile.label}
        </button>
      </div>
      <button className="hangup" onClick={() => {
        setAudioSettingsOpen(false);
        setRoom(null);
        setCallContext(null);
        void chat.disconnect();
        void call.leave(roomId);
      }}><PhoneOff/><span>Sair</span></button>
    </footer>

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
          <strong>{participants[peer.peerId]?.displayName ?? peer.peerId.slice(0, 8)}</strong>
          <span>WebRTC {peer.connectionState} · ICE {peer.iceConnectionState}</span>
          <span>{routeLabel}{path.protocol ? ` · ${path.protocol.toLocaleUpperCase()}` : ""}</span>
          {candidateTypes && <small>Candidates {candidateTypes}{path.provider ? ` · VPN ${path.provider}` : ""}</small>}
          <small>RTT {peer.roundTripTimeMs ?? 0} ms · jitter {peer.jitterMs ?? 0} ms · perdas {peer.packetsLost ?? 0}</small>
        </article>;
      })}
      <button onClick={() => void navigator.clipboard.writeText(JSON.stringify(diagnostics, null, 2))}><Copy/> Copiar relatório sanitizado</button>
    </section>}
    {sourcePickerOpen && <ScreenSourcePicker
      sources={screenSources}
      loading={screenSourcesLoading}
      includeAudio={screenAudioEnabled}
      onAudioChange={setScreenAudioEnabled}
      onChoose={(source) => void chooseScreen(source)}
      onClose={() => setSourcePickerOpen(false)}
    />}
  </main>;
}
