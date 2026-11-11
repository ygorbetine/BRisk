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
