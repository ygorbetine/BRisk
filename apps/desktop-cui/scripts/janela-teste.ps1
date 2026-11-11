# Janela de teste para a transmissão: conteúdo colorido com um contador que muda 10x por segundo, aberta na 2ª tela e SEM pegar o
# foco. Obedece a arquivos-sinal na pasta -Flags: resize.flag (muda de tamanho), min.flag (minimiza), restore.flag, close.flag.
# Uso: powershell -File janela-teste.ps1 -X 3500 -Y 560 -Flags C:\pasta
param([int]$X = 3500, [int]$Y = 560, [int]$W = 640, [int]$H = 400, [string]$Flags = $env:TEMP, [string]$Title = "Janela de teste da transmissao")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @"
using System.Windows.Forms;
public class QuietForm : Form { protected override bool ShowWithoutActivation { get { return true; } } }
"@
$f = New-Object QuietForm
$f.Text = $Title; $f.StartPosition = 'Manual'; $f.Location = New-Object System.Drawing.Point($X, $Y); $f.Size = New-Object System.Drawing.Size($W, $H)
$f.BackColor = [System.Drawing.Color]::FromArgb(24, 60, 120)
$l = New-Object System.Windows.Forms.Label
$l.Dock = 'Fill'; $l.TextAlign = 'MiddleCenter'; $l.ForeColor = [System.Drawing.Color]::White
$l.Font = New-Object System.Drawing.Font('Segoe UI', 44, [System.Drawing.FontStyle]::Bold)
$f.Controls.Add($l)
$bar = New-Object System.Windows.Forms.Panel
$bar.Dock = 'Top'; $bar.Height = 60; $bar.BackColor = [System.Drawing.Color]::FromArgb(230, 120, 30)
$f.Controls.Add($bar)
$n = 0
$t = New-Object System.Windows.Forms.Timer
$t.Interval = 100
$t.Add_Tick({
    $script:n++
    $l.Text = "TESTE " + $script:n
    $bar.BackColor = if ($script:n % 20 -lt 10) { [System.Drawing.Color]::FromArgb(230, 120, 30) } else { [System.Drawing.Color]::FromArgb(40, 190, 110) }
    foreach ($k in 'resize', 'min', 'restore', 'close') {
        $p = Join-Path $Flags "$k.flag"
        if (Test-Path $p) {
            Remove-Item $p -Force
            switch ($k) {
                'resize' { $f.Size = New-Object System.Drawing.Size(900, 520) }
                'min' { $f.WindowState = 'Minimized' }
                'restore' { $f.WindowState = 'Normal' }
                'close' { $f.Close() }
            }
        }
    }
})
$t.Start()
[System.Windows.Forms.Application]::Run($f)
