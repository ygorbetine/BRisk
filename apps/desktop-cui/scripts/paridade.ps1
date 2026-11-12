<#
  Roteiro automatizado de PARIDADE Electron x CUI.
  Uso:  powershell -NoProfile -File apps\desktop-cui\scripts\paridade.ps1 [-Saida pasta] [-Filtro regex] [-SoInventario] [-Exe caminho]

  O que faz (nada aparece na tela de ninguém):
   1. Lê scripts\paridade\itens.csv (inventário: um item por linha, com a classe esperada).
   2. Roda cada caso de scripts\paridade\testes.json: abre o app OCULTO (BRIKS_HIDDEN=1, modo demo, dados e layout em pasta
      temporária), injeta mouse/teclado pelo BRIKS_SCRIPT, tira capturas internas do próprio app (CUI_SCREENSHOT/shot) e
      compara as capturas (muda / igual) para decidir PASSOU ou FALHOU. Nunca captura o monitor.
   3. Para itens AUSENTE e SEM-EFEITO roda uma "sonda": procura no código-fonte o que NÃO deveria existir. Se achar, o item
      aparece como REVER (alguém implementou: reclassifique no itens.csv).
   4. Gera relatorio.md e relatorio.csv em -Saida (padrão: %TEMP%\briks-paridade) e imprime o resumo.
  Código de saída: 0 sem FALHOU/REVER; 1 caso contrário.
  Sem GPU: nada aqui abre transmissão/captura. Não toca em %APPDATA%\@briks; confere que %APPDATA%\Briks-CUI não mudou.
#>
param(
    [string]$Saida = (Join-Path $env:TEMP "briks-paridade"),
    [string]$Filtro = "",
    [switch]$SoInventario,
    [string]$Exe = "",
    [int]$TimeoutSeg = 120,
    [int]$Dy = 39        # deslocamento vertical dos cliques dos roteiros (escritos antes da barra de título própria: 36 px + filete de 3 px)
)
$ErrorActionPreference = "Stop"
$cui = Split-Path -Parent $PSScriptRoot                  # apps\desktop-cui
$dir = Join-Path $PSScriptRoot "paridade"
if (-not $Exe) { $Exe = Join-Path $cui "build\briks_cui.exe" }
$buildDir = Split-Path -Parent $Exe
New-Item -ItemType Directory -Force $Saida | Out-Null

Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Drawing.Imaging;
public static class ImgDiff {
  // pixels que diferem (soma dos canais > thr) dentro da região; -1 se a imagem faltar
  public static int Count(string a, string b, int rx, int ry, int rw, int rh, int thr) {
    try {
      using (var A = new Bitmap(a)) using (var B = new Bitmap(b)) {
        int w = Math.Min(A.Width, B.Width), h = Math.Min(A.Height, B.Height);
        if (rw <= 0) { rx = 0; ry = 0; rw = w; rh = h; }
        int x1 = Math.Min(w, rx + rw), y1 = Math.Min(h, ry + rh), n = 0;
        var da = A.LockBits(new Rectangle(0, 0, A.Width, A.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        var db = B.LockBits(new Rectangle(0, 0, B.Width, B.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        unsafe0(da, db, rx, ry, x1, y1, thr, ref n);
        A.UnlockBits(da); B.UnlockBits(db);
        return n;
      }
    } catch { return -1; }
  }
  static void unsafe0(BitmapData da, BitmapData db, int rx, int ry, int x1, int y1, int thr, ref int n) {
    byte[] pa = new byte[Math.Abs(da.Stride) * da.Height], pb = new byte[Math.Abs(db.Stride) * db.Height];
    System.Runtime.InteropServices.Marshal.Copy(da.Scan0, pa, 0, pa.Length);
    System.Runtime.InteropServices.Marshal.Copy(db.Scan0, pb, 0, pb.Length);
    for (int y = ry; y < y1; y++) for (int x = rx; x < x1; x++) {
      int ia = y * da.Stride + x * 4, ib = y * db.Stride + x * 4;
      int d = Math.Abs(pa[ia] - pb[ib]) + Math.Abs(pa[ia + 1] - pb[ib + 1]) + Math.Abs(pa[ia + 2] - pb[ib + 2]);
      if (d > thr) n++;
    }
  }
}
"@

function Remove-Acentos([string]$t) {
    $n = $t.Normalize([Text.NormalizationForm]::FormD)
    return ([regex]::Replace($n, "\p{Mn}", "")).ToLowerInvariant()
}

# macros do roteiro: snap <nome> (cursor no canto, espera, captura), tap X Y (clique de verdade e espera, coordenada do layout antigo + Dy),
# mtap X Y (igual, coordenada absoluta: modais) e tap <ponto simbolico> (ex.: rel:chat:R-30:T+40, ver dock_script_point)
function Expand-Roteiro([string]$origem, [string]$destino) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($l in (Get-Content -LiteralPath $origem -Encoding UTF8)) {
        $s = $l.Trim()
        if (-not $s -or $s.StartsWith("#")) { continue }
        $t = $s -split "\s+", 3
        switch ($t[0]) {
            "snap" { $out.Add("at 2 790"); $out.Add("waitms 450"); $out.Add("shot $($t[1])") }
            "mtap" { $out.Add("click $($t[1]) $($t[2])"); $out.Add("at 2 790"); $out.Add("waitms 500") }   # clique em coordenada absoluta da janela (modais centralizados: sem o deslocamento Dy)
            "mat"  { $out.Add("at $($t[1]) $($t[2])") }   # at/to absolutos (sem Dy): dentro de modais
            "mto"  { $out.Add("to " + (($s -split "\s+", 2)[1])) }
            "mclick" { $out.Add("click $($t[1]) $($t[2])") }   # click absoluto, sem Dy
            "tap"  {
                if ($t[1] -match '^-?\d+$') { $out.Add("click $($t[1]) $([int]$t[2] + $Dy)") } else { $out.Add("click $($t[1])") }   # tap rel:chat:R-30:T+40 (ancora simbolica: sem Dy)
                $out.Add("at 2 790"); $out.Add("waitms 500") }
            default {
                $s = $s.Replace("{ASSETS}", (Join-Path $dir "ativos"))
                $m = [regex]::Match($s, '^(click|rclick|at|to)\s+(-?\d+)\s+(-?\d+)(.*)$')
                if ($m.Success -and -not ($m.Groups[1].Value -eq "at" -and $m.Groups[2].Value -eq "2" -and $m.Groups[3].Value -eq "790")) { $s = "$($m.Groups[1].Value) $($m.Groups[2].Value) $([int]$m.Groups[3].Value + $Dy)$($m.Groups[4].Value)" }
                $out.Add($s)
            }
        }
    }
    [IO.File]::WriteAllLines($destino, $out, (New-Object Text.UTF8Encoding($false)))
}

function Stop-Filhos {
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($buildDir, [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { try { $_.Kill() } catch {} }
}

$itens = Import-Csv -LiteralPath (Join-Path $dir "itens.csv") -Delimiter "|" -Encoding UTF8
$resultado = @{}   # id -> @{ res; det }
function Set-Res($id, $res, $det) {
    if (-not $resultado.ContainsKey($id) -or $res -eq "FALHOU") { $resultado[$id] = @{ res = $res; det = $det } }
    elseif ($resultado[$id].res -ne "FALHOU") { $resultado[$id].det += "; " + $det }
}

# ---- 1. casos executáveis ----
$appdataCui = Join-Path $env:APPDATA "Briks-CUI"
$antes = @{}
if (Test-Path $appdataCui) { Get-ChildItem $appdataCui -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { $antes[$_.FullName] = $_.LastWriteTimeUtc.Ticks } }
$casosFalhos = 0
if (-not $SoInventario) {
    if (-not (Test-Path $Exe)) { throw "briks_cui.exe não encontrado em $Exe. Compile antes (apps\desktop-cui\build.ps1)." }
    $casos = (Get-Content -LiteralPath (Join-Path $dir "testes.json") -Raw -Encoding UTF8 | ConvertFrom-Json).casos
    foreach ($c in $casos) {
        if ($Filtro -and $c.id -notmatch $Filtro) { continue }
        $w = Join-Path $Saida $c.id
        if (Test-Path $w) { Remove-Item -Recurse -Force $w }
        New-Item -ItemType Directory -Force $w, (Join-Path $w "dados"), (Join-Path $w "capturas") | Out-Null
        $scr = Join-Path $w "roteiro.txt"
        Expand-Roteiro (Join-Path $dir $c.roteiro) $scr
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = $Exe; $psi.WorkingDirectory = $buildDir; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardError = $true   # stderr do roteiro (linhas "FALHOU linha N") vai para stderr.txt do caso
        $e = $psi.EnvironmentVariables
        $e["BRIKS_NET_LOOPBACK_ONLY"] = "1"; $e["BRIKS_HIDDEN"] = "1"; $e["BRIKS_DEMO"] = $c.demo; $e["BRIKS_DOCK"] = "default"; $e["BRIKS_WINDOW"] = "1280x800"
        $e["BRIKS_SCRIPT"] = $scr; $e["BRIKS_SCRIPT_OUT"] = (Join-Path $w "capturas"); $e["BRIKS_SCRIPT_SHOTS"] = "1"
        $e["BRIKS_NO_BREATH"] = "1"; $e["BRIKS_NO_IMPORT"] = "1"; $e["BRIKS_DISABLE_UPDATE"] = "1"
        $e["BRIKS_LAYOUT_FILE"] = (Join-Path $w "layout.json"); $e["BRIKS_THEME_FILE"] = (Join-Path $w "theme.json")
        $e["BRIKS_SETTINGS_FILE"] = (Join-Path $w "settings.cfg"); $e["BRIKS_GRADIENTS_FILE"] = (Join-Path $w "grad.json")
        $e["BRIKS_CORE_DATA"] = (Join-Path $w "dados"); $e["BRIKS_CUI_DATA"] = (Join-Path $w "dados")
        if ($c.env) { foreach ($p in $c.env.PSObject.Properties) { $e[$p.Name] = ([string]$p.Value).Replace("{w}", $w) } }
        # "preparar": arquivos de teste a criar na pasta do caso, como "pasta\nome.bin:KB" (o caso os consome; nada fora da pasta do caso)
        if ($c.preparar) { foreach ($pr in $c.preparar) { $pp = ($pr -split ":", 2); $alvo = Join-Path $w $pp[0]; New-Item -ItemType Directory -Force (Split-Path -Parent $alvo) | Out-Null; [IO.File]::WriteAllBytes($alvo, (New-Object byte[] ([int]$pp[1] * 1024))) } }
        # até 2 tentativas: o app já saiu com código 1 de forma intermitente ao clicar em Sair (anotado na página de paridade)
        for ($tent = 1; $tent -le 2; $tent++) {
            $proc = $null; $motivo = ""
            try {
                $proc = [Diagnostics.Process]::Start($psi); $errTask = $proc.StandardError.ReadToEndAsync()
                if (-not $proc.WaitForExit($TimeoutSeg * 1000)) { $motivo = "tempo esgotado ($TimeoutSeg s)"; try { $proc.Kill() } catch {} }
                elseif ($proc.ExitCode -ne 0) { $motivo = "saiu com código $($proc.ExitCode)" }
            } finally { Stop-Filhos; try { if ($errTask.Wait(3000)) { [IO.File]::WriteAllText((Join-Path $w "stderr.txt"), $errTask.Result) } } catch {} }
            if (-not $motivo) { break }
            if ($tent -eq 1) { Write-Host ("caso {0,-18} {1}; repetindo" -f $c.id, $motivo); Get-ChildItem (Join-Path $w "capturas") -ErrorAction SilentlyContinue | Remove-Item -Force }
        }
        $cap = Join-Path $w "capturas"
        $lista = @{}
        foreach ($k in $c.checagens) {
            $ids = @($k.item)
            $a = Join-Path $cap "$($k.de).png"; $b = Join-Path $cap "$($k.para).png"
            if ($motivo -or -not (Test-Path $a) -or -not (Test-Path $b)) {
                foreach ($id in $ids) { Set-Res $id "FALHOU" "$($c.id): $($k.de)>$($k.para) sem captura ($motivo)" }
                $casosFalhos++; continue
            }
            $rg = @(0, 0, 0, 0); if ($k.regiao) { $rg = @($k.regiao) }
            $n = [ImgDiff]::Count($a, $b, $rg[0], $rg[1], $rg[2], $rg[3], 48)
            $min = 300; if ($k.min) { $min = [int]$k.min }
            if ($k.esperado -eq "muda") { $ok = ($n -ge $min) } else { $ok = ($n -ge 0 -and $n -le 200) }
            $txt = "$($c.id) $($k.de)>$($k.para) esperado=$($k.esperado) pixels=$n"
            foreach ($id in $ids) { if ($ok) { Set-Res $id "PASSOU" $txt } else { Set-Res $id "FALHOU" $txt } }
            if (-not $ok) { $casosFalhos++ }
        }
        Write-Host ("caso {0,-18} {1}" -f $c.id, $(if ($motivo) { $motivo } else { "ok" }))
    }
    Stop-Filhos
}

# integridade: o app de teste não pode ter gravado em %APPDATA%\Briks-CUI
$mudou = @()
if (Test-Path $appdataCui) { Get-ChildItem $appdataCui -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { if (-not $antes.ContainsKey($_.FullName) -or $antes[$_.FullName] -ne $_.LastWriteTimeUtc.Ticks) { $mudou += $_.FullName } } }

# ---- 2. sondas (AUSENTE / SEM-EFEITO) ----
$fontes = @(Get-ChildItem (Join-Path $cui "src") -Filter *.c -File) + @(Get-ChildItem (Join-Path $cui "briks-core\src") -Filter *.rs -File -Recurse -ErrorAction SilentlyContinue)
$texto = @{}
foreach ($f in $fontes) { $texto[$f.FullName] = @((Get-Content -LiteralPath $f.FullName -Encoding UTF8) | ForEach-Object { Remove-Acentos $_ }) }
foreach ($it in $itens) {
    if ($it.classe -ne "AUSENTE" -and $it.classe -ne "PRESENTE-SEM-EFEITO") { continue }
    if (-not $it.sonda) { continue }
    $rx = (Remove-Acentos $it.sonda).Replace([string][char]0x00A6, "|")
    $excl = @(); if ($it.excluir) { $excl = $it.excluir -split "," }
    $achou = @()
    foreach ($f in $fontes) {
        if (@($excl | Where-Object { $f.Name -like $_ }).Count) { continue }
        $ls = $texto[$f.FullName]
        for ($i = 0; $i -lt $ls.Count; $i++) {
            $ln = $ls[$i]
            if ($ln -match $rx) { if ($ln.TrimStart().StartsWith("//") -or $ln.TrimStart().StartsWith("/*") -or $ln.TrimStart().StartsWith("*")) { continue }; $achou += "$($f.Name):$($i + 1)"; if ($achou.Count -ge 3) { break } }
        }
        if ($achou.Count -ge 3) { break }
    }
    if ($achou.Count) { Set-Res $it.id "REVER" ("a sonda /" + $it.sonda + "/ achou código em " + ($achou -join ", ")) }
}

# ---- 3. relatório ----
$linhas = @()
foreach ($it in $itens) {
    $r = $resultado[$it.id]
    switch ($it.classe) {
        "PRESENTE-FUNCIONAL" { if ($r) { $res = $r.res; $det = $r.det } else { $res = "SEM-TESTE"; $det = "sem roteiro automático (conferir à mão ou por teste do core)" } }
        "PARCIAL" { if ($r) { $res = $r.res; $det = "parcial: falta parte do Electron. " + $r.det } else { $res = "SEM-TESTE"; $det = "parcial: falta parte do Electron; sem roteiro automático" } }
        "PRESENTE-SEM-EFEITO" {
            if ($r -and ($r.res -eq "REVER" -or $r.res -eq "FALHOU")) { $res = "REVER"; $det = "agora tem efeito visível ou código novo: reclassifique. " + $r.det }
            elseif ($r -and $r.res -eq "PASSOU") { $res = "SEM-EFEITO"; $det = "confirmado: o clique não muda nada. " + $r.det }
            else { $res = "SEM-EFEITO"; $det = "controle existe e só grava/mostra; a sonda não achou uso no código" } }
        "AUSENTE" { if ($r -and $r.res -eq "REVER") { $res = "REVER"; $det = $r.det } else { $res = "AUSENTE"; $det = "não encontrado no código (sonda)" } }
        default { $res = "VERIFICAR"; $det = "não foi possível mapear com certeza" }
    }
    $linhas += [pscustomobject]@{ id = $it.id; area = $it.area; item = $it.item; classe = $it.classe; prioridade = $it.prioridade; resultado = $res; detalhe = $det }
}
$linhas | Export-Csv -LiteralPath (Join-Path $Saida "relatorio.csv") -NoTypeInformation -Encoding UTF8 -Delimiter ";"
$cont = $linhas | Group-Object resultado | Sort-Object Name
$md = New-Object System.Collections.Generic.List[string]
$md.Add("# Relatório de paridade Electron x CUI"); $md.Add("")
$md.Add("Gerado em " + (Get-Date -Format "yyyy-MM-dd HH:mm") + " com " + $Exe); $md.Add("")
$md.Add("| Resultado | Itens |"); $md.Add("|---|---|")
foreach ($g in $cont) { $md.Add("| $($g.Name) | $($g.Count) |") }
if ($mudou.Count) { $md.Add(""); $md.Add("**ATENÇÃO:** arquivos de %APPDATA%\Briks-CUI mudaram durante o teste (os casos usam caminhos isolados; costuma ser outra instância do app rodando na máquina): " + ($mudou -join ", ")) }
$md.Add(""); $md.Add("| ID | Área | Item | Classe | Prio | Resultado | Detalhe |"); $md.Add("|---|---|---|---|---|---|---|")
foreach ($l in $linhas) { $md.Add("| $($l.id) | $($l.area) | $($l.item) | $($l.classe) | $($l.prioridade) | **$($l.resultado)** | $($l.detalhe) |") }
[IO.File]::WriteAllLines((Join-Path $Saida "relatorio.md"), $md, (New-Object Text.UTF8Encoding($true)))

Write-Host ""; Write-Host "=== Paridade Electron x CUI ==="
foreach ($g in $cont) { Write-Host ("{0,-12} {1}" -f $g.Name, $g.Count) }
Write-Host ("Relatório: " + (Join-Path $Saida "relatorio.md"))
if ($mudou.Count) { Write-Host "ATENÇÃO: arquivos de %APPDATA%\Briks-CUI mudaram (os casos usam caminhos isolados; costuma ser outra instância do app rodando): $($mudou -join ', ')" -ForegroundColor Yellow }
$ruim = @($linhas | Where-Object { $_.resultado -eq "FALHOU" -or $_.resultado -eq "REVER" }).Count
if ($ruim -gt 0) { Write-Host "$ruim item(ns) FALHOU/REVER" -ForegroundColor Red; exit 1 }
exit 0
