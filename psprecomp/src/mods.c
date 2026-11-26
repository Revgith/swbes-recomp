/* psprecomp -- mods (see mods.h). Windows: PNGs through WIC, no dependencies. */
#include "psprecomp/mods.h"
#include "psprecomp/window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>

#define MAX_MODS 64

static psp_mod_t g_mod[MAX_MODS];
static int g_nmod, g_inited, g_dump;
static volatile LONG g_gen = 1;
static char g_root[MAX_PATH];
static CRITICAL_SECTION g_lock;

/* Texture index: hash -> (mod, loaded image). Rebuilt when the set changes. */
typedef struct { uint64_t hash; int mod; uint32_t *px; int w, h, failed; } tex_ent;
static tex_ent *g_tex; static int g_ntex, g_captex;
/* Hashes already dumped this run. */
static uint64_t *g_dumped; static int g_ndumped, g_capdumped;

const char *psp_mods_root(void) { psp_mods_init(); return g_root; }

static void ini_read(const char *ini, const char *key, char *out, int cap) {
    GetPrivateProfileStringA("mod", key, "", out, (DWORD)cap, ini);
}

static int cmp_dir(const void *a, const void *b) { return _stricmp(((const psp_mod_t *)a)->dir, ((const psp_mod_t *)b)->dir); }

static void index_textures(void) {   /* g_lock held */
    for (int i = 0; i < g_ntex; i++) free(g_tex[i].px);
    g_ntex = 0;
    for (int m = 0; m < g_nmod; m++) {
        g_mod[m].textures = 0;
        char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\%s\\textures\\*.png", g_root, g_mod[m].dir);
        WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(pat, &fd);
        if (fh == INVALID_HANDLE_VALUE) continue;
        do {
            char *end; const uint64_t h = _strtoui64(fd.cFileName, &end, 16);
            if (end - fd.cFileName != 16 || _stricmp(end, ".png")) continue;
            g_mod[m].textures++;
            if (!g_mod[m].on) continue;
            int k = 0; for (; k < g_ntex; k++) if (g_tex[k].hash == h) break;   /* later mods win */
            if (k == g_ntex) {
                if (g_ntex == g_captex) { g_captex = g_captex ? g_captex * 2 : 256; g_tex = (tex_ent *)realloc(g_tex, sizeof *g_tex * (size_t)g_captex); }
                g_ntex++;
            }
            g_tex[k].hash = h; g_tex[k].mod = m; g_tex[k].px = NULL; g_tex[k].w = g_tex[k].h = 0; g_tex[k].failed = 0;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
}

void psp_mods_init(void) {
    if (g_inited) return;
    g_inited = 1;
    InitializeCriticalSection(&g_lock);
    GetCurrentDirectoryA(MAX_PATH - 8, g_root); strcat(g_root, "\\mods");
    CreateDirectoryA(g_root, NULL);
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", g_root);
    WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || g_nmod >= MAX_MODS) continue;
            if (strlen(fd.cFileName) >= sizeof g_mod[0].dir) continue;
            psp_mod_t *m = &g_mod[g_nmod++]; memset(m, 0, sizeof *m);
            snprintf(m->dir, sizeof m->dir, "%s", fd.cFileName);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    qsort(g_mod, (size_t)g_nmod, sizeof g_mod[0], cmp_dir);
    for (int i = 0; i < g_nmod; i++) {
        psp_mod_t *m = &g_mod[i];
        char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%s\\%s\\mod.ini", g_root, m->dir);
        ini_read(ini, "name", m->name, sizeof m->name); if (!m->name[0]) snprintf(m->name, sizeof m->name, "%s", m->dir);
        ini_read(ini, "author", m->author, sizeof m->author);
        ini_read(ini, "version", m->version, sizeof m->version);
        ini_read(ini, "description", m->desc, sizeof m->desc);
        char d[MAX_PATH]; snprintf(d, sizeof d, "%s\\%s\\disc", g_root, m->dir);
        const DWORD a = GetFileAttributesA(d); m->files = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        const int def = (int)GetPrivateProfileIntA("mod", "on", 1, ini) != 0;   /* mod.ini on=0: off until switched on */
        m->on = (int)GetPrivateProfileIntA("mods", m->dir, def, psp_ini_path()) != 0;
    }
    g_dump = (int)GetPrivateProfileIntA("mod_options", "dump_textures", 0, psp_ini_path()) != 0;
    EnterCriticalSection(&g_lock); index_textures(); LeaveCriticalSection(&g_lock);
    if (g_nmod) { fprintf(stderr, "mods: %d in %s\n", g_nmod, psp_logpath(g_root)); for (int i = 0; i < g_nmod; i++)
        fprintf(stderr, "mods:   %s %s (%s)%s%s\n", g_mod[i].name, g_mod[i].version, g_mod[i].on ? "on" : "off", g_mod[i].files ? " files" : "", g_mod[i].textures ? " textures" : ""); }
}

int psp_mods_count(void) { psp_mods_init(); return g_nmod; }
const psp_mod_t *psp_mods_get(int i) { psp_mods_init(); return i >= 0 && i < g_nmod ? &g_mod[i] : NULL; }
void psp_mods_set(int i, int on) {
    psp_mods_init();
    if (i < 0 || i >= g_nmod || g_mod[i].on == !!on) return;
    g_mod[i].on = !!on;
    WritePrivateProfileStringA("mods", g_mod[i].dir, on ? "1" : "0", psp_ini_path());
    EnterCriticalSection(&g_lock); index_textures(); LeaveCriticalSection(&g_lock);
    InterlockedIncrement(&g_gen);
}
int  psp_mods_dump(void) { psp_mods_init(); return g_dump; }
void psp_mods_set_dump(int on) {
    psp_mods_init(); g_dump = !!on;
    WritePrivateProfileStringA("mod_options", "dump_textures", on ? "1" : "0", psp_ini_path());
    if (on) InterlockedIncrement(&g_gen);   /* decode everything again, so it gets dumped */
}
void psp_mods_open_folder(void) { psp_mods_init(); ShellExecuteA(NULL, "open", g_root, NULL, NULL, SW_SHOWNORMAL); }
unsigned psp_mods_generation(void) { return (unsigned)g_gen; }

int psp_mods_file(const char *rel, char *out, size_t cap) {
    psp_mods_init();
    while (*rel == '/' || *rel == '\\') rel++;
    for (int i = g_nmod - 1; i >= 0; i--) {
        if (!g_mod[i].on || !g_mod[i].files) continue;
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s\\disc\\%s", g_root, g_mod[i].dir, rel);
        for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
        const DWORD a = GetFileAttributesA(p);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) { snprintf(out, cap, "%s", p); return 1; }
    }
    return 0;
}

/* ---- PNG through WIC ---------------------------------------------------- */
static IWICImagingFactory *wic(void) {
    static IWICImagingFactory *f; static int tried;
    if (!f && !tried) { tried = 1;
        CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* RPC_E_CHANGED_MODE is fine: COM is up either way */
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f))) f = NULL; }
    return f;
}
static uint32_t *png_load(const char *path, int *w, int *h) {
    IWICImagingFactory *f = wic(); if (!f) return NULL;
    wchar_t wp[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH);
    IWICBitmapDecoder *d = NULL; IWICBitmapFrameDecode *fr = NULL; IWICFormatConverter *cv = NULL; uint32_t *px = NULL;
    UINT W = 0, H = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(cv, &W, &H)) && W && H && W <= 8192 && H <= 8192) {
        px = (uint32_t *)malloc((size_t)W * H * 4);
        if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, W * 4, W * H * 4, (BYTE *)px))) { free(px); px = NULL; }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (d) IWICBitmapDecoder_Release(d);
    *w = (int)W; *h = (int)H;
    return px;
}
static int png_save(const char *path, const uint32_t *px, int w, int h) {
    IWICImagingFactory *f = wic(); if (!f) return 0;
    wchar_t wp[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH);
    uint8_t *bgra = (uint8_t *)malloc((size_t)w * h * 4); if (!bgra) return 0;
    for (int i = 0; i < w * h; i++) { const uint32_t c = px[i];
        bgra[4 * i] = (uint8_t)(c >> 16); bgra[4 * i + 1] = (uint8_t)(c >> 8); bgra[4 * i + 2] = (uint8_t)c; bgra[4 * i + 3] = (uint8_t)(c >> 24); }
    IWICStream *s = NULL; IWICBitmapEncoder *e = NULL; IWICBitmapFrameEncode *fr = NULL; int ok = 0;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(f, &s)) &&
        SUCCEEDED(IWICStream_InitializeFromFilename(s, wp, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, NULL, &e)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(e, (IStream *)s, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(e, &fr, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, (UINT)w, (UINT)h)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt)) && IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, (UINT)h, (UINT)w * 4, (UINT)(w * h * 4), bgra)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(e))) ok = 1;
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (e) IWICBitmapEncoder_Release(e);
    if (s) IWICStream_Release(s);
    free(bgra);
    return ok;
}

const uint32_t *psp_mods_texture(uint64_t hash, int *w, int *h) {
    psp_mods_init();
    if (!g_ntex) return NULL;
    const uint32_t *r = NULL;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ntex; i++) {
        tex_ent *t = &g_tex[i];
        if (t->hash != hash) continue;
        if (!t->px && !t->failed) {
            char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s\\textures\\%016llx.png", g_root, g_mod[t->mod].dir, (unsigned long long)hash);
            t->px = png_load(p, &t->w, &t->h);
            if (!t->px) { t->failed = 1; fprintf(stderr, "mods: could not read %s\n", psp_logpath(p));
                { char q[MAX_PATH], e[200]; snprintf(q, sizeof q, "%s", p); char *f = strrchr(q, '\\'), *m = NULL;   /* <mod>\textures\<file>: name both */
                  if (f) { *f++ = 0; char *t2 = strrchr(q, '\\'); if (t2) { *t2 = 0; m = strrchr(q, '\\'); m = m ? m + 1 : q; } }
                  snprintf(e, sizeof e, "%s: could not read %s", m ? m : "?", f ? f : p); psp_error("Mods", e, 15); } }
        }
        if (t->px) { r = t->px; *w = t->w; *h = t->h; }
        break;
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

void psp_mods_dump_texture(uint64_t hash, const uint32_t *px, int w, int h) {
    if (!psp_mods_dump() || w <= 0 || h <= 0) return;
    for (int i = 0; i < g_ndumped; i++) if (g_dumped[i] == hash) return;
    if (g_ndumped == g_capdumped) { g_capdumped = g_capdumped ? g_capdumped * 2 : 1024; g_dumped = (uint64_t *)realloc(g_dumped, sizeof *g_dumped * (size_t)g_capdumped); }
    g_dumped[g_ndumped++] = hash;
    char d[MAX_PATH]; GetCurrentDirectoryA(MAX_PATH - 32, d); strcat(d, "\\dump"); CreateDirectoryA(d, NULL);
    strcat(d, "\\textures"); CreateDirectoryA(d, NULL);
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%016llx.png", d, (unsigned long long)hash);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return;
    png_save(p, px, w, h);
}

#else   /* no mods elsewhere yet */
void psp_mods_init(void) {}
int psp_mods_count(void) { return 0; }
const psp_mod_t *psp_mods_get(int i) { (void)i; return NULL; }
void psp_mods_set(int i, int on) { (void)i; (void)on; }
int psp_mods_dump(void) { return 0; }
void psp_mods_set_dump(int on) { (void)on; }
void psp_mods_open_folder(void) {}
const char *psp_mods_root(void) { return "mods"; }
int psp_mods_file(const char *rel, char *out, size_t cap) { (void)rel; (void)out; (void)cap; return 0; }
unsigned psp_mods_generation(void) { return 1; }
const uint32_t *psp_mods_texture(uint64_t hash, int *w, int *h) { (void)hash; (void)w; (void)h; return NULL; }
void psp_mods_dump_texture(uint64_t hash, const uint32_t *px, int w, int h) { (void)hash; (void)px; (void)w; (void)h; }
#endif
