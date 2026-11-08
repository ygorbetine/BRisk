import { useEffect, useMemo, useState } from "react";
import {
  loadVoiceVideoSettings,
  saveVoiceVideoSettings,
  type NoiseSuppressionMode,
  type VoiceVideoSettings,
} from "../services/audio/settings";
import { useMicrophoneTest } from "./useMicrophoneTest";
import "./voice-video-settings.css";
import "./settings-ui.css";

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
  const micTest = useMicrophoneTest(settings.microphoneDeviceId);

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

  const noiseModes: Array<{ value: NoiseSuppressionMode; text: string; hint: string }> = [
    { value: "standard", text: "Padrão", hint: "O mais compatível. Recomendado." },
    { value: "rnnoise", text: "RNNoise", hint: "Experimental. Processa a voz no seu computador e volta ao padrão se não conseguir iniciar." },
    { value: "off", text: "Desligada", hint: "Sem filtro algum." },
  ];

  return <div className="sx-stack voice-video-settings">
    <section className="sx-card">
      <div className="sx-row">
        <span className="sx-title">Entrada</span>
        <button type="button" className="sx-btn small" disabled={loadingMicrophones} onClick={() => { void refreshMicrophones(true); }}>
          {loadingMicrophones ? "Atualizando…" : "Atualizar lista"}
        </button>
      </div>
      <select
        className="sx-select"
        aria-label="Microfone"
        value={settings.microphoneDeviceId}
        onChange={(event) => update({ microphoneDeviceId: event.target.value })}
      >
        <option value="">Padrão do sistema</option>
        {selectedMicrophoneMissing && <option value={settings.microphoneDeviceId}>Microfone salvo, indisponível</option>}
        {microphones.map((microphone) => <option key={microphone.deviceId} value={microphone.deviceId}>{microphone.label}</option>)}
      </select>
      <div className="sx-row">
        <div className="sx-meter" style={{ flex: 1 }} role="meter" aria-label="Nível do microfone" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(micTest.level * 100)}><i style={{ width: `${Math.round(micTest.level * 100)}%`, background: micTest.level > 0.85 ? "#e95555" : "#79e58d" }}/></div>
        <button type="button" className={`sx-btn small${micTest.active ? "" : " primary"}`} onClick={() => (micTest.active ? micTest.stop() : void micTest.start())}>{micTest.active ? "Parar" : "Testar"}</button>
      </div>
      <p className="sx-sub">{micTest.active ? "Fale algo: a barra mostra o que o Briks está ouvindo." : "Se o dispositivo salvo estiver desconectado, o Briks usa o microfone padrão."}</p>
      {microphoneError && <div className="sx-error">{microphoneError}</div>}
      {micTest.error && <div className="sx-error">{micTest.error}</div>}
    </section>

    <section className="sx-card">
      <span className="sx-title">Supressão de ruído</span>
      <div className="sx-seg" role="group" aria-label="Supressão de ruído">
        {noiseModes.map((mode) => <button key={mode.value} type="button" aria-pressed={settings.noiseSuppression === mode.value} className={settings.noiseSuppression === mode.value ? "on" : ""} onClick={() => update({ noiseSuppression: mode.value })}>{mode.text}</button>)}
      </div>
      <p className="sx-sub">{noiseModes.find((mode) => mode.value === settings.noiseSuppression)?.hint}</p>
    </section>

    <section className="sx-card">
      <label className="sx-row">
        <div><strong>Porta de ruído</strong><small>Silencia o microfone quando o som fica abaixo do limite (teclado, ventilador). Ajuste olhando o medidor.</small></div>
        <span className="sx-switch"><input type="checkbox" checked={settings.noiseGateEnabled} onChange={(event) => update({ noiseGateEnabled: event.target.checked })}/><i/></span>
      </label>
      {settings.noiseGateEnabled && <label className="sx-row">
        <div><strong>Limite</strong><small>{settings.noiseGateThresholdDb} dB — quanto mais alto, mais exigente</small></div>
        <input type="range" min={-70} max={-20} step={1} value={settings.noiseGateThresholdDb} aria-label="Limite da porta de ruído" onChange={(event) => update({ noiseGateThresholdDb: Number(event.target.value) })}/>
      </label>}
    </section>

    <section className="sx-card">
      <label className="sx-row">
        <div><strong>Cancelamento de eco</strong><small>Evita que o áudio dos alto-falantes volte pelo microfone.</small></div>
        <span className="sx-switch"><input type="checkbox" checked={settings.echoCancellation} onChange={(event) => update({ echoCancellation: event.target.checked })}/><i/></span>
      </label>
      <div className="sx-divider"/>
      <label className="sx-row">
        <div><strong>Excluir áudio do Briks da transmissão</strong><small>Ao compartilhar a tela, captura o PC sem retransmitir as vozes tocadas pelo próprio Briks.</small></div>
        <span className="sx-switch"><input type="checkbox" checked={settings.excludeBriksAudioFromScreenShare} onChange={(event) => update({ excludeBriksAudioFromScreenShare: event.target.checked })}/><i/></span>
      </label>
    </section>

    <p className="sx-note" key={savedAt}><b>Salvo automaticamente.</b> Microfone e ruído valem na próxima sala de voz; numa chamada, aplique pelo painel Áudio. O áudio do Briks vale na próxima transmissão.</p>
  </div>;
}
