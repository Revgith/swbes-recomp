/* psprecomp -- OpenGL 3.3 render backend (Windows / WGL).
 *
 * Same input as the software rasterizer: assembled screen-space vertices plus
 * the decoded draw state. What changes is where pixels go:
 *
 *   - every framebuffer address the GE draws into becomes a GL framebuffer
 *     object (colour texture + depth), created on first use;
 *   - a texture whose address is one of those framebuffers samples the FBO
 *     directly, so render-to-texture (the bloom chain, reflections) stays on
 *     the GPU;
 *   - any other texture is decoded once through psp_tex_fetch -- the one place
 *     that knows the formats, swizzle and CLUTs -- and cached by content hash;
 *   - the display path asks for the shown buffer back (read_display), so the
 *     existing window keeps working unchanged.
 *
 * Guest threads run one at a time but on different host threads, and a GL
 * context can only be current on one of them. Every entry point makes the
 * context current on the calling thread and finish() (end of each display
 * list) releases it again.
 *
 * PSPRECOMP_GL_SCALE=<n> renders every framebuffer at n x its PSP size.
 */
#include "psprecomp/render.h"
#include "psprecomp/mem.h"
#include "psprecomp/window.h"
#include "psprecomp/mods.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_tr_live;
static int g_skippost = -1;   /* nopost.cmd: "1"/"0" toggles the glow pass live */
static int g_fdraw, g_maxdraw = -2;   /* PSPRECOMP_GL_MAXDRAW / maxdraw.cmd: drop draws past N in a frame */   /* live trace: log every draw of this frame (work/run/gltrace.cmd) */
static int nd;
static unsigned long long g_vgen;

/* ---- the few GL 2+/3 entry points we need -------------------------------- */

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_FRAMEBUFFER            0x8D40
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_DEPTH_COMPONENT24      0x81A6
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_VERTEX_SHADER          0x8B31
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_ARRAY_BUFFER           0x8892
#define GL_STREAM_DRAW            0x88E0
#define GL_TEXTURE0               0x84C0
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_FUNC_ADD               0x8006
#define GL_FUNC_SUBTRACT          0x800A
#define GL_FUNC_REVERSE_SUBTRACT  0x800B
#define GL_MIN                    0x8007
#define GL_MAX                    0x8008
#define GL_CONSTANT_COLOR         0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_PIXEL_PACK_BUFFER      0x88EB
#define GL_STREAM_READ            0x88E1
#define GL_READ_ONLY              0x88B8
#define GL_READ_FRAMEBUFFER       0x8CA8
#define GL_DRAW_FRAMEBUFFER       0x8CA9
#define GL_BGRA_EXT               0x80E1

#define GLFUNCS(X) \
    X(void,   GenFramebuffers,        (GLsizei, GLuint *)) \
    X(void,   BindFramebuffer,        (GLenum, GLuint)) \
    X(void,   FramebufferTexture2D,   (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void,   GenRenderbuffers,       (GLsizei, GLuint *)) \
    X(void,   BindRenderbuffer,       (GLenum, GLuint)) \
    X(void,   RenderbufferStorage,    (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void,   FramebufferRenderbuffer,(GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(GLuint, CreateShader,           (GLenum)) \
    X(void,   ShaderSource,           (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void,   CompileShader,          (GLuint)) \
    X(void,   GetShaderiv,            (GLuint, GLenum, GLint *)) \
    X(void,   GetShaderInfoLog,       (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, CreateProgram,          (void)) \
    X(void,   AttachShader,           (GLuint, GLuint)) \
    X(void,   BindAttribLocation,     (GLuint, GLuint, const GLchar *)) \
    X(void,   LinkProgram,            (GLuint)) \
    X(void,   GetProgramiv,           (GLuint, GLenum, GLint *)) \
    X(void,   GetProgramInfoLog,      (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,   UseProgram,             (GLuint)) \
    X(GLint,  GetUniformLocation,     (GLuint, const GLchar *)) \
    X(void,   Uniform1i,              (GLint, GLint)) \
    X(void,   Uniform2f,              (GLint, GLfloat, GLfloat)) \
    X(void,   Uniform1f,              (GLint, GLfloat)) \
    X(void,   Uniform3f,              (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void,   Uniform3i,              (GLint, GLint, GLint, GLint)) \
    X(void,   BlendEquationSeparate,  (GLenum, GLenum)) \
    X(void,   Uniform4i,              (GLint, GLint, GLint, GLint, GLint)) \
    X(void,   Uniform1fv,             (GLint, GLsizei, const GLfloat *)) \
    X(void,   GenBuffers,             (GLsizei, GLuint *)) \
    X(void,   BindBuffer,             (GLenum, GLuint)) \
    X(void,   BufferData,             (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void,   GenVertexArrays,        (GLsizei, GLuint *)) \
    X(void,   BindVertexArray,        (GLuint)) \
    X(void,   VertexAttribPointer,    (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void,   EnableVertexAttribArray,(GLuint)) \
    X(void,   BlendFuncSeparate,      (GLenum, GLenum, GLenum, GLenum)) \
    X(void,   BlendEquation,          (GLenum)) \
    X(void,   BlendColor,             (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   ActiveTexture,          (GLenum)) \
    X(void,   GenerateMipmap,         (GLenum)) \
    X(void *, MapBuffer,              (GLenum, GLenum)) \
    X(GLboolean, UnmapBuffer,         (GLenum)) \
    X(void,   BlitFramebuffer,        (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void,   DeleteFramebuffers,     (GLsizei, const GLuint *)) \
    X(void,   DeleteRenderbuffers,    (GLsizei, const GLuint *)) \
    X(void,   DeleteBuffers,          (GLsizei, const GLuint *))

#define DECL(ret, name, args) typedef ret (APIENTRY *PFN_##name) args; static PFN_##name gl##name;
GLFUNCS(DECL)
#undef DECL

static void (APIENTRY *glTextureBarrier)(void);
static int load_gl(void) {
    int ok = 1;
#define LOAD(ret, name, args) \
    gl##name = (PFN_##name)(void *)wglGetProcAddress("gl" #name); \
    if (!gl##name) { fprintf(stderr, "gl: missing gl" #name "\n"); ok = 0; }
    GLFUNCS(LOAD)
    glTextureBarrier = (void (APIENTRY *)(void))(void *)wglGetProcAddress("glTextureBarrier");   /* optional (GL 4.5): the framebuffer-read path */
#undef LOAD
    return ok;
}

/* ---- context ------------------------------------------------------------- */

static HWND  g_wnd;
static HDC   g_dc;
static HGLRC g_rc;
static int   g_ready = -1;              /* -1 untried, 0 failed, 1 ok */
static int   g_scale = 1;
static CRITICAL_SECTION g_lock;
static DWORD g_owner;                   /* thread the context is current on */

static int create_context(void) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "psprecomp_gl";
    RegisterClassA(&wc);
    g_wnd = CreateWindowA(wc.lpszClassName, "gl", WS_POPUP, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 0;
    g_dc = GetDC(g_wnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd)) return 0;
    g_rc = wglCreateContext(g_dc);
    if (!g_rc || !wglMakeCurrent(g_dc, g_rc)) return 0;
    g_owner = GetCurrentThreadId();
    fprintf(stderr, "gl: %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    return load_gl();
}

static int setup_pipeline(void);

/* Make the context current here. Returns 0 if GL is unusable. */
static volatile int g_want_ctx;   /* another thread waits for the GL context */
static int acquire(void) {
    if (g_ready == 0) return 0;
    if (g_ready < 0) {
        InitializeCriticalSection(&g_lock);
        EnterCriticalSection(&g_lock);
        const char *s = getenv("PSPRECOMP_GL_SCALE");
        g_scale = s ? atoi(s) : 1;
        if (g_scale < 1) g_scale = 1;
        if (g_scale > 8) g_scale = 8;
        g_ready = create_context() && setup_pipeline();
        if (!g_ready) { fprintf(stderr, "gl: initialisation failed; nothing will be drawn\n"); LeaveCriticalSection(&g_lock); return 0; }
        return 1;
    }
    EnterCriticalSection(&g_lock);
    DWORD me = GetCurrentThreadId();
    if (g_owner != me && g_owner != 0) {
        /* The context stays bound to the thread that last drew (rebinding at
         * every display list was a measurable share of the frame). Ask that
         * thread to let go at its next list end, and wait briefly. */
        g_want_ctx = 1;
        for (int i = 0; i < 200 && g_owner != 0 && g_owner != me; i++) { LeaveCriticalSection(&g_lock); Sleep(1); EnterCriticalSection(&g_lock); }
        g_want_ctx = 0;
    }
    if (g_owner != me) {
        if (!wglMakeCurrent(g_dc, g_rc)) {
            static int nf; if (nf++ < 5) fprintf(stderr, "gl: makecurrent failed on thread %lu (owner %lu)\n", me, g_owner);
            LeaveCriticalSection(&g_lock); return 0; }
        g_owner = me;
    }
    return 1;
}
static void release(void) { LeaveCriticalSection(&g_lock); }

/* ---- shaders ------------------------------------------------------------- */

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 pos;\n"          /* clip-space, already * w */
    "layout(location=1) in vec2 uv;\n"
    "layout(location=2) in vec4 col;\n"
    "layout(location=3) in vec4 rect;\n"
    "layout(location=4) in float fog;\n"
    "layout(location=5) in float q;\n"
    "out vec2 v_uv; out vec4 v_col; flat out vec4 v_flat; flat out vec4 v_rect; out float v_fog, v_q;\n"
    "void main() { gl_Position = pos; v_uv = uv; v_col = col; v_flat = col; v_rect = rect; v_fog = fog; v_q = q; }\n";

static const char *FS =
    "#version 330 core\n"
    "in vec2 v_uv; in vec4 v_col; flat in vec4 v_flat; flat in vec4 v_rect; in float v_fog, v_q;\n"
    /* The framebuffer-read path, for what fixed-function GL cannot do: the
     * blend, stencil (in alpha), logic op, dither and bit masks, all done here
     * from the destination pixel. Only on for draws that need it. */
    "uniform sampler2D fb; uniform int fetch_on, b_on, b_src, b_dst, b_op; uniform vec3 b_sfix, b_dfix;\n"
    "uniform int st_on, st_func, st_ref, st_mask, sop_fail, sop_zfail, sop_zpass, st_pass, lop, fbfmt, dith_on; uniform ivec4 pmask;\n"
    "uniform float dith[16]; uniform float scale;\n"
    "bool cmpf(int f, int a, int b) { return f == 0 ? false : f == 1 ? true : f == 2 ? a == b : f == 3 ? a != b :\n"
    "  f == 4 ? a < b : f == 5 ? a <= b : f == 6 ? a > b : a >= b; }\n"
    "int sop(int op, int s) { return op == 0 ? s : op == 1 ? 0 : op == 2 ? st_ref : op == 3 ? (~s & 255) :\n"
    "  op == 4 ? min(s + 1, 255) : op == 5 ? max(s - 1, 0) : s; }\n"
    "vec3 bfac(int k, bool src, vec4 s, vec4 d) {\n"
    "  if (k == 0) return src ? d.rgb : s.rgb; if (k == 1) return 1.0 - (src ? d.rgb : s.rgb);\n"
    "  if (k == 2) return vec3(s.a); if (k == 3) return vec3(1.0 - s.a); if (k == 4) return vec3(d.a); if (k == 5) return vec3(1.0 - d.a);\n"
    "  if (k == 6) return vec3(min(2.0 * s.a, 1.0)); if (k == 7) return vec3(max(1.0 - 2.0 * s.a, 0.0));\n"
    "  if (k == 8) return vec3(min(2.0 * d.a, 1.0)); if (k == 9) return vec3(max(1.0 - 2.0 * d.a, 0.0));\n"
    "  return src ? b_sfix : b_dfix; }\n"
    "ivec3 logic(int f, ivec3 s, ivec3 d) {\n"
    "  ivec3 r = f == 0 ? ivec3(0) : f == 1 ? s & d : f == 2 ? s & ~d : f == 3 ? s : f == 4 ? ~s & d : f == 5 ? d :\n"
    "    f == 6 ? s ^ d : f == 7 ? s | d : f == 8 ? ~(s | d) : f == 9 ? ~(s ^ d) : f == 10 ? ~d : f == 11 ? s | ~d :\n"
    "    f == 12 ? ~s : f == 13 ? ~s | d : f == 14 ? ~(s & d) : ivec3(255);\n"
    "  return r & 255; }\n"
    "vec4 fetch_out(vec4 c) {\n"
    "  vec4 d = texelFetch(fb, ivec2(gl_FragCoord.xy), 0);\n"
    "  int da = int(d.a * 255.0 + 0.5), na = int(c.a * 255.0 + 0.5);\n"
    "  ivec3 dc = ivec3(d.rgb * 255.0 + 0.5);\n"
    /* Stencil, (ref & mask) FUNC (stencil & mask), tested before depth. A
     * stencil draw is split into passes (gl_draw): 1 = fragments failing
     * depth (zfail / fail op), 2 = passing depth but failing stencil (fail op,
     * no depth write), 0 = the draw proper (zpass op). Passes 1 and 2 only
     * change the stencil, i.e. alpha. */
    "  if (st_on == 1) {\n"
    "    bool sp = cmpf(st_func, st_ref & st_mask, da & st_mask);\n"
    "    if (st_pass == 1) return vec4(vec3(dc) / 255.0, float(sop(sp ? sop_zfail : sop_fail, da)) / 255.0);\n"
    "    if (st_pass == 2) { if (sp) discard; return vec4(vec3(dc) / 255.0, float(sop(sop_fail, da)) / 255.0); }\n"
    "    if (!sp) discard;\n"
    "    na = sop(sop_zpass, da); }\n"
    "  vec3 rgb = c.rgb;\n"
    "  if (b_on == 1) {\n"
    "    vec3 s = c.rgb * bfac(b_src, true, c, d), t = d.rgb * bfac(b_dst, false, c, d);\n"
    "    rgb = b_op == 0 ? s + t : b_op == 1 ? s - t : b_op == 2 ? t - s : b_op == 3 ? min(c.rgb, d.rgb) :\n"
    "          b_op == 4 ? max(c.rgb, d.rgb) : abs(c.rgb - d.rgb);\n"
    "    rgb = clamp(rgb, 0.0, 1.0); }\n"
    "  ivec3 ic = ivec3(rgb * 255.0 + 0.5);\n"
    "  if (dith_on == 1) { ivec2 p = ivec2(gl_FragCoord.xy / scale) & 3; ic = clamp(ic + int(dith[p.y * 4 + p.x]), 0, 255); }\n"
    "  if (fbfmt == 0) ic &= ivec3(0xF8, 0xFC, 0xF8); else if (fbfmt == 1) ic &= 0xF8; else if (fbfmt == 2) ic &= 0xF0;\n"
    "  if (fbfmt == 0) { ic |= ivec3(ic.r >> 5, ic.g >> 6, ic.b >> 5); } else if (fbfmt == 1) ic |= ic >> 5; else if (fbfmt == 2) ic |= ic >> 4;\n"
    "  if (lop >= 0) ic = logic(lop, ic, dc);\n"
    "  ic = (ic & ~pmask.rgb) | (dc & pmask.rgb); na = (na & ~pmask.a) | (da & pmask.a);\n"
    "  if (fbfmt == 1) na = na >= 128 ? 255 : 0; else if (fbfmt == 2) na = (na >> 4) * 17;\n"
    "  return vec4(vec3(ic) / 255.0, float(na) / 255.0); }\n"
    "uniform sampler2D tex;\n"
    "uniform int tex_on, func, tex_alpha, dbl, flat_shade, clear_mode;\n"
    "uniform int at_on, at_func, at_ref, at_mask;\n"
    "uniform vec2 tex_size; uniform vec3 env; uniform vec3 src_mul;\n"
    "uniform int fog_on, ct_func, zr_on; uniform vec3 fog_col; uniform ivec3 ct_ref, ct_mask; uniform vec2 zr;\n"
    "uniform int lod_mode; uniform float lod_slope, lod_bias;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  if (zr_on == 1) { float z = gl_FragCoord.z * 65535.0; if (z < zr.x || z > zr.y) discard; }\n"   /* MINZ/MAXZ */
    "  vec4 c = flat_shade == 1 ? v_flat : v_col;\n"
    "  if (clear_mode == 0 && tex_on == 1) {\n"
    "    vec2 tc = clamp(v_uv / v_q, v_rect.xy, v_rect.zw) / tex_size;\n"
    /* TEXLEVEL slope mode: the level from the fragment's w, not its derivatives */
    "    vec4 t = lod_mode == 1 ? textureLod(tex, tc, log2(max(lod_slope / gl_FragCoord.w, 1e-6)) + lod_bias) : texture(tex, tc);\n"
    "    vec3 rgb; float a = c.a;\n"
    "    if (func == 0)      { rgb = t.rgb * c.rgb;                          if (tex_alpha == 1) a = t.a * c.a; }\n"
    "    else if (func == 1) { rgb = tex_alpha == 1 ? mix(c.rgb, t.rgb, t.a) : t.rgb; }\n"
    "    else if (func == 2) { rgb = mix(c.rgb, env, t.rgb);                 if (tex_alpha == 1) a = t.a * c.a; }\n"
    "    else if (func == 3) { rgb = t.rgb;                                  if (tex_alpha == 1) a = t.a; }\n"
    "    else                { rgb = t.rgb + c.rgb;                          if (tex_alpha == 1) a = t.a * c.a; }\n"
    "    if (dbl == 1) rgb *= 2.0;\n"
    "    c = vec4(min(rgb, vec3(1.0)), a);\n"
    "  }\n"
    "  if (clear_mode == 0 && at_on == 1) {\n"
    "    int ia = int(c.a * 255.0 + 0.5) & at_mask, r = at_ref & at_mask; bool p;\n"
    "    if (at_func == 0) p = false; else if (at_func == 1) p = true;\n"
    "    else if (at_func == 2) p = ia == r; else if (at_func == 3) p = ia != r;\n"
    "    else if (at_func == 4) p = ia < r;  else if (at_func == 5) p = ia <= r;\n"
    "    else if (at_func == 6) p = ia > r;  else p = ia >= r;\n"
    "    if (!p) discard;\n"
    "  }\n"
    "  if (clear_mode == 0 && ct_func != 1) {\n"   /* colour test: 0 never, 2 equal, 3 not equal */
    "    ivec3 ic = ivec3(c.rgb * 255.0 + 0.5) & ct_mask; bool eq = ic == (ct_ref & ct_mask);\n"
    "    if (ct_func == 0 || (ct_func == 2 && !eq) || (ct_func == 3 && eq)) discard;\n"
    "  }\n"
    "  if (clear_mode == 0 && fog_on == 1) c.rgb = mix(c.rgb, fog_col, clamp(v_fog, 0.0, 1.0));\n"
    "  o = fetch_on == 1 && clear_mode == 0 ? fetch_out(c) : vec4(c.rgb * src_mul, c.a);\n"
    "}\n";

static GLuint g_prog, g_vao, g_vbo;
static GLint  u_src_mul, u_tex_on, u_func, u_tex_alpha, u_dbl, u_flat, u_clear, u_at_on, u_at_func,
              u_at_ref, u_at_mask, u_tex_size, u_env, u_tex, u_fog_on, u_fog_col, u_ct_func, u_ct_ref, u_ct_mask,
              u_zr_on, u_zr, u_lod_mode, u_lod_slope, u_lod_bias,
              u_fb, u_fetch_on, u_b_on, u_b_src, u_b_dst, u_b_op, u_b_sfix, u_b_dfix, u_st_on, u_st_func, u_st_ref,
              u_st_mask, u_sop_fail, u_sop_zfail, u_sop_zpass, u_st_pass, u_lop, u_fbfmt, u_dith_on, u_pmask, u_dith, u_scale;
static int g_fetch;   /* the applied state draws through the framebuffer-read path */

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "gl: shader: %s\n", log); }
    return s;
}

typedef struct { float x, y, z, w, u, v; uint32_t rgba; float r[4]; float fog, q; } gl_vtx;   /* r: uv clamp rect (sprites) */

static int setup_pipeline(void) {
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(g_prog);
    GLint ok; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(g_prog, sizeof log, NULL, log); fprintf(stderr, "gl: link: %s\n", log); return 0; }
    glUseProgram(g_prog);
#define U(v, n) v = glGetUniformLocation(g_prog, n)
    U(u_tex_on, "tex_on"); U(u_func, "func"); U(u_tex_alpha, "tex_alpha"); U(u_dbl, "dbl");
    U(u_flat, "flat_shade"); U(u_clear, "clear_mode"); U(u_src_mul, "src_mul"); U(u_at_on, "at_on"); U(u_at_func, "at_func");
    U(u_at_ref, "at_ref"); U(u_at_mask, "at_mask"); U(u_tex_size, "tex_size"); U(u_env, "env"); U(u_tex, "tex");
    U(u_fog_on, "fog_on"); U(u_fog_col, "fog_col"); U(u_ct_func, "ct_func"); U(u_ct_ref, "ct_ref"); U(u_ct_mask, "ct_mask");
    U(u_zr_on, "zr_on"); U(u_zr, "zr"); U(u_lod_mode, "lod_mode"); U(u_lod_slope, "lod_slope"); U(u_lod_bias, "lod_bias");
    U(u_fb, "fb"); U(u_fetch_on, "fetch_on"); U(u_b_on, "b_on"); U(u_b_src, "b_src"); U(u_b_dst, "b_dst"); U(u_b_op, "b_op");
    U(u_b_sfix, "b_sfix"); U(u_b_dfix, "b_dfix"); U(u_st_on, "st_on"); U(u_st_func, "st_func"); U(u_st_ref, "st_ref");
    U(u_st_mask, "st_mask"); U(u_sop_fail, "sop_fail"); U(u_sop_zpass, "sop_zpass"); U(u_sop_zfail, "sop_zfail"); U(u_st_pass, "st_pass"); U(u_lop, "lop"); U(u_fbfmt, "fbfmt");
    U(u_dith_on, "dith_on"); U(u_pmask, "pmask"); U(u_dith, "dith"); U(u_scale, "scale");
#undef U
    glUniform1i(u_tex, 0); glUniform1i(u_ct_func, 1); glUniform1i(u_fb, 1);
    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vtx), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(gl_vtx), (void *)16);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(gl_vtx), (void *)24);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vtx), (void *)28);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(gl_vtx), (void *)44);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2); glEnableVertexAttribArray(3);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(gl_vtx), (void *)48);
    glEnableVertexAttribArray(4); glEnableVertexAttribArray(5);
    return 1;
}

/* ---- framebuffers -------------------------------------------------------- */

typedef struct { uint32_t addr; int w, h, fmt; GLuint fbo, tex, depth; } gl_fb;
#define MAX_FB 32
static gl_fb g_fb[MAX_FB];
static int   g_nfb;
static gl_fb *g_cur;
static GLuint shared_depth;          /* see get_fb */
static uint32_t g_tgt_addr, g_tgt_stride; static int g_tgt_fmt;

static gl_fb *find_fb(uint32_t addr) {
    addr = (addr & (PSP_VRAM_SIZE - 1)) | PSP_VRAM_BASE;
    for (int i = 0; i < g_nfb; i++) if (g_fb[i].addr == addr) return &g_fb[i];
    return NULL;
}

/* The PSP never says how tall a framebuffer is; 272 rows is the screen, and
 * render targets for effects are smaller. 512 covers any texture view of it. */
static int g_sv;                   /* see apply_state cache below */
static gl_fb *get_fb(uint32_t addr, int stride, int fmt) {
    g_sv = 0;
    gl_fb *f = find_fb(addr);
    if (f && f->w == stride) return f;
    if (!f) {
        if (g_nfb == MAX_FB) return &g_fb[0];
        f = &g_fb[g_nfb++];
    }
    f->addr = (addr & (PSP_VRAM_SIZE - 1)) | PSP_VRAM_BASE; f->w = stride; f->h = 512; f->fmt = fmt;
    glGenTextures(1, &f->tex);
    glBindTexture(GL_TEXTURE_2D, f->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->w * g_scale, f->h * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* The GE has one depth buffer (ZBP) shared by every colour buffer: the game
     * clears it while drawing into one and depth-tests against it from the
     * other. One renderbuffer, attached to all, says the same. GL 3 allows
     * attachments of different sizes; drawing uses their intersection. */

    if (!shared_depth) {
        glGenRenderbuffers(1, &shared_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, shared_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 512 * g_scale, 512 * g_scale);
    }
    f->depth = shared_depth;
    glGenFramebuffers(1, &f->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, f->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "gl: framebuffer %08X incomplete\n", addr);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);       /* depth is shared: leave it to the game */
    fprintf(stderr, "gl: framebuffer %08X  %dx%d x%d\n", f->addr, f->w, f->h, g_scale);
    return f;
}

/* Internal resolution changed (settings menu): rebuild every render target at
 * the new size, carrying its picture over, and the shared depth buffer. Runs
 * at a vblank, between frames. */
static void rescale(int ns) {
    GLuint nd;
    glGenRenderbuffers(1, &nd);
    glBindRenderbuffer(GL_RENDERBUFFER, nd);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 512 * ns, 512 * ns);
    for (int i = 0; i < g_nfb; i++) {
        gl_fb *f = &g_fb[i]; GLuint t, fb;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->w * ns, f->h * ns, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, nd);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, f->w * g_scale, f->h * g_scale, 0, 0, f->w * ns, f->h * ns, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glDeleteFramebuffers(1, &f->fbo); glDeleteTextures(1, &f->tex);
        f->fbo = fb; f->tex = t; f->depth = nd;
    }
    if (shared_depth) glDeleteRenderbuffers(1, &shared_depth);
    shared_depth = nd;
    fprintf(stderr, "gl: internal resolution %dx -> %dx (%d targets)\n", g_scale, ns, g_nfb);
    g_scale = ns;
    g_sv = 0;                           /* viewport and scissor re-applied on the next draw */
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? g_cur->fbo : 0);
}

/* The shown frame at the internal resolution, BGRA top-down, for the window
 * (psp_display_read_hires). Two slots: the reader copies the published one
 * under a shared lock, the writer fills the other and flips under an
 * exclusive one, so a slot is never written while it is being read. */
static uint32_t *g_hi[2]; static int g_hi_w, g_hi_h, g_hi_cap; static volatile int g_hi_ready = -1;
static SRWLOCK g_hi_lock = SRWLOCK_INIT;
int psp_display_read_hires(unsigned int *out, int cap, int *w, int *h) {
    AcquireSRWLockShared(&g_hi_lock);
    const int s = g_hi_ready, n = g_hi_w * g_hi_h;
    int r = -1;
    if (s >= 0 && n <= cap) { memcpy(out, g_hi[s], (size_t)n * 4); *w = g_hi_w; *h = g_hi_h; r = 0; }
    ReleaseSRWLockShared(&g_hi_lock);
    return r;
}
int psp_display_hires_lock(const unsigned int **px, int *w, int *h) {
    AcquireSRWLockShared(&g_hi_lock);
    if (g_hi_ready < 0) { ReleaseSRWLockShared(&g_hi_lock); return -1; }
    *px = g_hi[g_hi_ready]; *w = g_hi_w; *h = g_hi_h;
    return 0;
}
void psp_display_hires_unlock(void) { ReleaseSRWLockShared(&g_hi_lock); }

/* Downscale target: the internal-resolution frame is shrunk on the GPU to the
 * size the window shows it at, so the readback and the window's draw are
 * that size, not the full render size. */
static GLuint g_dn_fbo, g_dn_tex; static int g_dn_w, g_dn_h;
static void dn_target(int w, int h) {
    if (g_dn_w == w && g_dn_h == h) return;
    if (!g_dn_fbo) { glGenFramebuffers(1, &g_dn_fbo); glGenTextures(1, &g_dn_tex); }
    glBindTexture(GL_TEXTURE_2D, g_dn_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, g_dn_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_dn_tex, 0);
    g_dn_w = w; g_dn_h = h;
}

/* Copy a render target back into emulated VRAM in its PSP pixel format
 * (0 = 565, 1 = 5551, 2 = 4444, 3 = 8888), point-sampled down from the
 * render scale. */
static void fb_to_vram(gl_fb *f, int rows) {
    static uint32_t px[512 * 512];
    if (rows > f->h) rows = f->h;
    if (rows <= 0) return;
    if (g_tr_live) fprintf(stderr, "gl:    fb_to_vram %08X\n", f->addr);
    /* Only the rows asked for, at 1x: above 1x a nearest blit shrinks them
     * on the GPU first, so the (synchronous) read stays the PSP's size. */
    if (g_scale > 1) {
        static GLuint sfb, stex;
        if (!sfb) { glGenFramebuffers(1, &sfb); glGenTextures(1, &stex); glBindTexture(GL_TEXTURE_2D, stex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glBindFramebuffer(GL_FRAMEBUFFER, sfb); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stex, 0); }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sfb);
        glDisable(GL_SCISSOR_TEST); g_sv = 0;
        glBlitFramebuffer(0, 0, f->w * g_scale, rows * g_scale, 0, 0, f->w, rows, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, sfb);
    } else glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, f->w, rows, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? g_cur->fbo : 0);
    g_vgen++;                           /* cached decodes of VRAM are now stale */
    const int bpp = f->fmt == 3 ? 4 : 2;
    for (int y = 0; y < rows; y++) {
        uint8_t *row = (uint8_t *)psp_mem_ptr(f->addr + (uint32_t)(y * f->w * bpp), (uint32_t)(f->w * bpp));
        if (!row) break;
        for (int x = 0; x < f->w; x++) {
            const uint32_t c = px[y * f->w + x];
            const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
            if (bpp == 4) { memcpy(row + x * 4, &c, 4); continue; }
            uint16_t v;
            if (f->fmt == 0)      v = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
            else if (f->fmt == 1) v = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15));
            else                  v = (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12));
            memcpy(row + x * 2, &v, 2);
        }
    }
}

/* ---- textures ------------------------------------------------------------ */

typedef struct { uint64_t key[4], hash; uint64_t frame; GLuint tex; int w, h; uint64_t used; int mip; } gl_tex;
#define MAX_TEX 1024
static gl_tex   g_tex[MAX_TEX];
static uint64_t g_tick, g_frame = 1;
static gl_tex  *g_last_tex;      /* the texture get_texture() last returned */
static int      g_through, g_tex_bound;      /* the current draw is screen-space (HUD, menus) */
/* The draw state last applied to GL, so an unchanged draw skips apply_state
 * (about 30 GL calls). Invalidated by anything else that touches GL state. */
static int g_sv; static psp_draw_state g_sv_st; static gl_fb *g_sv_fb; static int g_sv_through;

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    /* Four 64-bit lanes, 32 bytes per step: byte-wise FNV was a top profile
     * entry, since every texture is re-hashed once per frame. */
    uint64_t a = h, b = h ^ 0x9E3779B97F4A7C15ull, c = h + 0x632BE59BD9B4E019ull, d = ~h;
    const uint64_t K = 0xC2B2AE3D27D4EB4Full;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4]; memcpy(w, p + i, 32);
        a = (a ^ w[0]) * K; b = (b ^ w[1]) * K; c = (c ^ w[2]) * K; d = (d ^ w[3]) * K;
        a ^= a >> 29; b ^= b >> 29; c ^= c >> 29; d ^= d >> 29;
    }
    h = a ^ (b * 31) ^ (c * 1009) ^ (d * 65537) ^ (uint64_t)n;
    for (; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static const int TEX_BPP[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };

const uint32_t *(*psp_tex_post)(const uint32_t *px, int w, int h, int *ow, int *oh);
uint32_t (*psp_sprite_color)(int tw, int th, float u0, float v0, float u1, float v1, uint32_t rgba);
static volatile LONG g_tex_regen;
void psp_tex_refresh(void) { InterlockedIncrement(&g_tex_regen); }
uint32_t g_clut_src;   /* the last CLOAD's source (ge.c), for PSPRECOMP_TEXWATCH */
uint32_t g_clut_gen;   /* bumped by every CLOAD: the state's CLUT pointer never changes, its contents do */
static GLuint get_texture(const psp_draw_state *st) {
    const uint64_t frame = g_frame * 1000003ull + g_vgen;
    {   /* the enabled mods changed, or psp_tex_post's output did: decode (and replace) everything again */
        static unsigned gen; const unsigned g = psp_mods_generation() ^ (unsigned)g_tex_regen << 16;
        if (g != gen) { gen = g; for (int i = 0; i < MAX_TEX; i++) { g_tex[i].hash = 0; g_tex[i].frame = 0; } }
    }
    /* Exact identity, compared field by field: an XOR-folded key collided
     * (address and buffer width overlapped), and two textures sharing a key
     * evict each other every draw -- seen as textures blinking in and out.
     * The same texels read through a different CLUT window are a different
     * texture: the bloom chain reads one buffer several times per frame with
     * only the CLUT shift changed. */
    uint64_t key[4] = {
        (uint64_t)st->tex_addr | ((uint64_t)st->tex_fmt << 32) | ((uint64_t)st->tex_swizzle << 40),
        (uint64_t)st->tex_w | ((uint64_t)st->tex_h << 16) | ((uint64_t)st->tex_bw << 32), 0, 0 };
    if (st->tex_fmt >= PSP_TPSM_T4 && st->tex_fmt <= PSP_TPSM_T32) {
        /* CLUT contents, not its pointer: the game reloads a different
         * palette into the same CLUT buffer between draws (per-team unit
         * colours), and a pointer key served the first palette to both. */
        /* Consecutive draws mostly share a palette: a compare against the
         * last one is far cheaper than hashing it again. */
        static uint8_t last_clut[1024]; static uint64_t last_ch; static int have_ch;
        if (!st->clut) key[2] = 0;
        else if (have_ch && !memcmp(last_clut, st->clut, 1024)) key[2] = last_ch;
        else { memcpy(last_clut, st->clut, 1024); last_ch = key[2] = fnv(14695981039346656037ull, st->clut, 1024); have_ch = 1; }
        key[3] = (uint64_t)st->clut_shift | ((uint64_t)st->clut_mask << 8) |
                 ((uint64_t)st->clut_start << 16) | ((uint64_t)st->clut_fmt << 24);
    }
    /* Content: the texel bytes, plus the CLUT for indexed formats. */
    uint32_t bytes = (uint32_t)st->tex_bw * (uint32_t)st->tex_h * (uint32_t)TEX_BPP[(unsigned)st->tex_fmt % 11u] / 8;
    if (st->tex_fmt >= PSP_TPSM_DXT1) bytes = (uint32_t)st->tex_w * (uint32_t)st->tex_h;
    int slot = -1, lru = 0;
    /* Hint table: a hit is one compare. The full scan (which also finds the
     * LRU victim) ran on every draw and was ~10% of the frame profile. */
    static int16_t hint[4096];
    const uint32_t hk = (uint32_t)(((key[0] ^ key[1] * 31 ^ key[2] * 131 ^ key[3] * 1031) * 0x9E3779B97F4A7C15ull) >> 52);
    { const int c = hint[hk];
      if (g_tex[c].tex && g_tex[c].key[0] == key[0] && g_tex[c].key[1] == key[1] &&
          g_tex[c].key[2] == key[2] && g_tex[c].key[3] == key[3]) slot = c; }
    if (slot < 0) for (int i = 0; i < MAX_TEX; i++) {
        if (g_tex[i].tex && !memcmp(g_tex[i].key, key, sizeof key)) { slot = i; break; }
        if (g_tex[i].used < g_tex[lru].used) lru = i;
    }
    hint[hk] = (int16_t)(slot >= 0 ? slot : lru);
    uint64_t h = 14695981039346656037ull;
    /* VRAM is the game's texture cache: it uploads into it between draws, so
     * a once-a-frame check served a stale texture (vehicle paint blinking). */
    {   /* PSPRECOMP_TEXWATCH=<hex addr>: per draw of that texture, its CLUT and texel hashes */
        static uint32_t wa = 1; if (wa == 1) { const char *e = getenv("PSPRECOMP_TEXWATCH"); wa = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (wa && st->tex_addr == wa) { const uint8_t *p = (const uint8_t *)psp_mem_ptr(st->tex_addr, bytes);
            fprintf(stderr, "texwatch f%llu clut %016llX k3 %llX tex %016llX c0 %08X\n", (unsigned long long)g_frame, (unsigned long long)key[2], (unsigned long long)key[3],
                    (unsigned long long)(p ? fnv(1, p, bytes) : 0), st->clut ? *(const uint32_t *)st->clut : 0);
            static uint64_t seen[16]; int k = 0; while (k < 16 && seen[k] && seen[k] != key[2]) k++;
            if (k < 16 && !seen[k] && st->clut) { seen[k] = key[2]; fprintf(stderr, "texwatch clut %016llX src %08X:", (unsigned long long)key[2], g_clut_src);
                for (int i = 0; i < 16; i++) fprintf(stderr, " %08X", ((const uint32_t *)st->clut)[i]); fprintf(stderr, "\n"); } }
    }
    static int always = -1; if (always < 0) always = getenv("PSPRECOMP_TEX_ALWAYS") != NULL;
    const int in_vram = always || (st->tex_addr >= PSP_VRAM_BASE && st->tex_addr < PSP_VRAM_BASE + PSP_VRAM_SIZE);
    if (slot >= 0 && g_tex[slot].frame == frame && !in_vram) { g_tex[slot].used = ++g_tick; g_last_tex = &g_tex[slot]; return g_tex[slot].tex; }
    const uint8_t *p = (const uint8_t *)psp_mem_ptr(st->tex_addr, bytes);
    if (p) h = fnv(h, p, bytes);
    if (st->tex_levels > 0) h = fnv(h, (const uint8_t *)st->mip_addr, sizeof st->mip_addr) ^ (uint64_t)st->tex_levels;
    if (st->tex_fmt >= PSP_TPSM_T4 && st->tex_fmt <= PSP_TPSM_T32 && st->clut) {
        h = fnv(h, (const uint8_t *)&key[2], sizeof key[2]);   /* the CLUT's hash, from above */
        uint32_t m[4] = { (uint32_t)st->clut_fmt, (uint32_t)st->clut_shift, (uint32_t)st->clut_mask, (uint32_t)st->clut_start };
        h = fnv(h, (const uint8_t *)m, sizeof m);
    }
    if (slot >= 0 && g_tex[slot].hash == h) {
        g_tex[slot].frame = frame; g_tex[slot].used = ++g_tick; g_last_tex = &g_tex[slot]; return g_tex[slot].tex;
    }
    if (slot < 0) slot = lru;
    gl_tex *t = &g_tex[slot];
    if (!t->tex) glGenTextures(1, &t->tex);
    static uint32_t px[1024 * 1024];
    const int w = st->tex_w > 1024 ? 1024 : st->tex_w, hh = st->tex_h > 1024 ? 1024 : st->tex_h;
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < w; x++) px[y * w + x] = psp_tex_fetch(st, x, y);
    {   /* PSPRECOMP_TEX_DUMP=1: write each newly decoded texture as tex_<addr>_<fmt>.ppm (first 200) */
        static int nd2 = -1; if (nd2 < 0) nd2 = getenv("PSPRECOMP_TEX_DUMP") ? 0 : 1 << 30;
        static uint32_t dlo = 1; if (dlo == 1) { const char *e = getenv("PSPRECOMP_TEX_DUMP"); dlo = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }   /* value >1: dump only textures at/above that address */
        if ((dlo <= 1 || (st->tex_addr >= dlo && st->tex_addr < dlo + 0x100000u)) && nd2++ < 200) {
            char fn[64]; snprintf(fn, sizeof fn, "tex_%08X_%d_%d.ppm", st->tex_addr, st->tex_fmt, nd2);
            FILE *tf = fopen(fn, "wb");
            if (tf) { fprintf(tf, "P6 %d %d 255\n", w, hh);
                for (int i = 0; i < w * hh; i++) { uint8_t c[3] = { (uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16) }; fwrite(c, 1, 3, tf); }
                fclose(tf); fn[strlen(fn) - 3] = 'p'; fn[strlen(fn) - 2] = 'g'; fn[strlen(fn) - 1] = 'm'; tf = fopen(fn, "wb"); if (tf) { fprintf(tf, "P5 %d %d 255\n", w, hh); for (int i = 0; i < w * hh; i++) fputc((int)(px[i] >> 24), tf); fclose(tf); } { unsigned long as = 0; for (int i = 0; i < w * hh; i++) as += px[i] >> 24; fprintf(stderr, "texdump alpha avg %lu\n", as / (unsigned long)(w * hh)); } fprintf(stderr, "texdump %s sw%d bw%d clutfmt%d shift%d mask%X start%d\n", fn, st->tex_swizzle, st->tex_bw, st->clut_fmt, st->clut_shift, st->clut_mask, st->clut_start); }
        }
    }
    glBindTexture(GL_TEXTURE_2D, t->tex);
    {   /* game layer override: it may hand back a replacement, any size (UVs
         * are normalised by the PSP size, so a bigger image is just sharper) */
        int ow = 0, oh = 0;
        psp_mods_dump_texture(h, px, w, hh);
        const uint32_t *o = psp_mods_texture(h, &ow, &oh);   /* a mod's replacement wins */
        if (!o) o = psp_tex_post ? psp_tex_post(px, w, hh, &ow, &oh) : NULL;
        if (o) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, o);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, hh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        /* The game's own mip levels (TMODE, TBPn/TBWn/TSIZEn), when they form
         * a proper chain; otherwise GL builds one if the filter asks for it. */
        int nl = 0;
        if (!o && st->tex_levels > 0 && w == st->tex_w && hh == st->tex_h) {
            for (int l = 1; l <= st->tex_levels && l < 8; l++) {
                const int lw = w >> l ? w >> l : 1, lh = hh >> l ? hh >> l : 1;
                if (st->mip_w[l] != lw || st->mip_h[l] != lh || !st->mip_bw[l] ||
                    !psp_mem_ptr(st->mip_addr[l], (uint32_t)st->mip_bw[l] * (uint32_t)lh * (uint32_t)TEX_BPP[(unsigned)st->tex_fmt % 11u] / 8)) break;
                psp_draw_state ls = *st;
                ls.tex_addr = st->mip_addr[l]; ls.tex_bw = st->mip_bw[l]; ls.tex_w = lw; ls.tex_h = lh;
                for (int y = 0; y < lh; y++)
                    for (int x = 0; x < lw; x++) px[y * lw + x] = psp_tex_fetch(&ls, x, y);
                glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                nl = l;
            }
        }
        glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, nl ? nl : 1000);
        t->mip = nl ? 2 : 0; }
    memcpy(t->key, key, sizeof key); t->hash = h; t->frame = frame; t->used = ++g_tick; t->w = w; t->h = hh;
    g_last_tex = t;
    return t->tex;
}

/* ---- blend --------------------------------------------------------------- */

static GLenum blend_factor(int code, int is_src, uint32_t fix, int *uses_const) {
    switch (code) {
    case 0:  return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1:  return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: case 6: return GL_SRC_ALPHA;            /* 6, 8: doubled alpha, approximated */
    case 3: case 7: return GL_ONE_MINUS_SRC_ALPHA;
    case 4: case 8: return GL_DST_ALPHA;
    case 5: case 9: return GL_ONE_MINUS_DST_ALPHA;
    default:
        if ((fix & 0xFFFFFF) == 0) return GL_ZERO;
        if ((fix & 0xFFFFFF) == 0xFFFFFF) return GL_ONE;
        *uses_const = 1;
        return GL_CONSTANT_COLOR;
    }
}


/* ---- sharp glow ------------------------------------------------------------
 * The game's glow: the scene shrunk to 120 x 120, each colour channel of it
 * looked up in one palette (CLUT32, shift 0 / 8 / 16) and summed, then laid
 * over the scene at FIX src / FIX dst (about 24 %). Stretched back up 4 x, that
 * quarter-size copy reads as a smeared double image. Here the composite is
 * done instead from the full-resolution scene, with the same palette and
 * blend, in one pass. */
static const char *const GLOW_VS =
    "#version 330 core\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *const GLOW_FS =
    "#version 330 core\n"
    "uniform sampler2D scene, clut; uniform vec3 amt; uniform float lod; out vec4 o;\n"
    "vec3 pal(float v) { return texelFetch(clut, ivec2(int(v * 255.0 + 0.5), 0), 0).rgb; }\n"
    /* The colour the game's chain actually gives (fitted from its glow buffer
     * against its source in one frame): red = green, blue about 1.8 x. */
    "vec3 g(vec2 p) { ivec2 q = clamp(ivec2(p), ivec2(0), textureSize(scene, 0) - 1); vec3 c = texelFetch(scene, q, 0).rgb;\n"
    "  float l = dot(c, vec3(0.3224, 0.3083, 0.2657)); return min(vec3(l, l, dot(c, vec3(0.5258, 0.5929, 0.4542))), vec3(1.0)); }\n"
    /* lod = PSP pixel size: the palette per sample, then the 4 x stretch's
     * bilinear tent (taps 1 and 3 px out, weights 3/4 and 1/4), centred */
    "void main() { vec3 s = vec3(0.0);\n"
    "  if (lod > 0.0) { const float d[4] = float[](-3.0, -1.0, 1.0, 3.0), w[4] = float[](0.25, 0.75, 0.75, 0.25);\n"
    "    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) s += w[i] * w[j] * g(gl_FragCoord.xy + vec2(d[i], d[j]) * lod);\n"
    "    s *= 0.25; } else s = g(gl_FragCoord.xy);\n"
    "  o = vec4(s * amt, 1.0); }\n";
static uint8_t g_glow_clut[1024];
static uint32_t g_glow_buf;            /* the glow chain's last target: the composite samples it */
static uint64_t g_glow_done;           /* frame the sharp composite ran */
static void glow_sharp(const psp_draw_state *st) {
    static GLuint prog, vao, scene, clut; static GLint u_amt, u_lod; static int sw, sh;
    if (!prog) {
        prog = glCreateProgram();
        glAttachShader(prog, compile(GL_VERTEX_SHADER, GLOW_VS));
        glAttachShader(prog, compile(GL_FRAGMENT_SHADER, GLOW_FS));
        glLinkProgram(prog); glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "scene"), 0); glUniform1i(glGetUniformLocation(prog, "clut"), 1);
        u_amt = glGetUniformLocation(prog, "amt"); u_lod = glGetUniformLocation(prog, "lod");
        glGenVertexArrays(1, &vao); glGenTextures(1, &scene); glGenTextures(1, &clut);
    }
    const int w = 480 * g_scale, h = 272 * g_scale;
    glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, clut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_glow_clut);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, scene);
    if (sw != w || sh != h) { sw = w; sh = h; glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glUseProgram(prog); glBindVertexArray(vao);
    glUniform1f(u_lod, psp_settings()->filter_blur ? (float)g_scale : 0.0f);
    glViewport(0, 0, w, h); glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glColorMask(1, 1, 1, 0);
    const uint32_t a = st->blend_sfix, d = st->blend_dfix;
    glUniform3f(u_amt, (a & 0xFF) / 255.0f, ((a >> 8) & 0xFF) / 255.0f, ((a >> 16) & 0xFF) / 255.0f);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);
    glBlendColor((d & 0xFF) / 255.0f, ((d >> 8) & 0xFF) / 255.0f, ((d >> 16) & 0xFF) / 255.0f, 1.0f);
    glBlendFuncSeparate(GL_ONE, GL_CONSTANT_COLOR, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(g_prog); glBindVertexArray(g_vao); glColorMask(1, 1, 1, 1);
    g_sv = 0; g_tex_bound = 0;
}

static void apply_state(const psp_draw_state *st) {
    const int sw = g_cur->w * g_scale, sh = g_cur->h * g_scale;
    g_tex_bound = 0;
    glViewport(0, 0, sw, sh);
    glEnable(0x864F);   /* GL_DEPTH_CLAMP: the GE clamps depth, it never clips on it */
    glEnable(GL_SCISSOR_TEST);
    glScissor(st->sc_x0 * g_scale, st->sc_y0 * g_scale,
              (st->sc_x1 - st->sc_x0 + 1) * g_scale, (st->sc_y1 - st->sc_y0 + 1) * g_scale);
    glUniform1i(u_flat, !st->gouraud);
    glUniform1i(u_clear, st->clear);
    if (st->clear) {
        glDisable(GL_BLEND);
        glColorMask(st->clear_color, st->clear_color, st->clear_color, st->clear_alpha);
        if (st->clear_depth) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE); }
        else glDisable(GL_DEPTH_TEST);
        glUniform1i(u_tex_on, 0);
        glUniform1i(u_at_on, 0);
        glUniform1i(u_fog_on, 0); glUniform1i(u_ct_func, 1); glUniform1i(u_zr_on, 0);
        glUniform1i(u_fetch_on, 0); g_fetch = 0;
        return;
    }
    /* Stencil lives in the framebuffer's alpha. The fixed-function path
     * covers ALWAYS/NEVER with KEEP/ZERO/REPLACE on depth pass (the alpha
     * written becomes the stencil value); everything else is the
     * framebuffer-read path below. */
    int aw = 1, aconst = -1, never = 0;
    if (st->stencil) {
        if (st->st_func == 0) never = 1;
        else switch (st->sop_zpass) {
            case 1:  aconst = 0; break;
            case 2:  aconst = st->st_ref; break;
            default: aw = 0; break;
        }
    }
    /* PMSKC/PMSKA: a channel is masked when all its bits are (partial masks unsupported) */
    if (st->pmask_a == 0xFF) aw = 0;
    if (never) glColorMask(0, 0, 0, 0);
    else glColorMask((st->pmask_rgb & 0xFF) != 0xFF, ((st->pmask_rgb >> 8) & 0xFF) != 0xFF,
                     ((st->pmask_rgb >> 16) & 0xFF) != 0xFF, aw);
    glUniform1i(u_fog_on, st->fog && !g_through);
    if (st->fog && !g_through)
        glUniform3f(u_fog_col, (st->fog_color & 0xFF) / 255.0f, ((st->fog_color >> 8) & 0xFF) / 255.0f,
                    ((st->fog_color >> 16) & 0xFF) / 255.0f);
    glUniform1i(u_ct_func, st->ctest ? st->ct_func : 1);
    if (st->ctest) {
        glUniform3i(u_ct_ref, (int)(st->ct_ref & 0xFF), (int)((st->ct_ref >> 8) & 0xFF), (int)((st->ct_ref >> 16) & 0xFF));
        glUniform3i(u_ct_mask, (int)(st->ct_mask & 0xFF), (int)((st->ct_mask >> 8) & 0xFF), (int)((st->ct_mask >> 16) & 0xFF));
    }
    {   const int zr_on = st->minz > 0 || st->maxz_inv > 0;
        glUniform1i(u_zr_on, zr_on);
        if (zr_on) glUniform2f(u_zr, (float)st->minz - 0.5f, (float)(0xFFFF - st->maxz_inv) + 0.5f); }
    if (st->ztest) {
        static const GLenum F[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };
        glEnable(GL_DEPTH_TEST);
        { static int zi = -1; if (zi < 0) zi = getenv("PSPRECOMP_GL_ZINV") != NULL;
          static const GLenum FI[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_GREATER, GL_GEQUAL, GL_LESS, GL_LEQUAL };
          glDepthFunc(zi ? FI[st->zfunc & 7] : F[st->zfunc & 7]); }
        glDepthMask(st->zwrite && !never ? GL_TRUE : GL_FALSE);
    } else glDisable(GL_DEPTH_TEST);

    glUniform3f(u_src_mul, 1.0f, 1.0f, 1.0f);
    if (st->blend) {
        int uc = 0;
        GLenum s = blend_factor(st->blend_src, 1, st->blend_sfix, &uc);
        int ucd = 0;
        GLenum d = blend_factor(st->blend_dst, 0, st->blend_dfix, &ucd);
        uint32_t fix = uc ? st->blend_sfix : st->blend_dfix;
        if (uc && ucd && st->blend_sfix != st->blend_dfix) {
            /* One constant colour in GL: DFIX takes it, and SFIX is applied
             * by the shader to the source colour before blending. */
            s = GL_ONE; fix = st->blend_dfix;
            glUniform3f(u_src_mul, (st->blend_sfix & 0xFF) / 255.0f, ((st->blend_sfix >> 8) & 0xFF) / 255.0f,
                        ((st->blend_sfix >> 16) & 0xFF) / 255.0f);
        }
        if (uc || ucd || aconst >= 0)
            glBlendColor((fix & 0xFF) / 255.0f, ((fix >> 8) & 0xFF) / 255.0f, ((fix >> 16) & 0xFF) / 255.0f,
                         aconst >= 0 ? aconst / 255.0f : 1.0f);
        static const GLenum OP[6] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD };
        glEnable(GL_BLEND);
        const GLenum op = OP[st->blend_op < 6 ? st->blend_op : 0];
        /* alpha: source, as the software backend, or the stencil value */
        if (aconst >= 0 && (op == GL_MIN || op == GL_MAX)) glBlendEquationSeparate(op, GL_FUNC_ADD);
        else glBlendEquation(op);
        glBlendFuncSeparate(s, d, aconst >= 0 ? 0x8003 /* GL_CONSTANT_ALPHA */ : GL_ONE, GL_ZERO);
    } else if (aconst >= 0) {     /* unblended, but the alpha written is the stencil value */
        glEnable(GL_BLEND);
        glBlendColor(0, 0, 0, aconst / 255.0f);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ZERO, 0x8003, GL_ZERO);
    } else glDisable(GL_BLEND);

    /* What GL's fixed function cannot do exactly goes through the shader's
     * framebuffer-read path (fetch_out): stencil tests other than
     * ALWAYS/NEVER, INVERT/INCR/DECR and fail ops, logic ops, partial bit
     * masks, dithering into 16-bit targets, doubled-alpha factors and the
     * absolute-difference blend. PSPRECOMP_GL_NOFETCH=1 turns it off. */
    {
        static int nf = -1; if (nf < 0) nf = getenv("PSPRECOMP_GL_NOFETCH") != NULL;
        static int ff = -1; if (ff < 0) ff = getenv("PSPRECOMP_GL_FORCEFETCH") != NULL;   /* test: every draw through it */
        /* 16-bit targets: the GE quantises every write (and its blend reads the
         * quantised pixel); PSPRECOMP_GL_FB16=0 keeps 8-bit precision instead */
        static int fb16 = -1; if (fb16 < 0) { const char *e = getenv("PSPRECOMP_GL_FB16"); fb16 = !e || *e != '0'; }
        const uint32_t pm = st->pmask_rgb;
        int partial = st->pmask_a != 0 && st->pmask_a != 0xFF;
        for (int k = 0; k < 24; k += 8) { const uint32_t b = (pm >> k) & 0xFF; partial |= b != 0 && b != 0xFF; }
        const int dbl = st->blend && ((st->blend_src >= 6 && st->blend_src <= 9) || (st->blend_dst >= 6 && st->blend_dst <= 9) || st->blend_op == 5);
        g_fetch = !nf && (ff || (st->stencil && st->st_func >= 2) || (st->stencil && (st->sop_zpass >= 3 || st->sop_fail != 0 || st->sop_zfail != 0)) || (fb16 && g_tgt_fmt != 3) ||
                          (st->logic && st->logic_op != 3) || partial || (st->dither && g_tgt_fmt != 3) || dbl);
        glUniform1i(u_fetch_on, g_fetch);
        if (g_fetch) {
            static int seen; if (!seen) { seen = 1; fprintf(stderr, "gl: framebuffer-read path in use (stencil %d/%d/%d logic %d/%d mask %06X/%02X dither %d blend %d/%d/%d)\n",
                st->stencil, st->st_func, st->sop_zpass, st->logic, st->logic_op, st->pmask_rgb, st->pmask_a, st->dither, st->blend_src, st->blend_dst, st->blend_op); }
            glDisable(GL_BLEND);
            glColorMask(1, 1, 1, 1);   /* the shader discards what the stencil test fails */
            if (st->ztest) glDepthMask(st->zwrite ? GL_TRUE : GL_FALSE);
            glUniform1i(u_b_on, st->blend); glUniform1i(u_b_src, st->blend_src); glUniform1i(u_b_dst, st->blend_dst); glUniform1i(u_b_op, st->blend_op);
            glUniform3f(u_b_sfix, (st->blend_sfix & 0xFF) / 255.0f, ((st->blend_sfix >> 8) & 0xFF) / 255.0f, ((st->blend_sfix >> 16) & 0xFF) / 255.0f);
            glUniform3f(u_b_dfix, (st->blend_dfix & 0xFF) / 255.0f, ((st->blend_dfix >> 8) & 0xFF) / 255.0f, ((st->blend_dfix >> 16) & 0xFF) / 255.0f);
            glUniform1i(u_st_on, st->stencil); glUniform1i(u_st_func, st->st_func); glUniform1i(u_st_ref, st->st_ref);
            glUniform1i(u_st_mask, st->st_mask); glUniform1i(u_sop_fail, st->sop_fail); glUniform1i(u_sop_zpass, st->sop_zpass);
            glUniform1i(u_sop_zfail, st->sop_zfail); glUniform1i(u_st_pass, 0);
            glUniform1i(u_lop, st->logic ? st->logic_op : -1);
            glUniform1i(u_fbfmt, g_tgt_fmt);
            glUniform1i(u_dith_on, st->dither && g_tgt_fmt != 3);
            { float dm[16]; for (int k = 0; k < 16; k++) dm[k] = (float)st->dith[k]; glUniform1fv(u_dith, 16, dm); }
            glUniform1f(u_scale, (float)g_scale);
            glUniform4i(u_pmask, (int)(pm & 0xFF), (int)((pm >> 8) & 0xFF), (int)((pm >> 16) & 0xFF), st->pmask_a);
            glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, g_cur->tex); glActiveTexture(GL_TEXTURE0);
        }
    }

    /* PSPRECOMP_GL_NO=<bits>: debug kill-switches. 1 alpha test, 2 blend, 4 depth, 8 texture. */
    static int no = -1;
    if (no < 0) { const char *e = getenv("PSPRECOMP_GL_NO"); no = e ? atoi(e) : 0; }
    if (no & 2) glDisable(GL_BLEND);
    if (no & 4) glDisable(GL_DEPTH_TEST);
    glUniform1i(u_at_on, (no & 1) ? 0 : st->atest);
    glUniform1i(u_at_func, st->atest_func);
    glUniform1i(u_at_ref, st->atest_ref);
    glUniform1i(u_at_mask, st->atest_mask);

    glUniform1i(u_tex_on, (no & 8) ? 0 : st->tex_enable);
    if (st->tex_enable && !(no & 8)) {
        glUniform1i(u_func, st->tex_func);
        glUniform1i(u_tex_alpha, st->tex_alpha);
        glUniform1i(u_dbl, st->tex_double);
        glUniform3f(u_env, (st->tex_env & 0xFF) / 255.0f, ((st->tex_env >> 8) & 0xFF) / 255.0f,
                    ((st->tex_env >> 16) & 0xFF) / 255.0f);
        glActiveTexture(GL_TEXTURE0);
        gl_fb *src = (st->tex_addr >= PSP_VRAM_BASE && st->tex_addr < PSP_VRAM_BASE + PSP_VRAM_SIZE)
                     ? find_fb(st->tex_addr) : NULL;
        /* An indexed format over a render target is a palette trick (the bloom
         * chain reads its buffer as CLUT32 to remap colours): the GL texture
         * holds colours, not indices, so put the pixels back in VRAM in the
         * buffer's own format and decode them like any other texture. */
        if (src && src != g_cur && st->tex_fmt >= PSP_TPSM_T4 && st->tex_fmt <= PSP_TPSM_T32) {
            fb_to_vram(src, st->tex_h);
            if (st->clut && st->clut_fmt == 3 && st->tex_fmt == PSP_TPSM_T32) { memcpy(g_glow_clut, st->clut, sizeof g_glow_clut); g_glow_buf = g_cur->addr; }
            if (g_tr_live && st->clut) { const uint32_t *c = (const uint32_t *)st->clut;
                fprintf(stderr, "gl:    clut shift %d mask %X start %d fmt %d tfunc %d env %06X: %08X %08X %08X %08X %08X\n", st->clut_shift, st->clut_mask, st->clut_start, st->clut_fmt,
                        st->tex_func, st->tex_env, c[0], c[64], c[128], c[192], c[255]); }
            src = NULL;
        }
        if (src && src != g_cur) {
            glBindTexture(GL_TEXTURE_2D, src->tex);
            glUniform2f(u_tex_size, (float)src->w, (float)src->h);
        } else {
            glBindTexture(GL_TEXTURE_2D, get_texture(st));
            g_tex_bound = 1;
            glUniform2f(u_tex_size, (float)st->tex_w, (float)st->tex_h);
        }
        const GLint wu = st->tex_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        const GLint wv = st->tex_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wu);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wv);
        /* The GE's own TFLT choice: HUD art is drawn unfiltered, and linear
         * there bleeds the neighbouring atlas row in along sprite edges. */
        const int lin = !(src && src != g_cur);
        GLint fmin = !lin || st->tex_min_linear ? GL_LINEAR : GL_NEAREST;
        GLint fmag = !lin || st->tex_mag_linear ? GL_LINEAR : GL_NEAREST;
        float aniso = 1.0f;
        /* The game's mip levels, sampled as TFLT asks. TEXLEVEL's slope mode
         * is approximated by GL's own LOD, plus the bias. */
        const int real_mip = lin && g_tex_bound && g_last_tex && g_last_tex->mip == 2;
        float lod_bias = 0.0f, lod_min = -1000.0f, lod_max = 1000.0f;
        int slope = 0;
        if (real_mip && st->tex_min_mip) {
            static const GLint MM[4] = { 0x2700, 0x2701, 0x2702, 0x2703 };   /* {NEAREST,LINEAR}_MIPMAP_{NEAREST,LINEAR} */
            fmin = MM[(st->tex_min_linear ? 1 : 0) | (st->tex_min_mip == 2 ? 2 : 0)];
            if (st->tex_lod_mode == 1) lod_min = lod_max = st->tex_lod_bias / 16.0f;
            else if (st->tex_lod_mode == 0) lod_bias = st->tex_lod_bias / 16.0f;
            else slope = 1;
        }
        /* PC "Texture filtering" option, for world textures only: screen-space
         * draws (HUD, menus) and render targets keep the game's own choice. */
        const int tf = psp_settings()->tex_filter, uf = psp_settings()->ui_filter;
        if (lin && g_tex_bound && g_through && uf)      /* "UI filtering": 1 sharp, 2 smooth */
            fmin = fmag = uf == 1 ? GL_NEAREST : GL_LINEAR;
        if (lin && g_tex_bound && !g_through && tf) {
            if (tf == 1) fmin = fmag = GL_NEAREST;
            else if (tf == 2) fmin = fmag = GL_LINEAR;
            else {
                if (!g_last_tex->mip) { glGenerateMipmap(GL_TEXTURE_2D); g_last_tex->mip = 1; }
                lod_bias = 0.0f; lod_min = -1000.0f; lod_max = 1000.0f; slope = 0;
                fmin = 0x2703 /* GL_LINEAR_MIPMAP_LINEAR */; fmag = GL_LINEAR; aniso = 16.0f;
            }
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fmin);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fmag);
        glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, aniso);
        if (tf == 1 || tf == 2) slope = 0;
        glUniform1i(u_lod_mode, slope);
        if (slope) { glUniform1f(u_lod_slope, st->tex_slope); glUniform1f(u_lod_bias, st->tex_lod_bias / 16.0f); }
        if (g_tex_bound) {
            glTexParameterf(GL_TEXTURE_2D, 0x8501 /* GL_TEXTURE_LOD_BIAS */, lod_bias);
            glTexParameterf(GL_TEXTURE_2D, 0x813A /* GL_TEXTURE_MIN_LOD */, lod_min);
            glTexParameterf(GL_TEXTURE_2D, 0x813B /* GL_TEXTURE_MAX_LOD */, lod_max);
        }
    }
}

/* ---- the backend --------------------------------------------------------- */

static gl_vtx g_out[65536 * 3];
static int g_culling;

static __forceinline void put(gl_vtx *o, const psp_vertex *v, float u, float vv, uint32_t rgba) {
    const float sx = 2.0f / (float)g_cur->w, sy = 2.0f / (float)g_cur->h;
    const float w = v->w > 0 ? v->w : 1.0f;
    const float px = v->has_f ? v->fx : (float)v->x, py = v->has_f ? v->fy : (float)v->y;
    /* The GE clamps each vertex's depth to 0..65535 before interpolating it;
     * an unclamped far vertex would drag the depth of the whole triangle. */
    /* Whole units, as the GE's 16-bit screen z (PPSSPP's VertexDepthRounding):
     * near-coplanar faces then tie the same way every frame instead of flickering. */
    static int zr = -1; if (zr < 0) zr = getenv("PSPRECOMP_ZROUND") != NULL;   /* off: it made the intro ships flicker */
    const float zc = v->z < 0.0f ? 0.0f : v->z > 65535.0f ? 65535.0f : zr ? floorf(v->z + 0.5f) : v->z;
    o->x = ((px + 0.5f) * sx - 1.0f) * w; o->y = ((py + 0.5f) * sy - 1.0f) * w;
    o->z = (zc * (2.0f / 65535.0f) - 1.0f) * w; o->w = w;
    o->u = u; o->v = vv; o->rgba = rgba; o->fog = v->fog; o->q = v->q;
    o->r[0] = o->r[1] = -1e9f; o->r[2] = o->r[3] = 1e9f;
}

static int  gl_init(int w, int h) { (void)w; (void)h; return 0; }
static void gl_shutdown(void) { }

/* PSPRECOMP_GL_SHADOW=1: also run the software rasterizer into VRAM on the
 * same draw stream, so one frame can be diffed GL-vs-reference. */
static int shadow(void) {
    static int s = -1;
    if (s < 0) { const char *e = getenv("PSPRECOMP_GL_SHADOW"); s = e && *e == '1'; }
    return s;
}

static void gl_target(uint32_t addr, uint32_t stride, int fmt) {
    if (shadow()) psp_render_software.set_target(addr, stride, fmt);
    g_tgt_addr = addr; g_tgt_stride = stride; g_tgt_fmt = fmt;
    g_cur = NULL;                      /* bound lazily on the next draw */
}

static void gl_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    if (shadow()) psp_render_software.draw(prim, v, count, st);
    if (!acquire()) return;
    if (!g_cur || g_cur->addr != ((g_tgt_addr & (PSP_VRAM_SIZE - 1)) | PSP_VRAM_BASE)) {
        g_cur = get_fb(g_tgt_addr, (int)(g_tgt_stride ? g_tgt_stride : 512), g_tgt_fmt);
        glBindFramebuffer(GL_FRAMEBUFFER, g_cur->fbo);
    }
    if (g_tr_live) { float d0 = -1; GLint fbo = -1; glGetIntegerv(0x8CA6 /* FRAMEBUFFER_BINDING */, &fbo);
        glReadPixels(240 * g_scale, 136 * g_scale, 1, 1, 0x1902, 0x1406, &d0);
        fprintf(stderr, "gl:    pre fb %08X fbo %d/%u d %.5f thr %lu\n", g_cur->addr, fbo, g_cur->fbo, d0, (unsigned long)GetCurrentThreadId()); }
    int n = 0;
    GLenum mode = GL_TRIANGLES;
    switch (prim) {
    case PSP_PRIM_TRIANGLES:
        for (int i = 0; i + 2 < count; i += 3)
            for (int k = 0; k < 3; k++) { if (v[i + k].behind) goto skip3; }
        for (int i = 0; i < count; i++) put(&g_out[n++], &v[i], v[i].u, v[i].v, v[i].rgba);
        break;
    skip3:
        for (int i = 0; i + 2 < count; i += 3) {
            if (v[i].behind || v[i + 1].behind || v[i + 2].behind) continue;
            for (int k = 0; k < 3; k++) put(&g_out[n++], &v[i + k], v[i + k].u, v[i + k].v, v[i + k].rgba);
        }
        break;
    case PSP_PRIM_TRIANGLE_STRIP:
    case PSP_PRIM_TRIANGLE_FAN:
        /* Transformed and nothing behind the eye: draw it as a GL strip/fan,
         * each vertex sent once (GL's strip winding alternates like the
         * expansion below). Through mode keeps the expansion: its half-texel
         * clamp below works per triangle. */
        if (count >= 3 && v[0].has_f) {
            int any = 0;
            for (int i = 0; i < count; i++) any |= v[i].behind;
            if (!any) {
                for (int i = 0; i < count; i++) put(&g_out[n++], &v[i], v[i].u, v[i].v, v[i].rgba);
                mode = prim == PSP_PRIM_TRIANGLE_FAN ? GL_TRIANGLE_FAN : GL_TRIANGLE_STRIP;
                break;
            }
        }
        for (int i = 0; i + 2 < count; i++) {
            const psp_vertex *a, *b, *c;
            if (prim == PSP_PRIM_TRIANGLE_FAN) { a = &v[0]; b = &v[i + 1]; c = &v[i + 2]; }
            else if (i & 1) { a = &v[i + 1]; b = &v[i]; c = &v[i + 2]; }
            else            { a = &v[i]; b = &v[i + 1]; c = &v[i + 2]; }
            if (a->behind || b->behind || c->behind) continue;
            put(&g_out[n++], a, a->u, a->v, a->rgba);
            put(&g_out[n++], b, b->u, b->v, b->rgba);
            put(&g_out[n++], c, c->u, c->v, c->rgba);
        }
        break;
    case PSP_PRIM_SPRITES:
        for (int i = 0; i + 1 < count; i += 2) {
            const psp_vertex *p = &v[i], *q = &v[i + 1];
            if (p->behind || q->behind) continue;
            psp_vertex c[4] = { *p, *q, *p, *q };
            c[1].y = p->y; c[2].y = q->y;            /* (x1,y0) and (x0,y1) */
            c[1].z = c[2].z = q->z; c[0].z = q->z;
            float u0 = p->u, v0 = p->v, u1 = q->u, v1 = q->v;
            {   /* PSPRECOMP_UVLOG=<hex tex addr>: print each distinct sprite rect drawn from it */
                static uint32_t ua = 1; if (ua == 1) { const char *e = getenv("PSPRECOMP_UVLOG"); ua = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
                if (ua && st->tex_enable && st->tex_addr == ua) { static float seen[64][4]; static int ns; int k = 0;
                    for (; k < ns; k++) if (seen[k][0] == u0 && seen[k][1] == v0 && seen[k][2] == u1 && seen[k][3] == v1) break;
                    if (k == ns && ns < 64) { seen[ns][0] = u0; seen[ns][1] = v0; seen[ns][2] = u1; seen[ns][3] = v1; ns++;
                        fprintf(stderr, "uv %.2f %.2f %.2f %.2f at %d %d\n", u0, v0, u1, v1, (int)p->x, (int)p->y); } } }
            /* corners: 0 (x0,y0) 1 (x1,y0) 2 (x0,y1) 3 (x1,y1); colour is the second vertex's */
            const uint32_t col = psp_sprite_color && st->tex_enable ? psp_sprite_color(st->tex_w, st->tex_h, u0, v0, u1, v1, q->rgba) : q->rgba;
            put(&g_out[n++], &c[0], u0, v0, col);
            put(&g_out[n++], &c[1], u1, v0, col);
            put(&g_out[n++], &c[2], u0, v1, col);
            put(&g_out[n++], &c[1], u1, v0, col);
            put(&g_out[n++], &c[3], u1, v1, col);
            put(&g_out[n++], &c[2], u0, v1, col);
            /* Keep samples half a texel inside the sprite's own rectangle. At
             * native size the GE never reaches past it, but rendered upscaled
             * the edge pixels land outside and pull in the neighbouring atlas
             * entry (a bar over the HUD's rebel logo, from the icon below). */
            const float ru0 = (u0 < u1 ? u0 : u1) + 0.5f, ru1 = (u0 < u1 ? u1 : u0) - 0.5f;
            const float rv0 = (v0 < v1 ? v0 : v1) + 0.5f, rv1 = (v0 < v1 ? v1 : v0) - 0.5f;
            for (int k = n - 6; k < n; k++) {
                g_out[k].r[0] = ru0 <= ru1 ? ru0 : -1e9f; g_out[k].r[2] = ru0 <= ru1 ? ru1 : 1e9f;
                g_out[k].r[1] = rv0 <= rv1 ? rv0 : -1e9f; g_out[k].r[3] = rv0 <= rv1 ? rv1 : 1e9f;
            }
        }
        break;
    case PSP_PRIM_LINES: case PSP_PRIM_LINE_STRIP: case PSP_PRIM_POINTS:
        mode = prim == PSP_PRIM_POINTS ? GL_POINTS : prim == PSP_PRIM_LINES ? GL_LINES : GL_LINE_STRIP;
        for (int i = 0; i < count; i++) put(&g_out[n++], &v[i], v[i].u, v[i].v, v[i].rgba);
        break;
    }
    /* Through-mode (screen-space) textured triangles get the same half-texel
     * clamp as sprites. Upscaled, their edge pixels sample past the art: the
     * HUD score panel's mirrored right half (u 192 -> 0) pulled in the
     * transparent texel 192 and left a pale line down the screen's centre. */
    if (mode == GL_TRIANGLES && prim != PSP_PRIM_SPRITES && st->tex_enable && count && !v[0].has_f)
        for (int t = 0; t + 2 < n; t += 3) {
            float u0 = g_out[t].u, u1 = u0, v0 = g_out[t].v, v1 = v0;
            for (int k = 1; k < 3; k++) {
                const float u = g_out[t + k].u, vv = g_out[t + k].v;
                if (u < u0) u0 = u;
                if (u > u1) u1 = u;
                if (vv < v0) v0 = vv;
                if (vv > v1) v1 = vv;
            }
            const float ru0 = u0 + 0.5f, ru1 = u1 - 0.5f, rv0 = v0 + 0.5f, rv1 = v1 - 0.5f;
            for (int k = t; k < t + 3; k++) {
                if (ru0 <= ru1) { g_out[k].r[0] = ru0; g_out[k].r[2] = ru1; }
                if (rv0 <= rv1) { g_out[k].r[1] = rv0; g_out[k].r[3] = rv1; }
            }
        }
    /* PSPRECOMP_GL_SKIPPOST=1: drop draws that sample a VRAM render target (post-effect test). */
    { if (g_skippost < 0) g_skippost = getenv("PSPRECOMP_GL_SKIPPOST") != NULL;
      if (g_skippost > 0 && st->tex_enable && (st->tex_addr >= 0x04000000u && st->tex_addr < 0x04170000u)) n = 0; }
    /* Color grade: the game's glow composite (the glow chain's buffer laid
     * over the scene with fixed blend factors) is redone at full resolution,
     * or dropped when off. Only that draw: the VRAM texture cache shares the
     * range, so a range test dropped real textures (soldiers' paint). */
    if (n && g_glow_buf && st->tex_enable && st->tex_addr == g_glow_buf && g_cur->addr != g_glow_buf &&
        st->blend && st->blend_src == 10 && st->blend_dst == 10) {
        if (psp_settings()->filter && g_glow_done != g_frame) { g_glow_done = g_frame; glow_sharp(st); }
        n = 0;
    }
    if (g_maxdraw == -2) { const char *m = getenv("PSPRECOMP_GL_MAXDRAW"); g_maxdraw = m ? atoi(m) : -1; }
    if (n && g_maxdraw >= 0 && ++g_fdraw > g_maxdraw) n = 0;
    if (n) {
        g_through = count && !v[0].has_f;
        const int vram_tex = st->tex_enable && st->tex_addr >= PSP_VRAM_BASE && st->tex_addr < PSP_VRAM_BASE + PSP_VRAM_SIZE;
        /* A palette reload keeps the state's CLUT pointer, so it is compared by
         * generation: two tanks drawn back to back with different team palettes
         * shared the first one's texture (paint blinking grey). */
        static uint32_t sv_clut_gen;
        const int same = g_sv && !vram_tex && g_sv_fb == g_cur && g_sv_through == g_through && !memcmp(st, &g_sv_st, sizeof *st) &&
                         (!st->tex_enable || st->tex_fmt < PSP_TPSM_T4 || st->tex_fmt > PSP_TPSM_T32 || sv_clut_gen == g_clut_gen);
        sv_clut_gen = g_clut_gen;
        {   /* PSPRECOMP_GL_DRAWSTATS=1: how many draws repeat the previous state */
            static long nd_all, nd_hit, nv; static int lg = -1; if (lg < 0) lg = getenv("PSPRECOMP_GL_DRAWSTATS") != NULL;
            if (lg) { nd_all++; nv += n; nd_hit += same;
                if (nd_all % 20000 == 0) fprintf(stderr, "gl: draws %ld same-state %ld verts/draw %.1f\n", nd_all, nd_hit, (double)nv / nd_all); }
        }
        if (!same) {
            apply_state(st);
            glBindVertexArray(g_vao);
            g_sv = !vram_tex; g_sv_st = *st; g_sv_fb = g_cur; g_sv_through = g_through;
        }
        {   /* Back-face culling (BCE/CULL). Rectangles are never culled. Drawn
             * unculled, a model's back faces fight its front ones along folds
             * (the tank's gun boxes against the hull). The framebuffer is
             * y-flipped, which mirrors the winding. PSPRECOMP_CULL=0 off, 2 flip. */
            static int cm = -1; if (cm < 0) { const char *e = getenv("PSPRECOMP_CULL"); cm = e ? atoi(e) : 1; }
            const int on = cm && st->cull && !st->clear && prim != PSP_PRIM_SPRITES && prim >= PSP_PRIM_TRIANGLES;
            static int last_dir = -1;
            if (on) {
                glEnable(GL_CULL_FACE);
                const int dir = st->cull_dir ^ (cm != 2);
                if (dir != last_dir) { glCullFace(dir ? GL_FRONT : GL_BACK); last_dir = dir; }
            }
            g_culling = on;
        }
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * (int)sizeof(gl_vtx)), g_out, GL_STREAM_DRAW);
        if (g_fetch && st->stencil && (st->sop_zfail || st->sop_fail)) {
            /* The stencil ops for fragments GL would drop or that must not
             * write depth, as passes of their own before the draw proper
             * (fetch_out's st_pass): 1 = failing depth (inverted test), 2 =
             * passing depth but failing stencil. Neither writes depth. */
            static const GLenum F[8]  = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };
            static const GLenum FI[8] = { GL_ALWAYS, GL_NEVER, GL_NOTEQUAL, GL_EQUAL, GL_GEQUAL, GL_GREATER, GL_LEQUAL, GL_LESS };
            glDepthMask(GL_FALSE);
            if (st->ztest) {
                glDepthFunc(FI[st->zfunc & 7]); glUniform1i(u_st_pass, 1);
                if (glTextureBarrier) glTextureBarrier();
                glDrawArrays(mode, 0, n);
                glDepthFunc(F[st->zfunc & 7]);
            }
            if (st->sop_fail) {
                glUniform1i(u_st_pass, 2);
                if (glTextureBarrier) glTextureBarrier();
                glDrawArrays(mode, 0, n);
            }
            glUniform1i(u_st_pass, 0);
            glDepthMask(st->ztest && st->zwrite ? GL_TRUE : GL_FALSE);
        }
        if (g_fetch && glTextureBarrier) glTextureBarrier();   /* earlier draws visible to fetch_out */
        glDrawArrays(mode, 0, n);
        if (g_culling) { glDisable(GL_CULL_FACE); g_culling = 0; }   /* the other passes draw unculled */
        if (st->clear && st->clear_depth && getenv("PSPRECOMP_GL_DEPTHDBG")) { static int nc; if (nc++ < 6) { float dz = -1; glReadPixels(240 * g_scale, 136 * g_scale, 1, 1, 0x1902, 0x1406, &dz); GLboolean dm; glGetBooleanv(0x0B72, &dm); fprintf(stderr, "gl: after depth clear fb %08X: %.4f mask %d test %d n %d z0 %.3f w %.3f\n", g_cur->addr, dz, dm, glIsEnabled(GL_DEPTH_TEST), n, g_out[0].z, g_out[0].w);
            glClearDepth(0.75); glClear(0x100); float d2 = -1; glReadPixels(240 * g_scale, 136 * g_scale, 1, 1, 0x1902, 0x1406, &d2);
            GLint bits = -1; glGetIntegerv(0x0D56 /* GL_DEPTH_BITS */, &bits);
            for (int k = 0; k < 12 && k < n; k++) fprintf(stderr, "gl:   v%d %.2f %.2f %.3f %.3f\n", k, g_out[k].x, g_out[k].y, g_out[k].z, g_out[k].w);
            fprintf(stderr, "gl:   glClear(0.75) reads %.4f, depth bits %d, err %04X\n", d2, bits, glGetError()); } }
        if (g_tr_live) { uint32_t pa = 0, pb = 0; glReadPixels(240 * g_scale, 136 * g_scale, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pa);
            static int prx = -1, pry = 220; if (prx < 0) { const char *e = getenv("PSPRECOMP_GL_PROBE"); prx = 240; if (e) sscanf(e, "%d,%d", &prx, &pry); }
            glReadPixels(prx * g_scale, pry * g_scale, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pb);
            float da = -1, db = -1; glReadPixels(240 * g_scale, 136 * g_scale, 1, 1, 0x1902, 0x1406, &da); glReadPixels(prx * g_scale, pry * g_scale, 1, 1, 0x1902, 0x1406, &db);
            fprintf(stderr, "gl:    px fb %08X centre %08X low %08X d %.5f %.5f%s", g_cur->addr, pa, pb, da, db, "\n"); }
        if (g_tr_live && (n <= 12 || n >= 200 || prim == PSP_PRIM_SPRITES)) for (int k = 0; k < n; k++)
            fprintf(stderr, "gl:    v%d ndc %.3f %.3f %.3f w %.4f c%08X uv %.1f %.1f r %.1f %.1f %.1f %.1f%s", k, g_out[k].x / g_out[k].w, g_out[k].y / g_out[k].w, g_out[k].z / g_out[k].w, g_out[k].w, g_out[k].rgba, g_out[k].u, g_out[k].v, g_out[k].r[0], g_out[k].r[1], g_out[k].r[2], g_out[k].r[3], "\n");
        static long tr0 = -2; if (tr0 == -2) { const char *ev = getenv("PSPRECOMP_GL_TRACE"); tr0 = ev ? atol(ev) : -1; }
        GLenum e = (tr0 >= 0 || g_tr_live) ? glGetError() : 0;   /* a per-draw driver round trip otherwise */
        ++nd;
        static int ntr;
        if (e || g_tr_live || (tr0 >= 0 && nd >= tr0 && (st->ztest || st->clear || getenv("PSPRECOMP_GL_ALL")) && ntr++ < 600))
            fprintf(stderr, "gl: draw %d prim %d n %d fb %08X clr %d/%d%d%d tex %d fmt %d %dx%d @%08X fn %d a%d blend %d %d/%d op%d fix %06X/%06X at %d f%d r%d z %d f%d w%d sc %d,%d-%d,%d v0 (%.1f %.1f %.3f w%.3f c%08X uv %.1f %.1f) err %04X\n",
                    nd, prim, n, g_cur->addr, st->clear, st->clear_color, st->clear_alpha, st->clear_depth,
                    st->tex_enable, st->tex_fmt, st->tex_w, st->tex_h, st->tex_addr, st->tex_func, st->tex_alpha,
                    st->blend, st->blend_src, st->blend_dst, st->blend_op, st->blend_sfix & 0xFFFFFF, st->blend_dfix & 0xFFFFFF,
                    st->atest, st->atest_func, st->atest_ref, st->ztest, st->zfunc, st->zwrite,
                    st->sc_x0, st->sc_y0, st->sc_x1, st->sc_y1,
                    g_out[0].x / g_out[0].w, g_out[0].y / g_out[0].w, g_out[0].z / g_out[0].w, g_out[0].w, g_out[0].rgba, g_out[0].u, g_out[0].v, e);
    }
    release();
}

/* End of a display list: let go of the context so another thread can take it. */
static void gl_finish(void) {
    if (g_ready != 1) return;
    EnterCriticalSection(&g_lock);
    if (g_owner == GetCurrentThreadId()) { glFlush(); if (g_want_ctx) { wglMakeCurrent(NULL, NULL); g_owner = 0; } }
    LeaveCriticalSection(&g_lock);
}

static void gl_present(void) { }
static int g_tr_live_pending;

/* The shown buffer, 480x272, as 0xAABBGGRR. -1 if that address was never a GL target. */
static int gl_read_display(uint32_t addr, uint32_t stride, int fmt, uint32_t *out) {
    g_sv = 0;                           /* once a frame: pick up settings changes */
    (void)stride; (void)fmt;
    if (g_ready != 1) return -1;
    if (!acquire()) return -1;
    {   /* internal resolution: settings menu (0 = auto from the window), PSPRECOMP_GL_SCALE forces it */
        static int forced = -1; extern int psp_res_auto(int);
        if (forced < 0) { const char *e = getenv("PSPRECOMP_GL_SCALE"); forced = e ? atoi(e) : 0; }
        const int rs = psp_settings()->res_scale;
        int want = forced > 0 ? forced : rs > 0 ? rs : psp_res_auto(psp_window_height());
        want = want < 1 ? 1 : want > 8 ? 8 : want;
        if (want != g_scale) rescale(want);
    }
    g_tr_live = g_tr_live_pending > 0; if (g_tr_live_pending > 0) g_tr_live_pending--;
    g_fdraw = 0;
    static ULONGLONG next_cmd; static unsigned seen_cmd; extern int psp_cmd_changed(unsigned *); const int poll = GetTickCount64() >= next_cmd && psp_cmd_changed(&seen_cmd); if (GetTickCount64() >= next_cmd) next_cmd = GetTickCount64() + 250;
    if (poll) { FILE *pf = fopen("nopost.cmd", "rb"); if (pf) { int v; if (fscanf(pf, "%d", &v) == 1) g_skippost = v; fclose(pf); remove("nopost.cmd"); } }
    if (poll) { FILE *df = fopen("fbdump.cmd", "rb"); if (df) { fclose(df); remove("fbdump.cmd");   /* debug: every render target as fbgl_<addr>.ppm, at the internal scale */
        static uint8_t buf[512 * 8 * 512 * 8 * 4];
        for (int i = 0; i < g_nfb; i++) { const gl_fb *f = &g_fb[i]; const int w = f->w * g_scale, hh = (f->addr >= 0x04150000u ? 120 : 272) * g_scale;
            glBindFramebuffer(GL_FRAMEBUFFER, f->fbo); glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadPixels(0, 0, w, hh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            char fn[48]; snprintf(fn, sizeof fn, "fbgl_%08X.ppm", f->addr); FILE *o = fopen(fn, "wb");
            if (o) { fprintf(o, "P6 %d %d 255\n", w, hh); for (int k = 0; k < w * hh; k++) fwrite(buf + 4 * k, 1, 3, o); fclose(o); } }
        glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? g_cur->fbo : 0); fprintf(stderr, "gl: fbdump %d\n", g_nfb); } }
    if (poll) { FILE *mf = fopen("maxdraw.cmd", "rb"); if (mf) { int v; if (fscanf(mf, "%d", &v) == 1) g_maxdraw = v; fclose(mf); remove("maxdraw.cmd"); } }
    if (poll) { FILE *tf = fopen("gltrace.cmd", "rb"); if (tf) { int k = 1; if (fscanf(tf, "%d", &k) != 1 || k < 1) k = 1; fclose(tf); remove("gltrace.cmd"); g_tr_live_pending = k; fprintf(stderr, "gl: live trace frame %u\n", (unsigned)g_frame + 1); } }
    g_frame++;          /* textures get re-hashed once per shown frame */ if (getenv("PSPRECOMP_GL_ALL") && g_frame % 100 == 0) fprintf(stderr, "gl: frame %u draws %d\n", (unsigned)g_frame, nd);
    { const char *dr = getenv("PSPRECOMP_DUMP_RAM");   /* <frame>: write ram.bin once */
      if (dr && g_frame == (unsigned)atoi(dr)) { FILE *rf = fopen("ram.bin", "wb"); if (rf) { fwrite(psp_mem.ram, 1, PSP_RAM_SIZE, rf); fclose(rf); } } }
    { const char *dv = getenv("PSPRECOMP_DUMP_VRAM");   /* <frame>: write vram.bin once */
      if (dv && g_frame == (unsigned)atoi(dv)) { FILE *vf = fopen("vram.bin", "wb"); if (vf) { fwrite(psp_mem.vram, 1, PSP_VRAM_SIZE, vf); fclose(vf); } } }
    { static uint32_t ov; FILE *of = poll ? fopen("showfb.cmd", "rb") : NULL;   /* showfb.cmd: hex fb address to display instead (0 = off) */
      if (of) { unsigned a; if (fscanf(of, "%x", &a) == 1) ov = a; fclose(of); remove("showfb.cmd"); }
      if (ov) addr = ov; }
    gl_fb *f = find_fb(addr);
    int r = -1;
    if (f) {
        static uint32_t big[480 * 8 * 272 * 8];
        glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        /* Asynchronous readback: start this frame's copy into a pixel buffer
         * and take the one started at the previous vblank, which the GPU has
         * finished. A plain glReadPixels made the game thread wait for the
         * GPU to finish every frame. Costs one vblank of display latency;
         * PSPRECOMP_GL_SYNCREAD=1 goes back to the synchronous read. */
        /* Above 1x: the frame is shrunk on the GPU to the size the window
         * shows it at (linear blit: supersampled), read as BGRA, published
         * for the window, and point-sampled to 480x272 for everything else
         * (screenshots). A window as big as the render reads it whole. */
        static LARGE_INTEGER t_sum; static int t_n; LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
        const int s = g_scale, RW = 480 * s, RH = 272 * s;
        int OW = RW, OH = RH;
        if (s > 1) {
            int vw, vh; psp_window_view(&vw, &vh);
            if (vw > 0 && vh > 0 && vw < RW) {
                OW = vw; OH = vh;
                dn_target(OW, OH);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dn_fbo);
                glDisable(GL_SCISSOR_TEST); g_sv = 0;
                glBlitFramebuffer(0, 0, RW, RH, 0, 0, OW, OH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer(GL_FRAMEBUFFER, g_dn_fbo);
            }
        }
        const GLenum rfmt = s > 1 ? GL_BGRA_EXT : GL_RGBA;
        static int async = -1, pbo_n; static GLuint pbo[2]; static int pi, pn;
        if (async < 0) { async = !getenv("PSPRECOMP_GL_SYNCREAD"); if (async) glGenBuffers(2, pbo); }
        if (async && pbo_n != OW * OH) {      /* (re)size for this read; restart the pipeline */
            for (int i = 0; i < 2; i++) { glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]); glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)OW * OH * 4, NULL, GL_STREAM_READ); }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); pbo_n = OW * OH; pn = 0;
        }
        static int pw[2], ph[2];              /* size each buffer was read at */
        const uint32_t *p = NULL; int iw = OW, ih = OH;
        if (async) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[pi]);
            glReadPixels(0, 0, OW, OH, rfmt, GL_UNSIGNED_BYTE, 0);
            pw[pi] = OW; ph[pi] = OH;
            pi = (pi + 1) % 2; if (pn < 2) pn++;
            if (pn == 2) {   /* pbo[pi] is the previous read */
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[pi]);
                p = (const uint32_t *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
                iw = pw[pi]; ih = ph[pi];
            }
            if (!p) { glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); if (g_cur) glBindFramebuffer(GL_FRAMEBUFFER, g_cur->fbo); release(); return -1; }
        } else {
            glReadPixels(0, 0, OW, OH, rfmt, GL_UNSIGNED_BYTE, big); p = big;
        }
        if (s == 1) {
            memcpy(out, p, 480 * 272 * 4);
            if (g_hi_ready >= 0) { AcquireSRWLockExclusive(&g_hi_lock); g_hi_ready = -1; ReleaseSRWLockExclusive(&g_hi_lock); }
        } else {
            const int n = iw * ih;
            if (n > g_hi_cap) {           /* grow both slots; no reader may hold one */
                AcquireSRWLockExclusive(&g_hi_lock);
                g_hi_ready = -1;
                for (int i = 0; i < 2; i++) { free(g_hi[i]); g_hi[i] = (uint32_t *)malloc((size_t)n * 4); }
                g_hi_cap = g_hi[0] && g_hi[1] ? n : 0;
                ReleaseSRWLockExclusive(&g_hi_lock);
            }
            if (g_hi_cap) {
                const int slot = g_hi_ready == 0 ? 1 : 0;
                memcpy(g_hi[slot], p, (size_t)n * 4);
                AcquireSRWLockExclusive(&g_hi_lock);
                g_hi_w = iw; g_hi_h = ih; g_hi_ready = slot;
                ReleaseSRWLockExclusive(&g_hi_lock);
            }
            static int xs[480], xw;           /* column of each 480-wide sample */
            if (xw != iw) { for (int x = 0; x < 480; x++) xs[x] = (x * iw + iw / 2) / 480; xw = iw; }
            for (int y = 0; y < 272; y++) {
                const uint32_t *row = p + (size_t)((y * ih + ih / 2) / 272) * iw;
                uint32_t *o = out + y * 480;
                for (int x = 0; x < 480; x++) { const uint32_t c = row[xs[x]]; o[x] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu); }
            }
        }
        if (async) { glUnmapBuffer(GL_PIXEL_PACK_BUFFER); glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); }
        glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
        { LARGE_INTEGER t1, fq; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq); t_sum.QuadPart += t1.QuadPart - t0.QuadPart;
          if (++t_n == 300) { fprintf(stderr, "gl: readback %.3f ms avg (%dx -> %dx%d)\n", (double)t_sum.QuadPart / fq.QuadPart * 1e3 / t_n, s, iw, ih); t_sum.QuadPart = 0; t_n = 0; } }
        if (getenv("PSPRECOMP_GL_DEPTHDBG") && g_frame % 100 == 0) {
            float dz[5] = { 0 };
            const int xs[5] = { 240, 60, 420, 240, 240 }, ys[5] = { 136, 136, 136, 30, 240 };
            for (int i = 0; i < 5; i++) glReadPixels(xs[i] * g_scale, ys[i] * g_scale, 1, 1, 0x1902 /* GL_DEPTH_COMPONENT */, 0x1406 /* GL_FLOAT */, &dz[i]);
            fprintf(stderr, "gl: depth %08X frame %u: %.4f %.4f %.4f %.4f %.4f err %04X\n", addr, (unsigned)g_frame, dz[0], dz[1], dz[2], dz[3], dz[4], glGetError());
        }
        if (g_cur) glBindFramebuffer(GL_FRAMEBUFFER, g_cur->fbo);
        r = 0;
        static int nl; if (nl++ % 300 == 0) { uint64_t sum = 0; for (int i = 0; i < 480 * 272; i++) sum += out[i] & 0xFFFFFF; fprintf(stderr, "gl: read %08X sum %llu\n", addr, (unsigned long long)sum); }
    } else { static int nm; if (nm++ < 5) fprintf(stderr, "gl: no fb for display %08X\n", addr); }
    if (g_owner == GetCurrentThreadId() && (g_want_ctx || getenv("PSPRECOMP_GL_UNBIND"))) { wglMakeCurrent(NULL, NULL); g_owner = 0; }   /* keep it bound: rebinding each frame cost time */
    release();
    return r;
}

const psp_render_backend psp_render_gl = {
    "gl", gl_init, gl_shutdown, gl_target, gl_draw, gl_finish, gl_present, gl_read_display
};

#else
const psp_render_backend psp_render_gl = { "gl", 0 };
#endif
