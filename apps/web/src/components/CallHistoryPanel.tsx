import { useEffect, useState } from "react";
import { MonitorUp, Phone, Trash2 } from "lucide-react";
import { clearCallHistory, formatDuration, isCallHistoryEnabled, loadCallHistory, setCallHistoryEnabled, watchCallHistory, type CallHistoryEntry } from "../services/call/call-history";

const dayFormat = new Intl.DateTimeFormat("pt-BR", { weekday: "long", day: "2-digit", month: "long" });
const timeFormat = new Intl.DateTimeFormat("pt-BR", { hour: "2-digit", minute: "2-digit" });

function dayLabel(timestamp: number, now = new Date()): string {
  const date = new Date(timestamp);
  const startOfDay = (value: Date) => new Date(value.getFullYear(), value.getMonth(), value.getDate()).getTime();
  const diff = Math.round((startOfDay(now) - startOfDay(date)) / 86_400_000);
  if (diff === 0) return "Hoje";
  if (diff === 1) return "Ontem";
  return dayFormat.format(date);
}

/** Lista local das chamadas, agrupada por dia, com opção de desligar e apagar. */
export function CallHistoryPanel() {
  const [entries, setEntries] = useState<CallHistoryEntry[]>(loadCallHistory);
  const [enabled, setEnabled] = useState(isCallHistoryEnabled);
  useEffect(() => watchCallHistory(() => { setEntries(loadCallHistory()); setEnabled(isCallHistoryEnabled()); }), []);

  const groups: Array<{ label: string; items: CallHistoryEntry[] }> = [];
  for (const entry of entries) {
    const label = dayLabel(entry.startedAt);
    const last = groups[groups.length - 1];
    if (last?.label === label) last.items.push(entry);
    else groups.push({ label, items: [entry] });
  }

  return <div className="call-history">
    <div className="call-history-head">
      <label className="rich-toggle"><input type="checkbox" checked={enabled} onChange={(event) => setCallHistoryEnabled(event.target.checked)}/><span>Guardar histórico neste computador</span></label>
      {entries.length > 0 && <button type="button" className="secondary" onClick={() => { if (window.confirm("Apagar todo o histórico de chamadas deste computador?")) clearCallHistory(); }}><Trash2 size={14}/> Apagar tudo</button>}
    </div>
    <small className="call-history-note">Fica só aqui: o Briks não envia o histórico para nenhum servidor. Guarda data, duração, sala e quem participou; nunca áudio, vídeo ou mensagens.</small>
    {!enabled && <p className="call-history-empty">O histórico está desligado.</p>}
    {enabled && !entries.length && <p className="call-history-empty">Nenhuma chamada registrada ainda.</p>}
    {groups.map((group) => <section key={group.label}>
      <h4>{group.label}</h4>
      <ul>
        {group.items.map((entry) => <li key={entry.id}>
          <span className="call-history-icon"><Phone size={15}/></span>
          <span className="call-history-main">
            <strong>{entry.channelName || "Sala de voz"}{entry.groupName && <em> · {entry.groupName}</em>}</strong>
            <small>{entry.participants.length ? `Com ${entry.participants.slice(0, 4).join(", ")}${entry.participants.length > 4 ? ` e mais ${entry.participants.length - 4}` : ""}` : "Só você"}</small>
          </span>
          {entry.streamed && <span className="call-history-badge" title="Você transmitiu nesta chamada"><MonitorUp size={13}/></span>}
          <span className="call-history-time"><strong>{timeFormat.format(entry.startedAt)}</strong><small>{formatDuration(entry.endedAt - entry.startedAt)}</small></span>
        </li>)}
      </ul>
    </section>)}
  </div>;
}
