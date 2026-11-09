import { useState } from "react";
import { isTelemetryEnabled, setTelemetryEnabled } from "../services/telemetry/call-telemetry";
import "./settings-ui.css";
import "./about-panel.css";

const version = import.meta.env.VITE_BRIKS_BUILD_VERSION ?? "dev";

export function AboutPanel() {
  const [telemetry, setTelemetry] = useState(isTelemetryEnabled);
  return <section className="sx-stack about-panel" aria-label="Sobre o Briks">
    <div className="about-hero">
      <span className="about-logo" aria-hidden="true">B</span>
      <div>
        <strong className="about-name">Briks</strong>
        <span className="sx-label">Versão {version}</span>
      </div>
    </div>
    <p className="sx-sub">Chamadas, chat e compartilhamento de tela ponto a ponto, com grupos assinados e @nick.</p>

    <div className="sx-card">
      <label className="sx-row">
        <div>
          <strong>Ajudar a melhorar as chamadas</strong>
          <small>Envia só contadores anônimos por dia: se a conexão deu certo ou falhou e se foi direta ou pelo relay. Nunca quem, com quem, sala, IP ou conteúdo.</small>
        </div>
        <span className="sx-switch"><input type="checkbox" checked={telemetry} onChange={(event) => { setTelemetryEnabled(event.target.checked); setTelemetry(event.target.checked); }}/><i/></span>
      </label>
    </div>

    <footer className="about-footer">
      <span>Feito por Becine Tools</span>
      <a href="https://github.com/ygorbetine/Briks" target="_blank" rel="noreferrer">Código-fonte</a>
      <span>Licença AGPL</span>
    </footer>
  </section>;
}
