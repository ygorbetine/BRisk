# Roda os 4 roteiros de modal numa instância oculta, com layout padrão fixo. -SemRegra desliga a regra central (os testes DEVEM falhar).
param([switch]$SemRegra)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$exe = Join-Path $root 'build\briks_cui.exe'
$env:BRIKS_NET_LOOPBACK_ONLY = "1"; $env:BRIKS_HIDDEN = '1'; $env:BRIKS_SCRIPT_SHOTS = '1'; $env:BRIKS_DEV_AUTH = '1'; $env:BRIKS_DEMO = 'social'; $env:BRIKS_DOCK = 'default'
if ($SemRegra) { $env:BRIKS_TEST_NO_MODAL = '1' } else { Remove-Item Env:BRIKS_TEST_NO_MODAL -ErrorAction SilentlyContinue }
$fail = 0
foreach ($t in @(@('perfil','profile','social'), @('config','settings:2','social'), @('seletor','','social'), @('contexto','','call'))) {
  $env:BRIKS_DEMO = $t[2]; if ($t[2] -eq 'call') { $env:BRIKS_DEMO_PEERS = '1' } else { Remove-Item Env:BRIKS_DEMO_PEERS -ErrorAction SilentlyContinue }
  $env:BRIKS_DEV_OPEN = $t[1]; $env:BRIKS_SCRIPT = Join-Path $PSScriptRoot ($t[0] + '.txt'); $env:BRIKS_SCRIPT_OUT = Join-Path $env:TEMP ('modal_' + $t[0])
  $p = Start-Process -FilePath $exe -PassThru -Wait -NoNewWindow -RedirectStandardError (Join-Path $env:TEMP 'modal_err.txt')
  $out = Get-Content (Join-Path $env:TEMP 'modal_err.txt') | Where-Object { $_ -match 'roteiro' }
  $out | ForEach-Object { "[$($t[0])] $_" }
  if ($p.ExitCode -ne 0) { $fail++ }
}
if ($SemRegra) { if ($fail -gt 0) { "OK: sem a regra central os testes falham ($fail de 4), como esperado"; exit 0 } else { 'ERRO: os testes passaram sem a regra'; exit 1 } }
if ($fail) { "FALHOU: $fail roteiro(s)"; exit 1 } else { 'OK: nenhum clique chegou às camadas de baixo (4 roteiros)'; exit 0 }
