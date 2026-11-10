# Briks nativo: interface em CUI + WebRTC nativo

Branch: `experimento/cui-nativo` (worktree `A:\BRiskCall\Risk-P2P-cui`). Independente do `brisk-dev`: nada daqui entra na linha de versões do Briks funcional.
Dados do experimento ficam em `%APPDATA%\Briks-CUI`, nunca em `@briks\desktop`.

## Arquitetura

```
CUI (C99, apps/desktop-cui)  ── HTTP loopback + token ──>  briks-desktop-backend (Rust/Axum/SQLite, já existe)
        │                                                          │
        └── eventos/quadros (marco 3+) <── briks-core (Rust): webrtc-rs, signaling Supabase, codecs, captura
```

- A UI não fala WebRTC: ela renderiza estado e envia comandos.
- O backend existente continua dono de contas, amigos, grupos, mensagens (SQLite). Reuso integral.
- `briks-core` é um crate novo (marco 3), no mesmo workspace Rust, com `webrtc-rs` (ICE/DTLS/SRTP/DataChannel em Rust puro), `tokio-tungstenite` para o Supabase Realtime (protocolo Phoenix) e captura via os módulos Windows que o backend já tem (`screen_audio`, `process_audio`).

## Marcos

1. **Contas e social local (feito primeiro):** sidecar iniciado pelo CUI, cliente HTTP em C, login/cadastro/redefinição reais, `/me`, amigos, pedidos de amizade, comunidades. Sem mocks.
2. **Mensagens e grupos P2P locais:** rotas `p2p::router` (mensagens, grupos, canais, anexos) ligadas às telas; histórico persistente.
3. **`briks-core`: signaling + DataChannel:** Presence/Broadcast do Supabase, negociação (Perfect Negotiation, offer/answer/ICE conforme `docs/protocol.md`), chat por DataChannel `briks.chat` com assinatura ECDSA. Interoperar com o app Electron atual enquanto os dois existirem.
4. **Áudio:** captura WASAPI, Opus, SRTP, reprodução, mudo/volume por pessoa, push-to-talk.
5. **Vídeo e tela:** captura Windows Graphics Capture, codificação H.264 por Media Foundation (hardware), decodificação e textura no CUI (`cui_texture`).
6. **Identidade:** a chave ECDSA hoje é `CryptoKey` não extraível no IndexedDB do Chromium. O app nativo não consegue lê-la: ou gera identidade nova, ou o Electron exporta uma última vez.
7. **Updater, instalador, overlay, atalhos globais, tray.**

## Riscos conhecidos

- Interoperabilidade de vídeo com navegadores (perfis H.264/VP8, RTCP, simulcast) é o maior risco do marco 5.
- Sem acessibilidade (screen reader) no CUI; sem Wayland nativo.
- webrtc-rs é menos maduro que libwebrtc; testar NAT/TURN cedo (marco 3).

## Glassmorfismo real (prioridade 1) — feito

`backdrop-filter: blur(20px) saturate(155%)` implementado em `apps/desktop-cui/src/glass.c`:

- **Gancho no CUI:** `patches/cui-render-override.patch` (aplicado por `patches/apply.cmake` via `PATCH_COMMAND`, idempotente; CUI fixado no commit `e8688b8`) acrescenta `cui_app_set_render_override()` e `cui_app_gl_renderer()`. O caminho de screenshot também passa pelo gancho.
- **Detecção sem mexer no layout:** em vez de um comando sentinela, o gancho varre os triângulos RRECT e trata como painel de vidro todo fundo com as cores `B_GLASS`, `B_GLASS_SOFT`, `B_GLASS_STRONG` (camada 0) ou `B_GLASS_POPUP` (camada 1; é o token `SURFACE_RAISED`, usado por diálogos). Retângulo e raios saem dos vértices.
- **Desenho em segmentos:** o quadro é partido no índice do primeiro triângulo de cada painel. Antes do fundo translúcido do painel: copia o framebuffer (`glCopyTexSubImage2D`), dual-Kawase (4 níveis, offset 1, ~ blur 20px; `BRIKS_GLASS_LEVELS`/`BRIKS_GLASS_OFFSET` ajustam), pinta o retângulo arredondado com o resultado saturado 1.55 e o brilho interno de 1px (`inset 0 1px 0 rgba(255,255,255,.09)`, compensado pela opacidade do tint). O CUI então desenha tint, borda e conteúdo. O desfoque só é refeito quando a camada muda (base compartilha o fundo; popups recapturam a cena inteira).
- **GL:** funções via `wglGetProcAddress` (+ `opengl32` para 1.1); estado/FBO/viewport restaurados. Fallback sem desfoque se for renderer por software, se faltar função ou com `BRIKS_GLASS=0`.
- **Ferramentas:** `BRIKS_GLASS_TEST=1` desenha barras nítidas atrás dos painéis; `BRIKS_GLASS_LOG=1` imprime ms por quadro (com `glFinish`); `BRIKS_DEV_AUTH` / `BRIKS_DEV_OPEN=settings|add` + `CUI_SCREENSHOT`/`CUI_SCREENSHOT_FRAMES` geram capturas (em `apps/desktop-cui/shots/`).

### Medições (1280x800, GPU integrada/dedicada do usuário, Release padrão do CMake)

| Item | Valor |
|---|---|
| `briks_cui.exe` | 1,06 MB (+ backend sidecar 8,1 MB) |
| CPU ocioso (10 s, logado) | 0 ms (0,00%) — o CUI dorme sem animação |
| RAM (working set) | 98 MB app (a maior parte é driver GL) + 13 MB sidecar |
| Custo por quadro com vidro | ~6 ms medidos com `glFinish` (inclui o render inteiro; só ocorre em quadros animados/interativos) |

### Corrigido no caminho

- O sidecar saía logo após `READY` quando o app não tinha stdin válido (GUI sem console): agora recebe um pipe de stdin mantido aberto.
- `main.c`: a faixa de gradiente era irmã de layout e roubava metade da largura na tela social; agora é uma coluna acima do STACK de telas.

### Limites conhecidos do vidro

- Painéis da mesma camada não desfocam uns aos outros (um painel dentro de outro mostra o fundo ambiente desfocado, não o pai); se precisar, usar camada 1 com cor `B_GLASS_POPUP`.
- Cada painel recorta pelo clip do comando de desenho, não por scroll aninhado.
- Cor do vidro é identificada por igualdade RGBA exata: evitar usar essas quatro cores para outra coisa.
- A comparação lado a lado com o app web ainda não foi feita (só capturas do CUI com e sem vidro em `shots/`).

### O que falta

Marco 2 (mensagens/grupos P2P e telas de perfil/configurações/histórico), overlay (`WS_EX_*`) como segundo executável, spike do marco 3 (`briks-core` com webrtc-rs) e screenshots lado a lado com o app web.
