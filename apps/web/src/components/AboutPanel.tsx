const version = import.meta.env.VITE_BRIKS_BUILD_VERSION ?? "dev";

export function AboutPanel() {
  return <section className="about-panel" aria-label="Sobre o Briks">
    <h2>Briks</h2>
    <p className="about-version">Versão <strong>{version}</strong></p>
    <p>Chamadas, chat e compartilhamento de tela ponto a ponto, com grupos assinados e @nick.</p>
    <p>O app procura atualizações sozinho e avisa quando uma nova versão estiver pronta para instalar.</p>
    <p><a href="https://github.com/ygorbetine/BRisk" target="_blank" rel="noreferrer">Código-fonte no GitHub</a> · licença AGPL</p>
  </section>;
}
