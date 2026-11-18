# Profile-guided build of swbes in build-pgo (MSVC /GENPROFILE, /USEPROFILE).
#   .\scripts\pgo.ps1 -Train     run the instrumented exe through a match; counts go to build-pgo\swbes*.pgc
#   then: cmake -B build-pgo -DSWBES_PGO=USE; cmake --build build-pgo --target swbes
# The training run: quick match on map 11, spawn, walk and look around, then
# pgosweep dumps the counters (a killed process writes none).
param([switch]$Train, [int]$Secs = 60)
$r = (Split-Path $PSScriptRoot -Parent); $run = "$r\work\run"; $exe = "$r\build-pgo\swbes.exe"
if (-not $Train) { "use -Train"; exit 1 }
$vc = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$sweep = Get-ChildItem "$vc\VC\Tools\MSVC\*\bin\Hostx64\x64\pgosweep.exe" | Select-Object -Last 1
if (-not $sweep) { "pgosweep.exe not found"; exit 1 }
Remove-Item "$r\build-pgo\swbes!*.pgc" -EA SilentlyContinue
$env:SWBES_EXE = $exe; $env:SWBES_QM_MAP = "11,-1,0"; $env:SWBES_FPS = "0"
Remove-Item Env:SWBES_QM_NOSPAWN -EA SilentlyContinue
& "$r\scripts\quick.ps1" | Out-Null
Remove-Item Env:SWBES_EXE
function st($k) { foreach ($l in (Get-Content "$run\status.txt" -EA SilentlyContinue)) { $p = $l -split ' '; if ($p[0] -eq $k) { return $p[1] } } }
$t = 0; while ((st 'mode') -ne 'foot' -and $t -lt 180) { Start-Sleep 1; $t++ }
if ($t -ge 180) { "never spawned"; Get-Process swbes -EA SilentlyContinue | Stop-Process -Force; exit 1 }
# move about: walk (W), strafe (A/D), turn, fire (left mouse = VK 01)
$end = (Get-Date).AddSeconds($Secs); $i = 0
while ((Get-Date) -lt $end) {
    $k = @('57', '41', '44', '57')[$i % 4]
    Set-Content "$run\key.cmd" "$k 1"; Set-Content "$run\look.cmd" "$(if ($i % 2) { 6 } else { -6 }) 0 60"
    Start-Sleep 2
    Set-Content "$run\key.cmd" "$k 0"; Set-Content "$run\key.cmd" "01 1"; Start-Sleep 1; Set-Content "$run\key.cmd" "01 0"
    $i++
}
& $sweep.FullName swbes.exe "$r\build-pgo\swbes!1.pgc"
Start-Sleep 2
Get-Process swbes -EA SilentlyContinue | Stop-Process -Force
Get-ChildItem "$r\build-pgo\*.pgc" | ForEach-Object { "$($_.Name) $([math]::Round($_.Length / 1MB, 1)) MB" }
