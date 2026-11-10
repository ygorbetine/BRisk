# Compila o spike com MSVC + CMake do Visual Studio. Uso: powershell -File build.ps1
# VSLANG=1033: o Ninja só lê as dependências de cabeçalho quando o MSVC fala inglês ("Note: including file:"); em pt-BR o rebuild incremental ficava com objetos velhos (tamanho de struct divergente = pilha corrompida)
$env:VSLANG = "1033"
# Com o MSVC em pt-BR o Ninja não entende "Observação: incluindo arquivo:" e perde as dependências de cabeçalho: sempre recompila os .obj do app (poucos segundos)
Remove-Item "$PSScriptRoot\build\CMakeFiles\briks_cui.dir\src\*.obj" -ErrorAction SilentlyContinue
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$cm = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm`" -S . -B build -G Ninja -DBRIKS_BACKEND_EXE=`"$env:BRIKS_BACKEND_EXE`" && `"$cm`" --build build"
