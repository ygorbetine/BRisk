# Transmissão de tela: checklist de boas práticas de streaming em tempo real

Estado em 2026-11-10, branch `cui-transmissao`. Cada item traz o status, onde está o código e a evidência (teste, número ou
captura). Regras desta lista: **FEITO e MEDIDO** só quando há número reproduzível; **FEITO sem medir** quando o código existe mas
falta prova; **NÃO FEITO** quando não existe. Os números vêm desta máquina (16 threads, GPU NVIDIA) e variam entre execuções.

Como reproduzir (em `apps/desktop-cui/briks-core`, com `CMAKE_POLICY_VERSION_MINIMUM=3.5`):

```bash
cargo test --release -- --test-threads=3
```

```bash
BRIKS_ADAPT_NO_CPU=1 BRIKS_ADAPT_OUT=saida cargo test --release -- --ignored --nocapture adaptation_follows_the_network
```

```bash
cargo test --release -- --ignored --nocapture loss_is_repaired_by_retransmission
```

```bash
python ../scripts/grafico-adaptacao.py saida/adaptacao-nitidez.csv
```

Provas guardadas no repositório: `apps/desktop-cui/provas/transmissao/` (`adaptacao-nitidez.{csv,png}`,
`adaptacao-fluidez.{csv,png}`, `sobrecarga.csv`, `enviado-0.png`, `recebido-0.png`).

## Resumo

| # | Item | Status |
|---|---|---|
| 1 | Bitrate adaptativo conforme a rede | FEITO e MEDIDO, com ressalvas |
| 2 | Quadro-chave sob demanda (PLI/FIR) e intervalo máximo | FEITO e MEDIDO |
| 3a | Recuperação de perda por NACK/retransmissão | FEITO e MEDIDO |
| 3b | FEC de vídeo | NÃO FEITO |
| 3c | Buffer de reordenação/jitter no receptor | FEITO e MEDIDO (reordenação); sem suavização de jitter |
| 4 | Qualidade por tipo de conteúdo | FEITO e MEDIDO (escolha manual; sem detecção automática) |
| 5 | Codificação por hardware com reserva por software | FEITO e MEDIDO (reservas só em bancada) |
| 6 | Simulcast / camadas | NÃO FEITO |
| 7 | Latência medida | MEDIDO de captura a quadro pronto; vidro a vidro NÃO medido |
| 8 | Telemetria local no chip | FEITO, com captura; indicador de adaptação sem captura no app |
| 9 | Áudio do processo sincronizado com o vídeo | NÃO FEITO e NÃO MEDIDO |

## 1. Bitrate adaptativo — FEITO e MEDIDO, com ressalvas

O webrtc-rs 0.12 não tem controle de congestionamento (nem goog-cc, nem REMB; o TWCC só existe no lado receptor). O laço é próprio.

- **Código**: `briks-core/src/adapt.rs` (`Estimator`, `Controller`), `briks-core/src/share.rs` (laço de 1 s em `start_sender`),
  `briks-core/src/mesh.rs` (leitura de Receiver Reports e NACK por espectador).
- **Sinais**: perda real = pacotes pedidos por NACK pela primeira vez ÷ pacotes enviados (o Receiver Report sozinho esconde o que a
  retransmissão consertou); atraso = RTT acima da base recente + 80 ms.
- **Regra**: perda < 3% sobe (25%/s longe do teto, 8%/s perto), 3–10% segura, > 10% corta; atraso corta para 70% do que passou.
  Depois de um congestionamento a estimativa não passa da capacidade observada por 6 s e relaxa 15%/s.
- **Ajuste sem reconectar**: aumento ao vivo por `ICodecAPI` (`CODECAPI_AVEncCommonMeanBitRate`); queda acima de 20% recria o
  codificador, porque o NVENC leva cerca de 2 s para obedecer ao ajuste ao vivo (medido: continuava enviando ~2× o pedido).
- **Modos manuais** (Alto, Máximo, Personalizado) são teto; o automático decide abaixo deles.

**Evidência** — teste `adaptation_follows_the_network`: proxy UDP local no meio de todo o tráfego (`briks-core/src/netsim.rs`),
fonte sintética 1080p30 com teto de 12 Mbps, degraus de 12–18 s:

| Degrau da rede | Enviado (Nitidez) | Enviado (Fluidez) |
|---|---|---|
| 20 Mbps, perda 0% | 8,7 Mbps | 7,5 Mbps |
| 6 Mbps, perda 0% | 0,36 Mbps | 0,52 Mbps |
| 2 Mbps, perda 2% | 1,6 Mbps | 0,94 Mbps |
| 0,8 Mbps, perda 5% | 0,28 Mbps | 0,28 Mbps |
| volta a 6 Mbps | 1,4 Mbps (subindo) | 1,7 Mbps (subindo) |

Gráficos: `provas/transmissao/adaptacao-nitidez.png` e `adaptacao-fluidez.png`.

**Ressalvas**:
- Na queda de 20 para 6 Mbps o laço **desce demais** (vai a ~0,4 Mbps quando caberiam ~5) e leva 10–20 s para voltar.
- Há congelamento nas quedas bruscas: pior intervalo sem quadro novo de 5,0 s (Nitidez) e 4,8 s (Fluidez).
- O sinal de atraso depende da cadência dos relatórios RTCP (1–2 s). Com TWCC a reação seria mais rápida. **TWCC não é usado.**
- Numa malha com vários espectadores vale o pior deles (há um só codificador).
- Só foi provado com rede simulada na mesma máquina. Rede real, Wi-Fi e TURN não foram testados.

## 2. Quadro-chave sob demanda e intervalo máximo — FEITO e MEDIDO

- **Código**: receptor pede em `share.rs` (`run_remote_video`: ao começar a assistir, em perda sem conserto, em erro do
  decodificador, e a cada 2 s sem quadro); emissor atende em `mesh.rs` (PLI e FIR no RTCP do remetente) e `share.rs`.
- **Limites**: no máximo um quadro-chave por segundo (um a cada 2 s com a rede apertada); tamanho limitado por VBV de 1 s; GOP de
  segurança de 30 s no codificador do Media Foundation.
- **Evidência**: `late_joiner_resize_and_leave_keep_the_share_alive` (quem entra depois recebe o primeiro quadro);
  `screen_share_flows_end_to_end_in_loopback` (parar e retomar); coluna `plis_acum` dos CSVs.
- **Não coberto**: no codificador de reserva OpenH264 o intervalo de segurança de 30 s não é configurado (só quadro-chave a pedido).

## 3a. NACK e retransmissão — FEITO e MEDIDO

- **Código**: `share.rs` (`pacer_task`: histórico de 2048 pacotes, retransmissão com orçamento de 25% da taxa do pacer, no máximo
  uma vez a cada 150 ms por pacote e só para pacotes com menos de 600 ms); `mesh.rs` (gerador de NACK do webrtc-rs mantido; o
  respondedor embutido foi retirado porque retransmite sem limite e sem espaçamento; janela anti-replay do SRTP de 64 para 8192,
  pois retransmissões eram rejeitadas).
- **Evidência** — `loss_is_repaired_by_retransmission` (720p30 a 3 Mbps fixos, perda aleatória, 8 s por degrau):

| Perda injetada | Quadros por segundo no receptor (de 30) | Quadros perdidos | Pedidos de quadro-chave |
|---|---|---|---|
| 0% | 30,8 | 0 | 0 |
| 2% | 30,6 | 0 | 0 |
| 5% | 25,4 | 1 | 7 |
| 10% | 26,2 | 0 | 9 |

## 3b. FEC de vídeo — NÃO FEITO

Não há FlexFEC nem ULPFEC. A voz usa FEC interno do Opus (já existia). O áudio da transmissão não liga o FEC do Opus.

## 3c. Buffer no receptor — FEITO e MEDIDO para reordenação

- **Código**: `share.rs` (`JitterBuf`): ordena por número de sequência, espera 400 ms por pacote faltante (tempo de a
  retransmissão chegar), exige que o quadro comece num pacote de início, salta para o quadro-chave mais recente quando fica sem
  referência e recria o decodificador após descontinuidade.
- **Evidência**: teste unitário `reorders_waits_for_retransmission_and_falls_back_to_keyframe`; tabela do item 3a.
- **O que não há**: buffer de suavização de jitter. O quadro é exibido assim que decodifica (menor latência, movimento menos
  uniforme com jitter). O áudio tem buffer de 60 ms.

## 4. Qualidade por tipo de conteúdo — FEITO e MEDIDO

- **Código**: `adapt.rs` (`Content`, `ladder`); escolha na UI em `src/ui_share.c` ("Se a rede ou o PC apertar": Nitidez / Fluidez).
- **Política**: Nitidez cede FPS antes da resolução (30 → 20 → 15 → 10, depois a resolução); Fluidez cede resolução antes do FPS
  (piso de 30 FPS enquanto há resolução para cortar, depois 20 e 15).
- **Evidência**: teste unitário `detail_drops_fps_before_resolution_and_motion_the_opposite`; com a mesma rede de 2 Mbps o teste
  de adaptação mediu **Nitidez 643p a 10 qps × Fluidez 411p a 16 qps**.
- **Ressalva**: a escolha é manual (padrão Nitidez). Não há detecção automática de jogo/movimento.

## 5. Codificação por hardware com reserva — FEITO e MEDIDO

- **Código**: `briks-core/src/mfenc.rs` (`MfEnc`: MFT de hardware NVENC/Quick Sync/AMF, depois o codificador de software da
  Microsoft), `video.rs` (`H264Enc`, com OpenH264 como última reserva; `BRIKS_ENCODER=mf-soft|openh264` força).
- **Evidência** — `bench_convert_and_encode` (tempo por quadro, 1080p):

| Codificador | 1080p |
|---|---|
| NVIDIA H.264 Encoder MFT (GPU) | 5,2 ms |
| H264 Encoder MFT (software da Microsoft) | 5,9 ms |
| OpenH264 (software, sem assembly) | 51 ms |

- **CPU medida no app** (tela 3440x1440 mudando inteira, 1080p30): core do emissor ~175% de um núcleo; com a tela parada, 3–11%.
- **Ressalvas**: as reservas só foram medidas em bancada (codificar e decodificar), não de ponta a ponta. AMD e Intel não foram
  testados. A conversão de cor e a escala ainda são feitas na CPU; é o maior custo em telas grandes.

## 6. Simulcast / camadas — NÃO FEITO

Há um único fluxo por transmissão. Limite da malha P2P: o codificador roda uma vez, mas o envio é multiplicado por espectador
(5 espectadores a 4,5 Mbps = 22,5 Mbps de upload) e a qualidade de todos é a do pior espectador. Acima de 3–4 espectadores com
redes diferentes é preciso um SFU com simulcast ou SVC. Só foi testado com até 3 pessoas, em loopback.

## 7. Latência — MEDIDA de captura a quadro pronto

- **Método**: a fonte de teste carimba o relógio no próprio quadro (blocos preto/branco); o receptor lê o carimbo quando grava o
  quadro para a UI. Inclui conversão, codificação, rede, reordenação, decodificação e conversão de volta. **Não inclui** a
  latência da API de captura nem a exibição na tela: não é vidro a vidro.
- **Loopback sem limite**: p50 27–30 ms, p95 43–53 ms (`screen_share_flows_end_to_end_in_loopback`, 720p).
- **Com rede limitada** (p50 por degrau, 1080p): 50–64 ms livre; 46–83 ms a 6 Mbps; 79–168 ms a 2 Mbps com 2% de perda;
  102–127 ms a 0,8 Mbps com 5% de perda (`adaptation_follows_the_network`, colunas `latencia_p50_ms`).
- **Não medido**: vidro a vidro com câmera ou fotodiodo; rede real.

## 8. Telemetria no chip — FEITO

- **Código**: `share.rs` (evento `share.stats` a cada segundo: bitrate enviado e alvo, estimativa, FPS, resolução, perda, RTT,
  NACK, retransmissões, codificador, motivo da adaptação); `src/ui_share.c` (`quality_chip`, popover "Qualidade da transmissão").
- **Evidência**: captura do app real `chip-ao-vivo.png` (pasta de notas da sessão, fora do repositório porque mostra a tela do
  usuário): chip "1080p · 28 FPS · 2 Mbps" com valores atuais, e popover com "No ar", "Rede" e "Codificador".
- **Sem captura**: o ícone de "adaptando" (seta para baixo por rede, processador por CPU) existe no código, mas não foi capturado
  no app, porque no loopback do app a rede não aperta. O estado em si é provado pelos testes (`adaptando` nos CSVs).

## 9. Áudio sincronizado com o vídeo — NÃO FEITO e NÃO MEDIDO

O áudio do processo e o vídeo seguem em faixas separadas, sem sincronização por Sender Reports no receptor. O vídeo é exibido ao
decodificar; o áudio passa por um buffer de 60 ms e sai em mono. O desvio entre os dois não foi medido. O que existe medido é só
a chegada do áudio (`process_audio_reaches_the_viewer`: 100 quadros Opus, pico 0,335).

## Sobrecarga local ("a taxa de quadros cai") — FEITO e MEDIDO

- **Código**: `adapt.rs` (`note_cpu`): se a conversão + codificação por quadro passa de 90% do período, ou se a captura entrega e
  o envio não acompanha, desce um degrau a cada 2 s; volta um degrau a cada 3 s de folga.
- **Evidência** — `local_overload_lowers_fps_and_recovers` (codificador atrasado artificialmente em 60 ms por quadro):
  FPS alvo 30 → 10 durante a sobrecarga (67 ms por quadro), de volta a 30 cerca de 12 s depois. Série em `provas/transmissao/sobrecarga.csv`.

## Boas práticas que os testes obrigaram a acrescentar

- **Pacer**: pacotes espaçados a 2,5× o bitrate em vez de despejar o quadro inteiro (`pacer_task`).
- **Descarte antes de codificar** quando a fila do pacer passa de 0,3 s, para não quebrar a cadeia de referência.
- **Leitura do RTCP no receptor**: sem ela o webrtc-rs não processa os Sender Reports e o RTT do outro lado fica sempre 0.

## Não testado

Supabase real, TURN, rede real (Wi-Fi, 4G), mais de 3 pessoas, cliente Chrome/Electron do outro lado, máquina sem GPU de ponta a
ponta, AMD/Intel, jogo em tela cheia, HDR, Discord realmente tocando, e os cliques manuais na interface (só ganchos de teste).
