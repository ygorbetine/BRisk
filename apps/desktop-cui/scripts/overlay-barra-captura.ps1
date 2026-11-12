# Captura OCULTA da barra de edição da sobreposição (modo de edição): roda o selftest e salva PNGs. Nada aparece na tela e nenhum atalho global é registrado.
#   powershell -NoProfile -File scripts\overlay-barra-captura.ps1 [-Saida pasta] [-Exe build\briks_overlay.exe]
# Gera: overlay-barra-edicao.png (cena completa: "jogo" de mentira + painel + barra), overlay-barra.png (só a barra) e overlay-painel-edicao.png (só o painel).
param([string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\briks_overlay.exe"), [string]$Saida = (Join-Path (Split-Path -Parent $PSScriptRoot) "shots\overlay-barra"))
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Saida | Out-Null; $Saida = (Resolve-Path $Saida).Path
$falhas = 0
foreach ($a in "--protocol-test", "--edit-test") {
    $txt = Join-Path $Saida "$($a.Trim('-')).txt"
    $p = Start-Process $Exe -ArgumentList $a -RedirectStandardOutput $txt -Wait -PassThru -WindowStyle Hidden
    "$a : saída $($p.ExitCode) ($((Get-Content -Encoding UTF8 $txt | Where-Object { $_ -match '^ok' }).Count) verificações ok)"
    if ($p.ExitCode) { $falhas++; Get-Content -Encoding UTF8 $txt | Where-Object { $_ -notmatch '^ok' } }
}
$bmp = Join-Path $Saida "cena.bmp"
Start-Process $Exe -ArgumentList "--shot", $bmp -Wait -WindowStyle Hidden
if (-not (Test-Path $bmp)) { throw "a captura não foi gerada" }
$b = [Drawing.Bitmap]::FromFile($bmp)
try {
    $b.Save((Join-Path $Saida "overlay-barra-edicao.png"), [Drawing.Imaging.ImageFormat]::Png)
    $w = [Math]::Min(700, $b.Width); $bar = $b.Clone([Drawing.Rectangle]::new([int](($b.Width - $w) / 2), $b.Height - 200, $w, 200), $b.PixelFormat); $bar.Save((Join-Path $Saida "overlay-barra.png")); $bar.Dispose()
    $pan = $b.Clone([Drawing.Rectangle]::new($b.Width - 320, 0, 320, 340), $b.PixelFormat); $pan.Save((Join-Path $Saida "overlay-painel-edicao.png")); $pan.Dispose()
} finally { $b.Dispose() }
Remove-Item $bmp
"PNGs em $Saida"
if ($falhas) { exit 1 }
