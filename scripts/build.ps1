# Build allegrexrecomp, regenerate the C from your EBOOT dump, build swbes.exe.
# The EBOOT is any decrypted ELF in game_data (eboot.elf, or a PPSSPP dump);
# without one, ebootdec decrypts it from the disc there. -Elf names one explicitly.
param([switch]$NoEmit, [string]$Elf)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;" + $env:PATH
$vc = & vswhere -latest -property installationPath
$vcvars = "$vc\VC\Auxiliary\Build\vcvars64.bat"
$b = "$root\build"
# Source sanity: a file re-saved through the wrong encoding turns UTF-8 text (key-cap arrows) into
# mojibake (U+00E2 / U+00C3 followed by a cp1252 byte), and that still compiles. Stop the build
# instead. Keep this file ASCII: PowerShell 5.1 reads a BOM-less .ps1 as ANSI.
$bad = Get-ChildItem "$root\games", "$root\psprecomp\src", "$root\psprecomp\include", "$root\psprecomp\tools" -Recurse -Include *.c, *.h, *.cpp, *.txt -File |
  Where-Object { $_.FullName -notmatch '\\third_party\\' } | Where-Object {
    $t = [IO.File]::ReadAllText($_.FullName, [Text.Encoding]::UTF8)
    $hi = "[$([char]0x80)-$([char]0xBF)$([char]0x152)-$([char]0x2122)]"; $t.Contains([char]0xFFFD) -or $t -cmatch "$([char]0xE2)$hi|$([char]0xC3)$hi" }
if ($bad) { throw "Encoding damage (mojibake or invalid UTF-8) in:`n  $($bad.FullName -join "`n  ")" }
cmd /c "`"$vcvars`" >nul && cmake -S `"$root`" -B `"$b`" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >nul && cmake --build `"$b`" --target allegrexrecomp"
if (-not $NoEmit) {
  if (-not $Elf) {
    $Elf = Get-ChildItem "$root\game_data" -File -EA SilentlyContinue | Where-Object {
      $s = [IO.File]::OpenRead($_.FullName); $h = New-Object byte[] 4; [void]$s.Read($h, 0, 4); $s.Close()
      $h[0] -eq 0x7F -and $h[1] -eq 0x45 -and $h[2] -eq 0x4C -and $h[3] -eq 0x46 } | Select-Object -First 1 -ExpandProperty FullName
  }
  if (-not $Elf) {   # decrypt the EBOOT from the disc (extracted folder, or the .iso mounted read-only)
    $src = Get-ChildItem "$root\game_data" -Recurse -Depth 3 -Filter EBOOT.BIN -EA SilentlyContinue | Where-Object { $_.Directory.Name -eq 'SYSDIR' } | Select-Object -First 1 -ExpandProperty FullName
    $iso = Get-ChildItem "$root\game_data" -Filter *.iso -File -EA SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
    cmd /c "`"$vcvars`" >nul && cmake --build `"$b`" --target ebootdec"
    $mounted = $null
    if (-not $src -and $iso) { $mounted = Mount-DiskImage -ImagePath $iso -Access ReadOnly -PassThru; $src = "$(($mounted | Get-Volume).DriveLetter):\PSP_GAME\SYSDIR\EBOOT.BIN" }
    if (-not $src) { throw "No disc in game_data\ (put your .iso or its extracted files there)" }
    try { & "$b\ebootdec.exe" $src "$root\game_data\eboot.elf"; if ($LASTEXITCODE) { throw "ebootdec failed on $src ($LASTEXITCODE)" } }
    finally { if ($mounted) { Dismount-DiskImage -ImagePath $iso | Out-Null } }
    $Elf = "$root\game_data\eboot.elf"
  }
  "recompiling $Elf"
  # Function counts change between emits; leftovers would double-define symbols.
  if (Test-Path "$b\gen") { Get-ChildItem "$b\gen" -Filter "swbes_*" | Remove-Item -Force }
  New-Item -ItemType Directory -Force "$b\gen" | Out-Null
  & "$b\psprecomp\tools\allegrexrecomp\allegrexrecomp.exe" emit "$Elf" "$b\gen" swbes | Select-Object -Last 3
}
cmd /c "`"$vcvars`" >nul && cmake -S `"$root`" -B `"$b`" >nul && cmake --build `"$b`" --target swbes"
