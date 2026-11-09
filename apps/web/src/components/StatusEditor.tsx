import { useRef, useState, type FormEvent } from "react";
import { Smile } from "lucide-react";
import { buildStatus, cleanStatusEmoji, STATUS_MAX_LENGTH, STATUS_PRESETS, type CustomStatus, type StatusExpiry } from "../services/profile/status";

const EXPIRY_LABELS: Record<StatusExpiry, string> = { "1h": "1 hora", "4h": "4 horas", today: "Hoje", never: "Não limpar" };

export function StatusEditor({ status, onChange }: { status: CustomStatus | null; onChange(status: CustomStatus | null): void }) {
  const [emoji, setEmoji] = useState(status?.emoji ?? "");
  const [text, setText] = useState(status?.text ?? "");
  const [expiry, setExpiry] = useState<StatusExpiry>("4h");
  const [saved, setSaved] = useState(false);
  const input = useRef<HTMLInputElement>(null);

  // Abre o teclado de emojis do sistema com o campo de texto em foco; o emoji digitado vai para o campo.
  async function openEmojiKeyboard(): Promise<void> {
    input.current?.focus();
    const opened = await window.desktop?.showEmojiPanel?.().catch(() => false);
    if (!opened) setEmoji((current) => current || "🙂");
  }

  // Emojis escolhidos no teclado do sistema entram no campo de texto: o primeiro vira o ícone do status.
  function changeText(value: string): void {
    const first = [...value][0] ?? "";
    const leading = cleanStatusEmoji(first);
    if (leading) {
      setEmoji(leading);
      setText(value.slice(first.length).trimStart());
    } else setText(value);
    setSaved(false);
  }

  function submit(event: FormEvent): void {
    event.preventDefault();
    onChange(buildStatus(emoji, text, expiry));
    setSaved(true);
  }

  function clear(): void {
    setEmoji("");
    setText("");
    onChange(null);
    setSaved(false);
  }

  return <form className="status-editor" onSubmit={submit}>
    <label className="profile-name-field">Status personalizado
      <span className="status-input">
        <button type="button" className="status-emoji-btn" onClick={() => void openEmojiKeyboard()} title="Escolher emoji" aria-label="Escolher emoji">{emoji || <Smile size={18}/>}</button>
        <input ref={input} value={text} maxLength={STATUS_MAX_LENGTH * 2} onChange={(event) => changeText(event.target.value)} placeholder="O que você está fazendo?"/>
        <em>{[...text].length}/{STATUS_MAX_LENGTH}</em>
      </span>
    </label>
    <div className="status-chips">{STATUS_PRESETS.map((preset) => <button type="button" key={preset.text} onClick={() => { setEmoji(preset.emoji); setText(preset.text); setSaved(false); }}>{preset.emoji} {preset.text}</button>)}</div>
    <label className="profile-name-field">Limpar depois de
      <select value={expiry} onChange={(event) => setExpiry(event.target.value as StatusExpiry)}>
        {(Object.keys(EXPIRY_LABELS) as StatusExpiry[]).map((key) => <option key={key} value={key}>{EXPIRY_LABELS[key]}</option>)}
      </select>
    </label>
    <small>Seus amigos veem esse texto no seu perfil, na lista de amigos e na chamada.</small>
    <div className="status-actions">
      <button type="submit">Salvar status</button>
      {status && <button type="button" className="secondary" onClick={clear}>Limpar status</button>}
      {saved && <span className="status-saved" role="status">Salvo</span>}
    </div>
  </form>;
}
