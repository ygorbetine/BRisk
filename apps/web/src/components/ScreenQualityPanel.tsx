import { useEffect, useState } from "react";
import { X } from "lucide-react";
import { summarizeSendMeter, type SendMeter, type SendMeterPeer } from "../services/rtc/send-meter";
import {
  estimatedUploadBitrate,
  formatMbps,
  MAX_CUSTOM_MBPS,
  MIN_CUSTOM_MBPS,
  resolveScreenBitrate,
  SCREEN_BITRATE_MODES,
  SCREEN_DEGRADATION_OPTIONS,
  SCREEN_FPS_OPTIONS,
  SCREEN_QUALITY_PRESETS,
  SCREEN_RESOLUTIONS,
  type ScreenBitrateMode,
  type ScreenDegradation,
  type ScreenFps,
  type ScreenQualitySettings,
  type ScreenResolution,
} from "../services/rtc/screen-quality-settings";
import "./screen-quality.css";

type Props = {
  settings: ScreenQualitySettings;
  viewers: number;
  onChange: (settings: ScreenQualitySettings) => void;
  /** Lê o diagnóstico ao vivo para o medidor de envio. */
  readDiagnostics?: () => Promise<{ peerConnections: SendMeterPeer[] }>;
  onClose: () => void;
};

export function ScreenQualityPanel({ settings, viewers, onChange, readDiagnostics, onClose }: Props) {
  const [meter, setMeter] = useState<SendMeter | null>(null);
  useEffect(() => {
    if (!readDiagnostics) return;
    let alive = true;
    const tick = () => { void readDiagnostics().then((value) => { if (alive) setMeter(summarizeSendMeter(value.peerConnections)); }).catch(() => undefined); };
    tick();
    const timer = window.setInterval(tick, 2000);
    return () => { alive = false; window.clearInterval(timer); };
  }, [readDiagnostics]);
  const bitrate = resolveScreenBitrate(settings);
  const upload = estimatedUploadBitrate(settings, viewers);
  const forced = settings.bitrateMode !== "auto";
  const update = (patch: Partial<ScreenQualitySettings>) => onChange({ ...settings, ...patch });

  return (
    <section className="screen-quality glass" role="dialog" aria-label="Qualidade da transmissão">
      <header>
        <strong>Qualidade da transmissão</strong>
        <button className="icon-button" onClick={onClose} aria-label="Fechar"><X size={16}/></button>
      </header>

      <div className="sq-presets" role="group" aria-label="Atalhos">
        {SCREEN_QUALITY_PRESETS.map((preset) => {
          const active = Object.entries(preset.settings).every(([key, value]) => settings[key as keyof ScreenQualitySettings] === value);
          return <button key={preset.id} className={active ? "active" : ""} onClick={() => update(preset.settings)}>{preset.label}</button>;
        })}
      </div>

      <label>
        <span>Resolução</span>
        <select value={settings.resolution} onChange={(event) => update({ resolution: event.target.value as ScreenResolution })}>
          {Object.entries(SCREEN_RESOLUTIONS).map(([value, item]) => <option key={value} value={value}>{item.label}</option>)}
        </select>
      </label>

      <label>
        <span>Taxa de quadros</span>
        <select value={settings.fps} onChange={(event) => update({ fps: Number(event.target.value) as ScreenFps })}>
          {SCREEN_FPS_OPTIONS.map((fps) => <option key={fps} value={fps}>{fps} FPS</option>)}
        </select>
      </label>

      <label>
        <span>Bitrate</span>
        <select value={settings.bitrateMode} onChange={(event) => update({ bitrateMode: event.target.value as ScreenBitrateMode })}>
          {Object.entries(SCREEN_BITRATE_MODES).map(([value, item]) => <option key={value} value={value}>{item.label}</option>)}
        </select>
      </label>
      {settings.bitrateMode === "custom" && <label>
        <span>Mbps</span>
        <input
          type="number"
          min={MIN_CUSTOM_MBPS}
          max={MAX_CUSTOM_MBPS}
          step={0.5}
          value={settings.customMbps}
          onChange={(event) => {
            const value = Number(event.target.value);
            if (Number.isFinite(value)) update({ customMbps: Math.min(MAX_CUSTOM_MBPS, Math.max(MIN_CUSTOM_MBPS, value)) });
          }}
        />
      </label>}
      <p className="sq-hint">{SCREEN_BITRATE_MODES[settings.bitrateMode].hint}</p>

      <label>
        <span>Se a rede não aguentar</span>
        <select value={settings.degradation} onChange={(event) => update({ degradation: event.target.value as ScreenDegradation })}>
          {Object.entries(SCREEN_DEGRADATION_OPTIONS).map(([value, label]) => <option key={value} value={value}>{label}</option>)}
        </select>
      </label>

      <footer>
        <div><small>Por espectador</small><strong>{formatMbps(bitrate)}</strong></div>
        <div><small>Upload com {Math.max(1, viewers)} {viewers === 1 ? "espectador" : "espectadores"}</small><strong>≈ {formatMbps(upload)}</strong></div>
      </footer>
      {forced && viewers > 1 && <p className="sq-warning">
        Bitrate forçado multiplicado por {viewers} espectadores consome muita banda de envio. Se a imagem travar, volte para Automático.
      </p>}
      {!forced && viewers >= 4 && <p className="sq-hint">
        Com {viewers} espectadores na malha P2P o envio é dividido entre todos e a imagem por pessoa fica mais leve. Para qualidade máxima use o SFU.
      </p>}
      {meter && meter.peers > 0 && <div className={`sq-meter ${meter.health}`} aria-label="Medidor de envio">
        <div><small>Enviando</small><strong>{formatMbps(meter.outboundKbps * 1000)}</strong></div>
        <div><small>Latência</small><strong>{meter.worstRttMs !== undefined ? `${Math.round(meter.worstRttMs)} ms` : "—"}</strong></div>
        <div><small>Perda</small><strong>{meter.worstLossPercent !== undefined ? `${meter.worstLossPercent.toFixed(1).replace(".", ",")}%` : "—"}</strong></div>
      </div>}
      <p className="sq-note">Vale na hora, com a transmissão ativa.</p>
    </section>
  );
}
