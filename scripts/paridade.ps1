# Atalho para o roteiro de paridade Electron x CUI (ver docs/plans/paridade-electron-cui.md).
# Uso: powershell -NoProfile -File scripts\paridade.ps1 [-Saida pasta] [-Filtro regex] [-SoInventario]
& (Join-Path $PSScriptRoot "..\apps\desktop-cui\scripts\paridade.ps1") @args
exit $LASTEXITCODE
