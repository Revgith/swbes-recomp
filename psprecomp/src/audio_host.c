/* psprecomp -- host audio output.
 *
 * The PSP's sceAudio has 8 hardware channels, each fed 16-bit stereo blocks by
 * the game. Here every channel is a small queue; a host thread mixes the queues
 * and keeps a waveOut stream (44.1 kHz stereo) fed. The guest is still paced by
 * the HLE's own blocking model, so the queues only absorb jitter: one that grows
 * past MAX_QUEUE drops its oldest samples instead of drifting into latency, and
 * one that runs dry plays silence.
 *
 * PSPRECOMP_MUTE=1 turns output off. Non-Windows builds have no backend yet and
 * accept the samples silently. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psprecomp/audio_host.h"
#include "psprecomp/window.h"

#define CHANNELS 9                   /* 0-7 sceAudio, 8 native movies (movie.c) */
#define RATE 44100
#define MAX_QUEUE (RATE / 4)         /* 250 ms per channel */
#define BLOCK 512                    /* frames per waveOut buffer (~11.6 ms) */
#define NBUF 6

typedef struct { int16_t s[MAX_QUEUE * 2]; uint32_t rd, n; } queue_t;
static queue_t g_q[CHANNELS];

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

static CRITICAL_SECTION g_lock;
static HWAVEOUT g_wo;
static HANDLE g_ev, g_thread;
static WAVEHDR g_hdr[NBUF];
static int16_t g_buf[NBUF][BLOCK * 2];
static volatile LONG g_state;        /* 0 = not started, 1 = running, -1 = unavailable */

static void mix(int16_t *out, uint32_t frames) {
    int32_t acc[BLOCK * 2];
    memset(acc, 0, sizeof acc);
    EnterCriticalSection(&g_lock);
    for (int c = 0; c < CHANNELS; c++) {
        queue_t *q = &g_q[c];
        uint32_t n = q->n < frames ? q->n : frames;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = (q->rd + i) % MAX_QUEUE;
            acc[i * 2] += q->s[k * 2]; acc[i * 2 + 1] += q->s[k * 2 + 1];
        }
        q->rd = (q->rd + n) % MAX_QUEUE; q->n -= n;
    }
    LeaveCriticalSection(&g_lock);
    for (uint32_t i = 0; i < frames * 2; i++)
        out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
    /* PSPRECOMP_AUDIO_WAV=<path>: also write the mix as raw 44.1 kHz stereo s16 (no header). */
    static FILE *cap; static int tried;
    if (!tried) { const char *w = getenv("PSPRECOMP_AUDIO_WAV"); tried = 1; if (w) cap = fopen(w, "wb"); }
    if (cap) { fwrite(out, 4, frames, cap); fflush(cap); }
}

static DWORD WINAPI pump(LPVOID unused) {
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_ev, 50);
        for (int b = 0; b < NBUF; b++) {
            if (!(g_hdr[b].dwFlags & WHDR_DONE)) continue;
            mix(g_buf[b], BLOCK);
            g_hdr[b].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wo, &g_hdr[b], sizeof g_hdr[b]);
        }
    }
}

static int start(void) {
    if (g_state) return g_state > 0;
    if (getenv("PSPRECOMP_MUTE")) { g_state = -1; return 0; }
    InitializeCriticalSection(&g_lock);
    WAVEFORMATEX f = {0};
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = RATE;
    f.wBitsPerSample = 16; f.nBlockAlign = 4; f.nAvgBytesPerSec = RATE * 4;
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &f, (DWORD_PTR)g_ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) { g_state = -1; return 0; }
    /* PSPRECOMP_SILENT: full audio path, no sound (tests). This sets the app's
     * volume in the Windows mixer, which Windows remembers per program, so a
     * normal run sets it back to full or one silent test mutes every later run. */
    waveOutSetVolume(g_wo, getenv("PSPRECOMP_SILENT") ? 0 : 0xFFFFFFFFu);
    for (int b = 0; b < NBUF; b++) {
        memset(g_buf[b], 0, sizeof g_buf[b]);
        g_hdr[b].lpData = (LPSTR)g_buf[b]; g_hdr[b].dwBufferLength = sizeof g_buf[b];
        waveOutPrepareHeader(g_wo, &g_hdr[b], sizeof g_hdr[b]);
        waveOutWrite(g_wo, &g_hdr[b], sizeof g_hdr[b]);
    }
    g_thread = CreateThread(NULL, 0, pump, NULL, 0, NULL);
    SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
    g_state = 1;
    return 1;
}
#define LOCK() EnterCriticalSection(&g_lock)
#define UNLOCK() LeaveCriticalSection(&g_lock)
#else
static int start(void) { return 0; }
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

void psp_audio_host_push(int ch, const int16_t *src, uint32_t frames, int mono, int lvol, int rvol) {
    if (ch < 0 || ch >= CHANNELS || !src || !frames || !start()) return;
    LOCK();
    queue_t *q = &g_q[ch];
    const int vol = psp_settings()->volume;   /* PC settings menu */
    for (uint32_t i = 0; i < frames; i++) {
        int32_t l = mono ? src[i] : src[i * 2], r = mono ? src[i] : src[i * 2 + 1];
        l = l * lvol / 0x8000 * vol / 100; r = r * rvol / 0x8000 * vol / 100;
        if (q->n == MAX_QUEUE) { q->rd = (q->rd + 1) % MAX_QUEUE; q->n--; }   /* drop oldest */
        uint32_t k = (q->rd + q->n) % MAX_QUEUE;
        q->s[k * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        q->s[k * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        q->n++;
    }
    UNLOCK();
}

uint32_t psp_audio_host_queued(int ch) {
    if (ch < 0 || ch >= CHANNELS || g_state <= 0) return 0;   /* muted: the lock was never set up */
    LOCK(); uint32_t n = g_q[ch].n; UNLOCK();
    return n;
}

void psp_audio_host_reset(int ch) {
    if (ch < 0 || ch >= CHANNELS || g_state <= 0) return;
    LOCK(); g_q[ch].rd = g_q[ch].n = 0; UNLOCK();
}
