/* sceAtrac3plus -- real ATRAC3 / ATRAC3+ decoding.
 *
 * The codec itself is FFmpeg's, vendored through PPSSPP's at3_standalone
 * (third_party/at3_standalone, LGPL 2.1). This file is the PSP library around
 * it: the RIFF header, the game-fed input buffer, loops and the play position.
 *
 * Two buffer models, picked when data is set:
 *
 *  - linear: the buffer is at least as large as the file. File offset f lives
 *    at buf + f. The game may still fill it in pieces (SetHalfwayBuffer), so
 *    `filled` says how much of the file is there. Loops just move the frame.
 *
 *  - stream: the buffer is smaller than the file and is used as a FIFO of
 *    file bytes in the order the decoder will consume them. GetStreamDataInfo
 *    tells the game where to write and which file offset to read next;
 *    AddStreamData commits it. When the producer reaches the loop end (or the
 *    end of the data) with loops remaining, it continues from the loop start,
 *    so the bytes still arrive in decode order.
 *
 * Output is always signed 16-bit interleaved stereo (mono is duplicated),
 * which is what DecodeData's callers on this firmware expect. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "at3_c.h"

#define MAX_ATRAC 6
#define ERR_BAD_ID          0x80630005u
#define ERR_NO_ID           0x80630006u
#define ERR_UNKNOWN_FORMAT  0x80630006u
#define ERR_BAD_DATA        0x80610002u
#define ERR_NO_DATA         0x80630010u
#define ERR_NO_SECONDBUF    0x80630022u
#define ERR_EMPTY           0x80630023u
#define ERR_ALL_DECODED     0x80630024u
#define ERR_NO_LOOP         0x80630021u

typedef struct {
    int used;
    int plus;                  /* 1 = ATRAC3+, 0 = ATRAC3 */
    void *dec;
    int channels, align, frame_samples;
    uint32_t buf, cap;         /* game buffer and its size */
    uint32_t file_size, data_off, data_end;
    uint32_t total;            /* samples in the track */
    uint32_t first_skip;       /* encoder delay to drop from the start */
    int32_t loop_start, loop_end;  /* samples; -1 = no loop */
    int loop_num;              /* remaining loops, -1 = forever */
    uint32_t pos;              /* next sample to output */
    int stream;
    /* linear */
    uint32_t filled;
    /* stream FIFO */
    uint32_t rd, cnt;          /* ring read index and bytes queued */
    uint32_t next_file;        /* next file offset the game should supply */
    uint32_t first_frame_skip; /* header bytes at the FIFO head on first fill */
    int out_ch;
} atrac_t;
static atrac_t g_at[MAX_ATRAC];

static atrac_t *get(uint32_t id) { return (id < MAX_ATRAC && g_at[id].used) ? &g_at[id] : NULL; }
static void wr(uint32_t p, uint32_t v) { if (p) psp_write32(p, v); }
static int alog(void) { static int v = -1; if (v < 0) v = getenv("PSPRECOMP_ATRAC_LOG") != NULL; return v; }

static void free_dec(atrac_t *a) {
    if (!a->dec) return;
    if (a->plus) psp_at3p_free(a->dec); else psp_at3_free(a->dec);
    a->dec = NULL;
}

/* Parse the RIFF header the game handed us. Returns 0 or an error code. */
static uint32_t parse(atrac_t *a, uint32_t buf, uint32_t avail) {
    if (avail < 12 || psp_read32(buf) != 0x46464952u || psp_read32(buf + 8) != 0x45564157u) return ERR_UNKNOWN_FORMAT;
    a->file_size = psp_read32(buf + 4) + 8;
    a->loop_start = a->loop_end = -1;
    a->total = 0; a->first_skip = 0;
    int have_fmt = 0;
    uint8_t extra[16]; int extra_n = 0;
    uint32_t o = 12;
    while (o + 8 <= avail) {
        uint32_t tag = psp_read32(buf + o), len = psp_read32(buf + o + 4), b = buf + o + 8;
        if (tag == 0x20746D66u) {                         /* fmt  */
            uint16_t fmt = psp_read16(b);
            a->channels = psp_read16(b + 2);
            a->align = psp_read16(b + 12);
            if (fmt == 0xFFFE) a->plus = 1;
            else if (fmt == 0x0270) {
                a->plus = 0;
                extra_n = len >= 18 ? psp_read16(b + 16) : 0;
                if (extra_n > 14) extra_n = 14;
                for (int i = 0; i < extra_n; i++) extra[i] = psp_read8(b + 18 + i);
            } else return ERR_UNKNOWN_FORMAT;
            have_fmt = 1;
        } else if (tag == 0x74636166u && len >= 4) {      /* fact */
            a->total = psp_read32(b);
            if (len >= 8) a->first_skip = psp_read32(b + 4);
        } else if (tag == 0x6C706D73u && len >= 36) {     /* smpl */
            if (psp_read32(b + 28) >= 1 && len >= 60) {
                a->loop_start = (int32_t)psp_read32(b + 44);
                a->loop_end = (int32_t)psp_read32(b + 48);
            }
        } else if (tag == 0x61746164u) {                  /* data */
            a->data_off = o + 8;
            a->data_end = a->data_off + len;
            if (a->data_end > a->file_size) a->data_end = a->file_size;
            break;
        }
        o += 8 + ((len + 1) & ~1u);
    }
    if (!have_fmt || !a->data_off || a->align <= 0 || a->channels < 1 || a->channels > 2) return ERR_UNKNOWN_FORMAT;
    a->frame_samples = a->plus ? 2048 : 1024;
    uint32_t frames = (a->data_end - a->data_off) / (uint32_t)a->align;
    if (!a->total || a->total > frames * (uint32_t)a->frame_samples) a->total = frames * (uint32_t)a->frame_samples;
    /* The fact delay counts samples the decoder emits before the track begins. */
    if (a->first_skip >= (uint32_t)a->frame_samples * 4) a->first_skip = 0;
    /* smpl loop points count from the start of the file's samples, delay included. */
    if (a->loop_start >= 0) {
        a->loop_start -= (int32_t)a->first_skip; a->loop_end -= (int32_t)a->first_skip;
        if (a->loop_start < 0) a->loop_start = 0;
    }
    if (a->loop_end >= (int32_t)a->total) a->loop_end = (int32_t)a->total - 1;
    free_dec(a);
    int ba = a->align;
    a->dec = a->plus ? psp_at3p_alloc(a->channels, &ba)
                     : psp_at3_alloc(a->channels, &ba, extra_n ? extra : NULL, extra_n);
    if (!a->dec) return ERR_UNKNOWN_FORMAT;
    if (alog()) fprintf(stderr, "atrac: %s ch %d align %d total %u skip %u loop %d..%d file %u data %u..%u\n",
                        a->plus ? "at3+" : "at3", a->channels, a->align, a->total, a->first_skip,
                        a->loop_start, a->loop_end, a->file_size, a->data_off, a->data_end);
    return 0;
}

static uint32_t frame_off(atrac_t *a, uint32_t frame) { return a->data_off + frame * (uint32_t)a->align; }
/* File frame holding output sample `pos` (the encoder delay shifts everything). */
static uint32_t frame_of(atrac_t *a, uint32_t pos) { return (pos + a->first_skip) / (uint32_t)a->frame_samples; }

static uint32_t set_buffer(atrac_t *a, uint32_t buf, uint32_t read, uint32_t cap) {
    a->buf = buf; a->cap = cap; a->pos = 0;
    uint32_t e = parse(a, buf, read < cap ? read : cap);
    if (e) return e;
    a->loop_num = 0;
    a->out_ch = 2;
    if (cap >= a->file_size) {
        a->stream = 0;
        a->filled = read > a->file_size ? a->file_size : read;
    } else {
        a->stream = 1;
        if (read > cap) read = cap;
        a->rd = a->data_off;                 /* header bytes sit at the FIFO head */
        a->cnt = read > a->data_off ? read - a->data_off : 0;
        a->next_file = read;
        if (a->rd > cap) return ERR_BAD_DATA;
    }
    return 0;
}

/* --- the stream producer: which file bytes come next, in decode order ------ */

static uint32_t loop_end_off(atrac_t *a) {
    /* Bytes past the frame that holds the loop end are never decoded while looping. */
    return frame_off(a, frame_of(a, (uint32_t)a->loop_end) + 1);
}
static int loops_left(atrac_t *a) { return a->loop_start >= 0 && a->loop_num != 0; }
static uint32_t stream_end(atrac_t *a) {
    if (loops_left(a)) { uint32_t e = loop_end_off(a); return e < a->data_end ? e : a->data_end; }
    return a->data_end;
}
static void producer_wrap(atrac_t *a) {
    if (a->next_file >= stream_end(a) && loops_left(a))
        a->next_file = frame_off(a, frame_of(a, (uint32_t)a->loop_start));
}
static int producer_done(atrac_t *a) { return !loops_left(a) && a->next_file >= a->data_end; }

static void stream_info(atrac_t *a, uint32_t *ptr, uint32_t *writable, uint32_t *file_off) {
    producer_wrap(a);
    uint32_t wi = (a->rd + a->cnt) % a->cap;
    uint32_t space = a->cap - a->cnt;
    uint32_t contig = a->cap - wi;
    uint32_t left = stream_end(a) > a->next_file ? stream_end(a) - a->next_file : 0;
    uint32_t n = space < contig ? space : contig;
    if (n > left) n = left;
    *ptr = a->buf + wi; *writable = n; *file_off = a->next_file;
}

/* Frames whose bytes are already in memory, from the current position on. */
static int remain_frames(atrac_t *a) {
    if (!a->stream) return a->filled >= a->file_size ? -1 : (int)((a->filled > frame_off(a, frame_of(a, a->pos)) ? a->filled - frame_off(a, frame_of(a, a->pos)) : 0) / (uint32_t)a->align);
    if (producer_done(a)) return -1;
    return (int)(a->cnt / (uint32_t)a->align);
}

/* Fetch the bytes of the frame that holds `pos`. Returns 0 if not yet supplied. */
static int fetch_frame(atrac_t *a, uint8_t *dst) {
    uint32_t n = (uint32_t)a->align;
    if (!a->stream) {
        uint32_t off = frame_off(a, frame_of(a, a->pos));
        if (off + n > a->filled) return 0;
        psp_mem_read_block(dst, a->buf + off, n);
        return 1;
    }
    if (a->cnt < n) return 0;
    for (uint32_t i = 0; i < n; i++) dst[i] = psp_read8(a->buf + (a->rd + i) % a->cap);
    a->rd = (a->rd + n) % a->cap; a->cnt -= n;
    return 1;
}

static void hle_DecodeData(void) {
    atrac_t *a = get(psp_arg(0));
    uint32_t out = psp_arg(1), psamp = psp_arg(2), pfin = psp_arg(3), prem = psp_arg(4);
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    if (!a->dec) { psp_ret(ERR_NO_DATA); return; }
    /* Reached the loop end with loops left: jump back. */
    if (a->loop_start >= 0 && a->loop_num != 0 && a->pos > (uint32_t)a->loop_end) {
        a->pos = (uint32_t)a->loop_start;
        if (a->loop_num > 0) a->loop_num--;
        if (!a->stream) { if (a->plus) psp_at3p_flush(a->dec); else psp_at3_flush(a->dec); }
    }
    if (a->pos >= a->total) { wr(psamp, 0); wr(pfin, 1); wr(prem, (uint32_t)remain_frames(a)); psp_ret(ERR_ALL_DECODED); return; }

    uint8_t frame[2048 + 64];
    if (a->align > 2048 || !fetch_frame(a, frame)) { wr(psamp, 0); wr(pfin, 0); wr(prem, 0); psp_ret(ERR_EMPTY); return; }
    memset(frame + a->align, 0, 64);
    float l[2048], r[2048]; float *planes[2] = { l, r };
    int nb = 0;
    int rc = a->plus ? psp_at3p_decode(a->dec, planes, &nb, frame, a->align)
                     : psp_at3_decode(a->dec, planes, &nb, frame, a->align);
    if (rc < 0 || nb <= 0) { nb = a->frame_samples; memset(l, 0, sizeof l); memset(r, 0, sizeof r);
                             if (alog()) fprintf(stderr, "atrac: frame decode error %d at %u\n", rc, a->pos); }
    if (a->channels == 1) memcpy(r, l, sizeof(float) * (size_t)nb);

    /* Position within the decoded frame, after the encoder delay. */
    uint32_t abs = a->pos + a->first_skip;
    uint32_t start = abs % (uint32_t)a->frame_samples;
    uint32_t n = (uint32_t)nb > start ? (uint32_t)nb - start : 0;
    if (n > a->total - a->pos) n = a->total - a->pos;
    if (a->loop_start >= 0 && a->loop_num != 0 && a->pos <= (uint32_t)a->loop_end && a->pos + n > (uint32_t)a->loop_end + 1)
        n = (uint32_t)a->loop_end + 1 - a->pos;
    if (out) {
        int16_t s[2048 * 2];
        for (uint32_t i = 0; i < n; i++) {
            float fl = l[start + i] * 32767.f, fr = r[start + i] * 32767.f;
            s[i * 2]     = (int16_t)(fl > 32767.f ? 32767 : fl < -32768.f ? -32768 : lrintf(fl));
            s[i * 2 + 1] = (int16_t)(fr > 32767.f ? 32767 : fr < -32768.f ? -32768 : lrintf(fr));
        }
        if (a->out_ch == 1) { for (uint32_t i = 0; i < n; i++) psp_write16(out + i * 2, (uint16_t)s[i * 2]); }
        else psp_mem_write_block(out, s, n * 4);
    }
    if (alog()) {
        static unsigned k; double e = 0;
        for (uint32_t i = 0; i < n; i++) e += (double)l[start + i] * l[start + i];
        if (k++ % 200 == 0) fprintf(stderr, "atrac: id %d pos %u n %u rms %.4f remain %d\n",
                                    (int)(a - g_at), a->pos, n, n ? sqrt(e / n) : 0.0, remain_frames(a));
    }
    a->pos += n;
    int fin = a->pos >= a->total && !(a->loop_start >= 0 && a->loop_num != 0);
    wr(psamp, n); wr(pfin, (uint32_t)fin); wr(prem, (uint32_t)remain_frames(a));
    psp_ret(0);
}

/* --- IDs and data ---------------------------------------------------------- */

static int alloc_id(void) {
    for (int i = 0; i < MAX_ATRAC; i++) if (!g_at[i].used) { memset(&g_at[i], 0, sizeof g_at[i]); g_at[i].used = 1; return i; }
    return -1;
}
static void hle_SetDataAndGetID(void) {
    int id = alloc_id();
    if (id < 0) { psp_ret(ERR_NO_ID); return; }
    uint32_t e = set_buffer(&g_at[id], psp_arg(0), psp_arg(1), psp_arg(1));
    if (e) { g_at[id].used = 0; psp_ret(e); return; }
    psp_ret((uint32_t)id);
}
static void hle_SetHalfwayBufferAndGetID(void) {
    int id = alloc_id();
    if (id < 0) { psp_ret(ERR_NO_ID); return; }
    uint32_t e = set_buffer(&g_at[id], psp_arg(0), psp_arg(1), psp_arg(2));
    if (e) { g_at[id].used = 0; psp_ret(e); return; }
    psp_ret((uint32_t)id);
}
static void hle_GetAtracID(void) { int id = alloc_id(); psp_ret(id < 0 ? ERR_NO_ID : (uint32_t)id); }
static void hle_SetData(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    psp_ret(set_buffer(a, psp_arg(1), psp_arg(2), psp_arg(2)));
}
static void hle_SetHalfwayBuffer(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    psp_ret(set_buffer(a, psp_arg(1), psp_arg(2), psp_arg(3)));
}
static void hle_Release(void) { atrac_t *a = get(psp_arg(0)); if (a) { free_dec(a); a->used = 0; } psp_ret(a ? 0 : ERR_BAD_ID); }

static void hle_GetStreamDataInfo(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    uint32_t p, n, f;
    if (a->stream) stream_info(a, &p, &n, &f);
    else { p = a->buf + a->filled; n = a->file_size - a->filled; f = a->filled; }
    wr(psp_arg(1), p); wr(psp_arg(2), n); wr(psp_arg(3), f);
    psp_ret(0);
}
static void hle_AddStreamData(void) {
    atrac_t *a = get(psp_arg(0));
    uint32_t n = psp_arg(1);
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    if (a->stream) {
        producer_wrap(a);
        if (n > a->cap - a->cnt) n = a->cap - a->cnt;
        a->cnt += n; a->next_file += n;
    } else {
        a->filled += n; if (a->filled > a->file_size) a->filled = a->file_size;
    }
    psp_ret(0);
}
static void hle_GetSecondBufferInfo(void) { wr(psp_arg(1), 0); wr(psp_arg(2), 0); psp_ret(ERR_NO_SECONDBUF); }
static void hle_SetSecondBuffer(void) { psp_ret(0); }
static void hle_IsSecondBufferNeeded(void) { psp_ret(0); }

/* --- queries --------------------------------------------------------------- */

static void hle_GetRemainFrame(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    wr(psp_arg(1), (uint32_t)remain_frames(a)); psp_ret(0);
}
static void hle_GetNextSample(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    uint32_t n = 0;
    if (a->pos < a->total) {
        uint32_t start = (a->pos + a->first_skip) % (uint32_t)a->frame_samples;
        n = (uint32_t)a->frame_samples - start;
        if (n > a->total - a->pos) n = a->total - a->pos;
    }
    wr(psp_arg(1), n); psp_ret(0);
}
static void hle_GetMaxSample(void) {
    atrac_t *a = get(psp_arg(0));
    wr(psp_arg(1), a ? (uint32_t)a->frame_samples : 2048); psp_ret(a ? 0 : ERR_BAD_ID);
}
static void hle_GetNextDecodePosition(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    if (a->pos >= a->total) { psp_ret(ERR_ALL_DECODED); return; }
    wr(psp_arg(1), a->pos); psp_ret(0);
}
static void hle_GetSoundSample(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    wr(psp_arg(1), a->total ? a->total - 1 : 0);
    wr(psp_arg(2), (uint32_t)a->loop_start); wr(psp_arg(3), (uint32_t)a->loop_end);
    psp_ret(0);
}
static void hle_SetLoopNum(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    if (a->loop_start < 0) { psp_ret(ERR_NO_LOOP); return; }
    a->loop_num = (int)psp_arg(1); psp_ret(0);
}
static void hle_GetLoopStatus(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    wr(psp_arg(1), (uint32_t)a->loop_num); wr(psp_arg(2), a->loop_start >= 0); psp_ret(0);
}
static void hle_GetBufferInfoForReseting(void) {
    atrac_t *a = get(psp_arg(0));
    uint32_t sample = psp_arg(1), p = psp_arg(2);
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    if (sample >= a->total) { psp_ret(0x80630028u); return; }  /* bad sample */
    uint32_t off = frame_off(a, frame_of(a, sample));
    if (p) {
        if (!a->stream) {
            uint32_t have = a->filled >= a->file_size;
            psp_write32(p + 0, a->buf + a->filled); psp_write32(p + 4, have ? 0 : a->file_size - a->filled);
            psp_write32(p + 8, 0); psp_write32(p + 12, a->filled);
        } else {
            uint32_t n = a->cap; if (n > a->data_end - off) n = a->data_end - off;
            psp_write32(p + 0, a->buf); psp_write32(p + 4, n);
            psp_write32(p + 8, (uint32_t)a->align); psp_write32(p + 12, off);
        }
        for (int i = 4; i < 8; i++) psp_write32(p + (uint32_t)i * 4, 0);
    }
    psp_ret(0);
}
static void hle_ResetPlayPosition(void) {
    atrac_t *a = get(psp_arg(0));
    uint32_t sample = psp_arg(1), first = psp_arg(2);
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    if (sample >= a->total) { psp_ret(0x80630028u); return; }
    a->pos = sample;
    if (a->stream) {
        uint32_t off = frame_off(a, frame_of(a, sample));
        a->rd = 0; a->cnt = first > a->cap ? a->cap : first; a->next_file = off + a->cnt;
    } else if (first) {
        a->filled += first; if (a->filled > a->file_size) a->filled = a->file_size;
    }
    if (a->plus) psp_at3p_flush(a->dec); else psp_at3_flush(a->dec);
    psp_ret(0);
}
static void hle_GetChannel(void) { atrac_t *a = get(psp_arg(0)); wr(psp_arg(1), a ? (uint32_t)a->channels : 2); psp_ret(a ? 0 : ERR_BAD_ID); }
static void hle_GetOutputChannel(void) { atrac_t *a = get(psp_arg(0)); wr(psp_arg(1), a ? (uint32_t)a->out_ch : 2); psp_ret(a ? 0 : ERR_BAD_ID); }
static void hle_GetBitrate(void) {
    atrac_t *a = get(psp_arg(0));
    if (!a) { psp_ret(ERR_BAD_ID); return; }
    /* kbps from bytes per frame at 44.1 kHz. */
    uint32_t kbps = (uint32_t)(((uint64_t)a->align * 8u * 44100u / (uint32_t)a->frame_samples + 500) / 1000);
    wr(psp_arg(1), kbps); psp_ret(0);
}
static void hle_GetInternalErrorInfo(void) { wr(psp_arg(1), 0); psp_ret(get(psp_arg(0)) ? 0 : ERR_BAD_ID); }
static void hle_Reinit(void) {
    for (int i = 0; i < MAX_ATRAC; i++) if (g_at[i].used) { free_dec(&g_at[i]); g_at[i].used = 0; }
    psp_ret(0);
}

void psp_atrac_register(void) {
    psp_hle_register(0x7A20E7AF, "sceAtrac3plus", "sceAtracSetDataAndGetID", hle_SetDataAndGetID);
    psp_hle_register(0x0FAE370E, "sceAtrac3plus", "sceAtracSetHalfwayBufferAndGetID", hle_SetHalfwayBufferAndGetID);
    psp_hle_register(0x780F88D1, "sceAtrac3plus", "sceAtracGetAtracID", hle_GetAtracID);
    psp_hle_register(0x0E2A73AB, "sceAtrac3plus", "sceAtracSetData", hle_SetData);
    psp_hle_register(0x3F6E26B5, "sceAtrac3plus", "sceAtracSetHalfwayBuffer", hle_SetHalfwayBuffer);
    psp_hle_register(0x61EB33F5, "sceAtrac3plus", "sceAtracReleaseAtracID", hle_Release);
    psp_hle_register(0x6A8C3CD5, "sceAtrac3plus", "sceAtracDecodeData", hle_DecodeData);
    psp_hle_register(0x5D268707, "sceAtrac3plus", "sceAtracGetStreamDataInfo", hle_GetStreamDataInfo);
    psp_hle_register(0x7DB31251, "sceAtrac3plus", "sceAtracAddStreamData", hle_AddStreamData);
    psp_hle_register(0xA2BBA8BE, "sceAtrac3plus", "sceAtracGetSoundSample", hle_GetSoundSample);
    psp_hle_register(0x83E85EA0, "sceAtrac3plus", "sceAtracGetSecondBufferInfo", hle_GetSecondBufferInfo);
    psp_hle_register(0x83BF7AFD, "sceAtrac3plus", "sceAtracSetSecondBuffer", hle_SetSecondBuffer);
    psp_hle_register(0x9AE849A7, "sceAtrac3plus", "sceAtracGetRemainFrame", hle_GetRemainFrame);
    psp_hle_register(0x36FAABFB, "sceAtrac3plus", "sceAtracGetNextSample", hle_GetNextSample);
    psp_hle_register(0xD6A5F2F7, "sceAtrac3plus", "sceAtracGetMaxSample", hle_GetMaxSample);
    psp_hle_register(0xE23E3A35, "sceAtrac3plus", "sceAtracGetNextDecodePosition", hle_GetNextDecodePosition);
    psp_hle_register(0xA554A158, "sceAtrac3plus", "sceAtracGetBitrate", hle_GetBitrate);
    psp_hle_register(0x868120B5, "sceAtrac3plus", "sceAtracSetLoopNum", hle_SetLoopNum);
    psp_hle_register(0xFAA4F89B, "sceAtrac3plus", "sceAtracGetLoopStatus", hle_GetLoopStatus);
    psp_hle_register(0x644E5607, "sceAtrac3plus", "sceAtracResetPlayPosition", hle_ResetPlayPosition);
    psp_hle_register(0xCA3CA3D2, "sceAtrac3plus", "sceAtracGetBufferInfoForReseting", hle_GetBufferInfoForReseting);
    psp_hle_register(0x31668BAA, "sceAtrac3plus", "sceAtracGetChannel", hle_GetChannel);
    psp_hle_register(0xB3B5D042, "sceAtrac3plus", "sceAtracGetOutputChannel", hle_GetOutputChannel);
    psp_hle_register(0xE88F759B, "sceAtrac3plus", "sceAtracGetInternalErrorInfo", hle_GetInternalErrorInfo);
    psp_hle_register(0x132F1ECA, "sceAtrac3plus", "sceAtracReinit", hle_Reinit);
    psp_hle_register(0xECA32A99, "sceAtrac3plus", "sceAtracIsSecondBufferNeeded", hle_IsSecondBufferNeeded);
}
