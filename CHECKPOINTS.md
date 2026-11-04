# Checkpoints — BRisk (fork de Risk-P2P para transmissão de tela)

Nota: o código ainda usa "Risk" em identificadores internos (veja o CP6). O nome visível ao usuário já é BRisk.

Legenda: `[x]` feito · `[ ]` pendente · `[~]` feito, não verificado

Licença: o projeto base é AGPL-3.0. Software derivado distribuído (ou oferecido por rede) precisa ter o código-fonte aberto sob AGPL.

---

## CP0 — Ambiente

- [x] Clonar `xxsamboladiones/Risk-P2P` em `A:\BRiskCall\Risk-P2P`
- [x] `pnpm install` na raiz (pnpm 10.15 via corepack; Node 22)
- [~] Toolchain: Node e pnpm ok; **Rust (cargo) não instalado**, necessário para o `desktop-backend`
- [x] `pnpm typecheck` e `pnpm test` passando (web 176, rtc 69, desktop 13, protocol 7)
- [x] `pnpm dev` abre o app Electron (Rust instalado; backend compilado)
- [x] Supabase: organização BRiskGroup, projeto `brisk` (plano Free); URL e chave pública no `.env` local (fora do git)

**Pronto quando:** o app abre e os testes originais passam.

---

## CP1 — Estética RASTRO

Referência: captura do RASTRO (fundo ameixa escuro, barra de título magenta, acento violeta, painéis arredondados).

- [x] Remapear paleta lima → violeta nos CSS (`apps/web/src/styles.css`, `components/*.css`)
- [x] Fundo da janela Electron e `theme-color`
- [~] Conferir visualmente todas as telas (login, lobby, chamada, modais, chat, configurações de rede/áudio)
- [ ] Ajustar contrastes ruins encontrados na conferência
- [ ] Barra de título magenta (`#b01fcf`) com menu enxuto
- [ ] Tipografia e logo próprios (a fonte atual é DM Sans/Manrope)
- [ ] Seletor de idioma EN/PT, se quiser manter
- [ ] Tokens de cor em variáveis CSS (`:root`) para facilitar novos ajustes

**Pronto quando:** nenhuma tela mantém resquício do tema verde e o texto continua legível.

---

## CP2 — Áudio só da janela (Windows)

Hoje: `loopbackWithoutChrome` captura o áudio do sistema inteiro, exceto o do Risk (`apps/desktop/src/screen-capture.ts:109`).

Meta: ao compartilhar uma janela, enviar só o áudio daquele aplicativo.

- [x] Obter o PID a partir do id da fonte (`window:HWND` → `GetWindowThreadProcessId`)
- [x] Captura por processo com WASAPI *process loopback* (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, incluir árvore de processos) no `desktop-backend` (Rust)
- [x] Endpoint no sidecar (ex.: `/screen-audio/prepare` com PID) e streaming do PCM até o renderer
- [x] Renderer: criar a faixa de áudio (AudioWorklet ou `MediaStreamTrackGenerator`) e anexá-la ao stream da tela (`apps/web/src/call/media/screen-share.ts`)
- [x] Tela inteira: mixa as capturas por processo, exceto Discord e BRisk
- [x] Fallback quando o Windows for anterior ao 10 2004 ou a captura falhar
- [x] Encerrar a captura ao parar o compartilhamento ou fechar a janela
- [~] Testes: unitários do mapeamento fonte → PID (ok); endpoint testado com áudio real (pico 0.17). Falta teste manual dentro do Electron com jogo/player e Discord

Limitação conhecida: navegadores agrupam o áudio de todas as abas sob o mesmo processo. Dá para isolar o navegador, não uma aba. Ver CP3.

**Pronto quando:** compartilhar uma janela de app transmite só o som dela, sem notificações nem outros apps.

---

## CP2b — Conviver com o Discord (requisito)

Objetivo: estar em call no Discord enquanto o Risk transmite, sem um atrapalhar o outro.

- [x] Tela inteira: capturas por processo, sem Discord e BRisk (em vez do modo *excluir*, que aceita um PID só) (`PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`) para tudo menos Discord e Risk, de modo que a voz do Discord nunca entra na transmissão
- [x] Janela: modo **incluir** só o processo da janela (CP2)
- [x] Microfone desligado por padrão no BRisk: o Discord continua dono do microfone e da voz
- [x] Não mexer no dispositivo padrão de áudio nem usar modo exclusivo do WASAPI
- [ ] Sem eco: o espectador não ouve a própria voz de volta; confirmar que o áudio do Risk não vaza para o Discord
- [ ] Teste manual: call no Discord + jogo + transmissão no Risk, conferindo que a voz do Discord não aparece no stream

**Pronto quando:** o espectador ouve só o app/jogo, e quem está no Discord não percebe o Risk.

---

## CP3 — Abas do navegador (decisão pendente)

- [x] Escolher a abordagem: A (navegador inteiro via loopback de processo), pois áudio no Windows é por processo. Opções B/C ficam como evolução:
  - **A.** Aceitar áudio do navegador inteiro (já resolvido pelo CP2)
  - **B.** Extensão ou captura via Chrome (`tabCapture`) para áudio de uma aba específica
  - **C.** Abrir a aba em janela própria/perfil separado do navegador, para ficar num processo isolado
- [x] Implementar a opção escolhida (nota explicativa no seletor de tela)

**Pronto quando:** a abordagem está decidida e o comportamento está documentado na interface.

---

## CP4 — Conexão mais estável

Diagnóstico: por padrão só há STUN (`apps/web/src/services/rtc/ice.ts`), que falha em CGNAT, NAT simétrico e firewalls restritos. A recuperação fica em `packages/rtc/src/transport/mesh.ts`.

Infraestrutura (depende de você):
- [ ] Subir o Coturn (`infrastructure/coturn`): UDP/TCP 3478 e TLS 5349
- [ ] Publicar o emissor de credenciais temporárias e definir `VITE_TURN_CREDENTIALS_URL`
- [ ] `pnpm verify:turn-config` passando

Código:
- [~] Medir onde cai: os estados já são registrados; falta medir o tempo até conectar em redes reais
- [x] Ajustar tempos e tentativas de recuperação: queda detectada em 2,5 s (antes 4 s); o lado de id maior assume a recuperação após 6 s se o outro não agir; vigia reinicia o ICE se a conexão inicial não completar em 15 s
- [x] Conectar mais rápido: pré-coleta de candidatos (`iceCandidatePoolSize: 4`). TURN/TCP 443 depende do servidor TURN (infra)
- [x] Reiniciar o ICE ao mudar de rede (Wi-Fi/VPN/volta da internet), sem esperar a queda
- [ ] Reduzir a configuração: entrar por um código/link, sem exigir ajustes de rede
- [ ] Simplificar o diagnóstico para o usuário: mensagem única do tipo "sem relay disponível"
- [ ] Testar em redes ruins: 4G/CGNAT, Wi-Fi corporativo, VPN

**Pronto quando:** duas pessoas em redes diferentes conectam sempre, e uma queda breve se recupera sozinha.

---

## CP5 — Foco em transmissão de tela (enxugar)

- [x] Decisão: manter chat, grupos e arquivos (você pediu para manter a organização do Risk); nada removido. Decidir o que sai: chat, grupos, amizades, transferência de arquivos, persistência SQLite
- [ ] Remover o que sobrar do escopo, em commits pequenos
- [ ] Entrada simples: criar sala → copiar link → transmitir
- [ ] Presets de qualidade (até 1080p/60) mantidos e testados

**Pronto quando:** o fluxo "abrir → compartilhar → alguém assiste" tem poucos cliques e nenhuma tela desnecessária.

---

## CP5b — Qualidade nível Discord

Hoje é WebRTC em malha (mesh): o transmissor envia uma cópia do vídeo para cada espectador. Isso não escala.

Requisitos definidos: normalmente 4 pessoas, até 10; várias pessoas podem transmitir a tela ao mesmo tempo.

Com 10 pessoas e vários transmissores, a malha exige até 9 envios por transmissor (ex.: 9 × 8 Mbps = 72 Mbps de upload em 1080p60). Não é viável.

- [x] Decisão: usar um **SFU** (recomendado: LiveKit, que tem servidor aberto, SDK para o navegador e simulcast). Cada transmissor envia um fluxo só; o servidor distribui.
- [~] Subir o SFU: `infrastructure/livekit` (compose + config + README) e emissor de tokens `supabase/functions/livekit-token` prontos; falta o usuário hospedar o servidor e publicar a função
- [x] Trocar o transporte em `packages/rtc` (hoje `transport/mesh.ts`) por um cliente do SFU (`transport/sfu.ts`, LiveKit; ativo com `VITE_LIVEKIT_TOKEN_URL`), mantendo a interface `call-transport.ts`
- [ ] Várias transmissões simultâneas na mesma sala, com layout de grade e foco em uma delas
- [ ] Simulcast/SVC: o espectador recebe a camada que cabe na sua rede
- [ ] Manter a malha P2P como modo opcional para 2 a 3 pessoas? (decidir; custo extra de manutenção)
- [ ] Codificação por hardware (NVENC/AMF/QuickSync) para não derrubar o FPS do jogo; verificar as flags do Chromium no Electron
- [ ] Codec: H.264 por hardware como padrão, AV1/VP9 onde houver suporte
- [ ] Prioridade de nitidez/fluidez conforme o conteúdo (`contentHint` texto/movimento) e degradação graciosa quando a rede cai
- [x] Controles **independentes** de resolução e FPS (ex.: 1440p30, 720p60), não só presets fixos; presets continuam como atalho
- [x] Bitrate em **Automático** por padrão (ajusta conforme a rede e o conteúdo)
- [x] Níveis de bitrate para forçar qualidade: Automático / Alto / Máximo / Personalizado (Mbps). Em Máximo/Personalizado, prioriza nitidez e avisa sobre o custo de banda
- [x] Em rede ruim, a degradação é configurável: preservar resolução (cai FPS) ou preservar FPS (cai resolução)
- [x] Aplicar as mudanças com a transmissão ativa (já existia `updateScreenQuality`; o painel usa o mesmo caminho) (sem reiniciar o compartilhamento), usando `RTCRtpSender.setParameters` / o equivalente do SFU
- [~] Limites seguros: upload estimado e aviso de banda no painel; falta validar contra o hardware e o upload medido resolução × FPS × bitrate contra o hardware e o upload medido
- [ ] Medidor de qualidade para o transmissor (bitrate, FPS, perda, RTT)
- [ ] Latência baixa: meta abaixo de ~300 ms em rede normal
- [ ] Comparar lado a lado com o Discord (mesmo jogo, mesma rede) e registrar os resultados

**Pronto quando:** em teste lado a lado, nitidez, fluidez e latência ficam equivalentes às do Discord.

---

### Interface (pedido extra)
- [x] Glassmorfismo: fundo ambiente em degradê, painéis de vidro (menu lateral, cabeçalho, conta, composer, modais, barra da chamada, rótulos), com *fallback* sem `backdrop-filter` e para `prefers-reduced-transparency`
- [~] Conferir visualmente as telas internas (só vi o login)

---

## CP6 — Entrega

- [x] Nome do produto: **BRisk** (interface, título da janela, bandeja, `productName`, `appId` `com.brisk.calls`, nome do instalador)
- [ ] Renomear os identificadores internos de Risk para BRisk (pacotes `@risk/*`, `RISK_*`, `risk-desktop-token`, esquema de protocolo, diretório de dados). Só depois de o app rodar, porque mexe no protocolo e nos dados locais
- [x] Atualizar `README.md`, `docs/` e `infrastructure/coturn/README.md` com o nome novo
- [x] Ícone e logo próprios do BRisk (marca "B" em degradê magenta/violeta: `apps/web/src/assets/brisk-mark.svg`, `icon.png`/`icon.ico` do desktop, favicon)
- [x] Revisar o `THIRD_PARTY_NOTICES.md` e manter a licença AGPL
- [x] `pnpm package:win` gera o instalador (`BRisk Setup 0.2.1.exe`, ~100 MB, sem assinatura de código)
- [ ] Teste de ponta a ponta entre duas máquinas
- [ ] Criar seu próprio repositório (fork) e subir o trabalho

---

## Ordem sugerida

CP0 → CP1 (conferir) → CP2 + CP2b → CP4 → CP5b → CP3 → CP5 → CP6

O CP4 em paralelo ao CP2, já que a parte de infraestrutura do TURN não depende de código.

Com o SFU, o TURN do CP4 continua necessário para clientes atrás de firewall restrito, mas o LiveKit já traz TURN embutido, então vale avaliar juntar os dois.
