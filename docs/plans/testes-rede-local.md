# Testes só em loopback (sem aviso do Firewall do Windows)

**Para todos os agentes:** cada exe novo que escuta em 0.0.0.0 ou em IP de rede faz o Windows perguntar "permitir acesso?". Os testes
automáticos agora rodam em **modo somente local**: nada escuta fora de 127.0.0.1/::1 e nada fala com a internet. Não mexemos em regras do
firewall nem em configuração do sistema.

## Como liga

`BRIKS_NET_LOOPBACK_ONLY=1` (módulo `briks-core/src/netmode.rs`). Já fica ligado sozinho em:

- `cargo test` / `cargo run` dentro de `apps/desktop-cui/briks-core` (`.cargo/config.toml` `[env]`), e no binário de teste (`cfg(test)`:
  sem a variável, o modo é ligado);
- `build.ps1`, `scripts/paridade.ps1`, `scripts/capturas-*.ps1`, `scripts/prova/matriz.ps1`, `scripts/prova/modal/rodar.ps1`,
  `scripts/prova/sistema/rodar.ps1` (o app C e o core filho herdam o ambiente).

Scripts novos que abrem o app/core: definam `BRIKS_NET_LOOPBACK_ONLY=1` junto com `BRIKS_HIDDEN=1`.
Produção: sem a variável nada muda (o `.cargo/config.toml` só vale para processos lançados pelo cargo, não para o exe instalado).

## O que o modo faz

- **ICE/webrtc-rs** (malha, SFU, sonda de rede): filtro de IP só loopback + candidato loopback incluído; mDNS desligado (o padrão abre
  UDP 0.0.0.0:5353); TODOS os servidores STUN/TURN são descartados (coletar srflx/relay abre UDP em 0.0.0.0, medido com Get-NetUDPEndpoint, até com o servidor em 127.0.0.1).
  Todos os `SettingEngine` passam por `netmode::apply`, e as listas de servidores por `netmode::ice_servers`. **Quem criar um novo
  `SettingEngine`/`RTCConfiguration` deve chamar os dois.**
- **Saída HTTP/WebSocket**: `netmode::deny_url` recusa host que não seja loopback em Realtime/SFU (`open_io`), contas/Supabase,
  `turn-credentials`, `livekit-token`/`livekit-kick`, foto de perfil, prévia de link. No app C: atualizador (`update.c`, só feed
  `http://127.0.0.1`/localhost) e sonda de internet (`net_state.c`, estado "online" sem sondar). Novo código de rede externa deve chamar
  `deny_url` antes de conectar.
- **Sockets do core**: IPC, mocks, OAuth loopback e simulador de rede já usam 127.0.0.1 (o `netsim` agora também, no modo).
- **LiveKit de teste**: `tests_sfu::lk_port()` lê `BRIKS_LIVEKIT_TEST_PORT` (padrão 7880; a de mídia UDP é porta+2) e
  `tests_sfu::lk_args()` monta `--dev --bind 127.0.0.1 --config <yaml>` (porta HTTP, `bind_addresses` e `rtc.ips.includes: 127.0.0.0/8`,
  `tcp_port: 0`, `use_external_ip: false`, YAML na pasta TMP do processo). Use `lk_port()`/`lk_args()` em qualquer teste novo que
  suba o livekit-server (`BRIKS_LIVEKIT_SERVER_BIN`); nada de 7880 fixo.

## Variáveis

| Variável | Efeito |
|---|---|
| `BRIKS_NET_LOOPBACK_ONLY=1` | liga o modo; `=0` desliga mesmo dentro do cargo test |
| `BRIKS_TEST_REAL_NET=1` | em `cargo test`, desliga o modo para os testes que precisam de STUN/TURN/internet reais (marcados `#[ignore]` + `netmode::real_net_wanted()`); rode com `--ignored` |
| `BRIKS_LIVEKIT_TEST_PORT` | porta HTTP do livekit-server de teste |
| `BRIKS_LIVEKIT_SERVER_BIN` | caminho do binário (sem ele os testes de SFU são pulados) |

## O que fica de fora / não verificado

- Testes marcados `#[ignore]` que usam rede de verdade (`real_supabase`, matriz de rede, etc.) continuam fora da suíte padrão.
- **Limitação do modo loopback para o LiveKit (verificada com livekit-server 1.13.11 em 2026-11-11):** com `BRIKS_NET_LOOPBACK_ONLY=1` no
  cliente, 11 testes SFU/hybrid falham ("publisher não conectou"): o servidor não anuncia candidato 127.0.0.1 (nem com `rtc.node_ip` nem com
  `rtc.ips.includes 127.0.0.0/8`) e o ICE nunca sai de `checking`. Com `BRIKS_NET_LOOPBACK_ONLY=0` os mesmos testes passam (19 ok, 5 `#[ignore]`).
  Por isso os testes SFU rodam com a variável em `0` e o servidor preso a `127.0.0.1` (`lk_args()`: `--dev --bind 127.0.0.1 --node-ip 127.0.0.1`),
  sem internet. Ressalva: com o filtro desligado o cliente coleta candidatos nas interfaces locais (inclusive VPN/LAN), mas fala só com o servidor
  em 127.0.0.1. Comando: `BRIKS_NET_LOOPBACK_ONLY=0 BRIKS_LIVEKIT_SERVER_BIN=<exe> cargo test --release -- --test-threads=1 tests_sfu tests_hybrid`.
  Os binários ficam fora do git (ex.: `A:\BRiskCall\_tools\livekit\livekit-server.exe`).
- Subprocessos externos dos testes (deno/PowerShell em `tests_sfukick`) não passam pelo modo.
- Medição (Get-NetUDPEndpoint + Get-NetTCPConnection -State Listen, amostrando os processos de teste a cada 100 ms durante a suíte do
  briks-core): só 127.0.0.1 em 1154 amostras, exceto UM socket UDP 0.0.0.0 durante `ice::probe_reports_a_dead_turn...` (coleta relay) --
  corrigido descartando todos os STUN/TURN no modo. **Não reconferido depois da correção** com a suíte inteira, e não medidos: C
  (briks_*_test e roteiros do app), livekit-server (binário ausente). (`Get-NetTCPEndpoint` não existe: use `Get-NetTCPConnection`.)
- Em debug, três testes de vídeo/áudio por contagem de quadros falham por desempenho (com e sem o modo, portanto não são da rede):
  `screen_share_flows_end_to_end_in_loopback`, `two_simultaneous_shares_in_one_room`, `screen_audio_is_stereo_and_av_offset_is_measured`.
  A documentação do SFU manda rodar com `--release`.
- Ao abrir o app real (fora de teste) o modo não vale: só com a variável definida explicitamente.
