import { useEffect, useMemo, useState } from "react";
import {
  loadVoiceVideoSettings,
  saveVoiceVideoSettings,
  type NoiseSuppressionMode,
  type VoiceVideoSettings,
} from "../services/audio/settings";
import "./voice-video-settings.css";

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

export function VoiceVideoSettingsPanel() {
  const [settings, setSettings] = useState<VoiceVideoSettings>(() => loadVoiceVideoSettings());
  const [microphones, setMicrophones] = useState<MicrophoneOption[]>([]);
  const [loadingMicrophones, setLoadingMicrophones] = useState(false);
  const [microphoneError, setMicrophoneError] = useState<string | null>(null);
  const [savedAt, setSavedAt] = useState<number>(() => Date.now());

  async function refreshMicrophones(requestPermission = false) {
    if (!navigator.mediaDevices?.enumerateDevices) {
      setMicrophoneError("Este ambiente não permite listar dispositivos de áudio.");
      return;
    }
    setLoadingMicrophones(true);
    setMicrophoneError(null);
    let permissionStream: MediaStream | undefined;
    try {
      if (requestPermission) {
        permissionStream = await navigator.mediaDevices.getUserMedia({ audio: true, video: false });
      }
      const devices = await navigator.mediaDevices.enumerateDevices();
      const seen = new Set<string>();
      const options = devices
        .filter((device) => device.kind === "audioinput" && Boolean(device.deviceId))
        .filter((device) => {
          if (seen.has(device.deviceId)) return false;
          seen.add(device.deviceId);
          return true;
        })
        .map((device, index) => ({ deviceId: device.deviceId, label: microphoneLabel(device, index) }));
      setMicrophones(options);
    } catch (error) {
      setMicrophoneError(error instanceof Error ? error.message : "Não foi possível listar os microfones.");
    } finally {
      permissionStream?.getTracks().forEach((track) => track.stop());
      setLoadingMicrophones(false);
    }
  }

  useEffect(() => {
    setSettings(loadVoiceVideoSettings());
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

  function update(patch: Partial<VoiceVideoSettings>) {
    const next = { ...settings, ...patch };
    setSettings(next);
    saveVoiceVideoSettings(next);
    setSavedAt(Date.now());
  }

  return <div className="voice-video-settings">
    <label className="settings-field">
      <span>Microfone</span>
      <select
        value={settings.microphoneDeviceId}
        onChange={(event) => update({ microphoneDeviceId: event.target.value })}
      >
        <option value="">Padrão do sistema</option>
        {selectedMicrophoneMissing && <option value={settings.microphoneDeviceId}>Microfone salvo — indisponível</option>}
        {microphones.map((microphone) => <option key={microphone.deviceId} value={microphone.deviceId}>{microphone.label}</option>)}
      </select>
      <small>Escolha o dispositivo de entrada usado nas chamadas. Funciona no Windows e Linux. Se o dispositivo salvo estiver desconectado, o Briks usa o microfone padrão como fallback.</small>
    </label>

    <button
      type="button"
      className="settings-secondary-button"
      disabled={loadingMicrophones}
      onClick={() => { void refreshMicrophones(true); }}
    >
      {loadingMicrophones ? "Atualizando microfones…" : "Atualizar microfones"}
    </button>
    {microphoneError && <div className="settings-device-error">{microphoneError}</div>}

    <label className="settings-field">
      <span>Supressão de ruído</span>
      <select
        value={settings.noiseSuppression}
        onChange={(event) => update({ noiseSuppression: event.target.value as NoiseSuppressionMode })}
      >
        <option value="standard">Padrão do WebRTC — recomendado</option>
        <option value="rnnoise">RNNoise — experimental</option>
        <option value="off">Desativada</option>
      </select>
      <small>O modo padrão é o mais compatível. RNNoise processa a voz localmente em um AudioWorklet e volta automaticamente ao WebRTC se não conseguir iniciar.</small>
    </label>

    <label className="settings-toggle">
      <input
        type="checkbox"
        checked={settings.echoCancellation}
        onChange={(event) => update({ echoCancellation: event.target.checked })}
      />
      <span><strong>Cancelamento de eco</strong><small>Evita que o áudio dos alto-falantes volte pelo microfone.</small></span>
    </label>

    <label className="settings-toggle">
      <input
        type="checkbox"
        checked={settings.excludeBriksAudioFromScreenShare}
        onChange={(event) => update({ excludeBriksAudioFromScreenShare: event.target.checked })}
      />
      <span><strong>Excluir áudio do Briks da transmissão</strong><small>Ao compartilhar a tela, tenta capturar o PC sem retransmitir as vozes reproduzidas pelo próprio Briks.</small></span>
    </label>

    <div className="settings-note">
      Configurações salvas automaticamente. Mudanças de microfone e anti-noise entram em vigor na próxima entrada em uma sala de voz; durante uma chamada, use o painel Áudio para aplicá-las sem desconectar. A exclusão do áudio do Briks vale no próximo compartilhamento de tela.
      <span className="settings-autosave" key={savedAt}>Salvo automaticamente</span>
    </div>
  </div>;
}
