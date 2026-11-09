import { useEffect, useMemo, useRef, useState } from "react";
import { observeVoiceActivity } from "../services/audio/voice-activity";
import { loadOverlaySettings, watchOverlaySettings } from "../services/overlay/settings";
import { useCallStore } from "../store";
import { buildOverlayState, pickScreenStream } from "./overlay-model";

const FRAME_WIDTH = 480;
const FRAME_INTERVAL_MS = 100;
const JPEG_QUALITY = 0.6;

/**
 * Liga o app principal à janela de sobreposição (só no aplicativo instalado): abre e fecha a janela,
 * publica quem está na sala e quem fala, e envia quadros da transmissão escolhida.
 */
export function useOverlayBridge(): void {
  const overlay = window.desktop?.overlay;
  const roomId = useCallStore((state) => state.roomId);
  const callContext = useCallStore((state) => state.callContext);
  const participants = useCallStore((state) => state.participants);
  const localState = useCallStore((state) => state.localState);
  const localMicrophone = useCallStore((state) => state.localPreviews.microphone);
  const [settings, setSettings] = useState(loadOverlaySettings);
  const [speaking, setSpeaking] = useState<ReadonlySet<string>>(new Set());
  const [selfSpeaking, setSelfSpeaking] = useState(false);
  const [preferredLive, setPreferredLive] = useState<string | null>(null);

  useEffect(() => watchOverlaySettings(setSettings), []);

  const active = Boolean(overlay) && settings.enabled && Boolean(roomId);

  useEffect(() => {
    if (!overlay) return undefined;
    void overlay.setVisible(active).catch(() => undefined);
    return () => { void overlay.setVisible(false).catch(() => undefined); };
  }, [overlay, active]);

  useEffect(() => {
    if (!overlay) return undefined;
    return overlay.onCommand((command) => {
      if (command.type === "select-live") setPreferredLive(command.peerId);
    });
  }, [overlay]);

  // Quem está falando: um observador por participante com áudio.
  const audioEntries = useMemo(() => Object.values(participants).flatMap((person) => {
    const stream = Object.values(person.streams ?? {}).find((item) => item.getAudioTracks().length > 0 && item.getVideoTracks().length === 0);
    return stream ? [{ peerId: person.peerId, stream }] : [];
  }), [participants]);
  const audioKey = audioEntries.map((entry) => `${entry.peerId}:${entry.stream.id}`).join("|");

  useEffect(() => {
    if (!active) return undefined;
    const stops = audioEntries.map(({ peerId, stream }) => observeVoiceActivity(stream, (isSpeaking) => {
      setSpeaking((current) => {
        if (current.has(peerId) === isSpeaking) return current;
        const next = new Set(current);
        if (isSpeaking) next.add(peerId); else next.delete(peerId);
        return next;
      });
    }));
    return () => { stops.forEach((stop) => stop()); setSpeaking(new Set()); };
    // audioEntries muda de identidade a cada atualização do store; a chave identifica o que mudou de fato.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [active, audioKey]);

  useEffect(() => {
    if (!active || !localMicrophone || !localState.microphone) { setSelfSpeaking(false); return undefined; }
    const stop = observeVoiceActivity(localMicrophone, setSelfSpeaking);
    return () => { stop(); setSelfSpeaking(false); };
  }, [active, localMicrophone, localState.microphone]);

  const state = useMemo(() => buildOverlayState({
    active,
    roomName: callContext?.voiceChannelName ?? "",
    groupName: callContext?.groupName ?? "",
    selfName: callContext?.displayName ?? "Você",
    micOn: localState.microphone,
    selfLive: localState.screenShare,
    selfSpeaking,
    participants: Object.values(participants),
    speaking,
    preferredLive,
  }), [active, callContext, localState.microphone, localState.screenShare, selfSpeaking, participants, speaking, preferredLive]);

  const stateJson = JSON.stringify(state);
  const lastJson = useRef("");
  useEffect(() => {
    if (!overlay || !active || lastJson.current === stateJson) return;
    lastJson.current = stateJson;
    overlay.pushState(JSON.parse(stateJson) as unknown);
  }, [overlay, active, stateJson]);
  useEffect(() => { if (!active) lastJson.current = ""; }, [active]);

  // Quadros da transmissão escolhida (miniatura em JPEG, ~10 por segundo).
  const liveWidgetOn = settings.widgets.live.on;
  const liveStream = useMemo(() => {
    const person = state.liveId ? participants[state.liveId] : undefined;
    return person ? pickScreenStream(person.streams, person.state) : null;
  }, [state.liveId, participants]);

  useEffect(() => {
    if (!overlay || !active || !liveWidgetOn || !liveStream) return undefined;
    const video = document.createElement("video");
    video.muted = true;
    video.playsInline = true;
    video.srcObject = liveStream;
    void video.play().catch(() => undefined);
    const canvas = document.createElement("canvas");
    const context = canvas.getContext("2d");
    let busy = false;
    const timer = window.setInterval(() => {
      if (busy || !context || video.readyState < 2 || !video.videoWidth || !video.videoHeight) return;
      busy = true;
      canvas.width = FRAME_WIDTH;
      canvas.height = Math.max(1, Math.round((FRAME_WIDTH * video.videoHeight) / video.videoWidth));
      context.drawImage(video, 0, 0, canvas.width, canvas.height);
      canvas.toBlob((blob) => {
        if (!blob) { busy = false; return; }
        void blob.arrayBuffer().then((buffer) => overlay.pushFrame(buffer)).catch(() => undefined).finally(() => { busy = false; });
      }, "image/jpeg", JPEG_QUALITY);
    }, FRAME_INTERVAL_MS);
    return () => {
      window.clearInterval(timer);
      video.pause();
      video.srcObject = null;
    };
  }, [overlay, active, liveWidgetOn, liveStream]);
}
