# 1.4.3: prova da previa inline de video/audio (Media Foundation + waveOut). Gera um mp4 (testsrc 6 s + seno 440 Hz) e um mp3 com o ffmpeg,
# monta o roteiro com esses caminhos e roda o app oculto: estados 0 -> 2 -> 3 -> 4 -> 2 -> 0. Exige ffmpeg no PATH e build\briks_cui.exe.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSScriptRoot
$w = Join-Path $env:TEMP 'briks-media-prova'; New-Item -ItemType Directory -Force $w | Out-Null
$mp4 = "$w\teste.mp4"; $mp3 = "$w\tom.mp3"
if (-not (Test-Path $mp4)) { ffmpeg -y -loglevel error -f lavfi -i testsrc=size=640x360:rate=25 -f lavfi -i sine=frequency=440 -t 6 -pix_fmt yuv420p -c:v libx264 -c:a aac $mp4 }
if (-not (Test-Path $mp3)) { ffmpeg -y -loglevel error -f lavfi -i sine=frequency=440 -t 5 $mp3 }
$f4 = $mp4.Replace('\', '/'); $f3 = $mp3.Replace('\', '/')
(Get-Content "$here\scripts\media-inline.txt") -replace '(?<=media play ).*teste\.mp4', $f4 -replace '(?<=media play ).*tom\.mp3', $f3 | Set-Content "$w\roteiro.txt"
$env:BRIKS_DEMO = 'msgs'; $env:BRIKS_DEMO_ATTACH = '1'; $env:BRIKS_DOCK = 'default'; $env:BRIKS_NET_LOOPBACK_ONLY = '1'; $env:BRIKS_HIDDEN = '1'; $env:BRIKS_MEDIA_KEEP = '1'
$env:BRIKS_DEMO_MEDIA_VIDEO = $mp4; $env:BRIKS_DEMO_MEDIA_AUDIO = $mp3; $env:BRIKS_SCRIPT = "$w\roteiro.txt"; $env:BRIKS_SCRIPT_OUT = "$w\saida"
$p = Start-Process "$here\build\briks_cui.exe" -PassThru -Wait -RedirectStandardError "$w\err.txt" -NoNewWindow
Get-Content "$w\err.txt" | Select-String 'roteiro'
if ($p.ExitCode -ne 0) { Write-Host 'FALHOU' -ForegroundColor Red; exit 1 } else { Write-Host "OK (capturas em $w\saida)" -ForegroundColor Green }
