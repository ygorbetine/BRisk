# Monta referencia | captura lado a lado (mesma altura) e imprime cores amostradas em pontos relativos (so o bloco de conversa).
param([string]$Ref, [string]$Cap, [string]$Out)
Add-Type -AssemblyName System.Drawing
$a = [System.Drawing.Bitmap]::FromFile($Ref); $b = [System.Drawing.Bitmap]::FromFile($Cap)
$h = 772; $wa = $a.Width; $wb = [int]($b.Width * $h / $b.Height)
$o = New-Object System.Drawing.Bitmap ($wa + $wb + 8), $h
$g = [System.Drawing.Graphics]::FromImage($o); $g.Clear([System.Drawing.Color]::White)
$g.DrawImage($a, 0, 0, $wa, $h); $g.DrawImage($b, $wa + 8, 0, $wb, $h)
$o.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$pts = @(@('canto sup esq chat', .30, .03), @('topo central', .65, .03), @('canto sup dir', .97, .04), @('meio dir', .97, .30), @('centro', .60, .45), @('meio esq', .35, .45), @('base central', .62, .93), @('base dir', .97, .93), @('sidebar topo', .15, .03), @('sidebar meio', .15, .50))
foreach ($p in $pts) {
    $ca = $a.GetPixel([int]($a.Width * $p[1]), [int]($a.Height * $p[2])); $cb = $b.GetPixel([int]($b.Width * $p[1]), [int]($b.Height * $p[2]))
    '{0,-20} ref #{1:x2}{2:x2}{3:x2}   cui #{4:x2}{5:x2}{6:x2}' -f $p[0], $ca.R, $ca.G, $ca.B, $cb.R, $cb.G, $cb.B
}
$g.Dispose(); $o.Dispose(); $a.Dispose(); $b.Dispose()
