/* psprecomp -- host audio output (see src/audio_host.c). */
#ifndef PSPRECOMP_AUDIO_HOST_H
#define PSPRECOMP_AUDIO_HOST_H
#include <stdint.h>

/* Queue `frames` 16-bit frames (stereo pairs, or one sample each when mono)
 * on hardware channel `ch`, scaled by volumes where 0x8000 is unity. */
void psp_audio_host_push(int ch, const int16_t *src, uint32_t frames, int mono, int lvol, int rvol);
/* Frames still waiting to play on `ch`. */
uint32_t psp_audio_host_queued(int ch);
/* Drop whatever `ch` has queued (on release). */
void psp_audio_host_reset(int ch);

#endif
