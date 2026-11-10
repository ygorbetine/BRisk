# Migração: app nativo (CUI) como app principal

Decisão do usuário: "Não quero mais usar o Electron". O app principal passa a ser `apps/desktop-cui` (C99 + CUI, com o `briks-core` em Rust para sinalização/WebRTC/áudio). O Electron foi removido do branch em um commit separado, fácil de reverter.

## Como voltar atrás
- Branch local `backup/electron-pre-cui` aponta para o último commit de `brisk-dev` que ainda tinha o Electron (`0e4da19`).
- O commit "remove Electron" é independente: `git revert <hash>` devolve `apps/desktop`, `release.yml` e os scripts.

## O que foi removido
`apps/desktop/**` (main, preload, updater, tray, overlay, empacotamento), `.github/workflows/release.yml`, `infrastructure/desktop/Dockerfile.linux`, `scripts/package-linux-docker.ps1`, `scripts/verify-packaged-renderer-origin.mjs`, `scripts/verify-packaged-two-instances.mjs`, scripts `dev:desktop`, `build:desktop`, `package:*`, `verify:packaged-*` do `package.json`, `electron` do `pnpm-workspace.yaml`, o importer `apps/desktop` do `pnpm-lock.yaml`.

## O que continua (o backend, o site e os outros apps dependem)
`apps/web` (site/renderer React), `desktop-backend` (sidecar Rust, também empacotado com o app nativo), `server`, `supabase`, `infrastructure/{coturn,livekit}`, `packages/*`, workflows de CI (`ci.yml`, `audit-fixes.yml`).

## Como compilar
`powershell -File scripts/build-cui.ps1` (`-SkipTests` pula o `cargo test`). Saída em `apps/desktop-cui/build/`.
Supabase: `VITE_SUPABASE_URL` / `VITE_SUPABASE_ANON_KEY` por variável de ambiente ou `BRIKS_ENV_FILE`.

## Dados do usuário
- O app nativo grava em `%APPDATA%\Briks-CUI` (`settings.json`, `profile.json`, `theme.json`...).
- O Electron usava a própria pasta de dados (`%APPDATA%\Briks`/userData do Electron). **Nada é migrado automaticamente**: a instalação e os dados do Electron não são tocados. Se quiser trazer algo (perfil, amigos), é uma migração a ser feita à parte, avisando o usuário.

## Pendências conhecidas (não bloqueiam este commit)
- Instalador, assinatura e auto-update do app nativo: não existem. Publicar release/tag/versão exige autorização separada do usuário.
- `pnpm-lock.yaml`: foi editado à mão (importer removido); rodar `pnpm install` para regenerar e limpar pacotes órfãos do Electron.
- CI (`ci.yml`) alterado e não executado: o job Windows agora roda `scripts/build-cui.ps1` e `briks_smoke`.
- Supabase real não testado pelo app nativo (só loopback/sinalização simulada).
- Sem interoperabilidade testada com o app Electron/Chromium (WebRTC entre os dois).
- Sem TURN configurado no nativo; WASAPI real (captura/reprodução) e AEC não testados.
- Dono da sala = menor `joinedAt` (falsificável); status/presença só sincronizam dentro da mesma sala (lista de amigos do backend não os recebe); audiência "Só na chamada" é gravada mas não aplicada.
- `CHECKPOINTS.md` e `docs/PLANO-1.2.0.md` mantêm referências históricas ao Electron.
