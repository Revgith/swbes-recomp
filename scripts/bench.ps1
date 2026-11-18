# Repeatable fps benchmark. Each run: uncapped quick match on map 11; wait for
# the soldier to spawn, then a fixed 5 s; pause (Esc, retried until status.txt
# says "ui 1") so AI and physics stop and the frozen scene renders every
# frame; count frames. Prints each run and the median.
#   .\scripts\bench.ps1 [-Runs 3] [-Secs 10] [-Live]   (-Live: don't pause)
param([int]$Runs = 3, [int]$Secs = 10, [switch]$Live)
$r = (Split-Path $PSScriptRoot -Parent); $run = "$r\work\run"
function st($k) { foreach ($l in (Get-Content "$run\status.txt" -EA SilentlyContinue)) { $p = $l -split ' '; if ($p[0] -eq $k) { return $p[1] } } }
function fr { [int](st 'frame') }
$res = @()
for ($i = 1; $i -le $Runs; $i++) {
    $env:SWBES_QM_MAP = "11,-1,0"; Remove-Item Env:SWBES_QM_NOSPAWN -EA SilentlyContinue; $env:SWBES_FPS = "0"
    & "$r\scripts\quick.ps1" | Out-Null
    $t = 0; while ((st 'mode') -ne 'foot' -and $t -lt 90) { Start-Sleep 1; $t++ }
    if ($t -ge 90) { "run $i : never spawned"; continue }
    Start-Sleep 5
    if (-not $Live) {
        for ($k = 0; $k -lt 5 -and (st 'ui') -ne '1'; $k++) {
            Set-Content "$run\key.cmd" "1B 1"; Start-Sleep -Milliseconds 300
            Set-Content "$run\key.cmd" "1B 0"; Start-Sleep 2
        }
        if ((st 'ui') -ne '1') { "run $i : pause failed"; continue }
        Start-Sleep 2
    }
    $a = fr; $ca = [double](st 'cycles'); Start-Sleep $Secs; $b = fr; $cb = [double](st 'cycles')
    $f = [math]::Round(($b - $a) / $Secs, 1)
    $mc = [math]::Round(($cb - $ca) / ($b - $a) / 1e6, 3)    # game-thread Mcycles per frame
    $res += $mc; "run $i : $f fps   $mc Mcyc/frame"
}
Get-Process swbes -EA SilentlyContinue | Stop-Process -Force
if ($res.Count) { $s = $res | Sort-Object; "median: $($s[[int][math]::Floor($s.Count / 2)]) Mcyc/frame (lower is better)  spread: $($s[0])..$($s[-1])" }
