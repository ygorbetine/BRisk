# Transmissão de tela: o que foi tirado do "método Netflix" (2026-11-14)

A Netflix codifica cada título com calma, várias vezes; a transmissão do Briks codifica ao vivo, em milissegundos. O que serve daqui
são as ideias: bitrate obedecido, cadência da fonte, recuperação de perda sem quadro-chave gigante e imagem em ritmo constante.
Tudo abaixo foi medido nesta máquina (Ryzen 7 3800X, RTX 5070, NVIDIA H.264 Encoder MFT) ou com o `netsim` e o codificador de
software (`mf-soft`). **Nada foi validado em chamada real.**

## O que entrou

| Mudança | Onde | Medido |
|---|---|---|
| Receptor não pula quadro no meio da cadeia (`DecodeFeed`) | `share.rs` | teste unitário; causa provável do "datamoshing" |
| Teto de QP adaptativo (`QpGovernor`), também no `mf-soft` | `share.rs`, `mfenc.rs`, `video.rs` | grão pesado, 3 Mbps pedidos: teto 28 = 67,4 Mbps; 34 = 3,2 Mbps. Nos testes de rede o emissor foi de ~5 para 30 qps |
| Reprodução suavizada (`Playout`, teto 100 ms) | `share.rs` | chegadas a 63/3 ms saem a ~33 ms |
| Áudio da transmissão começa com 100 ms guardados (era 60) | `audio.rs` | teste unitário |
| FEC por paridade XOR, fora da faixa (`fec.rs`) | `share.rs`, `sfu.rs`, `mesh.rs` | malha 8%: p95 283 → 155 ms, congelamentos 5 → 2 |
| Seguir a cadência da fonte (filme a 24 qps sai a 24) | `share.rs` (`Cadence`) | relógio simulado; no `rede.log` real a captura oferece 23–25 qps |
| `i420_to_nv12` vetorizado | `mfenc.rs` | 2580x1080: 2,26 → 0,23 ms por quadro |
| SFU: offer do emissor volta a anunciar NACK (regressão da 1.5.7) | `sfu.rs` | perda de 3% e 8% no envio: 0 → 30 qps, 11 → 0 congelamentos |

Variáveis de diagnóstico novas: `BRIKS_PLAYOUT_MAX_MS` (0 desliga a suavização), `BRIKS_FEC=0`, `BRIKS_SHARE_NO_FOLLOW`,
`BRIKS_ENC_INTRA_REFRESH`, `BRIKS_SFUREDE_LADO`, `BRIKS_SFUREDE_FEC`. O `rede.log` ganhou "teto de QP N", "seguindo a fonte a N",
"suavizacao N ms", "FEC refez N pacotes" e "descartados por atraso do decodificador N".

## Som e imagem

A imagem sai com 10 a 100 ms de suavização mais a decodificação (~6 ms) e a apresentação. O som da transmissão começava a tocar com
60 ms guardados, então podia ficar até ~60 ms adiantado, que é o lado em que o descompasso se nota mais cedo (referência usual:
~45 ms com o som adiantado, ~125 ms com o som atrasado). Com 100 ms fixos fica entre ~20 ms adiantado e ~70 ms atrasado. Acompanhar
o atraso variável da imagem exigiria esticar ou encolher o som em tempo real; não foi feito. Esses números são contas a partir dos
buffers, não uma medição de sincronismo.

## O que não entrou, e por quê

### B-quadros
O MFT da NVIDIA recusa `AVEncMPVDefaultBPictureCount` (`IsSupported` = falso, `SetValue` = E_INVALIDARG). Além disso cada B-quadro
soma pelo menos um quadro de latência. Encerrado.

### Intra refresh
O MFT diz que suporta `AVEncVideoGradualIntraRefresh` e aceita o valor, mas não aplica. Teste `video::qualidade::intra_refresh_recupera`
(720p, 3 Mbps, quadros 30–39 perdidos, sem quadro-chave): a luminância cai de 38 para 22,5 dB e continua em 22–23 dB 2 s depois, com o
intra refresh desligado e com os valores 1, 2 e 30. É o mesmo padrão do teto de QP ao vivo: aceito e não obedecido. Para ter intra
refresh seria preciso falar com o NVENC pelo SDK da NVIDIA em vez do Media Foundation. Mesmo funcionando, o receptor hoje descarta
tudo até o próximo quadro-chave depois de uma perda sem conserto (para não mostrar imagem corrompida); aproveitar o intra refresh
significaria mostrar a imagem quebrada por um ou dois segundos enquanto ela se refaz.

### Camadas temporais (SVC) e referência de longo prazo
O MFT aceita `AVEncVideoTemporalLayerCount` e `AVEncVideoLTRBufferControl`. Não foi verificado se aplica (ver acima: aceitar não é
aplicar). Só fariam diferença com o receptor ou o SFU descartando camadas, o que hoje não existe.

### Simulcast no SFU
O emissor publica uma codificação por tela. Simulcast pede duas ou três codificações simultâneas (NVENC dá conta, mas cada sessão
extra custa GPU e o MFT vaza ~4 handles por criação), `rid` na offer, e o `AddTrackRequest` com as camadas. O LiveKit escolheria a
camada por espectador. Não começado: é o item de maior custo desta lista e só ajuda quando há espectadores com redes muito diferentes
na mesma sala.

### BT.709
As duas pontas usam BT.601 de faixa limitada (CPU em `video.rs`, GPU em `gpuconv.rs`, receptor em `nv12_to_rgba`). É coerente, então
as cores estão certas; só não é o padrão de HD. Trocar exige que emissor e receptor mudem juntos: um receptor antigo mostraria as
cores de um emissor novo levemente erradas (e vice-versa). O caminho compatível é sinalizar a matriz no fluxo (VUI do SPS:
`colour_primaries` / `matrix_coefficients`) ou no `peer.state`/dados do SFU, e o receptor escolher a conversão por emissor. O ganho
visual é pequeno (desvio de alguns níveis em cores saturadas). Não feito.

### 4:4:4 e HDR
O H264 do MFT de hardware é 4:2:0 de 8 bits. Texto colorido fino perde croma (o teto medido da subamostragem está no teste
`rolagem_de_texto`). Não há caminho pelo Media Foundation.

### VBV maior e VBR
Filme a 2 Mbps: VBV de 125 / 500 / 1000 ms dá 36,7 / 37,0 / 37,0 dB com pico de 1 s de 2,87 / 4,12 / 5,06 Mbps; VBR com teto de
150% dá 37,2 dB gastando 3,04 Mbps. Ganho de no máximo 0,5 dB com rajada maior. Mantido como estava.

## O que falta validar

- Teste em 2 PCs, nos dois modos (SFU e P2P), olhando o `rede.log`.
- A perda no envio pelo SFU foi medida com `mf-soft`; falta com o NVENC.
- Sincronismo de som e imagem não foi medido, só calculado.
- A paridade do FEC (~13% a mais enquanto ligada) não entra na conta do estimador de banda, e no SFU vai para todos da sala.

## Falhas de teste que já existem na base (e456149)

- `share::enc_recovery_tests::hardware_encoder_that_always_fails_falls_back_to_software_and_the_stream_continues`: sempre, com a suíte
  inteira; passa sozinho.
- `tests_sfuipc::ipc_room_join_pure_sfu_end_to_end_with_remote_mute_and_kick` e
  `tests_sfurec::sfu_reconnects_after_server_restart_with_renewed_token`: sempre, no grupo com o LiveKit real.
- `tests_sfurede::sfu_network_loss_and_outage_matrix`: falhava já na fase sem perda (5,5 qps) por causa do estouro de bitrate do `mf-soft`.
- Ocasionais (2 em 20 rodadas na base): `camera_e2e::camera_flows_both_ways_next_to_a_screen_share_and_stops_cleanly`,
  `tests::two_simultaneous_shares_in_one_room`; `ipc::cfg_tests::realtime_falls_back_to_supabase_json_and_keeps_precedence` (3 em 20).

Ao comparar dois worktrees com o mesmo `CARGO_TARGET_DIR`, o cargo pode reaproveitar o binário do outro: use um diretório por worktree
ou dê `touch` nas fontes.
