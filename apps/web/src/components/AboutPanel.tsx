import { useState } from "react";
import { useUpdateState } from "./useUpdateState";

const version = import.meta.env.VITE_BRIKS_BUILD_VERSION ?? "dev";

export function AboutPanel() {
  const [checking, setChecking] = useState(false);
  const [note, setNote] = useState("");
  const update = useUpdateState();
  const canCheck = typeof window.desktop?.checkForUpdates === "function";

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

  return <section className="about-panel" aria-label="Sobre o Briks">
    <h2>Briks</h2>
    <p className="about-version">Versão <strong>{version}</strong></p>
    <p>Chamadas, chat e compartilhamento de tela ponto a ponto, com grupos assinados e @nick.</p>
    <p>O app procura atualizações sozinho e avisa quando uma nova versão estiver pronta para instalar.</p>
    {canCheck && <p><button type="button" className="about-update" disabled={checking} onClick={() => void check()}>{checking ? "Verificando…" : "Verificar atualização agora"}</button></p>}
    {update.phase === "downloading" && <p role="status" className="about-update-note">Baixando a versão {update.version ?? ""}… {update.percent ?? 0}%</p>}
    {update.phase === "ready" && <p role="status" className="about-update-note">
      A versão {update.version} está pronta para instalar. <button type="button" className="about-update" onClick={() => void window.desktop?.installUpdate?.()}>Reiniciar e instalar</button>
    </p>}
    {update.phase !== "downloading" && update.phase !== "ready" && note && <p role="status" className="about-update-note">{note}</p>}
    <p><a href="https://github.com/ygorbetine/BRisk" target="_blank" rel="noreferrer">Código-fonte no GitHub</a> · licença AGPL</p>
  </section>;
}
