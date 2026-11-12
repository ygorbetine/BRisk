# Verifica, de fora e SEM segredos de servidor, se o servidor de chamadas (SFU/LiveKit) esta de pe. Diz OK / FALHA / PULADO por etapa.
# Usa so o que e publico: livekit.json (url + token_url) e supabase.json (url + chave publishable/anon), mais um login de TESTE
# (conta Briks comum que voce criou so para isso). A senha vem de BRIKS_TEST_PASSWORD ou de um prompt mascarado; nunca de argumento.
# Uso:  powershell -NoProfile -File scripts/verificar-sfu.ps1 [-LivekitJson caminho] [-SupabaseJson caminho] [-Email conta@teste]
#       Sem login de teste (sem -Email nem BRIKS_TEST_EMAIL) as etapas autenticadas ficam PULADAS.
# Saida: codigo 0 se nenhuma etapa FALHOU; 1 caso contrario. Nao imprime token, senha nem chave.
param([string]$LivekitJson = "", [string]$SupabaseJson = "", [string]$Email = "", [string]$Url = "", [string]$TokenUrl = "", [string]$SupabaseUrl = "", [string]$AnonKey = "")
$ErrorActionPreference = "Stop"
$falhas = 0
function Etapa($nome, $estado, $det = "") {
    $cor = switch ($estado) { "OK" { "Green" } "FALHA" { "Red" } default { "Yellow" } }
    Write-Host ("[{0,-6}] {1}{2}" -f $estado, $nome, $(if ($det) { " - $det" } else { "" })) -ForegroundColor $cor
    if ($estado -eq "FALHA") { $script:falhas++ }
}
function Achar($nome) {   # pasta de dados do usuario, depois ao lado do exe instalado
    foreach ($p in (Join-Path $env:APPDATA "Briks-CUI\$nome"), (Join-Path $env:LOCALAPPDATA "Programs\@briksdesktop\$nome")) { if (Test-Path $p) { return $p } }
    return ""
}
function LerJson($p) { (Get-Content $p -Raw).TrimStart([char]0xFEFF) | ConvertFrom-Json }
function Http($metodo, $uri, $headers = @{}, $corpo = $null) {   # devolve status sem lancar excecao em 4xx/5xx
    try {
        $r = if ($null -ne $corpo) { Invoke-WebRequest -Method $metodo -Uri $uri -Headers $headers -Body $corpo -ContentType "application/json" -UseBasicParsing -TimeoutSec 20 }
             else { Invoke-WebRequest -Method $metodo -Uri $uri -Headers $headers -UseBasicParsing -TimeoutSec 20 }
        return [pscustomobject]@{ Status = [int]$r.StatusCode; Body = $r.Content }
    } catch {
        $resp = $_.Exception.Response
        if ($resp) { $txt = ""; try { $txt = (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd() } catch {}; return [pscustomobject]@{ Status = [int]$resp.StatusCode; Body = $txt } }
        return [pscustomobject]@{ Status = 0; Body = $_.Exception.Message }
    }
}

# --- 1. configuracao publica ---
if (-not $Url -or -not $TokenUrl) {
    $lj = if ($LivekitJson) { $LivekitJson } else { Achar "livekit.json" }
    if ($lj -and (Test-Path $lj)) { $c = LerJson $lj; if (-not $Url) { $Url = $c.url }; if (-not $TokenUrl) { $TokenUrl = $c.token_url } }
}
if (-not $Url) { $Url = $env:BRIKS_LIVEKIT_URL }
if (-not $TokenUrl) { $TokenUrl = $env:BRIKS_LIVEKIT_TOKEN_URL }
if ($Url -match '^wss://[^\s/]+' -and $TokenUrl -match '^https://[^\s/]+') { Etapa "Configuracao publica (livekit.json)" "OK" "$Url | $TokenUrl" }
else { Etapa "Configuracao publica (livekit.json)" "FALHA" "faltam url (wss://) e token_url (https://); veja docs/ativar-sfu.md"; exit 1 }

if (-not $SupabaseUrl -or -not $AnonKey) {
    $sj = if ($SupabaseJson) { $SupabaseJson } else { Achar "supabase.json" }
    if ($sj -and (Test-Path $sj)) { $c = LerJson $sj; if (-not $SupabaseUrl) { $SupabaseUrl = $c.url }; if (-not $AnonKey) { $AnonKey = if ($c.anon_key) { $c.anon_key } else { $c.publishable_key } } }
}
if ($AnonKey -match '^sb_secret_') { Etapa "Chave do supabase.json" "FALHA" "e uma chave SECRETA; use a publishable/anon"; exit 1 }

# --- 2. servidor LiveKit responde (HTTP) ---
$lkHttp = ($Url -replace '^wss://', 'https://').TrimEnd('/') + "/"
$r = Http "GET" $lkHttp
if ($r.Status -eq 200) { Etapa "Servidor LiveKit responde (HTTPS)" "OK" }
elseif ($r.Status -gt 0) { Etapa "Servidor LiveKit responde (HTTPS)" "FALHA" "HTTP $($r.Status)" }
else { Etapa "Servidor LiveKit responde (HTTPS)" "FALHA" $r.Body }

# --- 3. funcao de tokens protegida: sem JWT = 401 ---
$r = Http "POST" $TokenUrl @{} '{"room":"verificar-sfu"}'
if ($r.Status -eq 401) { Etapa "Funcao de tokens recusa quem nao entrou (401 sem JWT)" "OK" }
elseif ($r.Status -eq 404) { Etapa "Funcao de tokens recusa quem nao entrou (401 sem JWT)" "FALHA" "404: a funcao livekit-token nao esta publicada" }
else { Etapa "Funcao de tokens recusa quem nao entrou (401 sem JWT)" "FALHA" "esperado 401, veio $($r.Status)" }

# --- 4. login de teste -> token ---
if (-not $Email) { $Email = $env:BRIKS_TEST_EMAIL }
$token = $null
if (-not $Email -or -not $SupabaseUrl -or -not $AnonKey) {
    Etapa "Login de teste" "PULADO" "sem -Email/BRIKS_TEST_EMAIL ou sem supabase.json"
    Etapa "Funcao de tokens emite token (200 com JWT)" "PULADO"
    Etapa "Conexao WebSocket ao servidor com o token" "PULADO"
} else {
    $senha = $env:BRIKS_TEST_PASSWORD
    if (-not $senha) { $ss = Read-Host "Senha da conta de teste (nao aparece na tela)" -AsSecureString; $b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($ss); try { $senha = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b) } }
    $r = Http "POST" ($SupabaseUrl.TrimEnd('/') + "/auth/v1/token?grant_type=password") @{ apikey = $AnonKey } (@{ email = $Email; password = $senha } | ConvertTo-Json -Compress)
    $senha = $null
    $jwt = $null
    if ($r.Status -eq 200) { try { $jwt = ($r.Body | ConvertFrom-Json).access_token } catch {} }
    if ($jwt) { Etapa "Login de teste" "OK" } else { Etapa "Login de teste" "FALHA" "HTTP $($r.Status) (conta existe? e-mail confirmado?)" }
    if ($jwt) {
        $sala = "verificar-sfu-" + [guid]::NewGuid().ToString("N")
        $r = Http "POST" $TokenUrl @{ Authorization = "Bearer $jwt"; apikey = $AnonKey } (@{ room = $sala } | ConvertTo-Json -Compress)
        if ($r.Status -eq 200) { try { $token = ($r.Body | ConvertFrom-Json).token } catch {} }
        if ($token) { Etapa "Funcao de tokens emite token (200 com JWT)" "OK" }
        elseif ($r.Status -eq 503) { Etapa "Funcao de tokens emite token (200 com JWT)" "FALHA" "503: segredos LIVEKIT_* nao configurados no Supabase" }
        else { Etapa "Funcao de tokens emite token (200 com JWT)" "FALHA" "HTTP $($r.Status)" }
    } else { Etapa "Funcao de tokens emite token (200 com JWT)" "PULADO" "sem login" }
    $jwt = $null

    # --- 5. WebSocket de sinalizacao aceita o token ---
    if ($token) {
        $ws = New-Object Net.WebSockets.ClientWebSocket
        try {
            $uri = [Uri]($Url.TrimEnd('/') + "/rtc?access_token=$token&protocol=15&sdk=js&version=2.0.0&auto_subscribe=0")
            $cts = New-Object Threading.CancellationTokenSource 15000
            $ws.ConnectAsync($uri, $cts.Token).GetAwaiter().GetResult()
            Etapa "Conexao WebSocket ao servidor com o token" "OK"
            try { $ws.CloseAsync([Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "ok", [Threading.CancellationToken]::None).Wait(3000) | Out-Null } catch {}
        } catch {
            $m = $_.Exception.Message -replace 'access_token=[^&\s"]+', 'access_token=***'
            Etapa "Conexao WebSocket ao servidor com o token" "FALHA" $m
        } finally { $ws.Dispose() }
        $token = $null
    } else { Etapa "Conexao WebSocket ao servidor com o token" "PULADO" "sem token" }
}
if ($falhas) { Write-Host "`n$falhas etapa(s) com FALHA." -ForegroundColor Red; exit 1 }
Write-Host "`nTudo OK nas etapas executadas." -ForegroundColor Green
