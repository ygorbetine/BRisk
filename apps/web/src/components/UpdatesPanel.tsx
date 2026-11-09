import { useState } from "react";
import { notesFor } from "./release-notes";
import { useUpdateState } from "./useUpdateState";
import "./settings-ui.css";
import "./about-panel.css";

const version = import.meta.env.VITE_BRIKS_BUILD_VERSION ?? "dev";

export function UpdatesPanel() {
  const [checking, setChecking] = useState(false);
  const [note, setNote] = useState("");
  const update = useUpdateState();
  const canCheck = typeof window.desktop?.checkForUpdates === "function";
  const notes = notesFor(version);

  async function check(): Promise<void> {
    setChecking(true);
    setNote("");
    try {
      const outcome = await window.desktop!.checkForUpdates!();
      setNote(
        outcome.status === "available" ? `Versão ${outcome.version} encontrada. Baixando; você será avisado quando estiver pronta.`
          : outcome.status === "current" ? "Você já está na versão mais recente."
          : outcome.status === "disabled" ? (outcome.message ?? "Atualização indisponível nesta execução.")
          : `Não foi possível verificar agora. ${outcome.message ?? ""}`.trim(),
      );
    } catch {
      setNote("Não foi possível verificar agora.");
    } finally {
      setChecking(false);
    }
  }

  const ready = update.phase === "ready";
  const downloading = update.phase === "downloading";

  return <section className="sx-stack about-panel" aria-label="Atualizações">
    <div className={`sx-card${ready ? " accent" : ""}`}>
      {ready ? <>
        <div><strong className="sx-title">Versão {update.version} pronta para instalar</strong><p className="sx-sub">Baixada em segundo plano. Reinicie para instalar.</p></div>
        <button type="button" className="sx-btn primary" onClick={() => void window.desktop?.installUpdate?.()}>Reiniciar e instalar</button>
      </> : downloading ? <>
        <div><strong className="sx-title">Baixando a versão {update.version ?? ""}</strong><p className="sx-sub">{update.percent ?? 0}% concluído. Você será avisado quando estiver pronta.</p></div>
        <div className="sx-meter" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={update.percent ?? 0}><i style={{ width: `${update.percent ?? 0}%` }}/></div>
      </> : <div className="sx-row">
        <div><strong className="sx-title">{note || "O Briks procura atualizações sozinho."}</strong>{!note && <small>Quando uma versão nova estiver pronta, você é avisado aqui, na engrenagem e na tela principal.</small>}</div>
        {canCheck && <button type="button" className="sx-btn" disabled={checking} onClick={() => void check()}>{checking ? "Verificando…" : "Verificar agora"}</button>}
      </div>}
    </div>

    {notes && <div className="sx-card">
      <span className="sx-title">Novidades da {notes.version}</span>
      <ul className="about-notes">{notes.items.map((item) => <li key={item}>{item}</li>)}</ul>
    </div>}

  </section>;
}
