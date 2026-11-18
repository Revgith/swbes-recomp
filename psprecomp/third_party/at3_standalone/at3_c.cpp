// psprecomp: C entry points for the vendored ATRAC3/ATRAC3+ decoder.
#include "at3_decoders.h"
#include "at3_c.h"

extern "C" {
void *psp_at3p_alloc(int channels, int *block_align) { return atrac3p_alloc(channels, block_align); }
void psp_at3p_free(void *c) { atrac3p_free((ATRAC3PContext *)c); }
void psp_at3p_flush(void *c) { atrac3p_flush_buffers((ATRAC3PContext *)c); }
int psp_at3p_decode(void *c, float *out[2], int *nb, const uint8_t *buf, int size) {
    return atrac3p_decode_frame((ATRAC3PContext *)c, out, nb, buf, size);
}
void *psp_at3_alloc(int channels, int *block_align, const uint8_t *extra, int extra_size) {
    return atrac3_alloc(channels, block_align, extra, extra_size);
}
void psp_at3_free(void *c) { atrac3_free((ATRAC3Context *)c); }
void psp_at3_flush(void *c) { atrac3_flush_buffers((ATRAC3Context *)c); }
int psp_at3_decode(void *c, float *out[2], int *nb, const uint8_t *buf, int size) {
    return atrac3_decode_frame((ATRAC3Context *)c, out, nb, buf, size);
}
}
