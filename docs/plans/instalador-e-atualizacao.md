# Instalador e atualização do app nativo (CUI)

> **Aviso:** a transmissão de tela/janela só chega com a etapa E (sessão `cui-transmissao`). Quem trocar o Electron por este instalador perde a transmissão até lá. Por isso a **atualização automática está implementada, mas DESLIGADA e sem feed ativo**.

## Instalador (NSIS, `scripts/build-installer.ps1`)
- Mesmo produto do Electron 1.2.x: appId `com.briks.calls`, nome "Briks", pasta `%LOCALAPPDATA%\Programs\@briksdesktop`, chave de desinstalação `303b4820-db60-5afe-b1ba-dc7d7a2d7acb` (derivada do appId, igual à do 1.2.2), atalhos "Briks" (Menu Iniciar e Área de Trabalho), instalação **por usuário, sem administrador**.
- Instala `Briks.exe` (é o `briks_cui.exe` em Release, com ícone; o nome antigo mantém atalhos fixados e entradas de início), `briks-core.exe`, `briks_overlay.exe`, `backend\briks-desktop-backend.exe`, runtime do VC "app-local" (`vcruntime140*.dll`, `msvcp140*.dll`), licenças, ícone e `Uninstall Briks.exe`.
- Se já existe o mesmo produto (Electron ou CUI), roda o desinstalador antigo em silêncio com `/KEEP_APP_DATA _?=<pasta>` antes de copiar. Os dados do usuário (`%APPDATA%\@briks`, `%APPDATA%\Briks-CUI`) nunca são apagados, nem pelo desinstalador novo.
- Entende os argumentos do electron-updater: `/S`, `--updated`, `--force-run` (reabre o app).
- Sem assinatura de código (não há certificado): o Windows SmartScreen pode avisar.
- Saída: `apps/desktop-cui/installer-out/Briks-Setup-<versão>.exe` e `latest.yml` (formato do electron-updater: versão, nome, SHA-512 em base64, tamanho). A versão é parâmetro do script (`-Version`, padrão 1.3.0); nenhum arquivo versionado do repositório foi alterado.
- `-Test` gera `Briks-Test-Setup-<versão>.exe` com **outro GUID, outra pasta (`@briksdesktop-test`) e outro atalho**, para testar sem tocar na instalação real.

## Como testar com segurança
1. `powershell -File scripts/build-installer.ps1 -Version 1.3.0 -Test` (e `-Version 1.3.1 -Test` para ter uma versão para atualizar).
2. Instalar: `Briks-Test-Setup-1.3.0.exe /S` → conferir `HKCU\...\Uninstall\9d0f4b1e-7c2a-5f3b-8a61-0b2c3d4e5f60` e a pasta `@briksdesktop-test`.
3. Atualizar com feed local: copiar o instalador 1.3.1 e o `latest.yml` para uma pasta, `python -m http.server 8099 --bind 127.0.0.1 --directory <pasta>`, abrir o `Briks.exe` instalado com `BRIKS_UPDATE_FEED=http://127.0.0.1:8099/latest.yml`, `BRIKS_UPDATE_DELAY_MS=300` e `BRIKS_UPDATE_DIR=<pasta temporária>`. O app baixa, confere o SHA-512, mostra o aviso e instala ao fechar.
4. Desinstalar: `"Uninstall Briks.exe" /currentuser /S _?=<pasta>`.
5. Nunca rodar o instalador **sem** `-Test` neste computador sem querer substituir o Electron 1.2.2 instalado.

## Resultado dos testes feitos (nesta máquina, só com `-Test`)
- Instalar 1.3.0 em silêncio: ok (arquivos, chave de desinstalação, atalho de teste); a instalação real do Electron (1.2.2) continuou intacta.
- `Briks.exe` Release rodou da pasta instalada (sem DLLs de depuração).
- Atualização por feed local: baixou 1.3.1, SHA-512 conferido, aviso "Briks 1.3.1 pronto para instalar" (captura), instalador silencioso ao fechar atualizou 1.3.0 → 1.3.1 (a versão no registro passou a 1.3.1).
- Feed com SHA-512 errado: instalador descartado, nada executado.
- Desinstalar: pasta, chave e atalho removidos; dados do usuário não tocados.
- Testes unitários novos no `briks_smoke` (versão, leitura do `latest.yml`, caminho/URL recusados).

## Atualização automática (`src/update.c`)
- Feed padrão: `https://github.com/ygorbetine/Briks/releases/latest/download/latest.yml` (o mesmo release que o electron-updater do 1.2.2 consome). **Não é consultado** enquanto não for ligado: precisa de `BRIKS_ENABLE_UPDATE=1`, do arquivo `%APPDATA%\Briks-CUI\update-enabled`, ou de `BRIKS_UPDATE_FEED` (testes). `BRIKS_DISABLE_UPDATE=1` desliga sempre.
- Primeira checagem ~15 s depois de abrir e a cada 4 h; só HTTPS (HTTP apenas em loopback); `path` do feed só aceita `.exe` sem `..`/`\`; limite de 600 MB; SHA-512 base64 do feed conferido antes de aceitar. O aviso (vidro, canto inferior direito) tem "Reiniciar e instalar" e "Depois"; ao fechar o app, a atualização pronta é instalada em silêncio.
- Limite: sem assinatura de código, a integridade depende do HTTPS do GitHub e do `latest.yml`; quem controlar o release controla a atualização.
- Para o Electron 1.2.2 atualizar para o CUI: publicar o instalador e o `latest.yml` com versão maior que 1.2.2 num release do GitHub. **Isso é publicação e exige autorização do usuário; nada foi publicado.**

## Migração de dados
Ver `docs/plans/migracao-cui-principal.md` e o diálogo "Importar seus dados do Briks antigo?". A chave ECDSA/peerId do Electron **não** é importada (identidade nova); amigos, grupos e mensagens locais sim.

## Riscos
- Não testado contra o desinstalador real do Electron (só contra o desinstalador do próprio instalador em `-Test`).
- Sem assinatura: aviso do SmartScreen; atualização depende da confiança no feed.
- Atualizar o Electron para o CUI hoje faz perder a transmissão de tela até a etapa E.
- Arquivos comuns tocados por esta frente: `src/main.c`, `src/briks_ui.h`, `CMakeLists.txt`, `tests/smoke.c`, `src/profile.c` (só a lista de programas ignorados); o resto é novo (`migrate.c`, `update.c`, `ui_update.c`, `briks.rc`, `scripts/build-installer.ps1`, `scripts/installer/briks.nsi`).
