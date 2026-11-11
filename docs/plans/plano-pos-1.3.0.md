# Plano de ação pós-1.3.0 (Briks nativo)

Base: `brisk-dev` em db2c4ee (release v1.3.0). Cada frente trabalha no próprio branch/worktree, faz `git merge brisk-dev` antes de começar e de entregar, commits locais pequenos, sem push/tag/release (a publicação é do orquestrador, com ok do usuário). Testes ocultos (`BRIKS_HIDDEN=1`), uma instância, fechar processos, cantos arredondados conferidos. Documentar FEITO/NÃO FEITO com evidência.

## Ondas e donos
**Onda 1 (paralela)**
| Frente | Dono | Escopo |
|---|---|---|
| A. Transmissão | cui-transmissao | adaptação em quedas bruscas/rede ruim (preso a 150 kbps, ~0,4 Mbps), espectador lento sem vídeo (camada/bitrate por espectador), 4+ espectadores, FEC de vídeo, 1080p +120 ms, DPI/múltiplos monitores, tela cheia/DRM reais, Discord tocando, reconexão do emissor |
| B. Social | cui-ui-paridade | Amigos (abas: todos, online, pendentes, bloqueados, adicionar), grupos com canais e cargos/permissões, convites |
| C. Mensagens | cui-mensagens | pré-visualização de links, anexos (envio/recebimento P2P, imagens, arquivos), estados do chat (enviando, entregue, falhou, digitando), fixadas/busca |
| D. Chamada e correções | cui-paineis | painel de chamada completo, Câmera real (captura + envio, ou desligada com clareza), balões de status sobrepostos no layout empilhado, tooltip "Sair" preso, backends das Configurações (ou "em breve" honesto) |
| E. Conta e rede | cui-contas | tela Rede/Diagnóstico com TURN configurável na UI, teste de login real quando possível, foto/nome, recuperação por frase endurecida, indicador "Somente STUN" |

**Onda 2 (depois da 1; sessão nova ou as mesmas)**
- mutar/ouvir amigos individualmente (volume por pessoa);
- desconectar após 1 h sem falar;
- seletor de canal de áudio (isolar o som de um app);
- status personalizado (texto no perfil);
- Modo Jogo (plano já escrito em docs/plans/modo-jogo-e-desempenho.md).

## Integração e lançamento (1.3.1 / 1.4.0)
1. Cada frente entrega "pronto para integrar" (hash, testes, lista honesta).
2. cui-integracao mescla em ordem A, E, D, B, C; build Release, cargo test, smoke, roteiro oculto, instalador -Test (instalar, atualizar, desinstalar sem processo vivo, Electron intacto).
3. Hotfix urgente (se os testes do usuário com a 1.3.0 acharem algo grave) fura a fila: 1.3.1 só com a correção.
4. Publicação: scripts/release-cui.ps1 com ok do usuário no chat; reversão: `gh release delete v<versão> --cleanup-tag --yes`.

## Regras comuns
- Máquina do usuário teve tela azul com NVENC: testes de GPU curtos, lock `A:\BRiskCall\.gpu-test.lock`.
- Nunca capturar o monitor principal; nada de janelas na tela do usuário sem pedido.
- Fora do alcance (usuário testa): 2 PCs reais, TURN/NAT difícil, AMD/Intel, login Google/Discord real, interop.
- Limite de uso do plano: não abrir frentes além destas.
