import { useEffect, useState, type CSSProperties } from "react";
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
  /** No SFU o upload não cresce com os espectadores, mas inclui as camadas menores do simulcast. */
  sfu?: boolean;
  onChange: (settings: ScreenQualitySettings) => void;
  /** Lê o diagnóstico ao vivo para o medidor de envio. */
  readDiagnostics?: () => Promise<{ peerConnections: SendMeterPeer[] }>;
  onClose: () => void;
};

const RESOLUTION_SHORT: Record<ScreenResolution, string> = { "720p": "720p", "900p": "900p", "1080p": "1080p", "1440p": "2K", "2160p": "4K" };
const BITRATE_SHORT: Record<ScreenBitrateMode, string> = { auto: "Automático", high: "Alto", max: "Máximo", custom: "Manual" };
const DEGRADATION_SHORT: Record<ScreenDegradation, string> = { balanced: "Equilibrar", resolution: "Nitidez", framerate: "Fluidez" };
const DEGRADATION_HINT: Record<ScreenDegradation, string> = {
  balanced: "Se a rede piorar, reduz um pouco de tudo.",
  resolution: "Se a rede piorar, mantém a nitidez e cai o FPS.",
  framerate: "Se a rede piorar, mantém a fluidez e cai a resolução.",
};

function Segmented<T extends string | number>({ label, value, options, onChange }: {
  label: string;
  value: T;
  options: Array<{ value: T; text: string; title?: string }>;
  onChange: (value: T) => void;
}) {
  return <div className="sq-field">
    <span className="sq-label" id={`sq-${label}`}>{label}</span>
    <div className="sq-seg" role="group" aria-labelledby={`sq-${label}`}>
      {options.map((option) => <button key={String(option.value)} type="button" title={option.title} aria-pressed={option.value === value} className={option.value === value ? "on" : ""} onClick={() => onChange(option.value)}>{option.text}</button>)}
    </div>
  </div>;
}

export function ScreenQualityPanel({ settings, viewers, sfu = false, onChange, readDiagnostics, onClose }: Props) {
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
  const upload = estimatedUploadBitrate(settings, viewers, sfu);
  const forced = settings.bitrateMode !== "auto";
  const update = (patch: Partial<ScreenQualitySettings>) => onChange({ ...settings, ...patch });

  const customMbps = Math.min(MAX_CUSTOM_MBPS, Math.max(MIN_CUSTOM_MBPS, settings.customMbps));
  const sliderPercent = ((customMbps - MIN_CUSTOM_MBPS) / (MAX_CUSTOM_MBPS - MIN_CUSTOM_MBPS)) * 100;
  const health = meter && meter.peers > 0 ? meter.health : "idle";

  return (
    <section className="screen-quality glass" role="dialog" aria-label="Qualidade da transmissão">
      <header>
        <strong>Qualidade da transmissão</strong>
        <button className="icon-button" onClick={onClose} aria-label="Fechar"><X size={16}/></button>
      </header>

      <div className={`sq-meter ${health}`} aria-label="Medidor de envio" aria-live="polite">
        <div><small>Enviando</small><strong>{meter && meter.peers > 0 ? formatMbps(meter.outboundKbps * 1000) : "—"}</strong></div>
        <div><small>Latência</small><strong>{meter?.worstRttMs !== undefined && meter.peers > 0 ? `${Math.round(meter.worstRttMs)} ms` : "—"}</strong></div>
        <div><small>Perda</small><strong>{meter?.worstLossPercent !== undefined && meter.peers > 0 ? `${meter.worstLossPercent.toFixed(1).replace(".", ",")}%` : "—"}</strong></div>
      </div>
      <p className="sq-budget">
        {sfu
          ? <>Qualidade máxima <b>{formatMbps(bitrate)}</b> · upload total (com camadas menores) <b>≈ {formatMbps(upload)}</b></>
          : <>Por espectador <b>{formatMbps(bitrate)}</b> · upload com {Math.max(1, viewers)} {viewers === 1 ? "espectador" : "espectadores"} <b>≈ {formatMbps(upload)}</b></>}
      </p>

      <div className="sq-presets" role="group" aria-label="Atalhos">
        {SCREEN_QUALITY_PRESETS.map((preset) => {
          const active = Object.entries(preset.settings).every(([key, value]) => settings[key as keyof ScreenQualitySettings] === value);
          const next = { ...settings, ...preset.settings } as ScreenQualitySettings;
          const res = SCREEN_RESOLUTIONS[next.resolution];
          return <button key={preset.id} type="button" aria-pressed={active} className={active ? "active" : ""} onClick={() => update(preset.settings)}>
            <strong>{RESOLUTION_SHORT[next.resolution]} · {next.fps}</strong>
            <small>{res.width}×{res.height} · {formatMbps(resolveScreenBitrate(next))}</small>
          </button>;
        })}
      </div>

      <Segmented label="Resolução" value={settings.resolution} onChange={(resolution) => update({ resolution })}
        options={Object.entries(SCREEN_RESOLUTIONS).map(([value, item]) => ({ value: value as ScreenResolution, text: RESOLUTION_SHORT[value as ScreenResolution], title: `${item.width}×${item.height}` }))}/>
      <Segmented label="Quadros por segundo" value={settings.fps} onChange={(fps) => update({ fps })}
        options={SCREEN_FPS_OPTIONS.map((fps) => ({ value: fps, text: String(fps) }))}/>
      <Segmented label="Bitrate" value={settings.bitrateMode} onChange={(bitrateMode) => update({ bitrateMode })}
        options={Object.entries(SCREEN_BITRATE_MODES).map(([value, item]) => ({ value: value as ScreenBitrateMode, text: BITRATE_SHORT[value as ScreenBitrateMode], title: item.hint }))}/>
      {settings.bitrateMode === "custom" && <div className="sq-custom">
        <input
          type="range"
          aria-label="Bitrate em Mbps"
          min={MIN_CUSTOM_MBPS}
          max={MAX_CUSTOM_MBPS}
          step={0.5}
          value={customMbps}
          style={{ "--sq-fill": `${sliderPercent}%` } as CSSProperties}
          onChange={(event) => update({ customMbps: Number(event.target.value) })}
        />
        <label className="sq-number">
          <input
            type="number"
            aria-label="Mbps"
            min={MIN_CUSTOM_MBPS}
            max={MAX_CUSTOM_MBPS}
            step={0.5}
            value={settings.customMbps}
            onChange={(event) => {
              const value = Number(event.target.value);
              if (Number.isFinite(value)) update({ customMbps: Math.min(MAX_CUSTOM_MBPS, Math.max(MIN_CUSTOM_MBPS, value)) });
            }}
          />
          <span>Mbps</span>
        </label>
      </div>}
      <p className="sq-hint">{SCREEN_BITRATE_MODES[settings.bitrateMode].hint}</p>

      <Segmented label="Se a rede não aguentar" value={settings.degradation} onChange={(degradation) => update({ degradation })}
        options={Object.keys(SCREEN_DEGRADATION_OPTIONS).map((value) => ({ value: value as ScreenDegradation, text: DEGRADATION_SHORT[value as ScreenDegradation], title: SCREEN_DEGRADATION_OPTIONS[value as ScreenDegradation] }))}/>
      <p className="sq-hint">{DEGRADATION_HINT[settings.degradation]}</p>

      {forced && viewers > 1 && <p className="sq-warning">
        Bitrate forçado multiplicado por {viewers} espectadores consome muita banda de envio. Se a imagem travar, volte para Automático.
      </p>}
      {!forced && viewers >= 4 && <p className="sq-hint">
        Com {viewers} espectadores na malha P2P o envio é dividido entre todos e a imagem por pessoa fica mais leve. Para qualidade máxima use o SFU.
      </p>}
      <p className="sq-note">As mudanças valem na hora, com a transmissão ativa.</p>
    </section>
  );
}
