# Gera o instalador Windows do app nativo (NSIS, instalacao por usuario, mesmo appId/pasta/atalhos do Electron 1.x).
# Uso (raiz do repositorio): powershell -NoProfile -File scripts/build-installer.ps1 [-Version 1.3.0] [-Test]
#   -Test  usa outro GUID e outra pasta de instalacao ("@briksdesktop-test"), para testar sem tocar na instalacao real do Briks.
# Saida: apps/desktop-cui/installer-out/Briks-Setup-<versao>.exe (+ latest.yml no formato do electron-updater).
# So compila e grava em disco: nao publica, nao cria tag/release e nao executa o instalador.
param([string]$Version = "1.3.0", [switch]$Test, [string]$SupabaseJson = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$cui = Join-Path $root "apps\desktop-cui"
$num = ($Version -replace '[-+].*$', '')
if ($num -notmatch '^\d+\.\d+\.\d+$') { throw "Versao invalida: $Version" }

function Find-VsDir {
    foreach ($y in "18", "2022", "2019") { foreach ($e in "Community", "Professional", "Enterprise", "BuildTools") {
        $p = "C:\Program Files\Microsoft Visual Studio\$y\$e"; if (Test-Path "$p\VC\Auxiliary\Build\vcvars64.bat") { return $p } } }
    throw "Visual Studio (MSVC) nao encontrado."
}
$vs = Find-VsDir
$cmakeBin = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if (Test-Path $cmakeBin) { $env:PATH = "$cmakeBin;$env:PATH" }
$env:CMAKE_POLICY_VERSION_MINIMUM = "3.5"

Write-Host "== Rust (release) =="
cargo build --release --manifest-path "$root\desktop-backend\Cargo.toml"; if ($LASTEXITCODE) { throw "cargo (backend) falhou" }
cargo build --release --manifest-path "$cui\briks-core\Cargo.toml"; if ($LASTEXITCODE) { throw "cargo (briks-core) falhou" }
$backend = "$root\desktop-backend\target\release\briks-desktop-backend.exe"
$core = "$cui\briks-core\target\release\briks-core.exe"

Write-Host "== app nativo (Release: sem DLLs de depuracao) =="
$bd = "$cui\build-release"
Push-Location $cui
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBRIKS_VERSION=$num -DBRIKS_UPDATE_DEFAULT_ON=ON -DBRIKS_BACKEND_EXE=`"$backend`" && cmake --build build-release"
$rc = $LASTEXITCODE
Pop-Location
if ($rc) { throw "cmake falhou" }

Write-Host "== pasta de empacotamento =="
$stage = "$cui\stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory "$stage\backend" | Out-Null
Copy-Item "$bd\briks_cui.exe" "$stage\Briks.exe"          # mesmo nome do executavel do Electron: atalhos fixados e entradas de inicio continuam validos
Copy-Item "$bd\briks_overlay.exe" "$stage\briks_overlay.exe"
Copy-Item $core "$stage\briks-core.exe"
if (Test-Path "$cui\assets\login") { New-Item -ItemType Directory "$stage\assets" -Force | Out-Null; Copy-Item "$cui\assets\login" "$stage\assets\login" -Recurse }
if (Test-Path "$cui\assets\licenses.txt") { New-Item -ItemType Directory "$stage\assets" -Force | Out-Null; Copy-Item "$cui\assets\licenses.txt" "$stage\assets\licenses.txt" }   # Sobre > Licencas de terceiros
Copy-Item $backend "$stage\backend\briks-desktop-backend.exe"
$crt = Get-ChildItem "$vs\VC\Redist\MSVC" -Recurse -Filter vcruntime140.dll | Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC\d+\.CRT\\' -and $_.FullName -notmatch 'onecore' } | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "vcruntime140.dll (redistribuivel do VC) nao encontrado." }
Get-ChildItem $crt.DirectoryName -Filter "*.dll" | Where-Object { $_.Name -match '^(vcruntime140|msvcp140)(_1)?\.dll$' } | Copy-Item -Destination $stage   # runtime do VC "app-local" (briks-core e backend em Rust usam vcruntime140)
# supabase.json (URL + chave PUBLISHABLE/anon) ao lado do Briks.exe: outros PCs passam a ter login. Vem de -SupabaseJson ou da pasta de dados de quem compila.
# Nunca a chave secret/service_role: o script recusa. O arquivo nao e versionado.
if (-not $SupabaseJson) { $SupabaseJson = Join-Path $env:APPDATA "Briks-CUI\supabase.json" }
if (Test-Path $SupabaseJson) {
    $sj = Get-Content $SupabaseJson -Raw | ConvertFrom-Json
    $k = if ($sj.anon_key) { $sj.anon_key } elseif ($sj.publishable_key) { $sj.publishable_key } else { "" }
    if ($sj.url -notmatch '^https://' -or $k -match '^sb_secret_' -or $k -notmatch '^(sb_publishable_|eyJ)') { throw "supabase.json invalido ou com chave secreta: use URL https e chave publishable/anon." }
    if ($k -match '^eyJ') { $b = $k.Split('.')[1].Replace('-','+').Replace('_','/'); while ($b.Length % 4) { $b += '=' }; $role = ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($b)) | ConvertFrom-Json).role; if ($role -ne 'anon') { throw "supabase.json: chave nao e anon (role=$role)." } }
    @{ url = $sj.url; anon_key = $k } | ConvertTo-Json | Set-Content "$stage\supabase.json" -Encoding UTF8
    Write-Host "supabase.json incluido (chave publishable)."
} else { Write-Warning "supabase.json nao encontrado: o instalador sai sem login online." }
foreach ($f in "LICENSE", "THIRD_PARTY_NOTICES.md") { if (Test-Path "$root\$f") { Copy-Item "$root\$f" $stage } }
if (Test-Path "$root\LICENSES") { Copy-Item "$root\LICENSES" "$stage\LICENSES" -Recurse }

Write-Host "== NSIS =="
$mk = @(
    (Get-Command makensis -ErrorAction SilentlyContinue).Source,
    "C:\Program Files (x86)\NSIS\makensis.exe",
    (Get-ChildItem "$env:LOCALAPPDATA\electron-builder\Cache\nsis" -Recurse -Filter makensis.exe -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -notmatch '\\Bin$' } | Select-Object -First 1 -ExpandProperty FullName)
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $mk) { throw "makensis nao encontrado (instale o NSIS ou rode o electron-builder uma vez para ele baixar o NSIS)." }
$out = "$cui\installer-out"; New-Item -ItemType Directory -Force $out | Out-Null
$name = if ($Test) { "Briks-Test-Setup-$Version.exe" } else { "Briks-Setup-$Version.exe" }
$defs = @("/DAPP_VERSION=$num", "/DSTAGE_DIR=$stage", "/DOUT_FILE=$out\$name", "/DICON_FILE=$cui\assets\briks.ico")
if ($Test) { $defs += @("/DTEST_MODE", "/DAPP_GUID=9d0f4b1e-7c2a-5f3b-8a61-0b2c3d4e5f60", "/DINSTALL_SUBDIR=@briksdesktop-test") }
& $mk /V2 @defs "$PSScriptRoot\installer\briks.nsi"; if ($LASTEXITCODE) { throw "makensis falhou" }

$f = Get-Item "$out\$name"
$sha = [Convert]::ToBase64String([Security.Cryptography.SHA512]::Create().ComputeHash([IO.File]::ReadAllBytes($f.FullName)))
$yml = "version: $Version`nfiles:`n  - url: $name`n    sha512: $sha`n    size: $($f.Length)`npath: $name`nsha512: $sha`nreleaseDate: '$([DateTime]::UtcNow.ToString("o"))'`n"
[IO.File]::WriteAllText("$out\latest.yml", $yml, (New-Object Text.UTF8Encoding($false)))
Write-Host "Instalador: $($f.FullName) ($([math]::Round($f.Length/1MB,1)) MB)"
Write-Host "Feed:       $out\latest.yml"
