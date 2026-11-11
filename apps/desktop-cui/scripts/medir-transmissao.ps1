# Mede a CPU (em % de um núcleo) das instâncias do Briks deste build por alguns segundos e, se houver NVIDIA, o uso de GPU/codificador.
param([string]$Build = "$PSScriptRoot\..\build", [int]$Seconds = 4)
$root = (Resolve-Path $Build).Path
$p0 = Get-Process briks_cui, briks-core -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | ForEach-Object { [pscustomobject]@{ Id = $_.Id; Name = $_.ProcessName; Cpu = $_.TotalProcessorTime.TotalMilliseconds } }
$gpu = @()
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
for ($i = 0; $i -lt $Seconds; $i++) { Start-Sleep -Seconds 1; if ($smi) { $gpu += (& nvidia-smi --query-gpu=utilization.gpu,utilization.encoder,utilization.decoder --format=csv,noheader,nounits) } }
foreach ($a in $p0) {
  $b = Get-Process -Id $a.Id -ErrorAction SilentlyContinue
  if ($b) { "{0,-12} pid {1,-6} CPU {2,5:N0}% de um núcleo, memória {3:N0} MB" -f $a.Name, $a.Id, (($b.TotalProcessorTime.TotalMilliseconds - $a.Cpu) / ($Seconds * 10)), ($b.WorkingSet64 / 1MB) }
}
if ($gpu.Count) { "GPU (uso %, codificador %, decodificador %) por segundo: " + ($gpu -join " | ") }
