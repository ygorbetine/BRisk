# Capturas da interface completa em 1280x800 (dados de exemplo, layout temporário). Uso: powershell -File scripts/capturas-completas.ps1 [-Out pasta]
param([string]$Out = "$PSScriptRoot\..\shots\completa")
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$env:BRIKS_BACKEND_EXE = "$root/../../desktop-backend/target/release/briks-desktop-backend.exe"
$env:BRIKS_LAYOUT_FILE = "$env:TEMP\lay-capturas.json"; $env:BRIKS_WINDOW = "1280x800"; $env:CUI_SCREENSHOT_FRAMES = "30"
foreach ($m in @(@("social", "social", ""), @("grupo", "group", ""), @("chamada", "call", ""), @("historico", "history", ""), @("config", "social", "settings"))) {
    Remove-Item $env:BRIKS_LAYOUT_FILE -ErrorAction SilentlyContinue
    $env:BRIKS_DEMO = $m[1]; $env:BRIKS_DOCK = "default"; $env:CUI_SCREENSHOT = "$Out\$($m[0]).png"
    if ($m[2]) { $env:BRIKS_DEV_OPEN = $m[2] } else { Remove-Item Env:BRIKS_DEV_OPEN -ErrorAction SilentlyContinue }
    Start-Process -FilePath "$root\build\briks_cui.exe" -Wait -WorkingDirectory "$root\build"
}
Remove-Item Env:BRIKS_DEV_OPEN, Env:CUI_SCREENSHOT -ErrorAction SilentlyContinue
# quadro do meio do arraste (pilha)
Remove-Item $env:BRIKS_LAYOUT_FILE -ErrorAction SilentlyContinue
$fr = "$env:TEMP\briks-arraste"; New-Item -ItemType Directory -Force $fr | Out-Null; Get-ChildItem $fr -Filter *.png | Remove-Item
$env:BRIKS_DEMO = "social"; $env:BRIKS_DOCK = "default"; $env:BRIKS_SCRIPT = "$PSScriptRoot\demo-arraste.txt"; $env:BRIKS_SCRIPT_OUT = $fr
Start-Process -FilePath "$root\build\briks_cui.exe" -Wait -WorkingDirectory "$root\build"
$files = Get-ChildItem $fr -Filter frame_*.png | Sort-Object Name
if ($files.Count) { Copy-Item $files[[int]($files.Count * 0.7)].FullName "$Out\arraste-pilha.png" -Force }
Get-ChildItem $Out -Filter *.png | Select-Object FullName, Length
