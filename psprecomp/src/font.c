/* psprecomp -- real-font text (see font.h). */
#include "psprecomp/font.h"
#include "psprecomp/mods.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

#ifdef _WIN32
#include <windows.h>
#endif

static uint64_t g_hash[16]; static int g_nh;
static const psp_font_glyph *g_gl; static int g_ng;
static float g_cap;

static unsigned g_gen;           /* mods generation the font was loaded for */
static unsigned char *g_ttf; static stbtt_fontinfo g_fi; static int g_ok;
static float g_capu;             /* the font's cap height, in font units */

#define CW 2048
#define CH 2048
static uint32_t *g_px; static int g_sx, g_sy, g_rowh;   /* shelf packing */
static int g_dy0 = CH, g_dy1;   /* rows changed since the last upload */
static void mark(int y0, int y1) { if (y0 < g_dy0) g_dy0 = y0; if (y1 > g_dy1) g_dy1 = y1; }
typedef struct { uint32_t cp; int cap4, xs64; psp_font_bm bm; int ok; } ent;
static ent *g_ent; static int g_nent, g_capent;

void psp_font_atlas(const uint64_t *hashes, int nh, const psp_font_glyph *g, int ng) {
    g_nh = nh > 16 ? 16 : nh; memcpy(g_hash, hashes, sizeof *hashes * (size_t)g_nh);
    g_gl = g; g_ng = ng; g_cap = 0;
    for (int i = 0; i < ng; i++) if (g[i].cp == 'H') g_cap = (float)(g[i].y1 - g[i].y0) - 1.0f;   /* less the soft fringe */
}
float psp_font_cap(void) { return g_cap; }

static void reset_cache(void) {
    if (!g_px) g_px = (uint32_t *)malloc((size_t)CW * CH * 4);
    if (g_px) memset(g_px, 0, (size_t)CW * CH * 4);
    g_sx = g_sy = g_rowh = 0; g_nent = 0; mark(0, CH);
}

static void load(void) {
    const unsigned gen = psp_mods_generation();
    if (gen == g_gen) return;
    g_gen = gen; g_ok = 0; free(g_ttf); g_ttf = NULL; reset_cache();
#ifdef _WIN32
    char path[MAX_PATH] = "";
    for (int i = 0; i < psp_mods_count(); i++) {   /* the last enabled mod with a font wins */
        const psp_mod_t *m = psp_mods_get(i); if (!m || !m->on) continue;
        static const char *const ext[] = { "*.ttf", "*.otf" };
        for (int e = 0; e < 2; e++) {
            char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\%s\\%s", psp_mods_root(), m->dir, ext[e]);
            WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(pat, &fd);
            if (fh == INVALID_HANDLE_VALUE) continue;
            snprintf(path, sizeof path, "%s\\%s\\%s", psp_mods_root(), m->dir, fd.cFileName);
            FindClose(fh);
        }
    }
    if (!path[0]) return;
    FILE *f = fopen(path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    g_ttf = n > 0 ? (unsigned char *)malloc((size_t)n) : NULL;
    if (!g_ttf || fread(g_ttf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(g_ttf); g_ttf = NULL; return; }
    fclose(f);
    if (!stbtt_InitFont(&g_fi, g_ttf, stbtt_GetFontOffsetForIndex(g_ttf, 0))) { fprintf(stderr, "font: could not read %s\n", psp_logpath(path)); return; }
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(&g_fi, 'H', &x0, &y0, &x1, &y1) || y1 <= y0) { fprintf(stderr, "font: %s has no H\n", psp_logpath(path)); return; }
    g_capu = (float)(y1 - y0); g_ok = 1;
    fprintf(stderr, "font: %s\n", psp_logpath(path));
#endif
}

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    return 0.0;
#endif
}
int psp_font_on(uint64_t hash) {
    if (!g_ng) return 0;
    int k = 0; while (k < g_nh && g_hash[k] != hash) k++;
    if (k == g_nh) return 0;
    if (psp_mods_generation() != g_gen && getenv("PSPRECOMP_FONTDBG")) { const double t = now_ms(); load(); fprintf(stderr, "font: load %.2f ms\n", now_ms() - t); }
    load();
    return g_ok;
}

const psp_font_glyph *psp_font_find(float u0, float v0, float u1, float v1) {
    if (u0 > u1) { const float t = u0; u0 = u1; u1 = t; }
    if (v0 > v1) { const float t = v0; v0 = v1; v1 = t; }
    const psp_font_glyph *best = NULL; float bd = 1e9f;
    const float cu = (u0 + u1) * 0.5f, cv = (v0 + v1) * 0.5f;
    for (int i = 0; i < g_ng; i++) {   /* the ink box whose centre is inside the cell, nearest its centre */
        const psp_font_glyph *g = &g_gl[i];
        const float gx = (g->x0 + g->x1) * 0.5f, gy = (g->y0 + g->y1) * 0.5f;
        if (gx < u0 || gx > u1 || gy < v0 || gy > v1) continue;
        const float d = fabsf(gx - cu) + fabsf(gy - cv);
        if (d < bd) { bd = d; best = g; }
    }
    return best;
}

int psp_font_get(uint32_t cp, uint32_t prev, float cap, float xs, psp_font_bm *out, float *kern) {
    if (!g_ok || !g_px) return 0;
    const int cap4 = (int)floorf(cap * 4.0f + 0.5f), xs64 = (int)floorf(xs * 64.0f + 0.5f);
    const float sy = (float)cap4 / 4.0f / g_capu, sx = sy * (float)xs64 / 64.0f;
    if (kern) *kern = prev ? (float)stbtt_GetCodepointKernAdvance(&g_fi, (int)prev, (int)cp) * sx : 0.0f;
    for (int i = 0; i < g_nent; i++)
        if (g_ent[i].cp == cp && g_ent[i].cap4 == cap4 && g_ent[i].xs64 == xs64) { *out = g_ent[i].bm; return g_ent[i].ok; }
    if (g_nent == g_capent) { g_capent = g_capent ? g_capent * 2 : 256; g_ent = (ent *)realloc(g_ent, sizeof *g_ent * (size_t)g_capent); }
    ent *e = &g_ent[g_nent++]; memset(e, 0, sizeof *e); e->cp = cp; e->cap4 = cap4; e->xs64 = xs64;
    const int gi = stbtt_FindGlyphIndex(&g_fi, (int)cp);
    int adv, lsb; stbtt_GetGlyphHMetrics(&g_fi, gi, &adv, &lsb);
    e->bm.adv = (float)adv * sx;
    if (!gi) { *out = e->bm; return 0; }
    int x0, y0, x1, y1; stbtt_GetGlyphBitmapBox(&g_fi, gi, sx, sy, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    e->bm.xoff = (float)x0; e->bm.yoff = (float)y0; e->bm.w = w; e->bm.h = h; e->ok = 1;
    if (w > 0 && h > 0) {
        if (g_sx + w + 2 > CW) { g_sx = 0; g_sy += g_rowh + 2; g_rowh = 0; }
        if (g_sy + h + 2 > CH) { reset_cache(); return psp_font_get(cp, prev, cap, xs, out, kern); }   /* full: start over */
        unsigned char *a = (unsigned char *)malloc((size_t)w * h);
        if (a) {
            const double t = now_ms();
            stbtt_MakeGlyphBitmap(&g_fi, a, w, h, w, sx, sy, gi);
            if (getenv("PSPRECOMP_FONTDBG")) fprintf(stderr, "font: glyph %u cap %.2f %dx%d %.3f ms\n", (unsigned)cp, cap, w, h, now_ms() - t);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
                g_px[(size_t)(g_sy + 1 + y) * CW + g_sx + 1 + x] = (uint32_t)a[y * w + x] << 24 | 0xFFFFFFu;
            free(a);
        }
        e->bm.x = g_sx + 1; e->bm.y = g_sy + 1;
        g_sx += w + 2; if (h > g_rowh) g_rowh = h;
        mark(g_sy, g_sy + h + 2);
    }
    *out = e->bm;
    return 1;
}

const uint32_t *psp_font_cache(int *w, int *h, int *y0, int *y1) {
    *w = CW; *h = CH; *y0 = g_dy0; *y1 = g_dy1; g_dy0 = CH; g_dy1 = 0;
    return g_px;
}
