# Compila a libvpx (VP8/VP9) para apps/desktop-cui/briks-core/vendor/vpx/libvpx.lib. Necessário só para `cargo build --features vpx`.
# Tudo local e reversível: as ferramentas (nasm, make) e o código-fonte são baixados para uma pasta de cache (padrão: %LOCALAPPDATA%\briks-vpx-build,
# ou -Cache) e conferidos contra SHA-256 FIXADOS abaixo; nada é instalado no sistema e o PATH global não muda (só o do processo).
# Requer Visual Studio com C++ (MSBuild + vcvars64) e o perl do Git para Windows (já vem com o Git).
param([string]$Cache = "$env:LOCALAPPDATA\briks-vpx-build", [switch]$Force)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$outDir = "$root\apps\desktop-cui\briks-core\vendor\vpx"
$lib = "$outDir\libvpx.lib"
if ((Test-Path $lib) -and -not $Force) { "libvpx.lib já existe ($lib); use -Force para refazer."; return }

# O ambiente do processo e mexido abaixo (PATH com o usr/bin do Git, mais as variaveis do vcvars64: com elas o rustc acha que o ambiente MSVC ja esta pronto e usa o link.exe do PATH, que no Git e o do Unix): tudo restaurado no fim
$envAntes = @{}; Get-ChildItem env: | ForEach-Object { $envAntes[$_.Name] = $_.Value }
$pins = @{
  "libvpx.tgz" = @{ url = "https://github.com/webmproject/libvpx/archive/refs/tags/v1.14.1.tar.gz"; sha = "901747254d80a7937c933d03bd7c5d41e8e6c883e0665fadcb172542167c7977" }  # idêntico ao chromium.googlesource.com/webm/libvpx v1.14.1 (conferido por diff)
  "nasm.zip"   = @{ url = "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip"; sha = "3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6" }  # o nasm.us não publica hash; este é o hash do arquivo auditado
  "make.zip"   = @{ url = "https://sourceforge.net/projects/ezwinports/files/make-4.4.1-without-guile-w32-bin.zip/download"; sha = "fb66a02b530f7466f6222ce53c0b602c5288e601547a034e4156a512dd895ee7" }
}
New-Item -ItemType Directory -Force "$Cache\dl" | Out-Null
foreach ($k in $pins.Keys) {
  $f = "$Cache\dl\$k"
  if (-not (Test-Path $f)) { curl.exe -sSL --fail -m 300 -o $f $pins[$k].url }   # curl: o SourceForge devolve uma página HTML a clientes que não são curl
  $h = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
  if ($h -ne $pins[$k].sha) { Remove-Item $f; throw "SHA-256 de $k não confere ($h); abortado" }
}
$src = "$Cache\src"; $tools = "$Cache\tools"; $bld = "$Cache\build"
Remove-Item $src, $bld -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $src, $tools, $bld | Out-Null
& "$env:SystemRoot\System32\tar.exe" -xzf "$Cache\dl\libvpx.tgz" -C $src   # bsdtar do Windows: o tar do Git não entende caminhos C:\
Expand-Archive "$Cache\dl\nasm.zip" "$tools\nasm" -Force
Expand-Archive "$Cache\dl\make.zip" "$tools\make" -Force
$nasm = (Get-ChildItem "$tools\nasm" -Recurse -Filter nasm.exe | Select-Object -First 1).DirectoryName
$make = (Get-ChildItem "$tools\make" -Recurse -Filter make.exe | Select-Object -First 1).DirectoryName
# Visual Studio: o instalado mais novo (vswhere); o caminho fixo 18\Community só existe na máquina do mantenedor, não no runner do GitHub (VS 2022, toolset v143).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vs) { $vs = "C:\Program Files\Microsoft Visual Studio\18\Community" }
$toolset = if ($vs -match '\\2022\\') { "v143" } elseif ($vs -match '\\2019\\') { "v142" } else { "v145" }
$gitsh = (Get-Command git).Source | Split-Path | Split-Path   # raiz do Git: usr\bin tem sh/perl
$srcdir = (Get-ChildItem $src -Directory | Select-Object -First 1).FullName.Replace([string][char]92, '/')
# Tudo roda com o PATH só deste processo: git\usr\bin (sh, perl), nasm, make e o ambiente do Visual Studio.
$env:PATH = "$gitsh\usr\bin;$nasm;$make;$env:PATH"
$envdump = cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set"
foreach ($l in $envdump) { if ($l -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }
$env:PATH = "$gitsh\usr\bin;$nasm;$make;$env:PATH"
Set-Location $bld
# 1) configure gera os Makefiles; 2) make gera o vpx.vcxproj; 3) MSBuild compila a biblioteca estática (/MD, como o Rust)
cmd /c "sh `"$srcdir/configure`" --target=x86_64-win64-vs17 --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-install-docs --enable-vp8 --enable-vp9 --enable-static --disable-shared --enable-multithread --enable-runtime-cpu-detect >configure.log 2>&1"
if ($LASTEXITCODE) { throw "configure da libvpx falhou" }
cmd /c "make -j4 >make.log 2>&1"   # pode reclamar da linha longa (ver abaixo); o que vale é o vpx.vcxproj regenerado depois
# O make do Windows corta a linha de comando longa do gerador (8191 caracteres) e o vpx.vcxproj sai SEM metade das fontes (sem vpx_decoder.c,
# vpx_dsp...): o link falha com símbolos vpx_codec_* não resolvidos. Roda o gerador à mão, com a linha completa.
cmd /c "make -n -B target=libs vpx.vcxproj >dry.txt 2>&1"
$dry = (Get-Content dry.txt) -join "`n"
$i = $dry.IndexOf("gen_msvs_vcxproj.sh"); if ($i -lt 0) { throw "não achei o comando do gerador do vcxproj" }
$cmd = $dry.Substring($i); $j = $cmd.IndexOf("`necho"); if ($j -gt 0) { $cmd = $cmd.Substring(0, $j) }
$cmd = "$srcdir/build/make/" + ($cmd -replace ([regex]::Escape([string][char]92) + "\r?\n"), " ")
Remove-Item vpx.vcxproj -ErrorAction SilentlyContinue
Set-Content gen.sh $cmd -Encoding ASCII
cmd /c "sh gen.sh >gen.log 2>&1"
$nc = (Select-String -Path vpx.vcxproj -Pattern 'Include=.*\.c"' -AllMatches).Matches.Count
if ($nc -lt 200) { throw "vpx.vcxproj incompleto ($nc fontes .c; esperado ~209)" }
# Sem /GL (WholeProgramOptimization): objetos LTCG não resolvem símbolos quando o rustc empacota a .lib; sem PDB para a lib ficar pequena.
cmd /c "msbuild vpx.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=$toolset /m /v:m /p:WholeProgramOptimization=false /p:DebugInformationFormat=None /t:Rebuild >msbuild.log 2>&1"
if ($LASTEXITCODE) { throw "build da libvpx falhou" }
New-Item -ItemType Directory -Force $outDir | Out-Null
Copy-Item "$bld\x64\Release\vpxmd.lib" $lib -Force
Set-Location $root
# O rustc embute a .lib no rlib do env-libvpx-sys e o cargo não vê a troca do arquivo: limpa esse pacote para o próximo build religar.
cmd /c "cargo clean -p env-libvpx-sys --release --manifest-path `"$root\apps\desktop-cui\briks-core\Cargo.toml`" >nul 2>&1"
"OK: $lib ($([math]::Round((Get-Item $lib).Length/1MB,1)) MB)"
foreach ($e in @(Get-ChildItem env:)) { if (-not $envAntes.ContainsKey($e.Name)) { Remove-Item "env:$($e.Name)" -ErrorAction SilentlyContinue } }
foreach ($n in $envAntes.Keys) { Set-Item "env:$n" $envAntes[$n] }
