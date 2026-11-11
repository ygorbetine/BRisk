# Capturas OCULTAS (nunca o monitor) da interface em 3 tamanhos de janela x 3 modos de desempenho x tamanhos de texto.
# Uso: powershell -NoProfile -File scripts\capturas-acessib.ps1 [-Out pasta] [-Demos social,msgs,call,login] [-Tamanhos 1280x800,900x600,560x480]
#        [-Perfs normal,simplificado,minimo] [-Texto 1,3] [-Abrir settings:0] [-Env "BRIKS_X=1"]
# Cada captura vem do próprio app (CUI_SCREENSHOT), com dados, layout e configurações em pasta temporária.
param(
    [string]$Out = (Join-Path $env:TEMP "briks-acessib"),
    [string[]]$Demos = @("social", "msgs", "call", "login"),
    [string[]]$Tamanhos = @("1280x800", "900x600", "560x480"),
    [string[]]$Perfs = @("normal", "simplificado", "minimo"),
    [string[]]$Texto = @("1"),
    [string]$Abrir = "",
    [string]$Prefixo = "",
    [string[]]$Env = @()
)
$ErrorActionPreference = "Stop"
# com -File o PowerShell não separa "a,b" em lista: separa aqui
$Demos = @($Demos | ForEach-Object { $_ -split "," }); $Tamanhos = @($Tamanhos | ForEach-Object { $_ -split "," }); $Perfs = @($Perfs | ForEach-Object { $_ -split "," }); $Texto = @($Texto | ForEach-Object { $_ -split "," }); $Env = @($Env | ForEach-Object { $_ -split "," })
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\briks_cui.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
$tmp = Join-Path $env:TEMP "briks-acessib-dados"
foreach ($d in $Demos) { foreach ($t in $Tamanhos) { foreach ($p in $Perfs) { foreach ($x in $Texto) {
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $nome = "$Prefixo$d-$t-$p-t$x" + $(if ($Abrir) { "-" + ($Abrir -replace ":", "") } else { "" })
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe; $psi.WorkingDirectory = (Split-Path $exe); $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $e = $psi.EnvironmentVariables
    $e["BRIKS_NET_LOOPBACK_ONLY"] = "1"; $e["BRIKS_HIDDEN"] = "1"; $e["BRIKS_DEMO"] = $d; $e["BRIKS_DOCK"] = "default"; $e["BRIKS_WINDOW"] = $t
    $e["BRIKS_PERF"] = $p; $e["BRIKS_TEXT_SIZE"] = "$x"
    $e["CUI_SCREENSHOT"] = (Join-Path $Out "$nome.png"); $e["CUI_SCREENSHOT_FRAMES"] = "30"
    $e["BRIKS_NO_BREATH"] = "1"; $e["BRIKS_NO_IMPORT"] = "1"; $e["BRIKS_DISABLE_UPDATE"] = "1"
    $e["BRIKS_LAYOUT_FILE"] = (Join-Path $tmp "layout.json"); $e["BRIKS_THEME_FILE"] = (Join-Path $tmp "theme.json")
    $e["BRIKS_SETTINGS_FILE"] = (Join-Path $tmp "settings.cfg"); $e["BRIKS_GRADIENTS_FILE"] = (Join-Path $tmp "grad.json")
    $e["BRIKS_CORE_DATA"] = $tmp; $e["BRIKS_CUI_DATA"] = $tmp
    if ($Abrir) { $e["BRIKS_DEV_OPEN"] = $Abrir }
    foreach ($kv in $Env) { $i = $kv.IndexOf("="); if ($i -gt 0) { $e[$kv.Substring(0, $i)] = $kv.Substring($i + 1) } }
    $pr = [Diagnostics.Process]::Start($psi)
    if (-not $pr.WaitForExit(60000)) { try { $pr.Kill() } catch {}; Write-Host "$nome : tempo esgotado" }
    elseif ($pr.ExitCode -ne 0) { Write-Host "$nome : saiu com código $($pr.ExitCode)" }
    else { Write-Host "$nome ok" }
} } } }
Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith((Split-Path $exe), [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { try { $_.Kill() } catch {} }
