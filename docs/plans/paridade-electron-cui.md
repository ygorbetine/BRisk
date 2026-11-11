# Paridade Electron x CUI: inventário e roteiro de teste

Base: `brisk-dev` em db2c4ee (release v1.3.0). Fonte do Electron: branch `backup/electron-pre-cui` (`apps/web/src`, `apps/desktop/src`, versão 1.2.x). Fonte do CUI: `apps/desktop-cui` (`src/*.c`, `briks-core/src/*.rs`).
Este arquivo é gerado a partir de `apps/desktop-cui/scripts/paridade/itens.csv` (a fonte da verdade, uma linha por item) e substitui, para fins de acompanhamento, a lista de lacunas anterior (`cui-ui-paridade-lacunas.md`, que não existe no brisk-dev; ver o arquivo de mesmo nome, que agora aponta para cá). O inventário anterior por feature continua em `auditoria-features-cui.md`.

## Resultado em números

| Classe | Itens |
|---|---|
| PRESENTE-FUNCIONAL | 116 |
| PRESENTE-SEM-EFEITO | 33 |
| AUSENTE | 92 |
| VERIFICAR | 7 |
| **Total** | **248** |

Itens PRESENTE-FUNCIONAL com teste automático oculto: 52 de 116 (os demais dependem de core/GPU/rede real ou de conferência manual e saem como SEM-TESTE no relatório).

Definição das classes: **PRESENTE-FUNCIONAL** = existe e o clique/fluxo produz o efeito; **PRESENTE-SEM-EFEITO** = existe na tela (ou no core) mas não faz nada de verdade (interruptor que só grava, botão desabilitado, texto "em breve", dado que nada consome); **AUSENTE** = não existe; **VERIFICAR** = não consegui concluir.

## Limites e o que não consegui mapear

- **Escopo ampliado (pedido do usuário):** além de Electron x CUI, o inventário traz duas áreas novas: `BAS` (funções básicas de qualquer programa desktop: copiar/colar, atalhos, bandeja, janela, DPI, notificações, arrastar e soltar, acessibilidade, estados de erro/offline) e `LAY` (conferência dos painéis e estados contra o padrão novo: trilho, lista, conversa larga com composer embaixo, hub à direita). Elas saem no relatório e nas lacunas como as demais.
- Funções básicas foram classificadas pelo código do app e da biblioteca CUI (`cui_textedit.c`, `cui_app_win32.c`); só **Esc fecha diálogo** tem teste oculto. Copiar/colar/desfazer não têm teste automático porque o roteiro não injeta Ctrl+tecla e usar a área de transferência real mexeria nos dados do usuário. **Tab/Shift+Tab não é testável pelo roteiro**: o modo roteiro injeta a tecla depois do quadro que processa a navegação por Tab, então a tecla nunca chega; ficou SEM-TESTE (a biblioteca implementa a navegação).
- Nenhum teste cobre IME, múltiplos monitores, DPI diferente ou leitor de tela (exigem hardware/configuração do Windows); BAS-19 e BAS-42 ficaram VERIFICAR.
- `LAY`: a conferência de layout é visual sobre as capturas do próprio roteiro (as capturas ficam na pasta de saída do roteiro, uma por estado). Não tenho o documento original do padrão novo do usuário; usei a descrição "trilho, lista, conversa larga com composer embaixo, hub à direita" e as capturas atuais.
- O app às vezes sai com código 1 logo ao abrir (intermitente, visto em `call-sair` e `bas-esc`, sem causa identificada); o roteiro repete o caso uma vez. Vale investigar: pode ser uma corrida na subida do core.
- O Electron foi lido pelo código (JSX e handlers); não rodei o app antigo (regra: não tocar no Electron instalado). Itens que só existem por comportamento em tempo de execução (ordem de eventos, temporizadores, regras de supressão) podem ter ficado de fora; os que vi nas notas de versão (`release-notes.ts`: 1.0.0 a 1.2.0) entraram.
- Os componentes com linhas muito longas (`HomeView.tsx`, `CallWorkspace.tsx`) foram varridos por rótulos, `title`, `aria-label` e funções; pode haver botão sem rótulo que não apareceu.
- **GRP-22, MSG-17 e TRX-10 ficaram como VERIFICAR**: dependem do core P2P (manifesto de grupo, histórico entre pares, aviso Assistir) e de teste com dois computadores/interop, fora do alcance daqui. O branch `cui-ui-paridade` tem trabalho em grupos (`grupos-p2p-core.md`) que este inventário NÃO inclui (a base é o brisk-dev); reclassificar ao integrar.
- Transmissão (TRX-01 a TRX-09): classificada pelo código; não foi exercitada (sem GPU, sem core). Entrar numa sala, mute por pessoa, expulsar e a barra de qualidade dependem do `briks-core` e não foram abertos pelo roteiro oculto.
- Login Google/Discord, e-mail real, backend remoto e atualização real: não testáveis aqui; classificados pelo código.
- A verificação automática compara capturas do próprio app (pixels mudaram ou não). Ela prova que a ação tem efeito visível, não que o efeito esteja correto em todos os detalhes; o conteúdo correto de cada tela continua sendo conferido olhando as capturas (a pasta de saída guarda todas).
- O app é aberto em modo demo (`BRIKS_DEMO`), com dados fictícios e sem backend; fluxos que dependem de resposta do backend (criar grupo, enviar pedido de amizade) só são exercitados até a abertura do diálogo.

## As 15 lacunas mais importantes (ordem de prioridade)

| # | Itens | Lacuna |
|---|---|---|
| 1 | TRY-01..04 | Bandeja do sistema e "fechar para a bandeja" (a chamada continua com a janela fechada). Hoje fechar encerra o app e derruba a chamada. |
| 2 | MSG-19 | Enviar mensagem pela aba Chat da chamada (hoje só lista, sem campo de texto). |
| 3 | AMI-17 + GRP-16 | Entrar em grupo por código e convidar para o grupo (sem isso grupos entre computadores não funcionam). |
| 4 | GRP-22 | Sincronização do manifesto do grupo, cargos e revogações entre pares (VERIFICAR; depende do core e de 2 PCs). |
| 5 | ATL-01..04 | Atalhos globais (mutar, ensurdecer, transmitir) e apertar para falar: campos salvam e nada registra a tecla. |
| 6 | CFG-05/09/11/13 | Voz e vídeo sem efeito real: escolha de microfone/saída, supressão de ruído, eco, porta de ruído, volumes. |
| 7 | MSG-09 + MSG-10 | Anexos: botão desabilitado e nenhum cartão de anexo (enviar, baixar, pausar, cancelar). |
| 8 | CHM-04 | Câmera: pílula "em breve" desabilitada; sem captura nem vídeo remoto (CHM-24). |
| 9 | NOT-01..03 | Notificações: avisos de amigo online/transmitindo (interruptores sem efeito) e de nova mensagem (inexistente). |
| 10 | AMI-04 | Convite P2P por código (criar, usar, aceitar, recusar). |
| 11 | AMI-08 + AMI-10 | Amigos: abas Online/Todos/Pendentes e indicador online/offline. |
| 12 | GRP-04..14 | Canais (criar/renomear/apagar), personalizar grupo, sair/apagar e gestão de membros e administradores; painel Membros hoje lista amigos, não o grupo (GRP-10). |
| 13 | MSG-14 | Estados da mensagem (enviando, entregue, falhou, reenvio). |
| 14 | CHM-10 + CHM-11 | Disposição Grade/Foco e tela cheia da transmissão. |
| 15 | CHM-14 + CHM-15 + CHM-22 | Volume por pessoa, pedir transmissão, e registro real do histórico de chamadas (a tela existe mas nunca registra). |

### Lacunas mais importantes de funções básicas e de layout

| # | Itens | Lacuna |
|---|---|---|
| B1 | BAS-12/13/20 | Bandeja, fechar para a bandeja e notificações do sistema (mesmo problema do top 1 e 9; sem isso o app só funciona com a janela aberta). |
| B2 | BAS-02 | Menu de clique direito em campos de texto (colar com o mouse, recortar, selecionar tudo): hoje nada acontece ao clicar com o botão direito. |
| B3 | BAS-04 | Selecionar e copiar um trecho de texto numa mensagem (hoje só dá para copiar a mensagem inteira pelo menu). |
| B4 | BAS-16 | Lembrar posição, tamanho, monitor e maximizado da janela (abre sempre em 1280x800). |
| B5 | BAS-15 | Instância única: abrir o app duas vezes cria duas instâncias disputando o banco. |
| B6 | BAS-23 + BAS-24 | Arrastar e soltar arquivos e colar imagem (a biblioteca já entrega o arquivo solto; o app ignora). |
| B7 | BAS-22 | Badge de não lidas na lista, no trilho e na barra de tarefas. |
| B8 | LAY-04 + LAY-05 | Hub à direita no layout padrão e tela Amigos com abas (padrão novo do usuário). |
| B9 | LAY-07 + LAY-06 | Chat da chamada e Histórico no padrão de conversa/painel (hoje texto cru e tela cheia sem trilho). |
| B10 | BAS-32 + BAS-39 + LAY-11 | Banner offline, tamanho de texto ajustável e etiquetas de 10 a 11 px que ficam abaixo do "textos maiores". |
| B11 | BAS-08, BAS-14, BAS-21, BAS-27, BAS-28, BAS-37, BAS-40 | Busca global (Ctrl+K), iniciar com o Windows, sons, tema claro, idioma, licenças de terceiros e leitor de tela: prioridade baixa. |

Presentes e confirmados nas funções básicas: Ctrl+A/C/X/V/Z/Y e seleção por mouse nos campos (biblioteca), Esc fecha diálogos (teste oculto), Enter envia, DPI alto e redimensionar, rolagem por roda/touchpad, acentos ABNT2, estados vazios, mensagens de erro e sair da conta.

Fora do top 15 mas relevantes: TRY-05/06 (instância única, iniciar com o Windows), RDE-01/02/04/06 (modo de chamada e preferência de rede sem efeito, teste de rede, TURN na interface), AMI-06/07 (recusar pedido, desfazer amizade: o core já tem `friends.remove`/`respond`, falta a UI), PRF-03 (foto dos outros), PRF-06 (status não expira), AUT-03 (data de nascimento 18+), CFG-20..22 (limpar anexos, apagar histórico e exportar sem efeito).

## Lacunas: AUSENTE e PRESENTE-SEM-EFEITO (com adaptação ao layout novo)

A implementação segue o design do app novo (vidro, tema por gradiente, cantos arredondados, textos maiores, dock de painéis), não uma cópia do Electron. A coluna "Encaixe no CUI e adaptação ao layout novo" diz em qual painel, aba ou diálogo o item entra e qual padrão visual já existente reutiliza. Ordenado por prioridade (P0 bloqueia a troca do Electron).

| Prio | ID | Área | Item | Classe | No Electron | CUI hoje | Encaixe no CUI e adaptação ao layout novo |
|---|---|---|---|---|---|---|---|
| P0 | AMI-17 | Amigos | Entrar em grupo por convite/código | AUSENTE | views/HomeView.tsx (Entrar em grupo); GroupInvitePanel.tsx | (não existe) | Botão 'Entrar em grupo' no painel Servidores (tile tracejado ao lado de Criar grupo) e na barra lateral; diálogo de vidro com o campo do código |
| P0 | BAS-12 | Funções básicas de desktop | Ícone na bandeja do sistema (clique abre, menu Sair) | AUSENTE | desktop tray.ts | (não existe) | Ver TRY-01: recurso de sistema no main.c com menu nativo mínimo; o estado (em chamada, mutado) aparece só no tooltip, com o mesmo acento do tema |
| P0 | BAS-13 | Funções básicas de desktop | Minimizar e fechar para a bandeja (a chamada continua) | AUSENTE | desktop tray.ts | (não existe): fechar derruba o app e a chamada | Ver TRY-02: interruptor em Configurações (cartão de sistema novo); ao fechar em chamada, aviso no estilo do banner de atualização |
| P0 | BAS-20 | Funções básicas de desktop | Notificações do sistema (nova mensagem, amigo online, transmissão) | AUSENTE | desktop notifications | (não existe); interruptores de Configurações sem efeito | Ver NOT-01..03: toast nativo do Windows com ícone e texto curto; opção por tipo no cartão Notificações; clique abre a conversa |
| P0 | GRP-16 | Grupos, canais e cargos | Convite para o grupo (código) | AUSENTE | components/GroupInvitePanel.tsx | (não existe) | Item 'Convidar' no cabeçalho do painel do grupo; diálogo de vidro com o código e botão Copiar |
| P0 | MSG-19 | Mensagens | Enviar mensagem pela aba Chat da chamada | SEM-EFEITO | components/CallWorkspace.tsx | ui_call.c: lista sem campo de texto | Compositor (msgui_composer) no rodape da aba Chat da chamada, vidro, mesma altura do chat social |
| P0 | TRY-01 | Tray/janela | Ícone na bandeja do sistema | AUSENTE | desktop tray.ts | (não existe) | Sem tela própria: recurso de sistema (Win32) em src/main.c, com a opção correspondente num cartão de Configurações e avisos no estilo do banner de atualizacao |
| P0 | TRY-02 | Tray/janela | Menu da bandeja: Abrir app, Reiniciar app, Fechar o app | AUSENTE | desktop tray.ts | (não existe) | Sem tela própria: recurso de sistema (Win32) em src/main.c, com a opção correspondente num cartão de Configurações e avisos no estilo do banner de atualizacao; menu nativo (TrackPopupMenu) com 3 itens |
| P0 | TRY-03 | Tray/janela | Clique no ícone da bandeja abre a janela | AUSENTE | desktop tray.ts | (não existe) | Sem tela própria: recurso de sistema (Win32) em src/main.c, com a opção correspondente num cartão de Configurações e avisos no estilo do banner de atualizacao |
| P0 | TRY-04 | Tray/janela | Fechar a janela envia para a bandeja e a chamada continua | AUSENTE | desktop window.ts (evento close) | (fechar encerra o app) | Sem tela própria: recurso de sistema (Win32) em src/main.c, com a opção correspondente num cartão de Configurações e avisos no estilo do banner de atualizacao; opção 'Ao fechar' (bandeja/encerrar) na aba Sobre ou em Notificações |
| P1 | AMI-04 | Amigos | Adicionar amigo por código de convite P2P (criar convite, usar código, aceitar, recusar) | AUSENTE | components/P2PInvitePanel.tsx; services/invites/service.ts | (não existe) | Abas 'Por @nick / Por código' no diálogo Adicionar amigo existente (ui_social.c), código em fonte mono 13 px e botão Copiar; sem janela nova |
| P1 | AMI-06 | Amigos | Pedidos pendentes: recusar | SEM-EFEITO | components/P2PInvitePanel.tsx (Recusar) | core aceita friends.respond accept=false, mas a UI só tem Aceitar | Segundo ícone (X, secundario) ao lado do check em cada linha de PEDIDOS (ui_social.c), tooltip Recusar |
| P1 | AMI-07 | Amigos | Desfazer amizade (com confirmacao) | AUSENTE | views/FriendsView.tsx; HomeView.tsx deleteTarget | core tem friends.remove; sem UI | Item 'Desfazer amizade' no menu de contexto da linha do amigo (person_row) + diálogo de confirmacao no estilo Expulsar (ui_call.c) |
| P1 | AMI-08 | Amigos | Abas Online / Todos / Pendentes com contadores | AUSENTE | views/FriendsView.tsx | ui_social.c: uma lista única AMIGOS e PEDIDOS, sem filtro | cui_segmented no topo do painel Amigos (mesmo componente do 'Limpar depois de'), contadores nos rotulos; pendentes sobem para a aba |
| P1 | AMI-10 | Amigos | Indicador online/offline do amigo | AUSENTE | views/HomeView.tsx (title Online/Offline) | ui_social.c só mostra 'Disponivel' no próprio rodape | Bolinha 8 px (B_ONLINE / cinza) no canto do avatar em person_row; presença vem do core |
| P1 | ATL-01 | Atalhos | Atalho global: mutar/desmutar (Ctrl+Shift+M) | SEM-EFEITO | services/input/shortcuts.ts; desktop call-shortcuts.ts | settings.c campo de texto; nada registra o atalho | Já existe; RegisterHotKey no main.c só durante a chamada; o campo vira captura de teclas (cui_input readonly + Gravar) |
| P1 | ATL-02 | Atalhos | Atalho global: ensurdecer (Ctrl+Shift+D) | SEM-EFEITO | idem | idem | Idem ATL-01 |
| P1 | ATL-03 | Atalhos | Atalho global: transmitir/parar | SEM-EFEITO | idem | idem | Idem ATL-01 |
| P1 | ATL-04 | Atalhos | Apertar para falar (tecla, botão do meio/laterais, atraso ao soltar) | SEM-EFEITO | components/ShortcutsPanel.tsx | settings.c; só salvo | Já existe; GetAsyncKeyState no core/UI; captura de tecla e botoes do mouse no campo |
| P1 | BAS-02 | Funções básicas de desktop | Menu de contexto (clique direito) nos campos de texto: Recortar, Copiar, Colar, Selecionar tudo | AUSENTE | Chromium (nativo, sem código próprio) | (não existe): clique direito num campo não faz nada; só as mensagens têm menu | Popover de vidro no ponto do clique, mesmo desenho e componente do menu de mensagem (msgui.c, linhas com ícone, cantos arredondados), com Desfazer, Recortar, Copiar, Colar e Selecionar tudo; entra num wrapper do app sobre cui_input para valer em login, composer e Configurações |
| P1 | BAS-04 | Funções básicas de desktop | Selecionar e copiar trecho de texto dentro das mensagens (arrastar o mouse sobre o balão) | AUSENTE | Chromium (nativo, sem código próprio); MessageList.tsx | (não existe): texto do balão não é selecionável; só "Copiar texto" da mensagem inteira no menu | No balão da mensagem (msgui.c): seleção por arrasto com realce na cor de acento translúcida e Ctrl+C; o menu de contexto ganha "Copiar seleção" quando há trecho marcado. Mantém o menu atual como atalho para copiar tudo |
| P1 | BAS-15 | Funções básicas de desktop | Instância única (abrir de novo traz a janela existente) | AUSENTE | desktop (requestSingleInstanceLock) | (não existe): duas instâncias disputam o banco | Ver TRY-05: mutex nomeado no main.c e foco na janela existente; sem UI própria |
| P1 | BAS-16 | Funções básicas de desktop | Lembrar posição, tamanho, monitor e maximizado da janela | AUSENTE | desktop-lifecycle.ts (só calcula o tamanho inicial pela área de trabalho; não grava) | (não existe): abre sempre 1280x800 no lugar padrão do Windows; layout.json guarda só os painéis | Gravar no layout.json (mesmo arquivo dos painéis) retângulo, monitor e estado maximizado; restaurar com clamp na área visível dos monitores ligados |
| P1 | BAS-22 | Funções básicas de desktop | Badge de não lidas (lista de conversas, rail, ícone da barra de tarefas) | AUSENTE | (não existe no Electron também) | msgui.c só mostra "não lida" dentro de tópicos; nada na lista nem na barra de tarefas | Contador em pílula de acento no item da conversa (lista de amigos e grupos) e no rail; barra de tarefas com ITaskbarList3::SetOverlayIcon; zera ao abrir a conversa |
| P1 | BAS-23 | Funções básicas de desktop | Arrastar e soltar arquivos na janela ou no composer | AUSENTE | components/MessageComposer.tsx (Solte para enviar) | (não existe): a biblioteca já entrega os arquivos soltos (cui_dropped_file) e o app não usa | Ver MSG-09: dropzone de vidro cobrindo o painel de conversa com "Solte para enviar" (borda tracejada de acento, cantos arredondados); também aceita imagem para o avatar do perfil |
| P1 | CFG-05 | Configurações | Escolher microfone | SEM-EFEITO | components/VoiceVideoSettingsPanel.tsx | settings.c lista winmm e grava mic; o core usa o padrão | Já existe (dropdown da aba Voz e vídeo); enviar o dispositivo ao core (áudio.rs) |
| P1 | CFG-07 | Configurações | Testar o microfone fora da chamada | AUSENTE | components/useMicrophoneTest.ts | (o texto da tela diz que 'chega com o áudio nativo') | Botão 'Testar' no cartão ENTRADA, mesma barra de nivel; captura curta no core |
| P1 | CFG-09 | Configurações | Escolher saida de áudio | SEM-EFEITO | components/VoiceVideoSettingsPanel.tsx | settings.c dropdown; só salvo | Já existe; usar o dispositivo na reprodução do core |
| P1 | CFG-11 | Configurações | Supressão de ruído (Padrão/RNNoise/Desligada) | SEM-EFEITO | components/VoiceVideoSettingsPanel.tsx | settings.c segmentado; só salvo | Já existe; processar no core |
| P1 | CFG-13 | Configurações | Cancelamento de eco | SEM-EFEITO | components/VoiceVideoSettingsPanel.tsx | settings.c; só salvo | Já existe; AEC no core |
| P1 | CFG-21 | Configurações | Apagar histórico de mensagens (com confirmacao) | SEM-EFEITO | components/LocalStoragePanel.tsx | settings.c: confirma e não apaga | Diálogo de confirmacao já existe; chamar o backend local para apagar |
| P1 | CHM-04 | Chamada | Câmera (ligar e enviar vídeo) | SEM-EFEITO | components/CallWorkspace.tsx (Câmera) | ui_call.c: pílula desabilitada 'Câmera - em breve' | Pílula nova na barra flutuante de controles da chamada (pill() em ui_call.c, raio 28, vidro forte), com rótulo e estado ativo em violeta; captura e envio no core (Media Foundation + encoder já usados na transmissão) |
| P1 | CHM-10 | Chamada | Disposição Grade / Foco das telas | AUSENTE | components/CallWorkspace.tsx | (não existe) | Aba/segmento no cabeçalho da chamada, ao lado de Call/Transmissão e Chamada/Chat (tab() em ui_call.c) |
| P1 | CHM-11 | Chamada | Tela cheia da transmissão e da chamada | AUSENTE | components/CallWorkspace.tsx (Tela cheia); ipc window:fullscreen | (não existe) | Ícone Maximize no canto do bloco da transmissão e F11/Esc; ao entrar, a barra flutuante some após 3 s sem mouse |
| P1 | CHM-14 | Chamada | Volume por pessoa (slider) lembrado nas próximas chamadas | AUSENTE | services/áudio/person-áudio.ts | só liga/desliga mudo, sem volume e sem memória | Menu de contexto do bloco da pessoa na chamada (pmenu em ui_call.c, cui_menu_item), mais um item com ícone com cui_slider 0-200%; salvar por peerId em settings |
| P1 | CHM-15 | Chamada | Pedir a alguem que transmita (e aceitar/recusar o pedido) | AUSENTE | call/stream-request.ts | (não existe) | Menu de contexto do bloco da pessoa na chamada (pmenu em ui_call.c, cui_menu_item), mais um item com ícone 'Pedir transmissão'; no destino, banner de vidro no topo da chamada com Transmitir / Agora não (no estilo update_banner) |
| P1 | CHM-18 | Chamada | Ajustes de áudio durante a chamada (mic, ruído, eco, porta) | AUSENTE | components/InCallAudioSettings.tsx | (não existe: só Configurações, e sem efeito) | Pílula nova na barra flutuante de controles da chamada (pill() em ui_call.c, raio 28, vidro forte), com rótulo e estado ativo em violeta 'Áudio' abrindo popup de vidro com os controles da aba Voz e vídeo |
| P1 | CHM-22 | Chamada | Registro das chamadas (quando, duracao, sala, quem) | SEM-EFEITO | components/CallHistoryPanel.tsx; services/call/call-history.ts | ui_history.c: só o estado vazio; nada grava chamadas | Lista de linhas de vidro dentro do cartão existente de ui_history.c: avatar, sala, duracao, data, selo 'Você transmitiu' |
| P1 | GRP-04 | Grupos, canais e cargos | Criar canal de texto | AUSENTE | views/HomeView.tsx (Criar canal) | chat.c carrega canais, mas não há UI para criar | Botão + ao lado do título CANAIS DE TEXTO na barra lateral do grupo (section_title), diálogo de vidro com nome e tipo em cui_segmented |
| P1 | GRP-05 | Grupos, canais e cargos | Criar sala de voz do grupo | AUSENTE | views/HomeView.tsx | a sala de voz e uma única 'Sala geral' fixa, fora dos grupos | Mesmo diálogo do canal, tipo Voz; aparece sob SALAS DE VOZ da barra lateral do grupo |
| P1 | GRP-08 | Grupos, canais e cargos | Personalizar grupo (nome e imagem, sincroniza aos membros) | AUSENTE | components/GroupEditor.tsx | (não existe) | Diálogo de vidro 'Personalizar grupo' aberto da engrenagem no topo do painel do grupo; avatar 96 px como no editor de perfil |
| P1 | GRP-09 | Grupos, canais e cargos | Apagar ou sair do grupo | AUSENTE | views/HomeView.tsx (Apagar ou sair do grupo) | (não existe) | Item perigoso (vermelho) no menu de contexto do grupo no painel Servidores + confirmacao |
| P1 | GRP-10 | Grupos, canais e cargos | Lista de membros do grupo com marcas VOCÊ/DONO/Admin/@nick | SEM-EFEITO | views/HomeView.tsx (membros) | ui_social.c panel_members mostra você + seus amigos, não os membros do grupo | Painel Membros (já no dock): trocar a fonte para os membros do grupo ativo, com selos pequenos (pills 10,5 px) DONO/ADMIN/VOCÊ e @nick |
| P1 | GRP-12 | Grupos, canais e cargos | Adicionar membro (amigo) ao grupo | AUSENTE | views/HomeView.tsx (Adicionar membro) | (não existe) | Menu de contexto da linha do membro/amigo e botão + no painel Membros; escolhe amigo em lista de vidro |
| P1 | GRP-13 | Grupos, canais e cargos | Gerenciar membro: tornar/remover administrador | AUSENTE | views/HomeView.tsx (Gerenciar membro) | (não existe) | Menu de contexto da linha no painel Membros (só dono), itens com ícone escudo |
| P1 | GRP-14 | Grupos, canais e cargos | Remover membro do grupo | AUSENTE | views/HomeView.tsx removeSelectedMember | (não existe) | Mesmo menu de contexto, item vermelho + diálogo de confirmacao |
| P1 | GRP-17 | Grupos, canais e cargos | Sala de voz mostra quem está nela, 'Ao vivo' e Assistir | AUSENTE | views/HomeView.tsx | (não existe: Sala geral não mostra participantes) | Avatares empilhados 24 px sob a linha da sala na barra lateral (como a pilha do drag_stack_ui), chip Ao vivo violeta e botão Assistir |
| P1 | LAY-04 | Layout novo (trilho, lista, conversa, hub) | Hub à direita no layout padrão (membros do grupo, sala de voz, presença) | AUSENTE | (padrão novo do usuário) | layout_default (layout.c) cria só trilho, lista e conversa; Membros e Sala de voz existem como painéis, mas só entram pelo botão + do dock e o de Membros lista amigos, não o grupo (GRP-10) | Quarto painel do layout_default, à direita da conversa (largura ~270 px, mesmo vidro e cantos), com abas Membros / Sala de voz / Fixadas; abre sozinho em grupos e fecha em DM; o usuário ainda pode fechar (X do dock) e o estado é salvo no layout.json |
| P1 | LAY-05 | Layout novo (trilho, lista, conversa, hub) | Tela inicial "Amigos" com abas Online/Todos/Pendentes e cartões de amigo | AUSENTE | (padrão novo do usuário) | Amigos abre só um estado vazio "Escolha uma conversa", sem abas nem lista de pedidos (AMI-08) | Painel central em vez do estado vazio: cabeçalho com abas em pílula (padrão do seletor Call/Transmissão da chamada), linhas largas de amigo com ações e a lista de pedidos; hub à direita mostra "Ativos agora" |
| P1 | LAY-07 | Layout novo (trilho, lista, conversa, hub) | Aba Chat da chamada no padrão de conversa (balões, avatares, horário, composer embaixo) | AUSENTE | (padrão novo do usuário) | ui_call.c: texto cru "Nome / frase" sem balão, avatar, hora nem campo de envio (MSG-19) | Reutilizar o renderizador de mensagens do painel de conversa (msgui.c) dentro do cartão da aba Chat e o composer fixo sobre a barra de controles; mesmo vidro e cantos do cartão atual |
| P1 | MSG-09 | Mensagens | Anexar arquivo (botão, arrastar e soltar) | SEM-EFEITO | components/MessageComposer.tsx (Anexar arquivo, Solte para enviar) | ui_social.c: botão clip desabilitado 'Anexar (marco 3)' | Habilitar o clip do compositor (mesmo botão) + overlay de vidro 'Solte para enviar' sobre a área do chat; progresso como chip na bolha |
| P1 | MSG-10 | Mensagens | Cartão de anexo (baixar, pausar, retomar, cancelar, abrir) | AUSENTE | components/AttachmentCard.tsx | (não existe) | Cartão de vidro dentro da bolha (raio 14, ícone do tipo, nome, tamanho, barra de progresso violeta) |
| P1 | MSG-14 | Mensagens | Estados da mensagem: enviando, entregue, falhou e reenvio (outbox) | AUSENTE | chat/OutboxService.ts | (não existe: s->notice 'Não foi possivel gravar') | Ícone de 12 px no canto da bolha própria (relogio, check, alerta); toque no alerta reenvia |
| P1 | NOT-01 | Notificações | Aviso: amigo ficou online | SEM-EFEITO | services/profile/use-friend-notifications.ts | settings.c interruptor; só salvo | Já existe (aba Notificações); toast nativo do Windows + opcional toast de vidro no canto do app |
| P1 | NOT-02 | Notificações | Aviso: amigo comecou a transmitir | SEM-EFEITO | services/profile/use-friend-notifications.ts | settings.c interruptor; só salvo | Idem NOT-01 |
| P1 | NOT-03 | Notificações | Notificação de nova mensagem (DM e grupo) com o app em segundo plano | AUSENTE | services/chat/background-manager.ts | (não existe) | Sem tela própria: recurso de sistema (Win32) em src/main.c, com a opção correspondente num cartão de Configurações e avisos no estilo do banner de atualizacao; texto 'Autor: trecho' como no Electron; toast nativo |
| P1 | PRF-03 | Perfil | Mostrar a foto dos OUTROS em amigos, membros, tiles da chamada e mensagens | AUSENTE | components/ProfileAvatar.tsx | ui_social.c: só a sua foto aparece (user_photo); os demais usam iniciais | cui_avatar_ex com .image também em person_row, tiles (ui_call.c) e bolhas (msgui.c); 32-40 px, circulo, mesma borda; baixar a foto do amigo pelo core |
| P1 | RDE-01 | Rede | Modo da chamada (Automático, Sempre P2P, Sempre SFU) | SEM-EFEITO | components/NetworkSettingsPanel.tsx | settings.c; só salvo | Já existe; passar o modo ao core ao entrar na sala |
| P1 | RDE-02 | Rede | Preferência de conexão (Automático, Internet direta, VPN) | SEM-EFEITO | components/NetworkSettingsPanel.tsx | settings.c; só salvo | Já existe; filtrar interfaces/candidatos ICE no core (ice.rs) |
| P1 | RDE-04 | Rede | Teste de rede (STUN/relay, variacao, perda, 'precisa de relay') | AUSENTE | components/NetworkSettingsPanel.tsx | (não existe) | Botão 'Testar rede' no cartão DIAGNOSTICO de Configurações > Rede, resultado em linhas 13 px com selos de cor |
| P1 | RDE-06 | Rede | TURN com credenciais da conta / configuração na interface | AUSENTE | services/rtc/turn-credentials.ts | só por BRIKS_ICE_SERVERS_JSON ou ice.json | Campos no cartão PREFERENCIA DE CONEXAO (URL, usuário, senha) ou credenciais automaticas da conta; indicador 'Somente STUN' como chip |
| P1 | TRX-11 | Transmissão | SFU (LiveKit) como padrão para salas grandes e redes dificeis | AUSENTE | services/rtc/transport-factory | só malha P2P; a opção 'Sempre SFU' em Configurações não faz nada | Sem UI nova: o seletor Modo da chamada (Rede) passa a valer; indicador 'via servidor' no chip de rede da chamada |
| P1 | TRY-05 | Tray/janela | Instância única (segunda abertura foca a primeira) | AUSENTE | desktop main.ts requestSingleInstanceLock | (não existe) | Sem tela própria: recurso de sistema (Win32) em src/main.c, com a opção correspondente num cartão de Configurações e avisos no estilo do banner de atualizacao |
| P1 | TRY-06 | Tray/janela | Iniciar com o Windows | AUSENTE | desktop (setLoginItemSettings) | (não existe) | Sem tela própria: recurso de sistema (Win32) em src/main.c, com a opção correspondente num cartão de Configurações e avisos no estilo do banner de atualizacao; interruptor 'Iniciar com o Windows' em Configurações (cartão no topo de Sobre) |
| P2 | AMI-09 | Amigos | Buscar amigo | AUSENTE | views/FriendsView.tsx | (não existe) | cui_input com lupa no topo do painel Amigos (altura 36, raio 12), filtra a lista ao digitar |
| P2 | AMI-11 | Amigos | Painel Atividade (quem está em sala de voz nos grupos) | AUSENTE | views/FriendsView.tsx | (não existe) | Painel novo 'Atividade' no dock (menu + Painel), cartões de vidro por grupo com avatares de quem está na sala |
| P2 | AMI-15 | Amigos | Sino para silenciar avisos de cada amigo | AUSENTE | services/profile/use-friend-notifications.ts | (não existe) | Ícone de sino 14 px no menu de contexto do amigo (person_row); depende de NOT-01/02 |
| P2 | ATL-05 | Atalhos | Remover atalho e aviso de atalho ocupado | AUSENTE | components/ShortcutsPanel.tsx (Remover) | (não existe) | Botão Remover (ghost, 12 px) ao lado de cada campo + mensagem 'ocupado' em B_DANGER |
| P2 | ATU-04 | Atualização | Botão 'Verificar agora' | AUSENTE | components/UpdatesPanel.tsx; ipc updater:check | (não existe: só verifica sozinho a cada 4 h) | Botão secundario no cartão ATUALIZACOES (settings.c) com estado 'Verificando...' (loading) e resultado em 12 px |
| P2 | AUT-03 | Auth | Data de nascimento no cadastro (só maiores de 18) | AUSENTE | views/AuthView.tsx; services/account/age.ts | (não existe) | Campo no formulario de criar conta (ui_auth.c, modo signup), mesmo cui_input da senha; texto de ajuda 13 px; a data não e guardada |
| P2 | AUT-11 | Auth | Entrar digitando o @nick (dica local de e-mail) | AUSENTE | services/account/login-hints.ts | (não existe) | Aceitar @nick no campo de e-mail do formulario de entrar (ui_auth.c); dica 12 px abaixo do campo; sem tela nova |
| P2 | BAS-08 | Funções básicas de desktop | Busca global / troca rápida (Ctrl+K) e busca dentro da conversa (Ctrl+F) | AUSENTE | (não existe no Electron também) | (não existe): nenhuma busca de mensagens, amigos ou grupos | Item novo (o Electron também não tinha): paleta de vidro centralizada, aberta por Ctrl+K, com campo grande e resultados em linhas de amigos, grupos e mensagens, no estilo do popover de Adicionar painel; Ctrl+F abre uma barra de busca no topo do painel de conversa |
| P2 | BAS-14 | Funções básicas de desktop | Iniciar com o Windows | AUSENTE | desktop (loginItemSettings) | (não existe) | Ver TRY-06: interruptor no cartão de sistema de Configurações; chave Run do registro do usuário |
| P2 | BAS-21 | Funções básicas de desktop | Som de notificação e sons de entrar/sair da chamada | AUSENTE | services/audio/call-sounds.ts | (não existe) | Ver CHM-20: PlaySound de WAV curto, com volume e interruptor em Configurações > Notificações |
| P2 | BAS-24 | Funções básicas de desktop | Colar imagem da área de transferência no composer | AUSENTE | Chromium (nativo, sem código próprio) | (não existe): o clipboard da plataforma lê só texto (CF_UNICODETEXT) | Composer (ui_social.c): Ctrl+V com imagem cria a prévia do anexo no cartão acima do campo; depende de MSG-09/MSG-10 |
| P2 | BAS-32 | Funções básicas de desktop | Estado offline / sem internet visível ao usuário (banner persistente) | AUSENTE | Electron | Só o aviso de erro pontual (account.c) e o texto de Diagnóstico; sem banner persistente na janela principal | Banner fino de vidro no topo do painel de conversa (mesmo padrão do banner de atualização, ui_update.c), com "Sem conexão. Tentando reconectar" e botão Tentar agora; some sozinho ao voltar |
| P2 | BAS-39 | Funções básicas de desktop | Tamanho de texto ajustável pelo usuário (zoom da interface) | AUSENTE | Chromium (nativo, sem código próprio) (Ctrl+roda/Ctrl+mais) | (não existe): só segue a escala do Windows (BAS-18); sem Ctrl+mais/menos nem opção | Controle deslizante de 90% a 150% no cartão Temas ou Desempenho de Configurações e atalhos Ctrl+mais/Ctrl+menos/Ctrl+0; aplica a escala da biblioteca (cui_dpi_scale) e guarda em settings |
| P2 | CFG-08 | Configurações | Volume de entrada (ganho) | SEM-EFEITO | components/VoiceVideoSettingsPanel.tsx | settings.c slider; só salvo | Já existe; aplicar ganho no core (áudio.rs) |
| P2 | CFG-10 | Configurações | Volume de saida | SEM-EFEITO | components/VoiceVideoSettingsPanel.tsx | settings.c slider; só salvo | Já existe; aplicar na reprodução |
| P2 | CFG-12 | Configurações | Porta de ruído com limite | SEM-EFEITO | components/VoiceVideoSettingsPanel.tsx | settings.c; só salvo | Já existe; aplicar no core |
| P2 | CFG-20 | Configurações | Limpar anexos | SEM-EFEITO | components/LocalStoragePanel.tsx | settings.c: mostra 'Sem efeito real ainda' | Botão já existe; apagar a pasta de anexos pelo core e atualizar o tamanho |
| P2 | CFG-22 | Configurações | Exportar meus dados | SEM-EFEITO | components/LocalStoragePanel.tsx | settings.c: botão sem ação real | Botão já existe; abrir 'Salvar como' (GetSaveFileName) e gravar JSON sem senhas/chaves |
| P2 | CFG-23 | Configurações | Sobreposição em jogos: interruptor geral | SEM-EFEITO | components/OverlaySettingsPanel.tsx | settings.c; briks_overlay.exe existe mas o app não o abre | Já existe; iniciar/encerrar briks_overlay.exe e alimenta-lo com quem fala |
| P2 | CFG-24 | Configurações | Sobreposição: o que aparece (4 itens) e opacidade | SEM-EFEITO | components/OverlaySettingsPanel.tsx | settings.c; só salvo | Já existe; repassar ao overlay |
| P2 | CFG-29 | Configurações | Novidades da versão em uso | AUSENTE | components/release-notes.ts | (não existe) | Cartão NOVIDADES na aba Sobre/Atualizacoes, lista de 12-13 px, e diálogo de vidro 'O que há de novo' após atualizar |
| P2 | CHM-07 | Chamada | Mini-painel 'Voz conectada' na lateral (mic, fone, desligar, abrir) | SEM-EFEITO | views/HomeView.tsx | ui_social.c panel_voice só tem Voltar a sala | Rodape do painel Menu/Sala de voz: cartão de vidro 56 px com 3 icon_buttons (mic, fone, sair) e nome da sala |
| P2 | CHM-12 | Chamada | Zoom na transmissão (roda, pinca, arrastar, selo de %) | AUSENTE | components/vídeo-zoom.tsx | (não existe) | Selo de vidro 11 px no canto superior esquerdo do bloco (dica de zoom); arrastar move; sem painel |
| P2 | CHM-16 | Chamada | Desconexao após 1 h sem falar, com aviso e Continuar | AUSENTE | CallWorkspace.tsx; release 1.1.0 | (não existe) | Banner de vidro no topo da chamada nos últimos 5 min com botão Continuar (update_banner), contagem 13 px |
| P2 | CHM-17 | Chamada | Diagnóstico da conexão e indicador Rede Boa dentro da chamada | AUSENTE | components/CallWorkspace.tsx (Rede, Diagnóstico) | só em Configurações > Rede | Chip 'Rede boa/ruim' no cabeçalho da chamada que abre cartão de vidro com Signaling/Canal/Transporte (reusa o cartão DIAGNOSTICO de Configurações) |
| P2 | CHM-20 | Chamada | Sons de entrar e sair da chamada | AUSENTE | services/áudio/call-sounds.ts | (não existe) | Sem UI; PlaySound de dois WAV curtos no rtc.c; opção 'Sons da chamada' no cartão de Notificações |
| P2 | CHM-23 | Chamada | Guardar histórico on/off e apagar histórico | AUSENTE | components/CallHistoryPanel.tsx | (não existe) | Interruptor e botão Apagar no topo da tela de Histórico, cartão de vidro 14 px |
| P2 | CHM-24 | Chamada | Vídeo remoto, câmera dos outros e 2a tela | AUSENTE | components/CallWorkspace.tsx | (não existe) | Tile de vídeo no palco da chamada no lugar do avatar; 2a tela como tile extra com selo |
| P2 | GRP-06 | Grupos, canais e cargos | Renomear canal/sala | AUSENTE | views/HomeView.tsx | (não existe) | Ícone de lapis ao passar o mouse na linha do canal (hover do nav_item) + diálogo curto |
| P2 | GRP-07 | Grupos, canais e cargos | Apagar canal/sala | AUSENTE | views/HomeView.tsx | (não existe) | Ícone de lixeira no hover do canal + confirmacao no estilo Expulsar |
| P2 | GRP-11 | Grupos, canais e cargos | Recarregar lista de membros | AUSENTE | views/HomeView.tsx | (não existe) | Ícone de refresh no panel_shell do painel Membros |
| P2 | GRP-15 | Grupos, canais e cargos | Reparar/limpar identidades duplicadas de membros | AUSENTE | views/HomeView.tsx removeDuplicateMembers | (não existe) | Botão secundario no rodape do painel Membros, visivel só quando há duplicados |
| P2 | GRP-19 | Grupos, canais e cargos | Buscar mensagens do canal | AUSENTE | views/GroupView.tsx (Buscar) | (não existe) | Lupa no cabeçalho do chat (ao lado de Fixadas) abre cui_input embutido |
| P2 | LAY-06 | Layout novo (trilho, lista, conversa, hub) | Histórico de chamadas como painel do layout (hoje cobre a janela inteira, sem trilho nem lista) | AUSENTE | (padrão novo do usuário) | ui_history.c: o item da lista abre tela cheia com seta de voltar e só o estado vazio; existe também o painel Histórico do dock (+ painel), mas o item da lista não o usa | Abrir no painel central (no lugar da conversa) mantendo trilho e lista, com cartões de vidro por chamada (duração, participantes, botão Ligar de novo); seta de voltar só no modo estreito |
| P2 | LAY-11 | Layout novo (trilho, lista, conversa, hub) | Texto sempre legível: tamanho mínimo nas etiquetas (rótulos de seção em 10 a 11 px) | AUSENTE | (padrão novo do usuário) | Rótulos "SALAS DE VOZ", "GRUPOS", "AMIGOS", dicas e legendas dos balões usam 10 a 11 px, abaixo do padrão de "textos maiores" pedido; corpo e títulos estão bons | Subir etiquetas de seção para 12 px e dicas/horários para 12 px nos painéis lista e conversa (a constante está espalhada por .size nos .c; vale centralizar em briks_ui.h) |
| P2 | LAY-12 | Layout novo (trilho, lista, conversa, hub) | Dica "Sair" (tooltip) presa no topo da tela e círculo no canto inferior esquerdo em todas as capturas | AUSENTE | (padrão novo do usuário) | Nas capturas ocultas o tooltip do botão sair do rodapé aparece fixo em x=640 y=20; já listado em plano-pos-1.3.0 (frente D) | Corrigir a âncora do tooltip do rodapé (cui_tooltip) para o botão, e esconder o ponteiro do roteiro nas capturas |
| P2 | MSG-08 | Mensagens | Indicador 'digitando...' | AUSENTE | components/ConversationTimeline.tsx | (não existe) | Linha de 12 px cor B_MUTED acima do compositor, com três pontos animados (respeita reduced motion) |
| P2 | MSG-11 | Mensagens | Galeria de anexos do canal | AUSENTE | components/ConversationTimeline.tsx (Galeria do canal) | (não existe) | Painel novo 'Anexos' no dock (grade de miniaturas, raio 14) ou botão no cabeçalho do chat |
| P2 | MSG-12 | Mensagens | Pré-visualização de link | AUSENTE | components/SafeMessageContent.tsx | (não existe) | Cartão de vidro sob o texto da bolha (título 13 px, URL 11 px) |
| P2 | MSG-13 | Mensagens | Links e formatacao Markdown seguros no texto | AUSENTE | components/SafeMessageContent.tsx | msgui.c exibe texto puro; links não abrem | Renderizar na bolha (texto rico) com link em B_C3 sublinhado; clique abre no navegador padrão |
| P2 | MSG-15 | Mensagens | Buscar mensagens na conversa | AUSENTE | views/HomeView.tsx (Buscar) | (não existe) | Mesma lupa do GRP-19 no cabeçalho do chat |
| P2 | NOT-04 | Notificações | Notificação 'pediu para você transmitir' | AUSENTE | components/CallWorkspace.tsx | (não existe) | Junto de CHM-15 |
| P2 | NOT-05 | Notificações | Nada e avisado durante a chamada nem logo ao abrir | AUSENTE | release 1.2.0 | (sem avisos) | Regra de supressao no mesmo modulo de NOT-01 |
| P2 | PRF-06 | Perfil | Limpar o status depois de 30 min/1 h/4 h/hoje | SEM-EFEITO | components/StatusEditor.tsx | ui_social.c pf-clear grava status_clear, mas nada o le: o status nunca expira | Aplicar a expiracao em profile.c (timer) e mostrar 'some em X' sob o campo, 11 px, cor B_MUTED |
| P2 | PRF-12 | Perfil | Outros computadores ligados ao @nick (listar e revogar) | AUSENTE | components/AccountPanel.tsx (Revogar) | (não existe) | Cartão 'Computadores' na aba Conta do diálogo Perfil e conta: linhas com nome do PC, data e botão Revogar secundario, confirmacao em diálogo |
| P2 | RDE-03 | Rede | Redes privadas detectadas (ZeroTier, Tailscale, WireGuard) | AUSENTE | desktop network-interfaces.ts | (não existe) | Linha 'Detectado' com selo verde dentro do cartão PREFERENCIA DE CONEXAO |
| P2 | TRY-07 | Tray/janela | Links briks:// (convites e mensagens) | AUSENTE | desktop protocol.ts | msgui.c só COPIA briks://mensagem; o protocolo não está registrado | Sem tela própria: recurso de sistema (Win32) em src/main.c, com a opção correspondente num cartão de Configurações e avisos no estilo do banner de atualizacao; abre o diálogo de convite/mensagem correspondente |
| P2 | TRY-08 | Tray/janela | Abrir links externos no navegador | AUSENTE | desktop ipc shell:open-external | (não existe: links não são clicaveis) | Ver MSG-13 |
| P3 | AMI-16 | Amigos | Desconectar só o chat privado (Desconectar P2P) | AUSENTE | views/HomeView.tsx | (não existe) | Item no menu de contexto do amigo; texto 13 px; confirmacao simples |
| P3 | ATL-06 | Atalhos | Ctrl+Shift+O: editar posições do overlay em qualquer tela | AUSENTE | components/OverlaySettingsPanel.tsx | (não existe) | Depende de CFG-25 |
| P3 | BAS-27 | Funções básicas de desktop | Tema claro e escuro (e seguir o do Windows) | AUSENTE | (Electron: só tema escuro com cores) | Temas por gradiente (Configurações > Temas), sempre escuros; sem tema claro nem seguir o Windows | Opção nova no cartão Temas: "Claro" e "Seguir o Windows"; o gerador de tema (themes.c) precisa de uma variante clara com o mesmo contraste mínimo; fora do escopo do Electron |
| P3 | BAS-28 | Funções básicas de desktop | Idioma da interface (pt-BR, inglês) | AUSENTE | (não existe no Electron também) | (não existe): textos fixos em português no código | Tabela de strings e seletor de idioma no cartão Sobre de Configurações; só depois do conteúdo final dos painéis |
| P3 | BAS-37 | Funções básicas de desktop | Licenças de software de terceiros dentro do app | AUSENTE | (não existe no Electron também) | (não existe): THIRD_PARTY.md existe no repositório da biblioteca, não dentro do app | Botão "Licenças de terceiros" no cartão Sobre abrindo diálogo de vidro com rolagem e o texto empacotado |
| P3 | BAS-40 | Funções básicas de desktop | Leitor de tela e navegação assistiva (UI Automation / MSAA) | AUSENTE | Chromium (nativo, sem código próprio) (árvore de acessibilidade) | (não existe): a interface é desenhada pela biblioteca, sem UIA | Sem tela: provedor UIA mínimo na plataforma (nome, papel e estado de botões e campos); alto custo, vale só depois do resto |
| P3 | CFG-19 | Configurações | Divisão do espaco por tipo (mensagens, grupos, anexos) | AUSENTE | components/LocalStoragePanel.tsx | settings.c: 'depende do backend de dados' | Barra segmentada 8 px dentro do cartão USO NESTE COMPUTADOR, legenda 12 px |
| P3 | CFG-25 | Configurações | Sobreposição: editar posições (arrastar, redimensionar) | SEM-EFEITO | components/OverlaySettingsPanel.tsx | settings.c: botão sem ação | Botão já existe; modo de edicao do overlay com atalho Ctrl+Shift+O (ATL-06) |
| P3 | CFG-26 | Configurações | Aba Avisos: lista e limpar | SEM-EFEITO | views/SettingsView.tsx | settings.c: lista existe, mas nada gera avisos | Cartão já existe; chamar settings_notice() para erros reais (backend, core, atualizacao) |
| P3 | CFG-28 | Configurações | Telemetria anonima ligar/desligar | SEM-EFEITO | components/AboutPanel.tsx; services/telemetry | settings.c; só salvo, nada e enviado | Interruptor já existe; contadores diarios no core |
| P3 | EXT-07 | Extras do CUI (sem equivalente no Electron) | Lembrete de mensagem | SEM-EFEITO | (novo) | msgui.c: grava o lembrete mas não notifica ('sem efeito') | Disparar toast quando o prazo chega (depende de NOT-03) |
| P3 | TRX-12 | Transmissão | Excluir o áudio do Briks da transmissão | SEM-EFEITO | components/VoiceVideoSettingsPanel.tsx | settings.c interruptor sem efeito (o core já exclui Briks e Discord sempre) | Já existe em Voz e vídeo; ligar o interruptor ao core (share.áudio.set) |
| P3 | TRY-09 | Tray/janela | Barra de título própria com minimizar/maximizar/fechar e versão | AUSENTE | desktop window.ts | (janela do SO) | Barra de vidro 32 px no topo (briks_gradient_bar já existe como filete), controles Lucide 14 px; só se o design novo pedir |

## VERIFICAR (não consegui concluir)

- **GRP-22** Sincronização do manifesto do grupo, cargos e revogacoes entre pares. Electron: chat/GroupChatService.ts; SyncService.ts. CUI: briks-core: parte em chat_events.rs; trabalho em andamento no branch cui-ui-paridade (grupos-p2p-core.md); sem teste de interop.
- **MSG-17** Histórico sincronizado com o par (pedir/responder histórico). Electron: chat/HistoryService.ts. CUI: briks-core chat_events.rs; sem teste de interop.
- **TRX-10** Aviso 'Assistir' quando alguem comeca a transmitir e troca automatica para o modo Transmissão. Electron: release 1.1.0/1.1.1. CUI: ui_call.c/ui_share.c; comportamento não conferido.
- **BAS-19** Vários monitores: abrir no monitor onde estava e janela fora da área visível voltar para dentro. Electron: desktop-lifecycle.ts. CUI: (não existe): sem restauração de posição, então depende do Windows.
- **BAS-30** Estado de carregamento (spinner ou esqueleto) enquanto o core sobe, entra na sala ou carrega o histórico. Electron: Electron. CUI: VERIFICAR: há texto de status ("Conectando", "Baixando a atualização"), mas não vi spinner ou esqueleto nas listas; depende do core real.
- **BAS-42** IME (japonês, chinês, coreano) e emoji do teclado do Windows (Win+.). Electron: Chromium (nativo, sem código próprio). CUI: VERIFICAR: a plataforma trata WM_CHAR mas não WM_IME_COMPOSITION; o texto final de IME e o painel de emoji provavelmente entram, mas sem janela de composição junto ao cursor.
- **LAY-08** Tela de chamada em tela cheia (imersiva) em vez de dentro do layout trilho/lista/hub. Electron: (padrão novo do usuário). CUI: ui_call.c: cabeçalho próprio, grade de participantes e barra de controles flutuante; esconde trilho e lista. Decisão de design, não defeito.

## Tabela completa por área

### Auth

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| AUT-01 | Entrar com e-mail e senha | views/AuthView.tsx | ui_auth.c + account.c (account.login) | PRESENTE-FUNCIONAL | auth-a |
| AUT-02 | Criar conta com e-mail (nome, e-mail, senha, repetir senha) | views/AuthView.tsx | ui_auth.c (signup) | PRESENTE-FUNCIONAL | auth-b |
| AUT-03 | Data de nascimento no cadastro (só maiores de 18) | views/AuthView.tsx; services/account/age.ts | (não existe) | AUSENTE | sonda |
| AUT-04 | Esqueci minha senha (e-mail de recuperação) | views/AuthView.tsx | ui_auth.c (recover) | PRESENTE-FUNCIONAL | auth-a |
| AUT-05 | Código de 6 digitos / link de recuperação e nova senha | views/AuthView.tsx | ui_auth.c (code, recovery) | PRESENTE-FUNCIONAL | - |
| AUT-06 | Continuar com Google (navegador) | components/AccountPanel.tsx; services/account/account-service.ts | ui_auth.c account_oauth(google); não testado com provedor real | PRESENTE-FUNCIONAL | - |
| AUT-07 | Continuar com Discord (navegador) | components/AccountPanel.tsx | ui_auth.c account_oauth(discord); não testado com provedor real | PRESENTE-FUNCIONAL | - |
| AUT-08 | Cancelar a espera do navegador | views/AuthView.tsx | ui_auth.c (Cancelar / account.cancel) | PRESENTE-FUNCIONAL | - |
| AUT-09 | Mostrar/ocultar a senha (olho) | views/AuthView.tsx | ui_auth.c (cui_input password) | PRESENTE-FUNCIONAL | auth-c |
| AUT-10 | Alternar entre entrar, criar conta e Google/Discord | views/AuthView.tsx | ui_auth.c (links) | PRESENTE-FUNCIONAL | auth-c |
| AUT-11 | Entrar digitando o @nick (dica local de e-mail) | services/account/login-hints.ts | (não existe) | AUSENTE | sonda |
| AUT-12 | Tela de carregamento ao restaurar a sessao | App.tsx (Restaurando sua sessao) | ui_auth.c (Preparando seu espaco) | PRESENTE-FUNCIONAL | - |
| AUT-13 | Sair da conta (rodape, Perfil e Configurações) | views/HomeView.tsx (Sair, Desconectar) | ui_social.c / settings.c / app.c app_logout | PRESENTE-FUNCIONAL | perfil |
| AUT-14 | Erros de login em português claro | views/auth-errors.ts | account.c (mensagens) | PRESENTE-FUNCIONAL | - |

### Perfil

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| PRF-01 | Editar nome exibido | components/ProfileEditor.tsx | ui_social.c / ui_account.c (Nome, Salvar) | PRESENTE-FUNCIONAL | perfil |
| PRF-02 | Escolher/trocar foto de perfil | components/ProfileEditor.tsx; ProfileAvatar.tsx | ui_account.c account_pick_photo | PRESENTE-FUNCIONAL | - |
| PRF-03 | Mostrar a foto dos OUTROS em amigos, membros, tiles da chamada e mensagens | components/ProfileAvatar.tsx | ui_social.c: só a sua foto aparece (user_photo); os demais usam iniciais | AUSENTE | sonda |
| PRF-04 | Status personalizado (texto e emoji) | components/StatusEditor.tsx | ui_social.c (STATUS) | PRESENTE-FUNCIONAL | perfil |
| PRF-05 | Chips rapidos de status (Jogando, Estudando...) | components/StatusEditor.tsx | ui_social.c (CHIPS) | PRESENTE-FUNCIONAL | perfil |
| PRF-06 | Limpar o status depois de 30 min/1 h/4 h/hoje | components/StatusEditor.tsx | ui_social.c pf-clear grava status_clear, mas nada o le: o status nunca expira | PRESENTE-SEM-EFEITO | sonda |
| PRF-07 | Limpar status (botão) | components/StatusEditor.tsx | ui_social.c (Limpar) | PRESENTE-FUNCIONAL | - |
| PRF-08 | Programa em uso: ligar/desligar | components/RichPresencePanel.tsx | profile.c + ui_social.c | PRESENTE-FUNCIONAL | perfil |
| PRF-09 | Ocultar/ignorar programas especificos | components/RichPresencePanel.tsx | ui_social.c (Ignorar programa) | PRESENTE-FUNCIONAL | perfil |
| PRF-10 | Abas do perfil: Perfil, Status, Programa em uso, Conta e @nick | views/HomeView.tsx | ui_social.c (profile_tab) | PRESENTE-FUNCIONAL | perfil |
| PRF-11 | Criar/trocar @nick | services/account/nick-enrollment.ts; NickInvite.tsx | ui_account.c / ui_social.c (Trocar @nick) | PRESENTE-FUNCIONAL | - |
| PRF-12 | Outros computadores ligados ao @nick (listar e revogar) | components/AccountPanel.tsx (Revogar) | (não existe) | AUSENTE | sonda |
| PRF-13 | Identidade/peerId estavel entre reinicios | chat/ChatController.ts (localPeerId) | briks-core identity.rs + backup de identidade | PRESENTE-FUNCIONAL | - |

### Amigos

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| AMI-01 | Lista de amigos na barra lateral | views/HomeView.tsx; FriendsView.tsx | ui_social.c (AMIGOS) | PRESENTE-FUNCIONAL | social-a |
| AMI-02 | Abrir conversa direta ao clicar no amigo | views/HomeView.tsx | ui_social.c app_open_friend | PRESENTE-FUNCIONAL | social-a |
| AMI-03 | Adicionar amigo pelo @nick | components/NickInvite.tsx | ui_social.c (Adicionar amigo) / account.c | PRESENTE-FUNCIONAL | social-a |
| AMI-04 | Adicionar amigo por código de convite P2P (criar convite, usar código, aceitar, recusar) | components/P2PInvitePanel.tsx; services/invites/service.ts | (não existe) | AUSENTE | sonda |
| AMI-05 | Pedidos pendentes: aceitar | views/FriendsView.tsx (Aceitar) | ui_social.c (ícone check) | PRESENTE-FUNCIONAL | - |
| AMI-06 | Pedidos pendentes: recusar | components/P2PInvitePanel.tsx (Recusar) | core aceita friends.respond accept=false, mas a UI só tem Aceitar | PRESENTE-SEM-EFEITO | sonda |
| AMI-07 | Desfazer amizade (com confirmacao) | views/FriendsView.tsx; HomeView.tsx deleteTarget | core tem friends.remove; sem UI | AUSENTE | sonda |
| AMI-08 | Abas Online / Todos / Pendentes com contadores | views/FriendsView.tsx | ui_social.c: uma lista única AMIGOS e PEDIDOS, sem filtro | AUSENTE | sonda |
| AMI-09 | Buscar amigo | views/FriendsView.tsx | (não existe) | AUSENTE | sonda |
| AMI-10 | Indicador online/offline do amigo | views/HomeView.tsx (title Online/Offline) | ui_social.c só mostra 'Disponivel' no próprio rodape | AUSENTE | sonda |
| AMI-11 | Painel Atividade (quem está em sala de voz nos grupos) | views/FriendsView.tsx | (não existe) | AUSENTE | sonda |
| AMI-12 | Estado vazio de amigos com chamada para adicionar | views/FriendsView.tsx | ui_social.c ('Nenhum amigo ainda...') | PRESENTE-FUNCIONAL | - |
| AMI-13 | Estado vazio 'Escolha uma conversa' | views/HomeView.tsx | ui_social.c | PRESENTE-FUNCIONAL | social-a |
| AMI-14 | Status e programa em uso visiveis na lista | components/StatusEditor.tsx | ui_social.c person_row | PRESENTE-FUNCIONAL | - |
| AMI-15 | Sino para silenciar avisos de cada amigo | services/profile/use-friend-notifications.ts | (não existe) | AUSENTE | sonda |
| AMI-16 | Desconectar só o chat privado (Desconectar P2P) | views/HomeView.tsx | (não existe) | AUSENTE | sonda |
| AMI-17 | Entrar em grupo por convite/código | views/HomeView.tsx (Entrar em grupo); GroupInvitePanel.tsx | (não existe) | AUSENTE | sonda |

### Grupos, canais e cargos

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| GRP-01 | Criar grupo | views/HomeView.tsx (Criar grupo) | ui_social.c (create_group_open) | PRESENTE-FUNCIONAL | social-b |
| GRP-02 | Lista de grupos e abrir o grupo | views/HomeView.tsx | ui_social.c / painel Servidores | PRESENTE-FUNCIONAL | social-b |
| GRP-03 | Conversa do canal de texto geral do grupo | views/GroupView.tsx | ui_social.c (Grupo P2P canal geral) | PRESENTE-FUNCIONAL | social-b |
| GRP-04 | Criar canal de texto | views/HomeView.tsx (Criar canal) | chat.c carrega canais, mas não há UI para criar | AUSENTE | sonda |
| GRP-05 | Criar sala de voz do grupo | views/HomeView.tsx | a sala de voz e uma única 'Sala geral' fixa, fora dos grupos | AUSENTE | sonda |
| GRP-06 | Renomear canal/sala | views/HomeView.tsx | (não existe) | AUSENTE | sonda |
| GRP-07 | Apagar canal/sala | views/HomeView.tsx | (não existe) | AUSENTE | sonda |
| GRP-08 | Personalizar grupo (nome e imagem, sincroniza aos membros) | components/GroupEditor.tsx | (não existe) | AUSENTE | sonda |
| GRP-09 | Apagar ou sair do grupo | views/HomeView.tsx (Apagar ou sair do grupo) | (não existe) | AUSENTE | sonda |
| GRP-10 | Lista de membros do grupo com marcas VOCÊ/DONO/Admin/@nick | views/HomeView.tsx (membros) | ui_social.c panel_members mostra você + seus amigos, não os membros do grupo | PRESENTE-SEM-EFEITO | sonda |
| GRP-11 | Recarregar lista de membros | views/HomeView.tsx | (não existe) | AUSENTE | sonda |
| GRP-12 | Adicionar membro (amigo) ao grupo | views/HomeView.tsx (Adicionar membro) | (não existe) | AUSENTE | sonda |
| GRP-13 | Gerenciar membro: tornar/remover administrador | views/HomeView.tsx (Gerenciar membro) | (não existe) | AUSENTE | sonda |
| GRP-14 | Remover membro do grupo | views/HomeView.tsx removeSelectedMember | (não existe) | AUSENTE | sonda |
| GRP-15 | Reparar/limpar identidades duplicadas de membros | views/HomeView.tsx removeDuplicateMembers | (não existe) | AUSENTE | sonda |
| GRP-16 | Convite para o grupo (código) | components/GroupInvitePanel.tsx | (não existe) | AUSENTE | sonda |
| GRP-17 | Sala de voz mostra quem está nela, 'Ao vivo' e Assistir | views/HomeView.tsx | (não existe: Sala geral não mostra participantes) | AUSENTE | sonda |
| GRP-18 | Entrar na sala de voz pela barra lateral | views/HomeView.tsx | ui_social.c (Sala geral, voz-geral): sala única, não por grupo | PRESENTE-FUNCIONAL | social-b |
| GRP-19 | Buscar mensagens do canal | views/GroupView.tsx (Buscar) | (não existe) | AUSENTE | sonda |
| GRP-20 | Estado vazio do canal | views/GroupView.tsx | ui_social.c | PRESENTE-FUNCIONAL | - |
| GRP-21 | Permissões: dono/admin podem fixar e excluir de outros | chat/GroupChatService.ts | msgui.c can_pin / can_delete | PRESENTE-FUNCIONAL | - |
| GRP-22 | Sincronização do manifesto do grupo, cargos e revogacoes entre pares | chat/GroupChatService.ts; SyncService.ts | briks-core: parte em chat_events.rs; trabalho em andamento no branch cui-ui-paridade (grupos-p2p-core.md); sem teste de interop | VERIFICAR | - |

### Mensagens

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| MSG-01 | Enviar mensagem (Enter ou botão) | components/MessageComposer.tsx | ui_social.c / msgui.c | PRESENTE-FUNCIONAL | msg-enviar |
| MSG-02 | Lista de mensagens com autor, hora e bolhas | components/ConversationTimeline.tsx | ui_social.c bubble() | PRESENTE-FUNCIONAL | msg-enviar |
| MSG-03 | Responder a uma mensagem | components/ConversationTimeline.tsx | msgui.c (Responder) | PRESENTE-FUNCIONAL | msg-menu |
| MSG-04 | Editar mensagem própria | components/ConversationTimeline.tsx | msgui.c (Editar) | PRESENTE-FUNCIONAL | msg-editar |
| MSG-05 | Excluir mensagem (própria e, como admin, de outros) | components/ConversationTimeline.tsx | msgui.c (Excluir) | PRESENTE-FUNCIONAL | msg-excluir |
| MSG-06 | Reagir com emoji | components/ConversationTimeline.tsx | msgui.c (picker e chips) | PRESENTE-FUNCIONAL | msg-reagir |
| MSG-07 | Fixar/desafixar e ver Fixadas | components/ConversationTimeline.tsx | msgui.c (Fixar, chip Fixadas) | PRESENTE-FUNCIONAL | msg-fixadas |
| MSG-08 | Indicador 'digitando...' | components/ConversationTimeline.tsx | (não existe) | AUSENTE | sonda |
| MSG-09 | Anexar arquivo (botão, arrastar e soltar) | components/MessageComposer.tsx (Anexar arquivo, Solte para enviar) | ui_social.c: botão clip desabilitado 'Anexar (marco 3)' | PRESENTE-SEM-EFEITO | msg-anexar |
| MSG-10 | Cartão de anexo (baixar, pausar, retomar, cancelar, abrir) | components/AttachmentCard.tsx | (não existe) | AUSENTE | sonda |
| MSG-11 | Galeria de anexos do canal | components/ConversationTimeline.tsx (Galeria do canal) | (não existe) | AUSENTE | sonda |
| MSG-12 | Pré-visualização de link | components/SafeMessageContent.tsx | (não existe) | AUSENTE | sonda |
| MSG-13 | Links e formatacao Markdown seguros no texto | components/SafeMessageContent.tsx | msgui.c exibe texto puro; links não abrem | AUSENTE | sonda |
| MSG-14 | Estados da mensagem: enviando, entregue, falhou e reenvio (outbox) | chat/OutboxService.ts | (não existe: s->notice 'Não foi possivel gravar') | AUSENTE | sonda |
| MSG-15 | Buscar mensagens na conversa | views/HomeView.tsx (Buscar) | (não existe) | AUSENTE | sonda |
| MSG-16 | Mensagens ficam gravadas neste computador | services/offline/chat-storage.ts | msgui.c / backend local | PRESENTE-FUNCIONAL | - |
| MSG-17 | Histórico sincronizado com o par (pedir/responder histórico) | chat/HistoryService.ts | briks-core chat_events.rs; sem teste de interop | VERIFICAR | - |
| MSG-18 | Aba Chat da chamada mostra as mensagens da sala | components/CallWorkspace.tsx (Chat da chamada) | ui_call.c (call_view 1) | PRESENTE-FUNCIONAL | call |
| MSG-19 | Enviar mensagem pela aba Chat da chamada | components/CallWorkspace.tsx | ui_call.c: lista sem campo de texto | PRESENTE-SEM-EFEITO | sonda |

### Chamada

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| CHM-01 | Entrar na sala de voz | views/HomeView.tsx | ui_social.c / rtc.c (rtc_join_room) | PRESENTE-FUNCIONAL | social-b |
| CHM-02 | Mutar/desmutar o microfone | components/CallWorkspace.tsx | ui_call.c (Microfone) | PRESENTE-FUNCIONAL | call |
| CHM-03 | Ensurdecer | components/CallWorkspace.tsx | ui_call.c (Ensurdecer) | PRESENTE-FUNCIONAL | call |
| CHM-04 | Câmera (ligar e enviar vídeo) | components/CallWorkspace.tsx (Câmera) | ui_call.c: pílula desabilitada 'Câmera - em breve' | PRESENTE-SEM-EFEITO | call |
| CHM-05 | Sair da chamada | components/CallWorkspace.tsx (Sair) | ui_call.c (Sair) | PRESENTE-FUNCIONAL | call-sair |
| CHM-06 | Voltar ao menu sem sair da chamada | components/CallWorkspace.tsx (Voltar ao menu) | ui_call.c (ícone minimizar) | PRESENTE-FUNCIONAL | call |
| CHM-07 | Mini-painel 'Voz conectada' na lateral (mic, fone, desligar, abrir) | views/HomeView.tsx | ui_social.c panel_voice só tem Voltar a sala | PRESENTE-SEM-EFEITO | sonda |
| CHM-08 | Modo Call / Transmissão | components/CallWorkspace.tsx | ui_call.c (Call / Transmissão) | PRESENTE-FUNCIONAL | call |
| CHM-09 | Chamada / Chat | components/CallWorkspace.tsx | ui_call.c (Chamada / Chat) | PRESENTE-FUNCIONAL | call |
| CHM-10 | Disposição Grade / Foco das telas | components/CallWorkspace.tsx | (não existe) | AUSENTE | sonda |
| CHM-11 | Tela cheia da transmissão e da chamada | components/CallWorkspace.tsx (Tela cheia); ipc window:fullscreen | (não existe) | AUSENTE | sonda |
| CHM-12 | Zoom na transmissão (roda, pinca, arrastar, selo de %) | components/vídeo-zoom.tsx | (não existe) | AUSENTE | sonda |
| CHM-13 | Silenciar uma pessoa só para mim | components/CallWorkspace.tsx | ui_call.c (Silenciar para mim) | PRESENTE-FUNCIONAL | - |
| CHM-14 | Volume por pessoa (slider) lembrado nas próximas chamadas | services/áudio/person-áudio.ts | só liga/desliga mudo, sem volume e sem memória | AUSENTE | sonda |
| CHM-15 | Pedir a alguem que transmita (e aceitar/recusar o pedido) | call/stream-request.ts | (não existe) | AUSENTE | sonda |
| CHM-16 | Desconexao após 1 h sem falar, com aviso e Continuar | CallWorkspace.tsx; release 1.1.0 | (não existe) | AUSENTE | sonda |
| CHM-17 | Diagnóstico da conexão e indicador Rede Boa dentro da chamada | components/CallWorkspace.tsx (Rede, Diagnóstico) | só em Configurações > Rede | AUSENTE | sonda |
| CHM-18 | Ajustes de áudio durante a chamada (mic, ruído, eco, porta) | components/InCallAudioSettings.tsx | (não existe: só Configurações, e sem efeito) | AUSENTE | sonda |
| CHM-19 | Anel/nivel de quem está falando | components/CallWorkspace.tsx | ui_call.c (me_level, rtc_peers.level) | PRESENTE-FUNCIONAL | - |
| CHM-20 | Sons de entrar e sair da chamada | services/áudio/call-sounds.ts | (não existe) | AUSENTE | sonda |
| CHM-21 | Tela Histórico de chamadas abre e volta | components/CallHistoryPanel.tsx | ui_history.c | PRESENTE-FUNCIONAL | social-b |
| CHM-22 | Registro das chamadas (quando, duracao, sala, quem) | components/CallHistoryPanel.tsx; services/call/call-history.ts | ui_history.c: só o estado vazio; nada grava chamadas | PRESENTE-SEM-EFEITO | sonda |
| CHM-23 | Guardar histórico on/off e apagar histórico | components/CallHistoryPanel.tsx | (não existe) | AUSENTE | sonda |
| CHM-24 | Vídeo remoto, câmera dos outros e 2a tela | components/CallWorkspace.tsx | (não existe) | AUSENTE | sonda |
| CHM-25 | Expulsar da chamada (dono) e bloqueio de volta | (novo no CUI) | ui_call.c / rtc.c peer.kick | PRESENTE-FUNCIONAL | - |

### Transmissão

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| TRX-01 | Seletor de fonte: abas Telas/Janelas com miniaturas | components/CallWorkspace.tsx; desktop screen-capture.ts | ui_share.c (diálogo); não exercitado aqui (depende do core/GPU) | PRESENTE-FUNCIONAL | - |
| TRX-02 | Resolucao, FPS e bitrate | components/ScreenQualityPanel.tsx | ui_share.c; não exercitado aqui | PRESENTE-FUNCIONAL | - |
| TRX-03 | Áudio da transmissão: da fonte / escolher apps / sem áudio, lista Tocando agora | services/áudio/screen-áudio-apps.ts | ui_share.c + procaudio.rs; não exercitado aqui | PRESENTE-FUNCIONAL | - |
| TRX-04 | Iniciar e parar a transmissão | components/CallWorkspace.tsx | ui_share.c (Compartilhar, Parar); não exercitado aqui | PRESENTE-FUNCIONAL | - |
| TRX-05 | Ajustar a qualidade ao vivo | components/ScreenQualityPanel.tsx | ui_share.c (Ajustar, Qualidade); não exercitado aqui | PRESENTE-FUNCIONAL | - |
| TRX-06 | Chips de qualidade e de áudio na barra da chamada | components/CallWorkspace.tsx | ui_share.c (ui_share_bar) | PRESENTE-FUNCIONAL | - |
| TRX-07 | Assistir a transmissão de alguem | components/CallWorkspace.tsx (Assistir) | ui_share.c (share.watch); não exercitado aqui | PRESENTE-FUNCIONAL | - |
| TRX-08 | Volume da transmissão de quem assiste | components/CallWorkspace.tsx | ui_share.c (share.volume); não exercitado aqui | PRESENTE-FUNCIONAL | - |
| TRX-09 | Prévia local da própria transmissão | components/CallWorkspace.tsx (PREVIA) | ui_share.c (preview.bin); não exercitado aqui | PRESENTE-FUNCIONAL | - |
| TRX-10 | Aviso 'Assistir' quando alguem comeca a transmitir e troca automatica para o modo Transmissão | release 1.1.0/1.1.1 | ui_call.c/ui_share.c; comportamento não conferido | VERIFICAR | - |
| TRX-11 | SFU (LiveKit) como padrão para salas grandes e redes dificeis | services/rtc/transport-factory | só malha P2P; a opção 'Sempre SFU' em Configurações não faz nada | AUSENTE | sonda |
| TRX-12 | Excluir o áudio do Briks da transmissão | components/VoiceVideoSettingsPanel.tsx | settings.c interruptor sem efeito (o core já exclui Briks e Discord sempre) | PRESENTE-SEM-EFEITO | sonda |

### Configurações

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| CFG-01 | Abrir Configurações (engrenagem) | views/HomeView.tsx | ui_social.c / settings.c | PRESENTE-FUNCIONAL | config-abas |
| CFG-02 | Navegar pelas abas | views/SettingsView.tsx | settings.c (13 abas) | PRESENTE-FUNCIONAL | config-abas |
| CFG-03 | Fechar com X ou Esc | components/Modal.tsx | settings.c cui_dialog | PRESENTE-FUNCIONAL | config-abas |
| CFG-04 | Desconectar na lateral das Configurações | views/HomeView.tsx | settings.c (Desconectar) | PRESENTE-FUNCIONAL | - |
| CFG-05 | Escolher microfone | components/VoiceVideoSettingsPanel.tsx | settings.c lista winmm e grava mic; o core usa o padrão | PRESENTE-SEM-EFEITO | sonda |
| CFG-06 | Nivel do microfone ao vivo | components/VoiceVideoSettingsPanel.tsx | settings.c (barra ligada a me_level na sala) | PRESENTE-FUNCIONAL | - |
| CFG-07 | Testar o microfone fora da chamada | components/useMicrophoneTest.ts | (o texto da tela diz que 'chega com o áudio nativo') | AUSENTE | sonda |
| CFG-08 | Volume de entrada (ganho) | components/VoiceVideoSettingsPanel.tsx | settings.c slider; só salvo | PRESENTE-SEM-EFEITO | sonda |
| CFG-09 | Escolher saida de áudio | components/VoiceVideoSettingsPanel.tsx | settings.c dropdown; só salvo | PRESENTE-SEM-EFEITO | sonda |
| CFG-10 | Volume de saida | components/VoiceVideoSettingsPanel.tsx | settings.c slider; só salvo | PRESENTE-SEM-EFEITO | sonda |
| CFG-11 | Supressão de ruído (Padrão/RNNoise/Desligada) | components/VoiceVideoSettingsPanel.tsx | settings.c segmentado; só salvo | PRESENTE-SEM-EFEITO | sonda |
| CFG-12 | Porta de ruído com limite | components/VoiceVideoSettingsPanel.tsx | settings.c; só salvo | PRESENTE-SEM-EFEITO | sonda |
| CFG-13 | Cancelamento de eco | components/VoiceVideoSettingsPanel.tsx | settings.c; só salvo | PRESENTE-SEM-EFEITO | sonda |
| CFG-14 | Salvamento automatico das configurações | components/VoiceVideoSettingsPanel.tsx | settings.c (settings.cfg) | PRESENTE-FUNCIONAL | - |
| CFG-15 | Temas prontos (presets) e troca na hora | components/ThemePanel.tsx | themes.c | PRESENTE-FUNCIONAL | config-abas |
| CFG-16 | Criar, salvar, editar, duplicar e excluir temas personalizados | components/ThemePanel.tsx | themes.c (Meus gradientes) | PRESENTE-FUNCIONAL | - |
| CFG-17 | Restaurar o tema padrão | components/ThemePanel.tsx (Restaurar tema Briks) | themes.c | PRESENTE-FUNCIONAL | - |
| CFG-18 | Uso de disco local | components/LocalStoragePanel.tsx | settings.c (tamanho da pasta) | PRESENTE-FUNCIONAL | config-abas |
| CFG-19 | Divisão do espaco por tipo (mensagens, grupos, anexos) | components/LocalStoragePanel.tsx | settings.c: 'depende do backend de dados' | AUSENTE | sonda |
| CFG-20 | Limpar anexos | components/LocalStoragePanel.tsx | settings.c: mostra 'Sem efeito real ainda' | PRESENTE-SEM-EFEITO | sonda |
| CFG-21 | Apagar histórico de mensagens (com confirmacao) | components/LocalStoragePanel.tsx | settings.c: confirma e não apaga | PRESENTE-SEM-EFEITO | sonda |
| CFG-22 | Exportar meus dados | components/LocalStoragePanel.tsx | settings.c: botão sem ação real | PRESENTE-SEM-EFEITO | sonda |
| CFG-23 | Sobreposição em jogos: interruptor geral | components/OverlaySettingsPanel.tsx | settings.c; briks_overlay.exe existe mas o app não o abre | PRESENTE-SEM-EFEITO | sonda |
| CFG-24 | Sobreposição: o que aparece (4 itens) e opacidade | components/OverlaySettingsPanel.tsx | settings.c; só salvo | PRESENTE-SEM-EFEITO | sonda |
| CFG-25 | Sobreposição: editar posições (arrastar, redimensionar) | components/OverlaySettingsPanel.tsx | settings.c: botão sem ação | PRESENTE-SEM-EFEITO | sonda |
| CFG-26 | Aba Avisos: lista e limpar | views/SettingsView.tsx | settings.c: lista existe, mas nada gera avisos | PRESENTE-SEM-EFEITO | sonda |
| CFG-27 | Sobre: versão, licenca e código-fonte | components/AboutPanel.tsx | settings.c (Sobre) | PRESENTE-FUNCIONAL | config-abas |
| CFG-28 | Telemetria anonima ligar/desligar | components/AboutPanel.tsx; services/telemetry | settings.c; só salvo, nada e enviado | PRESENTE-SEM-EFEITO | sonda |
| CFG-29 | Novidades da versão em uso | components/release-notes.ts | (não existe) | AUSENTE | sonda |

### Rede

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| RDE-01 | Modo da chamada (Automático, Sempre P2P, Sempre SFU) | components/NetworkSettingsPanel.tsx | settings.c; só salvo | PRESENTE-SEM-EFEITO | sonda |
| RDE-02 | Preferência de conexão (Automático, Internet direta, VPN) | components/NetworkSettingsPanel.tsx | settings.c; só salvo | PRESENTE-SEM-EFEITO | sonda |
| RDE-03 | Redes privadas detectadas (ZeroTier, Tailscale, WireGuard) | desktop network-interfaces.ts | (não existe) | AUSENTE | sonda |
| RDE-04 | Teste de rede (STUN/relay, variacao, perda, 'precisa de relay') | components/NetworkSettingsPanel.tsx | (não existe) | AUSENTE | sonda |
| RDE-07 | Indicador Somente STUN / STUN + TURN | services/rtc/turn-credentials.ts | settings.c (DIAGNÓSTICO, ice_note) | PRESENTE-FUNCIONAL | - |
| RDE-05 | Diagnóstico: signaling, canal, pessoas | components/CallWorkspace.tsx (Diagnóstico) | settings.c (DIAGNOSTICO) | PRESENTE-FUNCIONAL | config-abas |
| RDE-06 | TURN com credenciais da conta / configuração na interface | services/rtc/turn-credentials.ts | só por BRIKS_ICE_SERVERS_JSON ou ice.json | AUSENTE | sonda |

### Atualização

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| ATU-01 | Versão atual e estado (baixando, pronta, erro) | components/UpdatesPanel.tsx; useUpdateState.ts | settings.c (ATUALIZACOES) + update.c | PRESENTE-FUNCIONAL | config-abas |
| ATU-02 | Banner 'Reiniciar e instalar' / 'Depois' | components/UpdateBanner.tsx | ui_update.c | PRESENTE-FUNCIONAL | - |
| ATU-03 | Instalar ao fechar o app | desktop updater.ts | update.c update_install_on_exit | PRESENTE-FUNCIONAL | - |
| ATU-04 | Botão 'Verificar agora' | components/UpdatesPanel.tsx; ipc updater:check | (não existe: só verifica sozinho a cada 4 h) | AUSENTE | sonda |

### Tray/janela

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| TRY-01 | Ícone na bandeja do sistema | desktop tray.ts | (não existe) | AUSENTE | sonda |
| TRY-02 | Menu da bandeja: Abrir app, Reiniciar app, Fechar o app | desktop tray.ts | (não existe) | AUSENTE | sonda |
| TRY-03 | Clique no ícone da bandeja abre a janela | desktop tray.ts | (não existe) | AUSENTE | sonda |
| TRY-04 | Fechar a janela envia para a bandeja e a chamada continua | desktop window.ts (evento close) | (fechar encerra o app) | AUSENTE | sonda |
| TRY-05 | Instância única (segunda abertura foca a primeira) | desktop main.ts requestSingleInstanceLock | (não existe) | AUSENTE | sonda |
| TRY-06 | Iniciar com o Windows | desktop (setLoginItemSettings) | (não existe) | AUSENTE | sonda |
| TRY-07 | Links briks:// (convites e mensagens) | desktop protocol.ts | msgui.c só COPIA briks://mensagem; o protocolo não está registrado | AUSENTE | sonda |
| TRY-08 | Abrir links externos no navegador | desktop ipc shell:open-external | (não existe: links não são clicaveis) | AUSENTE | sonda |
| TRY-09 | Barra de título própria com minimizar/maximizar/fechar e versão | desktop window.ts | (janela do SO) | AUSENTE | sonda |
| TRY-10 | Janela com tamanho mínimo e redimensionavel | desktop window.ts | main.c (min 560x480) | PRESENTE-FUNCIONAL | - |
| TRY-11 | Aviso de recuperação quando o backend cai | components/DesktopRecoveryNotice.tsx | ui_auth.c ('Quase la', 'Tentar de novo') | PRESENTE-FUNCIONAL | - |

### Notificações

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| NOT-01 | Aviso: amigo ficou online | services/profile/use-friend-notifications.ts | settings.c interruptor; só salvo | PRESENTE-SEM-EFEITO | sonda |
| NOT-02 | Aviso: amigo comecou a transmitir | services/profile/use-friend-notifications.ts | settings.c interruptor; só salvo | PRESENTE-SEM-EFEITO | sonda |
| NOT-03 | Notificação de nova mensagem (DM e grupo) com o app em segundo plano | services/chat/background-manager.ts | (não existe) | AUSENTE | sonda |
| NOT-04 | Notificação 'pediu para você transmitir' | components/CallWorkspace.tsx | (não existe) | AUSENTE | sonda |
| NOT-05 | Nada e avisado durante a chamada nem logo ao abrir | release 1.2.0 | (sem avisos) | AUSENTE | sonda |

### Atalhos

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| ATL-01 | Atalho global: mutar/desmutar (Ctrl+Shift+M) | services/input/shortcuts.ts; desktop call-shortcuts.ts | settings.c campo de texto; nada registra o atalho | PRESENTE-SEM-EFEITO | sonda |
| ATL-02 | Atalho global: ensurdecer (Ctrl+Shift+D) | idem | idem | PRESENTE-SEM-EFEITO | sonda |
| ATL-03 | Atalho global: transmitir/parar | idem | idem | PRESENTE-SEM-EFEITO | sonda |
| ATL-04 | Apertar para falar (tecla, botão do meio/laterais, atraso ao soltar) | components/ShortcutsPanel.tsx | settings.c; só salvo | PRESENTE-SEM-EFEITO | sonda |
| ATL-05 | Remover atalho e aviso de atalho ocupado | components/ShortcutsPanel.tsx (Remover) | (não existe) | AUSENTE | sonda |
| ATL-06 | Ctrl+Shift+O: editar posições do overlay em qualquer tela | components/OverlaySettingsPanel.tsx | (não existe) | AUSENTE | sonda |

### Extras do CUI (sem equivalente no Electron)

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| EXT-01 | Dock: menu + Painel e adicionar painel | (novo) | dock.c | PRESENTE-FUNCIONAL | dock |
| EXT-02 | Dock: fechar painel | (novo) | dock.c | PRESENTE-FUNCIONAL | dock |
| EXT-03 | Dock: restaurar layout padrão (Configurações > Paineis) | (novo) | dock.c / layout.c | PRESENTE-FUNCIONAL | config-abas |
| EXT-04 | Modos de desempenho (Desempenho) | (novo) | perf.c / settings.c | PRESENTE-FUNCIONAL | config-abas |
| EXT-05 | Cópia de segurança da identidade (frase de 12 palavras) | (novo) | ui_account.c | PRESENTE-FUNCIONAL | - |
| EXT-06 | Favoritar, encaminhar, copiar texto/link/ID, marcar como lida | (novo) | msgui.c | PRESENTE-FUNCIONAL | msg-menu |
| EXT-07 | Lembrete de mensagem | (novo) | msgui.c: grava o lembrete mas não notifica ('sem efeito') | PRESENTE-SEM-EFEITO | sonda |
| EXT-08 | Importar dados do Briks antigo (migracao) | (novo) | migrate.c | PRESENTE-FUNCIONAL | - |
| EXT-09 | Presença: quem pode ver o programa em uso | (novo) | settings.c / profile.c | PRESENTE-FUNCIONAL | config-abas |

### Funções básicas de desktop

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| BAS-01 | Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+Z e Ctrl+Y em todos os campos de texto | Chromium (nativo, sem código próprio) | biblioteca CUI (cui_textedit.c): seleção, copiar, recortar, colar (texto), desfazer e refazer; senha não copia | PRESENTE-FUNCIONAL | - |
| BAS-02 | Menu de contexto (clique direito) nos campos de texto: Recortar, Copiar, Colar, Selecionar tudo | Chromium (nativo, sem código próprio) | (não existe): clique direito num campo não faz nada; só as mensagens têm menu | AUSENTE | sonda |
| BAS-03 | Selecionar com mouse (arrastar, duplo clique para palavra) e Shift+setas nos campos | Chromium (nativo, sem código próprio) | biblioteca CUI (cui_textedit.c): âncora e cursor, Shift+setas, Ctrl+setas por palavra | PRESENTE-FUNCIONAL | - |
| BAS-04 | Selecionar e copiar trecho de texto dentro das mensagens (arrastar o mouse sobre o balão) | Chromium (nativo, sem código próprio); MessageList.tsx | (não existe): texto do balão não é selecionável; só "Copiar texto" da mensagem inteira no menu | AUSENTE | sonda |
| BAS-05 | Copiar a mensagem inteira, o link e o ID pelo menu da mensagem | components/MessageList.tsx | msgui.c: Copiar texto, Copiar link da mensagem, Copiar ID | PRESENTE-FUNCIONAL | - |
| BAS-06 | Esc fecha diálogos, menus, popovers e seletor de emoji | Chromium (nativo, sem código próprio); modais do app | biblioteca (cui_dialog, popover) e msgui.c tratam Esc | PRESENTE-FUNCIONAL | bas-esc |
| BAS-07 | Enter envia a mensagem (Shift+Enter quebra a linha) | components/MessageComposer.tsx | ui_social.c: composer; coberto por MSG-01 | PRESENTE-FUNCIONAL | - |
| BAS-08 | Busca global / troca rápida (Ctrl+K) e busca dentro da conversa (Ctrl+F) | (não existe no Electron também) | (não existe): nenhuma busca de mensagens, amigos ou grupos | AUSENTE | sonda |
| BAS-09 | Tab e Shift+Tab percorrem os campos e botões na ordem visual | Chromium (nativo) | biblioteca (cui_core.c): ordem de foco por quadro, Tab e Shift+Tab; Esc solta o Tab no editor de código | PRESENTE-FUNCIONAL | - |
| BAS-10 | Foco visível (anel no elemento focado) e Enter/Espaço ativando botões | Chromium (nativo) | biblioteca: anel de foco e ativação por teclado nos widgets | PRESENTE-FUNCIONAL | - |
| BAS-11 | Alt+F4 e botão X da janela fecham o app | Electron (BrowserWindow) | Win32 nativo: WM_CLOSE encerra o app e a chamada; sem confirmação durante a chamada | PRESENTE-FUNCIONAL | - |
| BAS-12 | Ícone na bandeja do sistema (clique abre, menu Sair) | desktop tray.ts | (não existe) | AUSENTE | sonda |
| BAS-13 | Minimizar e fechar para a bandeja (a chamada continua) | desktop tray.ts | (não existe): fechar derruba o app e a chamada | AUSENTE | sonda |
| BAS-14 | Iniciar com o Windows | desktop (loginItemSettings) | (não existe) | AUSENTE | sonda |
| BAS-15 | Instância única (abrir de novo traz a janela existente) | desktop (requestSingleInstanceLock) | (não existe): duas instâncias disputam o banco | AUSENTE | sonda |
| BAS-16 | Lembrar posição, tamanho, monitor e maximizado da janela | desktop-lifecycle.ts (só calcula o tamanho inicial pela área de trabalho; não grava) | (não existe): abre sempre 1280x800 no lugar padrão do Windows; layout.json guarda só os painéis | AUSENTE | sonda |
| BAS-17 | Redimensionar, maximizar e restaurar a janela com mínimo de 560x480 | Electron | Win32: WM_SIZE e WM_GETMINMAXINFO; os painéis se reajustam | PRESENTE-FUNCIONAL | - |
| BAS-18 | DPI alto e escala do Windows (PerMonitorV2, troca de monitor com DPI diferente) | Chromium (nativo, sem código próprio) | biblioteca: SetProcessDpiAwarenessContext, WM_DPICHANGED, cui_dpi_scale | PRESENTE-FUNCIONAL | - |
| BAS-19 | Vários monitores: abrir no monitor onde estava e janela fora da área visível voltar para dentro | desktop-lifecycle.ts | (não existe): sem restauração de posição, então depende do Windows | VERIFICAR | - |
| BAS-20 | Notificações do sistema (nova mensagem, amigo online, transmissão) | desktop notifications | (não existe); interruptores de Configurações sem efeito | AUSENTE | sonda |
| BAS-21 | Som de notificação e sons de entrar/sair da chamada | services/audio/call-sounds.ts | (não existe) | AUSENTE | sonda |
| BAS-22 | Badge de não lidas (lista de conversas, rail, ícone da barra de tarefas) | (não existe no Electron também) | msgui.c só mostra "não lida" dentro de tópicos; nada na lista nem na barra de tarefas | AUSENTE | sonda |
| BAS-23 | Arrastar e soltar arquivos na janela ou no composer | components/MessageComposer.tsx (Solte para enviar) | (não existe): a biblioteca já entrega os arquivos soltos (cui_dropped_file) e o app não usa | AUSENTE | sonda |
| BAS-24 | Colar imagem da área de transferência no composer | Chromium (nativo, sem código próprio) | (não existe): o clipboard da plataforma lê só texto (CF_UNICODETEXT) | AUSENTE | sonda |
| BAS-25 | Rolagem com a roda do mouse e o touchpad (vertical e horizontal) | Chromium (nativo, sem código próprio) | biblioteca: WM_MOUSEWHEEL e WM_MOUSEHWHEEL; cui_scroll nas listas e na conversa | PRESENTE-FUNCIONAL | - |
| BAS-26 | Barras de rolagem visíveis e arrastáveis | Chromium (nativo, sem código próprio) | biblioteca: cui_scroll desenha o polegar; editor de código tem barras próprias | PRESENTE-FUNCIONAL | - |
| BAS-27 | Tema claro e escuro (e seguir o do Windows) | (Electron: só tema escuro com cores) | Temas por gradiente (Configurações > Temas), sempre escuros; sem tema claro nem seguir o Windows | AUSENTE | sonda |
| BAS-28 | Idioma da interface (pt-BR, inglês) | (não existe no Electron também) | (não existe): textos fixos em português no código | AUSENTE | sonda |
| BAS-29 | Estados vazios com ícone e texto (sem amigos, sem mensagens, sem histórico) | Electron | ui_social.c e ui_history.c: ícone grande e texto de apoio | PRESENTE-FUNCIONAL | - |
| BAS-30 | Estado de carregamento (spinner ou esqueleto) enquanto o core sobe, entra na sala ou carrega o histórico | Electron | VERIFICAR: há texto de status ("Conectando", "Baixando a atualização"), mas não vi spinner ou esqueleto nas listas; depende do core real | VERIFICAR | - |
| BAS-31 | Mensagens de erro compreensíveis (login, rede, sem conexão com o core) | Electron | account.c e ui_auth.c: avisos em português claro ("Sem conexão com o Briks. Tente de novo…") | PRESENTE-FUNCIONAL | - |
| BAS-32 | Estado offline / sem internet visível ao usuário (banner persistente) | Electron | Só o aviso de erro pontual (account.c) e o texto de Diagnóstico; sem banner persistente na janela principal | AUSENTE | sonda |
| BAS-33 | Reconexão automática da chamada e do signaling com aviso | Electron | rtc.c: estado "Reconectando…" mostrado; reconexão feita pelo core | PRESENTE-FUNCIONAL | - |
| BAS-34 | Sair da conta e trocar de conta | Electron | Botão sair do rodapé e Desconectar em Configurações (AUT-13) | PRESENTE-FUNCIONAL | - |
| BAS-35 | Aviso de atualização disponível/pronta | Electron (electron-updater) | ui_update.c e Configurações > Atualizações (ATU-01) | PRESENTE-FUNCIONAL | - |
| BAS-36 | Sobre: versão do app e licença | Electron | Configurações > Sobre e Atualizações mostram versão e licença AGPL | PRESENTE-FUNCIONAL | - |
| BAS-37 | Licenças de software de terceiros dentro do app | (não existe no Electron também) | (não existe): THIRD_PARTY.md existe no repositório da biblioteca, não dentro do app | AUSENTE | sonda |
| BAS-38 | Contraste legível do texto sobre o fundo dos temas | Electron | themes.c limita a opacidade das manchas pela luminância e avisa quando clareia demais | PRESENTE-FUNCIONAL | - |
| BAS-39 | Tamanho de texto ajustável pelo usuário (zoom da interface) | Chromium (nativo, sem código próprio) (Ctrl+roda/Ctrl+mais) | (não existe): só segue a escala do Windows (BAS-18); sem Ctrl+mais/menos nem opção | AUSENTE | sonda |
| BAS-40 | Leitor de tela e navegação assistiva (UI Automation / MSAA) | Chromium (nativo, sem código próprio) (árvore de acessibilidade) | (não existe): a interface é desenhada pela biblioteca, sem UIA | AUSENTE | sonda |
| BAS-41 | Acentos, AltGr e teclas mortas (ç, ã, é) digitados no teclado ABNT2 | Chromium (nativo, sem código próprio) | biblioteca: WM_CHAR entrega o caractere final; funciona em todos os campos | PRESENTE-FUNCIONAL | - |
| BAS-42 | IME (japonês, chinês, coreano) e emoji do teclado do Windows (Win+.) | Chromium (nativo, sem código próprio) | VERIFICAR: a plataforma trata WM_CHAR mas não WM_IME_COMPOSITION; o texto final de IME e o painel de emoji provavelmente entram, mas sem janela de composição junto ao cursor | VERIFICAR | - |

### Layout novo (trilho, lista, conversa, hub)

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| LAY-01 | Trilho à esquerda: Início e grupos como cartões arredondados, "Criar grupo" no fim | (padrão novo do usuário) | ui_social.c (P_SERVERS): cartões com avatar e texto; conforme | PRESENTE-FUNCIONAL | - |
| LAY-02 | Lista ao lado do trilho: Amigos, Adicionar amigo, Histórico, salas de voz, grupos, amigos e cartão do usuário embaixo | (padrão novo do usuário) | ui_social.c (P_NAV): conforme; item ativo em vidro com acento | PRESENTE-FUNCIONAL | - |
| LAY-03 | Conversa larga com cabeçalho (nome, tipo, Fixadas), mensagens e composer fixo embaixo | (padrão novo do usuário) | ui_social.c (P_CHAT) e msgui.c: conforme, com anexar, campo e enviar na mesma barra | PRESENTE-FUNCIONAL | - |
| LAY-04 | Hub à direita no layout padrão (membros do grupo, sala de voz, presença) | (padrão novo do usuário) | layout_default (layout.c) cria só trilho, lista e conversa; Membros e Sala de voz existem como painéis, mas só entram pelo botão + do dock e o de Membros lista amigos, não o grupo (GRP-10) | AUSENTE | sonda |
| LAY-05 | Tela inicial "Amigos" com abas Online/Todos/Pendentes e cartões de amigo | (padrão novo do usuário) | Amigos abre só um estado vazio "Escolha uma conversa", sem abas nem lista de pedidos (AMI-08) | AUSENTE | sonda |
| LAY-06 | Histórico de chamadas como painel do layout (hoje cobre a janela inteira, sem trilho nem lista) | (padrão novo do usuário) | ui_history.c: o item da lista abre tela cheia com seta de voltar e só o estado vazio; existe também o painel Histórico do dock (+ painel), mas o item da lista não o usa | AUSENTE | sonda |
| LAY-07 | Aba Chat da chamada no padrão de conversa (balões, avatares, horário, composer embaixo) | (padrão novo do usuário) | ui_call.c: texto cru "Nome / frase" sem balão, avatar, hora nem campo de envio (MSG-19) | AUSENTE | - |
| LAY-08 | Tela de chamada em tela cheia (imersiva) em vez de dentro do layout trilho/lista/hub | (padrão novo do usuário) | ui_call.c: cabeçalho próprio, grade de participantes e barra de controles flutuante; esconde trilho e lista. Decisão de design, não defeito | VERIFICAR | - |
| LAY-09 | Diálogos (Configurações, perfil, criar grupo, excluir mensagem) em vidro, cantos arredondados, fundo escurecido | (padrão novo do usuário) | cui_dialog em settings.c, profile.c, msgui.c: conforme; 13 abas de Configurações em coluna à esquerda | PRESENTE-FUNCIONAL | - |
| LAY-10 | Tela de entrada (login, criar conta, recuperar) em cartão central de vidro | (padrão novo do usuário) | ui_auth.c: cartão central, título grande, botões Google/Discord e links: conforme | PRESENTE-FUNCIONAL | - |
| LAY-11 | Texto sempre legível: tamanho mínimo nas etiquetas (rótulos de seção em 10 a 11 px) | (padrão novo do usuário) | Rótulos "SALAS DE VOZ", "GRUPOS", "AMIGOS", dicas e legendas dos balões usam 10 a 11 px, abaixo do padrão de "textos maiores" pedido; corpo e títulos estão bons | AUSENTE | - |
| LAY-12 | Dica "Sair" (tooltip) presa no topo da tela e círculo no canto inferior esquerdo em todas as capturas | (padrão novo do usuário) | Nas capturas ocultas o tooltip do botão sair do rodapé aparece fixo em x=640 y=20; já listado em plano-pos-1.3.0 (frente D) | AUSENTE | - |
| LAY-13 | Janela mínima 560x480 sem corte ou sobreposição (captura gerada pelo roteiro) | (padrão novo do usuário) | O roteiro abre o app em 560x480 e captura; o teste só prova que abre e captura, a análise do desenho é visual na captura lay-min/capturas/l0.png | PRESENTE-FUNCIONAL | lay-min |

## Achados fora do escopo do inventário (bugs vistos pelo roteiro)

- **Clique atravessa os diálogos.** `briks_clicked()` (`briks_ui.h`) só confere o retângulo do widget e o clique, sem considerar que um diálogo modal está por cima. Com Configurações aberta, clicar na aba "Rede" (x 224-424, y 159) também aciona o item "Histórico de chamadas" da barra lateral que fica embaixo (x 277-489); a aba "Sobreposição" (y 264) entra na "Sala geral". O roteiro oculto reproduziu isso com o mouse injetado pelo mesmo caminho do mouse real; vale confirmar com o mouse físico. Não corrigi (fora do escopo: só inventário e testes). Os roteiros de Configurações clicam nas abas em x=240, fora da barra lateral, para não disparar isso.
- O tooltip "Sair" aparece preso no topo da tela em todas as capturas da tela social (já listado em `plano-pos-1.3.0.md`, frente D).

## Última execução do roteiro

| Resultado | Itens |
|---|---|
| AUSENTE | 90 |
| PASSOU | 52 |
| REVER | 2 |
| SEM-EFEITO | 33 |
| SEM-TESTE | 64 |
| VERIFICAR | 7 |

PASSOU = a ação teve o efeito visível esperado no app oculto; SEM-TESTE = item PRESENTE-FUNCIONAL sem roteiro automático (depende do core, da rede, de GPU ou de conferência manual); SEM-EFEITO/AUSENTE = confirmado pelo clique e/ou pela sonda; REVER = sonda ou clique indica que algo mudou; reclassifique.

Nota: os 2 REVER dessa execução (BAS-32 e LAY-06) eram sondas largas demais (achavam o texto "sem internet" do modo de desenvolvimento e o painel Histórico do dock). Corrigi as sondas no `itens.csv` e conferi com nova passada das sondas: nenhum REVER. Nenhum caso executável com FALHOU.

## Roteiro automatizado oculto

Um comando: `powershell -NoProfile -File apps\desktop-cui\scripts\paridade.ps1` (ou `scripts\paridade.ps1` na raiz do repositório). Pré-requisito: `apps\desktop-cui\build\briks_cui.exe` compilado (`apps\desktop-cui\build.ps1`).

- Abre o app **oculto** (`BRIKS_HIDDEN=1`, modo demo `BRIKS_DEMO`, 1280x800, `BRIKS_NO_BREATH=1` para o fundo não animar), uma instância por vez, com layout, tema, configurações e dados em pasta temporária (`BRIKS_LAYOUT_FILE`, `BRIKS_THEME_FILE`, `BRIKS_SETTINGS_FILE`, `BRIKS_CORE_DATA`, `BRIKS_CUI_DATA`), sem importação do Briks antigo (`BRIKS_NO_IMPORT`) e sem checar atualização. Nunca captura o monitor: as imagens são geradas pelo próprio app (`shot` do `BRIKS_SCRIPT`, mesmo caminho de `CUI_SCREENSHOT`).
- Roteiros em `apps/desktop-cui/scripts/paridade/*.txt`: usam os comandos do modo roteiro do app (`click`, `at`, `rdown`, `rup`, `waitms`, `shot`) mais `type <texto>` e `key enter|tab|backspace|esc` (acrescentados em `src/script.c`, só harness de teste) e dois atalhos de macro expandidos pelo runner: `snap <nome>` (cursor para o canto, espera, captura) e `tap X Y` (clique de verdade).
- `testes.json` liga cada par de capturas a um ou mais itens: `muda` (a ação teve efeito visível) ou `igual` (voltou ao estado anterior ou o controle é inerte). Itens PRESENTE-SEM-EFEITO só usam `igual`: se um dia o clique passar a mudar a tela, o relatório mostra REVER.
- Itens AUSENTE e SEM-EFEITO têm uma **sonda** (regex em `itens.csv`) que procura o código que ainda não deveria existir em `src/*.c` e `briks-core/src/**/*.rs`; se achar, o item vira REVER (alguém implementou; reclassifique).
- Saída: `relatorio.md` e `relatorio.csv` em `%TEMP%\briks-paridade` (ou `-Saida`), com PASSOU / FALHOU / SEM-TESTE / SEM-EFEITO / AUSENTE / REVER / VERIFICAR por item, e as capturas de cada caso. Código de saída 1 se houver FALHOU ou REVER. O runner confere que `%APPDATA%\Briks-CUI` não foi alterado e fecha todo processo iniciado a partir da pasta de build.
- Para cobrir um item novo: acrescente a linha em `itens.csv`, um roteiro em `paridade/` e a checagem em `testes.json`; depois regenere esta página.

## Extras do CUI que o Electron não tinha

- **EXT-01** Dock: menu + Painel e adicionar painel (PRESENTE-FUNCIONAL)
- **EXT-02** Dock: fechar painel (PRESENTE-FUNCIONAL)
- **EXT-03** Dock: restaurar layout padrão (Configurações > Paineis) (PRESENTE-FUNCIONAL)
- **EXT-04** Modos de desempenho (Desempenho) (PRESENTE-FUNCIONAL)
- **EXT-05** Cópia de segurança da identidade (frase de 12 palavras) (PRESENTE-FUNCIONAL)
- **EXT-06** Favoritar, encaminhar, copiar texto/link/ID, marcar como lida (PRESENTE-FUNCIONAL)
- **EXT-07** Lembrete de mensagem (PRESENTE-SEM-EFEITO)
- **EXT-08** Importar dados do Briks antigo (migracao) (PRESENTE-FUNCIONAL)
- **EXT-09** Presença: quem pode ver o programa em uso (PRESENTE-FUNCIONAL)

## Em andamento em outros branches (não contado aqui)

Base deste inventário é o brisk-dev. Branches com trabalho que muda classificações: `cui-ui-paridade` (grupos, social, `grupos-p2p-core.md`), `cui-mensagens` (anexos, estados do chat, prévia de links), `cui-paineis` (chamada completa, câmera, backends das Configurações), `cui-contas` (rede/TURN, foto, recuperação), `cui-transmissao` e `cui-instalador`. Rode `paridade.ps1` depois de cada integração: a sonda acusa REVER quando algo AUSENTE passa a existir.
