# Teste curto do briks_overlay.exe real pelo protocolo de stdin: abre, recebe um quadro, fica vivo, fecha por "quit" e por fim do pipe.
# A janela aparece por menos de 2 s no canto da tela e nunca recebe foco nem clique. Nada é capturado.
param([string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) "build\briks_overlay.exe"))
$ErrorActionPreference = "Stop"
$falhas = 0
function Ok($c, $m) { if ($c) { "ok: $m" } else { "FALHOU: $m"; $script:falhas++ } }
function Start-Ov {
    $psi = New-Object Diagnostics.ProcessStartInfo; $psi.FileName = $Exe; $psi.Arguments = "--stdin"
    $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.CreateNoWindow = $true
    [Diagnostics.Process]::Start($psi)
}
$utf8 = New-Object Text.UTF8Encoding($false)
$p = Start-Ov
$w = New-Object IO.StreamWriter($p.StandardInput.BaseStream, $utf8)
$w.NewLine = "`n"
$w.WriteLine("cfg 80 1"); $w.WriteLine("begin"); $w.WriteLine("i Sala conectada"); $w.WriteLine("p 1 Marina Costa"); $w.WriteLine("p 0 José"); $w.WriteLine("w Microfone mudo"); $w.WriteLine("end"); $w.Flush()
Start-Sleep -Milliseconds 800
Ok (-not $p.HasExited) "o overlay fica vivo depois de receber o quadro"
$p.Refresh(); Ok ($p.Responding) "o overlay responde (sem travar a fila de mensagens)"
$w.WriteLine("begin"); $w.WriteLine("end"); $w.Flush(); Start-Sleep -Milliseconds 300
Ok (-not $p.HasExited) "quadro vazio esconde a janela mas o processo continua"
$w.WriteLine("quit"); $w.Flush()
Ok ($p.WaitForExit(3000)) "quit fecha o overlay"
Ok ($p.ExitCode -eq 0) "saída 0 (veio $($p.ExitCode))"
$p2 = Start-Ov
Start-Sleep -Milliseconds 300
$p2.StandardInput.Close()
Ok ($p2.WaitForExit(3000)) "fim do pipe (app fechou) também fecha o overlay"
Get-Process briks_overlay -ErrorAction SilentlyContinue | Stop-Process -Force
if ($falhas) { "$falhas falha(s)"; exit 1 } else { "tudo certo" }
