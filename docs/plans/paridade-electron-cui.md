# Paridade Electron x CUI: inventário e roteiro de teste

Base: `brisk-dev` em db2c4ee (release v1.3.0). Fonte do Electron: branch `backup/electron-pre-cui` (`apps/web/src`, `apps/desktop/src`, versão 1.2.x). Fonte do CUI: `apps/desktop-cui` (`src/*.c`, `briks-core/src/*.rs`).
Este arquivo é gerado a partir de `apps/desktop-cui/scripts/paridade/itens.csv` (a fonte da verdade, uma linha por item) e substitui, para fins de acompanhamento, a lista de lacunas anterior (`cui-ui-paridade-lacunas.md`, que não existe no brisk-dev; ver o arquivo de mesmo nome, que agora aponta para cá). O inventário anterior por feature continua em `auditoria-features-cui.md`.

## Resultado em números

| Classe | Itens |
|---|---|
| PRESENTE-FUNCIONAL | 204 |
| PRESENTE-SEM-EFEITO | 9 |
| AUSENTE | 28 |
| VERIFICAR | 6 |
| **Total** | **248** |

Itens PRESENTE-FUNCIONAL com teste automático oculto: 111 de 204 (os demais dependem de core/GPU/rede real ou de conferência manual e saem como SEM-TESTE no relatório).

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
| P1 | AMI-06 | Amigos | Pedidos pendentes: recusar | SEM-EFEITO | components/P2PInvitePanel.tsx (Recusar) | core aceita friends.respond accept=false, mas a UI só tem Aceitar | Segundo ícone (X, secundario) ao lado do check em cada linha de PEDIDOS (ui_social.c), tooltip Recusar |
| P1 | AMI-07 | Amigos | Desfazer amizade (com confirmacao) | AUSENTE | views/FriendsView.tsx; HomeView.tsx deleteTarget | core tem friends.remove; sem UI | Item 'Desfazer amizade' no menu de contexto da linha do amigo (person_row) + diálogo de confirmacao no estilo Expulsar (ui_call.c) |
| P1 | AMI-08 | Amigos | Abas Online / Todos / Pendentes com contadores | AUSENTE | views/FriendsView.tsx | ui_social.c: uma lista única AMIGOS e PEDIDOS, sem filtro | cui_segmented no topo do painel Amigos (mesmo componente do 'Limpar depois de'), contadores nos rotulos; pendentes sobem para a aba |
| P1 | AMI-10 | Amigos | Indicador online/offline do amigo | AUSENTE | views/HomeView.tsx (title Online/Offline) | ui_social.c só mostra 'Disponivel' no próprio rodape | Bolinha 8 px (B_ONLINE / cinza) no canto do avatar em person_row; presença vem do core |
| P1 | CFG-13 | Configurações | Cancelamento de eco | AUSENTE | components/VoiceVideoSettingsPanel.tsx | settings.c: interruptor desabilitado com a nota em breve (sem AEC no core) | Já existe; AEC no core |
| P1 | CHM-04 | Chamada | Câmera (ligar e enviar vídeo) | SEM-EFEITO | components/CallWorkspace.tsx (Câmera) | ui_call.c: pílula desabilitada 'Câmera · em breve' (decisão: sem captura nem envio nesta versão; ver docs/plans/chamada-pacote-c.md) | Pílula nova na barra flutuante de controles da chamada (pill() em ui_call.c, raio 28, vidro forte), com rótulo e estado ativo em violeta; captura e envio no core (Media Foundation + encoder já usados na transmissão) |
| P1 | CHM-14 | Chamada | Volume por pessoa (slider) lembrado nas próximas chamadas | AUSENTE | services/áudio/person-áudio.ts | só liga/desliga mudo, sem volume e sem memória | Menu de contexto do bloco da pessoa na chamada (pmenu em ui_call.c, cui_menu_item), mais um item com ícone com cui_slider 0-200%; salvar por peerId em settings |
| P1 | LAY-05 | Layout novo (trilho, lista, conversa, hub) | Tela inicial "Amigos" com abas Online/Todos/Pendentes e cartões de amigo | AUSENTE | (padrão novo do usuário) | Amigos abre só um estado vazio "Escolha uma conversa", sem abas nem lista de pedidos (AMI-08) | Painel central em vez do estado vazio: cabeçalho com abas em pílula (padrão do seletor Call/Transmissão da chamada), linhas largas de amigo com ações e a lista de pedidos; hub à direita mostra "Ativos agora" |
| P1 | RDE-01 | Rede | Modo da chamada (Automático, Sempre P2P, Sempre SFU) | SEM-EFEITO | components/NetworkSettingsPanel.tsx | settings.c; só salvo | Já existe; passar o modo ao core ao entrar na sala |
| P1 | RDE-02 | Rede | Preferência de conexão (Automático, Internet direta, VPN) | SEM-EFEITO | components/NetworkSettingsPanel.tsx | settings.c; só salvo | Já existe; filtrar interfaces/candidatos ICE no core (ice.rs) |
| P1 | RDE-04 | Rede | Teste de rede (STUN/relay, variacao, perda, 'precisa de relay') | AUSENTE | components/NetworkSettingsPanel.tsx | (não existe) | Botão 'Testar rede' no cartão DIAGNOSTICO de Configurações > Rede, resultado em linhas 13 px com selos de cor |
| P1 | RDE-06 | Rede | TURN com credenciais da conta / configuração na interface | AUSENTE | services/rtc/turn-credentials.ts | só por BRIKS_ICE_SERVERS_JSON ou ice.json | Campos no cartão PREFERENCIA DE CONEXAO (URL, usuário, senha) ou credenciais automaticas da conta; indicador 'Somente STUN' como chip |
| P1 | TRX-11 | Transmissão | SFU (LiveKit) como padrão para salas grandes e redes dificeis | AUSENTE | services/rtc/transport-factory | só malha P2P; a opção 'Sempre SFU' em Configurações não faz nada | Sem UI nova: o seletor Modo da chamada (Rede) passa a valer; indicador 'via servidor' no chip de rede da chamada |
| P2 | AMI-09 | Amigos | Buscar amigo | AUSENTE | views/FriendsView.tsx | (não existe) | cui_input com lupa no topo do painel Amigos (altura 36, raio 12), filtra a lista ao digitar |
| P2 | AMI-11 | Amigos | Painel Atividade (quem está em sala de voz nos grupos) | AUSENTE | views/FriendsView.tsx | (não existe) | Painel novo 'Atividade' no dock (menu + Painel), cartões de vidro por grupo com avatares de quem está na sala |
| P2 | AMI-15 | Amigos | Sino para silenciar avisos de cada amigo | AUSENTE | services/profile/use-friend-notifications.ts | (não existe) | Ícone de sino 14 px no menu de contexto do amigo (person_row); depende de NOT-01/02 |
| P2 | ATU-04 | Atualização | Botão 'Verificar agora' | AUSENTE | components/UpdatesPanel.tsx; ipc updater:check | (não existe: só verifica sozinho a cada 4 h) | Botão secundario no cartão ATUALIZACOES (settings.c) com estado 'Verificando...' (loading) e resultado em 12 px |
| P2 | CFG-29 | Configurações | Novidades da versão em uso | AUSENTE | components/release-notes.ts | (não existe) | Cartão NOVIDADES na aba Sobre/Atualizacoes, lista de 12-13 px, e diálogo de vidro 'O que há de novo' após atualizar |
| P2 | CHM-12 | Chamada | Zoom na transmissão (roda, pinca, arrastar, selo de %) | AUSENTE | components/vídeo-zoom.tsx | (não existe) | Selo de vidro 11 px no canto superior esquerdo do bloco (dica de zoom); arrastar move; sem painel |
| P2 | CHM-16 | Chamada | Desconexao após 1 h sem falar, com aviso e Continuar | AUSENTE | CallWorkspace.tsx; release 1.1.0 | (não existe) | Banner de vidro no topo da chamada nos últimos 5 min com botão Continuar (update_banner), contagem 13 px |
| P2 | CHM-24 | Chamada | Vídeo remoto, câmera dos outros e 2a tela | AUSENTE | components/CallWorkspace.tsx | (não existe; depende da câmera e do pacote de transmissão: ver docs/plans/chamada-pacote-c.md) | Tile de vídeo no palco da chamada no lugar do avatar; 2a tela como tile extra com selo |
| P2 | LAY-12 | Layout novo (trilho, lista, conversa, hub) | Dica "Sair" (tooltip) presa no topo da tela e círculo no canto inferior esquerdo em todas as capturas | AUSENTE | (padrão novo do usuário) | Nas capturas ocultas o tooltip do botão sair do rodapé aparece fixo em x=640 y=20; já listado em plano-pos-1.3.0 (frente D) | Corrigir a âncora do tooltip do rodapé (cui_tooltip) para o botão, e esconder o ponteiro do roteiro nas capturas |
| P2 | MSG-08 | Mensagens | Indicador 'digitando...' | AUSENTE | components/ConversationTimeline.tsx | (não existe) | Linha de 12 px cor B_MUTED acima do compositor, com três pontos animados (respeita reduced motion) |
| P2 | PRF-06 | Perfil | Limpar o status depois de 30 min/1 h/4 h/hoje | SEM-EFEITO | components/StatusEditor.tsx | ui_social.c pf-clear grava status_clear, mas nada o le: o status nunca expira | Aplicar a expiracao em profile.c (timer) e mostrar 'some em X' sob o campo, 11 px, cor B_MUTED |
| P2 | PRF-12 | Perfil | Outros computadores ligados ao @nick (listar e revogar) | AUSENTE | components/AccountPanel.tsx (Revogar) | (não existe) | Cartão 'Computadores' na aba Conta do diálogo Perfil e conta: linhas com nome do PC, data e botão Revogar secundario, confirmacao em diálogo |
| P2 | RDE-03 | Rede | Redes privadas detectadas (ZeroTier, Tailscale, WireGuard) | AUSENTE | desktop network-interfaces.ts | (não existe) | Linha 'Detectado' com selo verde dentro do cartão PREFERENCIA DE CONEXAO |
| P3 | AMI-16 | Amigos | Desconectar só o chat privado (Desconectar P2P) | AUSENTE | views/HomeView.tsx | (não existe) | Item no menu de contexto do amigo; texto 13 px; confirmacao simples |
| P3 | ATL-06 | Atalhos | Ctrl+Shift+O: editar posições do overlay em qualquer tela | AUSENTE | components/OverlaySettingsPanel.tsx | (não existe) | Depende de CFG-25 |
| P3 | BAS-27 | Funções básicas de desktop | Tema claro e escuro (e seguir o do Windows) | AUSENTE | (Electron: só tema escuro com cores) | Temas por gradiente (Configurações > Temas), sempre escuros; sem tema claro nem seguir o Windows | Opção nova no cartão Temas: "Claro" e "Seguir o Windows"; o gerador de tema (themes.c) precisa de uma variante clara com o mesmo contraste mínimo; fora do escopo do Electron |
| P3 | BAS-28 | Funções básicas de desktop | Idioma da interface (pt-BR, inglês) | AUSENTE | (não existe no Electron também) | (não existe): textos fixos em português no código | Tabela de strings e seletor de idioma no cartão Sobre de Configurações; só depois do conteúdo final dos painéis |
| P3 | BAS-40 | Funções básicas de desktop | Leitor de tela e navegação assistiva (UI Automation / MSAA) | AUSENTE | Chromium (nativo, sem código próprio) (árvore de acessibilidade) | (não existe): a interface é desenhada pela biblioteca, sem UIA | Sem tela: provedor UIA mínimo na plataforma (nome, papel e estado de botões e campos); alto custo, vale só depois do resto |
| P3 | CFG-25 | Configurações | Sobreposição: editar posições (arrastar, redimensionar) | AUSENTE | components/OverlaySettingsPanel.tsx | settings.c: nota em breve; só os 4 cantos são reais (CFG-24) | Botão já existe; modo de edicao do overlay com atalho Ctrl+Shift+O (ATL-06) |
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

## Tabela completa por área

### Auth

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| AUT-01 | Entrar com e-mail e senha | views/AuthView.tsx | ui_auth.c + account.c (account.login) | PRESENTE-FUNCIONAL | auth-a |
| AUT-02 | Criar conta com e-mail (nome, e-mail, senha, repetir senha) | views/AuthView.tsx | ui_auth.c (signup) | PRESENTE-FUNCIONAL | auth-b |
| AUT-03 | Data de nascimento no cadastro (só maiores de 18) | views/AuthView.tsx; services/account/age.ts | Campo Data de nascimento no cadastro (accountx.c, ui_auth.c): só maiores de 18; valida na hora e a data não é guardada | PRESENTE-FUNCIONAL | auth-nasc |
| AUT-04 | Esqueci minha senha (e-mail de recuperação) | views/AuthView.tsx | ui_auth.c (recover) | PRESENTE-FUNCIONAL | auth-a |
| AUT-05 | Código de 6 digitos / link de recuperação e nova senha | views/AuthView.tsx | ui_auth.c (code, recovery) | PRESENTE-FUNCIONAL | - |
| AUT-06 | Continuar com Google (navegador) | components/AccountPanel.tsx; services/account/account-service.ts | ui_auth.c account_oauth(google); não testado com provedor real | PRESENTE-FUNCIONAL | - |
| AUT-07 | Continuar com Discord (navegador) | components/AccountPanel.tsx | ui_auth.c account_oauth(discord); não testado com provedor real | PRESENTE-FUNCIONAL | - |
| AUT-08 | Cancelar a espera do navegador | views/AuthView.tsx | ui_auth.c (Cancelar / account.cancel) | PRESENTE-FUNCIONAL | - |
| AUT-09 | Mostrar/ocultar a senha (olho) | views/AuthView.tsx | ui_auth.c (cui_input password) | PRESENTE-FUNCIONAL | auth-c |
| AUT-10 | Alternar entre entrar, criar conta e Google/Discord | views/AuthView.tsx | ui_auth.c (links) | PRESENTE-FUNCIONAL | auth-c |
| AUT-11 | Entrar digitando o @nick (dica local de e-mail) | services/account/login-hints.ts | Campo de e-mail do login aceita @nick (dica guardada só neste computador em login-hints.txt, accountx.c); @nick desconhecido mostra mensagem clara | PRESENTE-FUNCIONAL | auth-nick |
| AUT-12 | Tela de carregamento ao restaurar a sessao | App.tsx (Restaurando sua sessao) | ui_auth.c (Preparando seu espaco) | PRESENTE-FUNCIONAL | - |
| AUT-13 | Sair da conta (rodape, Perfil e Configurações) | views/HomeView.tsx (Sair, Desconectar) | ui_social.c / settings.c / app.c app_logout | PRESENTE-FUNCIONAL | perfil |
| AUT-14 | Erros de login em português claro | views/auth-errors.ts | account.c (mensagens) | PRESENTE-FUNCIONAL | - |

### Perfil

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| PRF-01 | Editar nome exibido | components/ProfileEditor.tsx | ui_social.c / ui_account.c (Nome, Salvar) | PRESENTE-FUNCIONAL | perfil |
| PRF-02 | Escolher/trocar foto de perfil | components/ProfileEditor.tsx; ProfileAvatar.tsx | ui_account.c account_pick_photo | PRESENTE-FUNCIONAL | - |
| PRF-03 | Mostrar a foto dos OUTROS em amigos, membros, tiles da chamada e mensagens | components/ProfileAvatar.tsx | Foto dos amigos (core: friends.photos baixa para o cache; UI: lista lateral, hub de Amigos, membros e bolhas das mensagens). Tiles da chamada ainda com iniciais | PRESENTE-FUNCIONAL | perfil-fotos |
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
| AMI-04 | Adicionar amigo por código de convite P2P (criar convite, usar código, aceitar, recusar) | components/P2PInvitePanel.tsx; services/invites/service.ts | ui_friends.c friends_code_card (dialogo Adicionar amigo e aba Adicionar do hub): Gerar meu codigo, Copiar, Colar e Adicionar; core friend_invite.rs (friend.invite/friend.accept, codigo briks-amg1.*, assinado, vale 7 dias) [teste amigos-codigo + cargo friend_invite]. LIMITES: sem aba "Por @nick/Por codigo" (e um cartao abaixo do @nick); aceitar grava em friends-p2p.json do core e entra na lista so nesta sessao; o amigo P2P ainda nao e usado em chamadas; aceitar codigo de OUTRA pessoa so testado em Rust (o roteiro oculto aceita o proprio codigo e ve a recusa); convite nao e de uso unico (sem servidor) | PRESENTE-FUNCIONAL | amigos-codigo |
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
| AMI-17 | Entrar em grupo por convite/código | views/HomeView.tsx (Entrar em grupo); GroupInvitePanel.tsx | ui_social.c panel_servers: tile Entrar em grupo (+ item Entrar com convite da lateral) [teste grupos-menu] | PRESENTE-FUNCIONAL | grupos-menu |

### Grupos, canais e cargos

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| GRP-01 | Criar grupo | views/HomeView.tsx (Criar grupo) | ui_social.c (create_group_open) | PRESENTE-FUNCIONAL | social-b |
| GRP-02 | Lista de grupos e abrir o grupo | views/HomeView.tsx | ui_social.c / painel Servidores | PRESENTE-FUNCIONAL | social-b |
| GRP-03 | Conversa do canal de texto geral do grupo | views/GroupView.tsx | ui_social.c (Grupo P2P canal geral) | PRESENTE-FUNCIONAL | social-b |
| GRP-04 | Criar canal de texto | views/HomeView.tsx (Criar canal) | ui_groups.c groups_sidebar/groups_dialogs: Novo canal (texto) [teste grupos-canais] | PRESENTE-FUNCIONAL | grupos-canais |
| GRP-05 | Criar sala de voz do grupo | views/HomeView.tsx | ui_groups.c: Novo canal aba Voz; sala de voz do grupo [teste grupos-canais] | PRESENTE-FUNCIONAL | grupos-canais |
| GRP-06 | Renomear canal/sala | views/HomeView.tsx | ui_groups.c: lápis no hover do canal + diálogo Renomear canal [teste grupos-canais] | PRESENTE-FUNCIONAL | grupos-canais |
| GRP-07 | Apagar canal/sala | views/HomeView.tsx | ui_groups.c: lixeira no hover + confirmação Apagar canal (sempre sobra um de texto) [teste grupos-canais] | PRESENTE-FUNCIONAL | grupos-canais |
| GRP-08 | Personalizar grupo (nome e imagem, sincroniza aos membros) | components/GroupEditor.tsx | ui_groups.c: Personalizar grupo (nome, imagem, canais) pelo menu do grupo/cabeçalho [teste grupos-menu] | PRESENTE-FUNCIONAL | grupos-menu |
| GRP-09 | Apagar ou sair do grupo | views/HomeView.tsx (Apagar ou sair do grupo) | ui_groups.c groups_overlay: menu de contexto (botão direito) da linha do grupo + Sair/Apagar com confirmação [testes grupos-canais, grupos-menu] | PRESENTE-FUNCIONAL | grupos-canais, grupos-menu |
| GRP-10 | Lista de membros do grupo com marcas VOCÊ/DONO/Admin/@nick | views/HomeView.tsx (membros) | ui_groups.c groups_members_panel: painel Membros do dock mostra os membros do GRUPO aberto com DONO/ADMIN/VOCÊ [teste grupos-membros] | PRESENTE-FUNCIONAL | grupos-membros, grupos-comum |
| GRP-11 | Recarregar lista de membros | views/HomeView.tsx | ui_groups.c: ícone recarregar no painel Membros (reload_groups); sem teste (precisa do backend) | PRESENTE-FUNCIONAL | - |
| GRP-12 | Adicionar membro (amigo) ao grupo | views/HomeView.tsx (Adicionar membro) | ui_groups.c: botão + do painel Membros e diálogo Membros (amigos e @nick); sem teste de ponta a ponta (convite assinado precisa do core) | PRESENTE-FUNCIONAL | - |
| GRP-13 | Gerenciar membro: tornar/remover administrador | views/HomeView.tsx (Gerenciar membro) | ui_groups.c: menu ... da linha no painel Membros (só o dono) [teste grupos-membros] | PRESENTE-FUNCIONAL | grupos-membros |
| GRP-14 | Remover membro do grupo | views/HomeView.tsx removeSelectedMember | ui_groups.c: Remover do grupo + confirmação [teste grupos-membros] | PRESENTE-FUNCIONAL | grupos-membros |
| GRP-15 | Reparar/limpar identidades duplicadas de membros | views/HomeView.tsx removeDuplicateMembers | ui_groups.c: botão Limpar duplicados no painel Membros (só aparece com duplicados) [teste grupos-membros] | PRESENTE-FUNCIONAL | grupos-membros |
| GRP-16 | Convite para o grupo (código) | components/GroupInvitePanel.tsx | ui_groups.c: Convidar (gerar código) no cabeçalho e no menu do grupo; o código vem do core (group.sign/group.code); sem teste ponta a ponta | PRESENTE-FUNCIONAL | - |
| GRP-17 | Sala de voz mostra quem está nela, 'Ao vivo' e Assistir | views/HomeView.tsx | ui_groups.c roster_get: lista, sob cada sala de voz do grupo, quem esta nela + AO VIVO e Assistir (abre a chamada no modo Transmissao) [teste grupos-voz]. LIMITE: o core ainda nao emite group.roster; so a sala em que EU estou tem dado real, as outras vem do demo (BRIKS_DEMO_GROSTER) | PRESENTE-FUNCIONAL | grupos-voz |
| GRP-18 | Entrar na sala de voz pela barra lateral | views/HomeView.tsx | ui_social.c (Sala geral, voz-geral): sala única, não por grupo | PRESENTE-FUNCIONAL | social-b |
| GRP-19 | Buscar mensagens do canal | views/GroupView.tsx (Buscar) | attach.c attach_search_button: lupa do cabecalho vale para grupo e DM; abre o campo (bug do dock corrigido) [teste grupos-busca] | PRESENTE-FUNCIONAL | grupos-busca |
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
| MSG-09 | Anexar arquivo (botão, arrastar e soltar) | components/MessageComposer.tsx (Anexar arquivo, Solte para enviar) | botão do clipe (seletor múltiplo) + arquivo solto + imagem colada; sem sala de voz o anexo fica preparado | PRESENTE-FUNCIONAL | msg-anexar, campos-anexos |
| MSG-10 | Cartão de anexo (baixar, pausar, retomar, cancelar, abrir) | components/AttachmentCard.tsx | msg-galeria.txt: cartão (ícone, nome, tamanho, barra de progresso, Pausar/Retomar/Cancelar/Abrir com aviso, mostrar na pasta) no fluxo da conversa; transferência real testada só no núcleo (teste Rust) | PRESENTE-FUNCIONAL | msg-galeria |
| MSG-11 | Galeria de anexos do canal | components/ConversationTimeline.tsx (Galeria do canal) | msg-galeria.txt: botão "Galeria de imagens" com miniaturas das imagens recebidas na sessão (o Electron guarda por canal; aqui só desta sessão) | PRESENTE-FUNCIONAL | msg-galeria |
| MSG-12 | Pré-visualização de link | components/SafeMessageContent.tsx | msg-rico.txt: cartão de prévia (site, título, descrição, miniatura) sob o balão; busca segura feita pelo núcleo (link.preview); sem rede mostra só o link | PRESENTE-FUNCIONAL | msg-rico |
| MSG-13 | Links e formatacao Markdown seguros no texto | components/SafeMessageContent.tsx | msg-rico.txt: markdown (negrito, código, link, menção) na bolha; link só http(s) abre no navegador padrão; "Selecionar texto…" no menu para copiar trecho | PRESENTE-FUNCIONAL | msg-rico |
| MSG-14 | Estados da mensagem: enviando, entregue, falhou e reenvio (outbox) | chat/OutboxService.ts | msg-estados.txt: relógio (enviando), check (gravada), check duplo (entregue: chat.ack do núcleo com o id da mensagem, DM com sala aberta), alerta (falhou; clique reenvia). Entrega ponta a ponta só testada no núcleo (teste Rust do id) | PRESENTE-FUNCIONAL | msg-estados |
| MSG-15 | Buscar mensagens na conversa | views/HomeView.tsx (Buscar) | lupa no cabeçalho + Ctrl+F (campos-busca.txt) filtra as mensagens | PRESENTE-FUNCIONAL | grupos-busca, campos-busca |
| MSG-16 | Mensagens ficam gravadas neste computador | services/offline/chat-storage.ts | msgui.c / backend local | PRESENTE-FUNCIONAL | - |
| MSG-17 | Histórico sincronizado com o par (pedir/responder histórico) | chat/HistoryService.ts | briks-core chat_events.rs; sem teste de interop | VERIFICAR | - |
| MSG-18 | Aba Chat da chamada mostra as mensagens da sala | components/CallWorkspace.tsx (Chat da chamada) | ui_call.c (call_view 1) | PRESENTE-FUNCIONAL | call |
| MSG-19 | Enviar mensagem pela aba Chat da chamada | components/CallWorkspace.tsx | ui_call.c (stage_chat: balões de briks_bubble + campo de escrever) / rtc.c room.chat.send / core roomchat.rs; vale só durante a chamada | PRESENTE-FUNCIONAL | call-chat |

### Chamada

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| CHM-01 | Entrar na sala de voz | views/HomeView.tsx | ui_social.c / rtc.c (rtc_join_room) | PRESENTE-FUNCIONAL | social-b |
| CHM-02 | Mutar/desmutar o microfone | components/CallWorkspace.tsx | ui_call.c (Microfone) | PRESENTE-FUNCIONAL | call |
| CHM-03 | Ensurdecer | components/CallWorkspace.tsx | ui_call.c (Ensurdecer) | PRESENTE-FUNCIONAL | call |
| CHM-04 | Câmera (ligar e enviar vídeo) | components/CallWorkspace.tsx (Câmera) | ui_call.c: pílula desabilitada 'Câmera · em breve' (decisão: sem captura nem envio nesta versão; ver docs/plans/chamada-pacote-c.md) | PRESENTE-SEM-EFEITO | call |
| CHM-05 | Sair da chamada | components/CallWorkspace.tsx (Sair) | ui_call.c (Sair) | PRESENTE-FUNCIONAL | call-sair |
| CHM-06 | Voltar ao menu sem sair da chamada | components/CallWorkspace.tsx (Voltar ao menu) | ui_call.c (ícone minimizar) | PRESENTE-FUNCIONAL | call |
| CHM-07 | Mini-painel 'Voz conectada' na lateral (mic, fone, desligar, abrir) | views/HomeView.tsx | ui_voice.c ui_voice_mini: cartão Voz conectada no rodapé do menu (mic, fone, sair, abrir) | PRESENTE-FUNCIONAL | historico |
| CHM-08 | Modo Call / Transmissão | components/CallWorkspace.tsx | ui_call.c (Call / Transmissão) | PRESENTE-FUNCIONAL | call |
| CHM-09 | Chamada / Chat | components/CallWorkspace.tsx | ui_call.c (Chamada / Chat) | PRESENTE-FUNCIONAL | call |
| CHM-10 | Disposição Grade / Foco das telas | components/CallWorkspace.tsx | ui_call.c: segmento Grade/Foco no cabeçalho; foco segue quem fala ou fica fixado por clique (só participantes; a transmissão tem palco próprio) | PRESENTE-FUNCIONAL | call-grade |
| CHM-11 | Tela cheia da transmissão e da chamada | components/CallWorkspace.tsx (Tela cheia); ipc window:fullscreen | ui_call.c call_set_fullscreen: botão, F11 e Esc; a barra some após 3 s sem mouse (tela cheia da chamada; o palco da transmissão é do pacote de transmissão) | PRESENTE-FUNCIONAL | call-grade |
| CHM-12 | Zoom na transmissão (roda, pinca, arrastar, selo de %) | components/vídeo-zoom.tsx | (não existe) | AUSENTE | sonda |
| CHM-13 | Silenciar uma pessoa só para mim | components/CallWorkspace.tsx | ui_call.c (Silenciar para mim) | PRESENTE-FUNCIONAL | - |
| CHM-14 | Volume por pessoa (slider) lembrado nas próximas chamadas | services/áudio/person-áudio.ts | só liga/desliga mudo, sem volume e sem memória | AUSENTE | sonda |
| CHM-15 | Pedir a alguem que transmita (e aceitar/recusar o pedido) | call/stream-request.ts | ui_call.c: menu do bloco > Pedir transmissão; aviso com Transmitir / Agora não; core stream.request/stream.reply (roomchat.rs) | PRESENTE-FUNCIONAL | call-pedir |
| CHM-16 | Desconexao após 1 h sem falar, com aviso e Continuar | CallWorkspace.tsx; release 1.1.0 | (não existe) | AUSENTE | sonda |
| CHM-17 | Diagnóstico da conexão e indicador Rede Boa dentro da chamada | components/CallWorkspace.tsx (Rede, Diagnóstico) | ui_call.c net_chip: chip no cabeçalho + cartão com sinalização, pessoas, transporte e teste STUN (callhist.c call_net_quality) | PRESENTE-FUNCIONAL | call-rede |
| CHM-18 | Ajustes de áudio durante a chamada (mic, ruído, eco, porta) | components/InCallAudioSettings.tsx | ui_call.c audio_popover: volume de entrada/saída e porta de ruído (valem na hora); supressão de ruído e eco seguem sem efeito | PRESENTE-FUNCIONAL | call-rede |
| CHM-19 | Anel/nivel de quem está falando | components/CallWorkspace.tsx | ui_call.c (me_level, rtc_peers.level) | PRESENTE-FUNCIONAL | - |
| CHM-20 | Sons de entrar e sair da chamada | services/áudio/call-sounds.ts | notify.c: sons de entrar/sair (eu e outros) por transicao de in_room/peers; opcao em Notificacoes; reproducao real nao testada | PRESENTE-FUNCIONAL | - |
| CHM-21 | Tela Histórico de chamadas abre e volta | components/CallHistoryPanel.tsx | ui_history.c | PRESENTE-FUNCIONAL | historico |
| CHM-22 | Registro das chamadas (quando, duracao, sala, quem) | components/CallHistoryPanel.tsx; services/call/call-history.ts | ui_history.c history_tick + callhist.c: grava quando, duração, sala, quem estava e se transmiti em call_history.json | PRESENTE-FUNCIONAL | historico |
| CHM-23 | Guardar histórico on/off e apagar histórico | components/CallHistoryPanel.tsx | ui_history.c: interruptor Guardar histórico e Apagar histórico (com confirmação) | PRESENTE-FUNCIONAL | historico |
| CHM-24 | Vídeo remoto, câmera dos outros e 2a tela | components/CallWorkspace.tsx | (não existe; depende da câmera e do pacote de transmissão: ver docs/plans/chamada-pacote-c.md) | AUSENTE | sonda |
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
| CFG-05 | Escolher microfone | components/VoiceVideoSettingsPanel.tsx | settings.c (aba Voz): lista dispositivos e envia o nome ao core (audio.cfg); o core abre o microfone escolhido e avisa se sumiu | PRESENTE-FUNCIONAL | - |
| CFG-06 | Nivel do microfone ao vivo | components/VoiceVideoSettingsPanel.tsx | settings.c (barra ligada a me_level na sala) | PRESENTE-FUNCIONAL | voz-teste-mic |
| CFG-07 | Testar o microfone fora da chamada | components/useMicrophoneTest.ts | settings.c (Voz): botão Testar + medidor; core mic.test (testado com tom falso no roteiro voz-teste-mic) | PRESENTE-FUNCIONAL | voz-teste-mic |
| CFG-08 | Volume de entrada (ganho) | components/VoiceVideoSettingsPanel.tsx | settings.c slider; ganho aplicado em audio.rs (push_capture) | PRESENTE-FUNCIONAL | - |
| CFG-09 | Escolher saida de áudio | components/VoiceVideoSettingsPanel.tsx | settings.c dropdown; core abre a saída escolhida (audio.rs, match_device) | PRESENTE-FUNCIONAL | - |
| CFG-10 | Volume de saida | components/VoiceVideoSettingsPanel.tsx | settings.c slider; ganho de saída aplicado em audio.rs | PRESENTE-FUNCIONAL | - |
| CFG-11 | Supressão de ruído (Padrão/RNNoise/Desligada) | components/VoiceVideoSettingsPanel.tsx | settings.c: Padrão (passa-altas + expansor suave, audio.rs NoiseFilter) e Desligada reais; RNNoise NÃO existe | PRESENTE-FUNCIONAL | - |
| CFG-12 | Porta de ruído com limite | components/VoiceVideoSettingsPanel.tsx | settings.c; porta de ruído aplicada em audio.rs | PRESENTE-FUNCIONAL | - |
| CFG-13 | Cancelamento de eco | components/VoiceVideoSettingsPanel.tsx | settings.c: interruptor desabilitado com a nota em breve (sem AEC no core) | AUSENTE | sonda |
| CFG-14 | Salvamento automatico das configurações | components/VoiceVideoSettingsPanel.tsx | settings.c (settings.cfg) | PRESENTE-FUNCIONAL | - |
| CFG-15 | Temas prontos (presets) e troca na hora | components/ThemePanel.tsx | themes.c | PRESENTE-FUNCIONAL | config-abas |
| CFG-16 | Criar, salvar, editar, duplicar e excluir temas personalizados | components/ThemePanel.tsx | themes.c (Meus gradientes) | PRESENTE-FUNCIONAL | - |
| CFG-17 | Restaurar o tema padrão | components/ThemePanel.tsx (Restaurar tema Briks) | themes.c | PRESENTE-FUNCIONAL | - |
| CFG-18 | Uso de disco local | components/LocalStoragePanel.tsx | settings.c (tamanho da pasta) | PRESENTE-FUNCIONAL | config-abas |
| CFG-19 | Divisão do espaco por tipo (mensagens, grupos, anexos) | components/LocalStoragePanel.tsx | settings_data.c: barra por tipo (anexos, histórico) medida nas pastas locais | PRESENTE-FUNCIONAL | dados-limpar-exportar |
| CFG-20 | Limpar anexos | components/LocalStoragePanel.tsx | settings_data.c: limpar anexos com confirmação, apaga a pasta e atualiza o tamanho (roteiro dados-limpar-exportar) | PRESENTE-FUNCIONAL | dados-limpar-exportar |
| CFG-21 | Apagar histórico de mensagens (com confirmacao) | components/LocalStoragePanel.tsx | settings_data.c: confirma e apaga os arquivos locais de histórico; apagar no banco do backend NÃO foi testado (sem backend no teste) | PRESENTE-FUNCIONAL | dados-limpar-exportar |
| CFG-22 | Exportar meus dados | components/LocalStoragePanel.tsx | settings_data.c: Salvar como (GetSaveFileName) e JSON com contatos, grupos e ajustes; sem mensagens, anexos nem segredos | PRESENTE-FUNCIONAL | dados-limpar-exportar |
| CFG-23 | Sobreposição em jogos: interruptor geral | components/OverlaySettingsPanel.tsx | overlay_host.c: abre/fecha briks_overlay.exe --stdin na chamada; protocolo testado (overlay-stdin.ps1, --protocol-test) | PRESENTE-FUNCIONAL | overlay-quadro |
| CFG-24 | Sobreposição: o que aparece (4 itens) e opacidade | components/OverlaySettingsPanel.tsx | overlay_host.c: quem está na sala, sala, transmissão, mudo, opacidade e canto vão por stdin (roteiro overlay-quadro) | PRESENTE-FUNCIONAL | overlay-quadro |
| CFG-25 | Sobreposição: editar posições (arrastar, redimensionar) | components/OverlaySettingsPanel.tsx | settings.c: nota em breve; só os 4 cantos são reais (CFG-24) | AUSENTE | sonda |
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
| TRY-01 | Ícone na bandeja do sistema | desktop tray.ts | winstate.c/tray.c: Shell_NotifyIcon; prova sistema/bandeja-fecha (tray_visible) | PRESENTE-FUNCIONAL | - |
| TRY-02 | Menu da bandeja: Abrir app, Reiniciar app, Fechar o app | desktop tray.ts | tray.c: menu nativo Abrir/Silenciar/Reiniciar/Sair; comandos provados por roteiro (menu nativo bloqueia: nao aberto no teste) | PRESENTE-FUNCIONAL | - |
| TRY-03 | Clique no ícone da bandeja abre a janela | desktop tray.ts | tray.c/winstate.c: clique no icone abre; provado postando NIN_SELECT (nao clique real do mouse) | PRESENTE-FUNCIONAL | - |
| TRY-04 | Fechar a janela envia para a bandeja e a chamada continua | desktop window.ts (evento close) | winstate.c: WM_CLOSE vai para a bandeja (opcao em Sobre); prova bandeja-fecha; com atualizacao pronta fecha de verdade | PRESENTE-FUNCIONAL | - |
| TRY-05 | Instância única (segunda abertura foca a primeira) | desktop main.ts requestSingleInstanceLock | winstate.c: mutex nomeado + mensagem registrada; prova instancia | PRESENTE-FUNCIONAL | - |
| TRY-06 | Iniciar com o Windows | desktop (setLoginItemSettings) | winstate.c: chave Run (--minimizado); provado so em chave de teste, nunca registrado de verdade | PRESENTE-FUNCIONAL | - |
| TRY-07 | Links briks:// (convites e mensagens) | desktop protocol.ts | winstate.c: briks://mensagem/canal/id (WM_COPYDATA da 2a instancia + registro HKCU Classes); convites briks://friend e group NAO tratados | PRESENTE-FUNCIONAL | - |
| TRY-08 | Abrir links externos no navegador | desktop ipc shell:open-external | attach.c (clique no link, rich_safe_url) + sys_open_url (http/https); prova links | PRESENTE-FUNCIONAL | msg-rico |
| TRY-09 | Barra de título própria com minimizar/maximizar/fechar e versão | desktop window.ts | (janela do SO) | AUSENTE | sonda |
| TRY-10 | Janela com tamanho mínimo e redimensionavel | desktop window.ts | main.c (min 560x480) | PRESENTE-FUNCIONAL | - |
| TRY-11 | Aviso de recuperação quando o backend cai | components/DesktopRecoveryNotice.tsx | ui_auth.c ('Quase la', 'Tentar de novo') | PRESENTE-FUNCIONAL | - |

### Notificações

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| NOT-01 | Aviso: amigo ficou online | services/profile/use-friend-notifications.ts | notify.c: balao quando amigo fica online (respeita interruptor/foco/chamada) | PRESENTE-FUNCIONAL | - |
| NOT-02 | Aviso: amigo comecou a transmitir | services/profile/use-friend-notifications.ts | notify.c: balao ao iniciar transmissao, suprimido em chamada (NOT-05) e so ha transmissao dentro da chamada: quase nunca dispara | PRESENTE-FUNCIONAL | - |
| NOT-03 | Notificação de nova mensagem (DM e grupo) com o app em segundo plano | services/chat/background-manager.ts | rtc.c chat.message -> notify.c (mensagem + nao lidas); depende do core entregar chat.message | PRESENTE-FUNCIONAL | - |
| NOT-04 | Notificação 'pediu para você transmitir' | components/CallWorkspace.tsx | ui_call.c call_banner: aviso dentro do app (na chamada e no menu); notificação do sistema fica com o pacote de janela/notificações | PRESENTE-FUNCIONAL | call-pedir |
| NOT-05 | Nada e avisado durante a chamada nem logo ao abrir | release 1.2.0 | sysmodel.h sm_toast_allowed: nada em chamada nem nos 15 s iniciais; teste unitario + avisos/avisos-espera | PRESENTE-FUNCIONAL | - |

### Atalhos

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| ATL-01 | Atalho global: mutar/desmutar (Ctrl+Shift+M) | services/input/shortcuts.ts; desktop call-shortcuts.ts | hotkeys_win.c + hotkeys_rt.c: RegisterHotKey só na chamada; Gravar/Alterar/Remover na aba Atalhos | PRESENTE-FUNCIONAL | atalhos-gravar |
| ATL-02 | Atalho global: ensurdecer (Ctrl+Shift+D) | idem | idem ATL-01 | PRESENTE-FUNCIONAL | atalhos-gravar |
| ATL-03 | Atalho global: transmitir/parar | idem | idem ATL-01 | PRESENTE-FUNCIONAL | atalhos-gravar |
| ATL-04 | Apertar para falar (tecla, botão do meio/laterais, atraso ao soltar) | components/ShortcutsPanel.tsx | hotkeys_rt.c: PTT por sondagem (GetAsyncKeyState), tecla ou botão do mouse; atraso ao soltar não existe ainda | PRESENTE-FUNCIONAL | atalhos-gravar |
| ATL-05 | Remover atalho e aviso de atalho ocupado | components/ShortcutsPanel.tsx (Remover) | settings.c: Remover, aviso de conflito e de combinação ocupada (hkw_probe) | PRESENTE-FUNCIONAL | atalhos-gravar |
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
| BAS-02 | Menu de contexto (clique direito) nos campos de texto: Recortar, Copiar, Colar, Selecionar tudo | Chromium (nativo, sem código próprio) | campos-menu.txt: clique direito em qualquer cui_input (briks_input_ex em msgui.c) abre Desfazer/Refazer/Recortar/Copiar/Colar/Selecionar tudo; modal | PRESENTE-FUNCIONAL | campos-menu |
| BAS-03 | Selecionar com mouse (arrastar, duplo clique para palavra) e Shift+setas nos campos | Chromium (nativo, sem código próprio) | biblioteca CUI (cui_textedit.c): âncora e cursor, Shift+setas, Ctrl+setas por palavra | PRESENTE-FUNCIONAL | - |
| BAS-04 | Selecionar e copiar trecho de texto dentro das mensagens (arrastar o mouse sobre o balão) | Chromium (nativo, sem código próprio); MessageList.tsx | campos-selecao.txt: texto puro do balão selecionável por arrasto (Ctrl+C e Copiar seleção); mensagens com formatação: menu "Selecionar texto…" abre o texto cru selecionável (msg-rico.txt) | PRESENTE-FUNCIONAL | msg-rico, campos-selecao |
| BAS-05 | Copiar a mensagem inteira, o link e o ID pelo menu da mensagem | components/MessageList.tsx | msgui.c: Copiar texto, Copiar link da mensagem, Copiar ID | PRESENTE-FUNCIONAL | - |
| BAS-06 | Esc fecha diálogos, menus, popovers e seletor de emoji | Chromium (nativo, sem código próprio); modais do app | biblioteca (cui_dialog, popover) e msgui.c tratam Esc | PRESENTE-FUNCIONAL | bas-esc |
| BAS-07 | Enter envia a mensagem (Shift+Enter quebra a linha) | components/MessageComposer.tsx | ui_social.c: composer; coberto por MSG-01 | PRESENTE-FUNCIONAL | - |
| BAS-08 | Busca global / troca rápida (Ctrl+K) e busca dentro da conversa (Ctrl+F) | (não existe no Electron também) | campos-busca.txt: Ctrl+K paleta (amigos, grupos, mensagens) e Ctrl+F busca na conversa | PRESENTE-FUNCIONAL | campos-busca |
| BAS-09 | Tab e Shift+Tab percorrem os campos e botões na ordem visual | Chromium (nativo) | biblioteca (cui_core.c): ordem de foco por quadro, Tab e Shift+Tab; Esc solta o Tab no editor de código | PRESENTE-FUNCIONAL | - |
| BAS-10 | Foco visível (anel no elemento focado) e Enter/Espaço ativando botões | Chromium (nativo) | biblioteca: anel de foco e ativação por teclado nos widgets | PRESENTE-FUNCIONAL | - |
| BAS-11 | Alt+F4 e botão X da janela fecham o app | Electron (BrowserWindow) | Win32 nativo: WM_CLOSE encerra o app e a chamada; sem confirmação durante a chamada | PRESENTE-FUNCIONAL | - |
| BAS-12 | Ícone na bandeja do sistema (clique abre, menu Sair) | desktop tray.ts | ver TRY-01 | PRESENTE-FUNCIONAL | - |
| BAS-13 | Minimizar e fechar para a bandeja (a chamada continua) | desktop tray.ts | ver TRY-04 | PRESENTE-FUNCIONAL | - |
| BAS-14 | Iniciar com o Windows | desktop (loginItemSettings) | ver TRY-06 | PRESENTE-FUNCIONAL | - |
| BAS-15 | Instância única (abrir de novo traz a janela existente) | desktop (requestSingleInstanceLock) | ver TRY-05 | PRESENTE-FUNCIONAL | - |
| BAS-16 | Lembrar posição, tamanho, monitor e maximizado da janela | desktop-lifecycle.ts (só calcula o tamanho inicial pela área de trabalho; não grava) | winstate.c/sysmodel.h: sistema.cfg (x,y,w,h,max) + wf_fit no monitor atual; prova janela-* e teste unitario; maximizado nao verificado em janela oculta | PRESENTE-FUNCIONAL | - |
| BAS-17 | Redimensionar, maximizar e restaurar a janela com mínimo de 560x480 | Electron | Win32: WM_SIZE e WM_GETMINMAXINFO; os painéis se reajustam | PRESENTE-FUNCIONAL | - |
| BAS-18 | DPI alto e escala do Windows (PerMonitorV2, troca de monitor com DPI diferente) | Chromium (nativo, sem código próprio) | biblioteca: SetProcessDpiAwarenessContext, WM_DPICHANGED, cui_dpi_scale | PRESENTE-FUNCIONAL | - |
| BAS-19 | Vários monitores: abrir no monitor onde estava e janela fora da área visível voltar para dentro | desktop-lifecycle.ts | (não existe): sem restauração de posição, então depende do Windows | VERIFICAR | - |
| BAS-20 | Notificações do sistema (nova mensagem, amigo online, transmissão) | desktop notifications | notify.c: balao nativo pelo icone da bandeja; regras provadas (avisos); balao real nao exibido em teste | PRESENTE-FUNCIONAL | - |
| BAS-21 | Som de notificação e sons de entrar/sair da chamada | services/audio/call-sounds.ts | notify.c: WAV sintetizado + PlaySound; tocar de verdade nao testado (teste so conta) | PRESENTE-FUNCIONAL | - |
| BAS-22 | Badge de não lidas (lista de conversas, rail, ícone da barra de tarefas) | (não existe no Electron também) | notify.c/tray.c: contador na dica+icone da bandeja e selo da barra de tarefas; NAO ha pilula na lista de conversas/trilho | PRESENTE-FUNCIONAL | - |
| BAS-23 | Arrastar e soltar arquivos na janela ou no composer | components/MessageComposer.tsx (Solte para enviar) | campos-anexos.txt: arquivo solto envia (com sala) ou fica preparado no cartão; "Solte para enviar" aparece quando a plataforma informa o arraste (Windows ainda não informa) | PRESENTE-FUNCIONAL | campos-anexos |
| BAS-24 | Colar imagem da área de transferência no composer | Chromium (nativo, sem código próprio) | campos-anexos.txt: Ctrl+V / Colar com imagem na área de transferência cria cartão de anexo preparado (CF_DIB); envio exige sala de voz | PRESENTE-FUNCIONAL | campos-anexos |
| BAS-25 | Rolagem com a roda do mouse e o touchpad (vertical e horizontal) | Chromium (nativo, sem código próprio) | biblioteca: WM_MOUSEWHEEL e WM_MOUSEHWHEEL; cui_scroll nas listas e na conversa | PRESENTE-FUNCIONAL | - |
| BAS-26 | Barras de rolagem visíveis e arrastáveis | Chromium (nativo, sem código próprio) | biblioteca: cui_scroll desenha o polegar; editor de código tem barras próprias | PRESENTE-FUNCIONAL | - |
| BAS-27 | Tema claro e escuro (e seguir o do Windows) | (Electron: só tema escuro com cores) | Temas por gradiente (Configurações > Temas), sempre escuros; sem tema claro nem seguir o Windows | AUSENTE | sonda |
| BAS-28 | Idioma da interface (pt-BR, inglês) | (não existe no Electron também) | (não existe): textos fixos em português no código | AUSENTE | sonda |
| BAS-29 | Estados vazios com ícone e texto (sem amigos, sem mensagens, sem histórico) | Electron | ui_social.c e ui_history.c: ícone grande e texto de apoio | PRESENTE-FUNCIONAL | - |
| BAS-30 | Estado de carregamento (spinner ou esqueleto) enquanto o core sobe, entra na sala ou carrega o histórico | Electron | VERIFICAR: há texto de status ("Conectando", "Baixando a atualização"), mas não vi spinner ou esqueleto nas listas; depende do core real | VERIFICAR | - |
| BAS-31 | Mensagens de erro compreensíveis (login, rede, sem conexão com o core) | Electron | account.c e ui_auth.c: avisos em português claro ("Sem conexão com o Briks. Tente de novo…") | PRESENTE-FUNCIONAL | - |
| BAS-32 | Estado offline / sem internet visível ao usuário (banner persistente) | Electron | Pílula no topo (net_state.c): sem conexão (sonda TCP em segundo plano, 2 falhas seguidas) com Tentar agora, reconectando à chamada e conexão restabelecida | PRESENTE-FUNCIONAL | net-offline, net-restaurada |
| BAS-33 | Reconexão automática da chamada e do signaling com aviso | Electron | rtc.c: estado "Reconectando…" mostrado; reconexão feita pelo core | PRESENTE-FUNCIONAL | - |
| BAS-34 | Sair da conta e trocar de conta | Electron | Botão sair do rodapé e Desconectar em Configurações (AUT-13) | PRESENTE-FUNCIONAL | - |
| BAS-35 | Aviso de atualização disponível/pronta | Electron (electron-updater) | ui_update.c e Configurações > Atualizações (ATU-01) | PRESENTE-FUNCIONAL | - |
| BAS-36 | Sobre: versão do app e licença | Electron | Configurações > Sobre e Atualizações mostram versão e licença AGPL | PRESENTE-FUNCIONAL | - |
| BAS-37 | Licenças de software de terceiros dentro do app | (não existe no Electron também) | Configurações > Sobre > Licenças de software de terceiros: diálogo de vidro com rolagem e o texto empacotado (assets\licenses.txt) | PRESENTE-FUNCIONAL | config-licencas |
| BAS-38 | Contraste legível do texto sobre o fundo dos temas | Electron | themes.c limita a opacidade das manchas pela luminância e avisa quando clareia demais | PRESENTE-FUNCIONAL | - |
| BAS-39 | Tamanho de texto ajustável pelo usuário (zoom da interface) | Chromium (nativo, sem código próprio) (Ctrl+roda/Ctrl+mais) | Configurações > Temas > Aparência (4 tamanhos, vale na hora e fica salvo) e atalhos Ctrl+mais, Ctrl+menos e Ctrl+0 | PRESENTE-FUNCIONAL | config-texto |
| BAS-40 | Leitor de tela e navegação assistiva (UI Automation / MSAA) | Chromium (nativo, sem código próprio) (árvore de acessibilidade) | (não existe): a interface é desenhada pela biblioteca, sem UIA | AUSENTE | sonda |
| BAS-41 | Acentos, AltGr e teclas mortas (ç, ã, é) digitados no teclado ABNT2 | Chromium (nativo, sem código próprio) | biblioteca: WM_CHAR entrega o caractere final; funciona em todos os campos | PRESENTE-FUNCIONAL | - |
| BAS-42 | IME (japonês, chinês, coreano) e emoji do teclado do Windows (Win+.) | Chromium (nativo, sem código próprio) | VERIFICAR: a plataforma trata WM_CHAR mas não WM_IME_COMPOSITION; o texto final de IME e o painel de emoji provavelmente entram, mas sem janela de composição junto ao cursor | VERIFICAR | - |

### Layout novo (trilho, lista, conversa, hub)

| ID | Item | Electron | CUI | Classe | Teste automático |
|---|---|---|---|---|---|
| LAY-01 | Trilho à esquerda: Início e grupos como cartões arredondados, "Criar grupo" no fim | (padrão novo do usuário) | ui_social.c (P_SERVERS): cartões com avatar e texto; conforme | PRESENTE-FUNCIONAL | - |
| LAY-02 | Lista ao lado do trilho: Amigos, Adicionar amigo, Histórico, salas de voz, grupos, amigos e cartão do usuário embaixo | (padrão novo do usuário) | ui_social.c (P_NAV): conforme; item ativo em vidro com acento | PRESENTE-FUNCIONAL | - |
| LAY-03 | Conversa larga com cabeçalho (nome, tipo, Fixadas), mensagens e composer fixo embaixo | (padrão novo do usuário) | ui_social.c (P_CHAT) e msgui.c: conforme, com anexar, campo e enviar na mesma barra | PRESENTE-FUNCIONAL | - |
| LAY-04 | Hub à direita no layout padrão (membros do grupo, sala de voz, presença) | (padrão novo do usuário) | layout.c layout_app_default (cui-paineis, 895bc9c): grupos  |  menu  | - |
| LAY-05 | Tela inicial "Amigos" com abas Online/Todos/Pendentes e cartões de amigo | (padrão novo do usuário) | Amigos abre só um estado vazio "Escolha uma conversa", sem abas nem lista de pedidos (AMI-08) | AUSENTE | sonda |
| LAY-06 | Histórico de chamadas como painel do layout (hoje cobre a janela inteira, sem trilho nem lista) | (padrão novo do usuário) | ui_history.c ui_history_panel: abre no painel central com trilho e menu à vista; tela inteira só sem painel de conversa | PRESENTE-FUNCIONAL | historico |
| LAY-07 | Aba Chat da chamada no padrão de conversa (balões, avatares, horário, composer embaixo) | (padrão novo do usuário) | ui_call.c stage_chat: mesmo balão do painel de conversa (briks_bubble), horário, avatar e campo fixo embaixo | PRESENTE-FUNCIONAL | call-chat |
| LAY-08 | Tela de chamada em tela cheia (imersiva) em vez de dentro do layout trilho/lista/hub | (padrão novo do usuário) | ui_call.c: chamada ocupa a janela (sem trilho/lista) e tem tela cheia real (F11) | PRESENTE-FUNCIONAL | call-grade |
| LAY-09 | Diálogos (Configurações, perfil, criar grupo, excluir mensagem) em vidro, cantos arredondados, fundo escurecido | (padrão novo do usuário) | cui_dialog em settings.c, profile.c, msgui.c: conforme; 13 abas de Configurações em coluna à esquerda | PRESENTE-FUNCIONAL | - |
| LAY-10 | Tela de entrada (login, criar conta, recuperar) em cartão central de vidro | (padrão novo do usuário) | ui_auth.c: cartão central, título grande, botões Google/Discord e links: conforme | PRESENTE-FUNCIONAL | - |
| LAY-11 | Texto sempre legível: tamanho mínimo nas etiquetas (rótulos de seção em 10 a 11 px) | (padrão novo do usuário) | Piso de 12 px no cui_text (briks_ui.h): nenhuma etiqueta abaixo disso; o tamanho do texto de Configurações multiplica por cima | PRESENTE-FUNCIONAL | - |
| LAY-12 | Dica "Sair" (tooltip) presa no topo da tela e círculo no canto inferior esquerdo em todas as capturas | (padrão novo do usuário) | Nas capturas ocultas o tooltip do botão sair do rodapé aparece fixo em x=640 y=20; já listado em plano-pos-1.3.0 (frente D) | AUSENTE | - |
| LAY-13 | Janela mínima 560x480 sem corte ou sobreposição (captura gerada pelo roteiro) | (padrão novo do usuário) | O roteiro abre o app em 560x480 e captura; o teste só prova que abre e captura, a análise do desenho é visual na captura lay-min/capturas/l0.png | PRESENTE-FUNCIONAL | lay-min |

## Achados fora do escopo do inventário (bugs vistos pelo roteiro)

- **Clique atravessa os diálogos.** `briks_clicked()` (`briks_ui.h`) só confere o retângulo do widget e o clique, sem considerar que um diálogo modal está por cima. Com Configurações aberta, clicar na aba "Rede" (x 224-424, y 159) também aciona o item "Histórico de chamadas" da barra lateral que fica embaixo (x 277-489); a aba "Sobreposição" (y 264) entra na "Sala geral". O roteiro oculto reproduziu isso com o mouse injetado pelo mesmo caminho do mouse real; vale confirmar com o mouse físico. Não corrigi (fora do escopo: só inventário e testes). Os roteiros de Configurações clicam nas abas em x=240, fora da barra lateral, para não disparar isso.
- O tooltip "Sair" aparece preso no topo da tela em todas as capturas da tela social (já listado em `plano-pos-1.3.0.md`, frente D).

## Última execução do roteiro

| Resultado | Itens |
|---|---|
| AUSENTE | 18 |
| PASSOU | 111 |
| REVER | 13 |
| SEM-EFEITO | 6 |
| SEM-TESTE | 93 |
| VERIFICAR | 7 |

PASSOU = a ação teve o efeito visível esperado no app oculto; SEM-TESTE = item PRESENTE-FUNCIONAL sem roteiro automático (depende do core, da rede, de GPU ou de conferência manual); SEM-EFEITO/AUSENTE = confirmado pelo clique e/ou pela sonda; REVER = sonda ou clique indica que algo mudou; reclassifique.

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
