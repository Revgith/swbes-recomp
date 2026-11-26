/* psprecomp -- cached getenv.
 *
 * Debug switches are read with getenv() all over the hot paths (per draw, per
 * controller read). The CRT walks the whole environment on every call, which
 * showed up in profiles, so every getenv() in the project goes through a
 * small cache instead (force-included, see CMakeLists.txt). The environment
 * is fixed after startup; _putenv() goes through psp_putenv(), which clears
 * the cache.
 *
 * Release builds (no PSPRECOMP_DEV) also shut the developer harness: only
 * the user-facing PSPRECOMP_/SWBES_ switches are read (RELEASING.md, release
 * strip list), and the command files tests drop in the working directory
 * (*.cmd, and the *.tmp status files) are never opened. */
#ifndef PSPRECOMP_ENVCACHE_H
#define PSPRECOMP_ENVCACHE_H
#include <stdlib.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
char *psp_getenv(const char *name);
int   psp_putenv(const char *s);
FILE *psp_fopen(const char *name, const char *mode);
/* path as it goes in the log: relative under the game folder, the user's
 * profile folder as %USERPROFILE%, so a shared log carries no user name */
const char *psp_logpath(const char *path);
#ifdef __cplusplus
}
#endif
#ifndef PSPRECOMP_ENVCACHE_IMPL
#define getenv(n)  psp_getenv(n)
#define _putenv(s) psp_putenv(s)
#define fopen(n, m) psp_fopen(n, m)
#endif
#endif
