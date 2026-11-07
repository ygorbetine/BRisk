import { useState } from "react";

const version = import.meta.env.VITE_BRIKS_BUILD_VERSION ?? "dev";

export function AboutPanel() {
  const [checking, setChecking] = useState(false);
  const [note, setNote] = useState("");
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
    {note && <p role="status" className="about-update-note">{note}</p>}
    <p><a href="https://github.com/ygorbetine/BRisk" target="_blank" rel="noreferrer">Código-fonte no GitHub</a> · licença AGPL</p>
  </section>;
}
