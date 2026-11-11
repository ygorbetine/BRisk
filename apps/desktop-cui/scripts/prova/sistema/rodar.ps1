# Provas do pacote "Janela e sistema" (bandeja, fechar para a bandeja, instância única, estado da janela, avisos, sons, não lidas, autostart, links).
# Tudo OCULTO (BRIKS_HIDDEN=1): nenhuma janela e nenhum aviso aparecem para ninguém, nenhum som toca; dados/layout/config em pasta temporária;
# o ícone da bandeja só existe nos casos "bandeja-*"/"badge" (BRIKS_TRAY_TEST=1), por alguns segundos, e é removido ao sair; o autostart só mexe numa chave
# de teste em HKCU\Software\BriksTeste (apagada no fim), nunca na chave Run real.
# Uso: powershell -NoProfile -File rodar.ps1 [-Filtro regex] [-Exe caminho]
param([string]$Filtro = "", [string]$Exe = "")
$ErrorActionPreference = "Continue"
$cui = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
if (-not $Exe) { $Exe = Join-Path $cui "build\briks_cui.exe" }
$buildDir = Split-Path -Parent $Exe
$raiz = Join-Path $env:TEMP ("briks-sistema-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $raiz | Out-Null
$runReal = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -ErrorAction SilentlyContinue
$runAntes = if ($runReal) { ($runReal.PSObject.Properties | Where-Object { $_.Name -notlike "PS*" } | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join "|" } else { "" }
$appdata = Join-Path $env:APPDATA "Briks-CUI"
$appAntes = @{}; if (Test-Path $appdata) { Get-ChildItem $appdata -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $appAntes[$_.FullName] = $_.LastWriteTimeUtc.Ticks } }
$falhas = 0; $resumo = @()

function Novo-Psi($w, $roteiro, $extra) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.WorkingDirectory = $buildDir; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardError = $true
    $e = $psi.EnvironmentVariables
    $e["BRIKS_HIDDEN"] = "1"; $e["BRIKS_DEMO"] = "social"; $e["BRIKS_DOCK"] = "default"; $e["BRIKS_WINDOW"] = "1280x800"
    $e["BRIKS_SCRIPT_SHOTS"] = "1"; $e["BRIKS_NO_BREATH"] = "1"; $e["BRIKS_NO_IMPORT"] = "1"; $e["BRIKS_DISABLE_UPDATE"] = "1"
    $e["BRIKS_LAYOUT_FILE"] = (Join-Path $w "layout.json"); $e["BRIKS_THEME_FILE"] = (Join-Path $w "theme.json")
    $e["BRIKS_SETTINGS_FILE"] = (Join-Path $w "settings.cfg"); $e["BRIKS_GRADIENTS_FILE"] = (Join-Path $w "grad.json")
    $e["BRIKS_CORE_DATA"] = (Join-Path $w "dados"); $e["BRIKS_CUI_DATA"] = (Join-Path $w "dados")
    $e["BRIKS_SCRIPT_OUT"] = (Join-Path $w "capturas"); $e["BRIKS_NOTIFY_GRACE"] = "0"
    if ($roteiro) { $e["BRIKS_SCRIPT"] = $roteiro }
    foreach ($k in $extra.Keys) { $e[$k] = [string]$extra[$k] }
    return $psi
}
function Stop-Filhos { Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($buildDir, [StringComparison]::OrdinalIgnoreCase) -and $_.ProcessName -like "briks*" } | ForEach-Object { try { $_.Kill() } catch {} } }

# roda um roteiro; devolve @{ saida; ok; falhou; err }
function Roda($nome, $arquivo, $extra, $texto = $null, $timeout = 60) {
    $w = Join-Path $raiz $nome; New-Item -ItemType Directory -Force $w, (Join-Path $w "dados"), (Join-Path $w "capturas") | Out-Null
    $scr = Join-Path $w "roteiro.txt"
    if ($texto) { [IO.File]::WriteAllText($scr, $texto, (New-Object Text.UTF8Encoding($false))) } else { Copy-Item (Join-Path $PSScriptRoot $arquivo) $scr }
    $psi = Novo-Psi $w $scr $extra
    $p = [Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $ok = $p.WaitForExit($timeout * 1000)
    if (-not $ok) { try { $p.Kill() } catch {}; $p.WaitForExit(3000) | Out-Null }
    $err = $errTask.Result
    $linhas = @($err -split "`r?`n" | Where-Object { $_ -match "roteiro" })
    return @{ w = $w; code = $(if ($ok) { $p.ExitCode } else { -1 }); ok = @($linhas | Where-Object { $_ -match "roteiro: ok" }).Count; falhou = @($linhas | Where-Object { $_ -match "FALHOU" }); err = $err; esgotou = (-not $ok); flags = $w }
}
function Conta-Expect($arquivo) { @(Get-Content (Join-Path $PSScriptRoot $arquivo) | Where-Object { $_ -match "^expect " }).Count }
function Relata($nome, $cond, $detalhe) {
    $script:resumo += [pscustomobject]@{ caso = $nome; ok = $cond; detalhe = $detalhe }
    if ($cond) { Write-Host ("[OK]     {0}  {1}" -f $nome, $detalhe) } else { Write-Host ("[FALHOU] {0}  {1}" -f $nome, $detalhe) -ForegroundColor Red; $script:falhas++ }
}
function Caso-Expects($nome, $arquivo, $extra, [switch]$SemFlagFim, [switch]$SaidaAntes) {
    if ($Filtro -and $nome -notmatch $Filtro) { return $null }
    $r = Roda $nome $arquivo $extra
    $esp = Conta-Expect $arquivo
    $flag = Test-Path (Join-Path $r.w "capturas\fim.flag")
    $cond = (-not $r.esgotou) -and $r.code -eq 0 -and $r.falhou.Count -eq 0
    if ($SemFlagFim) { $cond = $cond -and (-not $flag) } else { $cond = $cond -and ($r.ok -eq $esp) }
    $det = "saída=$($r.code) expect ok=$($r.ok)/$esp" + $(if ($r.esgotou) { " (tempo esgotado)" } else { "" }) + $(if ($r.falhou.Count) { " " + ($r.falhou -join " ; ") } else { "" }) + $(if ($SemFlagFim) { " encerrou-antes-do-fim=" + (-not $flag) } else { "" })
    Relata $nome $cond $det
    return $r
}

Write-Host "== Janela e sistema: provas (ocultas) =="
# ---- bandeja ----
$cfgB = @{ BRIKS_TRAY_TEST = "1"; BRIKS_SYS_FILE = $null }
$w1 = Join-Path $raiz "bandeja-fecha"; New-Item -ItemType Directory -Force $w1 | Out-Null
$r = Caso-Expects "bandeja-fecha" "bandeja-fecha.txt" @{ BRIKS_TRAY_TEST = "1"; BRIKS_SYS_FILE = (Join-Path $w1 "sistema.cfg") } -SemFlagFim
if ($r) { $c = if (Test-Path (Join-Path $w1 "sistema.cfg")) { Get-Content (Join-Path $w1 "sistema.cfg") -Raw } else { "" }; Relata "bandeja-fecha/cfg" ($c -match "tray_hint=1" -and $c -match "close_to_tray=0") "aviso da primeira vez e opção gravados em sistema.cfg" }
$w2 = Join-Path $raiz "bandeja-sair"; New-Item -ItemType Directory -Force $w2 | Out-Null
$null = Caso-Expects "bandeja-sair" "bandeja-sair.txt" @{ BRIKS_TRAY_TEST = "1"; BRIKS_SYS_FILE = (Join-Path $w2 "sistema.cfg") } -SemFlagFim
$null = Caso-Expects "bandeja-silenciar" "bandeja-silenciar.txt" @{}
$w3 = Join-Path $raiz "bandeja-reiniciar"; New-Item -ItemType Directory -Force $w3 | Out-Null
$rr = Caso-Expects "bandeja-reiniciar" "bandeja-reiniciar.txt" @{ BRIKS_SYS_RESTART_DRY = (Join-Path $w3 "cmd.txt") } -SemFlagFim
if ($rr) { $cmd = if (Test-Path (Join-Path $w3 "cmd.txt")) { Get-Content (Join-Path $w3 "cmd.txt") -Raw } else { "" }; Relata "bandeja-reiniciar/cmd" ($cmd -match 'briks_cui\.exe" --aguardar=\d+') "comando de reinício: $cmd" }
$null = Caso-Expects "badge-bandeja" "badge-bandeja.txt" @{ BRIKS_TRAY_TEST = "1" }
# ---- avisos ----
$null = Caso-Expects "avisos" "avisos.txt" @{}
$null = Caso-Expects "avisos-clique" "avisos-clique.txt" @{}
$null = Caso-Expects "avisos-espera" "avisos-espera.txt" @{ BRIKS_NOTIFY_GRACE = "60000" }
$null = Caso-Expects "links" "links.txt" @{}
$null = Caso-Expects "link-briks" "link.txt" @{}
# ---- autostart (chave de teste) ----
if (-not $Filtro -or "autostart" -match $Filtro) {
    $chave = "Software\BriksTeste\Run-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
    $w = Join-Path $raiz "autostart"; New-Item -ItemType Directory -Force $w, (Join-Path $w "capturas"), (Join-Path $w "dados") | Out-Null
    $scr = Join-Path $w "roteiro.txt"; Copy-Item (Join-Path $PSScriptRoot "autostart.txt") $scr
    $psi = Novo-Psi $w $scr @{ BRIKS_AUTOSTART_KEY = $chave }
    $p = [Diagnostics.Process]::Start($psi); $et = $p.StandardError.ReadToEndAsync()
    $flag = Join-Path $w "capturas\reg.flag"; $t0 = Get-Date
    while (-not (Test-Path $flag) -and ((Get-Date) - $t0).TotalSeconds -lt 40 -and -not $p.HasExited) { Start-Sleep -Milliseconds 150 }
    $v = $null; try { $v = (Get-ItemProperty "HKCU:\$chave" -ErrorAction Stop).Briks } catch {}
    Relata "autostart/registro" ($v -match '^".*briks_cui\.exe" --minimizado$') "valor na chave de teste: $v"
    $p.WaitForExit(40000) | Out-Null; if (-not $p.HasExited) { try { $p.Kill() } catch {} }
    $err = $et.Result; $linhas = @($err -split "`r?`n" | Where-Object { $_ -match "roteiro" })
    $apagou = $true; try { Get-ItemProperty "HKCU:\$chave" -ErrorAction Stop | Out-Null; $v2 = (Get-ItemProperty "HKCU:\$chave").Briks; $apagou = -not $v2 } catch {}
    Relata "autostart" ($p.ExitCode -eq 0 -and @($linhas | Where-Object { $_ -match "FALHOU" }).Count -eq 0 -and @($linhas | Where-Object { $_ -match "roteiro: ok" }).Count -eq (Conta-Expect "autostart.txt") -and $apagou) "saída=$($p.ExitCode) expect ok=$(@($linhas | Where-Object { $_ -match 'roteiro: ok' }).Count) e valor removido=$apagou"
    Remove-Item "HKCU:\Software\BriksTeste" -Recurse -ErrorAction SilentlyContinue
    $null = Caso-Expects "autostart-recusa" "autostart-recusa.txt" @{}
}
# ---- instância única ----
if (-not $Filtro -or "instancia" -match $Filtro) {
    $id = "teste-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
    $w = Join-Path $raiz "instancia"; New-Item -ItemType Directory -Force $w, (Join-Path $w "capturas"), (Join-Path $w "dados") | Out-Null
    $scr = Join-Path $w "roteiro.txt"; Copy-Item (Join-Path $PSScriptRoot "instancia.txt") $scr
    $pa = [Diagnostics.Process]::Start((Novo-Psi $w $scr @{ BRIKS_INSTANCE_ID = $id })); $ea = $pa.StandardError.ReadToEndAsync()
    $flag = Join-Path $w "capturas\pronto.flag"; $t0 = Get-Date
    while (-not (Test-Path $flag) -and ((Get-Date) - $t0).TotalSeconds -lt 40 -and -not $pa.HasExited) { Start-Sleep -Milliseconds 150 }
    $w2 = Join-Path $raiz "instancia-b"; New-Item -ItemType Directory -Force $w2, (Join-Path $w2 "capturas"), (Join-Path $w2 "dados") | Out-Null
    $t1 = Get-Date; $pb = [Diagnostics.Process]::Start((Novo-Psi $w2 $null @{ BRIKS_INSTANCE_ID = $id })); $eb = $pb.StandardError.ReadToEndAsync()
    $saiu = $pb.WaitForExit(8000); $dur = ((Get-Date) - $t1).TotalSeconds; if (-not $saiu) { try { $pb.Kill() } catch {} }
    Relata "instancia/segunda-sai" ($saiu -and $pb.ExitCode -eq 0) ("a segunda abertura saiu sozinha em {0:N1} s com código {1}" -f $dur, $(if ($saiu) { $pb.ExitCode } else { "(não saiu)" }))
    $pa.WaitForExit(40000) | Out-Null; if (-not $pa.HasExited) { try { $pa.Kill() } catch {} }
    $linhas = @($ea.Result -split "`r?`n" | Where-Object { $_ -match "roteiro" })
    Relata "instancia/primeira-recebe" ($pa.ExitCode -eq 0 -and @($linhas | Where-Object { $_ -match "FALHOU" }).Count -eq 0 -and @($linhas | Where-Object { $_ -match "roteiro: ok" }).Count -eq (Conta-Expect "instancia.txt")) "a primeira instância foi avisada (shown=1), saída=$($pa.ExitCode)"
    # sem BRIKS_INSTANCE_ID (modo automação) duas execuções não disputam o mutex: ambas sobem
}
# ---- janela: salvar, restaurar, monitor sumido, arquivo estragado ----
if (-not $Filtro -or "janela" -match $Filtro) {
    $w = Join-Path $raiz "janela"; New-Item -ItemType Directory -Force $w | Out-Null; $cfg = Join-Path $w "sistema.cfg"
    $env1 = @{ BRIKS_SYS_FILE = $cfg }; $extraSemJanela = $env1
    # o runner fixa BRIKS_WINDOW; aqui precisamos do caminho de restauração, então remove essa variável
    function Roda-Janela($nome, $texto) {
        $ww = Join-Path $raiz $nome; New-Item -ItemType Directory -Force $ww, (Join-Path $ww "dados"), (Join-Path $ww "capturas") | Out-Null
        $scr = Join-Path $ww "roteiro.txt"; [IO.File]::WriteAllText($scr, $texto, (New-Object Text.UTF8Encoding($false)))
        $psi = Novo-Psi $ww $scr @{ BRIKS_SYS_FILE = $cfg }; $psi.EnvironmentVariables.Remove("BRIKS_WINDOW")
        $p = [Diagnostics.Process]::Start($psi); $et = $p.StandardError.ReadToEndAsync(); $ok = $p.WaitForExit(60000); if (-not $ok) { try { $p.Kill() } catch {} }
        $ls = @($et.Result -split "`r?`n" | Where-Object { $_ -match "roteiro" })
        return @{ code = $(if ($ok) { $p.ExitCode } else { -1 }); ok = @($ls | Where-Object { $_ -match "roteiro: ok" }).Count; falhou = @($ls | Where-Object { $_ -match "FALHOU" }); err = $et.Result }
    }
    function Le-Cfg { $h = @{}; Get-Content $cfg | ForEach-Object { $k, $v = $_ -split "=", 2; $h[$k] = $v }; $h }
    $r1 = Roda-Janela "janela-1" (Get-Content (Join-Path $PSScriptRoot "janela-1.txt") -Raw)
    $c = if (Test-Path $cfg) { Le-Cfg } else { @{} }
    Relata "janela/salva" ($r1.code -eq 0 -and $c["win_valid"] -eq "1" -and [int]$c["win_w"] -gt 400 -and [int]$c["win_h"] -gt 300) "saída=$($r1.code) sistema.cfg: x=$($c['win_x']) y=$($c['win_y']) w=$($c['win_w']) h=$($c['win_h']) max=$($c['win_max'])"
    $t2 = "waitms 600`nexpect restored 0`nexpect wnd_x $($c['win_x'])`nexpect wnd_y $($c['win_y'])`nexpect wnd_w $($c['win_w'])`nexpect wnd_h $($c['win_h'])`nexpect wnd_inside 1`n"
    $r2 = Roda-Janela "janela-2" $t2
    Relata "janela/restaura" ($r2.code -eq 0 -and $r2.ok -eq 6 -and $r2.falhou.Count -eq 0) "segunda abertura volta na mesma posição e tamanho: expect ok=$($r2.ok)/6 $($r2.falhou -join ' ; ')"
    # monitor que sumiu / fora da tela / maior que o monitor
    foreach ($caso in @(@("fora-esquerda", -32000, -32000, 1280, 800), @("fora-direita", 40000, 100, 1280, 800), @("gigante", 0, 0, 9000, 9000), @("monitor-extra-sumiu", 2200, 120, 1000, 700))) {
        [IO.File]::WriteAllText($cfg, "win_x=$($caso[1])`nwin_y=$($caso[2])`nwin_w=$($caso[3])`nwin_h=$($caso[4])`nwin_max=0`nwin_valid=1`n")
        $r3 = Roda-Janela ("janela-" + $caso[0]) "waitms 600`nexpect restored 0`nexpect wnd_inside 1`n"
        Relata ("janela/" + $caso[0]) ($r3.code -eq 0 -and $r3.ok -eq 2 -and $r3.falhou.Count -eq 0) "salvo ($($caso[1]),$($caso[2]) $($caso[3])x$($caso[4])) volta inteiro dentro do monitor: expect ok=$($r3.ok)/2 $($r3.falhou -join ' ; ')"
    }
    [IO.File]::WriteAllText($cfg, "lixo sem igual`nwin_w=abc`nwin_valid=1`n")
    $r4 = Roda-Janela "janela-estragado" "waitms 600`nexpect restored -1`nexpect wnd_inside 1`n"
    Relata "janela/arquivo-estragado" ($r4.code -eq 0 -and $r4.ok -eq 2) "arquivo ilegível: abre no padrão, sem travar (ok=$($r4.ok)/2)"
}

Stop-Filhos
# integridade: nada mudou no AppData real, a chave Run real continua igual, nenhum processo ficou
$mudou = @(); if (Test-Path $appdata) { Get-ChildItem $appdata -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { if (-not $appAntes.ContainsKey($_.FullName) -or $appAntes[$_.FullName] -ne $_.LastWriteTimeUtc.Ticks) { $mudou += $_.FullName } } }
$runReal2 = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -ErrorAction SilentlyContinue
$runDepois = if ($runReal2) { ($runReal2.PSObject.Properties | Where-Object { $_.Name -notlike "PS*" } | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join "|" } else { "" }
Relata "integridade/runreal" ($runAntes -eq $runDepois) "chave Run real do usuário inalterada"
Relata "integridade/appdata" ($mudou.Count -eq 0) ("AppData\Briks-CUI inalterado" + $(if ($mudou.Count) { ": " + ($mudou -join ", ") } else { "" }))
$sobra = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($buildDir, [StringComparison]::OrdinalIgnoreCase) -and $_.ProcessName -like "briks_cui*" })
Relata "integridade/processos" ($sobra.Count -eq 0) "nenhum briks_cui.exe ficou rodando"
Remove-Item $raiz -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ""
if ($falhas) { Write-Host "FALHOU: $falhas verificação(ões)" -ForegroundColor Red; exit 1 } else { Write-Host ("OK: {0} verificações" -f $resumo.Count); exit 0 }
