# Packs a built release into dist\. Contains no game files. See RELEASING.md.
#   swbes-<version>.zip                 the release
#   swbes-<version>-ffmpeg-source.zip   ffmpeg's source (LGPL), published beside it
#   swbes-<version>-SHA256SUMS.txt      checksums of both
# The version comes from VERSION; -Version overrides it.
param([string]$Version)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Version) { $Version = (Get-Content "$root\VERSION" -TotalCount 1).Trim() }
$b = Join-Path $root "build"
foreach ($f in "swbes.exe", "ebootdec.exe") { if (-not (Test-Path "$b\$f")) { throw "build\$f missing: run scripts\build.ps1 first" } }
# A stale exe would ship under the new number: it must carry this version.
if (-not (Select-String -Path "$b\swbes.exe" -Pattern "v$Version" -SimpleMatch -Quiet)) { throw "build\swbes.exe is not version ${Version}: rebuild first" }
# Debug-only code does not ship (RELEASING.md): the exe must be built without PSPRECOMP_DEV.
if (Select-String -Path "$b\swbes.exe" -Pattern "psprecomp dev harness build" -SimpleMatch -Quiet) { throw "build\swbes.exe is a dev build: configure with -DPSPRECOMP_DEV=OFF and rebuild" }

$name = "swbes-$Version"
$dist = Join-Path $root "dist"
$out = Join-Path $dist $name
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\game_data", "$out\docs", "$out\licenses\ebootdec-source" | Out-Null

Copy-Item "$b\swbes.exe", "$b\ebootdec.exe" $out
if (Test-Path "$b\ffmpeg.exe") { Copy-Item "$b\ffmpeg.exe" $out } else { Write-Warning "no ffmpeg.exe: movies will be skipped" }
Copy-Item "$root\game_data\README.txt" "$out\game_data"
Copy-Item "$root\README.md", "$root\LICENSE", "$root\THIRD_PARTY.md", "$root\CHANGELOG.md" $out
Copy-Item "$root\games\swbes\CONTROLS.md", "$root\games\swbes\MODDING.md" "$out\docs"
Copy-Item "$root\games\swbes\mods" "$out\game_data" -Recurse   # bundled mods (game_data is where the game runs); on=0 in mod.ini keeps one off until switched on
Copy-Item "$root\psprecomp\LICENSE" "$out\licenses\psprecomp-LICENSE.txt"
Copy-Item "$root\psprecomp\third_party\imgui\LICENSE.txt" "$out\licenses\imgui-LICENSE.txt"
Copy-Item "$root\tools\ebootdec\*" "$out\licenses\ebootdec-source" -Recurse

# ffmpeg is LGPL: ship its licence and build script, and its source beside the release.
$ffsrc = Join-Path $root "third_party\ffmpeg-src"
if (Test-Path "$out\ffmpeg.exe") {
    if (-not (Test-Path $ffsrc)) { throw "third_party\ffmpeg-src missing: the ffmpeg source must ship with ffmpeg.exe" }
    New-Item -ItemType Directory -Force "$out\licenses\ffmpeg" | Out-Null
    Copy-Item "$root\third_party\ffmpeg-min\COPYING.LGPLv2.1", "$root\scripts\build_ffmpeg.sh" "$out\licenses\ffmpeg"
    Set-Content "$out\licenses\ffmpeg\SOURCE.txt" "ffmpeg.exe is FFmpeg 7.1 (LGPL 2.1), built by build_ffmpeg.sh.`r`nIts source is published beside this release as $name-ffmpeg-source.zip, and at https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz."
}

# Nothing from the game may ship.
$bad = Get-ChildItem $out -Recurse -File | Where-Object { $_.Extension -in ".iso", ".elf", ".bin", ".cso" -or $_.Name -like "EBOOT.*" }
if ($bad) { throw "game files in the package: $($bad.FullName -join ', ')" }

$zips = @("$dist\$name.zip")
Remove-Item $zips[0] -EA SilentlyContinue
Compress-Archive -Path "$out\*" -DestinationPath $zips[0]
if (Test-Path "$out\ffmpeg.exe") {
    $fz = "$dist\$name-ffmpeg-source.zip"; $tmp = "$dist\ffmpeg-source"; $zips += $fz
    # The ffmpeg source doesn't change between releases: reuse an existing zip.
    $prev = Get-ChildItem "$dist\swbes-*-ffmpeg-source.zip" -EA SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not (Test-Path $fz) -and $prev) { Copy-Item $prev.FullName $fz }
    if (-not (Test-Path $fz)) {
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        robocopy $ffsrc "$tmp\ffmpeg-7.1" /E /XF *.o *.a *.d *.exe *.dll *.pc *.lib /NFL /NDL /NJH /NJS /NP | Out-Null
        Copy-Item "$root\scripts\build_ffmpeg.sh" $tmp
        Compress-Archive -Path "$tmp\*" -DestinationPath $fz; Remove-Item -Recurse -Force $tmp
    }
}
$zips | ForEach-Object { "$((Get-FileHash $_ -Algorithm SHA256).Hash.ToLower())  $(Split-Path $_ -Leaf)" } | Set-Content "$dist\$name-SHA256SUMS.txt" -Encoding ascii
$zips | ForEach-Object { Write-Host "packed $_ ($([math]::Round((Get-Item $_).Length / 1MB, 1)) MB)" }
