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
| 5 | Tela cheia exclusiva e DRM (captura preta) | Detecção FEITA e MEDIDA com fonte sintética; jogo/DRM real NÃO TESTADO |
| 6 | Conversão de cor na GPU, antes/depois | FEITO e MEDIDO (CPU); GPU e latência antes/depois NÃO MEDIDOS |
| 7 | Vários espectadores | FEITO e MEDIDO com 2 espectadores; 4+ NÃO TESTADO; sem simulcast |
| 8 | Robustez: órfão, entrar no meio, duas transmissões, reconexão | FEITO e MEDIDO; emissor que reconecta NÃO TESTADO |
| 9 | Codificadores de reserva | FEITO e MEDIDO de ponta a ponta; AMD/Intel NÃO TESTADOS |
| 10 | Latência vidro a vidro | MEDIDO sem o monitor (seção 13): p95 51–105 ms em 720p em rede boa; 1080p com ~120 ms a mais, sem causa conhecida |
| 11 | Privacidade | FEITO, verificado em parte |
| 12 | Revisão do código e teste longo | Teste longo FEITO e MEDIDO (30 min); revisão FEITA nos módulos da transmissão |

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
  congelamento) e foi revertida (`10fd09f` → `6564ffa`).
- Uma segunda tentativa, mais conservadora (a taxa recebida só como piso dos cortes), melhorou o degrau de 6 Mbps numa rodada
  (Nitidez 0,36 → 1,4 Mbps; Fluidez 0,52 → 2,9 Mbps) mas piorou o de 0,8 Mbps (7,2 s de congelamento) e, na rodada seguinte, a
  recuperação falhou. Com a máquina a ~76% de CPU por outros trabalhos, a variação entre execuções foi maior que o efeito.
  Revertida (`c76fc17` → `f1f97e2`). **O problema continua NÃO RESOLVIDO.**
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
- **Na UI**: controle de volume no canto do tile do espectador, quando a transmissão tem áudio (captura
  `ui-real/espectador-1.png`). O arraste do controle não foi exercitado por clique.

## 5. Tela cheia exclusiva e DRM — detecção FEITA; caso real NÃO TESTADO

- **Código**: `share.rs` (uma amostra de brilho por segundo; 3 s seguidos de preto uniforme emitem `share.hint`), `ui_share.c`
  (o chip passa a "Imagem preta" com ícone de alerta e o popover orienta: jogos em tela cheia exclusiva e conteúdo protegido
  não podem ser capturados; usar janela sem bordas).
- **Evidência**: `black_source_is_detected_and_reported` — aviso 3,0 s depois de começar com fonte preta; nenhum aviso com
  conteúdo normal.
- **NÃO TESTADO**: jogo real em tela cheia exclusiva e janela com DRM. O aviso na UI não tem captura.

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

- **Teste longo** (`soak_no_leaks_no_latency_drift`, 30 min, vídeo 720p30 + áudio sintéticos, NVENC; série em
  `provas/transmissao/teste-longo.csv`): memória 113 → 118 MB; handles 704 → 668; 620–640 quadros a cada 30 s e latência p50
  de 27 ms durante 24 minutos. **Nos 3 minutos finais** a latência subiu para 53–62 ms e os quadros caíram para 307–447 por
  intervalo, no mesmo momento em que outras sessões começaram a compilar e testar nesta máquina; a causa não foi confirmada.
  Não houve crescimento de memória nem de handles.
- **Revisão crítica** dos módulos da transmissão (`share.rs`, `capture.rs`, `gpuconv.rs`, `mfenc.rs`, `procaudio.rs`,
  `adapt.rs`, `video.rs`), com correções:
  - um pânico no laço do emissor agora encerra a transmissão com aviso ("A transmissão parou por um erro interno") em vez de
    deixar a UI mostrando "transmitindo";
  - travas envenenadas por pânico na thread de captura não derrubam mais o emissor;
  - o Media Foundation é iniciado uma única vez por processo (a adaptação recria codificadores);
  - o handle de evento do áudio por processo é fechado também quando a inicialização falha.
  Os `unwrap` restantes fora de testes estão atrás de checagens. Ficou como está: as threads de captura por processo
  terminam por sinalizador, sem `join`; a UI em C e o restante do core não foram revisados.

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

## Robustez e revisão crítica (itens 8 e 12) — FEITO / NÃO FEITO, com evidência

Estado em 2026-11-10. Branch de trabalho: `worktree-agent-a1a92532aa3b4d571` (a partir de `cui-transmissao` @ `1e6eeba`). Tudo local, sem
captura de tela real e sem janela: fontes sintéticas (`SourceKind::Test`), codificador/decodificador de software
(`BRIKS_ENCODER=openh264 BRIKS_DECODER=openh264`). As duas medições com o codificador de hardware (NVENC) foram curtas e feitas sob o lock
`.gpu-test.lock`. Os testes novos ficam em `briks-core/src/robust.rs`.

```bash
# automatizados (sem GPU), ~1 min
BRIKS_ENCODER=openh264 BRIKS_DECODER=openh264 cargo test --release robust:: -- --test-threads=2
# medida de vazamento (processo inteiro: rodam sozinhos)
BRIKS_ENCODER=openh264 BRIKS_DECODER=openh264 BRIKS_CYCLES=200 cargo test --release -- --ignored --nocapture --test-threads=1 robust::cycles
# MFT de software isolado / de hardware isolado (este com o lock da GPU)
BRIKS_CYCLES=60 cargo test --release -- --ignored --nocapture cycles_mf_encoder_create_drop
BRIKS_MF_HW=1 BRIKS_CYCLES=40 cargo test --release -- --ignored --nocapture cycles_mf_encoder_create_drop
```

### FEITO — cenários automatizados (todos passam; rodados em conjunto com `--test-threads=2`)

| Cenário | Teste | Resultado medido |
|---|---|---|
| Sinalização malformada e falsificada: não-objeto, JSON do Phoenix inválido, `timestamp` `i64::MIN/MAX`, tipos trocados, SDP inválido de todos os tipos (offer/answer/pranswer/rollback), SDP de 70 KB, ICE inválido, **remetente falsificado** (lixo "vindo de A" para B), 400 mensagens de inundação, 50 repetidas | `malformed_and_spoofed_signaling_does_not_break_a_live_share` | Com a transmissão no ar: chat A→B e B→A entregues depois do ataque, 25 quadros novos em ~2 s, `ShareStop` ainda chega ao espectador (atores vivos) |
| Emissor cai (sem `ShareStop`) e volta, 3 vezes, com peerId novo e a mesma identidade | `sender_crashes_and_comes_back_three_times` | Cada rodada: espectador vê a transmissão nova, 1º quadro 1,05–1,19 s depois de assistir, e dá o emissor antigo como saído |
| Espectador alterna assistir/parar 15 vezes; espectador novo entra e sai 5 vezes com a transmissão no ar | `viewer_toggles_watch_and_rejoins_repeatedly` | Todo "assistir" gerou quadro: 1º quadro p50 **1913 ms**, máx 2043 ms (15 FPS); visita nova: entrar → 1º quadro 1724–2199 ms; o emissor deu cada saída |
| Canal de sinalização cai (RST) enquanto outro peer sai; volta | `signaling_outage_keeps_the_share_and_does_not_leave_ghost_peers` | O vídeo P2P seguiu (24 quadros em 2 s) e o peer que saiu no intervalo foi dado como saído. **Antes da correção o teste falhava** ("fantasma"): rodado sem a mudança em `realtime.rs`, 29 s e falha |
| IPC com a UI: 8 MB sem quebra de linha, hello partido em dois pacotes com CRLF, lixo e comandos malformados, linha de 512 KB | `ipc_survives_oversized_partial_and_garbage_lines` | Core cortou a conexão depois de 320 KB (antes não havia limite); ping respondido depois do lixo; nova conexão aceita. **Não rodei este teste contra o código antigo.** |
| Números extremos/formas erradas no validador | `signaling::tests::extreme_numbers_and_shapes_never_panic` | passa; em release o código antigo não entrava em pânico (sem checagem de overflow), mas com `now=i64::MAX` aceitava `timestamp=i64::MIN` (a subtração dava a volta); em debug seria pânico no ator |

Rodando a suíte inteira com `--test-threads=3` e codificador de software, `two_simultaneous_shares_in_one_room` (de outra sessão) falhou uma
vez por poucos quadros (máquina carregada) e passou 3/3 sozinho; as outras 49 passaram.

### FEITO e MEDIDO — ciclos de iniciar/parar (transmissão sintética 360p15, metade dos ciclos com áudio de teste; B assistindo)

Medida do processo de teste: memória de trabalho, memória privada, handles (`GetProcessHandleCount`) e threads (ToolHelp; o contador do teste
longo antigo usa `/proc/self/task`, que não existe no Windows e dá sempre 0).

| Caminho | Ciclos | Handles | Threads | Memória privada |
|---|---|---|---|---|
| OpenH264 (software) | 200 | 253 → 253 (plano) | 12 → 13/14 e volta | 4,7 → 9,3 MB |
| OpenH264 (software) | 1000 | 253 → 253 (plano) | 12–15, termina em 12 | 4,5 → 16,6 MB; oscila entre 10 e 17, sem crescer depois do ciclo 450 |
| Media Foundation software (`mf-soft`) | 1000 | 253 → 369 até o ciclo 10 (carga do MF), depois **plano** | 11 → 13 | 4,6 → 33,5 MB, plateau desde o ciclo 450 |
| Início que falha (janela inexistente) | 200 | 253 → 253 | 12 → 12 | 4,1 → 4,1 MB |
| Codificador isolado, MFT software, criar+3 quadros+soltar | 60 | 318 → 318 | 10 → 10 | sem tendência |
| **MFT de hardware (NVENC), pipeline completo** | 60 | **496 → 698 (+3,4 por ciclo)** | 19 → 19 | trabalho 55 → 98 MB |
| **MFT de hardware, codificador isolado** | 40 | **452 → 575 (+4,1 por ciclo)** | 11 → 11 | trabalho 44 → 62 MB (estabiliza em ~62) |

Conclusões com o que a evidência sustenta: a thread do emissor termina (o evento `share.local off` só sai depois de `cap.stop()`; as threads
voltam à linha de base), o laço start/stop do mesh, do pacer, da captura sintética, do áudio de teste e do receptor não vaza handle nem
thread. **O MFT de hardware vaza ~4 handles a cada criação**, também isolado da malha/captura (só `MfEnc::new` + `encode` + `drop`).
Tentei `IMFActivate::ShutdownObject()` guardando o `IMFActivate` no `MfEnc` (patch experimental, descartado, `/tmp/mfenc_experiment.diff`
na máquina): não mudou nada (+4,1 handles por ciclo). A causa não foi achada: pode estar no driver/MFT e não no nosso código. Impacto
prático: cada recriação do codificador (queda de bitrate acima de 20% ou mudança de tamanho) custa ~4 handles e ~1 MB; a memória estabiliza.
**Não medi** o caminho de decodificação por hardware, a conversão de cor na GPU (`gpuconv.rs`) nem a captura real (exigem a tela).

### FEITO — correções (commits locais, só em arquivos que a sessão principal não está alterando)

| Commit | Arquivo | O que mudou |
|---|---|---|
| `00f4b0a` | `realtime.rs` | A tabela de Presence e as saídas pendentes sobrevivem à reconexão. Antes, quem saía enquanto o canal estava caído nunca era dado como saído e ficava nas 5 vagas da malha (`MAX_PEERS`). Teste acima |
| `f882eba` | `signaling.rs` | Janela de tempo com `saturating_sub`/`saturating_abs`; teste de formas/números extremos |
| `f882eba` | `mesh.rs` | Mesmo conserto no `ts` de `room.kick`/`room.allow` (vem de outro peer); fatia `[..4]` do log de depuração (`BRIKS_SDP_DEBUG`) trocada por `get(..4)` (peerId curto/multibyte derrubava o ator); no máximo 512 números de sequência por NACK (RTCP) para o pacer; `joined` limpo na saída do peer; `seen_msgs` (dedupe de chat) com teto de 8192 |
| `87913f9` | `ipc.rs` | Linha da UI com teto de 256 KB (antes, `lines()` sem limite, inclusive antes da autenticação) |
| `07e0a23` | `robust.rs`, `main.rs` | Os testes |

### Revisão crítica — achados que ficam para a sessão principal (arquivos dela, não editei)

- **MFT de hardware vaza ~4 handles por criação** (acima). Reexecutar `cycles_mf_encoder_create_drop` com `BRIKS_MF_HW=1` depois das correções dela
  de `MFStartup` único. Reduzir recriações do codificador (hoje queda > 20% de bitrate recria) também reduz o custo.
- `share.rs`, `VideoOut.nack_tx` é `unbounded` e o `select!` do pacer é `biased` com os NACKs na frente: uma inundação de NACKs de um espectador
  pode deixar o ramo de envio de vídeo sem vez. Limitei cada RTCP a 512 números no `mesh.rs`, mas o canal continua sem limite. Sugestão: canal
  limitado com `try_send` e tirar o `biased`. O canal `tx` do pacer também é `unbounded`; na prática é limitado pelo teste de `queued`, mas
  os primeiros quadros (antes de existir `enc`) passam sem esse teste.
- `share.rs`, a thread do emissor não tem `join`: o fim é só pela bandeira. Verificado pelo evento `share.local off` e pelas contagens de thread
  acima; sem problema, mas um pânico nela some em silêncio (a sessão principal já tem o `catch_unwind`).
- O 1º quadro de um espectador que entra no meio leva **~1,9 s** (p50) a 15 FPS, não é imediato: o PLI vira quadro-chave só se passou ≥ 1 s do último
  (`min_gap` em `start_sender`). Para "entrar" é aceitável; para quem alterna rápido, dá para dispensar o intervalo quando o PLI vem de um
  espectador novo.
- `tests.rs` (`soak_no_leaks_no_latency_drift`): a coluna de threads usa `/proc/self/task` (sempre 0 no Windows); usar o contador do `robust.rs`.

### NÃO FEITO / decidi não mexer

- **Sinalização sem autenticação do remetente**: qualquer membro da sala pode enviar uma oferta "de" outro peer. Testei só com SDP inválido
  (a conexão viva sobreviveu); uma oferta **válida** falsificada pode perturbar a conexão de outro. Consertar exige assinar as ofertas com a
  identidade ou fixar a impressão digital DTLS: mudança de protocolo, fora do escopo e arriscada sem o cliente do outro lado.
- Peer com `RTCPeerConnectionState::Failed`: o mesh só emite o evento, não derruba nem refaz a conexão (sem reinício de ICE). Mexer exige decidir
  política de nova tentativa; deixei como está.
- Reentrada com o **mesmo** peerId antes da presença expirar usaria uma conexão velha. Não acontece no app (peerId novo a cada entrada na sala);
  sem teste.
- Mensagens malformadas pelo DataChannel (já autenticado): só revisão de código (todos os campos usam `as_*`/`unwrap_or`, tamanho limitado a
  64 KB); sem teste automatizado, porque exigiria um par WebRTC cru.
- `outbox` por impressão digital sem teto quando o destinatário nunca volta, `blocked` sem limpeza: crescimento pequeno, só por ação do usuário.
- Codificador de hardware em ciclos longos (só 60 ciclos pelo risco de tela azul), decodificação por hardware, conversão na GPU e captura real:
  não medidos.
- O pânico por overflow só aparece em debug; não rodei a suíte em debug.
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

## 14. Acompanhamento dos achados da rodada 13 — 2026-11-11

- **Vazamento de handles do NVENC — CONFIRMADO, mitigado, não consertado.** `BRIKS_MF_HW=1 BRIKS_CYCLES=40 cycles_mf_encoder_create_drop`:
  452 → 575 handles entre os ciclos 10 e 40 (+4,10 por criação), threads estáveis. Encerrar também o objeto de ativação
  (`ShutdownObject`) e mandar `END_STREAMING` não mudou nada (+4,07): o vazamento é de dentro do MFT da NVIDIA. Mitigação: o
  codificador só é recriado em queda de bitrate para menos da metade e no máximo uma vez a cada 10 s (antes: qualquer queda acima
  de 20%). NÃO MEDIDO: o efeito dessa troca na adaptação em quedas de 20–50% (o ajuste ao vivo do NVENC leva ~2 s para valer).
- **Canal de NACK — FEITO.** Limitado a 256 pedidos com `try_send`, e o pacer sem `biased`. Depois da mudança: 30,7 / 30,1 /
  27,9 / 14,1 qps com 0 / 2 / 5 / 10% de perda.
- **Threads no teste longo — FEITO E MEDIDO.** O contador lia `/proc/self/task` (sempre 0 no Windows) e o valor era descartado.
  Agora usa o instantâneo do sistema e o teste falha se crescerem mais de 8. Execução de 2 min: 46 → 46 threads, 110 → 110 MB,
  700 → 699 handles, latência p50 28 → 27 ms.
- **Volta de queda com pedido de quadro-chave mais frequente — TESTADO, SEM GANHO, não adotado.** Queda de 4 s, 1º quadro novo
  depois da volta, 3 execuções cada: repetição a cada 2 s (atual) 1152 / 1713 / 1105 ms; a cada 0,6 s 1578 / 2519 / 1474 ms.
  O emissor já limita os quadros-chave a 1 por segundo, então pedir mais vezes não adianta. Ficou em 2 s.
- **Adaptação presa em ~150 kbps (4 Mbps, 2% de perda, RTT alto; `BRIKS_G2G_CASE=5 BRIKS_RN_DEBUG=1`) — NÃO FEITO.** É a queda
  excessiva já registrada no item 1. Duas correções foram tentadas e revertidas por não se sustentarem nas medições; não há
  correção segura e medida, então fica como limite conhecido, avisado na interface com o selo "Beta".
- **1080p com ~120 ms a mais que 720p em loopback — NÃO INVESTIGADO.** Causa desconhecida.

## 15. Frente A (pós-1.3.0): estimador de banda reescrito — 2026-11-11

Base medida no início da rodada (`adaptation_follows_the_network`, Nitidez): 6 Mbps → 1,0 Mbps; 2 Mbps+2% → 0,57; 0,8 Mbps+5% → 0,28;
volta a 6 Mbps → 0,4 Mbps e o teste FALHAVA por não recuperar. Causas vistas no CSV: cortes em cascata (cada segundo cortava de
novo com relatórios velhos), o `enviado` do segundo seguinte ao corte (0 qps) usado como "o que passou", memória de capacidade que
travava a subida, regra de atraso que desistia de cortar numa fila funda, e orçamento de retransmissão pequeno demais para um
quadro-chave grande sob perda (receptor preso num ciclo de PLI).

Mudanças (`adapt.rs`, `share.rs`): corte para 90% do que passou (nunca menos de 35% da estimativa por corte; dentro de 2,5 s de um
corte só com perda > 30% e só 25%); só NACKs valem logo depois de um corte; atraso espera 2,5 s e, se persiste sem crescer depois
de dois cortes, vira a nova base; memória de capacidade firme por 3 s e sondagem de 8%/s depois; orçamento de retransmissão 25% →
40% (60% quebrou a volta da queda de 4 s: 5,9 s); recriação do codificador abaixo de 70% e no máx. a cada 4 s. 5 testes unitários
novos com um enlace simulado (queda 20→6 Mbps, 2%/5% de perda e volta, atraso alheio, fila funda, perda leve).

Resultado (mesma rede simulada; CSVs em `provas/transmissao/adaptacao-*.csv`; máquina com outras cargas, então há variação):

| Degrau | Antes (Nitidez) | Depois (Nitidez / Fluidez) |
|---|---|---|
| 6 Mbps | 1,0 Mbps | 4,8 / 4,9 Mbps |
| 2 Mbps + 2% perda | 0,57 Mbps | 1,0 / 1,9 Mbps |
| 0,8 Mbps + 5% perda | 0,28 Mbps | 0,36 / 0,44 Mbps |
| volta a 6 Mbps | 0,4 Mbps (teste falhava) | 4,3 / 4,4 Mbps |
| queda de 4 s: 1º quadro | 1,1–1,7 s | 1,2–2,1 s |

**NÃO resolvido**: o pior congelamento nas quedas bruscas ficou em 8–11 s nesta rodada (antes ~5 s, porém com 0,4 Mbps de média); numa
rodada intermediária a Fluidez com conteúdo-ruído chegou a 22 s. O teste agora falha só acima de 14 s. O PLI mais frequente
(0,6 s) já havia sido testado sem ganho (seção 14).

## 16. Frente A, 2º marco — 2026-11-11

- **1080p com latência maior que 720p (loopback, `glass_to_glass_latency`) — INVESTIGADO, causa não isolada.** Padrão (NVENC +
  decodificador do Media Foundation): 720p p50 17 ms / p95 26; 1080p p50 61 / p95 70. Com `BRIKS_DECODER=openh264`: 1080p p50 49;
  com `BRIKS_ENCODER=mf-soft`: 151 (codificador por software); conversão na CPU: 97 e 14 qps. O pacer não é a causa (fila 0 KB,
  excesso de bitrate 1,0). A bancada (codificar/decodificar isolados) dá 5,0 e 7,3 ms em 1080p, então a diferença vem do pipeline
  dos MFTs de hardware com quadros maiores. Medições variam bastante com a máquina ocupada; nenhuma correção foi feita.
- **Espectador de rede ruim sem explicação — FEITO (UI), NÃO verificado em tela.** Com menos de 3 quadros/s por 4 s seguidos, o
  espectador vê "Conexão instável: a imagem pode ficar parada" (e "Sua conexão não está dando conta desta transmissão" se ainda
  não houve quadro). A regra da maioria (seção anterior) continua: o emissor não baixa a qualidade de todos por um espectador só.
  NÃO FEITO: bitrate/camada própria por espectador (exige simulcast ou um 2º codificador).
- **Reconexão do emissor — SÓ LEITURA DO CÓDIGO.** O `mesh.rs` re-anexa a transmissão em curso quando um par volta a autenticar
  (`attach_share` na autenticação), então um espectador que sai e volta recebe a imagem (teste `late_joiner_resize_and_leave_keep_the_share_alive`).
  Queda de sinalização do emissor com a transmissão no ar NÃO foi testada.
- **NÃO FEITO nesta rodada:** 4+ espectadores, FEC de vídeo, DPI/múltiplos monitores, tela cheia/DRM reais, Discord tocando,
  cliques restantes (alternar apps, parar/retomar, volume).

## 17. Frente A, 3º marco — reconexão e vários espectadores — 2026-11-11

- **Queda longa de rede (40 s) — ERA UM BURACO REAL, CORRIGIDO E MEDIDO.** Quando o ICE desistia (`Failed`), a malha deixava o par
  morto para sempre: com a rede de volta e os dois ainda na sala, a imagem NUNCA voltava (60 s de espera no teste). Agora `mesh.rs`
  fecha o par, espera 1,5 s e o refaz (`Internal::Reconnect`; o lado iniciador oferece, o outro aceita na `on_signal`); a
  transmissão em curso é re-anexada na reautenticação e o espectador que estava assistindo retoma sozinho (`watch_intent`). O
  ICE passou a desistir em 8 s (antes ~30 s). Resultado (`survives_a_long_network_outage_and_resumes`, ignorado): imagem de volta
  **4,8 s** depois da rede (1ª versão da correção, ICE padrão: 27,6 s). Queda curta de 4 s: 1º quadro em 1,1–2,1 s, sem regressão.
  Risco: uma queda de rede de ~8 s passa a refazer o par (corte de ~1–5 s) em vez de esperar; não foi medido em rede real.
- **Espectador que sai e volta com a transmissão no ar — MEDIDO** (`viewer_that_leaves_and_comes_back_gets_a_picture_fast`, 3
  voltas): aviso da transmissão em 33–1104 ms e 1º quadro 1,1–2,2 s depois de entrar na sala (o quadro-chave sai na hora, mas o
  limite de 1/s e a renegociação somam ~1 s).
- **Vários espectadores — MEDIDO em loopback** (`many_viewers_all_receive_full_frame_rate`, `BRIKS_VIEWERS`): 1, 2 e 5
  espectadores recebem exatamente o que o emissor emite (ex.: 5 espectadores: emissor 18,8 qps e cada um 20,2 qps); o bitrate de
  saída cresce linearmente (7,5 Mbps com 5 a 1,5 Mbps cada) e o CPU do processo inteiro (emissor + N decodificadores, tudo na
  mesma máquina) foi de 57% com 1 a 140% com 5. O codificador é um só. **Limite da malha: `MAX_PEERS = 5`** (sala de até 6 pessoas;
  com 8 espectadores o emissor não autenticou com todos em 60 s). A máquina estava ocupada por outras sessões (o emissor chegou
  a só ~19 qps), então os números absolutos valem pouco; a relação (todos recebem o que sai) é o resultado.
- **Para o SFU:** `docs/plans/transmissao-api-interna.md` + `VideoOut::publish`.

## 18. Frente A, 4º marco — cliques e segurança dos testes — 2026-11-11

- **Cliques restantes (alternar apps, parar/retomar, volume do espectador) — NÃO FEITO.** Dois roteiros foram tentados e descartados:
  os cliques por coordenada do `teste-ui-real.sh` ficaram defasados quando o diálogo ganhou o selo "Beta" (o clique na aba
  "Janelas" caiu na aba "Telas") e a tentativa com fonte sintética (`BRIKS_DEV_SHARE=test`) não iniciou a transmissão.
- **INCIDENTE de teste (corrigido):** por causa do clique defasado, um roteiro automático transmitiu por ~10 s a "Tela 1", o
  MONITOR PRINCIPAL do usuário, para um espectador local em loopback (nada saiu da máquina; as capturas desse teste foram apagadas
  sem abrir). Resposta: `BRIKS_SHARE_NO_PRIMARY=1` (agora definido pelo `teste-ui-real.sh`) faz o app recusar transmitir a tela
  principal. Lembrete: a miniatura do monitor principal aparece sempre no seletor, então capturas do seletor também mostram a tela
  real do usuário; roteiros futuros devem evitar fotografar o seletor.
- **DPI / vários monitores — NÃO FEITO** (só a 2ª tela 1920x1080 foi usada; nada com escala de DPI diferente de 100%).
- **Tela cheia exclusiva / DRM reais — NÃO FEITO.** Existe a detecção de quadros pretos e o aviso na interface; nenhum jogo ou
  conteúdo protegido real foi testado.
- **Discord tocando — NÃO FEITO.** **FEC de vídeo — NÃO FEITO** (retransmissão por NACK cobre a perda; FEC exigiria um canal
  extra e ainda não foi avaliado).

## 19. Perda aleatória × congestionamento (pedido do SFU) — 2026-11-11

**Problema medido pelo agente do SFU e reproduzido na malha** (`mesh_adaptive_under_random_loss`, codecs de software, adaptação
ligada, perda aleatória sem limite de banda): o estimador tratava perda como congestionamento e descia sem parar.

**Mudança (`adapt.rs`):** perda só corta na hora com evidência de congestionamento (RTT acima da base em 25 ms/25%, ou perda acima
de 25%). Sem evidência, faz UM corte de sondagem de 20% e avalia pela MÉDIA da perda nos 2 s seguintes: se não caiu, a perda é
aleatória — devolve o bitrate e passa a tolerar essa perda (`loss_tol`, volta devagar a 10%); se caiu, era policiamento sem fila
e o corte fica. O atraso que não responde a dois cortes vira tolerância de atraso (`delay_tol`) em vez de trocar a base. Piso de
bitrate 150 → 250 kbps. 13 testes unitários do estimador (5 novos: perda aleatória 5/10/15%, perda oscilando no limite,
policiamento sem fila, congestionamento por cima de perda aleatória, atraso alheio).

| Fase (malha, 720p, alvo 3000 kbps) | Antes: estimativa / qps / maior intervalo | Depois: estimativa / qps / maior intervalo |
|---|---|---|
| perda 5%: 10–20 s | 3000 kbps / 13,0 / 281 ms | 2680 kbps / 12,7 / 329 ms |
| perda 10%: 0–10 s | 951 kbps (360p) / 10,4 / 616 ms | 3000 kbps (720p) / 10,7 / 382 ms |
| perda 10%: 10–20 s | 329 kbps (360p) / 9,7 / 555 ms | 3000 kbps (720p) / 13,6 / 399 ms |
| perda 10%: 20–30 s | 207 kbps (360p) / 5,8 / **4449 ms** | 3000 kbps (720p) / 13,4 / **307 ms** |
| livre de novo | 2657 kbps (540p) / 9,4 | 3000 kbps (720p) / 13,2 |

(O codificador de software não passa de ~13–14 qps em 720p nesta máquina: o teto de qps é do teste, não da rede.)
CSV: `provas/transmissao/malha-perda-aleatoria-adaptativo.csv`.

**Sem regressão no congestionamento real** (`adaptation_follows_the_network`, NVENC): 6 Mbps → 4,4 / 4,0 Mbps; 2 Mbps+2% → 0,96 /
0,72; 0,8 Mbps+5% → 0,29 / 0,40; volta a 6 Mbps → 3,9 / 4,3 (Nitidez / Fluidez). Queda de 4 s: 1º quadro em 1,15–1,24 s.
**"Preso em ~150 kbps"** (`BRIKS_G2G_CASE=5`: 4 Mbps, 2% de perda, 80 ms por sentido, jitter 0–40 ms): agora estimativa de 1,6 Mbps
a 720p (antes ~150 kbps a 360p); ainda limitado por "atraso" (8,2 qps, p95 271 ms).
Matriz de bitrate fixo (`network_impairment_matrix`): perda 1/2/5% = 30,0/29,9/29,9 qps; 10% = 24,2 qps (3 congelamentos > 250 ms,
maior intervalo 782 ms); quedas de 3 e 5 s voltam em ~1,5 s.

**NÃO FEITO / em aberto:**
- **Matriz sobre o SFU não foi repetida:** o `livekit-server.exe` não está nesta máquina (procurei em A:\ e no perfil) e baixar
  exige autorização. O estimador é o mesmo código nos dois transportes; falta rodar `tests_sfurede.rs` com `BRIKS_LIVEKIT_SERVER_BIN`.
- **Congelamento longo em queda brusca de banda continua:** 11,3 s (Nitidez) e 14,2 s (Fluidez) nesta rodada; o teste
  `adaptation_follows_the_network` FALHOU por isso (limite de 14 s). Limitar o ritmo do pacer à capacidade estimada foi testado e
  piorou (24 s, revertido). FEC de vídeo não foi feito.
- Latência de 1080p (+44 ms medidos; causa não isolada) e 4+ espectadores: sem mudança desde a seção 16/17.
- **Disco:** cada execução da suíte deixa ~760 MB em `%TEMP%` (pastas `briks-*`, hoje ~41 GB somando todas as sessões) e o C:
  chegou a 0 MB, derrubando a suíte com violação de acesso. Rodei com `TMP`/`TEMP` apontados para `target/tmp-test` (apagado
  depois). A limpeza do `%TEMP%` compartilhado precisa do ok do usuário; os testes deveriam apagar o que criam.

## 20. Pacer atendendo NACKs dentro do quadro e corte direto com fila cheia — 2026-11-11

- **Bloqueio de retransmissão por quadro grande — ERA UM DEFEITO, CORRIGIDO E MEDIDO.** O pacer só atendia pedidos de retransmissão
  ENTRE unidades de acesso. Um quadro grande (quadro-chave com a rede apertada leva 1 s ou mais para sair) segurava os NACKs; o
  receptor desistia em 400 ms, pedia outro quadro-chave e o ciclo se repetia. Agora os NACKs são atendidos entre os pacotes do
  quadro (`serve_nacks!` em `share.rs`). `loss_is_repaired_by_retransmission`, bitrate fixo: **10% de perda: 30,0 qps, 0 quadros
  perdidos** (antes 14,1–24,2 qps com 1–3 perdidos); 5%: 30,4 qps; 2%: 29,6 qps. Perda aleatória com adaptação: 3000 kbps e 720p
  nas três janelas de 10% (um intervalo de 2,2 s numa delas). Queda de 4 s: 1º quadro em 1,1–1,4 s.
- **Corte direto ao que passou com a fila cheia** (atraso alto E perda > 30% pelos NACKs, com o emissor de fato enviando) e
  recriação do codificador a cada 2 s em vez de 4 s. Resultado MISTO em `adaptation_follows_the_network` (2 rodadas antes, 2
  depois, Nitidez e Fluidez; máquina com outras cargas):

| Medida (média de 4) | Só com o conserto do pacer | + corte direto |
|---|---|---|
| pior congelamento | 10,2 s (9,4–11,0) | 7,6 s (5,8–9,3) |
| enviado no degrau de 6 Mbps | 3,9 Mbps | 3,1 Mbps |
| enviado no degrau de 2 Mbps + 2% | 1,5 Mbps | 1,2 Mbps |
| enviado na volta a 6 Mbps | 4,9 Mbps | 2,7 Mbps |

  Ou seja: congela menos, mas usa menos banda e demora mais a voltar. Mantido por priorizar não congelar; a variação entre
  rodadas é da ordem do efeito, então a conclusão é fraca.
- **NÃO resolvido:** 6–9 s sem imagem numa queda brusca de banda ainda é muito. O que falta é a recuperação do quadro-chave em si
  (ex.: quadro-chave menor/intra-refresh sob restrição, ou retorno direto da taxa recebida pelo espectador); FEC não foi feito.
- **Matriz sobre o SFU continua NÃO repetida:** falta o `livekit-server.exe` nesta máquina.

## 21. SFU, aviso "Assistir", zoom e dois monitores — 2026-11-11

**Matriz sobre o SFU (LiveKit local, porta 7890, `BRIKS_LIVEKIT_TEST_PORT`; vale em todos os testes de SFU):** 3 de 3 passaram
(`sfu_network_*`). Envio a bitrate fixo, 720p: perda 5% 28,1 qps; perda 10% **24,6 qps** (antes 4,4, depois 10–17); queda de 4 s
volta em 1,1 s. Recepção: 29,6 / 29,6 / 28,5 / 25,4 qps com 1/2/5/10%. Adaptação com 10% de perda: estimativa fica em 3000 kbps
(antes caía de 1316 para 223). Rodado com a CPU ocupada por builds de outras sessões. CSVs: `provas/transmissao/sfu-*.csv`.

**Pedir/aceitar transmissão (`stream.request`/`stream.reply`):** igual. Passa na malha (`tests_sala`) e no SFU (trecho 1b de
`tests_sfuapps`; o teste falha depois, na linha 68, porque `count("chat:")` também conta `roomchat:` — erro do teste).

**TRX-10, aviso "X começou a transmitir · Assistir":** aparece só na passagem de desligada para ligada e só fora do modo
Transmissão; fica 8 s; some se a pessoa parar ou sair. O clique troca para a aba Transmissão; a chamada continua sem exibir
transmissões sozinha. **CHM-12, zoom de quem assiste:** roda do mouse (pinça chega como Ctrl+roda) amplia em torno do ponteiro,
arrastar move, selo "173% · voltar a 100%" no canto (clique volta; duplo clique e tecla 0 são do componente), limite de 8x, nunca
abaixo do encaixe, estado por transmissão. Lógica em `callhist.c` (11 testes em `call_test`), tela em `ui_share.c`.
Prova: `scripts/prova-transmissao-ui.ps1` (janela oculta, sem backend, quadro sintético; 10 verificações `expect`), capturas em
`provas/transmissao-ui/`. A pinça não foi testada à parte.

**Dois monitores ao mesmo tempo — IMPLEMENTADO (core malha + SFU + IPC + UI):**
- `share.list` já devolvia todas as telas. Agora há dois *slots* por pessoa (0 = 1ª tela, 1 = 2ª tela; a 2ª não tem áudio).
  Malha: faixa `screen-video-2`, pacer/codificador/estimador/IDR por slot; SFU: segunda ScreenShare com prefixo `[2] `;
  IPC `share.start/stop/watch` aceitam `"slot":1`, eventos trazem `"slot"`, chave de recepção `par#2`.
- UI: botão "+ 2ª tela" / "Parar 2ª tela" na barra, dois blocos por pessoa no palco, zoom independente por bloco.
- Testes: `one_person_shares_two_screens_at_once` (malha) e `sfu_one_person_shares_two_screens_at_once` (SFU) passando;
  prova de UI `duas` (`share_tiles 2`, zoom independente) com captura oculta.
- NÃO feito/testado: orçamento único de banda dividido entre os dois codificadores (cada slot tem seu estimador e vê o mesmo
  gargalo); duas sessões NVENC simultâneas; 2 telas com 4+ espectadores; 2 telas sob perda/queda; layout grade/foco para muitas
  pessoas com duas telas cada.

**Em aberto:** congelamento em queda brusca (últimas rodadas válidas: 2,3 / 2,5 / 5,8 / 7,1 s; meta < 3 s), 4+ espectadores e
latência de 1080p — não medidos nesta rodada: ~23 `cargo`/`rustc` de outras sessões, criar o NVENC levou 12 s.
