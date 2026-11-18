/* psprecomp -- cached getenv.
 *
 * Debug switches are read with getenv() all over the hot paths (per draw, per
 * controller read). The CRT walks the whole environment on every call, which
 * showed up in profiles, so every getenv() in the project goes through a
 * small cache instead (force-included, see CMakeLists.txt). The environment
 * is fixed after startup; _putenv() goes through psp_putenv(), which clears
 * the cache. */
#ifndef PSPRECOMP_ENVCACHE_H
#define PSPRECOMP_ENVCACHE_H
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
char *psp_getenv(const char *name);
int   psp_putenv(const char *s);
#ifdef __cplusplus
}
#endif
#ifndef PSPRECOMP_ENVCACHE_IMPL
#define getenv(n)  psp_getenv(n)
#define _putenv(s) psp_putenv(s)
#endif
#endif
