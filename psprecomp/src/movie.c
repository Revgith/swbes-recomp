/* Native movie playback for PSMF (.pmf) files.
 *
 * The firmware's sceMpeg / scePsmf are not emulated (savedata.c fails them, so
 * a game's own player gives up). Instead a game layer calls psp_movie_play()
 * with the movie's host path and this plays it on the host, blocking:
 *
 *  - video: ffmpeg (on PATH, or PSPRECOMP_FFMPEG) decodes the H.264 stream to
 *    raw BGRA frames over a pipe; the window shows them in place of the PSP
 *    display (psp_window_movie).
 *  - audio: ffmpeg's mpeg demuxer does not see PSMF's ATRAC3+ track, so it is
 *    demuxed here (MPEG-PS private stream 1) and decoded with the vendored
 *    ATRAC3+ decoder, then queued on a channel of its own (audio_host).
 *
 * Video is paced by the audio clock when there is audio, else by wall time.
 * Enter, Esc or Space (or a pad's A, B or Start) skips. No ffmpeg = returns 0 and the game skips as before. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "psprecomp/audio_host.h"
#include "psprecomp/window.h"
#include "at3_c.h"

void psp_window_movie(const uint32_t *bgra);   /* window.c: NULL = back to the PSP display */
int psp_window_closed(void);
int psp_key_down(int vk);
void psp_key_fake(int vk, int down);
#define MOVIE_CH 8
#define VW 480
#define VH 272

static uint8_t *read_all(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 ? (uint8_t *)malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f); *n = b ? (size_t)sz : 0; return b;
}

/* One elementary stream out of the MPEG-PS. sid 0xE0: the H.264 video (fed to
 * ffmpeg raw: its PS demuxer finds no video packets in some PMFs, CREDITS.PMF).
 * sid 0xBD: the ATRAC3+ audio, private stream 1 channel 0, each payload after
 * its 4-byte PSMF sub-header (channel id + 3). */
static uint8_t *demux(const uint8_t *b, size_t n, uint8_t sid, size_t *out_n) {
    size_t pos = n >= 16 && !memcmp(b, "PSMF", 4) ? ((size_t)b[8] << 24 | (size_t)b[9] << 16 | (size_t)b[10] << 8 | b[11]) : 0;
    uint8_t *o = (uint8_t *)malloc(n); size_t on = 0;
    while (o && pos + 6 <= n) {
        if (b[pos] || b[pos + 1] || b[pos + 2] != 1) { pos++; continue; }
        const uint8_t id = b[pos + 3];
        if (id == 0xB9) break;
        if (id == 0xBA) { pos += pos + 13 < n ? 14 + (b[pos + 13] & 7) : 14; continue; }
        if (id < 0xBB) { pos += 4; continue; }
        const size_t len = (size_t)b[pos + 4] << 8 | b[pos + 5], end = pos + 6 + len;
        if (end > n) break;
        if (id == sid && pos + 9 < end) {
            const size_t p = pos + 9 + b[pos + 8];
            if (sid != 0xBD) { if (p < end) { memcpy(o + on, b + p, end - p); on += end - p; } }
            else if (p + 4 <= end && b[p] == 0) { memcpy(o + on, b + p + 4, end - p - 4); on += end - p - 4; }
        }
        pos = end;
    }
    *out_n = on; return o;
}

/* Frames are 0x0FD0 + 2 bytes (size, channels) + 4 zero bytes, then the data. */
static int16_t *decode_audio(const uint8_t *es, size_t n, size_t *frames) {
    size_t cap = 1 << 20, fr = 0; int16_t *pcm = (int16_t *)malloc(cap * 4);
    void *dec = NULL; int dch = 0;
    for (size_t p = 0; pcm && p + 8 <= n; ) {
        if (es[p] != 0x0F || es[p + 1] != 0xD0) { p++; continue; }
        const int size = (((es[p + 2] & 3) << 8) | es[p + 3]) * 8 + 8, ch = (es[p + 2] >> 2) & 7;
        if (p + 8 + (size_t)size > n || size > 2048) break;
        if (!dec || ch != dch) { if (dec) psp_at3p_free(dec); int ba = size; dch = ch; dec = psp_at3p_alloc(ch ? ch : 2, &ba); if (!dec) break; }
        uint8_t frame[2048 + 64]; memcpy(frame, es + p + 8, (size_t)size); memset(frame + size, 0, 64);
        float l[2048], r[2048]; float *pl[2] = { l, r }; int nb = 0;
        if (psp_at3p_decode(dec, pl, &nb, frame, size) < 0 || nb <= 0) nb = 0;
        if (dch == 1) memcpy(r, l, sizeof(float) * (size_t)nb);
        if (fr + (size_t)nb > cap) { cap *= 2; int16_t *t = (int16_t *)realloc(pcm, cap * 4); if (!t) break; pcm = t; }
        for (int i = 0; i < nb; i++) {
            const float a = l[i] * 32767.f, c = r[i] * 32767.f;
            pcm[(fr + i) * 2] = (int16_t)(a > 32767.f ? 32767 : a < -32768.f ? -32768 : lrintf(a));
            pcm[(fr + i) * 2 + 1] = (int16_t)(c > 32767.f ? 32767 : c < -32768.f ? -32768 : lrintf(c));
        }
        fr += (size_t)nb; p += 8 + (size_t)size;
    }
    if (dec) psp_at3p_free(dec);
    *frames = fr; return pcm;
}

static int find_ffmpeg(char *out, size_t cap) {
    const char *e = getenv("PSPRECOMP_FFMPEG");
    if (e && GetFileAttributesA(e) != INVALID_FILE_ATTRIBUTES) { snprintf(out, cap, "%s", e); return 1; }
    {   /* shipped beside the game exe (the build copies it there) */
        char d[MAX_PATH]; const DWORD k = GetModuleFileNameA(NULL, d, sizeof d);
        char *s = k ? strrchr(d, '\\') : NULL;
        if (s) { s[1] = 0; snprintf(out, cap, "%sffmpeg.exe", d); if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1; }
    }
    return SearchPathA(NULL, "ffmpeg.exe", NULL, (DWORD)cap, out, NULL) > 0;
}

static int skip_pressed(void) {
    static const int keys[] = { VK_RETURN, VK_ESCAPE, VK_SPACE };
    static int was[3]; int hit = 0;
    /* debug driving (key.cmd, as the game layer reads it, whose thread is blocked here) */
    { FILE *kf = fopen("key.cmd", "rb"); if (kf) { unsigned vk; int dn; while (fscanf(kf, "%x %d", &vk, &dn) == 2) psp_key_fake((int)vk, dn); fclose(kf); remove("key.cmd"); } }
    { FILE *pf = fopen("pad.cmd", "rb"); if (pf) { psp_pad_t p; memset(&p, 0, sizeof p); unsigned b; if (fscanf(pf, "%x", &b) == 1) { p.btn = b; p.on = 1; } psp_pad_fake(&p); fclose(pf); remove("pad.cmd"); } }
    for (int i = 0; i < 3; i++) { const int d = psp_key_down(keys[i]) != 0; if (d && !was[i]) hit = 1; was[i] = d; }
    {   /* a controller: A, B or Start (the game's input read, which polls it, is blocked here) */
        static unsigned pwas; const unsigned pb = psp_pad_poll() & (PSP_PAD_A | PSP_PAD_B | PSP_PAD_START);
        if (pb & ~pwas) hit = 1; pwas = pb; }
    return hit;
}

int psp_movie_play(const char *path) {
    char ff[MAX_PATH], cmd[2 * MAX_PATH + 160];
    if (!find_ffmpeg(ff, sizeof ff)) { fprintf(stderr, "movie: no ffmpeg (PATH or PSPRECOMP_FFMPEG), skipping %s\n", path); return 0; }
    size_t n = 0; uint8_t *file = read_all(path, &n);
    if (!file) { fprintf(stderr, "movie: cannot read %s\n", path); return 0; }
    size_t esn = 0, nfr = 0, vn = 0; uint8_t *es = demux(file, n, 0xBD, &esn);
    int16_t *pcm = es ? decode_audio(es, esn, &nfr) : NULL;
    free(es);
    char tmp[MAX_PATH]; { char td[MAX_PATH]; GetTempPathA(sizeof td, td); snprintf(tmp, sizeof tmp, "%spsprecomp_movie_%lu.264", td, GetCurrentProcessId()); }
    { uint8_t *v = demux(file, n, 0xE0, &vn); FILE *w = v && vn ? fopen(tmp, "wb") : NULL; if (w) { fwrite(v, 1, vn, w); fclose(w); } else vn = 0; free(v); }
    free(file);
    if (!vn) { fprintf(stderr, "movie: no video in %s\n", path); free(pcm); return 0; }
    fprintf(stderr, "movie: %s, audio %.1f s\n", path, nfr / 44100.0);
    if (getenv("PSPRECOMP_MUTE")) { free(pcm); pcm = NULL; nfr = 0; }   /* no audio device: pace by wall time */
    { const char *d = getenv("PSPRECOMP_MOVIE_DUMP"); if (d && pcm) { FILE *w = fopen(d, "wb"); if (w) { fwrite(pcm, 4, nfr, w); fclose(w); } } }   /* debug: decoded audio as raw s16 stereo 44.1k */

    snprintf(cmd, sizeof cmd, "\"%s\" -nostdin -loglevel quiet -f h264 -i \"%s\" -an -f rawvideo -pix_fmt bgra -s %dx%d -", ff, tmp, VW, VH);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE }; HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) { free(pcm); DeleteFileA(tmp); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); free(pcm); DeleteFileA(tmp); return 0; }
    CloseHandle(wr);

    const double fps = 30000.0 / 1001.0;
    uint32_t *frame = (uint32_t *)malloc(VW * VH * 4);
    LARGE_INTEGER qf, t0, t; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&t0);
    size_t apos = 0; uint64_t pushed_total = 0; long shown = 0; int skipped = 0;
    psp_audio_host_reset(MOVIE_CH); skip_pressed();
    for (;;) {
        DWORD got = 0, total = 0; int eof = 0;
        while (total < VW * VH * 4) { if (!ReadFile(rd, (uint8_t *)frame + total, VW * VH * 4 - total, &got, NULL) || !got) { eof = 1; break; } total += got; }
        if (eof) break;
        for (int i = 0; i < VW * VH; i++) frame[i] |= 0xFF000000u;
        const double due = shown / fps;
        for (;;) {   /* keep ~150 ms of audio queued; wait for this frame's time */
            while (pcm && apos < nfr && psp_audio_host_queued(MOVIE_CH) < 6615) {
                const size_t k = nfr - apos < 1024 ? nfr - apos : 1024;
                psp_audio_host_push(MOVIE_CH, pcm + apos * 2, (uint32_t)k, 0, 0x8000, 0x8000); apos += k; pushed_total += k;
            }
            if (skip_pressed() || psp_window_closed()) { skipped = 1; break; }
            QueryPerformanceCounter(&t);
            double now = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart;
            if (pcm && nfr && (apos < nfr || psp_audio_host_queued(MOVIE_CH))) now = (double)(pushed_total - psp_audio_host_queued(MOVIE_CH)) / 44100.0;   /* audio clock while audio plays */
            if (now >= due) break;
            Sleep(1);
        }
        if (skipped) break;
        psp_window_movie(frame); shown++;
        { static const char *d; static int init; if (!init) { init = 1; d = getenv("PSPRECOMP_MOVIE_SHOT"); }   /* debug: frame 60 of each movie as a .ppm */
          if (d && shown == 60) { FILE *w = fopen(d, "wb"); if (w) { fprintf(w, "P6\n%d %d\n255\n", VW, VH);
              for (int i = 0; i < VW * VH; i++) { const uint8_t px[3] = { (uint8_t)(frame[i] >> 16), (uint8_t)(frame[i] >> 8), (uint8_t)frame[i] }; fwrite(px, 1, 3, w); } fclose(w); } } }
    }
    TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    if (skipped || !shown) psp_audio_host_reset(MOVIE_CH);   /* no video decoded: no audio tail over a frozen screen */
    else while (pcm && apos < nfr) {   /* let a tail of audio past the last frame finish */
        if (psp_audio_host_queued(MOVIE_CH) < 6615) { const size_t k = nfr - apos < 1024 ? nfr - apos : 1024; psp_audio_host_push(MOVIE_CH, pcm + apos * 2, (uint32_t)k, 0, 0x8000, 0x8000); apos += k; }
        else Sleep(5);
        if (skip_pressed() || psp_window_closed()) { psp_audio_host_reset(MOVIE_CH); break; } }
    psp_window_movie(NULL);
    free(frame); free(pcm); DeleteFileA(tmp);
    fprintf(stderr, "movie: %ld frames%s\n", shown, skipped ? " (skipped)" : "");
    return 1;
}
#else
int psp_movie_play(const char *path) { (void)path; return 0; }
#endif
