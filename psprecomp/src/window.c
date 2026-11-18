/* psprecomp -- a window to play in. See include/psprecomp/window.h. */

#include "psprecomp/window.h"
#include "psprecomp/hle.h"
#include "psprecomp/mods.h"
#include "ui.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define W 480
#define H 272

/* PSP button bits (SceCtrlData.Buttons). */
enum {
    B_SELECT = 0x0001, B_START = 0x0008,
    B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
    B_L = 0x0100, B_R = 0x0200,
    B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000
};

static volatile LONG g_closed;
static int g_scale;
static const char *g_title = "psprecomp";
static uint32_t g_px[W * H];
static uint32_t g_movie[W * H]; static volatile LONG g_movie_on;   /* movie.c: a frame shown instead of the PSP display */
void psp_window_kick(void);
void psp_window_movie(const uint32_t *bgra) { if (bgra) { memcpy(g_movie, bgra, sizeof g_movie); InterlockedExchange(&g_movie_on, 1); } else InterlockedExchange(&g_movie_on, 0); psp_window_kick(); }

static volatile LONG g_ui;   /* the game's own menu is up (see psp_ui_set) */
/* Test hook: keys held by a script (key.cmd), no window focus needed. */
static volatile uint8_t g_fake[256]; static volatile LONG g_nfake;
void psp_key_fake(int vk, int down) { if ((g_fake[vk & 255] != 0) != (down != 0)) { g_fake[vk & 255] = (uint8_t)(down != 0); InterlockedExchangeAdd(&g_nfake, down ? 1 : -1); } }
static int key_down(int vk) { return g_fake[vk & 255] || (GetAsyncKeyState(vk) & 0x8000); }
static HWND g_hwnd; static int g_menu;   /* defined below */
/* For game layers: a key held, only while the window has focus (or a test holds it). */
int psp_key_down(int vk) { return g_fake[vk & 255] || (g_hwnd && GetForegroundWindow() == g_hwnd && !g_menu && (GetAsyncKeyState(vk) & 0x8000)); }

extern volatile long psp_game_frames;
static volatile long g_fly_keys = -100;   /* game frame of the last psp_keys_fly() */
void psp_keys_fly(void) { g_fly_keys = psp_game_frames; }
static volatile long g_foot_keys = -100;  /* game frame of the last psp_keys_foot() */
void psp_keys_foot(void) { g_foot_keys = psp_game_frames; }

/* Keyboard: WASD = analog stick, arrows = d-pad, Space = Cross (menus), R = Square,
 * F = Triangle, Q/E = L/R, Enter = Start (in play), P = Start, Tab/Backspace = Select, F1 = PC settings.
 * Those are defaults: a game layer may rebind each through psp_key_remap. */
int (*psp_key_remap)(int vk);
/* Errors for the player (psp_error): grouped by source, each line shown for
 * its own time. Any thread may post; the window thread draws. */
enum { NERR = 16, ERR_PER_SRC = 4 };   /* past ERR_PER_SRC live lines a source counts the rest as "more" */
static struct { char src[48], msg[160]; ULONGLONG until; int more; } g_err[NERR]; static SRWLOCK g_err_lk = SRWLOCK_INIT;
void psp_error(const char *src, const char *msg, int secs) {
    AcquireSRWLockExclusive(&g_err_lk);
    const ULONGLONG now = GetTickCount64(), until = now + (ULONGLONG)secs * 1000; int k = 0, live = 0, last = -1;
    for (int i = 0; i < NERR; i++) if (g_err[i].until > now && !strcmp(g_err[i].src, src)) {
        if (!strcmp(g_err[i].msg, msg)) { g_err[i].until = until; ReleaseSRWLockExclusive(&g_err_lk); return; }   /* again: just its time */
        live++; last = i; }
    if (live >= ERR_PER_SRC) { g_err[last].more++; g_err[last].until = until; ReleaseSRWLockExclusive(&g_err_lk); return; }
    for (int i = 0; i < NERR; i++) if (g_err[i].until < g_err[k].until) k = i;   /* a free or expired slot, else the oldest */
    snprintf(g_err[k].src, sizeof g_err[k].src, "%s", src); snprintf(g_err[k].msg, sizeof g_err[k].msg, "%s", msg);
    g_err[k].until = until; g_err[k].more = 0;
    ReleaseSRWLockExclusive(&g_err_lk);
}
void psp_error_clear(const char *src) {
    AcquireSRWLockExclusive(&g_err_lk);
    for (int i = 0; i < NERR; i++) if (!src || !strcmp(g_err[i].src, src)) g_err[i].until = 0;
    ReleaseSRWLockExclusive(&g_err_lk);
}
static int kd(int vk) { const int v = psp_key_remap ? psp_key_remap(vk) : vk; return v > 0 && v < 255 && key_down(v); }
/* Set by a game layer that drives play actions itself (swbes host.c): then
 * the PSP-button keys only work in the game's menus, and in play every key is
 * free for that layer's own binds. Tab (Select) and P (Start) still pass. */
int psp_direct_play;
static uint32_t keyboard(uint8_t *ax, uint8_t *ay) {
    static const struct { int vk; uint32_t bit; } K[] = {
        { VK_UP, B_UP }, { VK_RIGHT, B_RIGHT }, { VK_DOWN, B_DOWN }, { VK_LEFT, B_LEFT },
        { 'F', B_TRIANGLE }, { 'R', B_SQUARE },   /* PC-friendly aliases, shown on the key caps (swbes host.c) */
        { 'Q', B_L }, { 'E', B_R }, { 'P', B_START },   /* P: Start anywhere (menus use Enter as select) */
        { VK_RETURN, B_START }, { VK_TAB, B_SELECT }, { VK_BACK, B_SELECT },
    };
    /* PC conventions: in the game's menus Enter selects and Esc / Backspace go
     * back; while playing Esc and Enter pause. A key keeps the meaning it had
     * when pressed until released, so the Esc that opens the pause menu does
     * not also close it. */
    static const struct { int vk; uint32_t play, menu; } C[] = {
        { VK_ESCAPE, B_START, B_CIRCLE }, { VK_RETURN, B_START, B_CROSS },
        { VK_BACK, B_SELECT, B_CIRCLE },
        /* In play Space / Shift are jump / sprint, fed by the game layer (swbes host.c);
         * Cross there only tilts the camera. In menus Space selects. */
        { VK_SPACE, 0, B_CROSS },
    };
    static uint32_t held[sizeof C / sizeof C[0]];
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (K[i].vk != VK_RETURN && K[i].vk != VK_BACK && kd(K[i].vk)
            && (g_ui || K[i].vk == VK_TAB || K[i].vk == 'P' || !psp_direct_play)) b |= K[i].bit;
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        if (!kd(C[i].vk)) { held[i] = 0; continue; }
        if (!held[i]) held[i] = g_ui ? C[i].menu : C[i].play;
        b |= held[i];
    }
    int x = 0, y = 0;
    if (psp_game_frames - g_foot_keys <= 3) {   /* on foot: the game layer moves the soldier from WASD itself */
    } else if (psp_game_frames - g_fly_keys <= 3) {     /* flying: WASD steer (game layer), Shift/Ctrl throttle */
        if (kd(VK_SHIFT)) y--;
        if (kd(VK_CONTROL)) y++;
    } else {
        if (kd('A')) x--;
        if (kd('D')) x++;
        if (kd('W')) y--;
        if (kd('S')) y++;
    }
    if (x) *ax = x < 0 ? 0 : 255;
    if (y) *ay = y < 0 ? 0 : 255;
    return b;
}

/* ---- controllers ---------------------------------------------------------
 * One pad state (window.h, psp_pad_t) from the first XInput controller, or a
 * PlayStation controller read as raw HID (DualSense, DualSense Edge,
 * DualShock 4 are not XInput devices). Buttons use the XInput bit layout. */
static psp_pad_t g_pad;                  /* read by the game layer (psp_pad) */
static volatile LONG g_pad_last;         /* 1 the pad was used last, 0 keyboard / mouse */
const psp_pad_t *psp_pad(void) { return &g_pad; }
static psp_settings_t g_set;
int psp_pad_last(void) { return g_set.prompts == 1 ? 0 : g_set.prompts >= 2 ? 1 : g_pad_last; }
static volatile LONG g_ds_kind;   /* last PlayStation pad seen: 1 DualSense, 2 DualShock 4 */
static volatile LONG g_xin_pid;   /* XInput pad's product id (0 unknown) */
static const char *const PROMPT_NAME[] = { "Auto", "Keyboard", "Xbox Series", "Xbox One", "Xbox 360", "DualSense", "DualShock 4", "Nintendo Switch" };
#define NPROMPT 8
int psp_pad_style(void) {
    if (g_set.prompts >= 2 && g_set.prompts < NPROMPT) return g_set.prompts - 2;
    if (g_pad.on == 2) return g_ds_kind == 2 ? PSP_STYLE_DS4 : PSP_STYLE_DS5;
    switch (g_xin_pid) {   /* Microsoft 045E */
    case 0x028E: case 0x028F: case 0x0291: case 0x0719: return PSP_STYLE_360;
    case 0x02D1: case 0x02DD: case 0x02E3: case 0x02EA: case 0x02FD: case 0x0B00: case 0x0B05: case 0x0B20: case 0x0B22: return PSP_STYLE_ONE;
    default: return PSP_STYLE_SERIES;
    }
}
/* Test hook (pad.cmd): a pad state held until changed; on = 0 drops it. */
static psp_pad_t g_pad_fake;
void psp_pad_fake(const psp_pad_t *p) { g_pad_fake = *p; }

/* PlayStation report, kept as the pad state it describes. */
static volatile LONG g_ds_btn, g_ds_ax[4] = { 128, 128, 128, 128 };
static volatile DWORD g_ds_tick;
static void ds_report(HANDLE dev, const BYTE *r, DWORD n) {
    static HANDLE known; static int kind;   /* 1 DualSense, 2 DualShock 4, -1 other */
    if (dev != known) {
        RID_DEVICE_INFO di; UINT sz = sizeof di; di.cbSize = sizeof di; kind = -1;
        if (GetRawInputDeviceInfoA(dev, RIDI_DEVICEINFO, &di, &sz) != (UINT)-1 &&
            di.dwType == RIM_TYPEHID && di.hid.dwVendorId == 0x054C) {
            const DWORD p = di.hid.dwProductId;
            if (p == 0x0CE6 || p == 0x0DF2) kind = 1;
            else if (p == 0x05C4 || p == 0x09CC || p == 0x0BA0) kind = 2;
        }
        known = dev;
    }
    if (kind < 0 || n < 10) return;
    g_ds_kind = kind;
    /* s = sticks (lx ly rx ry), f = hat + face buttons, m = shoulders, sticks, options/share. */
    int s, f, m;
    if (kind == 1 && r[0] == 0x01 && n >= 64) { s = 1; f = 8; m = 9; }       /* DualSense USB */
    else if (kind == 1 && r[0] == 0x31 && n >= 12) { s = 2; f = 9; m = 10; } /* DualSense Bluetooth */
    else if (r[0] == 0x01) { s = 1; f = 5; m = 6; }                          /* DS4, or DualSense BT basic */
    else if (kind == 2 && r[0] == 0x11 && n >= 10) { s = 3; f = 7; m = 8; }  /* DS4 Bluetooth */
    else return;
    static const WORD hat[8] = { PSP_PAD_UP, PSP_PAD_UP | PSP_PAD_RIGHT, PSP_PAD_RIGHT, PSP_PAD_RIGHT | PSP_PAD_DOWN,
                                 PSP_PAD_DOWN, PSP_PAD_DOWN | PSP_PAD_LEFT, PSP_PAD_LEFT, PSP_PAD_LEFT | PSP_PAD_UP };
    unsigned b = 0;
    if ((r[f] & 15) < 8) b |= hat[r[f] & 15];
    if (r[f] & 0x10) b |= PSP_PAD_X;        /* Square */
    if (r[f] & 0x20) b |= PSP_PAD_A;        /* Cross */
    if (r[f] & 0x40) b |= PSP_PAD_B;        /* Circle */
    if (r[f] & 0x80) b |= PSP_PAD_Y;        /* Triangle */
    if (r[m] & 0x01) b |= PSP_PAD_LB;
    if (r[m] & 0x02) b |= PSP_PAD_RB;
    if (r[m] & 0x04) b |= PSP_PAD_LT;       /* L2 / R2: digital bits, as triggers */
    if (r[m] & 0x08) b |= PSP_PAD_RT;
    if (r[m] & 0x10) b |= PSP_PAD_BACK;     /* Create / Share */
    if (r[m] & 0x20) b |= PSP_PAD_START;    /* Options */
    if (r[m] & 0x40) b |= PSP_PAD_LS;
    if (r[m] & 0x80) b |= PSP_PAD_RS;
    if ((DWORD)m + 1 < n) {   /* next byte: PS button, touchpad click; DualSense mute, Edge Fn / back buttons */
        const BYTE e = r[m + 1];
        if (e & 0x01) b |= PSP_PAD_HOME;
        if (e & 0x02) b |= PSP_PAD_TOUCH;
        if (kind == 1 && n >= 12) {   /* full DualSense report only (DS4's byte holds a frame counter there) */
            if (e & 0x04) b |= PSP_PAD_MUTE;
            if (e & 0x10) b |= PSP_PAD_FN_L;
            if (e & 0x20) b |= PSP_PAD_FN_R;
            if (e & 0x40) b |= PSP_PAD_PADDLE_L;
            if (e & 0x80) b |= PSP_PAD_PADDLE_R;
        }
    }
    /* Touch points (Linux hid-playstation layout): 4 bytes each, byte 0 bit 7
     * set = no finger, then x 12 bits (0..1919) and y 12 bits. A click takes
     * the side of the first finger down; with none reported it counts as left. */
    if (b & PSP_PAD_TOUCH) {
        const int tp = kind == 1 ? s + 32 : r[0] == 0x11 ? 37 : 35;   /* DualSense: after sticks, seq, buttons, motion, timestamp; DS4: after its touch-report count + timestamp */
        int x = -1;
        for (int k = 0; k < 2 && x < 0; k++) { const int o = tp + k * 4;
            if ((DWORD)o + 2 < n && !(r[o] & 0x80)) x = r[o + 1] | (r[o + 2] & 0x0F) << 8; }
        b |= x >= 960 ? PSP_PAD_TOUCH_R : PSP_PAD_TOUCH_L;
    }
    g_ds_btn = (LONG)b; for (int i = 0; i < 4; i++) g_ds_ax[i] = r[s + i];
    g_ds_tick = GetTickCount();
}

/* A stick axis to -1..1 with a radial dead zone and the rest rescaled, so
 * small moves still register just past the dead zone. */
static void stick(float x, float y, float dz, float *ox, float *oy) {
    const float m = sqrtf(x * x + y * y);
    if (m <= dz) { *ox = *oy = 0.0f; return; }
    const float k = (m > 1.0f ? 1.0f : m - dz) / (1.0f - dz) / m;
    *ox = x * k; *oy = y * k;
    if (*ox > 1.0f) *ox = 1.0f; if (*ox < -1.0f) *ox = -1.0f; if (*oy > 1.0f) *oy = 1.0f; if (*oy < -1.0f) *oy = -1.0f;
}
/* Refresh g_pad. XInput is polled at most every 4 ms (and backs off to once
 * a second while nothing is plugged in: XInputGetState on an empty slot is slow). */
static void pad_poll(void) {
    static DWORD next_try; XINPUT_STATE s; psp_pad_t p; memset(&p, 0, sizeof p);
    const DWORD now = GetTickCount();
    /* XInputGetStateEx (xinput1_4 ordinal 100, undocumented but stable) also
     * reports the Guide button, as 0x0400; plain XInputGetState hides it. */
    typedef DWORD (WINAPI *xin_ex_t)(DWORD, XINPUT_STATE *); static xin_ex_t xin_ex; static int xin_tried;
    if (!xin_tried) { xin_tried = 1; HMODULE x = LoadLibraryA("xinput1_4.dll"); if (x) xin_ex = (xin_ex_t)(void *)GetProcAddress(x, (LPCSTR)(ULONG_PTR)100); }
    if (now >= next_try && (xin_ex ? xin_ex(0, &s) : XInputGetState(0, &s)) == ERROR_SUCCESS) {
        p.on = 1; p.btn = s.Gamepad.wButtons & 0xF3FF;
        if (!g_xin_pid) {   /* which Xbox pad: XInputGetCapabilitiesEx (xinput1_4 ordinal 108) has its USB ids */
            typedef struct { XINPUT_CAPABILITIES c; WORD vid, pid, ver, pad; DWORD x; } caps_ex_t;
            typedef DWORD (WINAPI *caps_t)(DWORD, DWORD, DWORD, caps_ex_t *); static caps_t caps; static int tried;
            if (!tried) { tried = 1; HMODULE x = LoadLibraryA("xinput1_4.dll"); if (x) caps = (caps_t)(void *)GetProcAddress(x, (LPCSTR)(ULONG_PTR)108); }
            caps_ex_t ce; memset(&ce, 0, sizeof ce);
            g_xin_pid = caps && caps(1, 0, 0, &ce) == ERROR_SUCCESS && ce.pid ? ce.pid : 0xFFFF;
        }
        if (s.Gamepad.wButtons & 0x0400) p.btn |= PSP_PAD_HOME;   /* Guide */
        p.lt = s.Gamepad.bLeftTrigger / 255.0f; p.rt = s.Gamepad.bRightTrigger / 255.0f;
        if (s.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) p.btn |= PSP_PAD_LT;
        if (s.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) p.btn |= PSP_PAD_RT;
        stick(s.Gamepad.sThumbLX / 32767.0f, s.Gamepad.sThumbLY / 32767.0f, 0.24f, &p.lx, &p.ly);
        stick(s.Gamepad.sThumbRX / 32767.0f, s.Gamepad.sThumbRY / 32767.0f, 0.20f, &p.rx, &p.ry);
    } else {
        if (now >= next_try) { next_try = now + 1000; g_xin_pid = 0; }
        if (now - g_ds_tick <= 500) {    /* PlayStation pad: y grows downwards in its report */
            p.on = 2; p.btn = (unsigned)g_ds_btn;
            p.lt = p.btn & PSP_PAD_LT ? 1.0f : 0.0f; p.rt = p.btn & PSP_PAD_RT ? 1.0f : 0.0f;
            stick((g_ds_ax[0] - 128) / 127.0f, (128 - g_ds_ax[1]) / 127.0f, 0.20f, &p.lx, &p.ly);
            stick((g_ds_ax[2] - 128) / 127.0f, (128 - g_ds_ax[3]) / 127.0f, 0.16f, &p.rx, &p.ry);
        }
    }
    if (g_pad_fake.on) p = g_pad_fake;
    if (p.btn || p.lx || p.ly || p.rx || p.ry) g_pad_last = 1;
    g_pad = p;
}
unsigned psp_pad_poll(void) { pad_poll(); return g_pad.on ? g_pad.btn : 0; }

/* The pad as PSP buttons and stick, for the game's menus (and every game
 * when no game layer drives play itself). In play under a game layer
 * (psp_direct_play) only Start / Select pass, plus the left stick except on
 * foot (vehicles and ships take throttle from it); the layer reads the rest
 * through psp_pad. */
static uint32_t pad(uint8_t *ax, uint8_t *ay) {
    static const uint32_t P[12] = { B_UP, B_RIGHT, B_DOWN, B_LEFT, B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_L, B_R, B_START, B_SELECT };
    pad_poll();
    const psp_pad_t p = g_pad;
    if (!p.on) return 0;
    const int play = psp_direct_play && !g_ui;
    uint32_t b = 0;
    for (int i = 0; i < 12; i++) {
        const unsigned x = psp_pad_map[i] | (P[i] == B_SELECT && psp_pad_map[i] == PSP_PAD_BACK ? PSP_PAD_TOUCH : 0);
        if ((p.btn & x) && (!play || P[i] == B_START || P[i] == B_SELECT)) b |= P[i]; }
    if (play && !psp_fire_r && (p.btn & psp_pad_fire)) b |= B_R;    /* fire: the game's own R, as the left mouse button */
    if (!play || psp_game_frames - g_foot_keys > 3) {
        if (p.lx || p.ly) {   /* the PSP stick is 0..255 with 128 at rest and y growing downwards */
            *ax = (uint8_t)(128 + (int)(p.lx * 127.0f));
            *ay = (uint8_t)(128 - (int)(p.ly * 127.0f));
        }
    }
    return b;
}

/* ---- PC settings (see window.h) ------------------------------------- */
static psp_settings_t g_set;
static const psp_settings_t g_def = { 60, 100, 0, 100, 0, 0, 0, 0, 0, 1, 0, 100, 0, 0, 1 };
static const char *ini_path(void) {
    static char p[MAX_PATH];
    if (!p[0]) { GetCurrentDirectoryA(MAX_PATH - 16, p); strcat(p, "\\settings.ini"); }
    return p;
}
const char *psp_ini_path(void) { return ini_path(); }
#define SET_FIELDS(X) X(fps) X(sens_pct) X(invert) X(volume) X(smooth) X(fullscreen) X(show_fps) X(tex_filter) X(ui_filter) X(aim_assist) X(res_scale) X(pad_sens) X(prompts) X(filter) X(filter_blur)
psp_settings_t *psp_settings(void) {
    static volatile LONG loaded;
    if (!loaded) {
        g_set = g_def;
#define LOAD(f) g_set.f = (int)GetPrivateProfileIntA("pc", #f, g_def.f, ini_path());
        SET_FIELDS(LOAD)
#undef LOAD
        loaded = 1;
    }
    return &g_set;
}
void psp_settings_save(void) {
    char v[16];
#define SAVE(f) snprintf(v, sizeof v, "%d", g_set.f); WritePrivateProfileStringA("pc", #f, v, ini_path());
    SET_FIELDS(SAVE)
#undef SAVE
}

/* ---- PC menu: state for ui.cpp, which draws it ------------------------- */
volatile unsigned long long psp_game_cycles;
volatile long psp_game_frames;           /* bumped per game frame (display.c) */
static int g_menu, g_fps_shown;
int psp_menu_open(void) { return g_menu; }
int psp_window_height(void) { RECT c; if (!g_hwnd || !GetClientRect(g_hwnd, &c)) return 272; return c.bottom; }
static const psp_bind_t *g_binds; static int g_nbinds;
static char g_bval[64][16];   /* each bind's key name, as settings.ini has it */
static char g_pval[64][16];   /* ...and pad button, [pad] */
int ui_capt = -1, ui_capt_pad; char ui_kmsg[96];
unsigned psp_pad_fire = PSP_PAD_RT;
int (*psp_fire_r)(void);
unsigned psp_pad_map[12] = { PSP_PAD_UP, PSP_PAD_RIGHT, PSP_PAD_DOWN, PSP_PAD_LEFT, PSP_PAD_A, PSP_PAD_B, PSP_PAD_X, PSP_PAD_Y,
                             PSP_PAD_LB | PSP_PAD_LT, PSP_PAD_RB | PSP_PAD_RT, PSP_PAD_START, PSP_PAD_BACK };
static const struct { unsigned m; const char *n; } k_padn[] = {
    { PSP_PAD_A, "A" }, { PSP_PAD_B, "B" }, { PSP_PAD_X, "X" }, { PSP_PAD_Y, "Y" }, { PSP_PAD_LB, "LB" }, { PSP_PAD_RB, "RB" },
    { PSP_PAD_LT, "LT" }, { PSP_PAD_RT, "RT" }, { PSP_PAD_LS, "LS" }, { PSP_PAD_RS, "RS" }, { PSP_PAD_UP, "Up" }, { PSP_PAD_DOWN, "Down" },
    { PSP_PAD_LEFT, "Left" }, { PSP_PAD_RIGHT, "Right" }, { PSP_PAD_PADDLE_L, "PaddleL" }, { PSP_PAD_PADDLE_R, "PaddleR" },
    { PSP_PAD_FN_L, "FnL" }, { PSP_PAD_FN_R, "FnR" }, { PSP_PAD_START, "Start" }, { PSP_PAD_BACK, "Back" } };
unsigned psp_pad_bind_parse(const char *s) {
    for (size_t i = 0; i < sizeof k_padn / sizeof *k_padn; i++) if (!_stricmp(s, k_padn[i].n)) return k_padn[i].m;
    return 0;
}
const char *psp_pad_bind_name(unsigned m) {
    for (size_t i = 0; i < sizeof k_padn / sizeof *k_padn; i++) if (k_padn[i].m == m) return k_padn[i].n;
    return "None";
}
const char *ui_pval(int i) { return g_binds[i].paddef ? g_pval[i] : NULL; }
int ui_pad_clash(int i) {
    if (!g_binds[i].paddef || !_stricmp(g_pval[i], "None")) return -1;
    for (int j = 0; j < g_nbinds; j++)   /* defaults that share a button (used in different modes) are no clash */
        if (j != i && g_binds[j].paddef && !_stricmp(g_pval[i], g_pval[j]) && _stricmp(g_binds[i].paddef, g_binds[j].paddef)
            && (g_binds[i].flags & PSP_BIND_MENU) == (g_binds[j].flags & PSP_BIND_MENU)) return j;
    return -1;
}
/* A pad button pressed while bind i waits for one. */
static void pad_take(HWND h, int i, unsigned press) {
    for (size_t k = 0; k < sizeof k_padn / sizeof *k_padn; k++) if (press & k_padn[k].m) {
        snprintf(g_pval[i], sizeof g_pval[i], "%s", k_padn[k].n); WritePrivateProfileStringA("pad", g_binds[i].id, k_padn[k].n, ini_path());
        ui_capt = -1; ui_kmsg[0] = 0; InvalidateRect(h, NULL, FALSE); return; }
}
void ui_pad_clear(int i) {
    snprintf(g_pval[i], sizeof g_pval[i], "None"); WritePrivateProfileStringA("pad", g_binds[i].id, "None", ini_path());
}
void psp_binds_set(const psp_bind_t *b, int n) { g_binds = b; g_nbinds = n > 64 ? 64 : n; }
int ui_nbinds(void) { return g_nbinds; }
static const psp_cheat_t *g_cheats; static int g_ncheats; static void (*g_cheat_run)(const char *);
void psp_cheats_set(const psp_cheat_t *c, int n, void (*run)(const char *)) { g_cheats = c; g_ncheats = n; g_cheat_run = run; }
int ui_ncheats(void) { return g_cheat_run && !psp_netplay ? g_ncheats : 0; }
const char *ui_cheat(int i) { return g_cheats[i].label; }
void ui_cheat_run(int i) { if (g_cheat_run && !psp_netplay) g_cheat_run(g_cheats[i].cmd); }
const psp_bind_t *ui_bind(int i) { return &g_binds[i]; }
const char *ui_bval(int i) { return g_bval[i]; }
void ui_binds_read(void) {
    for (int i = 0; i < g_nbinds; i++) {
        GetPrivateProfileStringA("keys", g_binds[i].id, g_binds[i].def, g_bval[i], sizeof g_bval[i], ini_path());
        if (!g_bval[i][0]) snprintf(g_bval[i], sizeof g_bval[i], "%s", g_binds[i].def);
        if (g_binds[i].paddef) {
            GetPrivateProfileStringA("pad", g_binds[i].id, g_binds[i].paddef, g_pval[i], sizeof g_pval[i], ini_path());
            if (!psp_pad_bind_parse(g_pval[i])) snprintf(g_pval[i], sizeof g_pval[i], "None"); } }
}
/* The key name settings.ini uses for a pressed key (VK, or a mouse button / wheel as below). */
enum { KV_WHEEL = 0x1FF };
static void key_name(int vk, char *s, size_t n) {
    static const struct { int vk; const char *n; } N[] = {
        { VK_SPACE, "Space" }, { VK_SHIFT, "Shift" }, { VK_CONTROL, "Ctrl" }, { VK_MENU, "Alt" }, { VK_TAB, "Tab" }, { VK_RETURN, "Enter" },
        { VK_CAPITAL, "Caps" }, { VK_ESCAPE, "Esc" }, { VK_BACK, "Bksp" }, { VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" },
        { VK_LBUTTON, "LMB" }, { VK_RBUTTON, "RMB" }, { VK_MBUTTON, "Mouse3" }, { VK_XBUTTON1, "Mouse4" }, { VK_XBUTTON2, "Mouse5" }, { KV_WHEEL, "Wheel" } };
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++) if (N[i].vk == vk) { snprintf(s, n, "%s", N[i].n); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(s, n, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(s, n, "F%d", vk - VK_F1 + 1); return; }
    snprintf(s, n, "0x%02X", vk);
}
/* The other bind of the same kind (play / menu) on the same key, or -1. */
int ui_bind_clash(int i) {
    for (int j = 0; j < g_nbinds; j++)
        if (j != i && !_stricmp(g_bval[i], g_bval[j]) && (g_binds[i].flags & PSP_BIND_MENU) == (g_binds[j].flags & PSP_BIND_MENU)) return j;
    return -1;
}
/* A key pressed while bind i waits: refuse what the window keeps, else write it. */
static void bind_take(HWND h, int i, int vk) {
    if (ui_capt_pad) return;   /* waiting for a pad button: keys and clicks don't answer (Esc cancels) */
    const int f = g_binds[i].flags; char k[16]; key_name(vk, k, sizeof k);
    const char *why = vk == VK_F1 ? "F1 opens this menu"
        : vk == KV_WHEEL && !(f & PSP_BIND_WHEEL) ? "The wheel only works for next weapon"
        : vk == VK_SPACE && (f & PSP_BIND_MENU) && !(f & PSP_BIND_SPACE) ? "Space already selects in menus"
        : vk == VK_BACK && (f & PSP_BIND_MENU) && !(f & PSP_BIND_BKSP) ? "Backspace already goes back in menus" : NULL;
    ui_capt = -1;
    if (why) snprintf(ui_kmsg, sizeof ui_kmsg, "%s: %s not changed", why, g_binds[i].label);
    else { snprintf(g_bval[i], sizeof g_bval[i], "%s", k); WritePrivateProfileStringA("keys", g_binds[i].id, k, ini_path()); ui_kmsg[0] = 0; }
    InvalidateRect(h, NULL, FALSE);
}
void ui_binds_reset(void) {
    for (int i = 0; i < g_nbinds; i++) { snprintf(g_bval[i], sizeof g_bval[i], "%s", g_binds[i].def); WritePrivateProfileStringA("keys", g_binds[i].id, g_binds[i].def, ini_path());
        if (g_binds[i].paddef) { snprintf(g_pval[i], sizeof g_pval[i], "%s", g_binds[i].paddef); WritePrivateProfileStringA("pad", g_binds[i].id, g_binds[i].paddef, ini_path()); } }
    ui_kmsg[0] = 0;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int psp_res_auto(int ch) { return clampi((ch + 271) / 272, 1, 6); }   /* fits the window height */

static WINDOWPLACEMENT g_wp = { sizeof(WINDOWPLACEMENT) };
static void apply_fullscreen(HWND h) {
    const LONG st = GetWindowLongA(h, GWL_STYLE);
    if (g_set.fullscreen && (st & WS_OVERLAPPEDWINDOW)) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(h, &g_wp);
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(h, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else if (!g_set.fullscreen && !(st & WS_OVERLAPPEDWINDOW)) {
        SetWindowLongA(h, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_wp);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}
static void mouse_capture(HWND h, int on);
static void menu_set(HWND h, int on) {
    if (on == g_menu) return;
    g_menu = on; ui_capt = -1; ui_toggled();
    if (on) mouse_capture(h, 0);
    InvalidateRect(h, NULL, FALSE);
}
int ui_menu(void) { return g_menu; }
void ui_menu_close(void) { menu_set(g_hwnd, 0); }
static char g_version[32];
void psp_window_version(const char *v) { snprintf(g_version, sizeof g_version, "%s", v ? v : ""); }
const char *ui_version(void) { return g_version; }
const psp_settings_t *ui_defaults(void) { return &g_def; }
void ui_fullscreen(void) { apply_fullscreen(g_hwnd); }
int ui_fps(void) { return g_fps_shown; }
int ui_errors(ui_err_t *e, int cap) {   /* each live source's lines together, in slot order */
    int n = 0; const ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_err_lk);
    for (int s = 0; s < NERR; s++) if (g_err[s].until > now) {
        int seen = 0; for (int j = 0; j < s; j++) seen |= g_err[j].until > now && !strcmp(g_err[j].src, g_err[s].src);
        if (!seen) for (int i = s; i < NERR && n < cap; i++) if (g_err[i].until > now && !strcmp(g_err[i].src, g_err[s].src)) {
            memcpy(e[n].src, g_err[i].src, sizeof e[n].src); memcpy(e[n].msg, g_err[i].msg, sizeof e[n].msg); e[n].more = g_err[i].more; n++; } }
    ReleaseSRWLockShared(&g_err_lk);
    return n;
}

/* Fitted image size, for the GPU to downscale the internal-resolution frame
 * to exactly what is shown (psp_window_view). */
static volatile LONG g_view_w, g_view_h;
void psp_window_view(int *w, int *h) { *w = g_view_w; *h = g_view_h; }

/* Everything shown, into an off-screen DC of cw x ch. */
static void compose(HDC dc, uint32_t *px, int cw, int ch) {
    /* Keep the PSP's aspect, black bars around it. */
    int w = cw, hh = cw * H / W;
    if (hh > ch) { hh = ch; w = ch * W / H; }
    g_view_w = w; g_view_h = hh;
    const int x0 = (cw - w) / 2, y0 = (ch - hh) / 2;
    PatBlt(dc, 0, 0, cw, y0, BLACKNESS);
    PatBlt(dc, 0, y0 + hh, cw, ch - y0 - hh, BLACKNESS);
    PatBlt(dc, 0, 0, x0, ch, BLACKNESS);
    PatBlt(dc, x0 + w, 0, cw - x0 - w, ch, BLACKNESS);

    if (g_movie_on) { memcpy(g_px, g_movie, sizeof g_px); goto blit; }
    {   /* internal resolution above 1x: the frame as the GPU sized it, drawn
         * straight from the renderer's buffer (no copy unless dimmed) */
        const uint32_t *hp; int hw, hh2;
        if (psp_display_hires_lock(&hp, &hw, &hh2) == 0) {
            if (g_menu) {
                static uint32_t *hb; static int hcap;
                if (hw * hh2 > hcap) { free(hb); hcap = hw * hh2; hb = (uint32_t *)malloc((size_t)hcap * 4); }
                if (hb) { for (int i = 0; i < hw * hh2; i++) hb[i] = (hp[i] >> 3) & 0x001F1F1Fu; hp = hb; }
            }
            BITMAPINFO bi; ZeroMemory(&bi, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = hw; bi.bmiHeader.biHeight = -hh2;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            if (hw == w && hh2 == hh)    /* already the shown size: a plain copy */
                SetDIBitsToDevice(dc, x0, y0, hw, hh2, 0, 0, 0, hh2, hp, &bi, DIB_RGB_COLORS);
            else {
                SetStretchBltMode(dc, g_set.smooth ? HALFTONE : COLORONCOLOR); SetBrushOrgEx(dc, 0, 0, NULL);
                StretchDIBits(dc, x0, y0, w, hh, 0, 0, hw, hh2, hp, &bi, DIB_RGB_COLORS, SRCCOPY);
            }
            psp_display_hires_unlock();
            goto drawn;
        }
    }
    if (psp_display_read(g_px) == 0) {
        for (int i = 0; i < W * H; i++) {      /* 0xAABBGGRR to the DIB's BGRA */
            const uint32_t c = g_px[i];
            g_px[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
            if (g_menu) g_px[i] = (g_px[i] >> 3) & 0x001F1F1Fu;   /* dim behind the menu */
        }
        blit:;
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;            /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, g_set.smooth ? HALFTONE : COLORONCOLOR);   /* nearest: crisp pixels */
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, x0, y0, w, hh, 0, 0, W, H, g_px, &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        PatBlt(dc, x0, y0, w, hh, BLACKNESS);
    }
    drawn:
    GdiFlush();
    ui_frame(g_hwnd, px, cw, ch);   /* FPS, errors, the PC menu */
}
/* A top-down 32-bit DIB section of w x h selected into a memory DC: GDI and
 * ui.cpp both draw into its pixels. */
static uint32_t *dib(HDC *dc, HBITMAP *bm, int w, int h) {
    if (!*dc) *dc = CreateCompatibleDC(NULL);
    if (*bm) DeleteObject(*bm);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *p = NULL; *bm = CreateDIBSection(*dc, &bi, DIB_RGB_COLORS, &p, NULL, 0);
    SelectObject(*dc, *bm);
    return (uint32_t *)p;
}
static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    if (r.right <= 0 || r.bottom <= 0) { EndPaint(h, &ps); return; }
    /* Drawn off-screen, then one blit: no flicker under the overlay. */
    static HDC dc; static HBITMAP bm; static uint32_t *px; static int bw, bh;
    if (bw != r.right || bh != r.bottom) { px = dib(&dc, &bm, r.right, r.bottom); bw = r.right; bh = r.bottom; }
    if (px) { compose(dc, px, r.right, r.bottom); BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY); }
    EndPaint(h, &ps);
}
/* PSPRECOMP_PAINTBENCH=WxH: also compose every frame into an off-screen
 * WxH bitmap and log the time (a hidden test window never paints). */
static void paint_bench(void) {
    static int bw = -1, bh; static HDC dc; static HBITMAP bm; static uint32_t *bpx; static double tsum; static int n;
    if (bw < 0) { const char *e = getenv("PSPRECOMP_PAINTBENCH"); bw = 0; if (e) sscanf(e, "%dx%d", &bw, &bh);
        if (bw > 0) bpx = dib(&dc, &bm, bw, bh); if (!bpx) bw = 0; }
    if (bw <= 0) return;
    LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
    compose(dc, bpx, bw, bh); GdiFlush();
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    tsum += (double)(b.QuadPart - a.QuadPart) / f.QuadPart * 1e3;
    if (++n == 300) { fprintf(stderr, "paint: %.3f ms avg at %dx%d\n", tsum / n, bw, bh); tsum = 0; n = 0; }
    if (GetFileAttributesA("winshot.cmd") != INVALID_FILE_ATTRIBUTES) {   /* test hook: the composed window (menu included) -> winshot.bmp */
        DeleteFileA("winshot.cmd");
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), bw, -bh, 1, 32, BI_RGB } };
        uint32_t *px = (uint32_t *)malloc((size_t)bw * bh * 4); HBITMAP bm = (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP);
        FILE *o = px && GetDIBits(dc, bm, 0, (UINT)bh, px, &bi, DIB_RGB_COLORS) ? fopen("winshot.bmp", "wb") : NULL;
        if (o) { BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(54 + bw * bh * 4), 0, 0, 54 };
                 fwrite(&fh, sizeof fh, 1, o); fwrite(&bi.bmiHeader, 40, 1, o); fwrite(px, 4, (size_t)bw * bh, o); fclose(o); }
        free(px);
    }
}


/* Mouse look: click the window to capture the mouse, Esc (or losing focus)
 * releases it. Raw mouse deltas are accumulated here and taken by the game
 * layer (psp_mouse_take), which turns them straight into view rotation;
 * left button = R (fire). */
static volatile LONG g_mcap, g_mdx, g_mdy; static int g_mlb, g_mrb;
static HWND g_hwnd;
static volatile LONG g_ui, g_cx = -1, g_cy = -1, g_clicks, g_rclicks, g_wheel, g_moved;
#define WM_UI_RELEASE (WM_APP + 1)
#define WM_UI_RECAPTURE (WM_APP + 2)
static volatile LONG g_recap;   /* the mouse was looking when a menu came up: take it back after */
void psp_ui_set(int on) {
    if (on == g_ui) return;
    g_ui = on;
    if (getenv("PSPRECOMP_UIDEBUG")) fprintf(stderr, "ui mode %d\n", on);
    if (on) { g_recap = g_mcap; if (g_mcap && g_hwnd) PostMessageA(g_hwnd, WM_UI_RELEASE, 0, 0); }
    else if (g_recap && g_hwnd) { g_recap = 0; PostMessageA(g_hwnd, WM_UI_RECAPTURE, 0, 0); }
    InterlockedExchange(&g_clicks, 0); InterlockedExchange(&g_rclicks, 0); InterlockedExchange(&g_wheel, 0);
}
int psp_ui_cursor(float *x, float *y) {
    const LONG cx = g_cx, cy = g_cy;
    *x = (float)cx / 16.0f; *y = (float)cy / 16.0f;
    return cx >= 0 && cy >= 0 && cx < W * 16 && cy < H * 16 && !g_mcap;
}
void psp_ui_take(int *clicks, int *rclicks, int *wheel, int *moved) {
    *clicks = InterlockedExchange(&g_clicks, 0); *rclicks = InterlockedExchange(&g_rclicks, 0);
    *wheel = InterlockedExchange(&g_wheel, 0); *moved = InterlockedExchange(&g_moved, 0);
}
/* Client pixel -> PSP pixel (x16), through the same letterbox as paint(). */
static void track_cursor(HWND h, int mx, int my) {
    RECT r; GetClientRect(h, &r);
    int w = r.right, hh = r.right * H / W;
    if (hh > r.bottom) { hh = r.bottom; w = r.bottom * W / H; }
    if (w <= 0 || hh <= 0) return;
    const int x0 = (r.right - w) / 2, y0 = (r.bottom - hh) / 2;
    const LONG nx = (LONG)((mx - x0) * W * 16 / w), ny = (LONG)((my - y0) * H * 16 / hh);
    if (nx != g_cx || ny != g_cy) { g_cx = nx; g_cy = ny; InterlockedExchange(&g_moved, 1); }
}
static void mouse_capture(HWND h, int on) {
    if (on == g_mcap) return;
    g_mcap = on; InterlockedExchange(&g_mdx, 0); InterlockedExchange(&g_mdy, 0); g_mlb = g_mrb = 0;
    if (on) { RECT r; GetClientRect(h, &r); MapWindowPoints(h, NULL, (POINT *)&r, 2); ClipCursor(&r); while (ShowCursor(FALSE) >= 0) {} }
    else    { ClipCursor(NULL); while (ShowCursor(TRUE) < 0) {} }
}
int  psp_mouse_captured(void) { return g_mcap; }
int  psp_mouse_left(void) { return (g_mcap && g_mlb) || g_fake[VK_LBUTTON]; }   /* key.cmd "1 1" holds it */
int  psp_mouse_right(void) { return (g_mcap && g_mrb) || g_fake[VK_RBUTTON]; }   /* key.cmd "2 1" holds it */
void psp_mouse_take(long *dx, long *dy) { *dx = InterlockedExchange(&g_mdx, 0); *dy = InterlockedExchange(&g_mdy, 0); }
void psp_mouse_inject(long dx, long dy) { InterlockedExchangeAdd(&g_mdx, dx); InterlockedExchangeAdd(&g_mdy, dy); }
/* test/automation: put the menu cursor at PSP pixel (x, y), optionally with a click */
void psp_ui_cursor_inject(int x, int y, int click) { g_cx = x * 16; g_cy = y * 16; InterlockedExchange(&g_moved, 1); if (click) InterlockedIncrement(&g_clicks); }

static volatile LONG g_kicked;
/* Called by the display code when it latches a new frame: the window paints
 * it and samples input right away instead of on its next 16 ms timer tick,
 * which added up to a frame of lag and jitter to both. */
void psp_window_kick(void) {
    if (g_hwnd && !InterlockedExchange(&g_kicked, 1)) PostMessageA(g_hwnd, WM_APP + 7, 0, 0);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_menu && ui_capt < 0 && ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS))
        ui_wndproc(h, msg, wp, lp);   /* the menu (ImGui) sees input first; a bind waiting for a key does not share it */
    switch (msg) {
    case WM_APP + 7:   /* a new frame was latched (psp_window_kick): show it and sample input now */
        InterlockedExchange(&g_kicked, 0); paint_bench();
        /* fall through */
    case WM_TIMER: {
        uint8_t ax = 128, ay = 128;
        uint32_t b = pad(&ax, &ay);
        if (GetForegroundWindow() == h || g_nfake) b |= keyboard(&ax, &ay);   /* keys only when focused */
        else mouse_capture(h, 0);
        if (psp_fire_r ? psp_fire_r() : (g_mcap && g_mlb)) b |= 0x200;
        {   /* the PC menu on the pad: Back + Start opens / closes it; ImGui moves
             * through it (ui.cpp); B goes back or closes, Start closes */
            static unsigned was; static int swallow; const unsigned d = g_pad.btn, press = d & ~was;
            if ((d & (PSP_PAD_START | PSP_PAD_BACK)) == (PSP_PAD_START | PSP_PAD_BACK) && (press & (PSP_PAD_START | PSP_PAD_BACK))) {
                menu_set(h, !g_menu);
                psp_ctrl_set(0, 128, 128); was = d; swallow = 1; return 0;   /* neither reaches the game */
            } else if (g_menu && ui_capt >= 0 && ui_capt_pad) { if (press) pad_take(h, ui_capt, press); }   /* waiting for a button (Esc cancels) */
            else if (g_menu && !(d & PSP_PAD_BACK)) {
                if (ui_capt >= 0) { if (press & (PSP_PAD_B | PSP_PAD_START)) ui_capt = -1; }   /* waiting for a key: B cancels */
                else if (press & PSP_PAD_START) menu_set(h, 0);
                else if ((press & PSP_PAD_B) && !ui_back()) menu_set(h, 0);
            }
            /* buttons that toggled the menu stay held: don't let their release-press reach the game */
            if (!(d & (PSP_PAD_START | PSP_PAD_BACK))) swallow = 0;
            if (swallow) b &= ~(uint32_t)(B_START | B_SELECT);
            was = d;
        }
        if (g_menu && ui_capt >= 0 && g_nfake)          /* test hook: a key held by psp_key_fake answers a capture */
            for (int vk = 1; vk < 255; vk++) if (g_fake[vk]) { if (vk == VK_ESCAPE) ui_capt = -1; else bind_take(h, ui_capt, vk); break; }
        if (g_menu) { b = 0; ax = ay = 128; }          /* the menu has the input */
        psp_ctrl_set(b, ax, ay);
        { static DWORD t0; static long f0; const DWORD t = GetTickCount();
          if (t - t0 >= 1000) { const long f = psp_game_frames; g_fps_shown = (int)((f - f0) * 1000 / (long)(t - t0 ? t - t0 : 1)); f0 = f; t0 = t; } }
        InvalidateRect(h, NULL, FALSE);
        if (msg != WM_TIMER) UpdateWindow(h);   /* kicked: paint now, not when the queue empties */
        return 0;
    }
    case WM_INPUT: {
        static union { RAWINPUT r; BYTE b[1024]; } u; UINT sz = sizeof u;
        const RAWINPUT *rp = &u.r;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &u, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            const RAWINPUT ri = *rp;
            if (ri.header.dwType == RIM_TYPEHID && rp->data.hid.dwCount && rp->data.hid.dwSizeHid)
                ds_report(ri.header.hDevice, rp->data.hid.bRawData, rp->data.hid.dwSizeHid);
            else if (ri.header.dwType == RIM_TYPEMOUSE && g_mcap) {
                if (ri.data.mouse.lLastX || ri.data.mouse.lLastY || ri.data.mouse.usButtonFlags) g_pad_last = 0;
                if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) { InterlockedExchangeAdd(&g_mdx, ri.data.mouse.lLastX); InterlockedExchangeAdd(&g_mdy, ri.data.mouse.lLastY); }
                if (ri.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) g_mlb = 1;
                if (ri.data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)   g_mlb = 0;
                if (ri.data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) g_mrb = 1;
                if (ri.data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)   g_mrb = 0;
            }
        }
        return DefWindowProcA(h, msg, wp, lp);
    }
    case WM_LBUTTONDOWN:
        if (g_menu && ui_capt >= 0) { bind_take(h, ui_capt, VK_LBUTTON); return 0; }
        if (g_menu) return 0;
        if (g_ui) { track_cursor(h, (short)LOWORD(lp), (short)HIWORD(lp)); InterlockedIncrement(&g_clicks); return 0; }
        if (!g_mcap) mouse_capture(h, 1); return 0;
    case WM_RBUTTONDOWN:
        if (g_menu && ui_capt >= 0) { bind_take(h, ui_capt, VK_RBUTTON); return 0; }
        if (g_ui && !g_menu) InterlockedIncrement(&g_rclicks);
        return 0;
    case WM_UI_RELEASE: mouse_capture(h, 0); return 0;
    case WM_UI_RECAPTURE: if (!g_mcap && !g_menu && GetForegroundWindow() == h) mouse_capture(h, 1); return 0;
    case WM_MOUSEMOVE:
        if (!g_mcap) track_cursor(h, (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        return 0;
    case WM_MBUTTONDOWN: if (g_menu && ui_capt >= 0) bind_take(h, ui_capt, VK_MBUTTON); return 0;
    case WM_XBUTTONDOWN: if (g_menu && ui_capt >= 0) bind_take(h, ui_capt, HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2); return TRUE;
    case WM_MOUSEWHEEL:
        if (g_menu && ui_capt >= 0) { bind_take(h, ui_capt, KV_WHEEL); return 0; }
        if (g_menu) return 0;
        InterlockedExchangeAdd(&g_wheel, (short)HIWORD(wp) > 0 ? 1 : -1);
        return 0;
    case WM_SYSKEYDOWN:
        if (g_menu && ui_capt >= 0) { bind_take(h, ui_capt, (int)wp); return 0; }   /* Alt, F10 */
        if (wp == VK_RETURN) { g_set.fullscreen ^= 1; apply_fullscreen(h); psp_settings_save(); return 0; }
        return DefWindowProcA(h, msg, wp, lp);
    case WM_KEYDOWN:
        g_pad_last = 0;
        if (g_menu && ui_capt >= 0) { if (lp & 0x40000000) return 0;   /* auto-repeat of the key that started it */
            if (wp == VK_ESCAPE) ui_capt = -1; else bind_take(h, ui_capt, (int)wp); return 0; }
        if (wp == VK_F1 || (wp == VK_ESCAPE && g_menu)) { if (!(wp == VK_ESCAPE && ui_back())) menu_set(h, !g_menu); return 0; }
        return 0;
    case WM_KILLFOCUS:   mouse_capture(h, 0); return 0;
    case WM_SIZE: case WM_MOVE: if (g_mcap) { g_mcap = 0; mouse_capture(h, 1); } return DefWindowProcA(h, msg, wp, lp);
    case WM_PAINT:      paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:    PostQuitMessage(0); return 0;
    default:            return DefWindowProcA(h, msg, wp, lp);
    }
}

static DWORD WINAPI window_main(LPVOID arg) {
    HANDLE ready = (HANDLE)arg;
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));   /* the exe's icon resource 1, if any */
    wc.lpszClassName = "psprecomp_window";
    RegisterClassA(&wc);

    /* Per-monitor pixels, not Windows' blurry bitmap stretch of a DPI-unaware
     * window. With no scale asked for, take the largest whole multiple that
     * fits most of the screen's height. */
    SetProcessDPIAware();
    if (g_scale <= 0) {
        RECT wa;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        g_scale = (int)((wa.bottom - wa.top) * 7 / 10 / H);
        if (g_scale < 1) g_scale = 1;
    }
    RECT r = { 0, 0, W * g_scale, H * g_scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    /* PSPRECOMP_BACKGROUND=1: open minimized without taking focus (test runs). */
    const char *bg = getenv("PSPRECOMP_BACKGROUND");
    const int background = bg && *bg == '1';
    HWND h = CreateWindowA(wc.lpszClassName, g_title, WS_OVERLAPPEDWINDOW | (background ? 0 : WS_VISIBLE),
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!h) { InterlockedExchange(&g_closed, 1); SetEvent(ready); return 1; }
    /* Not minimized: a minimized window gets no WM_PAINT, which throttles the
     * frame loop to ~10 fps. Shown behind everything, without focus, instead. */
    if (background) {
        ShowWindow(h, SW_SHOWNOACTIVATE);
        SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    g_hwnd = h;
    psp_settings();
    if (g_set.fullscreen && !background) apply_fullscreen(h);
    { RAWINPUTDEVICE rid[2] = { { 0x01, 0x02, 0, h },                     /* generic mouse */
                                { 0x01, 0x05, RIDEV_INPUTSINK, h } };     /* gamepads (PlayStation pads) */
      RegisterRawInputDevices(rid, 2, sizeof rid[0]); }
    SetTimer(h, 1, 16, NULL);
    SetEvent(ready);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    InterlockedExchange(&g_closed, 1);
    return 0;
}

int psp_window_start(const char *title, int scale) {
    if (title) g_title = title;
    g_scale = scale;
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, window_main, ready, 0, NULL);
    if (!t) { CloseHandle(ready); return -1; }
    WaitForSingleObject(ready, INFINITE);       /* the window exists, or failed */
    CloseHandle(ready);
    CloseHandle(t);
    return g_closed ? -1 : 0;
}

int psp_window_closed(void) { return g_closed != 0; }

#else

/* ponytail: no window off Windows yet; SDL2, as ps3recomp uses there, is the
 * obvious next step. */
int psp_window_start(const char *title, int scale) { (void)title; (void)scale; return -1; }
int psp_window_closed(void) { return 0; }
void psp_window_version(const char *v) { (void)v; }

#endif

/* Test hook (ui.cmd): put the menu cursor at PSP pixel (x, y) and click. */
void psp_ui_fake(float x, float y, int clicks, int rclicks) {
    g_cx = (LONG)(x * 16.0f); g_cy = (LONG)(y * 16.0f); InterlockedExchange(&g_moved, 1);
    InterlockedExchangeAdd(&g_clicks, clicks); InterlockedExchangeAdd(&g_rclicks, rclicks);
}
