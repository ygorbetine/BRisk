import { useEffect, useMemo, useState } from "react";
import { RefreshCw, X } from "lucide-react";
import type { CallController } from "../call";
import {
  loadVoiceVideoSettings,
  saveVoiceVideoSettings,
  type NoiseSuppressionMode,
  type VoiceVideoSettings,
} from "../services/audio/settings";
import "./in-call-audio-settings.css";

type MicrophoneOption = {
  deviceId: string;
  label: string;
};

function microphoneLabel(device: MediaDeviceInfo, index: number): string {
  const label = device.label.trim();
  if (label) return label;
  if (device.deviceId === "default") return "Microfone padrão";
  if (device.deviceId === "communications") return "Dispositivo de comunicação padrão";
  return `Microfone ${index + 1}`;
}

export function InCallAudioSettings({ call, onClose }: { call: CallController; onClose(): void }) {
  const [settings, setSettings] = useState<VoiceVideoSettings>(() => loadVoiceVideoSettings());
  const [microphones, setMicrophones] = useState<MicrophoneOption[]>([]);
  const [busy, setBusy] = useState(false);
  const [loadingDevices, setLoadingDevices] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function refreshMicrophones(requestPermission = false) {
    if (!navigator.mediaDevices?.enumerateDevices) {
      setError("Este ambiente não permite listar dispositivos de áudio.");
      return;
    }
    setLoadingDevices(true);
    let permissionStream: MediaStream | undefined;
    try {
      if (requestPermission) permissionStream = await navigator.mediaDevices.getUserMedia({ audio: true, video: false });
      const devices = await navigator.mediaDevices.enumerateDevices();
      const seen = new Set<string>();
      setMicrophones(devices
        .filter((device) => device.kind === "audioinput" && Boolean(device.deviceId))
        .filter((device) => {
          if (seen.has(device.deviceId)) return false;
          seen.add(device.deviceId);
          return true;
        })
        .map((device, index) => ({ deviceId: device.deviceId, label: microphoneLabel(device, index) })));
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível atualizar os microfones.");
    } finally {
      permissionStream?.getTracks().forEach((track) => track.stop());
      setLoadingDevices(false);
    }
  }

  useEffect(() => {
    void refreshMicrophones(false);
    const mediaDevices = navigator.mediaDevices;
    if (!mediaDevices?.addEventListener) return undefined;
    const onDeviceChange = () => { void refreshMicrophones(false); };
    mediaDevices.addEventListener("devicechange", onDeviceChange);
    return () => mediaDevices.removeEventListener("devicechange", onDeviceChange);
  }, []);

  useEffect(() => {
    const onSettingsChanged = (event: Event) => {
      const detail = (event as CustomEvent<VoiceVideoSettings>).detail;
      if (detail) setSettings(detail);
    };
    window.addEventListener("briks:voice-video-settings", onSettingsChanged);
    return () => window.removeEventListener("briks:voice-video-settings", onSettingsChanged);
  }, []);

  const selectedMicrophoneMissing = useMemo(() => {
    if (!settings.microphoneDeviceId || microphones.length === 0) return false;
    return !microphones.some((device) => device.deviceId === settings.microphoneDeviceId);
  }, [microphones, settings.microphoneDeviceId]);

  async function apply(patch: Partial<VoiceVideoSettings>) {
    if (busy) return;
    const next = { ...settings, ...patch };
    setBusy(true);
    setError(null);
    try {
      await call.updateVoiceInput(next);
      saveVoiceVideoSettings(next);
      setSettings(next);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível aplicar a configuração de áudio.");
    } finally {
      setBusy(false);
    }
  }

  return <section className="in-call-audio-settings" aria-label="Configurações de áudio da chamada">
    <header>
      <div>
        <strong>Áudio da chamada</strong>
        <small>As alterações são aplicadas sem sair da ligação e ficam salvas para as próximas chamadas.</small>
      </div>
      <button type="button" onClick={onClose} aria-label="Fechar configurações de áudio"><X size={17}/></button>
    </header>

    <label>
      <span>Microfone</span>
      <select
        value={settings.microphoneDeviceId}
        disabled={busy}
        onChange={(event) => { void apply({ microphoneDeviceId: event.target.value }); }}
      >
        <option value="">Padrão do sistema</option>
        {selectedMicrophoneMissing && <option value={settings.microphoneDeviceId}>Microfone salvo — indisponível</option>}
        {microphones.map((microphone) => <option key={microphone.deviceId} value={microphone.deviceId}>{microphone.label}</option>)}
      </select>
    </label>

    <button
      type="button"
      className="audio-refresh"
      disabled={busy || loadingDevices}
      onClick={() => { void refreshMicrophones(true); }}
    >
      <RefreshCw size={14}/>{loadingDevices ? "Atualizando…" : "Atualizar microfones"}
    </button>

    <label>
      <span>Supressão de ruído</span>
      <select
        value={settings.noiseSuppression}
        disabled={busy}
        onChange={(event) => { void apply({ noiseSuppression: event.target.value as NoiseSuppressionMode }); }}
      >
        <option value="standard">Padrão do WebRTC</option>
        <option value="rnnoise">RNNoise</option>
        <option value="off">Desativada</option>
      </select>
    </label>

    <label className="audio-toggle">
      <input
        type="checkbox"
        checked={settings.echoCancellation}
        disabled={busy}
        onChange={(event) => { void apply({ echoCancellation: event.target.checked }); }}
      />
      <span><strong>Cancelamento de eco</strong><small>Também é reaplicado sem desconectar a chamada.</small></span>
    </label>

    <div className={`audio-apply-status ${busy ? "busy" : ""}`}>
      {busy ? "Trocando a track de áudio…" : "Configuração atual aplicada e salva"}
    </div>
    {error && <div className="audio-apply-error">{error}</div>}
  </section>;
}
