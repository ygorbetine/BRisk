# Plano: substituir Electron por Tauri (mantendo a estética)

Escrito em 2026-11-09. Baseado na leitura de `apps/desktop/src` (≈1.570 linhas de TS no processo principal) e `docs/architecture.md`.
CUI foi identificado: yappologistic/CUI, biblioteca de UI nativa em C (ver seção 9).

## 1. Por que a estética é o menor risco

O visual vive 100% em `apps/web` (React + Vite + CSS). Tauri renderiza o mesmo front num WebView, então telas, CSS, animações e o overlay (`#overlay`) não são reescritos. O trabalho real está **no que o Electron fornecia por trás**, não na UI.

Cuidado único de estética: o motor muda. Windows = WebView2 (Chromium, igual ao Electron: fontes, `backdrop-filter`, scrollbar iguais). Linux = WebKitGTK (renderiza diferente; scrollbars, `backdrop-filter`, fontes e WebRTC mudam). macOS = WKWebView (não é alvo hoje).

## 2. Mapa Electron → Tauri

| Hoje (Electron) | Em Tauri | Risco |
|---|---|---|
| `window.ts`, tray, tela cheia, instância única | `tauri` core + `tray-icon` + plugin `single-instance` + `window-state` | baixo |
| `protocol.ts` (`briks://app`, origem estável p/ IndexedDB) | `tauri://localhost` / `http://tauri.localhost` (origem **muda** → migrar IndexedDB/localStorage) | **alto** (identidade P2P é `CryptoKey` não extraível em IndexedDB) |
| `backend.ts` (sidecar Rust/Axum, token, porta loopback) | Sidecar via plugin `shell` **ou** embutir o crate `desktop-backend` como lib dentro do processo Tauri | baixo; embutir remove um processo |
| `ipc/index.ts` (backend:config, rede, ícone, emoji, abrir link, updater) | `#[tauri::command]` + capabilities; `app:icon` via crate de ícones Win32; emoji panel via `SendInput Win+.` | médio |
| `call-shortcuts.ts`, atalhos do overlay | plugin `global-shortcut` | baixo |
| `overlay.ts` (transparente, topo, click-through, sem foco) | `WebviewWindow` com `transparent`, `always_on_top`, `set_ignore_cursor_events`, `focusable(false)` | médio (testar sobre jogos fullscreen borderless) |
| `updater.ts` (electron-updater + GitHub releases) | plugin `updater` (assinatura Ed25519 obrigatória, manifest `latest.json`) | médio — nova chave de assinatura |
| `permissions.ts` | capabilities + permissões do WebView2 (`PermissionRequested`) | baixo |
| `gpu.ts` (flags Chromium: NVENC/AMF, zero-copy, autoplay) | WebView2 aceita `additionalBrowserArguments` (mesmas flags); Linux sem equivalente | médio |
| **`screen-capture.ts`** (`desktopCapturer`, `setDisplayMediaRequestHandler`, áudio loopback do sistema) | **Não existe em WebView.** `getDisplayMedia` abre o seletor do WebView2 sem controle de fonte e **sem áudio do sistema/por app** | **crítico** |

## 3. O bloqueio crítico: captura de tela e áudio

É o núcleo do produto (transmissão com áudio, e a próxima feature "isolar o som de um app, ex.: Spotify"). Opções:

- **A. Captura nativa em Rust (recomendada):** Windows Graphics Capture (`windows-capture`/`windows` crate) para vídeo + WASAPI process-loopback (`ActivateAudioInterfaceAsync` com `PROCESS_LOOPBACK`) para áudio por app ou sistema. Entregar ao front como `MediaStreamTrack` via `MediaStreamTrackGenerator`/`VideoFrame`+`AudioData` (WebCodecs) ou, melhor, empurrar frames para um `<canvas>.captureStream()` e áudio por `AudioWorklet`. Ganha de bônus o seletor de áudio por app que já está planejado.
- **B. Manter `getDisplayMedia` do WebView2:** funciona para tela/janela com o seletor nativo do Chromium, mas perde o seletor próprio, perde loopback controlado e o áudio por app. Serve só como fallback.
- **C. Híbrido:** vídeo via `getDisplayMedia` (B) e áudio via Rust (A). Menos código, bom primeiro marco.

Linux: WebKitGTK tem WebRTC instável/inexistente conforme a distro. **Decisão necessária:** manter Electron só no Linux, ou abandonar `.deb/AppImage` por ora. Sugiro Tauri só no Windows na primeira entrega.

## 4. Fases

0. **Spike (2–3 dias, sem tocar no Electron):** projeto `apps/desktop-tauri` abrindo `apps/web/dist`; validar numa janela: WebRTC P2P entre dois peers, `getDisplayMedia` no WebView2, IndexedDB, overlay transparente click-through sobre um jogo borderless. **Critério de parada:** se WebRTC/overlay falhar, abortar.
1. **Casca:** janela, tray, instância única, tela cheia, atalhos globais, links externos, migrador de dados (`@briks/desktop` → identificador Tauri `com.briks.calls`).
2. **Backend:** embutir/spawnar `desktop-backend`, expor `backend:config` como command, manter token por execução. Adaptar o preload (`preload.cts`) para uma camada `window.briks` que fala `invoke` (o front não muda a API, só o bridge).
3. **Origem e dados:** decidir a origem estável (`tauri.localhost`) e migrar IndexedDB (identidade P2P!) e localStorage da origem `briks://app`. Sem migração o usuário perde a chave de identidade. Plano: exportar na última versão Electron para um arquivo no `userData`, importar no primeiro boot Tauri. **Requer uma release final do Electron.**
4. **Overlay e atalhos:** portar `OverlayController` (mantendo canal de frames/estado via eventos Tauri).
5. **Captura (seção 3):** opção C primeiro, depois A.
6. **Updater e empacotamento:** NSIS via `tauri build`, chave de updater, `latest.json` no GitHub Releases, manter `Briks-Setup-${version}.exe`. Atualizar `scripts/package-*`, `verify-packaged-*`, `.github` e `THIRD_PARTY_NOTICES` (Electron/Chromium sai; Tauri/WebView2 entra).
7. **Transição:** updater do Electron entrega o último build Electron com migrador; só depois a primeira versão Tauri. Nada publicado sem seu ok.

## 5. Ganhos esperados / custos

- Instalador de ~100 MB+ → ~10–20 MB; RAM ociosa bem menor (relevante: roda ao lado de jogos).
- Custo: reescrever ~1.500 linhas de main process em Rust, captura nativa, nova cadeia de update/assinatura, testes de empacotamento refeitos, perda de paridade Linux.

## 6. Testes que precisam existir antes de trocar

Smoke empacotado (`verify-packaged-origin`, `two-instances`) reescritos para Tauri; chamada de 2 instâncias com vídeo+áudio; compartilhamento de tela com áudio; overlay sobre jogo; atualização Electron→Tauri preservando identidade e SQLite; instalar por cima sem perder `userData`.

## 7. Riscos ordenados

1. Captura de tela/áudio (crítico). 2. Perda da identidade P2P na troca de origem. 3. WebRTC no WebKitGTK. 4. Overlay sobre jogos exclusive-fullscreen (já é limitação hoje). 5. Chave de assinatura do updater (perder = não atualizar mais).

## 8. Estimativa

Spike 2–3 d · Fases 1–4 ≈ 2 semanas · Fase 5 (captura nativa) 2–3 semanas · Fases 6–7 ≈ 1 semana. Total ≈ 6–8 semanas de uma pessoa.

## 9. CUI (yappologistic/CUI) — avaliação

[CUI](https://github.com/yappologistic/CUI) é uma biblioteca **C99, MIT, sem dependências**, ~150–190 componentes nativos (OpenGL 3.3 com fallback por software; Windows/macOS/Linux X11). Não é um WebView. Fatos relevantes (fonte: README e site de docs do projeto):

- **Sem WebRTC, sem navegador.** O front atual usa WebRTC/`getDisplayMedia`/WebCrypto/IndexedDB do Chromium. Trocar o front por CUI exige substituir tudo isso por **libwebrtc nativa** (build gigante) ou outra pilha. Sem isso, não há chamadas.
- **Reescrita total da UI:** todo o `apps/web` (React) teria de ser refeito em C. Os `packages/*` TS (protocol, rtc) não rodam lá.
- **Estética:** o tema é de tokens semânticos (superfícies neutras, um acento, bordas finas, Inter/Geist/Lucide). Blur/vidro/gradientes **não são confirmados** na documentação; o visual roxo do Briks exigiria canvas customizado ou renderer próprio. Alguns pontos de risco: sem árvore de acessibilidade, sem Wayland nativo, IME simples, sem superfície de vídeo documentada.
- **Sem bindings Rust:** teria de chamar a API C via FFI.

**Conclusão:** CUI **não substitui** o Electron/React do Briks. Ela encaixa como **peça nativa dentro do shell Tauri**, principalmente em:

1. **Overlay de jogo:** hoje é um `BrowserWindow` Chromium inteiro transparente por cima do jogo. Uma janela nativa leve desenhada por CUI (ou só pelo renderer de triângulos dela) gastaria muito menos CPU/GPU/RAM junto do jogo, e alinha com o plano de Modo Jogo. Os frames/estado hoje vão por IPC (`overlay:state`, `overlay:frame`), que viraria um canal Rust → CUI.
2. **Janelas utilitárias** (mini-controle de chamada, seletor de tela/áudio por app), onde não precisamos de WebRTC.

### Fase extra: spike CUI (3–4 dias, depois do spike Tauri)
- Compilar CUI com CMake e ligar ao Rust (`cc`/`cmake` crate + bindgen).
- Janela borderless transparente, topo, click-through (CUI precisa expor o handle da janela, ou usar o renderer só de triângulos sobre a janela criada pelo Tauri/winit).
- Reproduzir 1 tela do overlay (lista de participantes + miniatura) com o tema roxo, testando se dá para obter o visual atual (gradiente/vidro) sem hack.
- **Critério:** se não igualar a estética, o overlay continua em WebView e CUI é descartada.

### Ordem recomendada
Spike Tauri (fase 0) → Tauri casca + backend + dados → captura nativa → só então o spike CUI para o overlay. Todo o trabalho acontece no branch `experimento/tauri`.

### Resultado do spike CUI (branch `experimento/cui`, `apps/desktop-cui`, 2026-11-09)
- Compila no Windows com MSVC + CMake (FetchContent), ~60 arquivos C, sem dependências. `apps/desktop-cui/build.ps1`.
- Paleta do Briks (`#12041a`, `#1a0a26`, `#2f1745`, `#8b5cf6`, `#a592b8`) aplicada via `cui_theme_get()->colors[]`: cores, raio 20, borda e texto saíram corretos.
- `cui_app_native_window()` devolve o HWND; com Win32 (`WS_EX_LAYERED|TRANSPARENT|NOACTIVATE|TOPMOST`) a janela fica no topo, sem foco, com cliques passando e opacidade global (LWA_ALPHA). Vidro/blur real e gradientes de fundo **não** foram testados e não têm API documentada.
- Falta no spike: remover a barra de título (estilo `WS_POPUP` via Win32), transparência por pixel, teste sobre jogo borderless, medir CPU/RAM (processo ocioso: ~129 MB de working set na primeira medição, a investigar; provavelmente OpenGL).
