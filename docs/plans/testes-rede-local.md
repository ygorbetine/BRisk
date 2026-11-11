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
  UDP 0.0.0.0:5353); servidores STUN/TURN que não sejam loopback são descartados (coletar srflx/relay abre 0.0.0.0:porta).
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
- O `livekit-server` não estava disponível na máquina ao implementar: a configuração YAML (`rtc.ips.includes`, `tcp_port: 0`) segue a
  documentação do LiveKit, mas **não foi conferida contra o binário** nem com `Get-NetUDPEndpoint`. Se ele ainda abrir 0.0.0.0, ajustar
  `lk_args()`.
- Subprocessos externos dos testes (deno/PowerShell em `tests_sfukick`) não passam pelo modo.
- A prova com `Get-NetUDPEndpoint`/`Get-NetTCPEndpoint` durante a suíte completa ainda não foi feita (ver seção abaixo quando houver).
- Ao abrir o app real (fora de teste) o modo não vale: só com a variável definida explicitamente.
