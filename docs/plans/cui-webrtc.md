# WebRTC nativo no app CUI (marcos 3 e 4) — branch experimento/cui-webrtc

Base: experimento/cui-janelas (layout de painéis ao vivo). Este plano cobre `briks-core`: sinalização real, DataChannel de chat e áudio. Vídeo/tela ficam só planejados.
Regras: não commitar sem ordem, não mexer no brisk-dev, não publicar versão. Nada de credenciais digitadas ou contas no Supabase de produção.

## Decisão: como a UI fala com o briks-core

**Processo filho com IPC local por TCP loopback + token** (mesmo padrão do sidecar `briks-desktop-backend`), em vez de DLL.

| | Processo filho (escolhido) | DLL |
|---|---|---|
| Isolamento | queda do WebRTC/áudio não derruba a UI; reinício independente | queda leva a UI junto |
| Runtime | tokio inteiro dentro do próprio processo, sem ponte C↔async | precisa expor ABI C estável e threads do tokio dentro do processo da UI |
| Cliente em C | winsock + linhas JSON (o `backend.c` já faz HTTP loopback) | `cbindgen` + gestão de memória na fronteira |
| Empacotamento | mais um exe no zip (já existe o `briks_overlay.exe`) | 1 arquivo a menos |
| Custo | +1 hop local (µs), irrelevante para eventos/estado | zero |

A DLL só ganharia em latência de áudio, e o áudio não passa pela UI (captura/codec/rede ficam todos no core). Reavaliar se o IPC virar gargalo.

**Protocolo de IPC** (`briks-core serve-ipc`): o core escolhe porta loopback livre, imprime `BRIKS_CORE_READY {"port":N}` no stdout; a UI conecta e manda `{"cmd":"hello","token":"..."}` (token aleatório passado por variável de ambiente `BRIKS_CORE_TOKEN`, nunca por argumento de linha de comando). Depois, linhas JSON nos dois sentidos:

- UI → core (comandos): `session.start {user_id, display_name}`, `room.join {room}`, `room.leave`, `mic.set {on}`, `deafen.set {on}`, `chat.open {channel, peer_user_ids}`, `chat.send {channel, text}`, `chat.history`…
- core → UI (eventos): `signaling.status {state}`, `room.peers [...]`, `peer.state {peer, connection}`, `speaking {peer, level}`, `chat.message {...}`, `error {where, message}`.

Estados de conexão exibidos na UI: `desconectado → conectando → conectado | erro (mensagem)`.

## Etapa 1 — sinalização Supabase Realtime + DataChannel de chat

**Realtime (protocolo Phoenix)** em `tokio-tungstenite` (+ rustls): `wss://<projeto>.supabase.co/realtime/v1/websocket?apikey=<anon>&vsn=1.0.0`. Frames `{topic, event, payload, ref, join_ref}`; `phx_join` em `realtime:briks:<namespace>:<sha256(namespace:sala)[0..32]>` com `config.broadcast {self:false, ack:true}` e `config.presence {key: peerId}`; heartbeat em `phoenix` a cada 30 s; `presence_state`/`presence_diff` viram `peerJoined/peerLeft` (com graça de 10 s antes de declarar saída, como o cliente TS); `broadcast` com eventos `webrtc.offer|answer|ice-candidate` e `peer.state`. O envelope e as validações (versão 1, hash da sala, `targetPeerId`, peer presente, janela de timestamp, tamanho, dedupe por `messageId`, rate limit) reproduzem `docs/protocol.md` e `apps/web/src/services/supabase/signaling.ts`, para falar com clientes Electron na mesma sala. O hash é SHA-256 do texto `<namespace>:<sala>`.

**Configuração**: só variáveis que o repositório já define: `VITE_SUPABASE_URL` e `VITE_SUPABASE_ANON_KEY` (`.env.example`), lidas do ambiente do processo ou de um `.env` na raiz do repositório quando houver. Hoje **não existe `.env` nem variável definida neste computador**, então testes contra Supabase real exigem uma decisão do usuário (projeto de desenvolvimento). Sem a configuração o core entra em `erro: Supabase Realtime não configurado` e a UI mostra o aviso.

**Negociação**: ordem lexical dos `peerId` define quem inicia; Perfect Negotiation completa (polite/impolite, rollback na colisão), fila de ICE até a `remoteDescription` (256 por peer), limite de 5 peers (Mesh).

**DataChannel `briks.chat`**: o iniciador cria o canal ordenado. Mensagem wire v2 assinada: `{version:2,type:"chat.message",channelId,id,authorPeerId,author,content,timestamp,signature}`; o texto assinado é o JSON canônico de `canonicalSignedMessage` (`version,type,channelId,id,authorPeerId,author,content,timestamp`, nessa ordem). Assinatura ECDSA P-256/SHA-256 em formato raw r‖s (como o WebCrypto), base64url. Canal da DM = `dm-` + SHA-256(`briks-dm-v1:` + par ordenado de peerIds). ACK/outbox (`chat.message.ack`), dedupe por id, limite de 64 KiB por payload e 4.000 caracteres por mensagem.

**Identidade**: o app nativo gera um par ECDSA P-256 novo na primeira execução (crate `p256`), guardado no diretório de dados do core (`%APPDATA%\Briks-CUI\core-identity.json`, chave privada em texto no perfil do usuário nesta rodada; proteger com DPAPI fica pendente). **Incompatibilidade documentada**: a chave do Electron é `CryptoKey` com `extractable=false` no IndexedDB e não pode ser exportada; portanto o app nativo é *outra identidade* para quem já tem o Briks no Electron. O vínculo (um amigo só aceita mensagens de uma chave que já conhece) exige reenviar o convite/aceite pelo fluxo de amizade; isso é do protocolo, não um defeito do core. O desafio bilateral `chat.identity.proof` (nonce, `canonicalIdentityProof`) vem na etapa 1b.

**Persistência**: mensagens recebidas/enviadas vão para o sidecar existente (`/p2p/messages/{canal}`), como hoje; o core avisa a UI e a UI grava, ou o core chama o sidecar (decidir ao implementar; começar com a UI gravando, mais simples).

**TURN**: o core busca credenciais temporárias na função `turn-credentials` (exige sessão; usa o bearer do app, nunca `TURN_SECRET`). Sem TURN a conexão só funciona com NAT permissivo: **risco documentado** (CGNAT/firewall simétrico falham; o app deve dizer "Somente STUN" como o Electron).

**Testes da etapa 1** (`cargo test` + `briks_smoke`): (a) Phoenix: codificação/decodificação de frames e presença em servidor simulado; (b) servidor Phoenix/Realtime simulado em loopback (`127.0.0.1`) com dois cores em processo e Perfect Negotiation com colisão forçada; (c) DataChannel: mensagem assinada ida e volta, assinatura adulterada rejeitada, id repetido deduplicado; (d) IPC: token errado recusado, comandos/eventos; (e) C: parser de linhas e estado de conexão no smoke. Contra Supabase real: só com projeto de dev aprovado.

## Etapa 2 — áudio

- Captura WASAPI (crate `cpal`, backend WASAPI), reprodução idem; reamostragem para 48 kHz mono/estéreo conforme o dispositivo.
- Opus (`audiopus`/`opus`, compila libopus com CMake, já disponível): 48 kHz, quadros de 20 ms, FEC/DTX ligados, ~32 kbps de voz.
- SRTP/DTLS: do próprio webrtc-rs (`TrackLocalStaticSample` + `on_track`); jitter buffer simples (webrtc-rs não traz um) para reordenar e esconder perda (PLC do Opus).
- Controles: `mic.set` (muta o envio e atualiza `peer.state.microphone`), `deafen.set` (zera reprodução, implica mic mudo, como no Electron), entrar/sair da sala (`room.join/leave`).
- Indicador de quem fala: RMS por quadro, local e remoto; limiar com histerese; evento `speaking` a ~10 Hz; a tela de chamada acende o halo do participante.
- Mute por pessoa já existe na UI; passa a ser ganho zero na reprodução daquela pessoa.
- Testes: loopback com dois cores, tom de 440 Hz gerado em vez do microfone, verificar energia/frequência no decodificado; teste de mutar/ensurdecer; sem placa de som no CI, a captura usa fonte sintética (`BRIKS_CORE_FAKE_AUDIO=1`). A reprodução real (WASAPI) só se verifica ouvindo: não automatizável aqui.

## Etapa 3 — vídeo e tela (apenas planejado)

H.264 por Media Foundation (MFT encoder/decoder de hardware com fallback), captura de tela por Windows.Graphics.Capture, áudio da tela pelos módulos `screen_audio`/`process_audio` do backend. Ajuste de bitrate por quantidade de peers como o Electron. Renderização dos quadros no CUI via textura (patch do renderer ou `cui_image` atualizado por quadro). Não implementado nesta rodada.

## Riscos e limites

- webrtc-rs 0.12 é menos maduro que libwebrtc (SDP/ICE com clientes Chromium precisa de teste cruzado com o Electron real, que só dá para fazer com um projeto Supabase de dev).
- NAT/TURN: sem TURN válido, redes com NAT simétrico/CGNAT não conectam.
- Perfect Negotiation sobre rollback: webrtc-rs suporta `SdpType::Rollback`; cobrir com teste.
- Peso do exe do core (~11 MB no spike) + libopus.
- Rede de teste somente em loopback ou em ambiente de dev indicado pelo repositório.

## Estado

### Etapa 1 (feita em 2026-11-10)
- `briks-core` vira serviço: `briks-core serve-ipc` (processo filho, TCP 127.0.0.1 porta livre, token por variável de ambiente, linhas JSON). A UI usa `src/core.c` (inicia/monitora, thread de leitura, fila de eventos) e `src/rtc.c` (estado de conexão, entrar/sair da sala, mic/ensurdecer). A tela de chamada mostra "Conectando… / Conectado · N pessoas / Erro: …" e os tiles viram os peers da sala.
- Sinalização: Phoenix/Realtime (`phoenix.rs`, `realtime.rs`: join, Presence com graça de saída, Broadcast, heartbeat, reconexão com backoff, ws:// e wss:// com rustls+ring), envelope v1 e validações (`signaling.rs`), malha com Perfect Negotiation por rollback real (`mesh.rs`), DataChannel `briks.chat`, desafio/prova ECDSA, mensagens v2 assinadas com ACK, outbox com reenvio e dedupe (`identity.rs`).
- Servidor Phoenix simulado em loopback (`briks-core mock-realtime [porta]`) para testes.
- **Testado**: `cargo test --release` = 19 testes passando (frames Phoenix, Presence, JSON canônico idêntico ao `JSON.stringify` do TS, assinatura/adulteração, validações e rate limit, dois peers reais em loopback autenticando e trocando chat assinado nos dois sentidos, outbox entregando mensagem enviada antes do peer existir, offers simultâneas convergindo, IPC com token errado/certo e erro de configuração ausente, erro de servidor offline). `briks_smoke` (C): core inicia, aceita token, sessão, entra e sai de sala contra o simulador. Captura da UI com core real + simulador: `shots/call-conectado-mock.png` ("Conectado · você está sozinho na sala").
- **Não testado**: contra o Supabase real (não há `.env` nem variáveis neste computador; falta um projeto de dev aprovado); interoperabilidade com o cliente Electron/Chromium; TURN (core usa `ice_servers` vazio, só candidatos host em loopback); mensagens do core ainda não são gravadas no sidecar nem aparecem no painel de conversa; mais de 2 peers.
- Chave de identidade: `%APPDATA%\Briks-CUI\core-identity.json` (segredo ECDSA; nova identidade, incompatível com a chave não exportável do Electron). O handshake nativo acrescenta `identity` (JWK) e `displayName` ao `chat.identity.proof`, campos que clientes TS ignoram ou rejeitam: só vale entre clientes nativos por enquanto.
