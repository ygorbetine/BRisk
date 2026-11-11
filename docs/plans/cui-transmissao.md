# Transmissão de tela/janela no CUI

Estado em 2026-11-10, branch `cui-transmissao`. Código em `apps/desktop-cui/briks-core/src/{capture,video,mfenc,share,procaudio}.rs`
(núcleo) e `apps/desktop-cui/src/ui_share.c` (interface).

## Como funciona

```
captura (Windows Graphics Capture) ─► BGRA→I420 com escala ─► H264 (Media Foundation: GPU; senão software) ─► faixa WebRTC "screen-video"
áudio por processo (WASAPI process loopback) ─► mixagem ─► Opus estéreo ─► faixa WebRTC "screen-audio"
receptor: RTP ─► H264 (Media Foundation) ─► NV12→RGBA ─► arquivo mapeado ─► textura OpenGL na UI
```

- **Captura**: tela (HMONITOR) ou janela (HWND). A leitura usa uma única textura D3D11 de leitura reaproveitada; a conversão de cor lê
  direto da memória mapeada. Miniaturas do seletor por GDI (PNG em `<dados>/thumbs`).
- **Codificação**: `H264Enc` tenta o MFT de hardware (NVENC / Quick Sync / AMF), depois o codificador de software da Microsoft e por
  último o OpenH264. `BRIKS_ENCODER=mf-soft|openh264` força um deles. CBR, baixa latência, quadro-chave a cada 4 s ou por PLI/FIR.
- **Faixas**: criadas na primeira transmissão e mantidas ligadas a cada peer; parar e retomar não renegocia (só para de enviar). Quem
  entra depois recebe as faixas ao autenticar. O estado (ligada/desligada, tamanho, FPS, bitrate, rótulo, áudio) vai pelo
  DataChannel autenticado como `share.state`.
- **Receptor**: só decodifica enquanto alguém assiste (`share.watch`); fora disso os pacotes são descartados. Regra do produto: a
  chamada abre sem exibir transmissões e assistir exige o modo Transmissão.
- **Áudio**: uma captura por processo, com a árvore de filhos. "Da fonte" = processo da janela; numa tela inteira = tudo que tem
  sessão de áudio, menos executáveis que começam com `briks` ou `discord`, reavaliado a cada 2 s. "Escolher apps" = lista de PIDs,
  trocável durante a transmissão (`share.audio.set`). O receptor mixa em mono junto com as vozes; ensurdecer/silenciar valem também
  para essa faixa.
- **Qualidade**: resolução (720p, 1080p, 1440p, 4K; nunca amplia a origem), FPS (15, 30, 60) e bitrate (automático por resolução e
  FPS; Alto = 1,6×; Máximo = 2,5×; Personalizado 1–50 Mbps, com aviso acima de 20 Mbps).

## Quadro compartilhado (core → UI)

Arquivo mapeado de tamanho fixo em `<dados>/video/` (`preview.bin` para a prévia local, `remote-<peer>.bin` por transmissor assistido):

| deslocamento | campo |
|---|---|
| 0 | `BKFR` |
| 4 | u32 `seq` (ímpar = escrevendo) |
| 8, 12 | u32 largura, altura |
| 16 | u64 número do quadro |
| 24 | u64 relógio da gravação (ms) |
| 64 | RGBA |

A UI lê `seq`, sobe a textura e confere `seq` de novo; se mudou, sobe outra vez no quadro seguinte.

## IPC

Comandos: `share.list`, `share.audio.list`, `share.start {kind: screen|window|test, id, height, fps, kbps (0 = automático), label,
audio: source|apps|none, pids}`, `share.audio.set {audio, pids}`, `share.stop`, `share.watch {peer, on}`.

Eventos: `share.sources`, `share.audio.apps`, `share.local {state: on|off, w, h, fps, kbps, encoder, hardware, audio, reason}`,
`share.stats {fps, kbps, enc_ms, cpu_pct, audio_peak, audio_sources}`, `share.remote {peer_id, state: on|off|watch|unwatch|frame, ...}`.

## Provas

- `cargo test --release` em `briks-core` (defina `CMAKE_POLICY_VERSION_MINIMUM=3.5`): `screen_share_flows_end_to_end_in_loopback`
  compara o quadro recebido com o enviado e mede a latência; `late_joiner_resize_and_leave_keep_the_share_alive` cobre três pessoas,
  mudança de tamanho da fonte e saída de um espectador.
- `--ignored`: `captures_only_the_target_process` e `process_audio_reaches_the_viewer` (tocam som), `real_screen_share_stats` e
  `bench_convert_and_encode` (medições).
- `bash scripts/prova-transmissao.sh [pasta]`: sobe o Realtime simulado e três instâncias do app e salva capturas 1280x800.
  `scripts/medir-transmissao.ps1` mede CPU por processo e GPU.

## Limites conhecidos

- Bitrate fixo: não há controle de congestionamento (REMB/TWCC). Numa rede ruim o vídeo trava em vez de baixar a qualidade.
- Só H264. Um peer sem H264 negociado não recebe vídeo.
- O áudio da transmissão chega em mono no receptor.
- O perfil anunciado no SDP é Constrained Baseline 3.1 mesmo em 1440p/4K; os decodificadores testados aceitam.
- A conversão de cor e a escala são na CPU. O caminho todo em GPU (processador de vídeo D3D11, textura direto no codificador,
  YUV em shader na UI) reduziria bastante a CPU em telas grandes.
- O `briks-core` continua vivo esperando outra UI quando o app é encerrado à força (comportamento anterior a este trabalho).

## Não testado

Supabase real, TURN, perda de pacotes real, mais de 3 pessoas, interoperabilidade com cliente Chrome/Electron, máquina sem GPU
(só forçando `mf-soft`), AMD/Intel, jogo em tela cheia exclusiva, janela minimizada, HDR, áudio com o Discord realmente tocando.
