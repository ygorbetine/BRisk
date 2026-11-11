# Transmissão de tela: checklist de boas práticas de streaming em tempo real

Estado em 2026-11-10, branch `cui-transmissao`. Cada item traz o status, onde está o código e a evidência (teste, número ou
captura). **FEITO e MEDIDO** só quando há número reproduzível; **FEITO sem medir** quando o código existe mas falta prova;
**NÃO FEITO** quando não existe. Os números vêm desta máquina (16 threads, GPU NVIDIA) e variam entre execuções; a máquina
estava em uso por outras sessões durante as medições.

## Como reproduzir

Em `apps/desktop-cui/briks-core`, com `CMAKE_POLICY_VERSION_MINIMUM=3.5` no ambiente:

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
cargo test --release -- --ignored --nocapture survives_a_four_second_network_outage
```

```bash
BRIKS_ADAPT_NO_CPU=1 cargo test --release -- --ignored --nocapture slow_viewer_does_not_drag_the_others
```

```bash
cargo test --release -- --ignored --nocapture bench_gpu_vs_cpu_conversion
```

```bash
BRIKS_SOAK_MIN=30 BRIKS_ADAPT_OUT=saida cargo test --release -- --ignored --nocapture soak_no_leaks_no_latency_drift
```

Em `apps/desktop-cui` (abre janelas de teste na 2ª tela, sem foco, e não captura a tela principal):

```bash
bash scripts/teste-ui-real.sh scripts/ui-real-emissor.txt saida
```

```bash
bash scripts/teste-core-orfao.sh
```

Provas no repositório: `apps/desktop-cui/provas/transmissao/` (`adaptacao-*.{csv,png}`, `sobrecarga.csv`, `enviado-0.png`,
`recebido-0.png`). Capturas do app ficam fora do repositório, na pasta de notas da sessão (`transmissao/ui-real/`).

## Resumo

| # | Item da lista | Status |
|---|---|---|
| 1 | Adaptação automática de bitrate, FPS e resolução | FEITO e MEDIDO, com ressalvas (desce demais nas quedas bruscas) |
| 2 | Perda de pacotes: NACK, PLI/FIR, buffer, queda de rede | FEITO e MEDIDO; FEC de vídeo NÃO FEITO |
| 3 | Cliques reais na interface | FEITO e MEDIDO em parte (ver o que faltou) |
| 4 | Áudio: estéreo, A/V, volume, Discord, fora da árvore | FEITO e MEDIDO; Discord tocando NÃO TESTADO |
| 5 | Tela cheia exclusiva e DRM (captura preta) | NÃO FEITO |
| 6 | Conversão de cor na GPU, antes/depois | FEITO e MEDIDO (CPU); GPU e latência antes/depois NÃO MEDIDOS |
| 7 | Vários espectadores | FEITO e MEDIDO com 2 espectadores; 4+ NÃO TESTADO; sem simulcast |
| 8 | Robustez: órfão, entrar no meio, duas transmissões, reconexão | FEITO e MEDIDO; emissor que reconecta NÃO TESTADO |
| 9 | Codificadores de reserva | FEITO e MEDIDO de ponta a ponta; AMD/Intel NÃO TESTADOS |
| 10 | Latência vidro a vidro | MEDIDO sem o monitor (seção 13): p95 51–105 ms em 720p em rede boa; 1080p com ~120 ms a mais, sem causa conhecida |
| 11 | Privacidade | FEITO, verificado em parte |
| 12 | Revisão do código e teste longo | Teste longo: ver seção; revisão crítica NÃO FEITA |

## 1. Adaptação automática — FEITO e MEDIDO, com ressalvas

O webrtc-rs 0.12 não tem controle de congestionamento (nem goog-cc, nem REMB; o TWCC só existe no lado receptor). O laço é próprio.

- **Código**: `briks-core/src/adapt.rs` (`Estimator`, `Controller`, `NetShared`), `share.rs` (laço de 1 s em `start_sender`),
  `mesh.rs` (leitura de Receiver Reports e NACK por espectador).
- **Sinais**: perda real = pacotes pedidos por NACK pela primeira vez ÷ pacotes enviados; atraso = RTT acima da base + 80 ms.
- **Regra**: perda < 3% sobe (25%/s longe do teto, 8%/s perto), 3–10% segura, > 10% corta; atraso corta para 70% do que passou.
  Depois de um congestionamento a estimativa não passa da capacidade observada por 6 s e relaxa 15%/s.
- **Ajuste sem reconectar**: aumento ao vivo por `ICodecAPI` (`CODECAPI_AVEncCommonMeanBitRate`); queda acima de 20% recria o
  codificador, porque o NVENC leva cerca de 2 s para obedecer ao ajuste ao vivo (medido).
- **Política por conteúdo** (escolha manual na UI, padrão Nitidez): Nitidez cede FPS antes da resolução (piso de 10 FPS);
  Fluidez cede resolução antes do FPS. Não há detecção automática de jogo.
- **Modos manuais** (Alto, Máximo, Personalizado) são teto; o automático decide abaixo deles.
- **Sobrecarga local**: se converter + codificar passa de 90% do período, ou a captura entrega e o envio não acompanha, desce
  um degrau a cada 2 s; volta a cada 3 s de folga.

**Evidência** — `adaptation_follows_the_network`: proxy UDP local no meio de todo o tráfego (`netsim.rs`), fonte 1080p30 com
teto de 12 Mbps, degraus de 12–18 s. Gráficos em `provas/transmissao/adaptacao-{nitidez,fluidez}.png`.

| Degrau da rede | Enviado (Nitidez) | Enviado (Fluidez) |
|---|---|---|
| 20 Mbps, perda 0% | 8,7 Mbps | 7,5 Mbps |
| 6 Mbps, perda 0% | 0,36 Mbps | 0,52 Mbps |
| 2 Mbps, perda 2% | 1,6 Mbps | 0,94 Mbps |
| 0,8 Mbps, perda 5% | 0,28 Mbps | 0,28 Mbps |
| volta a 6 Mbps | 1,4 Mbps (subindo) | 1,7 Mbps (subindo) |

- Com a mesma rede de 2 Mbps: **Nitidez 643p a 10 qps × Fluidez 411p a 16 qps**.
- Sobrecarga local (`local_overload_lowers_fps_and_recovers`, codificador atrasado em 60 ms por quadro): FPS alvo 30 → 10 e de
  volta a 30 cerca de 12 s depois. Passou duas vezes sozinho e falhou uma com a máquina ocupada; roda isolado (`--ignored`).

**Ressalvas**:
- Na queda de 20 para 6 Mbps o laço **desce demais** (~0,4 Mbps quando caberiam ~5) e leva 10–20 s para voltar.
- Pior intervalo sem quadro novo nas quedas bruscas: 5,0 s (Nitidez) e 4,8 s (Fluidez).
- Uma tentativa de corrigir isso com retorno direto do espectador (taxa recebida e fila, 4x/s) **piorou** o teste (17 s de
  congelamento) e foi revertida (`10fd09f` → `6564ffa`). A causa não foi investigada.
- O sinal de atraso depende da cadência dos relatórios RTCP (1–2 s). **TWCC não é usado.**
- Só foi provado com rede simulada na mesma máquina.

## 2. Perda de pacotes — FEITO e MEDIDO; FEC de vídeo NÃO FEITO

- **Código**: `share.rs` — `pacer_task` (histórico de 2048 pacotes; retransmissão com orçamento de 25% da taxa, uma vez a cada
  150 ms por pacote e só para pacotes com menos de 600 ms), `JitterBuf` (ordena, espera 400 ms **por pacote faltante**, exige
  início de quadro, salta para o quadro-chave mais recente quando fica sem referência, recria o decodificador após
  descontinuidade), `run_remote_video` (PLI ao começar, em perda sem conserto, em erro do decodificador, a cada 2 s sem quadro).
  `mesh.rs` — gerador de NACK do webrtc-rs mantido, respondedor embutido retirado (retransmitia sem limite), janela anti-replay
  do SRTP de 64 para 8192 (as retransmissões eram rejeitadas), PLI e FIR atendidos.
- **Quadro-chave**: só a pedido; no máximo um por segundo (um a cada 2 s com a rede apertada); VBV de 1 s; GOP de segurança
  de 30 s no codificador do Media Foundation (não configurado no OpenH264).

**Evidência** — `loss_is_repaired_by_retransmission` (720p30 a 3 Mbps fixos, 8 s por degrau):

| Perda injetada | Quadros por segundo no receptor (de 30) | Quadros perdidos | Pedidos de quadro-chave |
|---|---|---|---|
| 0% | 30,8 | 0 | 0 |
| 2% | 30,6 | 0 | 0 |
| 5% | 25,4 | 1 | 7 |
| 10% | 26,2 | 0 | 9 |

Perda de 1% não foi medida em separado.

**Queda total de rede por 4 s** — `survives_a_four_second_network_outage`: antes 85 quadros em 4 s com latência p50 de 38 ms;
durante a queda 1 quadro; **primeiro quadro novo 1,2 s depois da volta**; latência p50 de 33 ms logo depois e 38 ms entre 6 e
9 s depois (sem acúmulo).

**O que não há**: FEC de vídeo (FlexFEC/ULPFEC) e buffer de suavização de jitter para vídeo (o quadro é exibido ao decodificar).
O áudio da transmissão liga o FEC interno do Opus e passa por um buffer de 60 ms.

## 3. Cliques reais na interface — FEITO e MEDIDO em parte

- **Como**: o modo roteiro do app injeta movimento e botão do mouse pelo mesmo caminho do mouse real (`src/script.c`, comandos
  `click`, `waitms`, `shot`, `signal`). O emissor e o espectador abrem na 2ª tela sem pegar o foco; a fonte transmitida é uma
  janela de teste (`scripts/janela-teste.ps1`).
- **Coberto, com captura**: abrir o seletor pelo botão Compartilhar; aba Janelas; escolher a janela; alternar o áudio (Sem áudio
  ↔ Da fonte); Ajustar (24 FPS, Fluidez); Transmitir; chip e popover ao vivo; **trocar a qualidade com a transmissão no ar**
  (15 FPS chegou ao espectador); redimensionar, minimizar, restaurar e fechar a janela transmitida (o chip do emissor mostra
  "A janela foi fechada" e o espectador volta a "Ninguém transmitindo").
- **Defeitos que o teste achou e que foram corrigidos**:
  - ids duplicados entre as pílulas da barra e as abas do cabeçalho em `ui_call.c`: clicar em Compartilhar também trocava para a
    aba Chat;
  - cliques dentro do seletor valiam também na barra por baixo dele;
  - o conversor da GPU não recriava as texturas quando a janela capturada mudava de tamanho.
- **NÃO FEITO por clique**: alternar apps na lista "Escolher apps", parar e retomar, mudança de DPI, múltiplos monitores
  (só a 2ª tela foi usada), janela minimizada como caso próprio (o espectador fica com o último quadro; não há aviso).
- **Limite conhecido**: `briks_clicked` ignora camadas modais no resto do app; só a barra de transmissão foi protegida.

## 4. Áudio — FEITO e MEDIDO; Discord tocando NÃO TESTADO

- **Código**: `procaudio.rs` (captura por processo, mixagem, Opus estéreo 128 kbps com FEC), `audio.rs` (buffers em estéreo;
  a faixa `peer#screen` é decodificada e tocada em estéreo; volume por espectador), `mesh.rs`/`ipc.rs` (`share.volume`).
- **Evidência** — `screen_audio_is_stereo_and_av_offset_is_measured` (bipe de 1 kHz só no canal esquerdo e vídeo com o relógio
  carimbado, presos ao mesmo relógio):
  - **Estéreo**: diferença entre canais de 0,55 no receptor (0 = mono).
  - **Sincronia A/V**: vídeo pronto 27 ms depois da captura; áudio 8 ms depois do bipe + 60 ms de buffer de reprodução →
    **o áudio sai cerca de 41 ms depois do vídeo**. Não há sincronização ativa por Sender Reports; o valor é o da soma dos caminhos.
  - **Volume do espectador**: pico 0,531 com volume 1 e 0,000 com volume 0.
- **Só o processo**: `captures_only_the_target_process` — alvo com pico 0,356; processo irmão mudo com 0,000.
- **Fora da árvore do processo**: "Da fonte" numa janela inclui as sessões de áudio do mesmo executável que vivem fora da árvore
  (`wanted` em `procaudio.rs`). Sem teste próprio.
- **Discord**: `lists_audio_sessions_and_marks_briks_and_discord_as_excluded` mostra a sessão real `Discord.exe` desta máquina
  marcada como excluída e fora da captura "tudo menos". **Não foi testado com o Discord emitindo som.**
- **Falta na UI**: o controle de volume do espectador existe no core e no IPC, mas não há controle na tela.

## 5. Tela cheia exclusiva e DRM — NÃO FEITO

Não há detecção de quadro preto nem orientação ao usuário. Não foi testado com jogo em tela cheia exclusiva nem com janela
protegida.

## 6. Conversão de cor na GPU — FEITO e MEDIDO (CPU)

- **Código**: `briks-core/src/gpuconv.rs` (processador de vídeo do Direct3D 11: escala + BGRA → NV12 no tamanho de envio; só o
  quadro reduzido volta para a memória). Reserva automática por CPU se a GPU falhar; `BRIKS_CONVERT=cpu` força o caminho antigo.
- **Cores**: `gpu_conversion_matches_the_cpu_one` — diferença de no máximo 1 nível em 8 cores.
- **Bancada** (`bench_gpu_vs_cpu_conversion`, textura sintética, sem capturar tela), tempo de CPU por quadro:

| Caso | CPU (antes) | GPU (depois) |
|---|---|---|
| 1080p | 7,3 ms | 2,3 ms |
| 1440p | 9,9 ms | 4,2 ms |
| ultrawide 3440x1440 → 1080p | 18,2 ms | 3,1 ms |
| ultrawide nativo | 14,8 ms | 4,7 ms |

- **Captura real da 2ª tela (1920x1080)**, emissor e receptor no mesmo processo (`real_screen_share_stats`), CPU do processo em
  % de um núcleo: 1080p30 **75–95% → 59–86%**; 1080p60 **148–194% → ~110%**.
- **NÃO MEDIDO**: uso de GPU antes/depois, latência antes/depois, e 1440p60/ultrawide com captura real (a única tela desses
  tamanhos é o monitor principal, que não é capturado nos testes).

## 7. Vários espectadores — FEITO e MEDIDO com 2

- **Código**: `adapt.rs` (`NetShared::majority`): se pelo menos metade dos espectadores está bem, quem está congestionado deixa
  de ditar o bitrate.
- **Evidência** — `slow_viewer_does_not_drag_the_others` (A transmite 720p; B em rede boa; C entra a 0,5 Mbps com 5% de perda):

| Situação | Enviado | Alvo | Espectador bom (B) | Espectador ruim (C) | CPU do processo |
|---|---|---|---|---|---|
| só B | 2868 kbps | 2550 kbps | 17,2 qps | — | 49% |
| B + C | 2611 kbps | 2550 kbps | 15,9 qps | 0 qps | 46% |

- **Limite da malha P2P**: há um único fluxo. O espectador ruim fica **sem vídeo** em vez de receber uma versão menor; o envio
  é multiplicado por espectador (5 espectadores a 4,5 Mbps = 22,5 Mbps de upload); as retransmissões pedidas por um vão para
  todos. Acima de 3–4 espectadores com redes diferentes é preciso um SFU com simulcast ou SVC.
- **NÃO TESTADO**: 4 ou mais espectadores.

## 8. Robustez — FEITO e MEDIDO

- **Core órfão**: o core vigia o processo pai e encerra quando ele morre (`watch_parent` em `main.rs`), além do Job Object em
  `src/core.c`. Prova: `scripts/teste-core-orfao.sh` — app morto à força, core some em menos de 2 s.
- **Espectador entrando no meio**: `late_joiner_resize_and_leave_keep_the_share_alive` (3 pessoas, mudança de tamanho da fonte,
  saída de um espectador). O receptor só monta quadro a partir de um pacote de início (entrar no meio de um quadro-chave
  travava o decodificador sem erro).
- **Duas transmissões na sala**: `two_simultaneous_shares_in_one_room` — A recebeu 90 quadros de B e B 65 de A em ~3 s; parar
  uma não para a outra. Para isso a renegociação deixou de colidir: só o lado iniciador faz ofertas e o outro pede por
  `share.reneg` (as ofertas cruzadas travavam a conexão).
- **Parar e retomar**: `screen_share_flows_end_to_end_in_loopback` (sem renegociar, mesma numeração RTP).
- **NÃO TESTADO**: emissor que cai e reconecta com a transmissão no ar.

## 9. Codificadores de reserva — FEITO e MEDIDO de ponta a ponta

`screen_share_flows_end_to_end_in_loopback` rodado com cada combinação (720p, fonte sintética):

| Configuração | Quadros em 4 s | Latência p50 |
|---|---|---|
| NVIDIA H.264 Encoder MFT (GPU) + decodificador Media Foundation | 85–86 | 27 ms |
| `BRIKS_ENCODER=mf-soft` (software da Microsoft) | 85 | 25–26 ms |
| `BRIKS_ENCODER=openh264` | 85 | 29–31 ms |
| `BRIKS_ENCODER=openh264 BRIKS_DECODER=openh264` | 85–86 | 28–29 ms |

Tempo de codificação em bancada, 1080p: GPU 5,2 ms; software da Microsoft 5,9 ms; OpenH264 51 ms (sem assembly).
**Não testado por falta de hardware**: AMD (AMF) e Intel (Quick Sync). Só H264; não há VP8/VP9/AV1.

## 10. Latência — captura → quadro pronto MEDIDA; vidro a vidro NÃO MEDIDA

- **Método**: a fonte de teste carimba o relógio no próprio quadro; o receptor lê o carimbo quando grava o quadro para a UI.
  Inclui conversão, codificação, rede, reordenação, decodificação e conversão de volta. **Não inclui** a latência da API de
  captura, a subida da textura nem a exibição no monitor.
- **Loopback**: p50 25–31 ms, p95 40–53 ms (720p).
- **Rede limitada** (p50 por degrau, 1080p): 50–64 ms livre; 46–83 ms a 6 Mbps; 79–168 ms a 2 Mbps com 2% de perda;
  102–127 ms a 0,8 Mbps com 5% de perda.
- A meta de p95 < 250 ms em rede boa é atendida **nesta medida parcial**; a medida vidro a vidro (câmera ou fotodiodo) não foi feita.

## 11. Privacidade — FEITO, verificado em parte

- **Nada de captura sem confirmação**: a captura só começa pelo botão "Transmitir" do seletor (`start_selected` em
  `src/ui_share.c`). Exceção de desenvolvimento: a variável de ambiente `BRIKS_DEV_SHARE` inicia sem diálogo. Abrir o seletor
  gera miniaturas das telas e janelas (ficam em `<dados>/thumbs`).
- **Indicador visível**: pílula verde "Parar transmissão" na barra, chip "Com áudio/Sem áudio" e "Transmitindo a tela" no tile
  (capturas `ui-real/b2-transmitindo.png`).
- **Parar ao fechar**: sair da sala fecha a malha e para a captura; o fim da conexão com a UI também; e o core encerra se o
  app morrer (item 8). Verificado para o app morto à força; os outros dois caminhos estão no código, sem teste próprio.

## 12. Revisão do código e teste longo

- **Teste longo** (`soak_no_leaks_no_latency_drift`, 30 min, vídeo + áudio sintéticos): ver o resultado no final deste arquivo.
- **Revisão crítica do código**: NÃO FEITA de forma sistemática. O que se sabe: os caminhos quentes usam `lock().unwrap()` em
  mutexes (um pânico em outra thread derrubaria a transmissão); as threads de captura por processo terminam por sinalizador,
  sem `join`; o histórico do pacer e o buffer do receptor têm tamanho limitado.

## Fora do alcance desta máquina — como testar

- **Supabase real com 2 PCs**: configurar `VITE_SUPABASE_URL` e `VITE_SUPABASE_ANON_KEY`, abrir o app nos dois, entrar na mesma
  sala, transmitir de um e assistir no outro; conferir o chip e o popover de qualidade dos dois lados.
- **TURN / NAT difícil**: hoje o core não recebe servidores ICE (`ice_servers` vazio em `ipc.rs`). Sem TURN, dois PCs atrás de
  NAT simétrico não conectam. É preciso configurar STUN/TURN antes de testar.
- **Outro cliente (Chrome/Electron)**: não testado. O perfil anunciado é H264 Constrained Baseline, pacote modo 1; o áudio da
  transmissão é uma segunda faixa Opus com id `screen-audio`.
- **AMD/Intel**: rodar `bench_convert_and_encode` e `screen_share_flows_end_to_end_in_loopback` e conferir o nome do codificador
  no popover.

## Resultado do teste longo

(preenchido ao fim da execução)

## 13. Rodada de rede ruim e latência vidro a vidro (itens 2 e 10) — 2026-11-11

Branch `cui-transmissao-rede` (a partir de `1e6eeba`). Tudo com fontes sintéticas e proxy UDP local, **sem GPU e sem captura de
tela**: `BRIKS_ENCODER=mf-soft BRIKS_CONVERT=cpu` (codificador por software da Microsoft, conversão por CPU). A máquina estava
dividida com outras sessões (as medições variam de uma execução para outra em dezenas de ms, principalmente em 1080p). Provas em
`apps/desktop-cui/provas/transmissao/`.

### Como repetir

Em `apps/desktop-cui/briks-core`, com `CMAKE_POLICY_VERSION_MINIMUM=3.5`:

```bash
BRIKS_ENCODER=mf-soft BRIKS_CONVERT=cpu BRIKS_PROVAS_OUT=../provas/transmissao cargo test --release -- --ignored --nocapture --test-threads=1 network_impairment_matrix
```

```bash
BRIKS_ENCODER=mf-soft BRIKS_CONVERT=cpu BRIKS_PROVAS_OUT=../provas/transmissao cargo test --release -- --ignored --nocapture --test-threads=1 glass_to_glass_latency
```

`BRIKS_G2G_CASE=<0..6>` roda só um caso da latência e `BRIKS_RN_DEBUG=1` imprime a telemetria do emissor a cada segundo.
O código está em `src/tests_rede.rs` e `src/netsim.rs` (o proxy ganhou **jitter** e **reordenação**: `jitter_ms`, `reorder`).

### FEITO e MEDIDO — perda, jitter e queda de rede (720p30, 3 Mbps fixos, 8 s por cenário)

Uma sessão, sem limite de banda, um espectador. "Congel." = intervalos entre quadros novos acima de 250 ms.
Antes = gerador de NACK padrão do webrtc-rs (a cada 100 ms, janela 8192); depois = a cada 20 ms, janela 2048 (`mesh.rs`).

| Cenário | qps antes → depois | p95 de latência antes → depois | Maior intervalo antes → depois | Congel. antes → depois |
|---|---|---|---|---|
| base (0%) | 30,1 → 30,0 | 64 → 64 ms | 61 → 62 ms | 0 → 0 |
| perda 1% | 29,9 → 30,2 | 171 → **98** ms | 253 → 157 ms | 1 → 0 |
| perda 2% | 29,6 → 29,9 | 190 → **103** ms | 284 → 283 ms | 1 → 1 |
| perda 5% | 28,4 → 29,4 | 317 → **243** ms | 458 → 336 ms | 4 → 2 |
| perda 10% | 13,9 → **28,0** | 769 → **483** ms | 2283 → **338** ms | 1 → 6 |
| jitter 0–30 ms em ordem | 30,1 → 30,0 | 101 → 100 ms | 66 → 64 ms | 0 → 0 |
| jitter 0–80 ms em ordem | 30,0 → 30,1 | 150 → 158 ms | 65 → 62 ms | 0 → 0 |
| perda 2% + jitter 0–40 ms | 29,4 → 30,2 | 374 → 298 ms | 292 → 184 ms | 2 → 0 |

- **Perda de 1%** (não medida antes): 30 qps, p95 de 98 ms; 76 pacotes perdidos de verdade, 77 retransmitidos, nenhum quadro
  perdido nem pedido de quadro-chave. Com o NACK a 100 ms o p95 era 171 ms: o buraco esperava até 100 ms só para ser pedido.
- **Perda de 10%**: com o NACK a 100 ms o receptor caía para 13,9 qps e ficou 2,3 s sem quadro novo; com 20 ms, 28 qps e
  no máximo 338 ms. Os congelamentos curtos (> 250 ms) subiram de 1 para 6 porque agora há vários de 250–340 ms no lugar de um
  de 2,3 s.
- **Queda total de rede de 3 s e de 5 s**: nenhum quadro durante a queda; **o primeiro quadro novo chegou 1,5 s depois da volta**
  nas duas (1483–1533 ms), 22,5 qps nos 6 s seguintes, 2–3 PLIs, **latência de volta ao normal** (p50 62 → 61 ms, 8–12 s depois;
  sem acúmulo). A queda de 4 s já tinha prova própria (`survives_a_four_second_network_outage`, 1,2 s).
- **Jitter em ordem** (um roteador que atrasa sem reordenar): não perde quadro nem pede retransmissão; a latência sobe o que o
  atraso adicionado soma (p50 98 ms com 0–30 ms; 144 ms com 0–80 ms). O receptor de vídeo **não tem buffer de suavização**: exibe
  quando decodifica, então o jitter vira latência e não congelamento.
- **Efeito do NACK a 20 ms com janela 8192 (descartado)**: depois de uma queda de rede o gerador repetia pedidos de milhares de
  pacotes velhos (545 mil a 1 milhão de números de sequência pedidos em 6 s, contra 112–209 mil com 100 ms). Por isso a janela foi
  para 2048 (cobre 1,5 s a 10 Mbps, acima dos 600 ms em que o emissor ainda retransmite): ~121 mil em 6 s, igual ao padrão antigo.
  Provas: `matriz-antes-nack100ms.csv`, `matriz-nack20ms.csv` (20 ms com janela 8192), `matriz-final-nack20ms-janela2048.csv`.
  `matriz-nack20ms-skip3-descartado.csv`: ignorar os 3 últimos pacotes no gerador (tolerância a reordenação) mudou pouco (dentro
  do ruído entre execuções) e **não foi adotado**.

### FEITO e MEDIDO — latência vidro a vidro (sem o monitor)

Método: o relógio é carimbado no quadro sintético no instante da captura; o receptor lê o carimbo quando grava o quadro RGBA para a
UI. Não inclui a latência da API de captura real, a subida da textura nem a exibição. 15 s de amostra por caso; nos casos de rede
limitada a adaptação fica ligada (como no app) e há 10 s de acomodação. Resultado em `latencia-vidro-a-vidro.csv` (uma execução;
em outras execuções os números mudaram dezenas de ms).

| Caso | p50 | p95 | p99 | qps | conversão+codificação | RTT |
|---|---|---|---|---|---|---|
| loopback 720p30 (sem proxy) | 44 ms | **51 ms** | 52 ms | 30,0 | 7,7 ms | 1 ms |
| proxy sem limite, 720p30 | 63 ms | 79 ms | 89 ms | 27,3 | 16,6 ms | 20 ms |
| rede boa: 15 ms/sentido, jitter 0–5 ms, 20 Mbps, 720p30 | 64 ms | **105 ms** | 108 ms | 27,1 | 8,8 ms | 62 ms |
| rede média: 40 ms/sentido, jitter 0–15 ms, 8 Mbps, 1% de perda | 148 ms | **256 ms** | 303 ms | 26,8 | 9,0 ms | 118 ms |
| rede ruim: 80 ms/sentido, jitter 0–40 ms, 4 Mbps, 2% de perda | 178 ms | 235 ms | 838 ms | 6,1 | 2,6 ms | 232 ms |
| loopback 1080p30 | 167 ms | 182 ms | 188 ms | 26,0 | 8,7 ms | 1 ms |
| rede boa 1080p30 | 138 ms | 428 ms | 595 ms | 8,8 | 69 ms | 60 ms |

- **Meta p95 < 250 ms em rede boa: atendida em 720p** (51 ms em loopback, 105 ms com RTT de 62 ms). Em rede média ficou no limite
  (256 ms; o p50 já é 148 ms porque a adaptação segura a taxa e a fila do limite de 8 Mbps contribui).
- **Onde o tempo vai (720p, loopback, p50 44 ms)**: conversão + codificação 7,7 ms (codificação 3,0); decodificação + conversão
  para RGBA 5,1 ms (bancada: BGRA→I420 1,6 ms, codificar 3,0 ms, decodificar+RGBA 5,1 ms; em 1080p: 1,8 / 5,6 / 6,6 ms); rede em
  loopback ≈ 0,5 ms. **Os ~30 ms restantes são captura→emissor, pacer, reordenação e espera do laço**: a fonte entrega a 60 qps e o
  laço do emissor pega o próximo quadro no seu período de 33 ms (média de 16 ms), mais a leitura do teste a cada 6 ms. Esse resto
  **não foi separado por etapa** (faltam carimbos intermediários no pacer e no buffer de recepção).
- Com rede de 15 ms por sentido: rede = RTT/2 ≈ 31 ms mais o jitter.
- **O codificador e o decodificador não guardam quadros**: a bancada mostra saída a cada entrada (0 sem saída em 130) em 720p e 1080p.

### O que a medição achou e NÃO está resolvido

1. **1080p tem cerca de 120 ms a mais que 720p, e a causa não foi encontrada.** Em loopback sem proxy: 720p p50 44–49 ms; 1080p p50
   153–167 ms (quatro execuções), com p95 de 182 ms (estável, então é atraso fixo, não oscilação). A conversão/codificação medida
   (8,7 ms) e a decodificação (6,6 ms) não explicam, e o codificador/decodificador não seguram quadros. Pistas: carimbos
   intermediários no emissor (fila do pacer), no `JitterBuf` e na entrega ao decodificador. Com a máquina ocupada o 1080p chegou a p95
   de 428 ms (conversão por CPU 69 ms): **o 1080p por software não foi validado**; o caminho GPU/NVENC **não foi testado nesta rodada**.
2. **A volta de uma queda de rede leva ~1,5 s e vem do receptor**: sem quadros, o PLI só é repetido a cada 2 s
   (`last_frame.elapsed() > 2 s` em `run_remote_video`, `share.rs`). Reduzir para ~0,5–0,7 s deve cortar o tempo médio de volta
   para menos de 1 s (hipótese, não testada). **Não alterado**: `share.rs` tem alterações não commitadas de outra sessão.
3. **Reordenação forte de pacotes** (jitter de 0–40 ms *reordenando* todos os pacotes): 22 qps, p50 de 300 ms e milhares de
   retransmissões desnecessárias (1930 retransmitidos com 0% de perda real). O gerador de NACK pede o que só chegou fora de ordem.
   Cenário extremo (a Internet real reordena muito menos), mas não tratado.
4. **A adaptação fica presa em ~150 kbps** numa rede de 4 Mbps com RTT de ~230 ms, jitter de 0–40 ms e 2% de perda aleatória: a
   estimativa cai no pico de partida (perda de 40% na 1ª subida, com o teto em 6 Mbps) e fica em 150 kbps, 360p a 10 qps, por mais de
   15 s com perda reportada de 0–3% (`BRIKS_G2G_CASE=5 BRIKS_RN_DEBUG=1`). Pertence ao item 1 (adaptação); `adapt.rs` **não foi alterado**.
5. **Sem FEC de vídeo.** A retransmissão cobre até ~5% com RTT baixo; com RTT alto (> 150 ms) a retransmissão chega depois do prazo
   de 400 ms do buffer e o custo vira quadro-chave. Só o caso 4 acima cobre perda com RTT alto.

### NÃO MEDIDO

- Latência vidro a vidro com monitor/câmera, captura real, GPU/NVENC, AMD/Intel, e a etapa de render da UI.
- Perda de 1% e jitter com a adaptação ligada (a matriz usa bitrate fixo para isolar o conserto de perda).
- Vários espectadores sob perda.
