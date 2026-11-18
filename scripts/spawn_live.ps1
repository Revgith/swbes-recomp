# Launch swbes detached, muted, with the spawn menu sequence queued; leaves it running.
$r = (Split-Path $PSScriptRoot -Parent); $run = "$r\work\run"
Get-Process swbes -EA SilentlyContinue | Stop-Process -Force
Remove-Item "$run\shot*.ppm","$run\live.ppm","$run\shot.cmd","$run\ram.cmd" -EA SilentlyContinue
Copy-Item "$run\spawn_seq.txt" "$run\input.cmd"
$env:PSPRECOMP_MUTE = "1"; $env:PSPRECOMP_FPSLOG = "1"
$p = Start-Process "$r\build\swbes.exe" -ArgumentList "`"$run\swbes.elf`"" -WorkingDirectory $run -RedirectStandardOutput "$r\work\out.txt" -RedirectStandardError "$r\work\err.txt" -PassThru
$p.Id
