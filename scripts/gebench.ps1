# Deterministic render benchmark: redraw one captured game frame many times.
#   .\scripts\gebench.ps1 -Capture        record a fresh frame from a match into work\gebench.snap
#   .\scripts\gebench.ps1 [-Runs 3] [-Reps 300]   replay it; prints ms per frame (median of each run)
# The snapshot holds guest memory and GE state, so it only fits the build that
# made it while the module layout and GE state struct are unchanged; recapture
# after changing either. Replay images land in work\run\gebench{0,1,2}.ppm.
param([switch]$Capture, [int]$Runs = 3, [int]$Reps = 300)
$r = (Split-Path $PSScriptRoot -Parent); $run = "$r\work\run"; $snap = "$r\work\gebench.snap"
$env:PSPRECOMP_MUTE = "1"
if ($Capture) {
    Remove-Item "$run\gebench.snap", "$run\gebench.cmd" -EA SilentlyContinue
    Remove-Item Env:SWBES_QM_NOSPAWN, Env:PSPRECOMP_GEREPLAY -EA SilentlyContinue
    $env:SWBES_QM_MAP = "11,-1,0"; $env:SWBES_FPS = "0"
    & "$r\scripts\quick.ps1" | Out-Null
    $t = 0; while (-not ((Get-Content "$run\status.txt" -EA SilentlyContinue) -match '^mode foot') -and $t -lt 90) { Start-Sleep 1; $t++ }
    Start-Sleep 5
    Set-Content "$run\gebench.cmd" ""
    $t = 0; while ((Test-Path "$run\gebench.cmd") -and $t -lt 20) { Start-Sleep 1; $t++ }
    Get-Process swbes -EA SilentlyContinue | Stop-Process -Force
    if (-not (Test-Path "$run\gebench.snap")) { "capture failed"; exit 1 }
    Move-Item "$run\gebench.snap" $snap -Force; "captured -> $snap"
    exit 0
}
if (-not (Test-Path $snap)) { "no snapshot: run with -Capture first"; exit 1 }
$res = @()
for ($i = 1; $i -le $Runs; $i++) {
    Remove-Item "$run\gebench.txt" -EA SilentlyContinue
    $env:PSPRECOMP_GEREPLAY = "$snap $Reps"; $env:SWBES_QUICKMATCH = "0"
    $p = Start-Process -WindowStyle Hidden "$r\build\swbes.exe" -ArgumentList "`"$run\swbes.elf`"" -WorkingDirectory $run -RedirectStandardError "$r\work\err.txt" -RedirectStandardOutput "$r\work\out.txt" -PassThru
    if (-not $p.WaitForExit(120000)) { Stop-Process -Id $p.Id -Force; "run $i : timeout"; continue }
    $kv = @{}; Get-Content "$run\gebench.txt" -EA SilentlyContinue | ForEach-Object { $a = $_ -split ' '; $kv[$a[0]] = $a[1] }
    if (-not $kv.ms_median) { "run $i : failed (see work\err.txt)"; continue }
    $res += [double]$kv.mcycles_per_frame
    "run $i : $($kv.mcycles_per_frame) Mcyc  median $($kv.ms_median) ms  best $($kv.ms_best)  $($kv.vertices_per_frame) verts"
}
Remove-Item Env:PSPRECOMP_GEREPLAY -EA SilentlyContinue
if ($res.Count) { $s = $res | Sort-Object; "median: $($s[[int][math]::Floor($s.Count / 2)]) Mcycles/frame (game-thread CPU, lower is better)  spread: $($s[0])..$($s[-1])" }
