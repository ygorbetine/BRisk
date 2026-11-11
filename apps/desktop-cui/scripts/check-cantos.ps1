# Verificacao automatica de cantos: renderiza cada tela (1280x800), e para cada painel testa, nos 4 cantos, o pixel que fica FORA do arco
# (3 px na diagonal para dentro do retangulo envolvente) contra o pixel logo fora do retangulo (3 px na diagonal para fora).
# Se o canto vazar reto, o primeiro e' claramente diferente do segundo. Falha (exit 1) se a diferenca maxima por canal passar do limite.
# Geometria: o proprio app grava os retangulos reais dos paineis (BRIKS_DUMP_RECTS, px da janela) em cada tela; nada de numeros fixos
# que envelhecem quando o layout padrao ou a barra de titulo mudam. Tela sem captura ou sem retangulos conta como FALHA (nunca passa em silencio).
param([string]$Exe = "build\briks_cui.exe", [int]$Limit = 8, [string]$Out = "shots\cantos")
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$root = Split-Path $PSScriptRoot -Parent
$env:BRIKS_BACKEND_EXE = "$root/../../desktop-backend/target/release/briks-desktop-backend.exe"
$env:CUI_SCREENSHOT_FRAMES = '30'; Remove-Item Env:BRIKS_WINDOW -ErrorAction SilentlyContinue
# nome, demo, dev_open, retangulos fixos x,y,w,h (so para telas que nao passam pelo dock: medidos na captura 1296x839 em 2026-11; $null = usa o BRIKS_DUMP_RECTS)
$telas = @(
  @('social',   'social',  '', $null),
  @('grupo',    'group',   '', $null),
  @('historico','history', '', @(,@(10,49,1276,780))),
  @('chamada',  'call',    '', @(@(18,129,623,587), @(655,129,623,587))),
  @('config',   'social',  'settings', $null)
)
$falhas = 0
foreach ($t in $telas) {
    $png = "$Out\$($t[0]).png"; $rects = "$Out\$($t[0]).rects.txt"
    Remove-Item $png, $rects -ErrorAction SilentlyContinue
    $env:BRIKS_DEMO = $t[1]; $env:CUI_SCREENSHOT = $png; $env:BRIKS_DUMP_RECTS = $rects
    if ($t[2]) { $env:BRIKS_DEV_OPEN = $t[2] } else { Remove-Item Env:BRIKS_DEV_OPEN -ErrorAction SilentlyContinue }
    $proc = Start-Process -FilePath $Exe -PassThru -Wait -WindowStyle Hidden   # exe GUI: sem -Wait o PowerShell nao espera e a captura ainda nao existe
    $lista = @()
    if ($t[3]) { foreach ($r in $t[3]) { $lista += ,@("0", $r[0], $r[1], $r[2], $r[3]) } }
    elseif (Test-Path $rects) { foreach ($ln in (Get-Content $rects)) { $lista += ,($ln -split ' ') } }
    if (-not (Test-Path $png) -or -not $lista.Count) { "{0,-10} FALHA: sem captura ou sem retangulos" -f $t[0]; $falhas++; continue }
    $bmp = $null
    try { $bmp = [System.Drawing.Bitmap]::FromFile($png) } catch { }
    if (-not $bmp) { "{0,-10} FALHA: captura ilegivel" -f $t[0]; $falhas++; continue }
    $i = 0
    foreach ($f in $lista) {
        $x = [int][Math]::Round([double]::Parse($f[1], [Globalization.CultureInfo]::InvariantCulture)); $y = [int][Math]::Round([double]::Parse($f[2], [Globalization.CultureInfo]::InvariantCulture))
        $w = [int][Math]::Round([double]::Parse($f[3], [Globalization.CultureInfo]::InvariantCulture)); $h = [int][Math]::Round([double]::Parse($f[4], [Globalization.CultureInfo]::InvariantCulture))
        $cantos = @(@('sup-esq', $x, $y, 1, 1), @('sup-dir', ($x + $w - 1), $y, -1, 1), @('inf-esq', $x, ($y + $h - 1), 1, -1), @('inf-dir', ($x + $w - 1), ($y + $h - 1), -1, -1))
        foreach ($c in $cantos) {
            $px = $c[1] + 3 * $c[3]; $py = $c[2] + 3 * $c[4]
            $qx = [Math]::Max(0, [Math]::Min($bmp.Width - 1, $c[1] - 3 * $c[3])); $qy = [Math]::Max(0, [Math]::Min($bmp.Height - 1, $c[2] - 3 * $c[4]))
            if ($px -lt 0 -or $py -lt 0 -or $px -ge $bmp.Width -or $py -ge $bmp.Height) { "{0,-10} painel {1} {2,-8} fora da captura ({3}x{4}) - painel passa da janela" -f $t[0], $i, $c[0], $bmp.Width, $bmp.Height; $falhas++; continue }
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
Remove-Item Env:BRIKS_DUMP_RECTS -ErrorAction SilentlyContinue
Remove-Item Env:BRIKS_DEV_OPEN -ErrorAction SilentlyContinue
if ($falhas) { "FALHOU: $falhas cantos"; exit 1 } else { 'cantos ok' }

