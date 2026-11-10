# Capturas da interação com mensagens (BRIKS_DEMO=msgs, sem backend). Uso: powershell -File capturas-mensagens.ps1 [-Out pasta]
# Cada cenário roda um roteiro (scripts/msgs-*.txt) e guarda o último quadro como PNG.
param([string]$Out = "$PSScriptRoot\..\shots-mensagens", [string[]]$Cenarios = @("hover", "menu"))
$Cenarios = $Cenarios -split ","
$exe = "$PSScriptRoot\..\build\briks_cui.exe"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($c in $Cenarios) {
    $frames = Join-Path $env:TEMP "briks-msgs-$c"; Remove-Item -Recurse -Force $frames -ErrorAction SilentlyContinue
    $env:BRIKS_DEMO = "msgs"; $env:BRIKS_DOCK = "default"
    $env:BRIKS_SCRIPT = "$PSScriptRoot\msgs-$c.txt"; $env:BRIKS_SCRIPT_OUT = $frames
    if ($c -like "*admin*") { $env:BRIKS_DEMO_ADMIN = "1" }
    Start-Process -FilePath $exe -Wait
    $last = Get-ChildItem $frames -Filter *.png | Sort-Object Name | Select-Object -Last 1
    Copy-Item $last.FullName (Join-Path $Out "msgs-$c.png") -Force
    Write-Host "capturado: msgs-$c.png"
    Remove-Item Env:BRIKS_DEMO_ADMIN -ErrorAction SilentlyContinue
}
Remove-Item Env:BRIKS_DEMO, Env:BRIKS_DOCK, Env:BRIKS_SCRIPT, Env:BRIKS_SCRIPT_OUT -ErrorAction SilentlyContinue
