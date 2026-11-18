/* Sampling profiler (debug). PSPRECOMP_PROF="<delay s> <length s>" (default
 * "30 15"): after the delay, every 1 ms each other thread of the process is
 * suspended and its instruction pointer recorded; at the end the samples are
 * resolved to symbols (exe PDB, system DLL exports) and the top functions,
 * per thread and overall, are written to prof.txt in the working directory. */
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "dbghelp.lib")

#define MAXT 64
#define HBITS 16
typedef struct { DWORD64 ip; DWORD tid; unsigned n; } slot_t;
static slot_t g_tab[1 << HBITS];
static DWORD g_tids[MAXT]; static unsigned g_tsamp[MAXT]; static int g_nt;

static void add(DWORD tid, DWORD64 ip) {
    unsigned h = (unsigned)((ip * 0x9E3779B97F4A7C15ull) >> (64 - HBITS));
    for (;;) {
        slot_t *s = &g_tab[h];
        if (!s->n) { s->ip = ip; s->tid = tid; s->n = 1; return; }
        if (s->ip == ip && s->tid == tid) { s->n++; return; }
        h = (h + 1) & ((1u << HBITS) - 1);
    }
}
typedef struct { char name[160]; DWORD tid; unsigned n; } fn_t;
static int by_n(const void *a, const void *b) { return (int)((const fn_t *)b)->n - (int)((const fn_t *)a)->n; }

static DWORD WINAPI prof_thread(LPVOID arg) {
    (void)arg;
    double delay = 30, len = 15; sscanf(getenv("PSPRECOMP_PROF"), "%lf %lf", &delay, &len);
    Sleep((DWORD)(delay * 1000));
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    timeBeginPeriod(1);
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)(len * 1000);
    unsigned total = 0, rounds = 0; HANDLE th[MAXT] = { 0 }; ULONGLONG next_enum = 0;
    while (GetTickCount64() < end) {
        if (GetTickCount64() >= next_enum) {   /* thread list, refreshed each second; handles stay open */
            next_enum = GetTickCount64() + 1000;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te = { sizeof te };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
                int i = 0; while (i < g_nt && g_tids[i] != te.th32ThreadID) i++;
                if (i == g_nt && g_nt < MAXT) { g_tids[g_nt] = te.th32ThreadID; th[g_nt++] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID); }
            }
            CloseHandle(snap);
        }
        for (int i = 0; i < g_nt; i++) {
            if (!th[i] || SuspendThread(th[i]) == (DWORD)-1) continue;
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th[i], &c)) { add(g_tids[i], c.Rip); total++; g_tsamp[i]++; }
            ResumeThread(th[i]);
        }
        rounds++;
        Sleep(1);
    }
    fprintf(stderr, "prof: %u rounds\n", rounds);
    /* resolve: group samples by (thread, symbol) */
    HANDLE p = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(p, NULL, TRUE);
    static fn_t fns[1 << HBITS]; int nf = 0;
    static fn_t all[1 << HBITS]; int na = 0;
    for (unsigned i = 0; i < (1u << HBITS); i++) {
        if (!g_tab[i].n) continue;
        char buf[sizeof(SYMBOL_INFO) + 128]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 127; DWORD64 disp = 0;
        char name[160];
        if (SymFromAddr(p, g_tab[i].ip, &disp, si)) {
            IMAGEHLP_MODULE64 mi = { sizeof mi };
            if (SymGetModuleInfo64(p, g_tab[i].ip, &mi)) snprintf(name, sizeof name, "%s!%s", mi.ModuleName, si->Name);
            else snprintf(name, sizeof name, "%s", si->Name);
        } else {
            IMAGEHLP_MODULE64 mi = { sizeof mi };
            if (SymGetModuleInfo64(p, g_tab[i].ip, &mi)) snprintf(name, sizeof name, "%s!?", mi.ModuleName);
            else snprintf(name, sizeof name, "?%llX", (unsigned long long)g_tab[i].ip);
        }
        int j = 0; while (j < nf && (fns[j].tid != g_tab[i].tid || strcmp(fns[j].name, name))) j++;
        if (j == nf) { strcpy(fns[nf].name, name); fns[nf].tid = g_tab[i].tid; fns[nf].n = 0; nf++; }
        fns[j].n += g_tab[i].n;
        j = 0; while (j < na && strcmp(all[j].name, name)) j++;
        if (j == na) { strcpy(all[na].name, name); all[na].n = 0; na++; }
        all[j].n += g_tab[i].n;
    }
    qsort(fns, (size_t)nf, sizeof fns[0], by_n); qsort(all, (size_t)na, sizeof all[0], by_n);
    FILE *f = fopen("prof.txt", "w"); if (!f) return 0;
    fprintf(f, "%u samples over %.0f s\n\n== threads ==\n", total, len);
    for (int i = 0; i < g_nt; i++) {
        fprintf(f, "thread %5lu  %u samples\n", g_tids[i], g_tsamp[i]);
        int shown = 0;
        for (int j = 0; j < nf && shown < 30; j++) if (fns[j].tid == g_tids[i] && g_tsamp[i] > total / 50) {
            fprintf(f, "    %5.1f%%  %s\n", 100.0 * fns[j].n / g_tsamp[i], fns[j].name); shown++; }
    }
    fprintf(f, "\n== overall ==\n");
    for (int j = 0; j < na && j < 60; j++) fprintf(f, "%5.1f%%  %s\n", 100.0 * all[j].n / (total ? total : 1), all[j].name);
    fclose(f);
    if ((f = fopen("prof_all.txt", "w")) != NULL) {   /* every (thread, function, samples), for grouping */
        for (int j = 0; j < nf; j++) fprintf(f, "%lu\t%u\t%s\n", fns[j].tid, fns[j].n, fns[j].name);
        fclose(f); }
    fprintf(stderr, "prof: wrote prof.txt (%u samples)\n", total);
    return 0;
}
void psp_prof_start(void) {
    if (getenv("PSPRECOMP_PROF")) CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
}
#else
void psp_prof_start(void) {}
#endif
