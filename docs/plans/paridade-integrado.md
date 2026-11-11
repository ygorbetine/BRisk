# Paridade no branch integrado (cui-integracao) — lacunas reais e pacotes

Complementa `paridade-electron-cui.md` (gerado do `itens.csv`, classificado sobre o brisk-dev). Este arquivo é escrito à mão: registra o que o roteiro `scripts/paridade.ps1` mostrou no build integrado (painéis 3aa4bc3, paridade e039295, mensagens 0a9cf2d, contas a529283, auditoria 8e0bfb9; SEM transmissão nova e SEM SFU).

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
