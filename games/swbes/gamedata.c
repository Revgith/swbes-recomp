/* Finding the player's own copy of the game.
 *
 * Nothing from the game ships with this project. The player puts their disc in
 * a game_data folder (beside swbes.exe, or in the repository root): the .iso,
 * or its extracted contents (a folder holding PSP_GAME). An .iso is unpacked
 * once into game_data/disc, and its EBOOT is decrypted once into eboot.elf by
 * ebootdec.exe (a separate GPL program). A PPSSPP EBOOT dump of the same disc in
 * game_data is used as is.
 * Saves, settings.ini and mods then live in game_data too. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <direct.h>
#include "psprecomp/hle.h"

static int is_dir(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int head(const char *p, uint8_t *b, int n) {
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    const int got = (int)fread(b, 1, (size_t)n, f); fclose(f); return got == n;
}

/* ---- ISO9660, just enough to unpack a UMD image ---------------------------- */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int iso_copy(FILE *iso, uint32_t lba, uint32_t size, const char *dst) {
    FILE *o = fopen(dst, "wb"); if (!o) return -1;
    static uint8_t buf[1 << 20];
    _fseeki64(iso, (int64_t)lba * 2048, SEEK_SET);
    while (size) { const size_t n = size < sizeof buf ? size : sizeof buf;
        if (fread(buf, 1, n, iso) != n || fwrite(buf, 1, n, o) != n) { fclose(o); return -1; } size -= (uint32_t)n; }
    fclose(o); return 0;
}
static int iso_entries;   /* a looping or hostile image cannot make the unpack run forever */
static int iso_dir(FILE *iso, uint32_t lba, uint32_t size, const char *dst, int depth) {
    if (depth > 16 || size > (16u << 20) || ++iso_entries > 100000) return -1;
    _mkdir(dst);
    uint8_t *d = (uint8_t *)malloc(size); if (!d) return -1;
    _fseeki64(iso, (int64_t)lba * 2048, SEEK_SET);
    if (fread(d, 1, size, iso) != size) { free(d); return -1; }
    int rc = 0;
    for (uint32_t off = 0; off < size && !rc; ) {
        const uint8_t *r = d + off;
        if (!r[0]) { off = (off / 2048 + 1) * 2048; continue; }   /* records never straddle a sector */
        if (r[0] < 34 || off + r[0] > size || 33u + r[32] > r[0]) { rc = -1; break; }
        const int nl = r[32]; char name[256]; memcpy(name, r + 33, (size_t)nl); name[nl] = 0;
        char *semi = strchr(name, ';'); if (semi) *semi = 0;
        if (!(nl == 1 && (r[33] == 0 || r[33] == 1))) {
            /* names come from the image: never let one climb out of dst */
            if (!name[0] || strpbrk(name, "\\/:*?\"<>|") || !strcmp(name, ".") || !strcmp(name, "..")) { rc = -1; break; }
            char path[1024]; snprintf(path, sizeof path, "%s/%s", dst, name);
            if (++iso_entries > 100000) { rc = -1; break; }
            rc = (r[25] & 2) ? iso_dir(iso, le32(r + 2), le32(r + 10), path, depth + 1) : iso_copy(iso, le32(r + 2), le32(r + 10), path);
        }
        off += r[0];
    }
    free(d); return rc;
}
static int iso_unpack(const char *iso_path, const char *dst) {
    FILE *f = fopen(iso_path, "rb"); if (!f) return -1;
    uint8_t pvd[2048]; _fseeki64(f, 16 * 2048, SEEK_SET);
    if (fread(pvd, 1, 2048, f) != 2048 || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) { fclose(f); return -1; }
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.partial", dst);
    iso_entries = 0;
    const int rc = iso_dir(f, le32(pvd + 156 + 2), le32(pvd + 156 + 10), tmp, 0);
    fclose(f);
    if (rc || !MoveFileA(tmp, dst)) return -1;   /* a half-unpacked disc never passes for a whole one */
    return 0;
}

/* ---- the search ------------------------------------------------------------ */
/* The recompiled code matches one binary: ULES-01214 v1.00's EBOOT, decrypted
 * (by ebootdec or PPSSPP). Entry point, segment size and section table offset
 * single it out; a PPSSPP dump only adds junk after the section table. */
static int is_game_elf(const char *p) {
    uint8_t b[64]; if (!head(p, b, 64) || memcmp(b, "\x7F" "ELF", 4)) return 0;
    const uint32_t phoff = le32(b + 28);
    uint8_t ph[32]; FILE *f = fopen(p, "rb"); if (!f) return 0;
    const int ok = !_fseeki64(f, phoff, SEEK_SET) && fread(ph, 1, 32, f) == 32; fclose(f);
    return ok && le32(b + 24) == 0x0036F698u && le32(b + 32) == 0x00457818u && le32(ph + 12) == 0x003EC8ACu && le32(ph + 16) == 0x004565B0u;
}

static int find_data_dir(char *out, size_t cap) {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\'); if (s) *s = 0;
    const char *env = getenv("SWBES_GAME_DATA");
    char c[4][MAX_PATH + 32]; int n = 0;
    if (env && *env) snprintf(c[n++], sizeof c[0], "%s", env);
    snprintf(c[n++], sizeof c[0], "%s\\game_data", exe);
    snprintf(c[n++], sizeof c[0], "%s\\..\\game_data", exe);   /* build\swbes.exe in a checkout */
    snprintf(c[n++], sizeof c[0], "game_data");
    for (int i = 0; i < n; i++) if (is_dir(c[i])) { GetFullPathNameA(c[i], (DWORD)cap, out, NULL); return 1; }
    return 0;
}

static void fail(const char *msg) {
    fprintf(stderr, "game data: %s\n", msg);
    MessageBoxA(NULL, msg, "Battlefront: Elite Squadron", MB_OK | MB_ICONINFORMATION);
}

/* Fills elf (the EBOOT dump) and disc (the folder holding PSP_GAME), and makes
 * game_data the working directory. 0 on success; on failure tells the player. */
int swbes_find_game(char *elf, size_t elf_cap, char *disc, size_t disc_cap) {
    char dir[MAX_PATH];
    if (!find_data_dir(dir, sizeof dir)) {
        fail("Game files not found.\n\nMake a folder named game_data next to swbes.exe and put your "
             "Elite Squadron .iso (or its extracted files) in it.");
        return -1;
    }
    elf[0] = disc[0] = 0;
    char iso[MAX_PATH] = "", pat[MAX_PATH + 8];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                char g[MAX_PATH * 2 + 16]; snprintf(g, sizeof g, "%s\\PSP_GAME", p);
                if (!disc[0] && is_dir(g) && !strstr(fd.cFileName, ".partial")) snprintf(disc, disc_cap, "%s", p);
                continue;
            }
            if (!elf[0] && is_game_elf(p)) snprintf(elf, elf_cap, "%s", p);
            const size_t l = strlen(fd.cFileName);
            if (!iso[0] && l > 4 && !_stricmp(fd.cFileName + l - 4, ".iso")) snprintf(iso, sizeof iso, "%s", p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    { char g[MAX_PATH + 16]; snprintf(g, sizeof g, "%s\\PSP_GAME", dir); if (!disc[0] && is_dir(g)) snprintf(disc, disc_cap, "%s", dir); }
    if (!disc[0] && iso[0]) {
        char d[MAX_PATH + 8]; snprintf(d, sizeof d, "%s\\disc", dir);
        fprintf(stderr, "game data: unpacking %s (once)\n", iso);
        if (iso_unpack(iso, d)) { fail("Could not unpack the .iso in game_data (is it a PSP disc image, and is there ~2 GB free?)."); return -1; }
        snprintf(disc, disc_cap, "%s", d);
    }
    if (!disc[0]) { fail("No disc in game_data.\n\nPut your Elite Squadron .iso (or its extracted files) in game_data."); return -1; }
    if (!elf[0]) {   /* decrypt the disc's EBOOT once, with the separate ebootdec helper */
        char src[MAX_PATH * 2], dst[MAX_PATH + 16], tool[MAX_PATH + 16], cmd[MAX_PATH * 4];
        snprintf(src, sizeof src, "%s\\PSP_GAME\\SYSDIR\\EBOOT.BIN", disc);
        snprintf(dst, sizeof dst, "%s\\eboot.elf", dir);
        GetModuleFileNameA(NULL, tool, MAX_PATH); char *s = strrchr(tool, '\\'); if (s) s[1] = 0;
        strcat(tool, "ebootdec.exe");
        snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" \"%s\"", tool, src, dst);
        STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi; DWORD rc = 1; int started = 0;
        fprintf(stderr, "game data: decrypting %s (once)\n", src);
        if ((started = CreateProcessA(tool, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi) != 0)) {
            WaitForSingleObject(pi.hProcess, INFINITE); GetExitCodeProcess(pi.hProcess, &rc);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        if (rc || !is_game_elf(dst)) {
            DeleteFileA(dst);
            fail(!started ? "Could not decrypt the game's EBOOT: ebootdec.exe is missing beside swbes.exe." :
                 rc == 3 ? "Could not write game_data\\eboot.elf. Is the disk full, or is game_data read-only?" :
                 rc == 4 ? "The disc's EBOOT.BIN could not be read." :
                 "This disc is not the version this port is built for.\n\n"
                 "It needs Star Wars Battlefront: Elite Squadron, EU (ULES-01214) v1.00.");
            return -1;
        }
        snprintf(elf, elf_cap, "%s", dst);
    }
    _chdir(dir);   /* saves (ms), settings.ini and mods live beside the game */
    fprintf(stderr, "game data: %s\n  eboot %s\n  disc  %s\n", dir, elf, disc);
    return 0;
}
