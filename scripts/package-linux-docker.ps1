$ErrorActionPreference = "Stop"

$projectPath = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$releasePath = Join-Path $projectPath "apps\desktop\release"
$environmentPath = Join-Path $projectPath ".env"
New-Item -ItemType Directory -Force -Path $releasePath | Out-Null

$publicEnvironment = @{}
if (Test-Path $environmentPath) {
  foreach ($line in Get-Content -LiteralPath $environmentPath) {
    if ($line -match '^\s*(VITE_SUPABASE_URL|VITE_SUPABASE_ANON_KEY|VITE_DEBUG_SIGNALING|VITE_ICE_SERVERS_JSON)\s*=\s*(.*)\s*$') {
      $publicEnvironment[$matches[1]] = $matches[2].Trim().Trim('"').Trim("'")
    }
  }
}

$required = @("VITE_SUPABASE_URL", "VITE_SUPABASE_ANON_KEY")
foreach ($name in $required) {
  if (-not $publicEnvironment[$name]) { throw "Defina $name no arquivo .env antes de empacotar o aplicativo." }
}

Write-Host "Gerando AppImage e DEB com frontend + backend Rust local dentro de um container Linux..."
& docker build --file infrastructure/desktop/Dockerfile.linux --tag briks-p2p-linux-builder:local .
if ($LASTEXITCODE -ne 0) { throw "Não foi possível preparar a imagem Linux." }

$dockerArgs = @(
  "run", "--rm",
  "--env", "VITE_SUPABASE_URL=$($publicEnvironment.VITE_SUPABASE_URL)",
  "--env", "VITE_SUPABASE_ANON_KEY=$($publicEnvironment.VITE_SUPABASE_ANON_KEY)",
  "--volume", "${releasePath}:/project/apps/desktop/release",
  "--volume", "briks-p2p-electron-cache:/root/.cache/electron",
  "--volume", "briks-p2p-electron-builder-cache:/root/.cache/electron-builder",
  "--volume", "briks-p2p-cargo-registry:/root/.cargo/registry",
  "--volume", "briks-p2p-cargo-git:/root/.cargo/git"
)
foreach ($name in @("VITE_DEBUG_SIGNALING", "VITE_ICE_SERVERS_JSON")) {
  if ($publicEnvironment[$name]) {
    $dockerArgs += @("--env", "$name=$($publicEnvironment[$name])")
  }
}
$dockerArgs += "briks-p2p-linux-builder:local"

& docker @dockerArgs
if ($LASTEXITCODE -ne 0) { throw "O empacotamento Linux falhou com código $LASTEXITCODE." }
Write-Host "Pacotes disponíveis em apps/desktop/release/."
