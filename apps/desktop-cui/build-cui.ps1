# Compila só o briks_cui (o build.ps1 inteiro para no briks_smoke antes de ligar o exe). Renomeia o exe em uso para o link não travar.
$env:VSLANG = "1033"
$exe = "$PSScriptRoot\build\briks_cui.exe"
if (Test-Path $exe) { Rename-Item $exe ("briks_cui_old" + (Get-Random) + ".exe") -ErrorAction SilentlyContinue }
Remove-Item "$PSScriptRoot\build\CMakeFiles\briks_cui.dir\src\*.obj" -ErrorAction SilentlyContinue
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$cm = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm`" -S . -B build -G Ninja -DBRIKS_BACKEND_EXE=`"$env:BRIKS_BACKEND_EXE`" >nul && `"$cm`" --build build --target briks_cui" 2>&1 | Select-String "error|warning C|FAILED|Linking" | Select-Object -First 25
if (Test-Path $exe) { "OK: briks_cui.exe" } else { "FALHOU" }
