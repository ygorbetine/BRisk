# Pacote (c) Chamada — o que entrou, decisões e o que ficou de fora

Complementa `paridade-integrado.md`. Vale para o branch integrado (cui-integracao) mais este pacote.

## O que entrou
| Item | Como ficou | Onde |
|---|---|---|
| MSG-19 / LAY-07 | Aba Chat da chamada no padrão de conversa: mesmo balão do painel de conversa, avatar, horário, campo de escrever fixo embaixo, Enter ou botão enviam; contador de não lidas na aba. | `ui_call.c` (stage_chat), `ui_social.c` (briks_bubble) |
| CHM-10 | Grade (blocos iguais, última linha centralizada, até 10 blocos) e Foco (um bloco grande e os demais numa faixa). No Foco o destaque segue quem fala, ou fica fixado ao clicar num bloco; clicar de novo solta. | `ui_call.c`, `callhist.c` (call_grid) |
| CHM-11 / LAY-08 | Tela cheia da chamada: botão no cabeçalho, F11 e Esc. A janela vira sem borda no monitor atual; cabeçalho some, a barra flutua e some após 3 s sem mouse. | `ui_call.c` (call_set_fullscreen) |
| CHM-15 / NOT-04 | Botão direito no bloco > "Pedir transmissão". Quem recebe vê um aviso com Transmitir / Agora não (na chamada e no menu); quem pediu vê o retorno. | `ui_call.c`, `rtc.c`, core `roomchat.rs` + `mesh.rs` + `ipc.rs` |
| CHM-22 / CHM-23 / LAY-06 | Histórico real: quando, duração, sala, quem estava, se transmiti; "Guardar histórico" e "Apagar histórico" com confirmação; "Ligar de novo". Abre no painel central (trilho e menu à vista); o painel Histórico do dock usa a mesma lista. | `ui_history.c`, `callhist.c` |
| CHM-07 | Cartão "Voz conectada" no rodapé do menu lateral enquanto se está numa sala: nome da sala, microfone, fone, desligar; clicar no texto abre a chamada. | `ui_voice.c` (ui_voice_mini) |
| CHM-17 | Chip de rede no cabeçalho (Rede boa / instável / Sem conexão / Conectando) e cartão com sinalização, pessoas, transporte e o teste STUN. | `ui_call.c` (net_chip), `callhist.c` (call_net_quality) |
| CHM-18 | Pílula "Áudio" na barra: nível do microfone, volume de entrada e saída, porta de ruído. Vale na hora e grava como a aba Voz. | `ui_call.c` (audio_popover), `settings.c` (settings_audio_set) |

## Decisões
### Câmera (CHM-04) e vídeo remoto (CHM-24): ENTRARAM NA 1.4.0 (a decisão abaixo foi superada)
Implementado: captura por Media Foundation em `briks-core/src/camera.rs` (câmera falsa com `BRIKS_FAKE_CAMERA=1` para testes sem hardware), faixa `camera-video` própria na malha P2P e no LiveKit, mesmo pipeline de H264/adaptação da transmissão (640x360/30), vídeo no bloco da pessoa (`src/ui_cam.c`) com avatar quando desligada, escolha e teste em Configurações > Voz e vídeo. Câmera e tela coexistem (duas faixas). A câmera só abre enquanto ligada. Texto histórico:
A pílula continua "Câmera · em breve", desligada, com a dica dizendo que o vídeo da câmera ainda não é enviado. Motivos:
1. Enviar a câmera exige uma segunda faixa de vídeo por peer com renegociação e um destino para ela no palco. Isso mora em `mesh.rs`/`share.rs`/`video.rs`, que estão sendo alterados agora pelas frentes de transmissão e SFU; mexer ali em paralelo quebraria a integração deles.
2. Não há como testar com segurança no modo oculto: ligar a webcam real do usuário durante um teste automatizado é invasivo (a luz da câmera acende) e a máquina de teste pode nem ter câmera.
3. O codificador hoje disponível sem GPU é o caminho de software da transmissão; duas codificações simultâneas (tela + câmera) em CPU não foram medidas.
Caminho proposto para depois que a transmissão/SFU estabilizar: captura por Media Foundation (`IMFSourceReader`, NV12 640x360 a 15–24 fps) num módulo próprio do core, fonte de teste sintética para os roteiros ocultos, envio pela mesma estrutura de faixa da transmissão com um segundo `mid`, e o bloco do participante trocando o avatar pelo vídeo. CHM-24 (câmera dos outros e 2ª tela) depende disso.

### Chat da sala é efêmero
As mensagens da aba Chat vão só para quem está autenticado na sala naquele momento e somem ao sair. Não entram no histórico de DMs nem no banco local. O autor exibido é a identidade provada do DataChannel (o que vem no JSON é ignorado); há limite de 12 mensagens a cada 10 s por remetente.

### Pedido de transmissão
Um pedido por remetente a cada 10 s; a resposta só é aceita se houve pedido meu nos últimos 2 min; sem resposta em 1 min conta como "Agora não"; quem já está transmitindo responde "sim" sozinho.

### Histórico
Fica só neste computador (`call_history.json` na pasta de dados do app; `BRIKS_CUI_DATA` nos testes). Guarda as 100 chamadas mais recentes. A duração conta a partir da conexão; chamada que nunca conectou não entra. A cada 30 s a chamada em andamento é gravada como se terminasse ali, para não se perder se o app fechar no meio.

### Popovers do CUI
`cui_popover` ancora no último *componente* (botão, cartão, canvas), não em caixa simples. O chip de rede e a pílula Áudio usam um canvas como âncora; sem isso o popover abre junto do cursor.

### Cabeçalho dos painéis do dock não recebe botões do CUI
O cabeçalho (56 px) é a alça de arrasto do painel e, com o mouse em cima, um canvas cobre o painel. Por isso as opções do Histórico ficam numa faixa abaixo do cabeçalho.

### Janela estreita e texto grande
Os limites do cabeçalho e da barra usam a largura dividida pela escala do texto: abaixo de ~1120 px "de texto" as abas viram só ícone (com dica), abaixo de ~900 as pílulas também. A barra de controles quebra em duas ou três linhas em vez de passar da largura, e o palco desconta a altura real dela. O palco é medido pela janela, nunca por uma caixa do layout (os blocos têm tamanho fixo em px e realimentavam o tamanho antigo). Em bloco pequeno o avatar fica acima do rótulo. Com o palco baixo demais o Foco cai para a Grade. Conferido por captura em 560x480, 900x600 e 1280x800, com texto Normal e Máximo: nada cortado nem para fora. Em 560x480 com texto Máximo a barra ocupa três linhas e os blocos ficam pequenos (legíveis).
Atenção nos roteiros: `at 2 790` (macro `snap`/`tap` do paridade.ps1) põe o cursor de teste fora de uma janela menor que 800 px de altura e o desenho do cursor empurra o layout inteiro; em janela pequena use um ponto dentro dela.

## Não feito / limites conhecidos
- CHM-04 e CHM-24 (acima).
- Grade/Foco e tela cheia valem para os participantes e para a tela da chamada. O palco da transmissão (`ui_share_stage`) não foi tocado: zoom, tela cheia do vídeo e várias telas lado a lado são do pacote de transmissão.
- Em tela cheia o cabeçalho some: para trocar de aba (Chat, Transmissão) é preciso sair da tela cheia.
- NOT-04 é o aviso dentro do app; a notificação do sistema é do pacote (a).
- CHM-14: o volume por pessoa existe no painel Sala de voz, mas ainda não é lembrado entre chamadas.
- CHM-12, CHM-16, CHM-20: fora deste pacote.
- Chat da sala e pedido de transmissão foram testados entre dois nós reais do core em loopback e, na interface, com participantes de exemplo. Não houve teste ponta a ponta com duas janelas do app numa sala real.
- `social-b.txt` segue com coordenadas antigas para os itens de grupo; só as checagens de CHM-21 saíram dele (agora em `historico.txt`).
