# Paridade no branch integrado (cui-integracao) — lacunas reais e pacotes

Complementa `paridade-electron-cui.md` (gerado do `itens.csv`, classificado sobre o brisk-dev). Este arquivo é escrito à mão: registra o que o roteiro `scripts/paridade.ps1` mostrou no build integrado (painéis 3aa4bc3, paridade e039295, mensagens 0a9cf2d, contas a529283, auditoria 8e0bfb9; SEM transmissão nova e SEM SFU).

## Atualização de 2026-11-11 (rodada com audio-robusto, câmera, transmissão e painéis integrados)
Números reais do `scripts/paridade.ps1` (248 itens, janela oculta, `BRIKS_NET_LOOPBACK_ONLY=1`): **123 PASSOU, 0 FALHOU, 110 SEM-TESTE, 7 VERIFICAR, 5 AUSENTE, 3 SEM-EFEITO** (antes das integrações: 113 PASSOU; na rodada anterior: 54 FALHOU por roteiro velho).
- Os 54 FALHOU eram roteiros com coordenadas do layout antigo (barra de título de 36 px + filete, lateral com "Entrar com convite", botões das caixas de diálogo). Nenhum era bug do app. Os roteiros foram recalibrados: pontos simbólicos onde há (`tap plus:<painel>`, `tap rel:<painel>:<L|R>dx:<T|B>dy`, `at <ponto>`) e coordenadas absolutas das caixas modais (`mtap`/`mat`/`mclick`/`mto`, sem o deslocamento `-Dy 39` do conteúdo) lidas das capturas.
- Dois itens ainda falhavam por motivos próprios: CHM-04 (câmera): a câmera agora existe, o botão muda a captura, então a checagem passou de "igual" para "muda"; CFG-07 (microfone negado): o `briks-core.exe` do build era anterior ao merge do áudio robusto (`BRIKS_FAKE_MIC` não chegava ao teste); recompilado e passa.
- Reclassificados no `itens.csv`: CHM-12 (zoom na transmissão, `ui_share.c`/`callhist.c`) e CHM-24 (câmera dos outros e 2ª tela, `ui_cam.c`/`ui_share.c`) viraram PRESENTE-FUNCIONAL (SEM-TESTE de pixels); LAY-12 e CHM-04 já estavam PRESENTE-FUNCIONAL. Os 7 AUSENTE de antes viram 5 (RDE-06, BAS-27, BAS-28, BAS-40, LAY-05).
- Mic negado (CFG-07 / `voz-mic-negado`, `call-mic-aviso`) passa com o core novo.
- Provas: firewall 3/3 (netsh falso; botões da caixa mudaram de posição e foram atualizados), modal 4 roteiros sem clique vazando para as camadas de baixo, smoke "tudo certo".
- Não coberto: os testes `camera_e2e` do core falham em paralelo (estado global da câmera falsa compartilhado; `camera_adapts_to_loss...` também falha sozinho: adaptação à perda de banda em loopback), por isso o build foi feito com `-SkipTests`; GPU real, Release, instalador e ícone da barra de tarefas não foram testados.

## Atualização anterior de 2026-11-11 (este arquivo abaixo é histórico)
- Os 5 FALHOU por coordenadas velhas não existem mais: os roteiros `social-a/b`, `config-abas` e `call` passam no integrado.
- `atalhos-gravar` às vezes sai com código 1 ao abrir e passa na repetição (a saída 1 intermitente descrita no fim deste arquivo).
- Os 13 REVER restantes foram conferidos e reclassificados no `itens.csv`:
  - Viraram PRESENTE-FUNCIONAL: PRF-06, AMI-06, AMI-07, AMI-08, AMI-09, AMI-11, MSG-08, CHM-14, CFG-26, RDE-03.
  - TRX-11 (SFU) virou VERIFICAR: o código existe e o modo de Configurações > Rede chega ao core, mas falta teste com LiveKit real.
  - CHM-16 (desconexão por inatividade) e CFG-29 (novidades da versão) continuam AUSENTE: as sondas davam falso positivo e foram corrigidas.
- Ressalvas: CHM-14 passou a lembrar o volume por identidade (`peer-volumes.txt`), só compilado, sem teste; CFG-26 só recebe aviso da falha de "iniciar com o Windows"; em AMI-11 o texto de atividade só chega de quem está na sua chamada. Só AMI-08, AMI-09 e AMI-11 ganharam roteiro (`amigos-abas`); PRF-06 tem teste em `tests/profile_test.c`; os demais foram conferidos por leitura do código.
- O cartão "MODO DA CHAMADA" em Configurações > Rede perdeu a etiqueta "sem efeito" (o modo já chegava ao core); "PREFERÊNCIA DE CONEXÃO" continua sem efeito.

## Números
| | brisk-dev (auditoria) | integrado (roteiro) |
|---|---|---|
| Funcionais | 116 | 47 PASSOU + 64 SEM-TESTE + 5 FALHOU |
| Sem efeito | 33 | 29 (4 viraram REVER) |
| Ausentes | 92 | 74 (18 viraram REVER) |
| Verificar | 7 | 7 |

## 5 FALHOU: roteiro desatualizado, não regressão (conferido nas capturas)
As coordenadas dos toques foram gravadas no layout do brisk-dev; no integrado a lateral ganhou "Entrar com convite" e as Configurações mudaram de altura.
- AMI-02 (abrir conversa ao clicar no amigo), CHM-21 (histórico abre e volta): o toque cai um item acima/abaixo (em CHM-21 abriu "Entrar com convite").
- CFG-02 e ATU-01 (abas de Configurações): os dois toques caem na mesma aba (Avisos).
- CHM-06 (voltar ao menu sem sair da chamada): o toque não acerta o botão.
Correção: regravar `social-a.txt`, `social-b.txt`, `config-abas.txt` e `call.txt` (ou usar pontos simbólicos em vez de coordenadas). NÃO FEITO.

## 22 REVER: há código novo, falta confirmar o efeito e reclassificar no itens.csv
PRF-06 (limpar status), AMI-06 (recusar pedido), AMI-07 (desfazer amizade), AMI-08 (abas com contadores), AMI-09 (buscar amigo), AMI-11 (Atividade), GRP-06 (renomear canal), GRP-08 (personalizar grupo), GRP-09 (apagar/sair), GRP-13 (administrador), MSG-08 (digitando), MSG-12 (prévia de link), MSG-13 (links/markdown), CHM-14 (volume por pessoa), CHM-16 (desconexão por inatividade), CFG-12 (porta de ruído), CFG-29 (novidades), RDE-03 (redes privadas), TRY-08 (links externos), ATL-04 (apertar para falar), BAS-23 (arrastar e soltar), BAS-39 (tamanho do texto).

## Ainda marcados AUSENTE/SEM-EFEITO, mas com código no integrado (as sondas não pegaram; conferir e reclassificar)
Achados por busca no código, sem teste de efeito: AMI-17/GRP-16 (entrar por convite/código), GRP-04/05 (criar canal e sala), GRP-12/14 (adicionar/remover membro), AMI-10 (online/offline), AMI-15 (sino por amigo), MSG-09/10 (anexos: `filetransfer.rs`, `attach.c`), MSG-14 (ack de entrega), MSG-15 (busca na conversa), MSG-17 (histórico sincronizado: `history.rs`), MSG-19 (chat da chamada), RDE-04 (teste de rede), RDE-06 (TURN na interface), CFG-05 (microfone).

## Lacunas sem nenhum código no integrado (busca vazia)
Bandeja, instância única, notificações do sistema, lembrar janela, sons, atalhos globais (`RegisterHotKey`), teste de microfone fora da chamada, registro real das chamadas, foto dos outros.

## Pacotes independentes (por arquivos, para delegar em paralelo)
- **(a) Janela e sistema** — arquivos novos `src/tray.c`, `src/notify.c`, `src/winstate.c` + ganchos em `main.c`: TRY-01..07, BAS-12..16, BAS-20..22, NOT-01..05, CHM-20, TRY-09.
- **(b) Campos de texto e mensagens** — `msgui.c`, `attach.c`, `richtext.c` (+ patch do CUI para menu de contexto): BAS-02, BAS-04, BAS-24, BAS-08; conferir BAS-23, MSG-09..15.
- **(c) Chamada** — `ui_call.c`, `ui_voice.c`, `ui_history.c`, `rtc.c`: MSG-19/LAY-07 (chat da chamada), CHM-10 (grade/foco), CHM-11/LAY-08 (tela cheia), CHM-15 (pedir transmissão), CHM-22/23 (histórico real), CHM-07, CHM-17, CHM-18, CHM-24, CHM-04 (câmera).
- **(d) Voz e atalhos** — `settings.c` (abas Voz e Atalhos), `hotkeys.c`, `hotkeys_rt.c`, `briks-core/src/audio.rs`: CFG-05..13, CFG-07, ATL-01..06, RDE-01/02.
- **(e) Grupos** — `ui_groups.c`, `briks-core/src/group_sync.rs`: GRP-04..17, GRP-19, GRP-22, AMI-04, AMI-17, LAY-04; em boa parte é conferir e reclassificar.
- **(f) Acessibilidade, texto e offline** — `theme*.c`, `perf.c`, `account.c` (banner offline): BAS-27, BAS-28, BAS-32, BAS-37, BAS-40, BAS-42, LAY-11, LAY-12.
- **(g) Dados e sobreposição** — `settings.c` (Armazenamento, Sobreposição), `overlay.c`: CFG-19..28. Conflita com (d) em `settings.c`: fazer em sequência ou separar as abas em arquivos antes.
- **(h) Conta** — `account.c`, `ui_account.c`, `ui_auth.c`: AUT-03, AUT-11, PRF-03, PRF-12.
Pontos de conflito entre pacotes: `main.c` (a, c), `settings.c` (d, g), `briks_ui.h` (todos: só acrescentar campos no fim de `app_state_t`).

## Saída 1 intermitente ao abrir (call-sair, bas-esc, lay-min)
NÃO reproduzida: 36 execuções seguidas dos três roteiros no build integrado, todas com saída 0. No código do app o único caminho para saída 1 é `cui_app_create` falhar (criação da janela/contexto gráfico); não há corrida com o core, que sobe depois. Outra origem possível, não confirmada: outra sessão matando `briks_cui.exe` por nome (`taskkill /F` devolve 1). Mudança feita: `main.c` tenta criar a janela uma segunda vez e escreve o motivo em stderr, para a próxima ocorrência ser identificável.

## Clique atravessando modais
Confirmado no integrado: `scripts/prova/modal/rodar.ps1` passa (3 roteiros) e falha com `-SemRegra` (3 de 3), ou seja, a regra central do cui-paineis (`briks_blocked` em `briks_clicked`/`briks_hover`, e `cui_dialog`/`cui_menu`/`cui_popover` redefinidos em `briks_ui.h`) cobre os arquivos das outras frentes. Dois cliques diretos de botão direito escapavam da regra (menu da mensagem em `msgui.c`, menu do participante em `ui_call.c`): corrigidos aqui. O redimensionar de divisória em `dock.c` usa clique direto; não testado com modal aberto.

## Auditoria de lacunas (1.4.0)
Classe nova `PARCIAL` no `itens.csv` (o runner a trata como funcional, com o aviso "falta parte do Electron"). Reclassificados: RDE-06 (TURN já existe) → PRESENTE-FUNCIONAL; CFG-11, CFG-22, CFG-24, CFG-25, MSG-10 → PARCIAL. CFG-26 (aba Avisos ligada a erros de chamada, transmissão, arquivo, conta e rede), NOT-04 (aviso do sistema "pediu para você transmitir", `NT_REQUEST`) e o novo ATU-05 (bolinha de atualização pronta) foram implementados na 1.4.0 e ficam PRESENTE-FUNCIONAL. Itens novos: OVL-01..04, MSG-20, MSG-21, CHM-26, CFG-30, PRF-14, ATU-05, TRX-13 (as lacunas restantes ficam para a 1.4.1).
