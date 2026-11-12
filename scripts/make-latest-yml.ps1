# Gera o latest.yml (formato do electron-updater) a partir de um instalador JA ASSINADO: o sha512 precisa ser do arquivo final,
# porque a assinatura muda os bytes. Uso: powershell -NoProfile -File scripts/make-latest-yml.ps1 -Installer <Briks-Setup-x.y.z.exe> -Version x.y.z
param([Parameter(Mandatory)][string]$Installer, [Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = "Stop"
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Versao invalida: $Version" }
$f = Get-Item $Installer; $name = $f.Name
$sha = [Convert]::ToBase64String([Security.Cryptography.SHA512]::Create().ComputeHash([IO.File]::ReadAllBytes($f.FullName)))
$yml = "version: $Version`nfiles:`n  - url: $name`n    sha512: $sha`n    size: $($f.Length)`npath: $name`nsha512: $sha`nreleaseDate: '$([DateTime]::UtcNow.ToString("o"))'`n"
$out = Join-Path $f.DirectoryName "latest.yml"
[IO.File]::WriteAllText($out, $yml, (New-Object Text.UTF8Encoding($false)))
Write-Host "latest.yml: $out"
