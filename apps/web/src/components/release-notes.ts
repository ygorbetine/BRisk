export type ReleaseNotes = { version: string; items: string[] };

/** Novidades por versão, da mais nova para a mais antiga. A aba Sobre mostra as da versão em uso. */
export const RELEASE_NOTES: ReleaseNotes[] = [
  {
    version: "0.11.4",
    items: [
      "Sobreposição: ao fixar depois de editar, ela volta ao topo do jogo e a sala conectada fica visível",
      "Removido o aviso de reiniciar após baixar a atualização; use o botão Reiniciar e instalar",
    ],
  },
  {
    version: "0.11.3",
    items: [
      "Sobreposição em jogos agora vem ligada por padrão, mostrando a sala e o aviso de mudo mesmo sem transmissão",
      "Novo visual do seletor de tela e janela, com prévias maiores e áudio em um interruptor",
      "Cabeçalho das Configurações alinhado",
    ],
  },
  {
    version: "0.11.2",
    items: [
      "Corrige o app que não abria na 0.11.1 (arquivo da sobreposição faltando no pacote)",
      "Tudo da 0.11.1 continua: sobreposição em jogos, login por @nick, desconectar conta e esqueci minha senha",
    ],
  },
  {
    version: "0.11.1",
    items: [
      "Entrar com @nick (o app lembra o e-mail dos @nicks que já entraram neste computador)",
      "Desconectar conta volta à tela inicial e encerra também a sessão do @nick",
      "Entrada na chamada: sem aviso falso de desconexão e sem entrada duplicada ao clicar duas vezes",
      "Armazenamento e outras abas das Configurações realinhados; aviso de avisos agora fica em Configurações",
      "Botão de áudio da transmissão redesenhado; menu do Alt removido",
      "Sobreposição em jogos: live, quem está na sala, sala conectada e aviso de mudo por cima do jogo, com modo de edição (Ctrl+Shift+O) para mover, redimensionar e ligar ou desligar cada item",
      "Esqueci minha senha na tela de login (contas locais)",
    ],
  },
  {
    version: "0.11.0",
    items: [
      "Configurações reformuladas: Voz e vídeo, Rede, Temas, Armazenamento e Sobre",
      "Temas salvos com nome, na categoria Personalizados",
      "Armazenamento por tipo, com limpeza e exportação dos dados",
      "Teste de microfone com medidor de nível",
      "Aviso na engrenagem quando há atualização pronta",
      "Porta de ruído (noise gate) com limite ajustável, também durante a chamada",
      "Painel Áudio da chamada redesenhado",
      "Miniatura \"Ao vivo\" no painel de voz conectada; participantes e botão Assistir na sala",
      "Amigos com filtros Online, Todos e Pendentes",
      "Perfil e conta num só lugar; central de avisos; erro de login mais claro",
      "Diagnóstico por pessoa com relatório em texto; abas Telas e Janelas ao compartilhar",
      "Chat com suas mensagens à direita e agrupamento por autor",
    ],
  },
  {
    version: "0.10.5",
    items: [
      "Botão Configurações dentro da chamada",
      "Painel de qualidade da transmissão redesenhado",
      "Lives centralizadas e encaixe automático na tela",
    ],
  },
];

export function notesFor(version: string): ReleaseNotes | undefined {
  return RELEASE_NOTES.find((entry) => entry.version === version) ?? RELEASE_NOTES[0];
}
