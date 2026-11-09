import "./settings-ui.css";
import "./about-panel.css";

const version = import.meta.env.VITE_BRIKS_BUILD_VERSION ?? "dev";

export function AboutPanel() {
  return <section className="sx-stack about-panel" aria-label="Sobre o Briks">
    <div className="about-hero">
      <span className="about-logo" aria-hidden="true">B</span>
      <div>
        <strong className="about-name">Briks</strong>
        <span className="sx-label">Versão {version}</span>
      </div>
    </div>
    <p className="sx-sub">Chamadas, chat e compartilhamento de tela ponto a ponto, com grupos assinados e @nick.</p>

    <footer className="about-footer">
      <span>Feito por Becine Tools</span>
      <a href="https://github.com/ygorbetine/Briks" target="_blank" rel="noreferrer">Código-fonte</a>
      <span>Licença AGPL</span>
    </footer>
  </section>;
}
