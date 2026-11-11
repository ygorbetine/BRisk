# Matriz de verificação: 3 tamanhos x 3 modos de desempenho x 5 layouts + 13 seções de Configurações + Perfil.
# Uso: matriz.ps1 -Out <pasta> [-Limpo]   (-Limpo: perfil, layout e configurações novos em pasta temporária)
param([Parameter(Mandatory)][string]$Out, [switch]$Limpo)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $root 'build\briks_cui.exe'
$tmp = Join-Path $env:TEMP ('briks_matriz_' + [guid]::NewGuid().ToString('N').Substring(0,8)); New-Item -ItemType Directory -Force $tmp | Out-Null
$env:BRIKS_NET_LOOPBACK_ONLY="1"; $env:BRIKS_HIDDEN='1'; $env:BRIKS_SCRIPT_SHOTS='1'; $env:BRIKS_DEV_AUTH='1'; $env:BRIKS_DEMO='social'; $env:BRIKS_DEMO_FTAB='0'
if ($Limpo) { $env:BRIKS_PROFILE_FILE = Join-Path $tmp 'perfil.json'; $env:BRIKS_SETTINGS_FILE = Join-Path $tmp 'settings.json' }
$sizes = '2000x900','1280x800','800x600'; $perfs = 'normal','simples','minimo'; $docks = 'default','stacked','moved','added','unico'
$log = Join-Path $Out 'execucao.txt'; New-Item -ItemType Directory -Force $Out | Out-Null; '' | Set-Content $log
function Run($name, $script, $w, $extra) {
  $d = Join-Path $Out $name; New-Item -ItemType Directory -Force $d | Out-Null
  $env:BRIKS_WINDOW = $w; $env:BRIKS_SCRIPT = $script; $env:BRIKS_SCRIPT_OUT = $d
  foreach ($k in $extra.Keys) { Set-Item -Path "Env:$k" -Value $extra[$k] }
  $p = Start-Process -FilePath $exe -PassThru -NoNewWindow -RedirectStandardError (Join-Path $tmp 'err.txt')
  if (-not $p.WaitForExit(90000)) { $p.Kill(); "$name TRAVOU (90 s)" | Add-Content $log }
  elseif ($p.ExitCode -ne 0) { "$name saiu com código $($p.ExitCode)" | Add-Content $log } else { "$name ok" | Add-Content $log }
  foreach ($k in $extra.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
}
$cena = Join-Path $PSScriptRoot 'matriz-cena.txt'
foreach ($w in $sizes) { foreach ($pf in $perfs) { foreach ($dk in $docks) {
  $ex = @{ BRIKS_PERF = $pf }
  if ($dk -eq 'unico') { $lf = Join-Path $tmp 'unico.json'; '{"p":"chat"}' | Set-Content $lf; $ex.BRIKS_LAYOUT_FILE = $lf; Remove-Item Env:BRIKS_DOCK -ErrorAction SilentlyContinue } else { $env:BRIKS_DOCK = $dk }
  Run "matriz_${w}_${pf}_${dk}" $cena $w $ex
}}}
$one = Join-Path $tmp 'uma.txt'; "wait 14`nshot x" | Set-Content $one
Remove-Item Env:BRIKS_DOCK -ErrorAction SilentlyContinue; $env:BRIKS_DOCK = 'default'; Remove-Item Env:BRIKS_DEMO_FTAB -ErrorAction SilentlyContinue
foreach ($w in $sizes) {
  for ($i = 0; $i -lt 13; $i++) { Run "config_${w}_$i" $one $w @{ BRIKS_DEV_OPEN = "settings:$i" } }
  Run "perfil_$w" $one $w @{ BRIKS_DEV_OPEN = 'profile' }
}
"fim: $Out ; temp: $tmp" | Add-Content $log
