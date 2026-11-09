import { useEffect, useState, type KeyboardEvent, type MouseEvent } from "react";
import {
  acceleratorFromKey,
  formatAccelerator,
  loadShortcutSettings,
  pttKeyFromKeyboard,
  pttKeyFromMouse,
  saveShortcutSettings,
  SHORTCUT_ACTIONS,
  watchShortcutSettings,
  type ShortcutAction,
  type ShortcutSettings,
} from "../services/input/shortcuts";

type Recording = ShortcutAction | "ptt" | null;

/** Atalhos globais da chamada e push-to-talk. Gravação: clicar no botão e apertar a combinação (Esc cancela). */
export function ShortcutsPanel() {
  const [settings, setSettings] = useState<ShortcutSettings>(loadShortcutSettings);
  const [recording, setRecording] = useState<Recording>(null);
  const [hint, setHint] = useState<string | null>(null);
  useEffect(() => watchShortcutSettings(setSettings), []);
  const update = (next: ShortcutSettings) => setSettings(saveShortcutSettings(next));
  const available = Boolean(window.desktop?.setCallShortcuts);

  function onKeyDown(event: KeyboardEvent<HTMLButtonElement>, target: Recording): void {
    if (recording !== target || !target) return;
    event.preventDefault();
    event.stopPropagation();
    if (event.code === "Escape") { setRecording(null); setHint(null); return; }
    if (target === "ptt") {
      const key = pttKeyFromKeyboard(event);
      if (!key) return;
      update({ ...settings, pushToTalk: { ...settings.pushToTalk, key } });
    } else {
      const accelerator = acceleratorFromKey(event);
      if (!accelerator) { setHint("Use Ctrl, Alt ou Win junto com a tecla (F1–F24 podem ir sozinhas)."); return; }
      const taken = SHORTCUT_ACTIONS.find((action) => action.id !== target && settings.accelerators[action.id] === accelerator);
      if (taken) { setHint(`Esse atalho já está em "${taken.label}".`); return; }
      update({ ...settings, accelerators: { ...settings.accelerators, [target]: accelerator } });
    }
    setRecording(null);
    setHint(null);
  }

  function onMouseDown(event: MouseEvent<HTMLButtonElement>): void {
    if (recording !== "ptt") return;
    const key = pttKeyFromMouse(event.button);
    if (!key) return;
    event.preventDefault();
    update({ ...settings, pushToTalk: { ...settings.pushToTalk, key } });
    setRecording(null);
  }

  return <section className="sx-stack" aria-label="Atalhos">
    <div className="sx-card">
      <span className="sx-title">Atalhos globais</span>
      <small className="sx-note">Funcionam mesmo com o Briks em segundo plano, só enquanto você está numa chamada. Se outro programa já usar a combinação, ela não funciona: escolha outra.</small>
      {SHORTCUT_ACTIONS.map((action, index) => <div key={action.id}>
        {index > 0 && <div className="sx-divider"/>}
        <div className="sx-row">
          <div><strong>{action.label}</strong></div>
          <span className="shortcut-controls">
            <button type="button" className={`shortcut-key${recording === action.id ? " recording" : ""}`} disabled={!available}
              onClick={() => { setRecording(recording === action.id ? null : action.id); setHint(null); }}
              onKeyDown={(event) => onKeyDown(event, action.id)} onBlur={() => recording === action.id && setRecording(null)}>
              {recording === action.id ? "Aperte a combinação…" : formatAccelerator(settings.accelerators[action.id])}
            </button>
            {settings.accelerators[action.id] && <button type="button" className="secondary shortcut-clear" disabled={!available} onClick={() => update({ ...settings, accelerators: { ...settings.accelerators, [action.id]: null } })}>Remover</button>}
          </span>
        </div>
      </div>)}
      {hint && <p className="sx-note" role="status">{hint}</p>}
      {!available && <p className="sx-note">Disponível só no aplicativo instalado.</p>}
    </div>

    <div className={`sx-card${settings.pushToTalk.enabled ? " accent" : ""}`}>
      <label className="sx-row">
        <div><strong className="sx-title">Apertar para falar</strong><small>O microfone fica mudo e só abre enquanto você segura a tecla, mesmo dentro do jogo. A tecla continua funcionando no jogo.</small></div>
        <span className="sx-switch"><input type="checkbox" checked={settings.pushToTalk.enabled} disabled={!available} onChange={(event) => update({ ...settings, pushToTalk: { ...settings.pushToTalk, enabled: event.target.checked } })}/><i/></span>
      </label>
      {settings.pushToTalk.enabled && <>
        <div className="sx-divider"/>
        <div className="sx-row">
          <div><strong>Tecla</strong><small>Teclado, botão do meio ou botões laterais do mouse.</small></div>
          <button type="button" className={`shortcut-key${recording === "ptt" ? " recording" : ""}`}
            onClick={() => setRecording(recording === "ptt" ? null : "ptt")}
            onKeyDown={(event) => onKeyDown(event, "ptt")} onMouseDown={onMouseDown} onBlur={() => recording === "ptt" && setRecording(null)}>
            {recording === "ptt" ? "Aperte a tecla…" : settings.pushToTalk.key?.label ?? "Escolher tecla"}
          </button>
        </div>
        <div className="sx-divider"/>
        <label className="sx-row">
          <div><strong>Atraso ao soltar</strong><small>{settings.pushToTalk.releaseDelayMs} ms, para não cortar o fim da fala.</small></div>
          <input type="range" min={0} max={1000} step={50} value={settings.pushToTalk.releaseDelayMs} onChange={(event) => update({ ...settings, pushToTalk: { ...settings.pushToTalk, releaseDelayMs: Number(event.target.value) } })}/>
        </label>
        {!settings.pushToTalk.key && <p className="sx-note">Escolha uma tecla para o push-to-talk funcionar.</p>}
      </>}
    </div>
  </section>;
}
