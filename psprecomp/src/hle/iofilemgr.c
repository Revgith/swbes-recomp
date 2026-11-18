/* psprecomp — IoFileMgrForUser.
 *
 * The PSP's file API, mapped onto a host directory. Games address the UMD as
 * `disc0:/` and the Memory Stick as `ms0:/`, so those prefixes are rewritten to
 * subdirectories of a root the host chooses.
 *
 * Reads go straight into guest memory, which means a game loading assets is
 * doing the real thing -- and a texture or model that arrives byte-correct is
 * strong evidence the recompiled code around it is behaving too.
 */

#include "psprecomp/hle.h"
#include "psprecomp/mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <io.h>
#else
#  include <dirent.h>
#endif

#define MAX_FILES 64
#define MAX_DIRS  16

/* Open flags, as the guest passes them. */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

/* base/len: a window onto the host file, for raw-sector ("sce_lbn") opens;
 * len < 0 means the whole file. */
typedef struct { FILE *f; int used; int64_t base, len; } io_file;

/* A virtual UMD layout. The disc is a host folder, so there are no real
 * sector numbers; files are handed out consecutive, non-overlapping LBNs the
 * first time they are listed or stat'ed (SceIoStat.private[0]), and
 * "disc0:/sce_lbn0x<L>_size0x<S>" opens are mapped back through this table. */
typedef struct { char *host; uint32_t lbn, sectors; uint64_t size; } lbn_ent;
static lbn_ent *g_lbn; static int g_nlbn, g_caplbn;
static uint32_t g_next_lbn = 0x1000;

static int path_eq(const char *a, const char *b) {   /* case- and slash-insensitive */
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x == '\\') x = '/'; if (y == '\\') y = '/';
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static uint32_t lbn_for(const char *host, uint64_t size) {
    { static int off = -1; if (off < 0) off = getenv("PSPRECOMP_NOLBN") != NULL; if (off) return 0; }
    for (int i = 0; i < g_nlbn; i++)
        if (path_eq(g_lbn[i].host, host)) return g_lbn[i].lbn;
    if (g_nlbn == g_caplbn) {
        g_caplbn = g_caplbn ? g_caplbn * 2 : 256;
        g_lbn = (lbn_ent *)realloc(g_lbn, sizeof *g_lbn * (size_t)g_caplbn);
    }
    lbn_ent *e = &g_lbn[g_nlbn++];
    e->host = _strdup(host); e->size = size; e->lbn = g_next_lbn;
    e->sectors = (uint32_t)((size + 2047) / 2048); if (!e->sectors) e->sectors = 1;
    g_next_lbn += e->sectors + 1;
    return e->lbn;
}
static const lbn_ent *lbn_find(uint32_t lbn) {
    for (int i = 0; i < g_nlbn; i++)
        if (lbn >= g_lbn[i].lbn && lbn < g_lbn[i].lbn + g_lbn[i].sectors) return &g_lbn[i];
    return NULL;
}

typedef struct {
    int used;
    char path[1088];
#ifdef _WIN32
    intptr_t handle;
    struct _finddata_t data;
    int first;
    int done;
#else
    DIR *dir;
#endif
} io_dir;

static io_file g_file[MAX_FILES];
static io_dir  g_dir[MAX_DIRS];
static char    g_root[512];
static uint64_t g_bytes_read;

void psp_io_set_root(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
}

/* Where disc0:/umd0: live, if not <root>/disc: the folder holding PSP_GAME. */
static char g_disc[512];
void psp_io_set_disc(const char *dir) { snprintf(g_disc, sizeof g_disc, "%s", dir ? dir : ""); }
const char *psp_io_disc(void) {
    static char d[600];
    if (g_disc[0]) return g_disc;
    snprintf(d, sizeof d, "%s/disc", g_root[0] ? g_root : "."); return d;
}

void psp_io_reset(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used && g_file[i].f) fclose(g_file[i].f);
        g_file[i].used = 0;
        g_file[i].f = NULL;
    }
    memset(g_dir, 0, sizeof g_dir);
    g_bytes_read = 0;
    if (!g_root[0]) psp_io_set_root(".");
}

void psp_io_init(void) { g_root[0] = '\0'; psp_io_reset(); }

uint64_t psp_io_bytes_read(void) { return g_bytes_read; }

/* Rewrite a PSP path into a host path. The device prefix becomes a
 * subdirectory so a disc image and a memory-stick image can coexist under one
 * root without colliding. */
/* Device names are case-insensitive: Asura opens "DISC0:/PSP_GAME/...". */
static int pfx(const char *s, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char a = s[i], b = p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 1;
    }
    return 0;
}

static void map_path(const char *guest, char *out, size_t cap) {
    const char *p = guest;
    const char *sub = "disc";

    if      (!pfx(p, "disc0:", 6)) { p += 6; sub = "disc"; }
    else if (!pfx(p, "umd0:",  5)) { p += 5; sub = "disc"; }
    else if (!pfx(p, "ms0:",   4)) { p += 4; sub = "ms";   }
    else if (!pfx(p, "flash0:",7)) { p += 7; sub = "flash";}
    else if (!pfx(p, "host0:", 6)) { p += 6; sub = "host"; }

    while (*p == '/' || *p == '\\') p++;
    /* An enabled mod's copy of a disc file wins (mods.h). */
    if (!strcmp(sub, "disc") && *p && psp_mods_file(p, out, cap)) {
        if (getenv("PSPRECOMP_TRACE_IO")) fprintf(stderr, "io: mod file %s\n", out);
        return;
    }
    if (!strcmp(sub, "disc") && g_disc[0]) snprintf(out, cap, "%s/%s", g_disc, p);
    else snprintf(out, cap, "%s/%s/%s", g_root, sub, p);
}

static void hle_Open(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    uint32_t flags = psp_arg(1);
    map_path(guest, host, sizeof host);

    const char *mode = "rb";
    if (flags & PSP_O_TRUNC)                  mode = (flags & PSP_O_RDWR) == PSP_O_RDWR ? "w+b" : "wb";
    else if (flags & PSP_O_APPEND)            mode = "ab";
    else if ((flags & PSP_O_RDWR) == PSP_O_RDWR) mode = "r+b";
    else if (flags & PSP_O_WRONLY)            mode = (flags & PSP_O_CREAT) ? "wb" : "r+b";

    int64_t base = 0, len = -1;
    FILE *f = NULL;
    const char *lb = strstr(guest, "sce_lbn");
    if (lb) {
        uint32_t L = (uint32_t)strtoul(lb + 7, NULL, 16);
        const char *sz = strstr(lb, "_size");
        len = sz ? (int64_t)strtoull(sz + 5, NULL, 16) : -1;
        const lbn_ent *e = lbn_find(L);
        if (e) { f = fopen(e->host, "rb"); base = (int64_t)(L - e->lbn) * 2048; snprintf(host, sizeof host, "%s", e->host); }
    } else {
        f = fopen(host, mode);
        if (!f && (flags & PSP_O_CREAT)) f = fopen(host, "w+b");
    }
    if (f && base) _fseeki64(f, base, SEEK_SET);
    /* PSPRECOMP_TRACE_IO=1 names every open and whether it worked. A game
     * that cannot find its assets usually does not crash; it waits. */
    if (getenv("PSPRECOMP_TRACE_IO"))
        fprintf(stderr, "io: open \"%s\" -> \"%s\" flags 0x%X: %s\n",
                guest, host, flags, f ? "ok" : "not found");
    if (!f) {
        /* A failed open is normal (a game probing for a save file) and is not
         * worth a warning, but the mapped path is worth knowing when a game
         * cannot find assets it expects. */
        psp_ret(0x80010002);          /* ENOENT */
        return;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used) continue;
        g_file[i].f = f;
        g_file[i].used = 1;
        g_file[i].base = base;
        g_file[i].len = len;
        psp_ret((uint32_t)(i + 3));   /* 0-2 are reserved for the std streams */
        return;
    }
    fclose(f);
    psp_ret(0x80010018);              /* too many open files */
}

static io_file *fd_arg(void) {
    int32_t fd = (int32_t)psp_arg(0) - 3;
    if (fd < 0 || fd >= MAX_FILES || !g_file[fd].used) return NULL;
    return &g_file[fd];
}

static void hle_Close(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(0x80020323); return; }
    fclose(h->f);
    h->f = NULL;
    h->used = 0;
    psp_ret(0);
}

static void hle_Read(void) {
    io_file *h = fd_arg();
    uint32_t dst = psp_arg(1), size = psp_arg(2);
    if (!h) { psp_ret(0x80020323); return; }
    if (!size) { psp_ret(0); return; }

    /* Read through a host buffer and then place it, so a read that straddles
     * the end of a guest region is rejected by the memory layer rather than
     * writing past it. */
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) { psp_ret(0x80020190); return; }

    int64_t lbn_pos = -1;
    static int noclamp = -1; if (noclamp < 0) noclamp = getenv("PSPRECOMP_LBN_NOCLAMP") != NULL;
    if (h->len >= 0 && !noclamp) {
        /* A raw-sector window reads whole sectors, as the disc does: the
         * last one past the file's end comes back zero-filled, not short. */
        int64_t lim = (h->len + 2047) & ~(int64_t)2047;
        int64_t at = _ftelli64(h->f) - h->base, left = lim - at;
        if (left <= 0) { free(tmp); psp_ret(0); return; }
        if ((int64_t)size > left) size = (uint32_t)left;
        lbn_pos = at;
    }
    size_t got = fread(tmp, 1, size, h->f);
    if (lbn_pos >= 0 && got < size) {
        memset(tmp + got, 0, size - got);
        _fseeki64(h->f, h->base + lbn_pos + size, SEEK_SET);
        got = size;
    }
    if (got && psp_mem_write_block(dst, tmp, (uint32_t)got) != 0) {
        /* Fall back to byte-at-a-time so a partially mapped destination still
         * gets what fits, and the bad-access counter records the rest. */
        for (size_t i = 0; i < got; i++) psp_write8(dst + (uint32_t)i, tmp[i]);
    }
    free(tmp);
    g_bytes_read += got;
    psp_ret((uint32_t)got);
}

static void hle_Write(void) {
    io_file *h = fd_arg();
    uint32_t src = psp_arg(1), size = psp_arg(2);

    /* fd 1 and 2 are stdout/stderr: a game writing there is talking to us. */
    int32_t fd = (int32_t)psp_arg(0);
    if (fd == 1 || fd == 2) {
        for (uint32_t i = 0; i < size; i++) fputc(psp_read8(src + i), stderr);
        psp_ret(size);
        return;
    }
    if (!h) { psp_ret(0x80020323); return; }

    uint8_t *tmp = (uint8_t *)malloc(size ? size : 1);
    if (!tmp) { psp_ret(0x80020190); return; }
    for (uint32_t i = 0; i < size; i++) tmp[i] = psp_read8(src + i);
    size_t put = fwrite(tmp, 1, size, h->f);
    free(tmp);
    psp_ret((uint32_t)put);
}

/* sceIoLseek takes a 64-bit offset and returns one. Under o32 a 64-bit
 * argument is register-aligned, so it lands in $a2:$a3 rather than $a1:$a2 --
 * and the result comes back in $v0:$v1. Getting either wrong makes every seek
 * land somewhere plausible but wrong. */
static void hle_Lseek(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    uint32_t whence = psp_arg(4);
    if (!h) { psp_ret(0x80020323); return; }

    int64_t o = (int64_t)off, r;
    if (whence == 1)      r = _fseeki64(h->f, o, SEEK_CUR);
    else if (whence == 2) r = h->len >= 0 ? _fseeki64(h->f, h->base + h->len + o, SEEK_SET) : _fseeki64(h->f, o, SEEK_END);
    else                  r = _fseeki64(h->f, h->base + o, SEEK_SET);
    if (r != 0) { psp_ret(0xFFFFFFFFu); return; }

    int64_t pos = _ftelli64(h->f) - h->base;
    psp_cpu.r[PSP_REG_V0] = (uint32_t)pos;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)((uint64_t)pos >> 32);
}

static void hle_Rename(void) {
    char a[512], b[512], ha[1024], hb[1024];
    psp_str(psp_arg(0), a, sizeof a);
    psp_str(psp_arg(1), b, sizeof b);
    map_path(a, ha, sizeof ha);
    map_path(b, hb, sizeof hb);
    psp_ret(rename(ha, hb) == 0 ? 0 : 0x80010002);
}

/* ---- directories --------------------------------------------------------- */

static void hle_Dopen(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    if (getenv("PSPRECOMP_TRACE_IO")) fprintf(stderr, "io: dopen \"%s\"\n", guest);

    for (int i = 0; i < MAX_DIRS; i++) {
        if (g_dir[i].used) continue;
#ifdef _WIN32
        char pattern[1088];
        snprintf(pattern, sizeof pattern, "%s/*", host);
        g_dir[i].handle = _findfirst(pattern, &g_dir[i].data);
        if (g_dir[i].handle == -1) { psp_ret(0x80010002); return; }
        g_dir[i].first = 1;
        g_dir[i].done = 0;
#else
        g_dir[i].dir = opendir(host);
        if (!g_dir[i].dir) { psp_ret(0x80010002); return; }
#endif
        g_dir[i].used = 1;
        snprintf(g_dir[i].path, sizeof g_dir[i].path, "%s", host);
        { size_t L = strlen(g_dir[i].path); while (L && (g_dir[i].path[L-1] == '/' || g_dir[i].path[L-1] == '\\')) g_dir[i].path[--L] = 0; }
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(0x80010018);
}

/* Fill a SceIoDirent. Only the name is populated: games use Dread to enumerate
 * save slots and asset directories by name, and a wrong stat block would be
 * worse than an empty one. */
static void hle_Dread(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    uint32_t dirent = psp_arg(1);
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }

    const char *name = NULL;
#ifdef _WIN32
    if (g_dir[id].done || getenv("PSPRECOMP_NODREAD")) { psp_ret(0); return; }
    for (;;) {      /* skip "." and "..": games recurse into every directory */
        if (g_dir[id].first) {
            g_dir[id].first = 0;
            name = g_dir[id].data.name;
        } else if (_findnext(g_dir[id].handle, &g_dir[id].data) == 0) {
            name = g_dir[id].data.name;
        } else {
            g_dir[id].done = 1;
            psp_ret(0);
            return;
        }
        { static int hide = -1; if (hide < 0) hide = getenv("PSPRECOMP_HIDE_PMF") != NULL;
          size_t nl = strlen(name);
          if (hide && nl > 4 && !_stricmp(name + nl - 4, ".PMF")) continue; }
        if (strcmp(name, ".") && strcmp(name, "..")) break;
    }
#else
    struct dirent *de;
    do { de = readdir(g_dir[id].dir); } while (de && (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")));
    if (!de) { psp_ret(0); return; }
    name = de->d_name;
#endif

    /* SceIoDirent: an 0x58-byte SceIoStat (mode, attr, size, 3 times,
     * private[6]), then char d_name[256]. Mode/attr say file or directory. */
    for (int i = 0; i < 0x58; i++) psp_write8(dirent + (uint32_t)i, 0);
    {
        int isdir = 0; uint64_t size = 0;
#ifdef _WIN32
        isdir = (g_dir[id].data.attrib & _A_SUBDIR) != 0;
        size = (uint64_t)g_dir[id].data.size;
#endif
        psp_write32(dirent + 0, isdir ? 0x11FFu : 0x21FFu);
        psp_write32(dirent + 4, isdir ? 0x10u : 0x20u);
        psp_write32(dirent + 8, (uint32_t)size);
        psp_write32(dirent + 12, (uint32_t)(size >> 32));
        if (!isdir && g_dir[id].path[0]) {
            char full[1400];
            snprintf(full, sizeof full, "%s/%s", g_dir[id].path, name);
            psp_write32(dirent + 0x40, lbn_for(full, size));
        }
    }
    uint32_t at = dirent + 0x58;
    for (uint32_t i = 0; i < 255 && name[i]; i++) psp_write8(at + i, (uint8_t)name[i]);
    psp_write8(at + (uint32_t)strlen(name), 0);

    psp_ret(1);                       /* more entries may follow */
}

static void hle_Dclose(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
#ifdef _WIN32
    if (g_dir[id].handle != -1) _findclose(g_dir[id].handle);
#else
    if (g_dir[id].dir) closedir(g_dir[id].dir);
#endif
    g_dir[id].used = 0;
    psp_ret(0);
}

/* (path, SceIoStat *) -- mode, attr, size (s64), ctime/atime/mtime
 * (ScePspDateTime, 16 bytes each), private[6]. Asura stats every archive it
 * opens and treats a zero size as a missing file. */
static void hle_Getstat(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);
    uint32_t out = psp_arg(1);
    struct _stat64 st;
    if (_stat64(host, &st) != 0) { psp_ret(0x80010002u); return; }   /* ENOENT */
    if (!out) { psp_ret(0x80010016u); return; }
    for (uint32_t o = 0; o < 0x58u; o += 4) psp_write32(out + o, 0);
    int dir = (st.st_mode & _S_IFDIR) != 0;
    psp_write32(out + 0, dir ? 0x11FFu : 0x21FFu);   /* FIO_S_IFDIR/IFREG | 0777 */
    psp_write32(out + 4, dir ? 0x10u : 0x20u);
    psp_write32(out + 8,  (uint32_t)st.st_size);
    psp_write32(out + 12, (uint32_t)((uint64_t)st.st_size >> 32));
    if (!dir) psp_write32(out + 0x40, lbn_for(host, (uint64_t)st.st_size));
    psp_ret(0);
}

void psp_io_register(void) {
    psp_hle_register(0x109F50BC, "IoFileMgrForUser", "sceIoOpen",   hle_Open);
    psp_hle_register(0xACE946E8, "IoFileMgrForUser", "sceIoGetstat", hle_Getstat);
    psp_hle_register(0x810C4BC3, "IoFileMgrForUser", "sceIoClose",  hle_Close);
    psp_hle_register(0x6A638D83, "IoFileMgrForUser", "sceIoRead",   hle_Read);
    psp_hle_register(0x42EC03AC, "IoFileMgrForUser", "sceIoWrite",  hle_Write);
    psp_hle_register(0x27EB27B8, "IoFileMgrForUser", "sceIoLseek",  hle_Lseek);
    psp_hle_register(0x779103A0, "IoFileMgrForUser", "sceIoRename", hle_Rename);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",  hle_Dopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",  hle_Dread);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose", hle_Dclose);
}
