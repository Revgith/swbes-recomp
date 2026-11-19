# Releasing

Versions are `MAJOR.MINOR.PATCH`. A hotfix bumps PATCH (0.1.0 -> 0.1.1).

1. Fix on a branch, or on `main` for a small hotfix.
2. Bump `VERSION` and add a section at the top of `CHANGELOG.md`.
3. Build: `scripts\build.ps1` (or `cmake --build build` if the EBOOT and generated C are unchanged).
4. Package: `scripts\package.ps1`. It refuses an exe built from another version. It writes to `dist\`:
   - `swbes-<version>.zip`, the release;
   - `swbes-<version>-ffmpeg-source.zip`, which must be published with it (LGPL);
   - `swbes-<version>-SHA256SUMS.txt`.
5. Test the zip from scratch: extract it to an empty folder, add only the .iso, start a match.
6. Commit, tag `v<version>`, push, and create a GitHub release from the tag. Attach the three files and use the changelog section as the notes:
   `gh release create v<version> dist\swbes-<version>*.* --title v<version> --notes-file <section>`

Players update by extracting the new zip over their old folder. `game_data` (disc, eboot.elf, saves, settings, mods) is never in a release, so it is kept. A release must therefore keep reading older `settings.ini` files: new settings and binds fall back to their defaults.
When a default bind changes, add it to `settings_migrate` in `games/swbes/host.c` and bump its `CUR`: a file stamped older moves a bind still at the old default to the new one.
