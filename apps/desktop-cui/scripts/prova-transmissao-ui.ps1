# Provas da interface da transmissão, sem backend e com a janela OCULTA (nada aparece na tela de ninguém):
#   aviso  -> TRX-10: "X começou a transmitir · Assistir" no modo Call; o clique troca para o modo Transmissão
#   zoom   -> CHM-12: zoom de quem assiste (roda do mouse, arrastar, selo de % e volta a 100%)
#   duas   -> dois monitores: duas telas da mesma pessoa, dois blocos, zoom independente
# Uso: powershell -File prova-transmissao-ui.ps1 [-Out pasta] [-Casos aviso,zoom,duas]
# Dados, layout, configurações e temporários ficam numa pasta própria, apagada no fim. Sai com código 1 se algum "expect" falhar.
param([string]$Out = "$PSScriptRoot\..\provas\transmissao-ui", [string[]]$Casos = @("aviso", "zoom", "duas", "grade", "qualidade"))
$Casos = $Casos -split ","
$exe = "$PSScriptRoot\..\build\briks_cui.exe"
$base = Join-Path $PSScriptRoot "..\build\tmp-prova-transmissao"
New-Item -ItemType Directory -Force $Out | Out-Null
$falhas = 0
foreach ($c in $Casos) {
    $w = Join-Path $base $c
    if (Test-Path $w) { Remove-Item -Recurse -Force $w }
    New-Item -ItemType Directory -Force $w, (Join-Path $w "dados"), (Join-Path $w "capturas"), (Join-Path $w "tmp") | Out-Null
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $exe).Path; $psi.WorkingDirectory = (Split-Path -Parent $psi.FileName); $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardError = $true
    $e = $psi.EnvironmentVariables
    $e["BRIKS_HIDDEN"] = "1"; $e["BRIKS_DEMO"] = "call"; $e["BRIKS_DEMO_PEERS"] = $(if ($c -eq "grade" -or $c -eq "gradefoco" -or $c -eq "gradeaudio") { "4" } else { "3" }); $e["BRIKS_DOCK"] = "default"; $e["BRIKS_WINDOW"] = "1280x800"
    $e["BRIKS_DEV_SHARE"] = $(if ($c -eq "mini") { "demo-mini" } elseif ($c -eq "previa") { "demo-self" } elseif ($c -eq "sheen") { "demo-sheen" } elseif ($c -eq "lateral") { "demo-two" } elseif ($c -eq "zoom") { "demo-zoom" } elseif ($c -eq "duas") { "demo-two" } elseif ($c -eq "gradeaudio") { "demo-audio" } elseif ($c -eq "grade" -or $c -eq "gradefoco") { "demo-many" } elseif ($c -eq "qualidade") { "demo-quality" } elseif ($c -eq "adaptando") { "demo-adapt" } else { "demo-remote" })
    $e["BRIKS_SCRIPT"] = (Join-Path $PSScriptRoot "share-$c.txt"); $e["BRIKS_SCRIPT_OUT"] = (Join-Path $w "capturas"); $e["BRIKS_SCRIPT_SHOTS"] = "1"
    if ($c -eq "emoji" -or $c -eq "emojibusca") { $e["BRIKS_DEV_OPEN"] = "profile:1"; $e["BRIKS_DEMO"] = "social" }
    if ($c -eq "foto" -or $c -eq "fotovoz") { $e["BRIKS_DEV_PHOTO"] = (Resolve-Path "$PSScriptRoot/../provas/fotos-teste/eu.png").Path; $e["BRIKS_DEV_FPHOTO"] = (Resolve-Path "$PSScriptRoot/../provas/fotos-teste/amigo.png").Path; $e["BRIKS_DEMO"] = "social"; if ($c -eq "fotovoz") { $e["BRIKS_DOCK"] = "added"; $e["BRIKS_DEV_ROOM"] = "voz-geral" } }
    if ($c -eq "voz") { $e["BRIKS_DOCK"] = "added"; $e["BRIKS_DEMO"] = "social"; $e["BRIKS_DEV_ROOM"] = "voz-geral" }
    if ($c -eq "overlay" -or $c -eq "overlayreal") { $e["BRIKS_OVERLAY_FORCE"] = "1"; $e["BRIKS_DEV_ROOM"] = "voz-geral" }
    $e["BRIKS_NO_BREATH"] = "1"; $e["BRIKS_NO_IMPORT"] = "1"; $e["BRIKS_DISABLE_UPDATE"] = "1"
    $e["BRIKS_LAYOUT_FILE"] = (Join-Path $w "layout.json"); $e["BRIKS_THEME_FILE"] = (Join-Path $w "theme.json")
    $e["BRIKS_SETTINGS_FILE"] = (Join-Path $w "settings.cfg"); $e["BRIKS_GRADIENTS_FILE"] = (Join-Path $w "grad.json")
    $e["BRIKS_CORE_DATA"] = (Join-Path $w "dados"); $e["BRIKS_CUI_DATA"] = (Join-Path $w "dados")
    $e["TEMP"] = (Join-Path $w "tmp"); $e["TMP"] = (Join-Path $w "tmp")
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(90000)) { $p.Kill(); Write-Host "${c}: passou de 90 s, encerrado"; $falhas++ }   # nunca deixa processo aberto
    elseif ($p.ExitCode -ne 0) { Write-Host "${c}: saiu com código $($p.ExitCode)"; $falhas++ }
    $err.Result -split "`n" | Where-Object { $_ -match "roteiro:" } | ForEach-Object { Write-Host "  $($_.Trim())" }
    Get-ChildItem (Join-Path $w "capturas") -Filter *.png | ForEach-Object { Copy-Item $_.FullName (Join-Path $Out $_.Name) -Force; Write-Host "capturado: $($_.Name)" }
}
Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
if ($falhas) { exit 1 }
