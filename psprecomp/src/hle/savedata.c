/*
 * sceUtilitySavedata -- the save/load dialog, done synchronously against a host
 * folder (work dir "ms/PSP/SAVEDATA/<gameName><saveName>/<fileName>").
 *
 * The real utility runs a dialog over several frames; games only watch the
 * status word (GetStatus) and the result field in the param block, so the whole
 * operation happens inside InitStart and the status reports FINISHED.
 *
 * SceUtilitySavedataParam offsets (as documented by PPSSPP):
 *   0x00 base (size, ..., result @0x1C), 0x30 mode, 0x3C gameName[13],
 *   0x4C saveName[20], 0x60 saveNameList*, 0x64 fileName[13],
 *   0x74 dataBuf*, 0x78 dataBufSize, 0x7C dataSize,
 *   0x80 sfoParam (title 0x80, savedataTitle 0x80, detail 0x400, parental u8),
 *   0x5D0 msFree*, 0x5D4 msData*, 0x5D8 utilityData*, 0x5F4 idList*,
 *   0x5F8 fileList*, 0x5FC sizeInfo*.
 */
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/recomp_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define mkdir_(p) _mkdir(p)
#else
#  include <dirent.h>
#  define mkdir_(p) mkdir(p, 0777)
#endif

enum { ST_NONE = 0, ST_INIT = 1, ST_RUNNING = 2, ST_FINISHED = 3, ST_SHUTDOWN = 4 };
static int g_status = ST_NONE;
static int g_trace = -1;

#define ERR_LOAD_NO_DATA      0x80110307u
#define ERR_LOAD_NO_MS        0x80110301u
#define ERR_SAVE_NO_MS        0x80110381u
#define ERR_RW_NO_DATA        0x80110327u   /* sizes/list: data not found */

static void rd_str(uint32_t a, char *dst, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) { char c = (char)psp_read8(a + (uint32_t)i); if (!c) break; dst[i] = c; }
    dst[i] = 0;
}

static void base_dir(char *out, size_t n) {
    snprintf(out, n, "ms/PSP/SAVEDATA");
}

static void save_dir(uint32_t p, const char *save, char *out, size_t n) {
    char game[16], b[256];
    rd_str(p + 0x3C, game, sizeof game);
    base_dir(b, sizeof b);
    snprintf(out, n, "%s/%s%s", b, game, save);
}

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *c = tmp + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir_(tmp); *c = '/'; }
    mkdir_(tmp);
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* First existing save among saveNameList (or the fixed saveName). */
static int pick_save(uint32_t p, int mode, char *save, size_t n, int want_existing) {
    rd_str(p + 0x4C, save, n);
    uint32_t list = psp_read32(p + 0x60);
    if ((mode == 4 || mode == 5 || mode == 6 || save[0] == 0 || save[0] == '<') && list) {
        char first[32] = "";
        for (int i = 0; i < 64; i++) {
            char nm[32], d[512];
            rd_str(list + (uint32_t)i * 20, nm, sizeof nm);
            if (!nm[0]) break;
            if (!first[0]) snprintf(first, sizeof first, "%s", nm);
            save_dir(p, nm, d, sizeof d);
            if (exists(d)) { snprintf(save, n, "%s", nm); return 1; }
        }
        if (!want_existing && first[0]) { snprintf(save, n, "%s", first); return 1; }
        return 0;
    }
    if (!want_existing) return 1;
    char d[512]; save_dir(p, save, d, sizeof d);
    return exists(d);
}

static uint32_t do_load(uint32_t p, int mode) {
    char save[32], dir[512], file[16], path[600];
    if (!pick_save(p, mode, save, sizeof save, 1)) return ERR_LOAD_NO_DATA;
    save_dir(p, save, dir, sizeof dir);
    rd_str(p + 0x64, file, sizeof file);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) return ERR_LOAD_NO_DATA;
    uint32_t buf = psp_read32(p + 0x74), cap = psp_read32(p + 0x78), got = 0;
    uint8_t tmp[4096]; size_t r;
    while (got < cap && (r = fread(tmp, 1, sizeof tmp < cap - got ? sizeof tmp : cap - got, f)) > 0) {
        for (size_t i = 0; i < r; i++) psp_write8(buf + got + (uint32_t)i, tmp[i]);
        got += (uint32_t)r;
    }
    fclose(f);
    psp_write32(p + 0x7C, got);
    for (int i = 0; i < 20; i++) psp_write8(p + 0x4C + (uint32_t)i, (uint8_t)(i < (int)strlen(save) + 1 ? save[i] : 0));
    return 0;
}

static uint32_t do_save(uint32_t p, int mode) {
    char save[32], dir[512], file[16], path[600];
    pick_save(p, mode, save, sizeof save, 0);
    save_dir(p, save, dir, sizeof dir);
    mkdirs(dir);
    rd_str(p + 0x64, file, sizeof file);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "wb");
    if (!f) return ERR_SAVE_NO_MS;
    uint32_t buf = psp_read32(p + 0x74), sz = psp_read32(p + 0x7C);
    for (uint32_t i = 0; i < sz; i++) fputc(psp_read8(buf + i), f);
    fclose(f);
    return 0;
}

/* MODE_SIZES (8): report free space and, if present, the existing data size. */
static uint32_t do_sizes(uint32_t p) {
    uint32_t msFree = psp_read32(p + 0x5D0), msData = psp_read32(p + 0x5D4), util = psp_read32(p + 0x5D8);
    if (msFree) {   /* clusterSize, freeClusters, freeSpaceKB, freeSpaceStr[8] */
        psp_write32(msFree + 0, 0x8000);
        psp_write32(msFree + 4, 0x8000);
        psp_write32(msFree + 8, 0x8000 * 32);
        const char *s = "1 GB";
        for (int i = 0; i < 8; i++) psp_write8(msFree + 12 + (uint32_t)i, (uint8_t)(i < 4 ? s[i] : 0));
    }
    if (util) {     /* usedClusters, usedSpaceKB, usedSpaceStr[8], usedSpace32KB, usedSpace32Str[8] */
        psp_write32(util + 0, 1); psp_write32(util + 4, 32);
        psp_write32(util + 16, 32);
    }
    if (msData) {   /* gameName[13] pad saveName[20] -> info {usedClusters, usedSpaceKB, str[8], 32KB, str[8]} */
        char game[16], save[24], d[512];
        rd_str(msData, game, sizeof game);
        rd_str(msData + 16, save, sizeof save);
        snprintf(d, sizeof d, "ms/PSP/SAVEDATA/%s%s", game, save);
        if (!exists(d)) return ERR_RW_NO_DATA;
        psp_write32(msData + 36, 1); psp_write32(msData + 40, 32);
    }
    return 0;
}

static void hle_InitStart(void) {
    uint32_t p = psp_arg(0);
    if (g_trace < 0) g_trace = getenv("PSPRECOMP_TRACE_SAVE") != NULL;
    int mode = (int)psp_read32(p + 0x30);
    uint32_t r = 0;
    switch (mode) {
    case 0: case 2: case 4: r = do_load(p, mode); break;      /* AUTOLOAD, LOAD, LISTLOAD */
    case 1: case 3: case 5: r = do_save(p, mode); break;      /* AUTOSAVE, SAVE, LISTSAVE */
    case 8: r = do_sizes(p); break;                           /* SIZES */
    case 11: case 12: {                                       /* LIST / FILES: say nothing exists */
        char save[32]; r = pick_save(p, 4, save, sizeof save, 1) ? 0 : ERR_RW_NO_DATA; break; }
    default: r = 0; break;
    }
    if (g_trace) {
        char game[16], save[24], file[16];
        rd_str(p + 0x3C, game, sizeof game); rd_str(p + 0x4C, save, sizeof save); rd_str(p + 0x64, file, sizeof file);
        fprintf(stderr, "savedata: mode %d game '%s' save '%s' file '%s' -> 0x%08X\n", mode, game, save, file, r);
    }
    psp_write32(p + 0x1C, r);
    g_status = ST_FINISHED;
    psp_ret(0);
}

static void hle_GetStatus(void) {
    int s = g_status;
    if (g_status == ST_SHUTDOWN) g_status = ST_NONE;
    psp_ret((uint32_t)s);
}
static void hle_Shutdown(void) { g_status = ST_SHUTDOWN; psp_ret(0); }
static void hle_Update(void)   { psp_ret(0); }

/* sceUtilityOsk: the on-screen keyboard, answered at once with "Player".
 * Params: base 0x30, datacount @0x30, data* @0x34. Each SceUtilityOskData:
 * outtextlength @0x24, outtext* (UTF-16) @0x28, result @0x2C, outtextlimit @0x30. */
static int g_osk = ST_NONE;
static void hle_OskInitStart(void) {
    uint32_t p = psp_arg(0);
    uint32_t n = psp_read32(p + 0x30), d = psp_read32(p + 0x34);
    static const char name[] = "Player";
    for (uint32_t k = 0; k < n && k < 8 && d; k++, d += 0x34) {
        uint32_t out = psp_read32(d + 0x28), lim = psp_read32(d + 0x30);
        uint32_t len = 0;
        for (; name[len] && (lim == 0 || len + 1 < lim); len++) psp_write16(out + len * 2, (uint16_t)name[len]);
        if (out) psp_write16(out + len * 2, 0);
        psp_write32(d + 0x24, len);
        psp_write32(d + 0x2C, 2);           /* OSK_RESULT_CHANGED */
    }
    psp_write32(p + 0x1C, 0);
    g_osk = ST_FINISHED;
    psp_ret(0);
}
static void hle_OskGetStatus(void) { int s = g_osk; if (g_osk == ST_SHUTDOWN) g_osk = ST_NONE; psp_ret((uint32_t)s); }
static void hle_OskShutdown(void)  { g_osk = ST_SHUTDOWN; psp_ret(0); }


/* Video (sceMpeg/scePsmf) is not emulated: report failure so FMVs are skipped. */
static void hle_fail_mpeg(void) { psp_ret(0x80618005u); }
static void hle_fail_psmf(void) { psp_ret(0x80615025u); }
static void hle_loadmodule(void) { psp_ret(0x4001u); }

void psp_savedata_register(void) {
    psp_hle_register(0x977DE386, "ModuleMgrForUser", "sceKernelLoadModule", hle_loadmodule);
    psp_hle_register(0x682A619B, "sceMpeg", "sceMpegInit", hle_fail_mpeg);
    psp_hle_register(0xC132E22F, "sceMpeg", "sceMpegQueryMemSize", hle_fail_mpeg);
    psp_hle_register(0xD7A29F46, "sceMpeg", "sceMpegRingbufferQueryMemSize", hle_fail_mpeg);
    psp_hle_register(0x37295ED8, "sceMpeg", "sceMpegRingbufferConstruct", hle_fail_mpeg);
    psp_hle_register(0xC22C8327, "scePsmf", "scePsmfGetStreamSize", hle_fail_psmf);
    psp_hle_register(0x68D42328, "scePsmf", "scePsmfGetNumberOfSpecificStreams", hle_fail_psmf);
    psp_hle_register(0xF6269B82, "sceUtility", "sceUtilityOskInitStart",      hle_OskInitStart);
    psp_hle_register(0xF3F76017, "sceUtility", "sceUtilityOskGetStatus",      hle_OskGetStatus);
    psp_hle_register(0x3DFAEBA9, "sceUtility", "sceUtilityOskShutdownStart",  hle_OskShutdown);
    psp_hle_register(0x4B85C861, "sceUtility", "sceUtilityOskUpdate",         hle_Update);
    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart",     hle_InitStart);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus",     hle_GetStatus);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart", hle_Shutdown);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate",        hle_Update);
}
