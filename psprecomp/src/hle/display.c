#include <time.h>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
/* psprecomp — sceDisplay.
 *
 * The PSP's display is a framebuffer address plus a pixel format, scanned out
 * at ~59.94 Hz. There is no blitting and no compositing: a game writes pixels
 * (usually via the GE) and tells sceDisplay where they are.
 *
 * That makes this small but pivotal — it is the first point in bring-up where
 * a recompiled game produces something you can *look at*. psp_display_capture()
 * exists for exactly that: dump whatever the game currently considers the
 * front buffer, and compare it against the same frame from an emulator.
 */

#include "psprecomp/hle.h"
#include "psprecomp/render.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PSP_SCREEN_W 480
#define PSP_SCREEN_H 272

/* Pixel formats, as passed to sceDisplaySetFrameBuf. */
#define PSP_DISPLAY_PIXEL_FORMAT_565  0
#define PSP_DISPLAY_PIXEL_FORMAT_5551 1
#define PSP_DISPLAY_PIXEL_FORMAT_4444 2
#define PSP_DISPLAY_PIXEL_FORMAT_8888 3

static uint32_t g_fb_addr;
static uint32_t g_fb_width;      /* in pixels, the stride -- usually 512 */
static uint32_t g_fb_format;
static uint32_t g_mode, g_mode_w, g_mode_h;
static uint64_t g_vblank_count;

void psp_display_reset(void) {
    g_fb_addr = 0;
    g_fb_width = 512;
    g_fb_format = PSP_DISPLAY_PIXEL_FORMAT_8888;
    g_mode = 0;
    g_mode_w = PSP_SCREEN_W;
    g_mode_h = PSP_SCREEN_H;
    g_vblank_count = 0;
}

void psp_display_init(void) { psp_display_reset(); }

uint64_t psp_display_vblanks(void) { return g_vblank_count; }

int psp_turbo(void) {
    static long until = -2;
    if (until == -2) { const char *e = getenv("PSPRECOMP_TURBO"); until = e ? atol(e) : -1;
        if (until >= 0) fprintf(stderr, "turbo: on until vblank %ld (0 = always)\n", until); }
    if (until < 0) return 0;
    if (until == 0 || g_vblank_count < (uint64_t)until) return 1;
    static int said; if (!said++) fprintf(stderr, "turbo: off at vblank %llu\n", (unsigned long long)g_vblank_count);
    return 0;
}
uint32_t psp_display_framebuffer(void) { return g_fb_addr; }

/* Debug command files (*.cmd in the working directory) used to be found by
 * trying to open each one, ~170 failed opens a second: 5% of the frame
 * profile. A directory change notification says when anything there changed;
 * a poller passes its own counter and gets 1 only when there is something new
 * since it last looked (and always the first time). */
int psp_cmd_changed(unsigned *seen) {
    static HANDLE h; static volatile LONG gen = 1; static int init;
    if (!init) { init = 1;
        h = FindFirstChangeNotificationA(".", FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
        if (h == INVALID_HANDLE_VALUE) h = NULL; }
    if (!h) return 1;   /* no notification: poll as before */
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { InterlockedIncrement(&gen); FindNextChangeNotification(h); }
    if (*seen == (unsigned)gen) return 0;
    *seen = (unsigned)gen;
    return 1;
}

/* Write the current framebuffer as a binary PPM. Returns 0 on success, -1 if
 * no framebuffer has been set yet (which is itself worth knowing during
 * bring-up: the game never got as far as showing anything). */
int psp_display_capture(const char *path) {
    static uint32_t px[PSP_SCREEN_W * PSP_SCREEN_H];
    if (psp_display_read(px) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", PSP_SCREEN_W, PSP_SCREEN_H);
    for (int i = 0; i < PSP_SCREEN_W * PSP_SCREEN_H; i++) {
        const uint32_t c = px[i];
        uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
        fwrite(rgb, 1, 3, f);            /* PPM is RGB; alpha is dropped */
    }
    fclose(f);
    return 0;
}

/* The front buffer, converted, for a window or a capture. Read while the game
 * runs: a frame being drawn into the *back* buffer is never seen, which is the
 * point of the game double-buffering. */
/* Live conversion of guest VRAM. Only called at a vblank (see below): read at
 * any other moment it catches the back buffer half-drawn or a buffer the game
 * just cleared, which shows up as flicker. */
static int read_vram_at(uint32_t addr, uint32_t stride, uint32_t fmt, uint32_t *out) {
    if (!addr) return -1;
    const psp_render_backend *rb = psp_render_current();
    if (rb->read_display && rb->read_display(addr, stride, (int)fmt, out) == 0) return 0;
    const int bpp = (fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) ? 4 : 2;
    for (int y = 0; y < PSP_SCREEN_H; y++) {
        for (int x = 0; x < PSP_SCREEN_W; x++) {
            uint32_t at = addr + (uint32_t)(y * (int)stride + x) * (uint32_t)bpp;
            uint32_t px = (bpp == 4) ? psp_read32(at) : psp_read16(at);
            out[y * PSP_SCREEN_W + x] = psp_color_expand(px, (int)fmt);
        }
    }
    return 0;
}

/* Completed frames, latched at vblank: two slots, the window reads the one
 * not being written. */
static uint32_t g_snap[2][PSP_SCREEN_W * PSP_SCREEN_H];
static volatile int g_snap_ready = -1;

/* Runs on the GE thread, after the lists the game queued before this vblank:
 * the buffer it reads is exactly the one the game was showing. */
static volatile LONG g_latch_pending;
static void latch_do(uint32_t addr, uint32_t stride, uint32_t fmt) {
    int slot = g_snap_ready == 0 ? 1 : 0;
#ifdef PSPRECOMP_DEV
    LARGE_INTEGER dq0; QueryPerformanceCounter(&dq0);
#endif
    if (read_vram_at(addr, stride, fmt, g_snap[slot]) == 0) { g_snap_ready = slot; void psp_window_kick(void); psp_window_kick(); }
#ifdef PSPRECOMP_DEV
    { static int on = -1; if (on < 0) on = getenv("PSPRECOMP_FPSLOG") != NULL;   /* latch gaps and slow readbacks */
      static LARGE_INTEGER f, p; LARGE_INTEGER c; if (on) { if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
        const double gap = p.QuadPart ? (double)(c.QuadPart - p.QuadPart) * 1000.0 / (double)f.QuadPart : 0, rd = (double)(c.QuadPart - dq0.QuadPart) * 1000.0 / (double)f.QuadPart; p = c;
        if (gap > 25.0 || rd > 8.0) fprintf(stderr, "latch spike: gap %.1f ms, read %.1f ms at %.3f s\n", gap, rd, (double)c.QuadPart / (double)f.QuadPart); } }
#endif
    InterlockedDecrement(&g_latch_pending);
}
static void latch_frame(void) {
    void psp_ge_drain(void);
    void psp_ge_submit_call(void (*)(uint32_t, uint32_t, uint32_t), uint32_t, uint32_t, uint32_t);
    /* At most one frame in flight: the game may run one frame ahead of what
     * the GE has drawn, never more (guest memory it reuses stays valid). */
    if (g_latch_pending > 0) psp_ge_drain();
    InterlockedIncrement(&g_latch_pending);
    psp_ge_submit_call(latch_do, g_fb_addr, g_fb_width, g_fb_format);
}

int psp_display_read(uint32_t *out) {
    int slot = g_snap_ready;
    if (slot < 0) return -1;
    memcpy(out, g_snap[slot], sizeof g_snap[0]);
    return 0;
}

/* ---- the calls ----------------------------------------------------------- */

static void hle_SetMode(void) {
    g_mode   = psp_arg(0);
    g_mode_w = psp_arg(1);
    g_mode_h = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetFrameBuf(void) {
    /* (topaddr, bufferwidth, pixelformat, sync) */
    g_fb_addr   = psp_arg(0);
    g_fb_width  = psp_arg(1);
    g_fb_format = psp_arg(2);
    if (getenv("PSPRECOMP_TRACE_GE")) { static int n; if (n++ < 40) fprintf(stderr, "SetFrameBuf %08X w %u fmt %u sync %u vbl %llu\n", psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3), (unsigned long long)g_vblank_count); }
    if (!g_fb_width) g_fb_width = 512;
    { static uint32_t last; if (g_fb_addr != last) { last = g_fb_addr; void psp_ge_bench_tick(void); psp_ge_bench_tick(); } }   /* a flip: frame boundary */
    {   /* PSPRECOMP_FPSLOG: game frames (buffer flips) per wall-clock second */
        static int on = -1; static uint32_t last_fb; static unsigned flips; static ULONGLONG t0;
        if (on < 0) on = getenv("PSPRECOMP_FPSLOG") != NULL;
        if (on && g_fb_addr != last_fb) { last_fb = g_fb_addr; flips++;
            { static LARGE_INTEGER f, p; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);   /* long frames */
              const double ms = p.QuadPart ? (double)(c.QuadPart - p.QuadPart) * 1000.0 / (double)f.QuadPart : 0; p = c;
              if (ms > 25.0) fprintf(stderr, "spike: %.1f ms at %.3f s\n", ms, (double)c.QuadPart / (double)f.QuadPart); }
            ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
            if (t - t0 >= 2000) { extern unsigned psp_clock_reads, psp_rtc_reads;
                fprintf(stderr, "fps: %.1f game frames/s (vbl %llu) clock reads %u rtc %u\n", flips * 1000.0 / (double)(t - t0), (unsigned long long)g_vblank_count, psp_clock_reads, psp_rtc_reads);
                psp_clock_reads = psp_rtc_reads = 0; flips = 0; t0 = t;
                { void psp_threadman_waitstats(FILE *); psp_threadman_waitstats(stderr); } } }
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* There is no scanout, so a vblank wait does not wait for one: it bumps the
 * counter and gives the CPU away. A game's main loop is usually
 * `render(); WaitVblank();`, which means this counter is the frame number --
 * the most useful single number to have during bring-up, because it tells you
 * whether the game is looping or stuck -- and the yield is what lets the
 * threads that feed that loop run. */
static void hle_WaitVblank(void) {
    g_vblank_count++;
    { extern volatile long psp_game_frames; psp_game_frames++;
      /* CPU cycles of the thread that presents frames (bench.ps1) */
      extern volatile unsigned long long psp_game_cycles; ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); psp_game_cycles = c;
      extern volatile unsigned long psp_game_tid; psp_game_tid = GetCurrentThreadId(); }
    if (g_vblank_count % 300 == 0 && getenv("PSPRECOMP_FPSLOG")) {
        static clock_t t0; clock_t t = clock();
        fprintf(stderr, "vbl %llu  %.1f vblanks/s\n", (unsigned long long)g_vblank_count, t0 ? 300.0 * CLOCKS_PER_SEC / (double)(t - t0) : 0.0);
        t0 = t;
    }
    { static __declspec(thread) char *base; static __declspec(thread) long last; char here;
      if (!base) base = &here;
      long used = (long)(base - &here);
      if (used > last + 65536 || (g_vblank_count % 600 == 0 && getenv("PSPRECOMP_FPSLOG"))) {
          fprintf(stderr, "stack: vbl %llu thread %lu grew %ld KB since first vblank wait\n",
                  (unsigned long long)g_vblank_count, (unsigned long)GetCurrentThreadId(), used / 1024);
          if (used > last) last = used; } }
    latch_frame();
    /* Real-time pacing: the PSP's vblank is 59.94 Hz, and the game times
     * itself by it, so a vblank wait must not return early or the game runs
     * fast. Deadline-based (no drift); if we fall behind by more than a few
     * frames the deadline resets instead of bursting to catch up.
     * PSPRECOMP_NOPACE=1 or turbo disables it. */
    if (!psp_turbo()) {
        static int off = -1; static LARGE_INTEGER freq; static LONGLONG next;
        if (off < 0) { off = getenv("PSPRECOMP_NOPACE") != NULL; QueryPerformanceFrequency(&freq); timeBeginPeriod(1); }
        if (!off) {
            static double hz = -1; if (hz < 0) { const char *e = getenv("PSPRECOMP_PACE_HZ"); hz = e ? atof(e) : 59.94; }
            const LONGLONG period = (LONGLONG)((double)freq.QuadPart / hz);
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (!next || now.QuadPart - next > period * 4) next = now.QuadPart;
            next += period;
            if (psp_realtime()) {   /* park until the deadline: other threads use the time */
                static int vfree = -1; if (vfree < 0) vfree = getenv("PSPRECOMP_VBLFREE") != NULL;
                if (vfree) { next = 0; psp_threadman_frame_wait_us(0); psp_ret(SCE_KERNEL_ERROR_OK); return; }
                QueryPerformanceCounter(&now);
                const LONGLONG left = next - now.QuadPart;
                psp_threadman_frame_wait_us(left > 0 ? (uint64_t)(left * 1000000 / freq.QuadPart) : 0);
                psp_ret(SCE_KERNEL_ERROR_OK);
                return;
            }
            for (;;) {
                QueryPerformanceCounter(&now);
                const LONGLONG left = next - now.QuadPart;
                if (left <= 0) break;
                if (left > freq.QuadPart / 500) Sleep(1);       /* > 2 ms: sleep */
                else SwitchToThread();                          /* last bit: yield-spin */
            }
        }
    }
    /* A frame boundary is the guest asking for time to pass, so it is the third
     * point where an interrupt is delivered (src/hle/intrman.c) -- and, on one
     * CPU, the most natural place to hand the CPU over. */
    psp_threadman_frame_wait();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_display_register(void) {
    psp_hle_register(0x0E20F177, "sceDisplay", "sceDisplaySetMode",           hle_SetMode);
    psp_hle_register(0x289D82FE, "sceDisplay", "sceDisplaySetFrameBuf",       hle_SetFrameBuf);
    psp_hle_register(0x36CDFADE, "sceDisplay", "sceDisplayWaitVblank",        hle_WaitVblank);
    psp_hle_register(0x46F186C3, "sceDisplay", "sceDisplayWaitVblankStartCB", hle_WaitVblank);
    psp_hle_register(0x984C27E7, "sceDisplay", "sceDisplayWaitVblankStart",   hle_WaitVblank);
}
