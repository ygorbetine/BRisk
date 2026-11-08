import { useCallback, useEffect, useState } from "react";
import { Download, Lock } from "lucide-react";
import {
  breakdownShares,
  clearLocalAttachments,
  clearLocalMessages,
  downloadLocalDataExport,
  measureLocalData,
  type StorageBreakdown,
} from "../services/offline/storage-tools";
import "./settings-ui.css";
import "./local-storage-panel.css";

type PendingAction = "attachments" | "messages" | null;

export function LocalStoragePanel() {
  const [usage, setUsage] = useState<number>();
  const [quota, setQuota] = useState<number>();
  const [breakdown, setBreakdown] = useState<StorageBreakdown>();
  const [pending, setPending] = useState<PendingAction>(null);
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<{ kind: "ok" | "error"; text: string } | null>(null);

  const refresh = useCallback(async () => {
    try {
      const estimate = await navigator.storage?.estimate();
      setUsage(estimate?.usage);
      setQuota(estimate?.quota);
    } catch { /* o navegador pode não expor a estimativa */ }
    try { setBreakdown(await measureLocalData()); } catch { setBreakdown(undefined); }
  }, []);

  useEffect(() => { void refresh(); }, [refresh]);

  async function run(action: () => Promise<void>, done: string): Promise<void> {
    setBusy(true);
    setMessage(null);
    try {
      await action();
      setMessage({ kind: "ok", text: done });
      await refresh();
    } catch (cause) {
      setMessage({ kind: "error", text: cause instanceof Error ? cause.message : "Não foi possível concluir a ação." });
    } finally {
      setBusy(false);
      setPending(null);
    }
  }

  const shares = breakdown ? breakdownShares(breakdown) : undefined;
  const rows: Array<{ key: keyof StorageBreakdown; label: string; color: string }> = [
    { key: "messages", label: "Mensagens", color: "var(--briks-c3)" },
    { key: "groups", label: "Grupos", color: "var(--briks-c2)" },
    { key: "attachments", label: "Anexos", color: "#ff7aa8" },
  ];

  return <section className="sx-stack local-storage-panel" aria-label="Armazenamento local">
    <div className="sx-card">
      <div className="sx-row" style={{ alignItems: "baseline" }}>
        <strong className="storage-total">{formatBytes(usage)}</strong>
        <span className="sx-label">{quota !== undefined ? `de ${formatBytes(quota)} disponíveis para o Briks` : "em uso"}</span>
      </div>
      <div className="storage-bar" role="img" aria-label="Divisão do espaço por tipo">
        {shares && rows.map((row) => <i key={row.key} style={{ width: `${Math.max(shares[row.key], shares[row.key] > 0 ? 1.5 : 0)}%`, background: row.color }}/>)}
      </div>
      <ul className="storage-legend">
        {rows.map((row) => <li key={row.key}>
          <span><i className="sx-dot" style={{ background: row.color }}/>{row.label}</span>
          <strong>{breakdown ? formatBytes(breakdown[row.key]) : "Calculando…"}</strong>
        </li>)}
      </ul>
      <p className="sx-sub">Mensagens e grupos são estimados pelo tamanho dos registros; anexos, pelo tamanho dos arquivos.</p>
    </div>

    <div className="sx-card">
      <span className="sx-title">Limpeza</span>
      <div className="sx-row">
        <div><strong>Limpar anexos</strong><small>Remove as cópias dos arquivos deste computador{breakdown ? ` e libera ${formatBytes(breakdown.attachments)}` : ""}. As mensagens continuam; dá para pedir o arquivo de novo.</small></div>
        {pending === "attachments"
          ? <span className="storage-confirm"><button type="button" className="sx-btn danger small" disabled={busy} onClick={() => void run(clearLocalAttachments, "Anexos removidos deste computador.")}>Confirmar</button><button type="button" className="sx-btn small" disabled={busy} onClick={() => setPending(null)}>Cancelar</button></span>
          : <button type="button" className="sx-btn small" disabled={busy || breakdown?.attachments === 0} onClick={() => setPending("attachments")}>Limpar</button>}
      </div>
      <div className="sx-divider"/>
      <div className="sx-row">
        <div><strong>Apagar histórico de mensagens</strong><small>Remove as conversas deste computador. Grupos, amigos e a sua conta ficam. Não dá para desfazer; exporte antes se quiser guardar.</small></div>
        {pending === "messages"
          ? <span className="storage-confirm"><button type="button" className="sx-btn danger small" disabled={busy} onClick={() => void run(clearLocalMessages, "Histórico de mensagens apagado.")}>Apagar tudo</button><button type="button" className="sx-btn small" disabled={busy} onClick={() => setPending(null)}>Cancelar</button></span>
          : <button type="button" className="sx-btn danger small" disabled={busy} onClick={() => setPending("messages")}>Apagar</button>}
      </div>
    </div>

    <div className="sx-card">
      <div className="sx-row">
        <div><strong>Exportar meus dados</strong><small>Salva um arquivo com mensagens, grupos e amigos. Não inclui senha, chaves nem o conteúdo dos anexos.</small></div>
        <button type="button" className="sx-btn small primary" disabled={busy} onClick={() => void run(downloadLocalDataExport, "Arquivo gerado. Veja a pasta de downloads.")}><Download size={14}/>Exportar</button>
      </div>
    </div>

    {message && <div className={message.kind === "ok" ? "sx-note" : "sx-error"} role="status">{message.text}</div>}

    <div className="sx-card storage-privacy">
      <Lock size={20} aria-hidden="true"/>
      <strong>Tudo fica neste dispositivo</strong>
      <p className="sx-sub">Mensagens, grupos e anexos permanecem somente neste computador.</p>
    </div>
  </section>;
}

function formatBytes(value?: number): string {
  if (value === undefined) return "Calculando…";
  if (value < 1024) return `${Math.round(value)} B`;
  if (value < 1024 ** 2) return `${Math.round(value / 1024)} KiB`;
  if (value < 1024 ** 3) return `${(value / 1024 ** 2).toFixed(1)} MiB`;
  return `${(value / 1024 ** 3).toFixed(1)} GiB`;
}
