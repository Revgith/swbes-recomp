/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl and sceAudio. Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"
#include "psprecomp/audio_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Kernel_Library ------------------------------------------------------
 * Interrupt masking. The mask belongs to the *thread* that opened the critical
 * section, not to the machine: with several guest threads on one CPU, a global
 * flag would let one thread's suspend silence interrupts inside another
 * thread's unrelated bracket. The scheduler owns that state, because it is
 * also what decides whether an interrupt may be delivered at a park. */

static void hle_CpuSuspendIntr(void) {
    psp_ret((uint32_t)psp_threadman_intr_suspend());   /* the cookie resume expects */
}

static void hle_CpuResumeIntr(void) {
    psp_threadman_intr_resume(psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- UtilsForUser -------------------------------------------------------- */

/* Cache maintenance. There is no cache to write back -- the recompiled code
 * and the GE share one flat backing store -- so these are genuinely no-ops
 * rather than unimplemented. */
static void hle_CacheOp(void) { psp_ret(SCE_KERNEL_ERROR_OK); }
/* The debug LEDs. Nothing to drive; the game only ever writes them. */
static void hle_SetGPO(void) { psp_ret(0); }
/* The 4 MB of "volatile" RAM at 0x08400000 that the system lends a game. The
 * runtime maps it as ordinary memory, so locking just reports where it is. */
static void hle_VolatileMemLock(void) {
    if (psp_arg(1)) psp_write32(psp_arg(1), 0x08400000u);
    if (psp_arg(2)) psp_write32(psp_arg(2), 0x00400000u);
    psp_ret(0);
}
/* The disc is always in and always ready. */
static void hle_UmdReady(void) { psp_ret(0); }
static void hle_UmdCheckMedium(void) { psp_ret(1); }
static void hle_UmdGetDriveStat(void) { psp_ret(0x32); }   /* PRESENT|READY|READABLE */
/* Firmware modules (net, AV codecs) are built into the runtime. */
static void hle_UtilityLoadModule(void) { psp_ret(0); }

static void hle_LibcTime(void) {
    time_t t = time(NULL);
    uint32_t out = psp_arg(0);
    if (out) psp_write32(out, (uint32_t)t);
    psp_ret((uint32_t)t);
}

static void hle_LibcClock(void) {
    /* Microseconds since start. A game that uses this for frame pacing needs
     * it to advance, so it is derived from the host clock rather than being a
     * constant -- a frozen clock makes a game either spin or run at infinite
     * speed, both of which look like a hang. */
    psp_ret((uint32_t)((uint64_t)clock() * 1000000ull / CLOCKS_PER_SEC));
}

static void hle_LibcGettimeofday(void) {
    uint32_t tv = psp_arg(0);
    if (tv) {
        time_t t = time(NULL);
        psp_write32(tv, (uint32_t)t);
        psp_write32(tv + 4, 0);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* General-purpose I/O pins, wired to the debug board. Nothing is connected. */
static void hle_GetGPI(void) { psp_ret(0); }

/* ---- StdioForUser -------------------------------------------------------- */
/* These return the file descriptors, which sceIoWrite then recognises. */
static void hle_Stdin(void)  { psp_ret(0); }
static void hle_Stdout(void) { psp_ret(1); }
static void hle_Stderr(void) { psp_ret(2); }

/* ---- sceSuspendForUser --------------------------------------------------- */
/* Power management around suspend. Nothing suspends here. */
static void hle_ok(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- LoadExecForUser ----------------------------------------------------- */

static int g_exit_requested;
int psp_exit_requested(void) { return g_exit_requested; }

static void hle_ExitGame(void) {
    /* A game calling this is finished. Recording it rather than terminating
     * the process lets the host report what happened and dump its counters. */
    g_exit_requested = 1;
    fprintf(stderr, "psprecomp: the game called sceKernelExitGame\n");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RegisterExitCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- ModuleMgrForUser ---------------------------------------------------- */
/* A game-sharing microgame is self-contained and does not load further
 * modules, so these report a plausible identity rather than doing anything.
 * A title that genuinely loads PRXs at run time will need real ones. */
/* These report the id of the one loaded module.
 *
 * A previous note here recorded the opposite -- that returning an id was a
 * "plausible lie" and an error was the truthful answer -- on the strength of a
 * test showing byte-identical output either way. **That test was run while
 * `jal` never assigned `$ra`**, so every non-leaf function in the program was
 * returning through a stale register. A falsification obtained under broken
 * codegen is not a falsification.
 *
 * Re-run after that fix, the two answers differ clearly: returning an id makes
 * the game's own "libc:_getmodreent: no reent structure" diagnostic disappear
 * and drops bad memory accesses from 3 to 0.
 *
 * And an id is the *truthful* answer. The question is "which module owns this
 * address", a self-contained microgame is exactly one module, and the host has
 * loaded it. Reporting 1 states that; reporting UNKNOWN_MODULE denies a module
 * that demonstrably exists. */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE 0x80020139u
#define PSP_MAIN_MODULE_ID 1u

static void hle_GetModuleId(void)          { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_GetModuleIdByAddress(void) { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_ModuleOk(void)             { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- sceCtrl ------------------------------------------------------------- */

static uint32_t g_buttons;
static uint8_t  g_analog_x = 128, g_analog_y = 128;
/* Test override of the stick, held in real time (host stick.cmd); -1 = off. */
int psp_stick_x = -1, psp_stick_y = -1;
static uint32_t g_ctrl_frame;

void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    g_buttons = buttons;
    g_analog_x = ax;
    g_analog_y = ay;
}

/* A button press injected for a few controller reads (mouse clicks in game
 * menus): starts `delay` game frames from now, lasts `reads` frames. */
static uint32_t g_pulse_bits, g_pulse_from, g_pulse_to;
void psp_ctrl_pulse(unsigned bits, unsigned delay, unsigned reads) {
    extern volatile long psp_game_frames;
    g_pulse_from = (uint32_t)psp_game_frames + delay; g_pulse_to = g_pulse_from + reads; g_pulse_bits = bits;
}

static void hle_CtrlSet(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16. */
static void hle_ReadBufferPositive(void) {
    uint32_t buf = psp_arg(0), count = psp_arg(1);
    if (!count) count = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = buf + i * 16;
        psp_write32(at, g_ctrl_frame++);
        /* PSPRECOMP_AUTOPRESS=<mask hex>: pulse those buttons (on 6 reads, off
         * 24) for unattended test runs that need to get through menus. */
        static long ap = -1;
        if (ap == -1) { const char *e = getenv("PSPRECOMP_AUTOPRESS"); ap = e ? (long)strtoul(e, NULL, 16) : 0; }
        /* PSPRECOMP_INPUTSEQ="read:mask,read:mask,...": hold mask (hex) for 6
         * reads starting at that controller-read count (decimal). */
        static int nseq = -1; static uint32_t sq_at[256], sq_mask[256];
        if (nseq < 0) {
            nseq = 0;
            const char *e = getenv("PSPRECOMP_INPUTSEQ");
            while (e && *e && nseq < 256) {
                char *end; sq_at[nseq] = (uint32_t)strtoul(e, &end, 10);
                if (*end != ':') break;
                sq_mask[nseq++] = (uint32_t)strtoul(end + 1, &end, 16);
                e = *end == ',' ? end + 1 : end;
            }
        }
        uint32_t seq = 0;
        for (int k = 0; k < nseq; k++)
            if (g_ctrl_frame >= sq_at[k] && g_ctrl_frame < sq_at[k] + 6) seq |= sq_mask[k];
        if (nseq > 0 && (g_ctrl_frame % 300) == 0) fprintf(stderr, "ctrl: read %u\n", g_ctrl_frame);
        psp_write8(at + 8, g_analog_x);
        psp_write8(at + 9, g_analog_y);
        /* Live driving: "input.cmd" holds hex masks; each is pressed 6 reads, then 14 released. */
        { static uint32_t q[4096]; static int qn, qi; static uint32_t qt;
          if ((g_ctrl_frame % 20) == 0) {
              { /* a new input.cmd replaces whatever is still queued */
                  static unsigned seen_in; extern int psp_cmd_changed(unsigned *);
                  FILE *cf = psp_cmd_changed(&seen_in) ? fopen("input.cmd", "rb") : NULL;
                  if (cf) { qn = qi = 0; char tok[32];
                      /* hex mask per 20-read slot; "wN" idles N slots; "sNAME" saves a shot */
                      while (qn < 4096 && fscanf(cf, "%31s", tok) == 1) {
                          if (tok[0] == 'w') { for (long n = strtol(tok + 1, NULL, 10); n > 0 && qn < 4096; n--) q[qn++] = 0; }
                          else if (tok[0] == 's') q[qn++] = 0x40000000u;
                          else if (tok[0] == 'r') q[qn++] = 0x40000001u;
                          else q[qn++] = (uint32_t)strtoul(tok, NULL, 16);
                      }
                      fclose(cf); remove("input.cmd"); fprintf(stderr, "input: queued %d slots\n", qn); }
              }
              qt = qi < qn ? q[qi++] : 0;
          }
          /* An entry with bit 31 set is an analog hold instead: bits 15..8 = x,
           * 7..0 = y (0x80 centre), held for the whole 20-read slot. */
          if (qt == 0x40000001u) { qt = 0; FILE *sf = fopen("ram.cmd", "wb"); if (sf) fclose(sf); }
          if (qt == 0x40000000u) { qt = 0; FILE *sf = fopen("shot.cmd", "wb"); if (sf) fclose(sf); fprintf(stderr, "input: shot at read %u\n", g_ctrl_frame); }
          if (qt & 0x80000000u) { psp_write8(at + 8, (uint8_t)(qt >> 8)); psp_write8(at + 9, (uint8_t)qt); }
          else if ((g_ctrl_frame % 20) < 6) seq |= qt; }
        { extern volatile long psp_game_frames; const uint32_t gf = (uint32_t)psp_game_frames;
          if (g_pulse_bits && gf >= g_pulse_from && gf < g_pulse_to) seq |= g_pulse_bits; }
        psp_write32(at + 4, g_buttons | seq | ((g_ctrl_frame % 30) < 6 ? (uint32_t)ap : 0u));
        if (psp_stick_x >= 0) { psp_write8(at + 8, (uint8_t)psp_stick_x); psp_write8(at + 9, (uint8_t)psp_stick_y); }
        for (int k = 10; k < 16; k++) psp_write8(at + (uint32_t)k, 0);
    }
    psp_ret(count);
}

/* ---- sceAudio ------------------------------------------------------------ */

#define AUDIO_CHANNELS 8

typedef struct { int reserved; uint32_t samples; uint32_t format; } audio_ch;
static audio_ch g_audio[AUDIO_CHANNELS];
static uint64_t g_audio_blocks;

uint64_t psp_audio_blocks(void) { return g_audio_blocks; }

static void hle_ChReserve(void) {
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: ChReserve ch %d samples %u format %u\n",
                (int)psp_arg(0), psp_arg(1), psp_arg(2));
    /* (channel, samplecount, format) -- channel -1 means "any". */
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (int i = 0; i < AUDIO_CHANNELS; i++)
            if (!g_audio[i].reserved) { ch = i; break; }
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 1;
    g_audio[ch].samples = psp_arg(1);
    g_audio[ch].format  = psp_arg(2);
    psp_ret((uint32_t)ch);
}

static void hle_ChRelease(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 0;
    psp_audio_host_reset(ch);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Hand one block to the host mixer. Format 0x10 is mono, anything else stereo;
 * volumes are 0..0x8000 (larger values are clamped like the hardware does). */
static void audio_submit(uint32_t ch, uint32_t lv, uint32_t rv, uint32_t buf) {
    g_audio_blocks++;
    if (ch >= AUDIO_CHANNELS || !buf) return;
    uint32_t n = g_audio[ch].samples;
    int mono = g_audio[ch].format == 0x10;
    if (!n || n > 4096) return;
    static int16_t tmp[4096 * 2];
    psp_mem_read_block(tmp, buf, n * (mono ? 2u : 4u));
    if (lv > 0xFFFF) lv = 0xFFFF;
    if (rv > 0xFFFF) rv = 0xFFFF;
    psp_audio_host_push((int)ch, tmp, n, mono, (int)lv, (int)rv);
}

/* (ch, vol, buf) */
static void hle_Output(void) {
    audio_submit(psp_arg(0), psp_arg(1), psp_arg(1), psp_arg(2));
    psp_ret(psp_arg(0) < AUDIO_CHANNELS ? g_audio[psp_arg(0)].samples : 0);
}
/* (ch, lvol, rvol, buf) */
static void hle_OutputPanned(void) {
    audio_submit(psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));
    psp_ret(psp_arg(0) < AUDIO_CHANNELS ? g_audio[psp_arg(0)].samples : 0);
}

/* The blocking forms hold the caller while the hardware drains a buffer, and
 * that is what paces a game's audio thread. Returning at once lets a mixer
 * loop spin forever and starve every other guest thread.
 *
 * Under real threads the hardware queue is modelled on the host clock: a
 * buffer is due one play time after the last, and the call only waits while
 * the queue is more than a buffer ahead. Waiting a fixed play time per call
 * did not work there. The scheduler is cooperative, each tick of the wait
 * handed the CPU to the game's main thread for a whole frame, and WTF's audio
 * ran at about 10 buffers a second instead of 172; its title waits for a
 * jingle that, by its own sample count, never finished. A thread that falls
 * behind now catches up without waiting, as it would against a real queue.
 * Inline mode keeps the virtual-time wait. */
#if PSPRECOMP_REAL_THREADS
static uint64_t host_now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static uint64_t g_audio_due_us[AUDIO_CHANNELS];
#endif

static void output_blocking(int panned) {
    uint32_t ch = psp_arg(0);
    uint32_t samples = ch < AUDIO_CHANNELS ? g_audio[ch].samples : 0;
    static int traced;
    if (getenv("PSPRECOMP_TRACE_SAS") && traced++ < 4)
        fprintf(stderr, "audio: OutputBlocking ch %u samples %u buf 0x%08X\n", ch, samples, psp_arg(2));
    if (panned) audio_submit(ch, psp_arg(1), psp_arg(2), psp_arg(3));
    else audio_submit(ch, psp_arg(1), psp_arg(1), psp_arg(2));
    const uint64_t play_us = (uint64_t)samples * 1000000u / 44100u;
#if PSPRECOMP_REAL_THREADS
    if (ch < AUDIO_CHANNELS) {
        uint64_t now = host_now_us();
        if (g_audio_due_us[ch] < now) g_audio_due_us[ch] = now;   /* underrun */
        g_audio_due_us[ch] += play_us;
        if (psp_turbo() && g_audio_due_us[ch] > now + 4 * play_us) g_audio_due_us[ch] = now + 4 * play_us;
        while (host_now_us() + play_us < g_audio_due_us[ch]) psp_threadman_yield();
    }
#else
    psp_threadman_delay(play_us);
#endif
    psp_ret(samples);
}
static void hle_OutputBlocking(void) { output_blocking(0); }
static void hle_OutputPannedBlocking(void) { output_blocking(1); }

/* The sample count a channel outputs per call. It is set at reserve time and
 * changed here; ignoring the change left WTF's channel at its reserved length,
 * so each blocking output waited for 4,096 samples while the game mixed 256,
 * and its audio ran 16 times slow. */
static void hle_SetChannelDataLen(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003); return; }   /* invalid channel */
    g_audio[ch].samples = psp_arg(1);
    if (getenv("PSPRECOMP_TRACE_SAS"))
        fprintf(stderr, "audio: SetChannelDataLen ch %u samples %u\n", ch, psp_arg(1));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Frames still queued for playback on the channel. */
static void hle_GetChannelRestLength(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003); return; }
    psp_ret(getenv("PSPRECOMP_REST_REAL") ? psp_audio_host_queued((int)ch) : 0);
}
/* (ch, format): 0 = stereo, 0x10 = mono. */
static void hle_ChangeChannelConfig(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003); return; }
    g_audio[ch].format = psp_arg(1);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
/* Volume travels with every output call here, so a separate change has nothing to hold. */
static void hle_ChangeChannelVolume(void) { psp_ret(psp_arg(0) < AUDIO_CHANNELS ? SCE_KERNEL_ERROR_OK : 0x80260003); }
/* sceIoDevctl(dev, cmd, in, inlen, out, outlen): the memory-stick and UMD
 * queries games make. The memory stick is reported inserted with 1 GiB free. */
static void hle_IoDevctl(void) {
    uint32_t cmd = psp_arg(1), in = psp_arg(2), out = psp_arg(4), outlen = psp_arg(5);
    switch (cmd) {
    case 0x02425818: {                  /* ms free space: in -> pointer to 5-word struct */
        uint32_t p = in ? psp_read32(in) : 0;
        if (p) {
            const uint32_t sector = 512, per_cluster = 64, clusters = (1u << 30) / (sector * per_cluster);
            psp_write32(p + 0, clusters); psp_write32(p + 4, clusters);
            psp_write32(p + 8, clusters); psp_write32(p + 12, sector); psp_write32(p + 16, per_cluster);
        }
        psp_ret(0); return;
    }
    case 0x02415821: case 0x02415822:   /* ms insert/eject callback (un)register: never fires */
    case 0x02415823:
        psp_ret(0); return;
    case 0x02025806:                    /* ms inserted */
    case 0x02025801:                    /* ms state: 4 = ready */
        if (out && outlen >= 4) psp_write32(out, cmd == 0x02025801 ? 4 : 1);
        psp_ret(0); return;
    case 0x01F20001:                    /* UMD disc type: game disc */
        if (out && outlen >= 8) { psp_write32(out, 0xFFFFFFFFu); psp_write32(out + 4, 0x10); }
        psp_ret(0); return;
    case 0x01F20002: case 0x01F20003:   /* UMD current/total LBA */
        if (out && outlen >= 4) psp_write32(out, 0);
        psp_ret(0); return;
    default:
        if (getenv("PSPRECOMP_TRACE_DEVCTL")) fprintf(stderr, "devctl: unhandled cmd %08X\n", cmd);
        psp_ret(0); return;
    }
}

static void hle_Output2GetRestSample(void) { psp_ret(getenv("PSPRECOMP_REST_REAL") ? psp_audio_host_queued(7) : 0); }

void psp_misc_reset(void) {
    /* The interrupt mask is per thread and lives in the scheduler; a reset
     * here cannot reach another thread's bracket, and should not. */
    g_exit_requested = 0;
    g_buttons = 0;
    g_analog_x = g_analog_y = 128;
    g_ctrl_frame = 0;
    memset(g_audio, 0, sizeof g_audio);
#if PSPRECOMP_REAL_THREADS
    memset(g_audio_due_us, 0, sizeof g_audio_due_us);
#endif
    g_audio_blocks = 0;
}

void psp_misc_init(void) { psp_misc_reset(); }

/* sceAudioOutput2*: the single "SRC" output, modelled as channel 7. */
#define OUTPUT2_CH 7
static void hle_Output2Reserve(void) {
    g_audio[OUTPUT2_CH].reserved = 1;
    g_audio[OUTPUT2_CH].samples = psp_arg(0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_Output2Release(void) { g_audio[OUTPUT2_CH].reserved = 0; psp_audio_host_end(OUTPUT2_CH); psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_Output2ChangeLength(void) { g_audio[OUTPUT2_CH].samples = psp_arg(0); psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_Output2OutputBlocking(void) {      /* (vol, buf) -> (ch, vol, buf) */
    psp_cpu.r[PSP_REG_A2] = psp_arg(1);
    psp_cpu.r[PSP_REG_A1] = psp_arg(0);
    psp_cpu.r[PSP_REG_A0] = OUTPUT2_CH;
    hle_OutputBlocking();
}

/* sceUtilityGetSystemParam*: English, X confirms, everything else 0. */
static void hle_GetSystemParamInt(void) {
    uint32_t id = psp_arg(0), v = 0;
    if (id == 8) v = 1;                /* language: English */
    else if (id == 9) v = 1;           /* button swap: X = enter */
    if (psp_arg(1)) psp_write32(psp_arg(1), v);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_GetSystemParamString(void) {
    static const char nick[] = "Player";
    uint32_t buf = psp_arg(1), len = psp_arg(2);
    for (uint32_t i = 0; buf && i < len && i < sizeof nick; i++)
        psp_write8(buf + i, (uint8_t)nick[i]);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceRtcGetCurrentTick(u64 *tick): microseconds since 0001-01-01. */
unsigned psp_rtc_reads;
static void hle_RtcGetCurrentTick(void) { psp_rtc_reads++;
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    uint64_t t = ((uint64_t)ts.tv_sec + 62135596800ull) * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
    if (psp_arg(0)) { psp_write32(psp_arg(0), (uint32_t)t); psp_write32(psp_arg(0) + 4, (uint32_t)(t >> 32)); }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceRtcGetCurrentClockLocalTime(ScePspDateTime *): u16 y,mo,d,h,mi,s; u32 us. */
static void hle_RtcGetLocalTime(void) {
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    time_t t = ts.tv_sec; struct tm *lt = localtime(&t);
    uint32_t o = psp_arg(0);
    if (o && lt) {
        psp_write16(o + 0, (uint16_t)(lt->tm_year + 1900)); psp_write16(o + 2, (uint16_t)(lt->tm_mon + 1));
        psp_write16(o + 4, (uint16_t)lt->tm_mday); psp_write16(o + 6, (uint16_t)lt->tm_hour);
        psp_write16(o + 8, (uint16_t)lt->tm_min);  psp_write16(o + 10, (uint16_t)lt->tm_sec);
        psp_write32(o + 12, (uint32_t)(ts.tv_nsec / 1000));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_savedata_register(void);
void psp_atrac_register(void);
void psp_misc_register(void) {
    psp_savedata_register();
    psp_atrac_register();
    psp_hle_register(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr", hle_CpuSuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr",  hle_CpuResumeIntr);

    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll",           hle_CacheOp);
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll", hle_CacheOp);
    psp_hle_register(0x3EE30821, "UtilsForUser", "sceKernelDcacheWritebackRange",           hle_CacheOp);
    psp_hle_register(0x34B9FA9E, "UtilsForUser", "sceKernelDcacheWritebackInvalidateRange", hle_CacheOp);
    psp_hle_register(0xBFA98062, "UtilsForUser", "sceKernelDcacheInvalidateRange",          hle_CacheOp);
    psp_hle_register(0x920F104A, "UtilsForUser", "sceKernelIcacheInvalidateAll",            hle_CacheOp);
    psp_hle_register(0xC2DF770E, "UtilsForUser", "sceKernelIcacheInvalidateRange",          hle_CacheOp);
    psp_hle_register(0x27CC57F0, "UtilsForUser", "sceKernelLibcTime",         hle_LibcTime);
    psp_hle_register(0x6AD345D7, "UtilsForUser", "sceKernelSetGPO",           hle_SetGPO);
    psp_hle_register(0x91E4F6A7, "UtilsForUser", "sceKernelLibcClock",        hle_LibcClock);
    psp_hle_register(0x71EC4271, "UtilsForUser", "sceKernelLibcGettimeofday", hle_LibcGettimeofday);
    psp_hle_register(0x37FB5C42, "UtilsForUser", "sceKernelGetGPI",           hle_GetGPI);

    psp_hle_register(0x172D316E, "StdioForUser", "sceKernelStdin",  hle_Stdin);
    psp_hle_register(0xA6BAB2E9, "StdioForUser", "sceKernelStdout", hle_Stdout);
    psp_hle_register(0xF78BA90A, "StdioForUser", "sceKernelStderr", hle_Stderr);

    psp_hle_register(0xEADB1BD7, "sceSuspendForUser", "sceKernelPowerLock",   hle_ok);
    psp_hle_register(0x3AEE7261, "sceSuspendForUser", "sceKernelPowerUnlock", hle_ok);
    psp_hle_register(0x090CCB3F, "sceSuspendForUser", "sceKernelPowerTick",   hle_ok);
    psp_hle_register(0x3E0271D3, "sceSuspendForUser", "sceKernelVolatileMemLock",    hle_VolatileMemLock);
    psp_hle_register(0xA14F40B2, "sceSuspendForUser", "sceKernelVolatileMemTryLock", hle_VolatileMemLock);
    psp_hle_register(0xA569E425, "sceSuspendForUser", "sceKernelVolatileMemUnlock",  hle_ok);
    psp_hle_register(0xC6183D47, "sceUmdUser", "sceUmdActivate",                 hle_UmdReady);
    psp_hle_register(0x8EF08FCE, "sceUmdUser", "sceUmdWaitDriveStat",            hle_UmdReady);
    psp_hle_register(0x56202973, "sceUmdUser", "sceUmdWaitDriveStatWithTimer",   hle_UmdReady);
    psp_hle_register(0x4A9E5E29, "sceUmdUser", "sceUmdWaitDriveStatCB",          hle_UmdReady);
    psp_hle_register(0x46EBB729, "sceUmdUser", "sceUmdCheckMedium",              hle_UmdCheckMedium);
    psp_hle_register(0x6B4A146C, "sceUmdUser", "sceUmdGetDriveStat",             hle_UmdGetDriveStat);
    psp_hle_register(0x2A2B3DE0, "sceUtility", "sceUtilityLoadModule",           hle_UtilityLoadModule);
    psp_hle_register(0xE49BFE92, "sceUtility", "sceUtilityUnloadModule",         hle_UtilityLoadModule);

    psp_hle_register(0x05572A5F, "LoadExecForUser", "sceKernelExitGame",             hle_ExitGame);
    psp_hle_register(0x4AC57943, "LoadExecForUser", "sceKernelRegisterExitCallback", hle_RegisterExitCallback);

    /* Observed but unidentified. The game calls this three times on its
     * heap-setup path, and an unimplemented call returns 0 -- which for a
     * module query means "no module" and makes heap establishment fail.
     * Eighteen plausible ModuleMgr names were tried against SHA-1 with no
     * match, so the name is genuinely unknown and is not invented here. */
    psp_hle_register_unnamed(0xF9275D98, "ModuleMgrForUser", hle_GetModuleId);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",          hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress", hle_GetModuleIdByAddress);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",          hle_ModuleOk);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",           hle_ModuleOk);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",         hle_ModuleOk);

    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",     hle_CtrlSet);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",    hle_CtrlSet);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive",  hle_ReadBufferPositive);

    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_OutputBlocking);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_OutputPannedBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_OutputPanned);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_SetChannelDataLen);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ChangeChannelConfig);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ChangeChannelVolume);
    psp_hle_register(0x01562BA3, "sceAudio", "sceAudioOutput2Reserve",        hle_Output2Reserve);
    psp_hle_register(0x43196845, "sceAudio", "sceAudioOutput2Release",        hle_Output2Release);
    psp_hle_register(0x2D53F36E, "sceAudio", "sceAudioOutput2OutputBlocking", hle_Output2OutputBlocking);
    psp_hle_register(0x63F2889C, "sceAudio", "sceAudioOutput2ChangeLength",   hle_Output2ChangeLength);
    psp_hle_register(0x647CEF33, "sceAudio", "sceAudioOutput2GetRestSample",  hle_Output2GetRestSample);
    psp_hle_register(0xA5DA2406, "sceUtility", "sceUtilityGetSystemParamInt",    hle_GetSystemParamInt);
    psp_hle_register(0xE7C27D1B, "sceRtc", "sceRtcGetCurrentClockLocalTime", hle_RtcGetLocalTime);
    psp_hle_register(0x3F7AD767, "sceRtc", "sceRtcGetCurrentTick",            hle_RtcGetCurrentTick);
    psp_hle_register(0x36AA6E91, "sceImpose", "sceImposeSetLanguageMode",     hle_ok);
    psp_hle_register(0xAEE7404D, "sceUmdUser", "sceUmdRegisterUMDCallBack",   hle_ok);
    psp_hle_register(0x54F5FB11, "IoFileMgrForUser", "sceIoDevctl",           hle_IoDevctl);
    psp_hle_register(0x34B78343, "sceUtility", "sceUtilityGetSystemParamString", hle_GetSystemParamString);
}

/* Debug probe hook: generated code can be patched to call this. */
void psp_probe(uint32_t at) {
    static int n;
    if (n++ < 600)
        fprintf(stderr, "PROBE %08X a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X v1=%08X ra=%08X\n", at, psp_cpu.r[4], psp_cpu.r[5], psp_cpu.r[6], psp_cpu.r[7], psp_cpu.r[8], psp_cpu.r[3], psp_cpu.r[31]);
}
