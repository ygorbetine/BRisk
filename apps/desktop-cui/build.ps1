# Compila o spike com MSVC + CMake do Visual Studio. Uso: powershell -File build.ps1
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$cm = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && `"$cm`" -S . -B build -G Ninja -DBRIKS_BACKEND_EXE=`"$env:BRIKS_BACKEND_EXE`" && `"$cm`" --build build"
