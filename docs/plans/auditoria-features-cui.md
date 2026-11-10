# Auditoria de features: Electron 1.2.x × app nativo (CUI)

Fontes: README, `apps/desktop` e `apps/web` (em `backup/electron-pre-cui`), `docs/PLANO-1.2.0.md`, `docs/plans/*`, e o código atual de `apps/desktop-cui`. Estado do CUI conferido por leitura do código (grep nas fontes C e Rust); o que não tem teste real de ponta a ponta está marcado.

Legenda: **Feito** = existe e foi exercitado; **Parcial** = existe só em parte ou só com dados/loopback; **Falta** = não existe. Estimativa em dias de trabalho focado. Prioridade: P0 (bloqueia trocar o Electron), P1 (alto valor), P2 (depois).

| # | Feature (Electron 1.2.x) | CUI hoje | Est. | Prio |
|---|---|---|---|---|
| 1 | Login/conta local (SQLite, `users`) | Parcial: tela de login e backend local; Supabase real não testado | 1–2 | P0 |
| 2 | Identidade ECDSA + peerId estável | Parcial: identidade nova (a chave do Chromium é não exportável); ver migração | 2–4 | P0 |
| 3 | Lista de amigos / convite por código | Parcial: lista e "adicionar amigo" existem; fluxo de convite P2P completo não foi validado | 2 | P0 |
| 4 | Chat DM por DataChannel assinado | Feito (loopback) / não testado contra o Electron | 2 (interop) | P0 |
| 5 | Grupos e canais (manifesto, cargos, revogação) | Parcial: grupos listados; sem criar/editar canais, cargos, manifesto | 5 | P1 |
| 6 | Chamada de voz P2P (malha) | Feito em loopback; WASAPI real e AEC não testados | 3 | P0 |
| 7 | Mutar/ensurdecer, mutar por pessoa, expulsar | Feito | – | – |
| 8 | Vídeo/câmera na chamada | Falta | 4 | P1 |
| 9 | Transmissão de tela/janela (qualidade, FPS, bitrate) | Falta (o backend Rust já tem captura de áudio por processo) | 8–12 | P0 |
| 10 | Áudio só da janela (WASAPI por processo) | Falta no app nativo (existe no `desktop-backend`) | 3 | P1 |
| 11 | SFU LiveKit como padrão | Falta (CUI só faz malha P2P) | 6–8 | P1 |
| 12 | TURN/ICE e rota (direta/VPN) | Falta | 2 | P1 |
| 13 | Atalhos globais e push-to-talk | Falta | 2 | P1 |
| 14 | Pedir transmissão / avisos de amigos | Falta (status e presença existem) | 2 | P2 |
| 15 | Teste de rede | Falta | 1 | P2 |
| 16 | Limite persistente / telemetria | Falta | 1 | P2 |
| 17 | Anexos e transferência de arquivos | Falta (backend tem `attachments.rs`) | 4 | P1 |
| 18 | Reações, respostas, edição, fixar, digitando, Markdown | Falta (só mensagem simples) | 4 | P1 |
| 19 | Histórico de chamadas | Feito (tela) | – | – |
| 20 | Status, programa em uso, balões | Feito | – | – |
| 21 | Overlay em jogo | Parcial (`briks_overlay.exe` existe; sem validação em jogo) | 2 | P2 |
| 22 | Notificações do Windows | Falta | 1 | P1 |
| 23 | Bandeja (tray) e fechar para a bandeja | Falta | 1 | P1 |
| 24 | Iniciar com o Windows | Falta | 0,5 | P1 |
| 25 | Instância única | Falta | 0,5 | P1 |
| 26 | Deep links (`briks://`) / convites por link | Falta | 1 | P2 |
| 27 | Auto-update (GitHub, `latest.yml`) | Falta (etapa D) | 3 | P0 |
| 28 | Instalador NSIS | Falta (etapa C) | 2 | P0 |
| 29 | Modos de desempenho, tema, vidro, layout arrastável | Feito (só no CUI) | – | – |
| 30 | Outbox/entrega com reenvio, busca local | Falta | 3 | P2 |

### Acrescentado pelo inventário da gravação do Electron 1.2.2 (A:\OBS\2026-11-10 16-33-24.mp4)
Dono das telas: sessão `cui-ui-paridade` (interface) e `cui-transmissao` (item 9/31–33). Aqui só o inventário e o estado do CUI atual.

| # | Feature vista no Electron | CUI hoje | Est. | Prio | Dono |
|---|---|---|---|---|---|
| 31 | Seletor de fonte: abas Telas/Janelas com miniaturas, "Transmitir Tela 1" | Falta | 3 | P0 | cui-transmissao |
| 32 | Áudio da transmissão: da fonte / escolher apps (por app, "Tocando agora") / sem áudio; "O Discord nunca entra" | Falta | 3 | P0 | cui-transmissao |
| 33 | Chip "Com áudio" e chip de qualidade "1080p · 30 FPS · 4 Mbps" na barra da call | Falta | 1 | P1 | cui-transmissao |
| 34 | Barra da call: alternar Call/Transmissão, Chamada/Chat, "Rede Boa", botões de layout, seletor de áudio, Câmera | Parcial (mutar/ensurdecer/sair; sem o resto) | 3 | P1 | cui-ui-paridade |
| 35 | Janela com barra de título roxa própria (min/max/fechar), versão "v1.2.2" | Falta (janela do SO) | 1 | P2 | cui-ui-paridade |
| 36 | Foto de perfil (avatar imagem) no rodapé, tiles e mensagens | Falta (só iniciais) | 2 | P1 | cui-ui-paridade |
| 37 | ＠nick e "Reparar identidades duplicadas" | Falta | 2 | P1 | cui-ui-paridade |
| 38 | Amigos: abas Online/Todos/Pendentes com contadores, "Buscar amigo", 3 ações por linha, painel Atividade, "Entrar em grupo" | Parcial (lista e adicionar) | 3 | P1 | cui-ui-paridade |
| 39 | Grupo: editar/excluir, canais de texto (#geral) e salas de voz com quem está nelas, busca | Parcial (só listagem) | 5 | P1 | cui-ui-paridade |
| 40 | Membros: marcas VOCÊ/DONO/＠nick, atualizar, limpar duplicados; "Gerenciar membro" (tornar administrador, remover), "Personalizar grupo" (imagem/nome, sincroniza por WebRTC) | Falta | 4 | P1 | cui-ui-paridade |
| 41 | Prévia de link nas mensagens (card com título/URL) | Falta | 2 | P2 | cui-ui-paridade |
| 42 | Mini-painel "Voz conectada" (mic, fone, expandir, desligar) | Parcial (controles na tela da call) | 1 | P2 | cui-ui-paridade |
| 43 | Configurações em abas: Voz e vídeo, Rede, Temas (presets + personalizado), Armazenamento, Sobreposição (interruptores, opacidade, editar posições), Atalhos, Notificações, Avisos, Atualizações, Sobre (telemetria, código-fonte, licença) | Parcial (uma tela: desempenho, presença, vidro) | 6 | P1 | cui-ui-paridade |
| 44 | Status personalizado: emoji, chips rápidos, "Limpar depois de 4 horas" | Parcial (texto; sem emoji/chips/expiração) | 1 | P2 | cui-ui-paridade |
| 45 | Programa em uso: lista "Ignorar programa" | Parcial (liga/desliga e público; sem lista de ignorados) | 1 | P2 | cui-ui-paridade |
| 46 | Conta: sair da conta / desconectar conta | Parcial (sair) | 0,5 | P2 | cui-ui-paridade |
| 47 | Atualizações: "Verificar agora", "Novidades da 1.2.2" | Parcial: núcleo do atualizador pronto, DESLIGADO por padrão; sem tela | 1 | P2 | cui-instalador |

Obs.: a gravação não tem áudio e amostra a cada ~2 s; receber/assistir a transmissão e comportamentos rápidos não aparecem.

## Leitura rápida
- **Bloqueia substituir o Electron de verdade:** 9 (transmissão de tela, a razão de ser do app), 6 (voz com áudio real), 2/3/4 (identidade, amigos, interop) e 27/28 (instalar e atualizar).
- **Sem o item 9 o CUI não substitui o Electron em uso real**: quem atualizar perde a transmissão de tela até o item existir. Isso precisa estar claro antes de publicar o instalador como atualização automática.
- Itens de custo baixo e alto valor (24, 25, 22, 23) entram junto do instalador (etapa C) por serem do lado do Windows.
