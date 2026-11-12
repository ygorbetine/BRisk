# 1.4.1 (item 20): ao encerrar o Briks nao pode sobrar briks-core / briks_overlay / backend. Abre o app (oculto, loopback, dados proprios),
# encerra de 3 jeitos (script termina = saida normal; fechar a janela; matar o app a forca) e confere que os filhos DESTA execucao sumiram.
# So olha processos cujo pai e o app que este teste abriu (nunca mexe em outras instalacoes). Uso: powershell -File scripts/teste-encerramento.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'apps\desktop-cui\build\briks_cui.exe'
$tmp = Join-Path $root 'apps\desktop-cui\build\_ci\enc'
$falhas = 0
function Filhos([int]$ppid) { @(Get-CimInstance Win32_Process | Where-Object { $_.ParentProcessId -eq $ppid }) }
function Descend([int]$ppid) { $r = @(); foreach ($c in (Filhos $ppid)) { $r += $c; $r += (Descend $c.ProcessId) }; $r }
foreach ($modo in 'saida-normal', 'fechar-janela', 'matar-app') {
    $w = Join-Path $tmp $modo; if (Test-Path $w) { Remove-Item -Recurse -Force $w }
    New-Item -ItemType Directory -Force "$w\dados" | Out-Null
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe; $psi.WorkingDirectory = Split-Path $exe; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $e = $psi.EnvironmentVariables
    $e['BRIKS_NET_LOOPBACK_ONLY'] = '1'; $e['BRIKS_HIDDEN'] = '1'; $e['BRIKS_DEMO'] = 'call'; $e['BRIKS_NO_IMPORT'] = '1'; $e['BRIKS_DISABLE_UPDATE'] = '1'
    $e['BRIKS_CORE_DATA'] = "$w\dados"; $e['BRIKS_CUI_DATA'] = "$w\dados"; $e['BRIKS_LAYOUT_FILE'] = "$w\layout.json"; $e['BRIKS_SETTINGS_FILE'] = "$w\settings.cfg"; $e['BRIKS_THEME_FILE'] = "$w\theme.json"
    if ($modo -eq 'saida-normal') { Set-Content "$w\r.txt" "waitms 4000" -Encoding ASCII; $e['BRIKS_SCRIPT'] = "$w\r.txt" } else { Set-Content "$w\r.txt" "waitms 60000" -Encoding ASCII; $e['BRIKS_SCRIPT'] = "$w\r.txt" }
    $p = [Diagnostics.Process]::Start($psi)
    $vistos = @(); $fim = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $fim -and -not ($vistos | Where-Object { $_.Name -like 'briks-core*' -or $_.Name -like 'briks_core*' })) { Start-Sleep -Milliseconds 300; $vistos = @(Descend $p.Id) }
    if (-not $vistos.Count) { Write-Host "$modo : o core nao subiu (teste invalido)"; $falhas++; try { $p.Kill() } catch {}; continue }
    $ids = @($vistos | ForEach-Object { $_.ProcessId })
    switch ($modo) {
        'saida-normal' { [void]$p.WaitForExit(30000) }
        'fechar-janela' { Start-Sleep -Seconds 1; [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(8000)) { Write-Host "$modo : a janela oculta nao fechou; encerrando o app" ; $p.Kill() } }
        'matar-app' { Start-Sleep -Seconds 1; Stop-Process -Id $p.Id -Force }
    }
    Start-Sleep -Seconds 3
    $vivos = @($ids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    if ($vivos.Count) { Write-Host "FALHOU $modo : sobraram PIDs $($vivos -join ',')"; $falhas++; foreach ($v in $vivos) { Stop-Process -Id $v -Force -ErrorAction SilentlyContinue } }
    else { Write-Host "ok $modo : nenhum filho vivo (eram $($ids.Count): $(($vistos | ForEach-Object Name) -join ', '))" }
}
if ($falhas) { Write-Host "$falhas falha(s)"; exit 1 } else { Write-Host 'encerramento: tudo limpo'; exit 0 }
