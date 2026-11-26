/* psprecomp -- cached getenv (see envcache.h). */
#define PSPRECOMP_ENVCACHE_IMPL
#include "psprecomp/envcache.h"
#include <string.h>
#include <stdint.h>
#include <windows.h>
#undef getenv    /* force-included before PSPRECOMP_ENVCACHE_IMPL: this file uses the real ones */
#undef _putenv
#undef fopen

#ifndef PSPRECOMP_DEV
/* Release: the switches users may set (RELEASING.md, keep list); every other
 * PSPRECOMP_/SWBES_ name reads as unset. */
static int allowed(const char *n) {
    if (strncmp(n, "PSPRECOMP_", 10) && strncmp(n, "SWBES_", 6)) return 1;
    static const char *const keep[] = { "SWBES_GAME_DATA", "PSPRECOMP_FFMPEG", "SWBES_RENDERER", "PSPRECOMP_GL_SCALE", "SWBES_FPS",
        "SWBES_MOUSE_SENS", "SWBES_MOUSE_INVERT", "PSPRECOMP_REALCLOCK", "PSPRECOMP_VBLFREE", "SWBES_VCLOCK" };
    for (size_t i = 0; i < sizeof keep / sizeof *keep; i++) if (!strcmp(n, keep[i])) return 1;
    return 0;
}
#define getenv(n) (allowed(n) ? getenv(n) : NULL)
#endif

FILE *psp_fopen(const char *name, const char *mode) {
#ifndef PSPRECOMP_DEV
    const char *dot = strrchr(name, '.');   /* the test harness's command and status files */
    if (dot && !strpbrk(name, "\\/") && (!_stricmp(dot, ".cmd") || !_stricmp(dot, ".tmp"))) return NULL;
#endif
    return fopen(name, mode);
}

#define N 1024   /* open-addressed by the name's address (a string literal at almost every call) */
typedef struct { const char *key; char name[64]; char *val; volatile LONG ready; } slot;
static slot g_s[N];
static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_gen;

char *psp_getenv(const char *name) {
    const size_t len = strlen(name);
    if (len >= sizeof g_s[0].name) return getenv(name);
    uintptr_t h = ((uintptr_t)name >> 3) * 2654435761u;
    for (int i = 0; i < 16; i++) {
        slot *s = &g_s[(h + (uintptr_t)i) & (N - 1)];
        if (s->ready) {
            if (s->key == name && !strcmp(s->name, name)) return s->val;
            continue;
        }
        /* empty: fill it under the lock */
        AcquireSRWLockExclusive(&g_lock);
        char *v = getenv(name);
        if (!s->ready) { s->key = name; memcpy(s->name, name, len + 1); s->val = v; MemoryBarrier(); s->ready = 1; }
        ReleaseSRWLockExclusive(&g_lock);
        return v;
    }
    return getenv(name);
}

int psp_putenv(const char *str) {
    AcquireSRWLockExclusive(&g_lock);
    const int r = _putenv(str);
    for (int i = 0; i < N; i++) g_s[i].ready = 0;
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

const char *psp_logpath(const char *path) {
    static char buf[4][MAX_PATH + 16]; static volatile LONG k;
    if (!path) return "(null)";
    char *o = buf[InterlockedIncrement(&k) & 3];
    char cwd[MAX_PATH], prof[MAX_PATH];
    const DWORD nc = GetCurrentDirectoryA(MAX_PATH, cwd);
    if (nc && nc < MAX_PATH && !_strnicmp(path, cwd, nc) && (path[nc] == '\\' || path[nc] == '/')) {
        snprintf(o, MAX_PATH + 16, "%s", path + nc + 1); return o; }
    const DWORD np = GetEnvironmentVariableA("USERPROFILE", prof, MAX_PATH);
    if (np && np < MAX_PATH && !_strnicmp(path, prof, np) && (path[np] == '\\' || path[np] == '/' || !path[np])) {
        snprintf(o, MAX_PATH + 16, "%%USERPROFILE%%%s", path + np); return o; }
    snprintf(o, MAX_PATH + 16, "%s", path); return o;
}
