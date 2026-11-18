/* psprecomp — sceGe_user.
 *
 * The GE is the PSP's GPU. It is not driven by function calls: user code builds
 * a **display list** — an array of 32-bit words, each an 8-bit command and 24
 * bits of argument — and hands the GE a pointer plus a *stall address*. The GE
 * consumes commands up to the stall, and the CPU moves the stall forward as it
 * writes more. That producer/consumer arrangement is the whole API.
 *
 * So `sceGu*` (the list-building library) is ordinary user code and gets
 * recompiled like anything else. Only list *execution* is emulated, and that is
 * this file.
 *
 * ## What this does and does not do
 *
 * It walks the list, follows control flow (JUMP/CALL/RET/END/FINISH), tracks
 * the state commands that matter, decodes and transforms vertices, and hands
 * assembled primitives plus a psp_draw_state to the render backend, which
 * rasterizes them (src/render.c). See the rasterizer section below for what
 * is and is not decoded.
 *
 * psp_ge_dump_stats() reports what the game asked for, including every
 * command not decoded, by opcode, with the last argument it carried. During
 * bring-up that census answers "what is the game drawing, and with what state"
 * before any pixel is trusted.
 */

#include "psprecomp/hle.h"
#include "psprecomp/render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <smmintrin.h>
/* floor by one SSE4.1 instruction: exactly floorf, without the CRT call */
static __forceinline float ffl(float x) { return _mm_cvtss_f32(_mm_round_ss(_mm_setzero_ps(), _mm_set_ss(x), _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC)); }
#endif

/* Display-list opcodes. Only the ones the walk needs to be correct about are
 * named; everything else is counted rather than guessed at, because a
 * misidentified state command silently changes rendering. */
#define GE_NOP          0x00
#define GE_VADDR        0x01
#define GE_IADDR        0x02
#define GE_PRIM         0x04
#define GE_BEZIER       0x05
#define GE_SPLINE       0x06
#define GE_JUMP         0x08
#define GE_BJUMP        0x09
#define GE_CALL         0x0A
#define GE_RET          0x0B
#define GE_END          0x0C
#define GE_SIGNAL       0x0E
#define GE_FINISH       0x0F
#define GE_BASE         0x10
#define GE_VTYPE        0x12
#define GE_OFFSET_ADDR  0x13
#define GE_ORIGIN_ADDR  0x14
#define GE_TME          0x1E
#define GE_LTE          0x17
#define GE_ABE          0x21
#define GE_ATE          0x22
#define GE_ZTE          0x23
#define GE_BCE          0x1D
#define GE_CULL         0x9B
#define GE_ZSCALE       0x44
#define GE_ZPOS         0x47
#define GE_RNORM        0x51
#define GE_CMAT         0x53
#define GE_EMC          0x54
#define GE_DMC          0x56
#define GE_SMC          0x57
#define GE_SPOW         0x5B
#define GE_ALC          0x5C
#define GE_ALA          0x5D
#define GE_ZBP          0x9E
#define GE_ZBW          0x9F
#define GE_ZTST         0xDE
#define GE_ZMSK         0xE7
#define GE_WMS          0x3A
#define GE_WORLD        0x3B
#define GE_VMS          0x3C
#define GE_VIEW         0x3D
#define GE_PMS          0x3E
#define GE_PROJ         0x3F
#define GE_XSCALE       0x42
#define GE_YSCALE       0x43
#define GE_XPOS         0x45
#define GE_YPOS         0x46
#define GE_USCALE       0x48
#define GE_VSCALE       0x49
#define GE_UOFFSET      0x4A
#define GE_VOFFSET      0x4B
#define GE_OFFSETX      0x4C
#define GE_OFFSETY      0x4D
#define GE_SHADE        0x50
#define GE_AMC          0x55
#define GE_AMA          0x58
#define GE_FBP          0x9C
#define GE_FBW          0x9D
#define GE_TBP0         0xA0
#define GE_TBW0         0xA8
#define GE_CBP          0xB0
#define GE_CBPH         0xB1
#define GE_TRXSBP       0xB2
#define GE_TRXSBW       0xB3
#define GE_TRXDBP       0xB4
#define GE_TRXDBW       0xB5
#define GE_TRXKICK      0xEA
#define GE_TRXSPOS      0xEB
#define GE_TRXDPOS      0xEC
#define GE_TRXSIZE      0xEE
static struct { uint32_t sbp, sbw, dbp, dbw, sx, sy, dx, dy, w, h; } g_trx;
static void trx_kick(uint32_t arg);
#define GE_TSIZE0       0xB8
#define GE_TMODE        0xC2
#define GE_TPSM         0xC3
#define GE_CLOAD        0xC4
#define GE_CMODE        0xC5
#define GE_TWRAP        0xC7
#define GE_TFUNC        0xC9
#define GE_TEC          0xCA
#define GE_PSM          0xD2
#define GE_CLEAR        0xD3
#define GE_SCISSOR1     0xD4
#define GE_SCISSOR2     0xD5
#define GE_ATST         0xDB
#define GE_ALPHA        0xDF
#define GE_SFIX         0xE0
#define GE_DFIX         0xE1

#define MAX_QUEUES 8
#define GE_STACK   8

/* Primitive types, from the PRIM argument's type field. */
static const char *const PRIM_NAME[8] = {
    "points", "lines", "line-strip", "triangles",
    "triangle-strip", "triangle-fan", "sprites", "?"
};

typedef struct {
    uint32_t id;
    uint32_t list;      /* current read pointer */
    uint32_t stall;     /* stop before this address; 0 means "no stall" */
    uint32_t base;      /* GE_BASE: high bits for addresses */
    uint32_t origin;
    int      used;
    int      done;
} ge_queue;

static ge_queue g_queue[MAX_QUEUES];
static uint32_t g_next_id;

static uint64_t g_unsupported;     /* vertices in a format we do not read */

/* Draw state handed to the backend with every primitive, and the raw words it
 * is decoded from where one value spans two commands. */
static psp_draw_state g_ds;
static uint32_t g_tbp, g_tbw, g_cbp, g_psm;
static uint8_t  g_clut[1024];      /* the GE's CLUT cache, filled by CLOAD */
/* A vertex with no colour takes the material colour: AMC's RGB, AMA's alpha. */
static uint32_t g_material = 0xFFFFFFFFu;

/* Lighting. Up to four lights, in world space like the normals they are
 * compared with. CMAT says which material colours the vertex colour stands in
 * for: bit 0 ambient, 1 diffuse, 2 specular. */
static struct {
    int      enable, on[4], type[4], comp[4], cmat, rnorm;
    uint32_t emissive, diffuse, specular;       /* EMC, DMC, SMC */
    uint32_t ambient;                           /* ALC | ALA << 24 */
    float    spow;
    float    pos[4][3], dir[4][3], att[4][3], spot_exp[4], spot_cut[4];
    uint32_t col[4][3];                         /* ambient, diffuse, specular */
} g_lt;
static uint32_t g_zbp, g_zbw;

/* The transform pipeline. Matrices are uploaded one float per command, column
 * by column: world and view are 4x3 (the last column is the translation),
 * projection is 4x4. The viewport maps clip space into the GE's 4096-wide
 * drawing space, and the offset moves the screen's top-left within it. */
static struct {
    float world[12], view[12], proj[16];
    float bone[8 * 12];                 /* skinning matrices, 4x3 each */
    int   wi, vi, pi, bi;
    float xscale, yscale, xpos, ypos;
    float offx, offy;                   /* pixels */
    float uscale, vscale, uoff, voff;
    float zscale, zpos;
    uint64_t behind;                    /* vertices behind the eye */
} g_xf;

/* GE floats are the top 24 bits of an IEEE single. */
static float u2f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static float f24(uint32_t arg) {
    union { uint32_t u; float f; } c;
    c.u = arg << 8;
    return c.f;
}

static void xf_reset(void) {
    memset(&g_xf, 0, sizeof g_xf);
    g_xf.world[0] = g_xf.world[4] = g_xf.world[8] = 1;
    g_xf.view[0] = g_xf.view[4] = g_xf.view[8] = 1;
    g_xf.proj[0] = g_xf.proj[5] = g_xf.proj[10] = g_xf.proj[15] = 1;
    g_xf.uscale = g_xf.vscale = 1;
}

/* Model space to screen pixels. Returns 0 for a point behind the eye.
 * ponytail: no near-plane clipping, so a primitive with any vertex behind the
 * eye is dropped whole; add clipping when 3D scenes lose triangles. */
/* Clip-space position of the last transformed vertex, for near-plane clipping. */
static float g_lastclip[4];
static float g_fog_end, g_fog_scale;      /* FOG1, FOG2 */
static float g_vwz[4];                    /* view x world, its z row: view-space depth for fog */
static uint32_t g_tbpn[8], g_tbwn[8];     /* TBPn/TBWn for mip levels */
static float g_tmat[12]; static int g_tmi;   /* TMS/TMATRIX: texture matrix, 4x3 like WORLD */
static int g_tmap_mode, g_tmap_src, g_tshade_u, g_tshade_v;   /* TMAP, TEXSHADE */

/* proj x view x world as one 4x4 (column-major, [col*4 + row]), composed in
 * double once per primitive (mvp_prep, from draw_prim): per vertex that is
 * one product instead of three. */
static float g_mvp[16];
static void mvp_prep(void) {
    const float *W = g_xf.world, *V = g_xf.view, *P = g_xf.proj;
    double VW[12];   /* view x world, 4x3 affine: column c, row r at [c*3 + r] */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 3; r++) {
            double s = c == 3 ? V[9 + r] : 0.0;
            for (int k = 0; k < 3; k++) s += (double)V[k * 3 + r] * (double)W[c * 3 + k];
            VW[c * 3 + r] = s;
            if (r == 2) g_vwz[c] = (float)s;
        }
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            double s = c == 3 ? P[12 + r] : 0.0;
            for (int k = 0; k < 3; k++) s += (double)P[k * 4 + r] * VW[c * 3 + k];
            g_mvp[c * 4 + r] = (float)s;
        }
}

static float g_lastdx, g_lastdy;   /* drawing-space x,y of the last transformed vertex */
static __forceinline int transform(const float p[3], float *sx, float *sy, float *sz) {
    const float *W = g_xf.world, *V = g_xf.view, *P = g_xf.proj;
    const float *M = g_mvp, x = p[0], y = p[1], z = p[2];
    float c[4];
    for (int r = 0; r < 4; r++)
        c[r] = M[r] * x + M[4 + r] * y + M[8 + r] * z + M[12 + r];
    memcpy(g_lastclip, c, sizeof c);
    if (!isfinite(c[0] + c[1] + c[2] + c[3])) {     /* where do NaN vertices come from? */
        static int nn;
        if (nn++ < 6) {
            int bw = 0, bv = 0, bp = 0;
            for (int i = 0; i < 12; i++) { bw |= !isfinite(W[i]); bv |= !isfinite(V[i]); }
            for (int i = 0; i < 16; i++) bp |= !isfinite(P[i]);
            fprintf(stderr, "ge: NaN vertex p=(%g %g %g) bad world %d view %d proj %d  clip (%g %g %g %g) vp %g %g %g\n",
                    (double)p[0], (double)p[1], (double)p[2], bw, bv, bp,
                    (double)c[0], (double)c[1], (double)c[2], (double)c[3],
                    (double)g_xf.xscale, (double)g_xf.yscale, (double)g_xf.zscale);
            fprintf(stderr, "   P:"); for (int i = 0; i < 16; i++) fprintf(stderr, " %g", (double)P[i]);
            fprintf(stderr, "\n   V:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %g", (double)V[i]);
            fprintf(stderr, "\n   W:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %g", (double)W[i]);
            fprintf(stderr, "\n");
        }
    }
    if (c[3] <= 0) return 0;
    g_lastdx = c[0] / c[3] * g_xf.xscale + g_xf.xpos; *sx = g_lastdx - g_xf.offx;
    g_lastdy = c[1] / c[3] * g_xf.yscale + g_xf.ypos; *sy = g_lastdy - g_xf.offy;
    *sz = c[2] / c[3] * g_xf.zscale + g_xf.zpos;
    return 1;
}

/* ---- lighting ------------------------------------------------------------ */

static void unpack(uint32_t c, float out[4]) {
    for (int i = 0; i < 4; i++) out[i] = (float)((c >> (8 * i)) & 0xFF);
}

static __forceinline float norm3(float v[3]) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) { const float k = 1.0f / l; v[0] *= k; v[1] *= k; v[2] *= k; }
    return l;
}

/* The lit colour of a vertex at model-space `p` with normal `n`, in world
 * space: emissive + ambient light x ambient material, plus for each enabled
 * light its ambient, its diffuse by N.L, and its specular by (N.H)^SPOW with
 * the viewer along +z, all scaled by distance attenuation and the spot cone.
 * Alpha is the ambient material's. */
/* Everything in light_vertex that does not depend on the vertex: colours
 * unpacked, directions normalised, the enabled lights listed. Built once per
 * primitive (light_prep, from draw_prim) -- per vertex it was a top entry in
 * the frame profile. */
static struct {
    int   n, idx[4];
    float e[4], al[4], gm[4], dm[4], sm[4];
    float la[4][3], ld[4][3], ls[4][3];
    float L[4][3], D[4][3];            /* directional L, spot direction */
    float base[3];                     /* emissive + ambient, when CMAT leaves ambient constant */
    float ka[4][3], kd[4][3], ks[4][3]; /* light x material products, for constant materials */
} g_lp;

static void light_prep(void) {
    unpack(g_lt.emissive, g_lp.e);
    unpack(g_lt.ambient, g_lp.al);
    unpack(g_material, g_lp.gm);
    unpack(g_lt.diffuse, g_lp.dm);
    unpack(g_lt.specular, g_lp.sm);
    if (getenv("PSPRECOMP_LIGHTLOG")) { static int nl; if (nl++ < 400 && nl % 20 == 0) fprintf(stderr, "lights: on %d%d%d%d type %d%d%d%d comp %d%d%d%d cmat %d\n", g_lt.on[0], g_lt.on[1], g_lt.on[2], g_lt.on[3], g_lt.type[0], g_lt.type[1], g_lt.type[2], g_lt.type[3], g_lt.comp[0], g_lt.comp[1], g_lt.comp[2], g_lt.comp[3], g_lt.cmat); }
    for (int k = 0; k < 3; k++) g_lp.base[k] = g_lp.e[k] + g_lp.al[k] * g_lp.gm[k] / 255.0f;
    g_lp.n = 0;
    for (int i = 0; i < 4; i++) {
        if (!g_lt.on[i]) continue;
        g_lp.idx[g_lp.n++] = i;
        float c[4];
        unpack(g_lt.col[i][0], c); for (int k = 0; k < 3; k++) g_lp.la[i][k] = c[k] / 255.0f;
        unpack(g_lt.col[i][1], c); for (int k = 0; k < 3; k++) g_lp.ld[i][k] = c[k] / 255.0f;
        unpack(g_lt.col[i][2], c); for (int k = 0; k < 3; k++) g_lp.ls[i][k] = c[k] / 255.0f;
        for (int r = 0; r < 3; r++) { g_lp.L[i][r] = g_lt.pos[i][r]; g_lp.D[i][r] = g_lt.dir[i][r]; }
        norm3(g_lp.L[i]); norm3(g_lp.D[i]);
        for (int k = 0; k < 3; k++) { g_lp.ka[i][k] = g_lp.la[i][k] * g_lp.gm[k]; g_lp.kd[i][k] = g_lp.ld[i][k] * g_lp.dm[k]; g_lp.ks[i][k] = g_lp.ls[i][k] * g_lp.sm[k]; }
    }
}

static inline void unpack3n(uint32_t c, float out[4]) {
    out[0] = (float)(c & 0xFF); out[1] = (float)((c >> 8) & 0xFF);
    out[2] = (float)((c >> 16) & 0xFF); out[3] = (float)(c >> 24);
}

/* The lit colour of a vertex at model-space `p` with normal `n`, in world
 * space: emissive + ambient light x ambient material, plus for each enabled
 * light its ambient, its diffuse by N.L, and its specular by (N.H)^SPOW with
 * the viewer along +z, all scaled by distance attenuation and the spot cone.
 * Alpha is the ambient material's. */
static __forceinline uint32_t light_vertex(const float p[3], const float n[3], uint32_t vcol) {
    const float *W = g_xf.world;
    float wn[3];
    if (g_lp.n) {   /* the normal only matters to the lights: most lit draws have none on */
    for (int r = 0; r < 3; r++)
        wn[r] = W[r] * n[0] + W[3 + r] * n[1] + W[6 + r] * n[2];
    if (g_lt.rnorm) for (int r = 0; r < 3; r++) wn[r] = -wn[r];
    norm3(wn);
    }

    float vc[4], out[3];
    const float *ma, *md, *ms;
    if (g_lt.cmat & 7) unpack3n(vcol, vc);
    ma = g_lt.cmat & 1 ? vc : g_lp.gm;
    md = g_lt.cmat & 2 ? vc : g_lp.dm;
    ms = g_lt.cmat & 4 ? vc : g_lp.sm;
    const int cmat = g_lt.cmat & 7;
    if (!(cmat & 1)) { out[0] = g_lp.base[0]; out[1] = g_lp.base[1]; out[2] = g_lp.base[2]; }
    else for (int k = 0; k < 3; k++) out[k] = g_lp.e[k] + g_lp.al[k] * ma[k] / 255.0f;

    float wp[3]; int have_wp = 0;
    for (int j = 0; j < g_lp.n; j++) {
        const int i = g_lp.idx[j];
        float L[3], att = 1.0f;
        if (g_lt.type[i] == 0) {                /* directional: pos is the direction */
            L[0] = g_lp.L[i][0]; L[1] = g_lp.L[i][1]; L[2] = g_lp.L[i][2];
        } else {
            if (!have_wp) { have_wp = 1;
                for (int r = 0; r < 3; r++) wp[r] = W[r] * p[0] + W[3 + r] * p[1] + W[6 + r] * p[2] + W[9 + r]; }
            for (int r = 0; r < 3; r++) L[r] = g_lt.pos[i][r] - wp[r];
            const float d = norm3(L);
            const float den = g_lt.att[i][0] + g_lt.att[i][1] * d + g_lt.att[i][2] * d * d;
            att = den > 0 ? 1.0f / den : 1.0f;
            if (att > 1.0f) att = 1.0f;
            if (g_lt.type[i] == 2) {            /* spot */
                const float *D = g_lp.D[i];
                const float cs = -(L[0] * D[0] + L[1] * D[1] + L[2] * D[2]);
                att *= cs > g_lt.spot_cut[i] ? powf(cs, g_lt.spot_exp[i]) : 0.0f;
            }
        }
        float ndl = wn[0] * L[0] + wn[1] * L[1] + wn[2] * L[2];
        if (ndl < 0) ndl = 0;
        if (g_lt.comp[i] == 2) ndl = powf(ndl, g_lt.spow);
        float spec = 0;
        if (g_lt.comp[i] == 1 && ndl > 0) {
            float H[3] = { L[0], L[1], L[2] + 1.0f };
            norm3(H);
            const float ndh = wn[0] * H[0] + wn[1] * H[1] + wn[2] * H[2];
            if (ndh > 0) spec = powf(ndh, g_lt.spow);
        }
        if (!cmat)          /* constant materials: products from light_prep */
            for (int k = 0; k < 3; k++)
                out[k] += att * (g_lp.ka[i][k] + g_lp.kd[i][k] * ndl + g_lp.ks[i][k] * spec);
        else
            for (int k = 0; k < 3; k++)
                out[k] += att * (g_lp.la[i][k] * ma[k] + g_lp.ld[i][k] * md[k] * ndl + g_lp.ls[i][k] * ms[k] * spec);
    }
    uint32_t c = (uint32_t)ma[3] << 24;
    for (int k = 0; k < 3; k++) {
        const int v = out[k] > 255.0f ? 255 : (int)out[k];
        c |= (uint32_t)v << (8 * k);
    }
    return c;
}

/* Light and material commands, which come in blocks of three or four. */
static int light_cmd(uint32_t cmd, uint32_t arg) {
    if (cmd >= 0x18 && cmd <= 0x1B) { g_lt.on[cmd - 0x18] = arg & 1; return 1; }
    if (cmd >= 0x5F && cmd <= 0x62) {
        g_lt.comp[cmd - 0x5F] = (int)(arg & 3);
        g_lt.type[cmd - 0x5F] = (int)((arg >> 8) & 3);
        return 1;
    }
    if (cmd >= 0x63 && cmd <= 0x6E) { g_lt.pos[(cmd - 0x63) / 3][(cmd - 0x63) % 3] = f24(arg); return 1; }
    if (cmd >= 0x6F && cmd <= 0x7A) { g_lt.dir[(cmd - 0x6F) / 3][(cmd - 0x6F) % 3] = f24(arg); return 1; }
    if (cmd >= 0x7B && cmd <= 0x86) { g_lt.att[(cmd - 0x7B) / 3][(cmd - 0x7B) % 3] = f24(arg); return 1; }
    if (cmd >= 0x87 && cmd <= 0x8A) { g_lt.spot_exp[cmd - 0x87] = f24(arg); return 1; }
    if (cmd >= 0x8B && cmd <= 0x8E) { g_lt.spot_cut[cmd - 0x8B] = f24(arg); return 1; }
    if (cmd >= 0x8F && cmd <= 0x9A) { g_lt.col[(cmd - 0x8F) / 3][(cmd - 0x8F) % 3] = arg; return 1; }
    switch (cmd) {
    case GE_LTE:   g_lt.enable = arg & 1; return 1;
    case GE_RNORM: g_lt.rnorm = arg & 1; return 1;
    case GE_CMAT:  g_lt.cmat = (int)(arg & 7); return 1;
    case GE_EMC:   g_lt.emissive = arg; return 1;
    case GE_DMC:   g_lt.diffuse = arg; return 1;
    case GE_SMC:   g_lt.specular = arg; return 1;
    case GE_SPOW:  g_lt.spow = f24(arg); return 1;
    case GE_ALC:   g_lt.ambient = (g_lt.ambient & 0xFF000000u) | arg; return 1;
    case GE_ALA:   g_lt.ambient = (g_lt.ambient & 0x00FFFFFFu) | ((arg & 0xFF) << 24); return 1;
    default:       return 0;
    }
}

/* Tracked state, and the counters that make the report worth reading. */
static struct {
    uint32_t fbp, fbw, vtype, vaddr, iaddr;
    uint64_t commands;
    uint64_t prims[8];
    uint64_t vertices;
    uint64_t unknown;
    uint64_t lists;
    uint64_t finishes;
    uint64_t op_count[256];     /* undecoded commands, by opcode */
    uint32_t op_last[256];      /* and the last argument each one carried */
} g_ge;

void psp_ge_reset(void) {
    memset(g_queue, 0, sizeof g_queue);
    memset(&g_ge, 0, sizeof g_ge);
    memset(&g_ds, 0, sizeof g_ds);
    memset(g_clut, 0, sizeof g_clut);
    g_tbp = g_tbw = g_cbp = 0;
    g_psm = 3;
    g_material = 0xFFFFFFFFu;
    g_ds.clut = g_clut;
    g_ds.clut_mask = 0xFF;
    g_ds.atest_func = 1;
    g_ds.atest_mask = 0xFF;
    g_ds.tex_w = g_ds.tex_h = 1;
    g_ds.tex_alpha = 1;
    g_ds.sc_x1 = 479;
    g_ds.sc_y1 = 271;
    g_ds.zwrite = 1;
    memset(&g_lt, 0, sizeof g_lt);
    g_zbp = g_zbw = 0;
    xf_reset();
    psp_render_reset_pixels();
    g_unsupported = 0;
    g_next_id = 0x00080000u;
}

void psp_ge_init(void) { psp_ge_reset(); }

void psp_ge_dump_stats(FILE *out) {
    fprintf(out, "GE: %llu lists, %llu commands, %llu finishes\n",
            (unsigned long long)g_ge.lists,
            (unsigned long long)g_ge.commands,
            (unsigned long long)g_ge.finishes);
    fprintf(out, "    framebuffer 0x%08X stride %u, vertex type 0x%06X\n",
            g_ge.fbp, g_ge.fbw, g_ge.vtype);
    fprintf(out, "    vertices submitted: %llu\n", (unsigned long long)g_ge.vertices);
    for (int i = 0; i < 8; i++)
        if (g_ge.prims[i])
            fprintf(out, "    %-15s %llu\n", PRIM_NAME[i], (unsigned long long)g_ge.prims[i]);
    if (g_ge.unknown) {
        fprintf(out, "    %llu commands not individually decoded:\n",
                (unsigned long long)g_ge.unknown);
        for (int i = 0; i < 256; i++)
            if (g_ge.op_count[i])
                fprintf(out, "      0x%02X x%-8llu last arg 0x%06X\n", i,
                        (unsigned long long)g_ge.op_count[i], g_ge.op_last[i]);
    }
    fprintf(out, "    pixels written: %llu\n", (unsigned long long)psp_render_pixels());
    if (g_xf.behind)
        fprintf(out, "    %llu vertices behind the eye (their triangles near-clipped)\n",
                (unsigned long long)g_xf.behind);
    if (psp_render_unsupported_texels())
        fprintf(out, "    %llu texels in an undecoded format (DXT), drawn magenta\n",
                (unsigned long long)psp_render_unsupported_texels());
    if (g_unsupported)
        fprintf(out, "    %llu vertices in an unsupported format (skinned, or no position)\n",
                (unsigned long long)g_unsupported);
}

uint64_t psp_ge_command_count(void) { return g_ge.commands; }
uint64_t psp_ge_vertex_count(void)  { return g_ge.vertices; }

/* ---- rasterizer ----------------------------------------------------------
 *
 * Enough of the GE to put pixels in the framebuffer. Deliberately narrow:
 *
 *  - "through" mode (VTYPE bit 23), where vertex coordinates are already in
 *    screen space and texture coordinates are texels, and transformed mode,
 *    where positions go through world, view and projection and the viewport,
 *    and texture coordinates are normalised and scaled by UV scale/offset.
 *    Transformed vertices are lit (four lights, per vertex) and get a depth.
 *    Skinning, morphing, the texture matrix and near-plane clipping are not
 *    done.
 *  - Position, normal and texture coordinates as 8- or 16-bit or float;
 *    colour as 8888, the 16-bit formats, or none; 8- or 16-bit indices.
 *  - Textures, CLUTs, the texture function, alpha test, depth test, blending,
 *    clear mode and scissor are decoded into psp_draw_state for the backend.
 *
 * The point is to close the loop from display list to visible pixels so the
 * rest can be measured against something. Everything omitted is omitted
 * loudly: unsupported vertex formats are counted, not guessed at.
 */

/* VTYPE field extraction. */
#define VT_TEX(v)     ((v) & 3)
#define VT_COLOR(v)   (((v) >> 2) & 7)
#define VT_NORMAL(v)  (((v) >> 5) & 3)
#define VT_POS(v)     (((v) >> 7) & 3)
#define VT_WEIGHT(v)  (((v) >> 9) & 3)
#define VT_INDEX(v)   (((v) >> 11) & 3)
#define VT_THROUGH(v) (((v) >> 23) & 1)


uint64_t psp_ge_pixels(void) { return psp_render_pixels(); }

/* Size of one vertex in bytes, and the offsets within it. Components appear in
 * a fixed order (weights, texture, colour, normal, position) and each is
 * aligned to its own size, which is what makes the stride awkward enough to be
 * worth computing rather than assuming. */
static int vertex_layout(uint32_t vtype, int *tex_off, int *col_off, int *nrm_off, int *pos_off) {
    static const int tex_sz[4]   = { 0, 1, 2, 4 };
    static const int col_sz[8]   = { 0, 0, 0, 0, 2, 2, 2, 4 };
    static const int norm_sz[4]  = { 0, 1, 2, 4 };
    static const int pos_sz[4]   = { 0, 1, 2, 4 };

    int off = 0, align = 1;
    int t = tex_sz[VT_TEX(vtype)] * 2;
    int c = col_sz[VT_COLOR(vtype)];
    int n = norm_sz[VT_NORMAL(vtype)] * 3;
    int p = pos_sz[VT_POS(vtype)] * 3;

    /* Skinning weights come first: count = bits 14..16 + 1, each 8/16-bit or float. */
    int ws = norm_sz[VT_WEIGHT(vtype)];
    if (ws) { off = ws * (int)(((vtype >> 14) & 7) + 1); align = ws; }

    int ts = tex_sz[VT_TEX(vtype)];
    if (ts) { off = (off + ts - 1) & ~(ts - 1); *tex_off = off; off += t; if (ts > align) align = ts; }
    else *tex_off = -1;
    int cs = col_sz[VT_COLOR(vtype)];
    if (cs) { off = (off + cs - 1) & ~(cs - 1); *col_off = off; off += c; if (cs > align) align = cs; }
    else *col_off = -1;
    int ns = norm_sz[VT_NORMAL(vtype)];
    if (ns) { off = (off + ns - 1) & ~(ns - 1); *nrm_off = off; off += n; if (ns > align) align = ns; }
    else *nrm_off = -1;
    int ps = pos_sz[VT_POS(vtype)];
    if (!ps) return 0;                        /* no position: nothing to draw */
    off = (off + ps - 1) & ~(ps - 1); *pos_off = off; off += p;
    if (ps > align) align = ps;

    return (off + align - 1) & ~(align - 1);  /* stride */
}

/* Three components of 8 or 16 bits or float, as fixed point unless `raw`. */
/* Vertex bytes are read through one host pointer per vertex (P8/P16/PF):
 * translating the guest address per component was a top profile entry. */
static __forceinline uint8_t  P8(const uint8_t *p)  { return *p; }
static __forceinline uint16_t P16(const uint8_t *p) { uint16_t x; memcpy(&x, p, 2); return x; }
static __forceinline uint32_t P32(const uint8_t *p) { uint32_t x; memcpy(&x, p, 4); return x; }
static __forceinline float    PF(const uint8_t *p)  { float x; memcpy(&x, p, 4); return x; }
static __forceinline void read3(const uint8_t *a, int fmt, int raw, float v[3]) {
    switch (fmt) {
    case 1: { const float k = raw ? 1.0f : 1.0f / 128.0f;
        for (int i = 0; i < 3; i++) v[i] = (float)(int8_t)a[i] * k; break; }
    case 2: { const float k = raw ? 1.0f : 1.0f / 32768.0f;
        for (int i = 0; i < 3; i++) v[i] = (float)(int16_t)P16(a + i * 2) * k; break; }
    default: for (int i = 0; i < 3; i++) v[i] = PF(a + i * 4); break;
    }
}

static int g_bone_lo = 999, g_bone_hi = 0;
static int read_vertex(uint32_t addr, uint32_t vtype, int tex_off, int col_off,
                       int nrm_off, int pos_off, int stride, psp_vertex *out) {
    static const uint8_t zero[256];     /* unmapped: every read is 0, as psp_read* gives */
    const uint8_t *A = (const uint8_t *)psp_mem_ptr(addr, (uint32_t)stride);
    if (!A) A = zero;
    float pos[3];
    /* Through mode reads 8/16-bit values as integers; transformed mode reads
     * them as fixed point, signed for positions. */
    const int through = (int)VT_THROUGH(vtype);
    out->rgba = g_material;
    if (col_off >= 0) {
        if (VT_COLOR(vtype) == 7)
            out->rgba = P32(A + col_off);
        else    /* 4, 5, 6: 565, 5551, 4444 -- the texture formats, numbered from 4 */
            out->rgba = psp_color_expand(P16(A + col_off),
                                         (int)VT_COLOR(vtype) - 4);
    }

    out->u = out->v = 0; out->q = 1.0f;
    float tu = 0, tv = 0;   /* normalised, before UV scale: a projection source */
    if (tex_off >= 0) {
        const uint8_t *t = A + tex_off;
        switch (VT_TEX(vtype)) {
        case 1: out->u = P8(t);  out->v = P8(t + 1);
                if (!through) { out->u /= 128.0f; out->v /= 128.0f; }
                break;
        case 2: out->u = P16(t); out->v = P16(t + 2);
                if (!through) { out->u /= 32768.0f; out->v /= 32768.0f; }
                break;
        default: out->u = PF(t); out->v = PF(t + 4); break;
        }
        tu = out->u; tv = out->v;
        if (!through) {     /* normalised, scaled, then in texels */
            out->u = (out->u * g_xf.uscale + g_xf.uoff) * (float)g_ds.tex_w;
            out->v = (out->v * g_xf.vscale + g_xf.voff) * (float)g_ds.tex_h;
        }
    }

    if (!VT_POS(vtype)) return 0;
    read3(A + pos_off, (int)VT_POS(vtype), through, pos);
    float skn[3] = { 0, 0, 1 };
    int skinned = 0;
    if (VT_WEIGHT(vtype) && !through) {
        /* Blend the position (and normal) through the bone matrices. */
        int nw = (int)((vtype >> 14) & 7) + 1, wf = (int)VT_WEIGHT(vtype);
        float nin[3] = { 0, 0, 1 }, P[3] = { 0, 0, 0 }, N[3] = { 0, 0, 0 };
        if (nrm_off >= 0) read3(A + nrm_off, (int)VT_NORMAL(vtype), 0, nin);
        for (int i = 0; i < nw; i++) {
            float w;
            if (wf == 1)      w = P8(A + i) / 128.0f;
            else if (wf == 2) w = P16(A + i * 2) / 32768.0f;
            else              w = PF(A + i * 4);
            if (w == 0.0f) continue;
            const float *B = &g_xf.bone[i * 12];
            for (int r = 0; r < 3; r++) {
                P[r] += w * (B[r] * pos[0] + B[3 + r] * pos[1] + B[6 + r] * pos[2] + B[9 + r]);
                N[r] += w * (B[r] * nin[0] + B[3 + r] * nin[1] + B[6 + r] * nin[2]);
            }
        }
        memcpy(pos, P, sizeof P); memcpy(skn, N, sizeof N);
        skinned = 1;
    }
    out->behind = 0;
    out->fog = 0;
    out->w = 1.0f;
    if (through) {
        /* Through mode's depth is an unsigned 16-bit value. */
        out->x = (int)pos[0];
        out->y = (int)pos[1];
        out->z = VT_POS(vtype) == 2 ? (float)P16(A + pos_off + 4) : pos[2];
        out->has_f = 0;
        return 1;
    }
    if (g_lt.enable) {
        float n[3] = { 0, 0, 1 };
        if (skinned) memcpy(n, skn, sizeof n);
        else if (nrm_off >= 0 && g_lp.n) read3(A + nrm_off, (int)VT_NORMAL(vtype), 0, n);
        out->rgba = light_vertex(pos, n, out->rgba);
    }
    if (g_tmap_mode == 1 || g_tmap_mode == 2) {
        /* TMAP: texture coordinates generated, not read. 1 = a source vector
         * through the texture matrix (projected: the backend divides by q per
         * pixel), 2 = environment map from two lights' directions. */
        float n[3] = { 0, 0, 1 };
        if (skinned) memcpy(n, skn, sizeof n);
        else if (nrm_off >= 0) read3(A + nrm_off, (int)VT_NORMAL(vtype), 0, n);
        if (g_tmap_mode == 1) {
            float s[3];
            switch (g_tmap_src) {
            case 0:  memcpy(s, pos, sizeof s); break;
            case 1:  s[0] = tu; s[1] = tv; s[2] = 0; break;
            case 2:  memcpy(s, n, sizeof s); norm3(s); break;
            default: memcpy(s, n, sizeof s); break;
            }
            const float *T = g_tmat;
            const float ps = T[0] * s[0] + T[3] * s[1] + T[6] * s[2] + T[9];
            const float pt = T[1] * s[0] + T[4] * s[1] + T[7] * s[2] + T[10];
            const float pq = T[2] * s[0] + T[5] * s[1] + T[8] * s[2] + T[11];
            out->u = ps * (float)g_ds.tex_w; out->v = pt * (float)g_ds.tex_h; out->q = pq;
        } else {
            const float *W = g_xf.world;
            float wn[3], L0[3], L1[3];
            for (int r = 0; r < 3; r++) wn[r] = W[r] * n[0] + W[3 + r] * n[1] + W[6 + r] * n[2];
            norm3(wn);
            for (int r = 0; r < 3; r++) { L0[r] = g_lt.pos[g_tshade_u][r]; L1[r] = g_lt.pos[g_tshade_v][r]; }
            norm3(L0); norm3(L1);
            const float du = wn[0] * L0[0] + wn[1] * L0[1] + wn[2] * L0[2];
            const float dv = wn[0] * L1[0] + wn[1] * L1[1] + wn[2] * L1[2];
            out->u = (1.0f + du) * 0.5f * g_xf.uscale * (float)g_ds.tex_w;
            out->v = (1.0f + dv) * 0.5f * g_xf.vscale * (float)g_ds.tex_h;
        }
    }
    if (g_ds.fog) out->fog = 1.0f - (g_vwz[0] * pos[0] + g_vwz[1] * pos[1] + g_vwz[2] * pos[2] + g_vwz[3] + g_fog_end) * g_fog_scale;
    float sx, sy, sz;
    if (!transform(pos, &sx, &sy, &sz)) { out->behind = 1; g_xf.behind++; return 1; }
    /* A NaN/inf position (bad matrix or vertex data) can't be rasterized
     * meaningfully; the software backend dropped it by accident, a GPU draws
     * garbage. Drop it on purpose for both. */
    if (!isfinite(sx) || !isfinite(sy) || !isfinite(sz) || fabsf(sx) > 1e6f || fabsf(sy) > 1e6f) {
        out->behind = 1; g_xf.behind++; return 1;
    }
    out->x = (int)ffl(sx);
    out->y = (int)ffl(sy);
    out->z = sz;
    out->w = g_lastclip[3];
    /* Sub-pixel position on the GE's 1/16-pixel grid, the same grid clipped
     * vertices use: a whole-pixel corner next to a clipped neighbour's exact
     * one left a crack along their shared edge (seams near the camera). */
    out->has_f = 1; out->fx = ffl(sx * 16.0f) / 16.0f; out->fy = ffl(sy * 16.0f) / 16.0f;
    return 1;
}


/* PSPRECOMP_TRACE_GE=<frame> logs every primitive drawn during that vblank:
 * what it is, its vertex format, and the texture state it is drawn with. */
static int trace_frame(void) {
    static long f = -2;
    if (f == -2) { const char *e = getenv("PSPRECOMP_TRACE_GE"); f = e ? atol(e) : -1; }
    if (f >= 0 && (long)psp_display_vblanks() == f) return 1;
    /* Or on demand: create trace.cmd in the working directory to trace the
     * next vblank (the file is removed once that frame is done). */
    static long tf = -1, chk = 0;
    long v = (long)psp_display_vblanks();
    if (tf >= 0 && v > tf) { remove("trace.cmd"); tf = -1; }
    static unsigned seen_tr; extern int psp_cmd_changed(unsigned *);
    if (tf < 0 && v != chk && ((chk = v), 1) && psp_cmd_changed(&seen_tr)) { FILE *fp = fopen("trace.cmd", "r"); if (fp) { fclose(fp); tf = v; } }
    return tf == v;
}

static uint32_t fb_vram(void);
/* ---- near-plane clipping ------------------------------------------------
 *
 * A triangle with a corner behind the eye used to be dropped whole, which
 * erased most ground and nearby geometry. Hardware clips it against the near
 * plane instead: here each such triangle is cut against w = NEAR_W in clip
 * space (Sutherland-Hodgman, one plane), the pieces projected, and the result
 * drawn as a triangle list. */
#define NEAR_W 1e-4f

static psp_vertex lerp_vtx(const psp_vertex *a, const psp_vertex *b, const float *ca,
                           const float *cb, float t, float *co) {
    psp_vertex r = *a;
    /* In double, and w pinned to the plane: with w in the thousands a float
     * lerp lands on 0 or just below it, and such a vertex projects to infinity
     * or flips across the screen -- the horizon mesh then filled the view. */
    for (int i = 0; i < 3; i++) co[i] = (float)((double)ca[i] + ((double)cb[i] - (double)ca[i]) * (double)t);
    co[3] = NEAR_W;
    r.u = a->u + (b->u - a->u) * t;
    r.v = a->v + (b->v - a->v) * t;
    r.fog = a->fog + (b->fog - a->fog) * t;
    r.q = a->q + (b->q - a->q) * t;
    uint32_t c = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a->rgba >> s) & 0xFF), y = (float)((b->rgba >> s) & 0xFF);
        c |= (uint32_t)(int)(x + (y - x) * t + 0.5f) << s;
    }
    r.rgba = c;
    return r;
}

static void project_vtx(psp_vertex *o, const float *c) {
    o->x = (int)floorf(c[0] / c[3] * g_xf.xscale + g_xf.xpos - g_xf.offx);
    o->y = (int)floorf(c[1] / c[3] * g_xf.yscale + g_xf.ypos - g_xf.offy);
    o->z = c[2] / c[3] * g_xf.zscale + g_xf.zpos;
    o->w = c[3];
    o->behind = 0;
    o->has_f = 1;
    o->fx = floorf((c[0] / c[3] * g_xf.xscale + g_xf.xpos - g_xf.offx) * 16.0f) / 16.0f;
    o->fy = floorf((c[1] / c[3] * g_xf.yscale + g_xf.ypos - g_xf.offy) * 16.0f) / 16.0f;
}

static void draw_clipped(int type, const psp_vertex *v, float (*clip)[4], uint32_t n) {
    static psp_vertex out[65536 * 3];
    uint32_t m = 0;
    for (uint32_t t = 0; t + 2 < n || (type == 3 && t + 2 < n); ) {
        uint32_t i0, i1, i2;
        if (type == 3)      { i0 = t; i1 = t + 1; i2 = t + 2; t += 3; }
        else if (type == 4) { i0 = t; i1 = t + 1; i2 = t + 2; if (t & 1) { i0 = t + 1; i1 = t; } t += 1; }
        else                { i0 = 0; i1 = t + 1; i2 = t + 2; t += 1; }
        if (type == 5 && i2 >= n) break;
        const uint32_t idx[3] = { i0, i1, i2 };
        /* The GE's drawing space is 4096x4096 (12.4 fixed point). A triangle
         * with a vertex in front of the camera that lands outside it is culled
         * whole -- that is what drops the sky dome's stitch triangles that pass
         * right by the camera (clipped, they became slivers covering the
         * screen). Triangles merely crossing the near plane are clipped, so the
         * sky no longer loses pieces as the camera turns.
         * PSPRECOMP_NEARMODE: 0 = clip only, 1 = cull anything crossing near,
         * 2 (default) = guard band. */
        { static int nm = -1; if (nm < 0) { const char *e = getenv("PSPRECOMP_NEARMODE"); nm = e ? atoi(e) : 0; }
          if (nm == 1 && (clip[i0][3] < NEAR_W || clip[i1][3] < NEAR_W || clip[i2][3] < NEAR_W)) continue;
          if (nm == 2) {
              int out_gb = 0;
              for (int e = 0; e < 3 && !out_gb; e++) {
                  const float *c = clip[idx[e]];
                  if (c[3] < NEAR_W) continue;
                  float dx = c[0] / c[3] * g_xf.xscale + g_xf.xpos, dy = c[1] / c[3] * g_xf.yscale + g_xf.ypos;
                  if (!(dx >= 0 && dx < 4096 && dy >= 0 && dy < 4096)) out_gb = 1;
              }
              if (out_gb) continue;
          } }
        psp_vertex pv[4]; float pc[4][4]; int pn = 0;
        for (int e = 0; e < 3; e++) {
            uint32_t a = idx[e], b = idx[(e + 1) % 3];
            float wa = clip[a][3] - NEAR_W, wb = clip[b][3] - NEAR_W;
            if (wa >= 0) { pv[pn] = v[a]; memcpy(pc[pn], clip[a], sizeof pc[0]); pn++; }
            if ((wa >= 0) != (wb >= 0) && pn < 4) {
                /* Always from the vertex in front to the one behind: the triangle
                 * on the other side of this edge walks it the other way, and
                 * lerping a->b vs b->a put the cut points apart (seams wobbling
                 * along shared ground edges near the camera). */
                const uint32_t i = wa >= 0 ? a : b, o = wa >= 0 ? b : a;
                const double wi = wa >= 0 ? wa : wb, wo = wa >= 0 ? wb : wa;
                const float f = (float)(wi / (wi - wo));
                pv[pn] = lerp_vtx(&v[i], &v[o], clip[i], clip[o], f, pc[pn]); pn++;
            }
        }
        if (pn < 3) continue;
        for (int k = 0; k < pn; k++) project_vtx(&pv[k], pc[k]);
        for (int k = 1; k + 1 < pn && m + 3 <= sizeof out / sizeof out[0]; k++) {
            out[m++] = pv[0]; out[m++] = pv[k]; out[m++] = pv[k + 1];
        }
    }
    if (m) psp_render_current()->draw(3, out, (int)m, &g_ds);
}

static void draw_prim(uint32_t type, uint32_t count) {
    if (trace_frame())
        fprintf(stderr, "GE target %08X fbw %u psm %d\n", fb_vram(), g_ge.fbw, (int)g_psm),
        fprintf(stderr, "GE prim %u x%u vtype %06X vaddr %08X tex %d fmt %d %dx%d bw %d at %08X sw %d func %d clut fmt %d blend %d atest %d clear %d z %d/%d/%d\n",
                type, count, g_ge.vtype, g_ge.vaddr, g_ds.tex_enable, g_ds.tex_fmt, g_ds.tex_w, g_ds.tex_h,
                g_ds.tex_bw, g_ds.tex_addr, g_ds.tex_swizzle, g_ds.tex_func, g_ds.clut_fmt, g_ds.blend,
                g_ds.atest, g_ds.clear, g_ds.ztest, g_ds.zfunc, g_ds.zwrite);
    { static int skip = -1; if (skip < 0) skip = getenv("PSPRECOMP_SKIP_FBTEX") != NULL;   /* debug: drop passes that sample the screen buffers */
      if (skip && g_ds.tex_enable && g_ds.tex_addr >= 0x04000000u && g_ds.tex_addr < 0x04170000u) return; }
    if (!g_ge.vaddr) { g_unsupported += count; return; }

    int tex_off = -1, col_off = -1, nrm_off = -1, pos_off = 0;
    int stride = vertex_layout(g_ge.vtype, &tex_off, &col_off, &nrm_off, &pos_off);
    { static long vn = -2; if (vn == -2) { const char *e = getenv("PSPRECOMP_VT_COUNT"); vn = e ? atol(e) : 0; }   /* debug: vtype of draws with this vertex count */
      static int vl; if (vn && (long)count == vn && vl < 20) { vl++; fprintf(stderr, "vt: n %u vtype %08X stride %d pos %d vaddr %08X\n", count, g_ge.vtype, stride, pos_off, g_ge.vaddr); } }
    if (!stride) { g_unsupported += count; return; }
    { /* skip.cmd in the working dir: one hex vertex address; draws from it are skipped (debug) */
        static uint32_t skip; static long chk = -1;
        long v = (long)psp_display_vblanks();
        static unsigned seen_sk; extern int psp_cmd_changed(unsigned *);
        if (v / 30 != chk && ((chk = v / 30), 1) && psp_cmd_changed(&seen_sk)) { skip = 0; FILE *fp = fopen("skip.cmd", "r"); if (fp) { if (fscanf(fp, "%x", &skip) != 1) skip = 0; fclose(fp); } }
        if (skip && g_ge.vaddr == skip) return;
        { static uint32_t ka; static int ks = -1; static long kc = -1;   /* bonekill.cmd: "<vaddr> <slot>" */
          static unsigned seen_bk;
          if (v / 30 != kc && ((kc = v / 30), 1) && psp_cmd_changed(&seen_bk)) { ka = 0; ks = -1; FILE *fp = fopen("bonekill.cmd", "r"); if (fp) { if (fscanf(fp, "%x %d", &ka, &ks) != 2) ka = 0; fclose(fp); } }
          if (ka && g_ge.vaddr == ka && ks > 0 && ks < 8) memcpy(&g_xf.bone[ks * 12], &g_xf.bone[0], 12 * sizeof(float)); }
    }
    { /* PSPRECOMP_BONE_WATCH=<hex vaddr>: log that draw's bone matrices every frame */
        static uint32_t bw = 1; if (bw == 1) { const char *e = getenv("PSPRECOMP_BONE_WATCH"); bw = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (bw && g_ge.vaddr == bw) {
            fprintf(stderr, "bw %ld", (long)psp_display_vblanks());
            for (int k = 0; k < 48; k++) fprintf(stderr, " %.4f", (double)g_xf.bone[k]);
            fprintf(stderr, "\n");
        }
    }
    if (VT_WEIGHT(g_ge.vtype)) {      /* PSPRECOMP_SKIN_LOG=<n>: describe the first n skinned draws */
        static long sl = -2;
        if (sl == -2) { const char *e = getenv("PSPRECOMP_SKIN_LOG"); sl = e ? atol(e) : 0; }
        { static long sk = -2; if (sk == -2) { const char *e = getenv("PSPRECOMP_SKIN_USE"); sk = e ? atol(e) : 0; }
          if (sk > 0) { sk--;
            int nw = (int)((g_ge.vtype >> 14) & 7) + 1, used = 0;
            for (uint32_t i = 0; i < count; i++) { uint32_t a = g_ge.vaddr + i * (uint32_t)stride;
                for (int k = 0; k < nw; k++) { int nz = VT_WEIGHT(g_ge.vtype) == 1 ? psp_read8(a + k) : VT_WEIGHT(g_ge.vtype) == 2 ? psp_read16(a + 2 * k) : psp_read32(a + 4 * k) != 0; if (nz) used |= 1 << k; } }
            fprintf(stderr, "skinuse: vtype %06X vaddr %08X n %u nw %d used %02X uploaded %d-%d\n", g_ge.vtype, g_ge.vaddr, count, nw, used, g_bone_lo, g_bone_hi);
          } }
        g_bone_lo = 999; g_bone_hi = 0;
        if (sl > 0) {
            sl--;
            fprintf(stderr, "skin: vtype %08X nw %u wf %u stride %d tex %d col %d nrm %d pos %d count %u prim %u bi %d\n",
                    g_ge.vtype, ((g_ge.vtype >> 14) & 7) + 1, VT_WEIGHT(g_ge.vtype), stride, tex_off, col_off,
                    nrm_off, pos_off, count, type, g_xf.bi);
            for (int b = 0; b < 8; b++) {
                fprintf(stderr, "  bone%d:", b);
                for (int k = 0; k < 12; k++) fprintf(stderr, " %.3f", (double)g_xf.bone[b * 12 + k]);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "  world:");
            for (int k = 0; k < 12; k++) fprintf(stderr, " %.3f", (double)g_xf.world[k]);
            fprintf(stderr, "\n");
            for (uint32_t i = 0; i < 3 && i < count; i++) {
                uint32_t a = g_ge.vaddr + i * (uint32_t)stride;
                fprintf(stderr, "  v%u raw:", i);
                for (int k = 0; k < stride && k < 32; k++) fprintf(stderr, " %02X", psp_read8(a + (uint32_t)k));
                fprintf(stderr, "\n");
            }
        }
    }

    /* Indexed: the vertices are vaddr[index[i]], with 8- or 16-bit indices.
     * Format decoding stays here rather than in each backend: the stride
     * arithmetic and component alignment are fiddly, and duplicating them per
     * backend means every backend is wrong in its own way. */
    static const int ISZ[4] = { 0, 1, 2, 4 };
    const int isz = ISZ[VT_INDEX(g_ge.vtype)];
    if (isz && !g_ge.iaddr) { g_unsupported += count; return; }

    static psp_vertex v[65536];
    static float clip[65536][4];
    static uint8_t oob[65536];
    uint32_t n = 0;
    if (g_lt.enable) light_prep();
    mvp_prep();
    /* Indexed meshes name the same vertex several times; each is decoded,
     * lit and transformed once per primitive (cache tagged by a generation). */
    static psp_vertex cv[65536]; static float cclip[65536][4]; static uint8_t coob[65536]; static uint32_t cgen[65536], gen;
    if (isz && ++gen == 0) { memset(cgen, 0, sizeof cgen); gen = 1; }
    const uint8_t *ip = isz ? (const uint8_t *)psp_mem_ptr(g_ge.iaddr, count * (uint32_t)isz) : NULL;
    for (; n < count; n++) {
        uint32_t idx = n;
        if (ip) idx = isz == 1 ? ip[n] : isz == 2 ? P16(ip + n * 2) : P32(ip + n * 4);
        else if (isz == 1) idx = psp_read8(g_ge.iaddr + n);
        else if (isz == 2) idx = psp_read16(g_ge.iaddr + n * 2);
        else if (isz == 4) idx = psp_read32(g_ge.iaddr + n * 4);
        const int cache = isz && idx < 65536;
        if (cache && cgen[idx] == gen) { v[n] = cv[idx]; memcpy(clip[n], cclip[idx], sizeof clip[n]); oob[n] = coob[idx]; continue; }
        if (!read_vertex(g_ge.vaddr + idx * (uint32_t)stride, g_ge.vtype,
                         tex_off, col_off, nrm_off, pos_off, stride, &v[n])) break;
        memcpy(clip[n], g_lastclip, sizeof clip[n]);
        /* outside the GE's 4096x4096 drawing space (hardware culls these),
         * from the projection transform already made */
        oob[n] = !v[n].behind && v[n].has_f && g_lastclip[3] >= NEAR_W &&
                 !(g_lastdx >= 0 && g_lastdx < 4096 && g_lastdy >= 0 && g_lastdy < 4096);
        if (cache) { cv[idx] = v[n]; memcpy(cclip[idx], g_lastclip, sizeof cclip[idx]); coob[idx] = oob[n]; cgen[idx] = gen; }
    }
    /* The GE leaves its read pointer after what a primitive consumed, so the
     * next PRIM continues from there without a new VADDR (or IADDR). */
    if (isz) g_ge.iaddr += count * (uint32_t)isz;
    else     g_ge.vaddr += count * (uint32_t)stride;

    if (trace_frame()) {
        fprintf(stderr, "  mat %08X cbp %08X lt %d tex0 %08X\n", g_material, g_cbp, g_lt.enable,
                psp_read32(g_ds.tex_addr));
        fprintf(stderr, "  W:"); for (int k = 0; k < 12; k++) fprintf(stderr, " %.3f", (double)g_xf.world[k]);
        fprintf(stderr, "\n  V:"); for (int k = 0; k < 12; k++) fprintf(stderr, " %.3f", (double)g_xf.view[k]);
        fprintf(stderr, "\n  P:"); for (int k = 0; k < 16; k++) fprintf(stderr, " %.3f", (double)g_xf.proj[k]);
        if (n) fprintf(stderr, "\n  clip0: %g %g %g %g", (double)clip[0][0], (double)clip[0][1], (double)clip[0][2], (double)clip[0][3]);
        fprintf(stderr, "\n");
        if (pos_off >= 0 && ((g_ge.vtype >> 7) & 3) == 3)
            for (uint32_t k = 0; k < 3; k++) {
                uint32_t i0 = isz == 2 ? psp_read16(g_ge.iaddr - count * 2 + k * 2) : k;
                uint32_t a = g_ge.vaddr + i0 * (uint32_t)stride + (uint32_t)pos_off;
                fprintf(stderr, "  raw%u idx %u: %g %g %g\n", k, i0, (double)u2f(psp_read32(a)),
                        (double)u2f(psp_read32(a + 4)), (double)u2f(psp_read32(a + 8)));
            }
        for (uint32_t k = 0; k < n && k < 16; k++)
            fprintf(stderr, "  v%u (%d,%d,%.0f) rgba %08X uv %.1f %.1f texel %08X\n", k, v[k].x, v[k].y,
                    (double)v[k].z, v[k].rgba, v[k].u, v[k].v, psp_tex_fetch(&g_ds, (int)v[k].u, (int)v[k].v));
    }
    if (n && !VT_THROUGH(g_ge.vtype) && type >= 3 && type <= 5) {
        int any = 0;
        for (uint32_t k = 0; k < n && !any; k++) {
            any |= v[k].behind | oob[k];
        }
        if (any) { draw_clipped((int)type, v, clip, n); return; }
    }
    if (n) psp_render_current()->draw((int)type, v, (int)n, &g_ds);
}

/* The framebuffer is always in eDRAM. Games send FBP relative to it, so a high
 * byte of 0 is the norm, and an absolute 0x04xxxxxx folds to the same place.
 * Taken literally, 0x88000 is main RAM: for a vaddr-0 module, its own code. */
static uint32_t fb_vram(void) { return PSP_VRAM_BASE | (g_ge.fbp & (PSP_VRAM_SIZE - 1)); }

/* Walk a list until END/FINISH, the stall address, or a step budget.
 *
 * The budget is not paranoia: a list whose JUMP forms a cycle is a normal
 * intermediate state while the CPU is still writing, and without a bound a
 * malformed or partially-written list hangs the host with no diagnostic. */
static void run_list(ge_queue *q) {
    uint32_t stack[GE_STACK];
    int sp = 0;
    uint64_t budget = 1u << 22;

    g_ge.lists++;

    while (budget--) {
        if (q->stall && q->list == q->stall) break;   /* caught up to the CPU */

        uint32_t word = psp_read32(q->list);
        uint32_t cmd  = word >> 24;
        uint32_t arg  = word & 0x00FFFFFF;
        q->list += 4;
        g_ge.commands++;
        if (trace_frame()) fprintf(stderr, "GE @%08X %02X %06X\n", q->list - 4, cmd, arg);

        switch (cmd) {
        case GE_NOP:
            break;

        case GE_PRIM: {
            uint32_t type  = (arg >> 16) & 7;
            uint32_t count = arg & 0xFFFF;
            g_ge.prims[type]++;
            g_ge.vertices += count;
            draw_prim(type, count);
            break;
        }
        case GE_BEZIER:
        case GE_SPLINE:
            /* Patches expand to triangles on hardware; counted as their own
             * thing rather than folded into the triangle count. */
            g_ge.prims[3]++;
            break;

        case GE_JUMP:
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_CALL:
            if (sp < GE_STACK) stack[sp++] = q->list;
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_RET:
            if (sp > 0) q->list = stack[--sp];
            break;
        case GE_BJUMP:
            /* Conditional on the bounding-box test, which needs geometry we do
             * not process. Not taking it means we walk the enclosed commands
             * rather than skipping them -- the conservative direction, since
             * skipping would under-report what the game drew. */
            break;

        case GE_END:
        case GE_FINISH:
            if (cmd == GE_FINISH) g_ge.finishes++;
            /* A finished list gives its slot back. Keeping it used meant the
             * ninth enqueue failed, and WTF then dropped 3,870 of 3,885
             * frames while running at full speed. */
            q->done = 1;
            q->used = 0;
            return;

        case GE_SIGNAL:
            /* Raises a callback on hardware. Callbacks are not delivered yet
             * (no scheduler), so this is recorded and ignored. */
            break;

        case GE_BASE:        q->base = (arg & 0xFF0000) << 8; break;
        case GE_ORIGIN_ADDR: q->origin = q->list - 4; break;
        case GE_OFFSET_ADDR: q->base = arg << 8; break;

        case GE_VTYPE: g_ge.vtype = arg; break;
        case GE_FBP:
            g_ge.fbp = (g_ge.fbp & 0xFF000000u) | arg;
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;
        case GE_FBW:
            g_ge.fbw = arg & 0xFFFF;
            g_ge.fbp = (g_ge.fbp & 0x00FFFFFFu) | ((arg & 0xFF0000) << 8);
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;

        case GE_PSM:
            g_psm = arg & 3;
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;

        case 0x2A:     g_xf.bi = (int)(arg & 0x7F); break;                  /* BOFS */
        case 0x2B:     if (g_xf.bi < 96) { if (g_bone_lo > g_xf.bi) g_bone_lo = g_xf.bi; g_xf.bone[g_xf.bi++] = f24(arg); if (g_bone_hi < g_xf.bi) g_bone_hi = g_xf.bi; } break;  /* BONE */
        case GE_WMS:   g_xf.wi = (int)(arg & 0xF); break;
        case GE_WORLD: if (g_xf.wi < 12) g_xf.world[g_xf.wi++] = f24(arg); break;
        case GE_VMS:   g_xf.vi = (int)(arg & 0xF); break;
        case GE_VIEW:
            if (!isfinite(f24(arg))) { static int nv; if (nv++ < 3) fprintf(stderr, "ge: NaN VIEW[%d] arg %06X at list 0x%08X\n", g_xf.vi, arg, q->list - 4); }
            if (g_xf.vi < 12) g_xf.view[g_xf.vi++] = f24(arg);
            if (g_xf.vi == 12) { static long lastv = -1; static int en = -1; if (en < 0) en = getenv("PSPRECOMP_VIEW_LOG") != NULL;
                long v = (long)psp_display_vblanks();
                if (en && v != lastv && v % 30 == 0) { lastv = v; fprintf(stderr, "view %ld @%08X", v, q->list - 4); for (int k = 0; k < 12; k++) fprintf(stderr, " %.2f", (double)g_xf.view[k]); fprintf(stderr, "\n"); } }
            break;
        case GE_PMS:   g_xf.pi = (int)(arg & 0xF); break;
        case GE_PROJ:  if (g_xf.pi < 16) g_xf.proj[g_xf.pi++] = f24(arg); break;
        case GE_XSCALE:  g_xf.xscale = f24(arg); break;
        case GE_YSCALE:  g_xf.yscale = f24(arg); break;
        case GE_XPOS:    g_xf.xpos = f24(arg); break;
        case GE_YPOS:    g_xf.ypos = f24(arg); break;
        case GE_USCALE:  g_xf.uscale = f24(arg); break;
        case GE_VSCALE:  g_xf.vscale = f24(arg); break;
        case GE_UOFFSET: g_xf.uoff = f24(arg); break;
        case GE_VOFFSET: g_xf.voff = f24(arg); break;
        case GE_OFFSETX: g_xf.offx = (float)(arg & 0xFFFF) / 16.0f; break;   /* 12.4 */
        case GE_OFFSETY: g_xf.offy = (float)(arg & 0xFFFF) / 16.0f; break;

        case GE_TME:   g_ds.tex_enable = arg & 1; break;
        case GE_ABE:   g_ds.blend = arg & 1; break;
        case GE_ATE:   g_ds.atest = arg & 1; break;
        case GE_SHADE: g_ds.gouraud = arg & 1; break;
        case GE_AMC:   g_material = (g_material & 0xFF000000u) | arg; break;
        case GE_AMA:   g_material = (g_material & 0x00FFFFFFu) | ((arg & 0xFF) << 24); break;
        case GE_CLEAR:
            g_ds.clear = arg & 1;
            g_ds.clear_color = (arg >> 8) & 1;
            g_ds.clear_alpha = (arg >> 9) & 1;
            g_ds.clear_depth = (arg >> 10) & 1;
            break;

        /* Depth. The buffer is 16 bits per pixel in VRAM; ZBW carries the
         * address's high byte like FBW does. */
        case GE_ZTE:    g_ds.ztest = arg & 1; break;
        case GE_BCE:    g_ds.cull = arg & 1; break;
        case GE_CULL:   g_ds.cull_dir = arg & 1; break;
        case GE_ZTST:   g_ds.zfunc = (int)(arg & 7); break;
        case GE_ZMSK:   g_ds.zwrite = !(arg & 1); break;
        case GE_ZSCALE: g_xf.zscale = f24(arg); break;
        case GE_ZPOS:   g_xf.zpos = f24(arg); break;
        case GE_ZBP:
        case GE_ZBW:
            if (cmd == GE_ZBP) g_zbp = arg; else g_zbw = arg;
            g_ds.zaddr = PSP_VRAM_BASE | ((((g_zbw & 0xFF0000u) << 8) | g_zbp) & (PSP_VRAM_SIZE - 1));
            g_ds.zbw = (int)(g_zbw & 0xFFFF);
            break;

        /* TBP0 carries the low 24 bits and TBW0 bits 24-31 in its top byte. */
        case GE_TBP0:
            g_tbp = arg;
            g_ds.tex_addr = ((g_tbw & 0xFF0000u) << 8) | g_tbp;
            break;
        case GE_TBW0:
            g_tbw = arg;
            g_ds.tex_bw = (int)(arg & 0xFFFF);
            g_ds.tex_addr = ((g_tbw & 0xFF0000u) << 8) | g_tbp;
            break;
        case GE_TSIZE0:
            g_ds.tex_w = 1 << (arg & 0xF);
            g_ds.tex_h = 1 << ((arg >> 8) & 0xF);
            break;
        case GE_TMODE: g_ds.tex_swizzle = arg & 1; g_ds.tex_levels = (int)((arg >> 16) & 7); break;
        case GE_TPSM:  g_ds.tex_fmt = (int)(arg & 0xF); break;
        case 0xC6:   /* TFLT: minify filter in bits 0-2 (bit 0 = linear), magnify in bit 8 */
            g_ds.tex_min_linear = arg & 1;
            g_ds.tex_mag_linear = (arg >> 8) & 1;
            g_ds.tex_min_mip = (arg & 7) >= 4 ? 1 + ((arg >> 1) & 1) : 0;
            break;
        case GE_TWRAP:
            g_ds.tex_clamp_u = arg & 1;
            g_ds.tex_clamp_v = (arg >> 8) & 1;
            break;
        case GE_TFUNC:
            g_ds.tex_func = (int)(arg & 7);
            g_ds.tex_alpha = (arg >> 8) & 1;
            g_ds.tex_double = (arg >> 16) & 1;
            break;
        case GE_TEC:   g_ds.tex_env = arg; break;

        /* Mip levels 1-7: each its own address, width and size. */
        case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7: {
            const int l = (int)(cmd - 0xA0); g_tbpn[l] = arg;
            g_ds.mip_addr[l] = ((g_tbwn[l] & 0xFF0000u) << 8) | g_tbpn[l]; break; }
        case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
            const int l = (int)(cmd - 0xA8); g_tbwn[l] = arg; g_ds.mip_bw[l] = (int)(arg & 0xFFFF);
            g_ds.mip_addr[l] = ((g_tbwn[l] & 0xFF0000u) << 8) | g_tbpn[l]; break; }
        case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            const int l = (int)(cmd - 0xB8);
            g_ds.mip_w[l] = 1 << (arg & 0xF); g_ds.mip_h[l] = 1 << ((arg >> 8) & 0xF); break; }
        case 0xD0:     g_ds.tex_slope = f24(arg); break;           /* TSLOPE */
        case 0x28:     g_ds.logic = arg & 1; break;               /* LOE */
        case 0xE6:     g_ds.logic_op = (int)(arg & 0xF); break;   /* LOP */
        case 0x20:     g_ds.dither = arg & 1; break;              /* DTE */
        case 0xE2: case 0xE3: case 0xE4: case 0xE5:              /* DTH0-3: four signed 4-bit entries */
            for (int k = 0; k < 4; k++) { int e = (int)((arg >> (4 * k)) & 0xF); g_ds.dith[(cmd - 0xE2) * 4 + k] = (int8_t)(e >= 8 ? e - 16 : e); } break;
        case 0x40:     g_tmi = (int)(arg & 0xF); break;                          /* TMS */
        case 0x41:     if (g_tmi < 12) g_tmat[g_tmi++] = f24(arg); break;        /* TMATRIX */
        case 0xC0:     g_tmap_mode = (int)(arg & 3); g_tmap_src = (int)((arg >> 8) & 3); break;   /* TMAP */
        case 0xC1:     g_tshade_u = (int)(arg & 3); g_tshade_v = (int)((arg >> 8) & 3); break;  /* TEXSHADE */
        case 0xC8:     /* TEXLEVEL */
            g_ds.tex_lod_mode = (int)(arg & 3);
            g_ds.tex_lod_bias = (int)(int8_t)((arg >> 16) & 0xFF);
            break;
        case 0x1F:     g_ds.fog = arg & 1; break;                  /* FGE */
        case 0xCD:     g_fog_end = f24(arg); break;                /* FOG1 */
        case 0xCE:     g_fog_scale = f24(arg); break;              /* FOG2 */
        case 0xCF:     g_ds.fog_color = arg & 0xFFFFFF; break;     /* FCOL */
        case 0x24:     g_ds.stencil = arg & 1; break;              /* STE */
        case 0xDC:     g_ds.st_func = (int)(arg & 7); g_ds.st_ref = (int)((arg >> 8) & 0xFF);
                       g_ds.st_mask = (int)((arg >> 16) & 0xFF); break;   /* STST */
        case 0xDD:     g_ds.sop_fail = (int)(arg & 7); g_ds.sop_zfail = (int)((arg >> 8) & 7);
                       g_ds.sop_zpass = (int)((arg >> 16) & 7); break;   /* SOP */
        case 0x27:     g_ds.ctest = arg & 1; break;                /* CTE */
        case 0xD8:     g_ds.ct_func = (int)(arg & 3); break;       /* CTST */
        case 0xD9:     g_ds.ct_ref = arg & 0xFFFFFF; break;        /* CREF */
        case 0xDA:     g_ds.ct_mask = arg & 0xFFFFFF; break;       /* CMSK */
        case 0xE8:     g_ds.pmask_rgb = arg & 0xFFFFFF; break;     /* PMSKC */
        case 0xE9:     g_ds.pmask_a = (int)(arg & 0xFF); break;    /* PMSKA */
        case 0xD6:     g_ds.minz = (int)(arg & 0xFFFF); break;     /* MINZ */
        case 0xD7:     g_ds.maxz_inv = 0xFFFF - (int)(arg & 0xFFFF); break;   /* MAXZ */

        /* The CLUT is copied into the GE when CLOAD runs, so a game may
         * rewrite the memory afterwards. CBPH carries bits 24-27. */
        case GE_CBP:   g_cbp = (g_cbp & 0xFF000000u) | arg; break;
        case GE_CBPH:  g_cbp = (g_cbp & 0x00FFFFFFu) | ((arg & 0x0F0000u) << 8); break;
        case GE_CLOAD: {
            uint32_t n = (arg & 0x3F) * 32u;
            if (n > sizeof g_clut) n = sizeof g_clut;
            for (uint32_t i = 0; i < n; i++) g_clut[i] = psp_read8(g_cbp + i);
            { extern uint32_t g_clut_src, g_clut_gen; g_clut_src = g_cbp; g_clut_gen++; }
            break;
        }
        case GE_CMODE:
            g_ds.clut_fmt = (int)(arg & 3);
            g_ds.clut_shift = (int)((arg >> 2) & 0x1F);
            g_ds.clut_mask = (int)((arg >> 8) & 0xFF);
            g_ds.clut_start = (int)((arg >> 16) & 0x1F);
            break;

        case GE_SCISSOR1:
            g_ds.sc_x0 = (int)(arg & 0x3FF);
            g_ds.sc_y0 = (int)((arg >> 10) & 0x3FF);
            break;
        case GE_SCISSOR2:
            g_ds.sc_x1 = (int)(arg & 0x3FF);
            g_ds.sc_y1 = (int)((arg >> 10) & 0x3FF);
            break;

        case GE_ATST:
            g_ds.atest_func = (int)(arg & 7);
            g_ds.atest_ref = (int)((arg >> 8) & 0xFF);
            g_ds.atest_mask = (int)((arg >> 16) & 0xFF);
            break;
        case GE_ALPHA:
            g_ds.blend_src = (int)(arg & 0xF);
            g_ds.blend_dst = (int)((arg >> 4) & 0xF);
            g_ds.blend_op = (int)((arg >> 8) & 0xF);
            break;
        case GE_SFIX:  g_ds.blend_sfix = arg; break;
        case GE_DFIX:  g_ds.blend_dfix = arg; break;

        case GE_TRXSBP:  g_trx.sbp = (g_trx.sbp & 0xFF000000u) | arg; break;
        case GE_TRXSBW:  g_trx.sbw = arg & 0xFFFF; g_trx.sbp = (g_trx.sbp & 0x00FFFFFFu) | ((arg & 0xFF0000u) << 8); break;
        case GE_TRXDBP:  g_trx.dbp = (g_trx.dbp & 0xFF000000u) | arg; break;
        case GE_TRXDBW:  g_trx.dbw = arg & 0xFFFF; g_trx.dbp = (g_trx.dbp & 0x00FFFFFFu) | ((arg & 0xFF0000u) << 8); break;
        case GE_TRXSPOS: g_trx.sx = arg & 0x3FF; g_trx.sy = (arg >> 10) & 0x3FF; break;
        case GE_TRXDPOS: g_trx.dx = arg & 0x3FF; g_trx.dy = (arg >> 10) & 0x3FF; break;
        case GE_TRXSIZE: g_trx.w = (arg & 0x3FF) + 1; g_trx.h = ((arg >> 10) & 0x3FF) + 1; break;
        case GE_TRXKICK: trx_kick(arg); break;

        case GE_VADDR: g_ge.vaddr = (q->base | (arg & 0xFFFFFF)); break;
        case GE_IADDR: g_ge.iaddr = (q->base | (arg & 0xFFFFFF)); break;

        default:
            if (light_cmd(cmd, arg)) break;
            /* A real state command we do not decode individually. Counted --
             * and, for the first few, named. "240 commands not individually
             * decoded" hides whether the game is configuring a draw or just
             * poking state, which is the difference between a rendering bug
             * and a game that has not asked to render yet. */
            g_ge.unknown++;
            g_ge.op_count[cmd]++;
            g_ge.op_last[cmd] = arg;
            break;
        }
    }
}

/* ---- frame replay benchmark ---------------------------------------------
 * Capture (in a running game): create gebench.cmd in the working directory.
 * The GE state at the next frame boundary, that frame's list segments, and
 * guest memory at the frame's end are written to gebench.snap.
 * Replay: PSPRECOMP_GEREPLAY="<snap> [replays]" (default 300). Once the game
 * has booted far enough to have a GL context, the snapshot is loaded over
 * memory and the frame is redrawn <replays> times on the game thread; timings
 * go to gebench.txt, the last image to gebench.ppm, and the process exits.
 * The same frame every time, so a change to the GE/render path can be
 * compared build to build. */
#define BENCH_SEGS 4096
static struct { uint32_t start, end, base; } g_bseg[BENCH_SEGS];
static int g_bn, g_brec;          /* segments recorded; 0 idle, 1 recording */

/* Every piece of GE state a list can depend on from before the frame. */
#define BENCH_STATE(X) X(g_trx) X(g_ds) X(g_tbp) X(g_tbw) X(g_cbp) X(g_psm) X(g_clut) \
    X(g_material) X(g_lt) X(g_zbp) X(g_zbw) X(g_xf) X(g_ge)
static uint8_t *g_bstate; static size_t g_bstate_n;
static size_t bench_state_size(void) { size_t n = 0;
#define SZ(v) n += sizeof v;
    BENCH_STATE(SZ)
#undef SZ
    return n; }
static void bench_state_save(uint8_t *p) {
#define SV(v) memcpy(p, &v, sizeof v); p += sizeof v;
    BENCH_STATE(SV)
#undef SV
}
static void bench_state_load(const uint8_t *p) {
#define LD(v) memcpy(&v, p, sizeof v); p += sizeof v;
    BENCH_STATE(LD)
#undef LD
    if (g_ds.clut) g_ds.clut = g_clut;   /* a host pointer: the saved one is from another process */
}

static void run_list_rec(ge_queue *q) {
    const int i = g_brec && g_bn < BENCH_SEGS ? g_bn++ : -1;
    if (i >= 0) { g_bseg[i].start = q->list; g_bseg[i].base = q->base; }
    run_list(q);
    if (i >= 0) g_bseg[i].end = q->list;
}

/* Guest memory saved with a capture: scratchpad, VRAM, RAM, and the module's
 * own image (mapped at its link address, outside RAM; game globals live there). */
static struct { uint32_t base, size; } BENCH_MEM[4] = {
    { PSP_SCRATCH_BASE, PSP_SCRATCH_SIZE }, { PSP_VRAM_BASE, PSP_VRAM_SIZE }, { PSP_RAM_BASE, PSP_RAM_SIZE }, { 0, 0 } };
static void bench_mem_init(void) { void psp_mem_module_range(uint32_t *, uint32_t *); psp_mem_module_range(&BENCH_MEM[3].base, &BENCH_MEM[3].size); }

static void bench_save(void) {
    FILE *f = fopen("gebench.snap", "wb");
    if (!f) { fprintf(stderr, "gebench: cannot write gebench.snap\n"); return; }
    const uint32_t hdr[3] = { 0x31424547u /* "GEB1" */, (uint32_t)g_bn, (uint32_t)g_bstate_n };
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(g_bseg, sizeof g_bseg[0], (size_t)g_bn, f);
    fwrite(g_bstate, 1, g_bstate_n, f);
    bench_mem_init();
    for (int i = 0; i < 4; i++) if (BENCH_MEM[i].size) fwrite(psp_mem_ptr(BENCH_MEM[i].base, BENCH_MEM[i].size), 1, BENCH_MEM[i].size, f);
    fclose(f);
    fprintf(stderr, "gebench: captured %d segments to gebench.snap\n", g_bn);
}

static int bench_load(const char *path) {
    FILE *f = fopen(path, "rb"); uint32_t hdr[3];
    if (!f || fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != 0x31424547u || hdr[1] > BENCH_SEGS || hdr[2] != bench_state_size()) {
        fprintf(stderr, "gebench: %s missing or from another build\n", path); if (f) fclose(f); return -1; }
    g_bn = (int)hdr[1];
    uint8_t *st = malloc(hdr[2]); int ok = fread(g_bseg, sizeof g_bseg[0], (size_t)g_bn, f) == (size_t)g_bn && fread(st, 1, hdr[2], f) == hdr[2];
    bench_mem_init();
    for (int i = 0; ok && i < 4; i++) if (BENCH_MEM[i].size) ok = fread(psp_mem_ptr(BENCH_MEM[i].base, BENCH_MEM[i].size), 1, BENCH_MEM[i].size, f) == BENCH_MEM[i].size;
    fclose(f);
    if (ok) bench_state_load(st);
    free(st);
    return ok ? 0 : -1;
}

static void bench_frame(const psp_render_backend *rb) {
    for (int i = 0; i < g_bn; i++) {
        ge_queue q = { 0 };
        q.list = g_bseg[i].start; q.stall = g_bseg[i].end; q.base = g_bseg[i].base; q.used = 1;
        run_list(&q);
        if (rb->finish) rb->finish();
    }
}

static void bench_replay(const char *path, int reps) {
    if (bench_load(path) != 0) exit(2);
    uint8_t *st0 = malloc(g_bstate_n = bench_state_size()); bench_state_save(st0); const uint64_t vsnap = g_ge.vertices;
    const psp_render_backend *rb = psp_render_current();
    for (int w = 0; w < 10; w++) { bench_state_load(st0); bench_frame(rb); }   /* warm caches, textures */
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
    ULONG64 c0 = 0, c1 = 0; double best = 1e9; double *each = malloc(sizeof(double) * (size_t)reps);
    QueryPerformanceCounter(&t0); QueryThreadCycleTime(GetCurrentThread(), &c0);

    for (int r = 0; r < reps; r++) {
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        bench_state_load(st0); bench_frame(rb);
        QueryPerformanceCounter(&b);
        each[r] = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (each[r] < best) best = each[r];
    }
    QueryPerformanceCounter(&t1); QueryThreadCycleTime(GetCurrentThread(), &c1);
    const uint64_t verts = g_ge.vertices - vsnap;      /* state reloads each replay: one frame */
    /* median */
    for (int i = 1; i < reps; i++) { double x = each[i]; int j = i - 1; while (j >= 0 && each[j] > x) { each[j + 1] = each[j]; j--; } each[j + 1] = x; }
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart / reps;
    FILE *o = fopen("gebench.txt", "w");
    if (o) {
        fprintf(o, "segments %d\nreplays %d\nms_mean %.4f\nms_median %.4f\nms_best %.4f\nmcycles_per_frame %.4f\nvertices_per_frame %llu\n",
                g_bn, reps, ms, each[reps / 2], best, (double)(c1 - c0) / 1e6 / reps, (unsigned long long)verts);
        fclose(o);
    }
    fprintf(stderr, "gebench: %d segments, median %.4f ms/frame, best %.4f, over %d replays\n", g_bn, each[reps / 2], best, reps);
    if (rb->read_display) {   /* the drawn frame, to check the replay is the real picture */
        static uint32_t px[480 * 272];
        for (int b = 0; b < 3; b++) {   /* the GE target, and the two usual display buffers */
          const uint32_t at = b == 0 ? PSP_VRAM_BASE | (g_ge.fbp & (PSP_VRAM_SIZE - 1)) : b == 1 ? PSP_VRAM_BASE : PSP_VRAM_BASE + 0x88000u;
          char nm[32]; snprintf(nm, sizeof nm, "gebench%d.ppm", b);
          if (rb->read_display(at, 512, 3, px) == 0) {
            FILE *p = fopen(nm, "wb");
            if (p) { fprintf(p, "P6\n480 272\n255\n");
                for (int i = 0; i < 480 * 272; i++) { const uint8_t c[3] = { (uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16) }; fwrite(c, 3, 1, p); }
                fclose(p); }
          }
        }
    }
    fflush(NULL);
    ExitProcess(0);
}

/* Called at each buffer flip (display.c SetFrameBuf): one per game frame. */
void psp_ge_bench_tick(void) {
    static unsigned n;
    static int replay = -1; static char rpath[260]; static int reps = 300;
    if (replay < 0) { const char *e = getenv("PSPRECOMP_GEREPLAY"); replay = 0;
        if (e) { replay = 1; if (sscanf(e, "%259s %d", rpath, &reps) < 1) replay = 0; if (reps <= 0) reps = 300; } }
    ++n;
    if (replay || g_brec || n % 30 == 0) { void psp_ge_drain(void); psp_ge_drain(); }   /* capture/replay see a quiet GE */
    if (replay) { if (n >= 120 && g_ge.lists) bench_replay(rpath, reps); return; }
    static uint64_t v_at, l_at;
    if (getenv("PSPRECOMP_GEBENCH_LOG")) { fprintf(stderr, "flip %u: lists %llu verts %llu\n", n, (unsigned long long)(g_ge.lists - l_at), (unsigned long long)(g_ge.vertices - v_at)); }
    v_at = g_ge.vertices; l_at = g_ge.lists;
    if (n % 30 == 0) { FILE *c = fopen("gestats.cmd", "rb");   /* gestats.cmd: write gestats.txt (debug) */
        if (c) { fclose(c); remove("gestats.cmd"); FILE *o = fopen("gestats.txt", "w"); if (o) { psp_ge_dump_stats(o); fclose(o); } } }
    if (g_brec) { g_brec = 0; bench_save(); remove("gebench.cmd"); return; }   /* the recorded frame just ended */
    if (n % 30) return;
    FILE *c = fopen("gebench.cmd", "r");
    if (!c) return;
    fclose(c);
    free(g_bstate); g_bstate = malloc(g_bstate_n = bench_state_size()); bench_state_save(g_bstate);
    g_bn = 0; g_brec = 1;                /* record from this frame boundary to the next */
}
/* ---- the calls ----------------------------------------------------------- */

static ge_queue *find_queue(uint32_t id) {
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queue[i].used && g_queue[i].id == id) return &g_queue[i];
    return NULL;
}

/* ---- the GE worker thread --------------------------------------------------
 *
 * Hardware runs display lists while the CPU goes on with the next frame. So
 * does this: list work (decoding, lighting, GL) is queued as jobs for one
 * worker thread, in order, and the game thread returns at once. Everything
 * that touches GE or GL state goes through the queue -- lists, stall moves,
 * the vblank frame latch (display.c) -- so the order is the game's order.
 * DrawSync/ListSync wait until the worker is idle, which is what they mean.
 * PSPRECOMP_GE_THREAD=0 runs every job inline instead (the old behaviour). */
enum { J_ENQ, J_STALL, J_CALL };
typedef struct { int kind; uint32_t id, list, stall; void (*fn)(uint32_t, uint32_t, uint32_t); uint32_t a, b, c; } ge_job;
#define GE_JOBS 4096
static ge_job g_jobs[GE_JOBS];
static unsigned g_jw, g_jr;                 /* written / finished; guarded by g_jl */
static SRWLOCK g_jl = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv_work = CONDITION_VARIABLE_INIT, g_cv_done = CONDITION_VARIABLE_INIT;
static int g_ge_threaded = -1;

static void job_run(const ge_job *j) {
    if (j->kind == J_ENQ) {
        ge_queue *q = NULL;
        for (int i = 0; i < MAX_QUEUES; i++) if (!g_queue[i].used) { q = &g_queue[i]; break; }
        if (!q) { static int nf; if (nf++ < 5) fprintf(stderr, "ge: list %08X dropped, all %d queues busy\n", j->list, MAX_QUEUES); return; }
        memset(q, 0, sizeof *q);
        q->id = j->id; q->list = j->list; q->stall = j->stall; q->used = 1;
        run_list_rec(q);
        if (psp_render_current()->finish) psp_render_current()->finish();
    } else if (j->kind == J_STALL) {
        ge_queue *q = find_queue(j->id);
        if (!q) return;                     /* finished already: moving its stall is harmless */
        q->stall = j->stall;
        if (!q->done) run_list_rec(q);      /* the new stall released more commands */
        if (psp_render_current()->finish) psp_render_current()->finish();
    } else j->fn(j->a, j->b, j->c);
}

static DWORD WINAPI ge_worker(LPVOID arg) {
    (void)arg;
    AcquireSRWLockExclusive(&g_jl);
    for (;;) {
        while (g_jr == g_jw) SleepConditionVariableSRW(&g_cv_work, &g_jl, INFINITE, 0);
        const ge_job j = g_jobs[g_jr % GE_JOBS];
        ReleaseSRWLockExclusive(&g_jl);
        job_run(&j);
        AcquireSRWLockExclusive(&g_jl);
        g_jr++;
        WakeAllConditionVariable(&g_cv_done);
    }
}

static int ge_threaded(void) {
    if (g_ge_threaded < 0) {
        const char *e = getenv("PSPRECOMP_GE_THREAD");
        g_ge_threaded = !(e && *e == '0') && !getenv("PSPRECOMP_GEREPLAY");
        if (g_ge_threaded) {
            HANDLE h = CreateThread(NULL, 1 << 20, ge_worker, NULL, 0, NULL);
            if (h) { SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(h); } else g_ge_threaded = 0;
        }
        fprintf(stderr, "ge: display lists run %s\n", g_ge_threaded ? "on a worker thread" : "inline");
    }
    return g_ge_threaded;
}

static void ge_submit(const ge_job *j) {
    if (!ge_threaded()) { job_run(j); return; }
    AcquireSRWLockExclusive(&g_jl);
    while (g_jw - g_jr >= GE_JOBS) SleepConditionVariableSRW(&g_cv_done, &g_jl, INFINITE, 0);
    g_jobs[g_jw % GE_JOBS] = *j;
    g_jw++;
    WakeConditionVariable(&g_cv_work);
    ReleaseSRWLockExclusive(&g_jl);
}

/* Wait until every job submitted so far has run. */
void psp_ge_drain(void) {
    if (g_ge_threaded <= 0) return;
    AcquireSRWLockExclusive(&g_jl);
    const unsigned target = g_jw;
    while ((int)(g_jr - target) < 0) SleepConditionVariableSRW(&g_cv_done, &g_jl, INFINITE, 0);
    ReleaseSRWLockExclusive(&g_jl);
}

/* Run fn(a, b, c) on the GE thread after the work queued before it. */
void psp_ge_submit_call(void (*fn)(uint32_t, uint32_t, uint32_t), uint32_t a, uint32_t b, uint32_t c) {
    ge_job j = { J_CALL }; j.fn = fn; j.a = a; j.b = b; j.c = c;
    ge_submit(&j);
}

static void enqueue(int head) {
    /* (list, stall, cbid, arg) */
    (void)head;
    ge_job j = { J_ENQ };
    j.id = (uint32_t)InterlockedIncrement((volatile LONG *)&g_next_id) - 1;
    j.list = psp_arg(0) & ~3u;
    j.stall = psp_arg(1) & ~3u;
    ge_submit(&j);
    psp_ret(j.id);
}

/* Block transfer (TRX*): a rectangle copy between RAM and VRAM. This is how
 * the game uploads its textures into VRAM; without it every VRAM texture
 * reads as zero and the whole frame samples black. */

static void trx_kick(uint32_t arg) {
    const uint32_t bpp = (arg & 1) ? 4u : 2u;
    static int nlog = -1;
    if (nlog < 0) nlog = getenv("PSPRECOMP_TRX_LOG") ? 0 : 1 << 30;
    if (nlog++ < 60)
        fprintf(stderr, "trx: %08X bw%u (%u,%u) -> %08X bw%u (%u,%u) %ux%u bpp%u\n",
                g_trx.sbp, g_trx.sbw, g_trx.sx, g_trx.sy, g_trx.dbp, g_trx.dbw, g_trx.dx, g_trx.dy,
                g_trx.w, g_trx.h, bpp);
    { static int nf; uint32_t sa = g_trx.sbp & 0x0FFFFFFFu, da = g_trx.dbp & 0x0FFFFFFFu;
      if (getenv("PSPRECOMP_TRX_LOG") && sa >= 0x04000000u && sa < 0x04200000u && da >= 0x04000000u && da < 0x04200000u && nf++ < 40)
          fprintf(stderr, "trx vram->vram: %08X (%u,%u) -> %08X (%u,%u) %ux%u bpp%u%s", g_trx.sbp, g_trx.sx, g_trx.sy, g_trx.dbp, g_trx.dx, g_trx.dy, g_trx.w, g_trx.h, bpp, "\n"); }
    for (uint32_t y = 0; y < g_trx.h; y++) {
        const uint32_t s = g_trx.sbp + ((g_trx.sy + y) * g_trx.sbw + g_trx.sx) * bpp;
        const uint32_t d = g_trx.dbp + ((g_trx.dy + y) * g_trx.dbw + g_trx.dx) * bpp;
        uint8_t *sp = (uint8_t *)psp_mem_ptr(s, g_trx.w * bpp);
        uint8_t *dp = (uint8_t *)psp_mem_ptr(d, g_trx.w * bpp);
        if (!sp || !dp) { fprintf(stderr, "ge: bad transfer %08X -> %08X%s", s, d, "\n"); return; }
        memmove(dp, sp, g_trx.w * bpp);
    }
}

static void hle_ListEnQueue(void)     { enqueue(0); }
static void hle_ListEnQueueHead(void) { enqueue(1); }

static void hle_ListUpdateStallAddr(void) {
    uint32_t id = psp_arg(0);
    /* An id never issued is an error; one that finished already is not. */
    if (!(id >= 0x00080000u && id < g_next_id)) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    ge_job j = { J_STALL };
    j.id = id; j.stall = psp_arg(1) & ~3u;
    ge_submit(&j);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Every list queued so far is complete once the worker is idle. */
static void hle_ListSync(void) { psp_ge_drain(); psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_DrawSync(void) { psp_ge_drain(); psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_Break(void)    { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_Continue(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

static void hle_SetCallback(void)   { psp_ret(0); }
static void hle_UnsetCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* eDRAM is the GPU-visible VRAM window: 2 MB at 0x04000000. */
static void hle_EdramGetAddr(void) { psp_ret(PSP_VRAM_BASE); }
static void hle_EdramGetSize(void) { psp_ret(PSP_VRAM_SIZE); }

void psp_ge_register(void) {
    psp_hle_register(0xAB49E76A, "sceGe_user", "sceGeListEnQueue",         hle_ListEnQueue);
    psp_hle_register(0x1C0D95A6, "sceGe_user", "sceGeListEnQueueHead",     hle_ListEnQueueHead);
    psp_hle_register(0xE0D68148, "sceGe_user", "sceGeListUpdateStallAddr", hle_ListUpdateStallAddr);
    psp_hle_register(0x03444EB4, "sceGe_user", "sceGeListSync",            hle_ListSync);
    psp_hle_register(0xB287BD61, "sceGe_user", "sceGeDrawSync",            hle_DrawSync);
    psp_hle_register(0xB448EC0D, "sceGe_user", "sceGeBreak",               hle_Break);
    psp_hle_register(0x4C06E472, "sceGe_user", "sceGeContinue",            hle_Continue);
    psp_hle_register(0xA4FC06A4, "sceGe_user", "sceGeSetCallback",         hle_SetCallback);
    psp_hle_register(0x05DB22CE, "sceGe_user", "sceGeUnsetCallback",       hle_UnsetCallback);
    psp_hle_register(0xE47E40E4, "sceGe_user", "sceGeEdramGetAddr",        hle_EdramGetAddr);
    psp_hle_register(0x1F6752AD, "sceGe_user", "sceGeEdramGetSize",        hle_EdramGetSize);
}
