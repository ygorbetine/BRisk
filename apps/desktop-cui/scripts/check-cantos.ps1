# Verificacao automatica de cantos: renderiza cada tela (1280x800), e para cada painel testa, nos 4 cantos, o pixel que fica FORA do arco
# (3 px na diagonal para dentro do retangulo envolvente) contra o pixel logo fora do retangulo (3 px na diagonal para fora).
# Se o canto vazar reto, o primeiro e' claramente diferente do segundo. Falha (exit 1) se a diferenca maxima por canal passar do limite.
# Geometria no tamanho padrao 1280x800 (mesmos retangulos do layout padrao); atualize aqui se o layout padrao mudar.
param([string]$Exe = "build\briks_cui.exe", [int]$Limit = 8, [string]$Out = "shots\cantos")
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$root = Split-Path $PSScriptRoot -Parent
$env:BRIKS_BACKEND_EXE = "$root/../../desktop-backend/target/release/briks-desktop-backend.exe"
$env:CUI_SCREENSHOT_FRAMES = '30'; Remove-Item Env:BRIKS_WINDOW -ErrorAction SilentlyContinue
# nome, demo, dev_open, retangulos x,y,w,h,raio
$telas = @(
  @('social',   'social',  '', @(@(11,13,74,776,22), @(97,13,268,776,22), @(377,13,892,776,22))),
  @('grupo',    'group',   '', @(@(11,13,74,776,22), @(97,13,268,776,22), @(377,13,892,776,22))),
  @('historico','history', '', @(,@(10,13,1260,777,22))),
  @('chamada',  'call',    '', @(@(18,92,615,585,20), @(647,92,614,585,20))),
  @('config',   'social',  'settings', @(@(11,13,74,776,22), @(97,13,268,776,22)))
)
$falhas = 0
foreach ($t in $telas) {
    $png = "$Out\$($t[0]).png"
    $env:BRIKS_DEMO = $t[1]; $env:CUI_SCREENSHOT = $png
    if ($t[2]) { $env:BRIKS_DEV_OPEN = $t[2] } else { Remove-Item Env:BRIKS_DEV_OPEN -ErrorAction SilentlyContinue }
    $null = & $Exe 2>&1
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    $i = 0
    foreach ($r in $t[3]) {
        $x, $y, $w, $h = $r[0], $r[1], $r[2], $r[3]
        $cantos = @(@('sup-esq', $x, $y, 1, 1), @('sup-dir', ($x + $w - 1), $y, -1, 1), @('inf-esq', $x, ($y + $h - 1), 1, -1), @('inf-dir', ($x + $w - 1), ($y + $h - 1), -1, -1))
        foreach ($c in $cantos) {
            $px = $c[1] + 3 * $c[3]; $py = $c[2] + 3 * $c[4]
            $qx = [Math]::Max(0, [Math]::Min($bmp.Width - 1, $c[1] - 3 * $c[3])); $qy = [Math]::Max(0, [Math]::Min($bmp.Height - 1, $c[2] - 3 * $c[4]))
            $a = $bmp.GetPixel($px, $py); $b = $bmp.GetPixel($qx, $qy)
            $d = [Math]::Max([Math]::Abs($a.R - $b.R), [Math]::Max([Math]::Abs($a.G - $b.G), [Math]::Abs($a.B - $b.B)))
            $ok = $d -le $Limit
            if (-not $ok) { $falhas++ }
            '{0,-10} painel {1} {2,-8} dentro #{3:x2}{4:x2}{5:x2} fora #{6:x2}{7:x2}{8:x2} dif {9,3}  {10}' -f $t[0], $i, $c[0], $a.R, $a.G, $a.B, $b.R, $b.G, $b.B, $d, $(if ($ok) { 'ok' } else { 'CANTO VAZANDO' })
        }
        $i++
    }
    $bmp.Dispose()
}
Remove-Item Env:BRIKS_DEV_OPEN -ErrorAction SilentlyContinue
if ($falhas) { "FALHOU: $falhas cantos"; exit 1 } else { 'cantos ok' }

