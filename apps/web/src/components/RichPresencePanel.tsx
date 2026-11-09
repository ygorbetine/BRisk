import { useState, type FormEvent } from "react";
import { X } from "lucide-react";
import type { RichPresenceSettings } from "../services/profile/rich-presence";

/** Opt-in do rich presence: interruptor, ocultar o programa atual e lista de ignorados. */
export function RichPresencePanel({ settings, detected, onChange }: { settings: RichPresenceSettings; detected: string | null; onChange(next: RichPresenceSettings): void }) {
  const [draft, setDraft] = useState("");
  const add = (name: string) => {
    const clean = name.trim().toLowerCase();
    if (!clean || clean.length > 60 || settings.ignored.includes(clean)) return;
    onChange({ ...settings, ignored: [...settings.ignored, clean].slice(0, 100) });
  };
  const submit = (event: FormEvent) => { event.preventDefault(); add(draft.endsWith(".exe") || !draft.trim() ? draft : `${draft}.exe`); setDraft(""); };
  const currentHidden = detected ? settings.ignored.includes(detected) : false;

  return <div className="rich-presence-panel">
    <label className="rich-toggle">
      <input type="checkbox" checked={settings.enabled} onChange={(event) => onChange({ ...settings, enabled: event.target.checked })}/>
      <span>Mostrar aos amigos o programa ou jogo que estou usando</span>
    </label>
    <small>Aparece numa linha própria, abaixo do seu status. O Briks envia só o nome do programa e o ícone, nunca o título da janela.</small>
    {settings.enabled && <>
      {detected && <div className="rich-current">
        <span>Agora: <strong>{detected}</strong>{currentHidden && " (oculto)"}</span>
        {!currentHidden && <button type="button" className="secondary" onClick={() => add(detected)}>Ocultar este programa</button>}
      </div>}
      <form className="rich-ignore-form" onSubmit={submit}>
        <input value={draft} maxLength={60} onChange={(event) => setDraft(event.target.value)} placeholder="Ignorar programa (ex.: chrome.exe)"/>
        <button type="submit" disabled={!draft.trim()}>Adicionar</button>
      </form>
      {settings.ignored.length > 0 && <ul className="rich-ignored">
        {settings.ignored.map((name) => <li key={name}>{name}<button type="button" title={`Voltar a mostrar ${name}`} aria-label={`Voltar a mostrar ${name}`} onClick={() => onChange({ ...settings, ignored: settings.ignored.filter((item) => item !== name) })}><X size={13}/></button></li>)}
      </ul>}
    </>}
  </div>;
}
