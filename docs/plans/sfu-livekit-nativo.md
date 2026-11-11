# SFU (LiveKit) no app nativo: decisão e estado

## Decisão: opção 2 — protocolo de sinalização do LiveKit sobre o webrtc-rs que o briks-core já usa

| | 1. crate `livekit` (rust-sdks, libwebrtc) | 2. sinalização LiveKit + webrtc-rs (escolhida) | 3. híbrida |
|---|---|---|---|
| Codificação | do libwebrtc; recebe quadros brutos. O README do SDK lista aceleração por hardware só para NVIDIA em **Linux**; em Windows não há NVENC/MFT | **os mesmos quadros H264 já codificados** pelo nosso encoder (MFT/NVENC, fallback software) | só o áudio/dados pelo SDK: dois stacks WebRTC no mesmo processo |
| Controle | perde `adapt.rs`, pacer, NACK/PLI próprios, conversão GPU, áudio por aplicativo | mantém tudo | mantém o vídeo, duplica o resto |
| Build/tamanho | segundo WebRTC (libwebrtc C++ baixado no build, C++/clang/MSVC); o README não traz instruções de Windows; binário muito maior | só `livekit-protocol` (prost, Apache-2.0, **sem protoc**); compila com o que já existe | os dois custos |
| Risco | menor no protocolo, **maior no produto** (regressão de qualidade no ponto que o usuário mais cuida) | nós mantemos o protocolo: versão fixada (`=0.8.2`, protocolo 15) e testada contra o servidor oficial | complexidade sem ganho |

Escolhida a 2 por seguir a instrução do usuário (SFU já, sem perder NVENC nem o pipeline atual) e porque o protocolo é pequeno:
WebSocket `/rtc` + protobuf; dois PeerConnections (subscriber: o servidor oferece, nós respondemos; publisher: `AddTrack` →
`TrackPublished` → offer). Fonte: docs oficiais do LiveKit (client protocol).

## Como funciona (`briks-core/src/sfu.rs`, `sfu_cfg.rs`)
- `sfu::connect` abre o WS (ws/wss pelo mesmo caminho do Realtime), espera o `JoinResponse` (falha rápida → o chamador cai no P2P) e devolve
  o mesmo par `MeshCmd`/`MeshEvent` da malha: o IPC e a UI não mudam por causa do modo.
- Áudio: a faixa Opus do `Audio` (mic) é publicada; as recebidas entram em `Audio::run_remote` (jitter/PLC/mixagem/mudo por pessoa iguais ao P2P).
  Mudo do mic = `MuteTrackRequest`; o outro lado vê `PeerState`.
- Tela: `share::start_sender` + `VideoOut` (encoder, adapt.rs, pacer, NACK) publicam H264 RTP; só o publisher é restrito a Opus+H264. Quem
  assiste só baixa o vídeo quando manda `share.watch` (`UpdateSubscription`), e o decodificador/`run_remote_video` são os de sempre.
  Parar = silenciar a faixa; retomar = reabrir (o webrtc-rs 0.12 não deixa republicar no transceiver removido).
- Realimentação: Receiver Reports/NACK/PLI do SFU alimentam `NetShared["sfu"]` → `adapt.rs` (mesma leitura da malha).
- Token: `fetch_token` faz POST em `BRIKS_LIVEKIT_TOKEN_URL` com `Authorization: Bearer <sessão da conta>`; o segredo fica na função
  `livekit-token` do Supabase (já existente, nada mudou nela). Sala = `<ns>:<32 hex de secure_room_id>`; identidade = id do usuário.
- Config (`sfu_cfg.rs`): `BRIKS_LIVEKIT_URL`/`BRIKS_LIVEKIT_TOKEN_URL` (ou `VITE_LIVEKIT_*`, ou `livekit.json`); só `wss://`+`https://`
  (`ws://`/`http://` só em loopback). Modo: Automático/SFU/P2P (`net-mode.json`; Configurações > Rede já tinha o seletor, agora ligado ao core
  pelo campo `net` do `room.join`). Eventos novos: `net.mode` (modo atual, motivo, erro, servidor) e `net.config`; o diagnóstico mostra "Modo da chamada".
- Só salas de voz (`room`/`group`) usam SFU; chat/amigos seguem pela malha.

## Achados do teste real (servidor oficial) que custaram horas
1. O webrtc-rs 0.12 anuncia `AEAD_AES_256_GCM` e **entra em pânico** no SRTP quando o LiveKit/pion escolhe esse perfil: o `sfu.rs` anuncia só os de 128 bits.
2. Com os codecs padrão a offer lista VP8 antes e o LiveKit registrava a faixa como VP8 (ninguém recebia): o publisher registra só Opus + H264 e manda `simulcast_codecs: h264`.
3. `remove_track` + novo `add_track` falha ("same envelope"): por isso parar/retomar usa mute.

## Evidência (livekit-server v1.13.11 oficial, `--dev`, loopback; SHA-256 do zip conferido com o `checksums.txt` do release: `5d071ea4…3667`)
`cargo test --release sfu -- --test-threads=1` com `BRIKS_LIVEKIT_SERVER_BIN` apontando para o `livekit-server.exe`: 11 testes passam.
- Entrar com token, ver o outro participante, saída vista pelo outro.
- Áudio sintético 440 Hz: 132/132 quadros recebidos (0% de perda), 1º quadro 15–41 ms depois de abrir o mic, tom 443 Hz, CPU ~9–11% de 1 núcleo (2 clientes + testes).
- Vídeo de tela sintético 720p (encoder/decoder **software**, sem GPU) publicado por A e assistido por B, duas rodadas parar/retomar: ~20 fps (limite do software),
  latência captura→quadro p50 30–54 ms, p95 50–125 ms. Feedback do SFU ao publisher: 10 Receiver Reports, RTT 0,4 ms, perda 0, PLIs atendidos.
- Token: sessão da conta (Supabase simulado) → `Bearer` correto no serviço de tokens → entra no SFU; sem sessão: erro legível e fallback P2P; o token nunca aparece em mensagens de erro.

## Não feito / limites (honestamente)
- **NVENC/MFT sobre o SFU não foi exercitado**: o lock de GPU estava com outra sessão. O caminho é o mesmo `VideoOut`, mas o perfil H264 do encoder de hardware
  precisa ser conferido com o SDP (42e01f) se for abrir para navegadores. Comando: `BRIKS_SFU_TEST_GPU=1 cargo test --release sfu_screen` (só com `A:\BRiskCall\.gpu-test.lock`).
- Adaptação: usa Receiver Reports/NACK/PLI. TWCC/REMB do servidor (estimativa de banda) ainda não; não testado sob perda/atraso (o netsim não fica entre o cliente e o LiveKit).
- Nome e impressão digital dos participantes: o SFU só conhece a identidade do token; hoje `fp` = identidade. Falta um "hello" assinado por DataChannel
  (chave ECDSA) para mostrar nome/amigos como no P2P. Status/presença e chat não passam pelo SFU. Expulsar exige a API de administração do LiveKit (ainda não).
- Reconexão automática do WebSocket/ICE: se a sessão cair, o core avisa e a UI sai da sala (sem retomada).
- Automático: quem não consegue o SFU cai no P2P e fica em outra "sala" que quem conseguiu. Aviso já existe na UI ("todos precisam usar o mesmo modo").
- Teste contra LiveKit Cloud não feito (sem credenciais).

## O que o usuário precisa fazer
1. LiveKit Cloud (ou VPS: `infrastructure/livekit/`): criar projeto, copiar URL `wss://…`, API key e secret.
2. `supabase secrets set LIVEKIT_API_KEY=… LIVEKIT_API_SECRET=… LIVEKIT_URL=wss://…` e `supabase functions deploy livekit-token` (JWT do gateway ligado).
3. No app: `BRIKS_LIVEKIT_URL` e `BRIKS_LIVEKIT_TOKEN_URL=https://<ref>.supabase.co/functions/v1/livekit-token` (ou `livekit.json` na pasta de dados / o `.env` com `VITE_LIVEKIT_*`).
4. Entrar na conta Briks no app e escolher Configurações > Rede > Modo da chamada. Conferir o diagnóstico ("SFU · servidor …").

## Próximos marcos (pedidos pelo coordenador; ainda NÃO FEITOS)
Estado em 01:47: o lock de GPU pertence à sessão da transmissão; nada de GPU foi rodado depois do primeiro relatório.
1. NVENC/MFT sobre o SFU (`BRIKS_SFU_TEST_GPU=1`), conferindo o perfil H264 contra o SDP: aguarda o lock.
2. Reconexão automática com backoff e token renovado: não feito.
3. Adaptação sob perda/atraso (merge de `cui-transmissao` + netsim) e TWCC/REMB: não feito.
4. Hello assinado por DataChannel (nome e fingerprint reais): não feito.
5. Regra proposta para o Automático sem salas divididas: SFU só se há configuração válida E o token foi obtido; qualquer falha no token = P2P para aquela pessoa. Para ninguém ficar sozinho em outra sala, o ideal é o dono/primeiro a entrar anunciar o modo no Realtime e os demais seguirem: não implementado.
6. **Risco achado por inspeção:** a UI manda `chat.send`/`chat.event.send` pela mesma sala `room` e, no modo SFU, o core responde "chat não disponível no modo SFU". Ou seja, hoje o SFU quebra o chat da sala. Correção prevista: no modo SFU manter também uma malha só de DataChannel (sem mídia) para chat, status, presença e expulsão, e rotear mídia ao SFU; precisa reconciliar `peer_id` (UUID da malha x identidade do SFU). Até lá, o Automático com SFU configurado não deve ser ligado para usuários.
7. Expulsar/mudo remoto por mensagens assinadas do dono: NÃO FEITO.

## Marco 6+4 (feito): modo híbrido, chat/status/presença/expulsão junto do SFU

- `hybrid.rs` junta o SFU (voz e tela) com a malha só de DataChannel (`audio: None`) num único `MeshHandle`: IPC e UI não mudam.
- Identidade: cada nó anuncia a sua identidade do SFU no `peer.state` da malha (campo `sfuId`, aceito só de peer autenticado pela prova ECDSA); eventos do SFU (fala, transmissão) saem com o peer_id da malha.
- Automático NUNCA escolhe SFU (só escolha manual em Rede ou configuração explícita) até o híbrido ser validado em uso real.
- Teste `tests_hybrid::hybrid_chat_presence_voice_share_and_kick` (livekit-server --dev real): DM, evento de chat assinado, status, voz (66 quadros/2 s), mudo, tela com peer_id da malha, expulsão pela dona. Passa.
- Limite: a malha tem `MAX_PEERS = 5`, então o chat em sala grande no SFU ainda é limitado. NÃO FEITO: reconexão automática, regra do Automático, perda de pacotes/merge `cui-transmissao`, NVENC sobre SFU, mudo remoto assinado.

## Marco 1 (feito): NVENC/MFT sobre o SFU

`BRIKS_SFU_TEST_GPU=1 cargo test --release sfu_screen` com o lock `A:\BRiskCall\.gpu-test.lock` criado de forma exclusiva e removido ao fim: codificador "NVIDIA H.264 Encoder MFT" (hardware: true), publicado ao `livekit-server --dev` e assistido por outro cliente (decodificação por software no teste). 40 quadros em 4 s (a fonte sintética roda a 10 fps), latência captura→quadro p50 35-42 ms, p95 69-92 ms, 12 Receiver Reports, RTT 1 ms, 13 PLIs. O SFU aceitou o H264 do NVENC (perfil negociado no SDP) e repassou ao assinante.

## Marco 6 (feito): camada de dados própria do SFU (sem teto de 5 peers)

Objetivo do usuário: o SFU deve conectar MUITO mais de 5 pessoas. A malha P2P (`MAX_PEERS = 5`) deixou de ser usada no modo SFU. `sfudata.rs` leva chat, status, presença, estado do microfone, expulsão e mudo remoto pelas data channels do LiveKit (DataPacket reliable, tópico `briks`), todas as mensagens assinadas pela identidade ECDSA.

- Confiança: o servidor carimba a identidade do remetente em cada pacote. O `hello` assinado carrega `sfuId` dentro do texto assinado: só vale se `sfuId == remetente carimbado` e a assinatura bate com a chave anunciada (que define a impressão digital). A 1ª identidade de um `sfuId` vale até ele sair.
- Mensagens: `hello`, `state` (mic/ensurdecido/status/presença, `ts` monotônico), `chat` (ChatWire v2 assinado, DM só ao destinatário, com `ack` e reenvio), `event` (`chat.event` assinado), `kick`/`allow`/`mute` (só o dono; assinatura sobre sala+alvo+ts, janela de 5 min).
- Descartes: lixo, `v` errado, > 15 KB, de quem não fez hello, de banido, estado repetido/antigo, conteúdo adulterado, não-dono, ts fora da janela, inundação (400 msgs/10 s por remetente). Prova em 10 testes unitários e ao vivo (cliente 4 injeta lixo e comandos forjados via data channel; ninguém é expulso/silenciado).
- Robustez: hello com `ack` (uma resposta perdida se repara sozinha, sem ping-pong); difusões (estado/evento) repetidas após 3 s e 9 s (receptores deduplicam); participante que nunca faz hello aparece como "não verificado" (fp = identidade do SFU) após 12 s.
- Dono: `owner_fp` do app ou o de menor `joinedAt` (assinado no hello) entre os autenticados, como na malha.
- IPC: no modo SFU o core não abre malha nem Realtime; o `peer_id` local é a identidade no SFU. `BRIKS_SFU_MESH=1` volta ao híbrido (SFU + malha até 5) de `hybrid.rs`.
- Teste ao vivo `tests_sfudata::sfu_data_layer_scales_past_five_clients` (`BRIKS_SFU_CLIENTS`, padrão 10): todos autenticam todos com o fp real, todos ouvem voz do dono e do vizinho, status, evento e DM (sem vazar), mudo remoto, expulsão pela dona. Passou com 10, 12, 16 e 20 clientes (20 duas vezes). Com 25 clientes num único processo o teste falha de forma intermitente (presença passa; um DM/evento fica sem chegar). Causa NÃO confirmada: 25 clientes x 24 fluxos de áudio decodificados no mesmo processo saturam a CPU; precisa de teste com clientes em processos separados.
- Limites: expulsar faz os clientes honestos ignorarem e silenciarem o banido (ele sai sozinho), mas só a API de administração do LiveKit (RemoveParticipant, numa função do Supabase que verifique o dono) o tira do servidor de mídia: NÃO FEITO. Mudo remoto assinado: FEITO só na camada de dados do SFU (a malha P2P responde "só no modo SFU"). Não testado pela UI C nem por `room.join` de ponta a ponta no IPC com SFU puro.

---

# Estado em 2026-11-11 (substitui as listas de "não feito" acima onde houver conflito)

Todos os testes abaixo rodam contra o `livekit-server --dev` oficial em loopback, com fontes sintéticas e codificadores de
software (sem GPU, sem janela, sem capturar tela). `BRIKS_LIVEKIT_SERVER_BIN` aponta para o binário; `--test-threads=1`.

## Marco: reconexão automática (`sfu.rs`, teste `tests_sfurec`)
- Gatilhos: WebSocket fechado, servidor mudo além do `ping_timeout`, PeerConnection `Failed`, `Leave` do servidor pedindo reconexão.
- Tentativas em 0,2 / 0,5 / 1 / 2 / 4 / 5 s (±20%); desiste em 2 min com `signaling.status` "erro" (a sala NÃO fica bloqueada).
- Token novo uma vez por queda (`TokenFn`: no app, a função `livekit-token` com a sessão da conta) e de novo se o servidor recusar (401/403).
- Volta do zero (o servidor pode ter reiniciado): PeerConnections novos, as mesmas faixas (microfone, tela, áudio da tela) republicadas
  sem parar o codificador; quem assistia reassina sozinho; mensagens e estado enviados durante a queda saem ao voltar.
- 20 s de tolerância depois de voltar antes de anunciar que alguém saiu ou parou de transmitir (a lista não pisca).
- `Kicked` só para `PARTICIPANT_REMOVED`. Antes, qualquer queda de conexão virava "expulso" e bloqueava a sala na sessão.
- Prova: servidor morto (kill) e reiniciado 2 vezes com 3 clientes; token de entrada com 6 s de validade (só entra quem renovou);
  voz nos 6 sentidos, vídeo assistido, DM e status da queda entregues; ninguém "saiu"; saída real ainda é vista. Voltam ~0,5 s depois de o servidor subir.
- NÃO testado: desistência após 2 min; reconexão com LiveKit Cloud (migração de nó). A retomada de sessão (`reconnect=1`) não é usada: sempre entra de novo.

## Marco: perda, queda e banda sobre o SFU (`tests_sfurede`, CSV em `apps/desktop-cui/provas/sfu/`)
O proxy UDP do `netsim` fica entre UM cliente e o servidor (candidatos ICE dos dois lados reescritos; nada contorna o proxy).
720p30, bitrate fixo, codificador `mf-soft` (que estoura o pedido: ~6 Mbps em vez de 3). Rodada que está no CSV:

| cenário | envio (emissor→SFU) qps / p50 | recepção (SFU→espectador) qps / p50 |
|---|---|---|
| base | 30,0 / 46 ms | 29,5 / 52 ms |
| perda 1% | 30,0 / 55 ms | 29,9 / 61 ms |
| perda 2% | 30,1 / 62 ms | 30,1 / 128 ms |
| perda 5% | 29,0 / 94 ms | 30,1 / 158 ms |
| perda 10% | **4,4 / 337 ms** | 29,6 / 199 ms |
| queda total de 4 s | 1º quadro 1,2 s após a volta, sem reconexão | 1º quadro 1,1 s após a volta, sem reconexão |

- **10% de perda: NÃO resolvido.** No envio a transmissão degrada muito (0,2 a 11 qps conforme a rodada); na recepção variou entre
  ~29 e 1,5 qps. Na malha P2P, com o mesmo codificador, 10% também degrada (17,6 qps, p50 285 ms). Em 2% houve uma rodada com 23,5 qps.
  O teste só exige ritmo até 5%; a 10% exige apenas voltar ao normal quando a perda passa.
- Mudança: o intervalo mínimo entre retransmissões do mesmo pacote para o SFU segue o RTT (30 a 150 ms) em vez dos 150 ms da malha
  (o LiveKit pede poucas vezes e desiste). Não mostrou ganho mensurável a 10%.
- Adaptação (`adapt.rs`) com o RR/NACK do SFU como único "par": banda do emissor limitada a 1,2 Mbps → desce para 360p, ~720 kbps
  (estimativa 922 kbps, motivo "perda" e depois "atraso"); liberada → volta a 720p, ~2,1 Mbps. Recebido enquanto apertado: 7 qps.
- NÃO feito: TWCC/REMB do servidor como estimativa de banda; simulcast; teste com NVENC sob perda (GPU não usada nesta rodada).

## Marco: regra do Automático sem salas divididas (`sfu_cfg::decide`, `ipc.rs`)
- Com o SFU configurado e válido, **Automático e "SFU" entram pelo servidor de chamadas** (SFU é o padrão quando configurado).
- Só entra no SFU quem tem configuração válida E conseguiu o token/entrada. Se falhar, a chamada **não abre em P2P**: `net.mode`
  com `mode:"none"` e erro em `room.join` com o motivo. Antes a pessoa caía sozinha numa sala P2P que o SFU não enxerga.
- Reserva P2P só ligada de propósito: `BRIKS_NET_FALLBACK=p2p` ou `"fallback":"p2p"` no `net-mode.json`.
- Sem configuração: P2P como sempre. "P2P" nunca tenta o SFU. Chat de amigo (`friend`/`chat`) continua na malha.
- **Decisão a validar com o usuário:** a troca é disponibilidade por consistência. Se o LiveKit/Supabase cair, as chamadas no
  Automático não abrem até voltar (ou até todos escolherem P2P). A alternativa mais completa (anunciar o modo da sala no Realtime e
  todos seguirem) NÃO foi feita. Configuração inválida num só aparelho ainda o deixa em P2P (aparece no diagnóstico).

## Marco: IPC de ponta a ponta com SFU puro e mudo remoto (`tests_sfuipc`)
- Dois cores (IPC), contas no servidor simulado, serviço de tokens simulado, LiveKit real, **sem Realtime**: Automático escolhe o SFU,
  participantes com impressão digital real, dono, fala, DM, status, evento de chat, `room.mute` (recusado para não-dono; a dona fecha
  e reabre; o estado local do microfone acompanha), expulsão, volta barrada, e sem conta a chamada não abre.
- IPC novo: `{"cmd":"room.mute","fp":"…","muted":true}`; evento `room.muted` (+ `mic.state`); `net.mode` pode vir com `mode:"none"`.
- UI C: "Silenciar para todos" / "Reabrir o microfone dele" no menu do participante (só o dono, só no SFU). Compila. Rodada oculta
  (`BRIKS_HIDDEN=1`) feita só no caminho de erro: sem conta, a tela mostra "Não foi possível entrar pelo servidor de chamadas…".
- NÃO feito: chamada completa pela UI C com o SFU (precisa de conta logada no roteiro de teste); clique real no item novo do menu.
  A UI mostra no máximo 8 participantes (`rtc_peers[8]`): o core aguenta mais, a tela não.
- O mudo remoto é um pedido: a pessoa silenciada pode reabrir o próprio microfone.

## Marco: falha intermitente com 25 clientes (`tests_sfuproc`, processos separados)
A máquina de teste (16 threads) estava sendo usada por outras sessões; a carga só foi medida nas últimas rodadas.
- 25 clientes num processo só (`tests_sfudata`, `BRIKS_SFU_CLIENTS=25`): 5 passaram e 7 falharam em 12 rodadas. As 3 primeiras passaram
  em ~9 s; as que falharam levaram 32–110 s e pararam cada uma num passo diferente (voz, status, mudo, banimento).
- 25 clientes em processos separados, todos falando: 4 passaram e 1 falhou em 5 rodadas (na que falhou a voz chegou com 36 de 150 quadros).
  As duas últimas passaram com a máquina a 57–80% de CPU antes de começar.
- 8 falando e o resto ouvindo, máquina a 60–80%: **40 clientes: 0 falhas; 60 clientes: 0 falhas** (presença completa em ≤ 10,8 s;
  voz, status, evento, DM, mudo remoto e expulsão conferidos em todos). 40 clientes todos falando: quebra (voz 0–10 de 150 quadros).
- O log do servidor nas rodadas de 25 não tem nenhum WARN/ERROR nem descarte de dados.
- Leitura (inferência, não prova): é falta de CPU/tempo, não erro de lógica na camada de dados. 25 pessoas falando AO MESMO TEMPO custam
  4 a 6,5 núcleos só nos clientes (cada um decodifica 24 vozes) e ~30 mil pacotes/s no servidor; o teste num processo divide isso em
  4 threads e tem prazos fixos. O que exatamente atrasa dentro do cliente sob carga NÃO foi isolado.
- NÃO feito: medir o servidor (CPU/rede) em separado; rodar em máquina ociosa e em máquinas diferentes; limitar no cliente quantas vozes
  são decodificadas ao mesmo tempo (hoje assina e decodifica todas).

## Marco: função de expulsão `livekit-kick` (escrita e testada; NÃO publicada; o app ainda não chama)
- `supabase/functions/livekit-kick/index.ts` + `_shared/kick.ts` (regra) + `_shared/livekit-admin.ts` (ListParticipants/RemoveParticipant
  por Twirp, token de administração de 60 s restrito à sala; sem dependências).
- Regra: sessão válida; quem pede é sempre o usuário da sessão; precisa estar na sala e ser o **dono = participante mais antigo
  segundo o LiveKit**; o alvo precisa estar na sala. Limite de requisições por IP e por usuário. Erros internos não vazam.
- Testes com simulações: `apps/web/src/services/rtc/edge-kick.test.ts` (15 casos, vitest).
- Teste real local, sem Supabase: `tests_sfukick` roda a mesma regra pelo Node (`supabase/tests/livekit-kick-local-check.ts`) contra o
  LiveKit real: não-dono 403, dona 200, o removido recebe o aviso e não reconecta, os outros veem a saída.
  (`BRIKS_NODE_BIN`; as chaves são as públicas do `--dev`, passadas pelo teste.)
- LIMITES: o dono do servidor é "o mais antigo na sala"; depois de uma reconexão geral a ordem pode mudar e divergir do dono que os apps
  enxergam (dono fixo exige uma tabela de salas). O expulso pode pedir outro token e voltar: o bloqueio é só no app dele e na lista
  de banidos dos outros apps. NÃO feito: o app chamar a função ao expulsar; rodar no Deno/Supabase de verdade (`Deno.serve` e os
  limitadores do `index.ts` não foram executados).

## Ainda NÃO feito (resumo)
Teste com LiveKit Cloud (sem credenciais); embutir `livekit.json` no instalador; chamada completa pela UI C no SFU; UI para mais de
8 pessoas; 10% de perda; TWCC/REMB e simulcast; anunciar o modo da sala no Realtime; app chamando `livekit-kick`; dono fixo no servidor.
Como ligar: `docs/ativar-sfu.md` (seção do app nativo).

---

# Rodada 2 de 2026-11-11 (decisões do coordenador)

## Falha do SFU: erro claro + "Entrar em modo P2P" (UI C)
Continua sem reserva silenciosa. Quando `net.mode` vem com `mode:"none"`, a tela da chamada mostra uma faixa com o motivo,
"Tentar de novo" e "Entrar em modo P2P" (reentra na mesma sala com `"net":"p2p"`, só para quem clicou, até sair da chamada).
O texto avisa que em P2P só se fala com quem também entrou em P2P. Sem anúncio de modo pelo Realtime.
Conferido por captura oculta (`BRIKS_DEMO_PEERS=9 BRIKS_DEMO_NETFAIL=1`). NÃO feito: clique real no botão num roteiro de teste.

## Expulsão chama a função `livekit-kick`
No SFU puro, `peer.kick` do dono manda o aviso assinado (como antes) e também pede à função para derrubar a conexão:
POST `{room, identity}` com `Authorization: Bearer <sessão da conta>`. URL: `BRIKS_LIVEKIT_KICK_URL`, `kick_url` no `livekit.json`
ou a irmã da função de tokens (`…/livekit-token` → `…/livekit-kick`). Só é chamada se sou o dono e conheço a identidade do alvo.
Falha (função não publicada, 500, sem rede) não vira erro para o usuário: sai só o evento de diagnóstico `net.kick`.
Teste com a função simulada em `tests_sfuipc` (Bearer da dona, sala do LiveKit, identidade; não-dono não chama; 500 tratado).

## UI para mais de 8 pessoas
`BRIKS_MAX_PEERS = 32` (antes 8 fixo em `rtc_peers`, nas transmissões remotas e nos níveis de fala). A chamada virou grade
(linhas × colunas conforme o total; avatar, iniciais e rótulo encolhem juntos; com 1 a 3 pessoas fica igual a antes).
Capturas ocultas conferidas com 2, 9, 25 (1280×800) e 32 pessoas (900×600: cabe, mas o rótulo cobre o avatar pequeno).
Sem rolagem: acima de 32 remotos os excedentes não aparecem (o áudio deles toca). NÃO conferido: transmissões de tela com muitos
participantes, menu de contexto nos quadros pequenos, zoom/escala do sistema diferente de 100%.

## 10% de perda no envio: melhorou, NÃO resolvido
- Mudança: com perda alta no relatório do SFU (liga em 3,5%, desliga abaixo de 1,5%; o relatório mostrou cerca de metade da perda
  real), cada retransmissão pedida sai em dobro, dentro do mesmo orçamento de retransmissão. Só no modo SFU.
- Bitrate fixo, 720p30, `mf-soft`, 10% no envio: antes 0,2 / 3,4 / 4,4 / 5,5 / 11 qps em cinco rodadas; depois **17,2 / 16,1 / 10,2 qps**
  em três rodadas, ainda com congelamentos de 1,6 a 5 s. 5% segue em 28–30 qps. Uma rodada com o limiar antigo (7%) deu 24,6 e outra 5,5.
- Com a adaptação ligada (como no app; `sfu-perda-envio-adaptativo.csv`, medido ANTES da retransmissão em dobro): não desaba de vez,
  mas o estimador trata perda aleatória como congestionamento e desce sem parar (1316 → 575 → 223 kbps, 360p a 10 qps; recebido
  7,2 / 9,5 / 3,5 qps, com congelamentos de 4 s).
- Limite registrado: enlace de envio com 10% de perda aleatória fica ruim no SFU. O que falta tentar: FEC de verdade (FlexFEC/RED),
  quadro-chave menor/intra-refresh, piso no estimador para perda que não responde a corte de taxa, TWCC. O CSV da matriz no
  repositório é da rodada anterior a esta mudança.

## Contrato da função `livekit-token` (branch `claude/cui-contas-b3d4c3`)
A função nesse branch é idêntica à deste worktree (diff vazio). O cliente casa com ela:
- pedido: POST, `Authorization: Bearer <sessão>`, corpo `{"room": "<ns>:<32 hex>"}` (cabe no padrão `^[A-Za-z0-9:_.\-]{1,160}$`);
- resposta: usa `token`; identidade = id do usuário (o cliente não manda `peerId`).
Divergências/atenções: (1) o cliente ignora o campo `url` da resposta e usa a URL do `livekit.json`: se as duas diferirem, o token
é de um servidor e a conexão vai para outro (erro de entrada, sem aviso específico); (2) limite de 30 tokens por usuário a cada
10 min: uma queda longa com o servidor recusando pode gastar isso, porque cada recusa pede token novo; resposta 429 hoje aparece
só como "serviço de tokens recusou (429)"; (3) a função não confere se o usuário pertence à sala (limitação já descrita nela).
