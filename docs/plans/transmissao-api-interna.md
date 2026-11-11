# API interna de publicação de vídeo H264 (para o SFU reaproveitar o codificador)

Tudo em `apps/desktop-cui/briks-core/src/`. O fluxo hoje: **captura → conversão (GPU) → H264 → `VideoOut` → pacer → RTP/SRTP por par**.

| Peça | Onde | Contrato |
|---|---|---|
| Captura | `capture.rs` `Capture::start(kind, fps, altura, preview, dump)` | entrega BGRA (ou I420 já convertido na GPU) em `Shared.latest` |
| Conversão | `gpuconv.rs` `GpuConv::convert(tex, sw, sh, dw, dh) -> Yuv` | BGRA→NV12/I420 + escala na GPU (cai para CPU em `video.rs`) |
| Codificador | `video.rs` `H264Enc::new(w,h,fps,bps)`, `encode(&Yuv) -> Vec<Vec<u8>>`, `force_idr()`, `set_bitrate(bps)`, `set_fps(fps)` | cada item devolvido é **uma unidade de acesso Annex-B**; o quadro-chave leva SPS/PPS; BT.601 limitado; backends: NVENC/MFT (`mfenc.rs`), MFT por software, OpenH264 |
| **Publicação** | `share.rs` `VideoOut::publish(au, samples, pace_bps)` | recebe UMA unidade de acesso H264 pronta. `samples` = ticks de 90 kHz desde o quadro anterior; `pace_bps` = ritmo de saída (2,5 × bitrate). Empacota (MTU 1180), espaça, guarda 2048 pacotes para NACK |
| Pedido de quadro-chave | `Sender.idr: Arc<AtomicBool>` (`request_idr()`) | PLI/FIR, novo espectador e erro de decodificação ligam o flag; o laço de codificação chama `force_idr()` (no máx. 1/s, 2/s com rede apertada) |
| Rede/adaptação | `adapt.rs` `NetShared` (por par: perda, RTT, jitter, NACKs, PLIs), `Estimator`, `Controller` | o laço de 1 s em `start_sender` escolhe altura/FPS/bitrate; **num SFU o par único é o servidor**: alimente `NetShared` com o RTCP do SFU (ou com a estimativa que ele der) e o resto continua igual |

## Para plugar um SFU
1. Reaproveite `Capture`, `GpuConv` e `H264Enc` sem mudança.
2. Troque o destino: em vez de `VideoOut::publish`, entregue a unidade de acesso ao publicador do SFU (cada item de `encode()` é um quadro completo; pule o empacotamento RTP se o SDK empacotar).
3. Mantenha `request_idr` ligado ao pedido de quadro-chave do SFU (novo assinante) e `set_bitrate` ligado à estimativa que o SFU devolver.
4. Com SFU, simulcast/SVC resolve "espectador lento sem derrubar os outros": publicar 2–3 camadas (ex.: 1080p, 540p, 270p) com 3 instâncias de `H264Enc` sobre a mesma captura.

Limites conhecidos: o vazamento de ~4 handles por criação do NVENC (evite recriar o codificador; hoje só abaixo de 70% do bitrate e no máx. a cada 4 s) e `ice_servers` vazio (ver `ice.rs`).
