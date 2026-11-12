# Compila o app nativo (CUI): backend local (Rust) -> briks-core (Rust) -> briks_cui.exe (C99 + CMake/Ninja).
# Uso (na raiz do repositório): powershell -NoProfile -File scripts/build-cui.ps1 [-SkipTests]
# Saída: apps/desktop-cui/build/{briks_cui.exe, briks-core.exe, backend/briks-desktop-backend.exe, briks_smoke.exe}
# Só compila localmente: não publica, não versiona e não gera instalador.
param([switch]$SkipTests, [switch]$NoVpx)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$cui = Join-Path $root "apps\desktop-cui"

function Find-VsDir {
    foreach ($y in "18", "2022", "2019") { foreach ($e in "Community", "Professional", "Enterprise", "BuildTools") {
        $p = "C:\Program Files\Microsoft Visual Studio\$y\$e"; if (Test-Path "$p\VC\Auxiliary\Build\vcvars64.bat") { return $p } } }
    throw "Visual Studio (MSVC) não encontrado."
}
$vs = Find-VsDir
$cmakeBin = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if (Test-Path $cmakeBin) { $env:PATH = "$cmakeBin;$env:PATH" }
$env:CMAKE_POLICY_VERSION_MINIMUM = "3.5"   # libopus/audiopus: política antiga do CMake

Write-Host "== backend local (desktop-backend) =="
cargo build --release --manifest-path "$root\desktop-backend\Cargo.toml"; if ($LASTEXITCODE) { throw "cargo (backend) falhou" }
$backend = "$root\desktop-backend\target\release\briks-desktop-backend.exe"

Write-Host "== briks-core =="
$feat = @(); if (-not $NoVpx) { & "$PSScriptRoot\build-vpx.ps1"; $feat = @("--features", "vpx"); $env:VPX_LIB_DIR = "$cui\briks-core\vendor\vpx"; $env:VPX_VERSION = "1.13.0"; $env:VPX_STATIC = "1" }   # VP8/VP9 (libvpx estática); -NoVpx = só H264
if (-not $SkipTests) { cargo test --release @feat --manifest-path "$cui\briks-core\Cargo.toml"; if ($LASTEXITCODE) { throw "cargo test (briks-core) falhou" } }
cargo build --release @feat --manifest-path "$cui\briks-core\Cargo.toml"; if ($LASTEXITCODE) { throw "cargo build (briks-core) falhou" }

Write-Host "== app nativo =="
Push-Location $cui
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && cmake -S . -B build -G Ninja -DBRIKS_BACKEND_EXE=`"$backend`" && cmake --build build"
$rc = $LASTEXITCODE
Pop-Location
if ($rc) { throw "cmake falhou" }
Copy-Item "$cui\briks-core\target\release\briks-core.exe" "$cui\build\briks-core.exe" -Force
Get-ChildItem "$cui\build\*.exe", "$cui\build\backend\*.exe" | Select-Object FullName, Length
