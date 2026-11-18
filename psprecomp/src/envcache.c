/* psprecomp -- cached getenv (see envcache.h). */
#define PSPRECOMP_ENVCACHE_IMPL
#include "psprecomp/envcache.h"
#include <string.h>
#include <stdint.h>
#include <windows.h>
#undef getenv    /* force-included before PSPRECOMP_ENVCACHE_IMPL: this file uses the real ones */
#undef _putenv

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
