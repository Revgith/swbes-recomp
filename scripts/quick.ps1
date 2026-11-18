# Launch swbes muted straight into a match (debug quick match), leaves it running.
$r = (Split-Path $PSScriptRoot -Parent); $run = "$r\work\run"
Get-Process swbes -EA SilentlyContinue | Stop-Process -Force
Remove-Item "$run\input.cmd","$run\match.cmd","$run\status.txt","$run\key.cmd","$run\look.cmd","$run\console.cmd","$run\fnhist.cmd" -EA SilentlyContinue
$env:PSPRECOMP_MUTE = "1"; $env:SWBES_QUICKMATCH = $(if ($env:QM) { $env:QM } else { "1" })
$exe = if ($env:SWBES_EXE) { $env:SWBES_EXE } else { "$r\build\swbes.exe" }
$p = Start-Process -WindowStyle Hidden $exe -ArgumentList "`"$run\swbes.elf`"" -WorkingDirectory $run -RedirectStandardOutput "$r\work\out.txt" -RedirectStandardError "$r\work\err.txt" -PassThru
$p.Id
