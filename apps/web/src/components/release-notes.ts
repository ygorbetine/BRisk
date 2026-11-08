export type ReleaseNotes = { version: string; items: string[] };

/** Novidades por versão, da mais nova para a mais antiga. A aba Sobre mostra as da versão em uso. */
export const RELEASE_NOTES: ReleaseNotes[] = [
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
