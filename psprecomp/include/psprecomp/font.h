/* psprecomp -- real-font text.
 *
 * The game draws text as sprites cut from its font atlas. The game side
 * registers that atlas (its texture hashes and where each character sits);
 * when an enabled mod carries a TrueType font (mods/<folder>/*.ttf or .otf),
 * the renderer draws those sprites from the font instead, laid out with the
 * font's own advances and kerning. */
#ifndef PSPRECOMP_FONT_H
#define PSPRECOMP_FONT_H
#include <stdint.h>

typedef struct { uint16_t cp, x0, y0, x1, y1, base; } psp_font_glyph;   /* ink box and the row's baseline, in atlas texels */

void psp_font_atlas(const uint64_t *hashes, int nh, const psp_font_glyph *g, int ng);

int psp_font_on(uint64_t hash);   /* a registered atlas, and a font mod is on */
/* The character drawn by the sprite with texels (u0,v0)-(u1,v1), or NULL. */
const psp_font_glyph *psp_font_find(float u0, float v0, float u1, float v1);
/* Cap height of the atlas's letters, in texels. */
float psp_font_cap(void);

/* A rasterised glyph in the cache (RGBA, white, coverage in alpha). */
typedef struct { int x, y, w, h; float xoff, yoff, adv; } psp_font_bm;
/* cap: cap height in pixels; xs: horizontal squeeze. prev: the code point
 * before (kerning, 0 for none), added to *kern in pixels. 0 if the font
 * lacks it (nothing to draw). */
int psp_font_get(uint32_t cp, uint32_t prev, float cap, float xs, psp_font_bm *out, float *kern);
/* The cache picture; rows [*y0, *y1) changed since the last call (none: *y1 <= *y0). */
const uint32_t *psp_font_cache(int *w, int *h, int *y0, int *y1);
#endif
