# Prova do Firewall (primeira chamada) SEM tocar no firewall real: BRIKS_NETSH aponta para um netsh falso que so registra os argumentos.
# Roda os roteiros ocultos (BRIKS_HIDDEN) e confere os argumentos registrados. Uso: powershell -NoProfile -File rodar.ps1 [-Exe caminho]
param([string]$Exe = "")
$ErrorActionPreference = 'Continue'
$cui = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
if (-not $Exe) { $Exe = Join-Path $cui 'build\briks_cui.exe' }
$w = Join-Path $env:TEMP ("briks-fw-" + [guid]::NewGuid().ToString("N").Substring(0, 8)); New-Item -ItemType Directory -Force $w | Out-Null
$fake = Join-Path $w 'fakenetsh.cmd'
Set-Content $fake "@echo off`r`necho %*>>`"%BRIKS_FAKE_LOG%`"`r`nif `"%3`"==`"show`" exit /b 1`r`nexit /b 0" -Encoding ascii
$fail = 0
function Caso($nome, $esperaLog) {
    $log = Join-Path $w "$nome.log"; Remove-Item $log -ErrorAction SilentlyContinue
    $env:BRIKS_HIDDEN = '1'; $env:BRIKS_SCRIPT_SHOTS = '1'; $env:BRIKS_DEV_AUTH = '1'; $env:BRIKS_DEMO = 'social'; $env:BRIKS_DOCK = 'default'
    $env:BRIKS_NETSH = $fake; $env:BRIKS_FAKE_LOG = $log; $env:BRIKS_BACKEND_EXE = 'C:\fake\briks-core.exe'
    $env:BRIKS_SCRIPT = Join-Path $PSScriptRoot "$nome.txt"; $env:BRIKS_SCRIPT_OUT = Join-Path $w $nome; New-Item -ItemType Directory -Force $env:BRIKS_SCRIPT_OUT | Out-Null
    $p = Start-Process -FilePath $Exe -PassThru -Wait -NoNewWindow -RedirectStandardError (Join-Path $w "$nome.err")
    $ok = ($p.ExitCode -eq 0)
    $txt = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
    foreach ($rx in $esperaLog) { if ($txt -notmatch $rx) { $ok = $false; Write-Host "  falta no log: $rx" } }
    if ($nome -ne 'liberar' -and $txt -match 'add rule') { $ok = $false; Write-Host "  criou regra sem pedir" }
    Write-Host ("[{0}] {1}  exit={2}" -f $(if ($ok) { 'OK' } else { 'FALHOU' }), $nome, $p.ExitCode)
    if (-not $ok) { $script:fail++; Get-Content (Join-Path $w "$nome.err") | Select-Object -Last 5; Write-Host $txt }
}
Caso 'liberar' @('show rule name="Briks"', 'delete rule name="Briks"', 'delete rule name="Briks core"',
    'add rule name="Briks" dir=in action=allow program=".+" enable=yes profile=private,public',
    'add rule name="Briks core" dir=in action=allow program="C:\\fake\\briks-core.exe" enable=yes profile=private,public')
Caso 'agora-nao' @('show rule name="Briks"')
Caso 'nunca' @('show rule name="Briks"')
Remove-Item $w -Recurse -Force -ErrorAction SilentlyContinue
if ($fail) { "FALHOU: $fail caso(s)"; exit 1 } else { 'OK: dialogo e argumentos do netsh (netsh falso, firewall real intacto)'; exit 0 }
