# Releasing

Versions are `MAJOR.MINOR.PATCH`. A hotfix bumps PATCH (0.1.0 -> 0.1.1).

Debug-only code does not ship: strip test hooks, debug env switches, logging and dump files before a release. Keep what crash logs or bug reports use.

1. Fix on a branch, or on `main` for a small hotfix.
2. Bump `VERSION` and add a section at the top of `CHANGELOG.md`.
3. Build: `scripts\build.ps1` (or `cmake --build build` if the EBOOT and generated C are unchanged). Release builds are configured with `-DPSPRECOMP_DEV=OFF` (the default); package.ps1 refuses a dev exe.
4. Package: `scripts\package.ps1`. It refuses an exe built from another version. It writes to `dist\`:
   - `swbes-<version>.zip`, the release;
   - `swbes-<version>-ffmpeg-source.zip`, which must be published with it (LGPL);
   - `swbes-<version>-SHA256SUMS.txt`.
5. Test the zip from scratch: extract it to an empty folder, add only the .iso, start a match.
6. Commit, tag `v<version>`, push, and create a GitHub release from the tag. Attach the three files and use the changelog section as the notes:
   `gh release create v<version> dist\swbes-<version>*.* --title v<version> --notes-file <section>`

Players update by extracting the new zip over their old folder. `game_data` (disc, eboot.elf, saves, settings, mods) is never in a release, so it is kept. A release must therefore keep reading older `settings.ini` files: new settings and binds fall back to their defaults.
When a default bind changes, add it to `settings_migrate` in `games/swbes/host.c` and bump its `CUR`: a file stamped older moves a bind still at the old default to the new one.

## Release strip list

Audited 2026-11-25 for the debug-only rule above. Re-check with
`grep -rn 'getenv("\|fopen("[a-z_]*\.cmd"' psprecomp/src games/swbes` before each release.

**Keep** (users, crash logs or bug reports):
- crash: `crash_filter` (host.c: exception and last guest functions in the log), `crash_ram.bin` (mem.c)
- `SWBES_GAME_DATA`, `PSPRECOMP_FFMPEG` (where to find files)
- `SWBES_RENDERER`, `PSPRECOMP_GL_SCALE`, `SWBES_FPS`, `SWBES_MOUSE_SENS`, `SWBES_MOUSE_INVERT` (documented overrides)
- `PSPRECOMP_REALCLOCK`, `PSPRECOMP_VBLFREE`: set by host.c itself for normal play; turn into plain flags, not env reads
- `SWBES_VCLOCK`: old-clock fallback, useful when a bug report blames pacing

**Strip** (test harness and dev-only):
- Command files polled from the working dir: host.c `key.cmd`, `ui.cmd`, `console.cmd`, `look.cmd`, `match.cmd`, `shot.cmd`, `ram.cmd`/`mod.bin`, `widgets.cmd`, `ev.cmd`, `act.cmd`, `evwatch.cmd`, `fnwatch.cmd`, `rwatch.cmd`, `rwatchall.cmd`, `wwatchall.cmd`, `watch.cmd`, `poke.cmd`, `peek.cmd`, `fnhist.cmd`, `spike.cmd`, `fps.cmd`, `pad.cmd`, `res.cmd`, `stick.cmd`, `mouse.cmd`, `cursor.cmd`, `status.tmp`; render_gl.c `shothi.cmd`, `gestats.cmd`, `gebench.cmd`, `nopost.cmd`, `fbdump.cmd`, `maxdraw.cmd`, `gltrace.cmd`, `showfb.cmd`; movie.c `key.cmd`, `pad.cmd`; misc.c `input.cmd`, `ram.cmd`, `shot.cmd`; ge.c `trace.cmd`, `skip.cmd`, `bonekill.cmd`
- Dumps: `ram.bin`, `vram.bin`, `live_hi.ppm`, `livehi.ppm`, `winshot.bmp`, `iconsheet.ppm`, `capsheet.ppm`, `padglyphs.ppm`, `hudcap.ppm`, `gebench.*`, `prof*.txt`
- Quick match: `SWBES_QUICKMATCH`, `SWBES_QM_MAP`, `SWBES_QM_NOPROFILE`, `SWBES_QM_NOSPAWN`, `SWBES_QM_CP`, `SWBES_QM_BOARD`, `SWBES_AUTOUSE`, `SWBES_SHOT`
- Test runs: `PSPRECOMP_MUTE`, `PSPRECOMP_SILENT`, `PSPRECOMP_PAINTBENCH`, `PSPRECOMP_BACKGROUND`, `PSPRECOMP_TURBO`, `PSPRECOMP_AUTOPRESS`, `PSPRECOMP_INPUTSEQ`, `PSPRECOMP_NOPACE`, `PSPRECOMP_PACE_HZ`, `SWBES_FPS_TRIM_US`
- Logs: `SWBES_VEHDBG`, `SWBES_HUMDBG`, `SWBES_TGTDBG`, `SWBES_HITLOG`, `SWBES_CHLOG`, `SWBES_OTHERLOG`, `SWBES_READLOG`, `SWBES_MOVELOG`, `SWBES_ZOOMLOG`, `SWBES_INPGLOG`, `SWBES_PGLOG`, `SWBES_SPRLOG`, `SWBES_GLYPHLOG`, `SWBES_GLYPHTEST`, `SWBES_WATCHTXT`, `SWBES_MAPDUMP`, `SWBES_WDUMP0`, `SWBES_WDUMPALL`, `SWBES_DUMPSHEET`, `SWBES_DUMPCAPS`, `SWBES_DUMPGLYPHS`, `SWBES_DUMPHUD`, `SWBES_PSP_PROMPTS`, `SWBES_STICK_MOVE`; `PSPRECOMP_TRACE_*` (HLE, IO, SAVE, GE, SAS, MEM, SCHED, DEVCTL), `PSPRECOMP_*LOG` (FPS, DELAY, VIEW, UV, TRX, GEBENCH, ATRAC, LIGHT, VNAN, VFAD, SKIN), `PSPRECOMP_GL_TRACE`, `PSPRECOMP_GL_ALL`, `PSPRECOMP_GL_DRAWSTATS`, `PSPRECOMP_GL_PROBE`, `PSPRECOMP_GL_DEPTHDBG`, `PSPRECOMP_UIDEBUG`, `PSPRECOMP_PROF`
- Watches and dumps: `PSPRECOMP_WATCH`, `PSPRECOMP_WATCHW`, `PSPRECOMP_WATCHW_FROM`, `PSPRECOMP_RWATCH`, `PSPRECOMP_TEXWATCH`, `PSPRECOMP_BONE_WATCH`, `PSPRECOMP_DUMP_RAM`, `PSPRECOMP_DUMP_VRAM`, `PSPRECOMP_TEX_DUMP`, `PSPRECOMP_MOVIE_DUMP`, `PSPRECOMP_MOVIE_SHOT`, `PSPRECOMP_AUDIO_WAV`, `PSPRECOMP_GEREPLAY`, `PSPRECOMP_GL_SHADOW`
- Experiment toggles (keep the winning path, drop the switch): `PSPRECOMP_GL_SKIPPOST`, `PSPRECOMP_GL_MAXDRAW`, `PSPRECOMP_GL_NO`, `PSPRECOMP_GL_NOFETCH`, `PSPRECOMP_GL_FORCEFETCH`, `PSPRECOMP_GL_FB16`, `PSPRECOMP_GL_ZINV`, `PSPRECOMP_GL_UNBIND`, `PSPRECOMP_GL_SYNCREAD`, `PSPRECOMP_GE_THREAD`, `PSPRECOMP_ZROUND`, `PSPRECOMP_CULL`, `PSPRECOMP_HUDTHIRDS`, `PSPRECOMP_SKIP_FBTEX`, `PSPRECOMP_VT_COUNT`, `PSPRECOMP_SKIN_USE`, `PSPRECOMP_TEX_ALWAYS`, `PSPRECOMP_NEARMODE`, `PSPRECOMP_VFAD_RANGE`, `PSPRECOMP_NOVFAD`, `PSPRECOMP_FLOAD_NAN`, `PSPRECOMP_FSTORE_NAN`, `PSPRECOMP_VFPU_NAN`, `PSPRECOMP_YIELD_TICK`, `PSPRECOMP_REST_REAL`, `PSPRECOMP_NODREAD`, `PSPRECOMP_NOLBN`, `PSPRECOMP_LBN_NOCLAMP`, `PSPRECOMP_HIDE_PMF`

Done by the `PSPRECOMP_DEV` build option (off by default; dev builds turn it on):
without it, envcache.c reads only the keep list above and never opens `*.cmd`
or `*.tmp` files, so every switch and command file in the strip list is inert.
Debug log lines (per-second `gl: read`, thread and allocation traces, idle-wait notices) are also `PSPRECOMP_DEV` only; a release log is startup info and real errors. `build-rel` is a local release build dir for checking this.
