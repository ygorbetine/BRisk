# Capturas estáveis do Briks-CUI para comparar com as outras versões (sem backend, dados fixos).
# Uso: powershell -File capture.ps1 [-Out pasta] [-Modes login,social,call,history,group] [-Dock default|resize|moved|added|closed|drag]
param([string]$Out = "$PSScriptRoot\shots", [string[]]$Modes = @("login", "social", "call", "history", "group"), [string]$Dock = "")
$exe = "$PSScriptRoot\build\briks_cui.exe"
$Modes = $Modes -split ','
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($m in $Modes) {
    $env:BRIKS_DEMO = $m
    if ($Dock) { $env:BRIKS_DOCK = $Dock; $env:CUI_SCREENSHOT = Join-Path $Out "dock-$Dock.png" } else { $env:CUI_SCREENSHOT = Join-Path $Out "cui-$m.png" }
    $env:CUI_SCREENSHOT_FRAMES = "30"
    Start-Process -FilePath $exe -Wait
    Write-Host "capturado: $env:CUI_SCREENSHOT"
}
Remove-Item Env:BRIKS_DOCK, Env:BRIKS_DEMO, Env:CUI_SCREENSHOT, Env:CUI_SCREENSHOT_FRAMES -ErrorAction SilentlyContinue

