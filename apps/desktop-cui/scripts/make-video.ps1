# Gera o MP4 de demonstração do layout: roda o app em modo roteiro (um PNG por quadro) e monta com ffmpeg.
# Uso: powershell -File scripts/make-video.ps1 -Ffmpeg <ffmpeg.exe> [-Out layout-demo.mp4]
param([string]$Ffmpeg = "ffmpeg", [string]$Out = "layout-demo.mp4", [string]$Frames = "$env:TEMP\briks-frames", [string]$Script = "demo-layout.txt")
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force $Frames | Out-Null
Get-ChildItem $Frames -Filter frame_*.png -ErrorAction SilentlyContinue | ForEach-Object { $_.Delete() }
$env:BRIKS_DEMO = "social"; $env:BRIKS_DOCK = "default"
$env:BRIKS_SCRIPT = "$PSScriptRoot\$Script"; $env:BRIKS_SCRIPT_OUT = $Frames
Start-Process -FilePath "$root\build\briks_cui.exe" -Wait
Remove-Item Env:BRIKS_DEMO, Env:BRIKS_DOCK, Env:BRIKS_SCRIPT, Env:BRIKS_SCRIPT_OUT
& $Ffmpeg -y -framerate 30 -i "$Frames\frame_%05d.png" -c:v libx264 -pix_fmt yuv420p -crf 18 -vf "scale=1280:800" $Out
