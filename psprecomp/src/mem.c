#include <windows.h>
/* psprecomp — PSP memory map. See include/psprecomp/mem.h. */

#include "psprecomp/mem.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/cpu.h"
#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

psp_memory psp_mem;
uint64_t   psp_mem_bad_access;

/* Watch writes to one address. "Which code writes this word" is a question
 * that came up repeatedly and could only be answered by guessing; the write
 * path is the one place that can answer it directly. */
/* Debug hooks cost a call on every guest access; with none of their env
 * vars set (checked once in psp_mem_init) they are skipped. */
static int g_mem_dbg;
uint8_t *psp_page[(PSP_ADDR_MASK + 1u) >> PSP_PAGE_SHIFT];
int psp_mem_slow;
static uint32_t g_rwatch;
static uint32_t g_wwatch;
static void slow_update(void) { psp_mem_slow = g_mem_dbg || g_rwatch || g_wwatch; }
static void map_pages(uint32_t base, uint32_t size, uint8_t *host) {
    for (uint32_t a = (base + PSP_PAGE_SIZE - 1) & ~(PSP_PAGE_SIZE - 1);
         host && a + PSP_PAGE_SIZE <= base + size && a < PSP_ADDR_MASK; a += PSP_PAGE_SIZE)
        psp_page[a >> PSP_PAGE_SHIFT] = host + (a - base);
}
static uint32_t g_wwatch;
static int g_whits;
void psp_mem_watch_write(uint32_t addr) { g_wwatch = addr; g_whits = 0; slow_update(); }
int psp_mem_watch_hits(void) { return g_whits; }

static void note_write(uint32_t addr, uint32_t width) {
    if (!g_wwatch || addr + width <= g_wwatch || addr > g_wwatch) return;
    { static long from = -2; if (from == -2) { const char *e = getenv("PSPRECOMP_WATCHW_FROM"); from = e ? atol(e) : -1; }
      if (from >= 0 && (long)psp_display_vblanks() < from) return; }
    if (g_whits++ < 8)
    {
        fprintf(stderr, "write%u @vb %ld to 0x%08X (watch 0x%08X) from fn 0x%08X ra 0x%08X calls", width * 8, (long)psp_display_vblanks(),
                addr, g_wwatch, psp_trace_last(), psp_cpu.r[PSP_REG_RA]);
        /* The return sites of the guest calls in flight, innermost first:
         * the writer is the body that called the first of them. */
        for (const psp_frame *f = psp_cpu.frame; f && f->ret; f = f->prev)
            fprintf(stderr, " 0x%08X", f->ret);
        fprintf(stderr, "\n");
    }
}

/* Watch reads of one address: "which code looks at this" is the question when
 * a value is right in memory and the game still does not act on it. A field
 * read every frame would repeat one name forever, so each distinct reading
 * function is printed once. */
static uint32_t g_rwatch;
static uint32_t g_rfns[16];
static int g_rfn_n;
void psp_mem_watch_read(uint32_t addr) { g_rwatch = addr; g_rfn_n = 0; slow_update(); }

static void note_read(uint32_t addr, uint32_t width) {
    if (!g_rwatch || addr + width <= g_rwatch || addr > g_rwatch) return;
    uint32_t fn = psp_trace_last();
    for (int i = 0; i < g_rfn_n; i++) if (g_rfns[i] == fn) return;
    if (g_rfn_n < 16) {
        g_rfns[g_rfn_n++] = fn;
        fprintf(stderr, "read%u of 0x%08X (watch 0x%08X) from fn 0x%08X\n",
                width * 8, addr, g_rwatch, fn);
    }
}

/* Bad accesses were only ever counted, which says an initialiser went wrong
 * without saying which one. The addresses are what identify it, and there are
 * few enough of them (28 in the current run) to simply print. */
static void bad_access(uint32_t addr, int write, int width) {
    /* The first one matters most: everything after it may be cascade from a
     * register that was already wrong. Dump the whole file once. */
    if (psp_mem_bad_access == 0) {
        static const char *N[32] = {
            "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3",
            "t4","t5","t6","t7","s0","s1","s2","s3","s4","s5","s6","s7",
            "t8","t9","k0","k1","gp","sp","fp","ra" };
        fprintf(stderr, "\n--- first bad access: %s%d at 0x%08X (last fn 0x%08X) ---\n",
                write ? "write" : "read", width * 8, addr, psp_trace_last());
        for (int i = 0; i < 32; i += 4)
            fprintf(stderr, "  %-3s=0x%08X  %-3s=0x%08X  %-3s=0x%08X  %-3s=0x%08X\n",
                    N[i], psp_cpu.r[i], N[i+1], psp_cpu.r[i+1],
                    N[i+2], psp_cpu.r[i+2], N[i+3], psp_cpu.r[i+3]);
        /* the memory as it was, for working out where the bad pointer came from */
        FILE *rf = fopen("crash_ram.bin", "wb"); if (rf) { fwrite(psp_mem.ram, 1, PSP_RAM_SIZE, rf); fclose(rf); }
    }
    if (psp_mem_bad_access < 32)
        fprintf(stderr, "psprecomp: bad %s%d at 0x%08X (last fn 0x%08X)\n",
                write ? "write" : "read", width * 8, addr, psp_trace_last());
    psp_mem_bad_access++;
}

int psp_mem_init(void) {
    g_mem_dbg = getenv("PSPRECOMP_RWATCH") || getenv("PSPRECOMP_WATCH") || getenv("PSPRECOMP_FLOAD_NAN");
    psp_mem.ram     = (uint8_t *)calloc(1, PSP_RAM_SIZE);
    psp_mem.vram    = (uint8_t *)calloc(1, PSP_VRAM_SIZE);
    psp_mem.scratch = (uint8_t *)calloc(1, PSP_SCRATCH_SIZE);
    if (!psp_mem.ram || !psp_mem.vram || !psp_mem.scratch) {
        psp_mem_free();
        return -1;
    }
    psp_mem_bad_access = 0;
    memset(psp_page, 0, sizeof psp_page);
    map_pages(PSP_RAM_BASE, PSP_RAM_SIZE, psp_mem.ram);
    map_pages(PSP_VRAM_BASE, PSP_VRAM_SIZE, psp_mem.vram);
    map_pages(PSP_SCRATCH_BASE, PSP_SCRATCH_SIZE, psp_mem.scratch);
    { const char *e = getenv("PSPRECOMP_WATCHW"); if (e) psp_mem_watch_write((uint32_t)strtoul(e, NULL, 16)); }
    slow_update();
    return 0;
}

void psp_mem_free(void) {
    free(psp_mem.ram);
    free(psp_mem.vram);
    free(psp_mem.scratch);
    psp_mem.ram = psp_mem.vram = psp_mem.scratch = NULL;
}

/* The loaded module image, for a PRX linked outside the RAM window. */
static uint8_t *g_module;
static uint32_t g_module_base, g_module_size;
void psp_mem_module_range(uint32_t *base, uint32_t *size) { *base = g_module_base; *size = g_module_size; }

int psp_mem_map_module(uint32_t base, uint32_t size) {
    free(g_module);
    g_module = (uint8_t *)calloc(1, size ? size : 1);
    if (!g_module) { g_module_size = 0; return -1; }
    g_module_base = base;
    g_module_size = size;
    map_pages(base, size, g_module);   /* checked first by psp_mem_ptr, so mapped last */
    return 0;
}

void *psp_mem_ptr(uint32_t addr, uint32_t size) {
    /* Collapse the three cache-behaviour mirrors onto one backing store. */
    const uint32_t a = addr & PSP_ADDR_MASK;

    /* Checked first: a module linked at 0 would otherwise fall through every
     * region test and read as unmapped. */
    if (g_module_size && a >= g_module_base && a < g_module_base + g_module_size) {
        uint32_t off = a - g_module_base;
        if (off + size > g_module_size) return NULL;
        return g_module + off;
    }

    if (a >= PSP_RAM_BASE && a < PSP_RAM_BASE + PSP_RAM_SIZE) {
        uint32_t off = a - PSP_RAM_BASE;
        if (off + size > PSP_RAM_SIZE) return NULL;   /* straddles the end */
        return psp_mem.ram + off;
    }
    if (a >= PSP_VRAM_BASE && a < PSP_VRAM_BASE + PSP_VRAM_SIZE) {
        uint32_t off = a - PSP_VRAM_BASE;
        if (off + size > PSP_VRAM_SIZE) return NULL;
        return psp_mem.vram + off;
    }
    if (a >= PSP_SCRATCH_BASE && a < PSP_SCRATCH_BASE + PSP_SCRATCH_SIZE) {
        uint32_t off = a - PSP_SCRATCH_BASE;
        if (off + size > PSP_SCRATCH_SIZE) return NULL;
        return psp_mem.scratch + off;
    }
    return NULL;
}

/* Reads of unmapped memory return 0 and are counted. Silently returning 0 is
 * what hardware roughly does, but a recompiled game should never be doing it
 * in a steady state — the counter is how you notice. */
#define READ_BODY(TYPE)                              \
    void *p = psp_mem_ptr(addr, (uint32_t)sizeof(TYPE)); \
    if (!p) { bad_access(addr, 0, (int)sizeof(TYPE)); return 0; }      \
    if (g_rwatch) note_read(addr, (uint32_t)sizeof(TYPE)); \
    TYPE v;                                          \
    memcpy(&v, p, sizeof v);                         \
    return v;

uint8_t  psp_read8_slow (uint32_t addr) { READ_BODY(uint8_t)  }
uint16_t psp_read16_slow(uint32_t addr) { READ_BODY(uint16_t) }
static void rwatch(uint32_t addr);
extern uint32_t psp_rwatch_live;
uint32_t psp_read32_slow(uint32_t addr) { if (psp_rwatch_live && addr == psp_rwatch_live) rwatch(addr); READ_BODY(uint32_t) }
/* PSPRECOMP_FLOAD_NAN=<lo>-<hi> (hex fn addresses): report float loads of a
 * NaN made inside that range of guest functions. */
static void fload_nan(uint32_t addr) {
    static int en = -1, n; static uint32_t lo, hi;
    if (en < 0) {
        const char *e = getenv("PSPRECOMP_FLOAD_NAN"); en = e != NULL;
        if (e) { lo = (uint32_t)strtoul(e, NULL, 16); const char *d = strchr(e, '-'); hi = d ? (uint32_t)strtoul(d + 1, NULL, 16) : lo; }
    }
    if (!en || n >= 25) return;
    const uint32_t fn = psp_trace_last();
    if (fn < lo || fn > hi) return;
    n++;
    fprintf(stderr, "fload: NaN %08X <- 0x%08X in fn 0x%08X ra 0x%08X s2 0x%08X s3 0x%08X\n",
            psp_read32(addr), addr, fn, psp_cpu.r[31], psp_cpu.r[18], psp_cpu.r[19]);
}
/* PSPRECOMP_RWATCH=<hex addr> or *<hex ptr>+<hex off>: report each distinct
 * guest function that loads a float from that address (debug aid). */
static void rwatch(uint32_t addr) {
    static int en = -1, deref; static uint32_t a, off, seen[64], ns;
    if (en < 0) { const char *e = getenv("PSPRECOMP_RWATCH"); en = e != NULL;
        if (e) { deref = *e == '*'; a = (uint32_t)strtoul(e + deref, NULL, 16); const char *p2 = strchr(e, '+'); off = p2 ? (uint32_t)strtoul(p2 + 1, NULL, 16) : 0; } }
    { extern uint32_t psp_rwatch_live; if (psp_rwatch_live) { if (psp_rwatch_live == 1) { ns = 0; psp_rwatch_live = 0; return; } en = 1; deref = 0; a = psp_rwatch_live; } }
    if (!en) return;
    uint32_t w = deref ? psp_read32(a) + off : a;
    if (addr != w || (deref && w == off)) return;
    uint32_t fn = psp_trace_last();
    for (uint32_t i = 0; i < ns; i++) if (seen[i] == fn) return;
    if (ns < 64) seen[ns++] = fn;
    fprintf(stderr, "rwatch: %08X read in fn 0x%08X ra 0x%08X\n", addr, fn, psp_cpu.r[31]);
}
float    psp_read_f32_slow(uint32_t addr) {
    if (psp_rwatch_live || g_mem_dbg) {
        rwatch(addr);
        void *pp = psp_mem_ptr(addr, 4);
        if (pp) { float f; memcpy(&f, pp, 4); if (f != f) fload_nan(addr); }
    }
    READ_BODY(float)
}

#define WRITE_BODY(TYPE)                             \
    void *p = psp_mem_ptr(addr, (uint32_t)sizeof(TYPE)); \
    if (!p) { bad_access(addr, 1, (int)sizeof(TYPE)); return; }         \
    if (g_wwatch) note_write(addr, (uint32_t)sizeof(TYPE));      \
    memcpy(p, &val, sizeof(TYPE));

void psp_write8_slow (uint32_t addr, uint8_t  val) { WRITE_BODY(uint8_t)  }
void psp_write16_slow(uint32_t addr, uint16_t val) { WRITE_BODY(uint16_t) }
/* PSPRECOMP_WATCH=<hex addr>: report writes to one word (debug aid). */
uint32_t psp_watch_live, psp_rwatch_live;   /* set at run time (host watch.cmd); 0 = off */
static void watch32(uint32_t addr, uint32_t val) {
    static long long w = -1; static int n;
    if (w == -1) { const char *e = getenv("PSPRECOMP_WATCH"); w = e ? (long long)strtoul(e, NULL, 16) : -2; }
    if (psp_watch_live && addr == psp_watch_live) {
        /* Host frames name the generated function exactly (map with the PDB);
         * psp_trace_last only tracks dispatched calls. */
        void *fr[4] = {0}; USHORT nf = CaptureStackBackTrace(1, 4, fr, NULL);
        uintptr_t mb = (uintptr_t)GetModuleHandleA(NULL), h1 = nf > 0 ? (uintptr_t)fr[0] - mb : 0, h2 = nf > 1 ? (uintptr_t)fr[1] - mb : 0;
        static uintptr_t seen[32]; static int ns; uint32_t fn = psp_trace_last(); int k; for (k = 0; k < ns; k++) if (seen[k] == (h1 ^ (h2 << 1))) break;
        if (k == ns && ns < 32) { seen[ns++] = h1 ^ (h2 << 1); fprintf(stderr, "wwatch: %08X <- %08X in fn 0x%08X ra 0x%08X a0 %08X s0 %08X s1 %08X host %llX %llX\n", addr, val, fn, psp_cpu.r[31], psp_cpu.r[4], psp_cpu.r[16], psp_cpu.r[17], (unsigned long long)h1, (unsigned long long)h2); } }
    static uint32_t last = 0xDEADBEEF;
    if (w >= 0 && addr == (uint32_t)w && val != last && n++ < 40 && ((last = val), 1))
        fprintf(stderr, "watch: %08X <- %08X (last fn 0x%08X, ra 0x%08X a1 0x%08X s0 0x%08X)\n", addr, val, psp_trace_last(), psp_cpu.r[31], psp_cpu.r[5], psp_cpu.r[16]);
}
void psp_write32_slow(uint32_t addr, uint32_t val) { if (psp_watch_live || g_mem_dbg) watch32(addr, val); WRITE_BODY(uint32_t) }
void psp_write_f32_slow(uint32_t addr, float  val) {
    uint32_t b; memcpy(&b, &val, 4); if (psp_watch_live || g_mem_dbg) watch32(addr, b);
    if (val != val) {   /* PSPRECOMP_FSTORE_NAN=1: first float stores of a NaN (not the VFPU memcpy) */
        static int en = -1, n;
        if (en < 0) { const char *e = getenv("PSPRECOMP_FSTORE_NAN"); en = e && *e == '1'; }
        const uint32_t fn = psp_trace_last();
        if (en && !(fn >= 0x0038BF98u && fn < 0x0038C390u) && n++ < 25)
            fprintf(stderr, "fstore: NaN %08X -> 0x%08X in fn 0x%08X ra 0x%08X\n", b, addr, fn, psp_cpu.r[31]);
    }
    WRITE_BODY(float)
}

int psp_mem_write_block(uint32_t addr, const void *src, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    note_write(addr, len);
    memcpy(p, src, len);
    return 0;
}

int psp_mem_read_block(void *dst, uint32_t addr, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(dst, p, len);
    return 0;
}

void psp_fnan_report(uint32_t pc, float a, float b) {
    static int n;
    if (n++ < 20)
        fprintf(stderr, "fpu: NaN/inf born at 0x%08X from %g, %g (fn 0x%08X)\n", pc, (double)a, (double)b, psp_trace_last());
}
