import { useEffect, useState, type CSSProperties } from "react";
import {
  OVERLAY_WIDGETS,
  loadOverlaySettings,
  saveOverlaySettings,
  watchOverlaySettings,
  type OverlaySettings,
  type OverlayWidgetId,
} from "../services/overlay/settings";
import { useCallStore } from "../store";
import { MAX_OVERLAY_LIVES } from "../services/overlay/settings";
import "./settings-ui.css";

export function OverlaySettingsPanel() {
  const [settings, setSettings] = useState<OverlaySettings>(loadOverlaySettings);
  const overlay = window.desktop?.overlay;
  const participants = useCallStore((state) => state.participants);
  const streamers = Object.values(participants).filter((person) => person.state.screenShare);
  useEffect(() => watchOverlaySettings(setSettings), []);

  const update = (next: OverlaySettings) => setSettings(saveOverlaySettings(next));
  const toggleWidget = (id: OverlayWidgetId, on: boolean) => update({ ...settings, widgets: { ...settings.widgets, [id]: { ...settings.widgets[id], on } } });

  return <section className="sx-stack" aria-label="Sobreposição em jogos">
    <div className={`sx-card${settings.enabled ? " accent" : ""}`}>
      <label className="sx-row">
        <div>
          <strong className="sx-title">Sobreposição em jogos</strong>
          <small>Mostra a live e quem está na sala por cima do jogo, sem tirar você dele. Funciona em jogos em janela ou janela sem bordas.</small>
        </div>
        <span className="sx-switch"><input type="checkbox" checked={settings.enabled} disabled={!overlay} onChange={(event) => update({ ...settings, enabled: event.target.checked })}/><i/></span>
      </label>
      {!overlay && <p className="sx-note">Disponível só no aplicativo instalado.</p>}
    </div>

    <div className="sx-card">
      <span className="sx-title">O que aparece</span>
      {OVERLAY_WIDGETS.map((widget, index) => <div key={widget.id}>
        {index > 0 && <div className="sx-divider"/>}
        <label className="sx-row">
          <div><strong>{widget.label}</strong><small>{widget.hint}</small></div>
          <span className="sx-switch"><input type="checkbox" checked={settings.widgets[widget.id].on} onChange={(event) => toggleWidget(widget.id, event.target.checked)}/><i/></span>
        </label>
        {widget.id === "live" && settings.widgets.live.on && <div className="sx-sublist">
          {streamers.length === 0 && <p className="sx-note">Ninguém está transmitindo agora. Quando alguém transmitir na sua chamada, escolha aqui quais lives aparecem (até {MAX_OVERLAY_LIVES} ao mesmo tempo).</p>}
          {streamers.map((person) => <label key={person.peerId} className="sx-row">
            <div><strong>Live de {person.displayName}</strong></div>
            <span className="sx-switch"><input type="checkbox" checked={settings.lives[person.peerId]?.on !== false} onChange={(event) => update({ ...settings, lives: { ...settings.lives, [person.peerId]: { ...(settings.lives[person.peerId] ?? { x: settings.widgets.live.x, y: settings.widgets.live.y, scale: settings.widgets.live.scale }), on: event.target.checked } } })}/><i/></span>
          </label>)}
        </div>}
      </div>)}
      <div className="sx-divider"/>
      <label className="sx-row">
        <div><strong>Opacidade</strong><small>{Math.round(settings.opacity * 100)}%</small></div>
        <input type="range" className="sx-range" style={{ "--fill": `${Math.round(settings.opacity * 100)}%` } as CSSProperties} min={0} max={100} step={1} value={Math.round(settings.opacity * 100)} aria-label="Opacidade da sobreposição" onChange={(event) => update({ ...settings, opacity: Number(event.target.value) / 100 })}/>
      </label>
    </div>

    <div className="sx-card">
      <div className="sx-row">
        <div>
          <strong className="sx-title">Mover e ajustar</strong>
          <small>O modo de edição deixa arrastar qualquer item, mudar o tamanho e ligar ou desligar o que quiser. Atalho em qualquer tela: Ctrl+Shift+O.</small>
        </div>
        <button type="button" className="sx-btn primary" disabled={!overlay || !settings.enabled} onClick={() => void overlay?.setEditing(true)}>Editar posições</button>
      </div>
      <p className="sx-note">Fora do modo de edição, a sobreposição não captura o mouse nem o teclado: os cliques vão direto para o jogo. Jogos em tela cheia exclusiva não aceitam nada por cima: use o modo janela sem bordas.</p>
    </div>
  </section>;
}
