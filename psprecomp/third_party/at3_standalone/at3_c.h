/* psprecomp: C view of the vendored ATRAC3/ATRAC3+ decoder (FFmpeg-derived, LGPL 2.1,
 * extracted by the PPSSPP project). Contexts are opaque. Decoders write planar float
 * samples in [-1, 1]; each returns bytes consumed or a negative error. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *psp_at3p_alloc(int channels, int *block_align);
void psp_at3p_free(void *c);
void psp_at3p_flush(void *c);
int psp_at3p_decode(void *c, float *out[2], int *nb, const uint8_t *buf, int size);
void *psp_at3_alloc(int channels, int *block_align, const uint8_t *extra, int extra_size);
void psp_at3_free(void *c);
void psp_at3_flush(void *c);
int psp_at3_decode(void *c, float *out[2], int *nb, const uint8_t *buf, int size);
#ifdef __cplusplus
}
#endif
