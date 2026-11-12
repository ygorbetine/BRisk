# Assistente do DONO para ligar o servidor de chamadas (SFU/LiveKit). Rode no SEU terminal, na raiz do repositorio:
#   powershell -NoProfile -File scripts/ativar-sfu.ps1 [-ProjectRef wusvizrteyoewggageeu] [-SemFuncaoKick] [-SoVerificar]
# O que faz: confere o Supabase CLI (logado + projeto), pergunta URL/API Key/API Secret (Key e Secret sem eco na tela, nunca por argumento),
# grava os segredos no Supabase (`supabase secrets set --env-file`, arquivo temporario so seu, apagado no fim: o segredo nao aparece na
# linha de comando nem em log), publica as funcoes livekit-token e livekit-kick e valida de ponta a ponta com scripts/verificar-sfu.ps1.
# Nenhum segredo vai para o app, para o git ou para a tela. Os enderecos publicos (url + token_url) ficam em livekit.json para o instalador.
param([string]$ProjectRef = "wusvizrteyoewggageeu", [switch]$SemFuncaoKick, [switch]$SoVerificar)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
function Passo($t) { Write-Host "`n== $t ==" -ForegroundColor Cyan }
function Falha($t) { Write-Host "ERRO: $t" -ForegroundColor Red; exit 1 }

Passo "1/5 Pre-requisitos"
if (-not (Get-Command supabase -ErrorAction SilentlyContinue)) { Falha "Supabase CLI nao encontrado. Instale (https://supabase.com/docs/guides/cli) e rode 'supabase login'." }
$lista = & supabase projects list 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { Falha "Nao deu para listar seus projetos: rode 'supabase login' e tente de novo." }
if ($lista -notmatch [regex]::Escape($ProjectRef)) { Falha "O projeto $ProjectRef nao aparece na sua conta do Supabase CLI (login na conta certa?)." }
Write-Host "Supabase CLI logado; projeto $ProjectRef encontrado."
foreach ($f in "livekit-token", "livekit-kick") { if (-not (Test-Path "$root\supabase\functions\$f\index.ts")) { Falha "Falta supabase/functions/$f (rode da raiz do repositorio certo)." } }

$publico = $null
if (-not $SoVerificar) {
    Passo "2/5 Dados do servidor LiveKit (a chave e o segredo NAO aparecem na tela)"
    $url = (Read-Host "URL do servidor (wss://SEU-PROJETO.livekit.cloud ou wss://seu-dominio)").Trim()
    if ($url -notmatch '^wss://[^\s/]+$') { Falha "A URL precisa comecar com wss:// e ter so o dominio (sem barra no fim)." }
    function LerSegredo($rotulo) {
        $ss = Read-Host $rotulo -AsSecureString
        $b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($ss)
        try { $v = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b) }
        $v = $v.Trim(); if (-not $v -or $v -match '\s|"|''') { Falha "$rotulo vazio ou com espaco/aspas." }; return $v
    }
    $key = LerSegredo "API Key do LiveKit"
    $secret = LerSegredo "API Secret do LiveKit"

    Passo "3/5 Gravando os segredos no Supabase"
    $envFile = Join-Path ([IO.Path]::GetTempPath()) ("briks-sfu-" + [guid]::NewGuid().ToString("N") + ".env")
    try {
        New-Item -ItemType File $envFile | Out-Null
        & icacls $envFile /inheritance:r /grant:r "$($env:USERNAME):F" | Out-Null   # so o seu usuario le
        [IO.File]::WriteAllText($envFile, "LIVEKIT_API_KEY=$key`nLIVEKIT_API_SECRET=$secret`nLIVEKIT_URL=$url`n", (New-Object Text.UTF8Encoding($false)))
        $key = $null; $secret = $null
        & supabase secrets set --env-file $envFile --project-ref $ProjectRef 2>&1 | ForEach-Object { if ($_ -notmatch 'LIVEKIT_API_(KEY|SECRET)=') { $_ } }
        if ($LASTEXITCODE -ne 0) { Falha "supabase secrets set falhou." }
    } finally {
        if (Test-Path $envFile) { [IO.File]::WriteAllText($envFile, ("x" * 256)); Remove-Item $envFile -Force }   # sobrescreve e apaga
    }

    Passo "4/5 Publicando as funcoes (JWT do gateway ligado)"
    Push-Location $root
    try {
        & supabase functions deploy livekit-token --project-ref $ProjectRef; if ($LASTEXITCODE -ne 0) { Falha "deploy da livekit-token falhou." }
        if (-not $SemFuncaoKick) { & supabase functions deploy livekit-kick --project-ref $ProjectRef; if ($LASTEXITCODE -ne 0) { Falha "deploy da livekit-kick falhou." } }
    } finally { Pop-Location }
    $publico = [ordered]@{ url = $url; token_url = "https://$ProjectRef.supabase.co/functions/v1/livekit-token" }
    $dir = Join-Path $env:APPDATA "Briks-CUI"; New-Item -ItemType Directory -Force $dir | Out-Null
    [IO.File]::WriteAllText((Join-Path $dir "livekit.json"), ($publico | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    Write-Host "Gravei $dir\livekit.json (so enderecos publicos) para o SEU app usar ja."
} else { Passo "2-4/5 Pulados (-SoVerificar)" }

Passo "5/5 Validacao de ponta a ponta"
Write-Host "Use uma conta Briks de TESTE (nao a sua principal). Enter em branco no e-mail pula as etapas com login."
$email = (Read-Host "E-mail da conta de teste").Trim()
$args2 = @(); if ($email) { $args2 += @("-Email", $email) }
if ($publico) { $args2 += @("-Url", $publico.url, "-TokenUrl", $publico.token_url) }
& powershell -NoProfile -File "$PSScriptRoot\verificar-sfu.ps1" @args2
$rc = $LASTEXITCODE

Write-Host "`nProximos passos:" -ForegroundColor Cyan
Write-Host "  Para TODOS receberem no instalador, gere-o com:"
Write-Host "    `$env:BRIKS_LIVEKIT_URL='wss://...'; `$env:BRIKS_LIVEKIT_TOKEN_URL='https://$ProjectRef.supabase.co/functions/v1/livekit-token'; powershell -File scripts/build-installer.ps1"
Write-Host "  (ou deixe o livekit.json acima e aponte BRIKS_LIVEKIT_JSON_SRC para ele). Veja docs/ativar-sfu.md."
exit $rc
