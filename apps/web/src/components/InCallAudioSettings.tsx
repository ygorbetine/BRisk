import { useEffect, useMemo, useState } from "react";
import { X } from "lucide-react";
import type { CallController } from "../call";
import {
  loadVoiceVideoSettings,
  saveVoiceVideoSettings,
  type NoiseSuppressionMode,
  type VoiceVideoSettings,
} from "../services/audio/settings";
import { useMicrophoneTest } from "./useMicrophoneTest";
import "./in-call-audio-settings.css";
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

export function InCallAudioSettings({ call, onClose }: { call: CallController; onClose(): void }) {
  const [settings, setSettings] = useState<VoiceVideoSettings>(() => loadVoiceVideoSettings());
  const [microphones, setMicrophones] = useState<MicrophoneOption[]>([]);
  const [busy, setBusy] = useState(false);
  const [gateDb, setGateDb] = useState(settings.noiseGateThresholdDb);
  const [loadingDevices, setLoadingDevices] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const micTest = useMicrophoneTest(settings.microphoneDeviceId);

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

  const noiseModes: Array<{ value: NoiseSuppressionMode; text: string }> = [
    { value: "standard", text: "Padrão" },
    { value: "rnnoise", text: "RNNoise" },
    { value: "off", text: "Desligada" },
  ];

  return <section className="in-call-audio-settings" aria-label="Configurações de áudio da chamada">
    <header>
      <strong>Áudio da chamada</strong>
      <button type="button" onClick={onClose} aria-label="Fechar configurações de áudio"><X size={16}/></button>
    </header>

    <div className="sx-card">
      <div className="sx-row">
        <span className="sx-title">Entrada</span>
        <button type="button" className="sx-btn small" disabled={busy || loadingDevices} onClick={() => { void refreshMicrophones(true); }}>{loadingDevices ? "Atualizando…" : "Atualizar lista"}</button>
      </div>
      <select className="sx-select" aria-label="Microfone" value={settings.microphoneDeviceId} disabled={busy} onChange={(event) => { void apply({ microphoneDeviceId: event.target.value }); }}>
        <option value="">Padrão do sistema</option>
        {selectedMicrophoneMissing && <option value={settings.microphoneDeviceId}>Microfone salvo, indisponível</option>}
        {microphones.map((microphone) => <option key={microphone.deviceId} value={microphone.deviceId}>{microphone.label}</option>)}
      </select>
      <div className="sx-row">
        <div className="sx-meter" style={{ flex: 1 }} role="meter" aria-label="Nível do microfone" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(micTest.level * 100)}><i style={{ width: `${Math.round(micTest.level * 100)}%`, background: micTest.level > 0.85 ? "#e95555" : "#79e58d" }}/></div>
        <button type="button" className={`sx-btn small${micTest.active ? "" : " primary"}`} onClick={() => (micTest.active ? micTest.stop() : void micTest.start())}>{micTest.active ? "Parar" : "Testar"}</button>
      </div>
      {micTest.error && <div className="sx-error">{micTest.error}</div>}
    </div>

    <div className="sx-card">
      <span className="sx-title">Supressão de ruído</span>
      <div className="sx-seg" role="group" aria-label="Supressão de ruído">
        {noiseModes.map((mode) => <button key={mode.value} type="button" disabled={busy} aria-pressed={settings.noiseSuppression === mode.value} className={settings.noiseSuppression === mode.value ? "on" : ""} onClick={() => { void apply({ noiseSuppression: mode.value }); }}>{mode.text}</button>)}
      </div>
    </div>

    <label className="sx-card sx-row">
      <div><strong>Cancelamento de eco</strong><small>Reaplicado sem desconectar a chamada.</small></div>
      <span className="sx-switch"><input type="checkbox" checked={settings.echoCancellation} disabled={busy} onChange={(event) => { void apply({ echoCancellation: event.target.checked }); }}/><i/></span>
    </label>

    <div className="sx-card">
      <label className="sx-row">
        <div><strong>Porta de ruído</strong><small>Silencia o microfone abaixo do limite, sem desconectar.</small></div>
        <span className="sx-switch"><input type="checkbox" checked={settings.noiseGateEnabled} disabled={busy} onChange={(event) => { void apply({ noiseGateEnabled: event.target.checked }); }}/><i/></span>
      </label>
      {settings.noiseGateEnabled && <label className="sx-row">
        <div><strong>Limite</strong><small>{gateDb} dB</small></div>
        <input type="range" min={-70} max={-20} step={1} value={gateDb} disabled={busy} aria-label="Limite da porta de ruído" onChange={(event) => setGateDb(Number(event.target.value))} onPointerUp={() => { void apply({ noiseGateThresholdDb: gateDb }); }} onKeyUp={() => { void apply({ noiseGateThresholdDb: gateDb }); }}/>
      </label>}
    </div>

    <p className={`sx-note${busy ? " busy" : ""}`} role="status">{busy ? "Aplicando…" : <><b>Aplicado e salvo.</b> Vale também para as próximas chamadas.</>}</p>
    {error && <div className="sx-error">{error}</div>}
  </section>;
}
