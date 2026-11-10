# Recorta e amplia (8x) regioes de uma captura. -Pts "x,y;x,y;..." = canto superior esquerdo de cada recorte 48x48; junta numa tira.
param([string]$In, [string]$Out, [string]$Pts)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $In))
$list = $Pts -split ';'
$z = 8; $sz = 48
$dst = New-Object System.Drawing.Bitmap ($list.Count * ($sz * $z + 6)), ($sz * $z)
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = 'NearestNeighbor'; $g.PixelOffsetMode = 'Half'
$i = 0
foreach ($p in $list) {
    $xy = $p -split ','; $x = [int]$xy[0]; $y = [int]$xy[1]
    $x = [Math]::Max(0, [Math]::Min($src.Width - $sz, $x)); $y = [Math]::Max(0, [Math]::Min($src.Height - $sz, $y))
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle ($i * ($sz * $z + 6)), 0, ($sz * $z), ($sz * $z)), (New-Object System.Drawing.Rectangle $x, $y, $sz, $sz), 'Pixel')
    $i++
}
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $dst.Dispose(); $src.Dispose()
