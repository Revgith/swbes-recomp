/* psprecomp -- a window to play in.
 *
 * Shows the PSP display, scaled, and turns the keyboard and the first game
 * controller into sceCtrl input. It runs on a host thread of its own and
 * reads the game's front buffer on a 60 Hz timer, so it never waits on the
 * game and the game never waits on it.
 *
 *   Keyboard            Controller (XInput)     PSP
 *   arrows              d-pad                   d-pad
 *   (none)              left stick              analog stick
 *   Z                   A                       Cross
 *   X                   B                       Circle
 *   A                   X                       Square
 *   S                   Y                       Triangle
 *   Q / W               LB, LT / RB, RT         L / R
 *   Enter               Start                   Start
 *   Backspace           Back                    Select
 *
 * Windows only for now (Win32 + GDI + XInput: no dependencies). Elsewhere
 * psp_window_start() returns -1. */
#ifndef PSPRECOMP_WINDOW_H
#define PSPRECOMP_WINDOW_H

/* Open the window, `scale` times the PSP's 480x272, or with `scale` 0 as
 * large as fits the screen. Returns 0 if it opened. */
int psp_window_start(const char *title, int scale);
/* Shown faintly in the PC menu, e.g. "v1.0.1". Call before or after start. */
void psp_window_version(const char *v);

/* Nonzero once the user has closed the window. */
int psp_window_closed(void);


/* Raw mouse look: deltas since the last take (only while captured). */
int  psp_mouse_captured(void);
int  psp_mouse_left(void);   /* left button held while the mouse is captured */
int  psp_mouse_right(void);  /* right button held while the mouse is captured */
void psp_keys_fly(void);     /* call each frame while flying: WASD leave the stick, Shift/Ctrl = stick up/down */
void psp_keys_foot(void);    /* call each frame on foot: WASD leave the stick (the game layer reads them) */
void psp_mouse_take(long *dx, long *dy);
void psp_mouse_inject(long dx, long dy);   /* test/automation feed */
void psp_ui_cursor_inject(int x, int y, int click);   /* test/automation: menu cursor at a PSP pixel */

/* PC settings: edited in the window's overlay menu (Esc or F1) and kept in
 * settings.ini in the working directory. The game layer reads them each
 * frame; a game may override fields at startup (e.g. from environment). */
typedef struct {
    int fps;          /* frame cap; 0 = uncapped */
    int sens_pct;     /* mouse sensitivity, percent of the default */
    int invert;       /* invert mouse Y */
    int volume;       /* 0..100 */
    int smooth;       /* smooth (halftone) scaling instead of nearest */
    int fullscreen;   /* borderless fullscreen */
    int show_fps;     /* frame counter in the corner */
    int tex_filter;   /* world textures: 0 game's choice, 1 nearest, 2 bilinear, 3 trilinear + 16x anisotropic */
    int ui_filter;    /* HUD and menus: 0 game's choice, 1 nearest, 2 bilinear */
    int aim_assist;   /* game aim assist (aim slows/pulls onto targets); 0 = off */
    int res_scale;    /* internal resolution, multiple of 480x272; 0 = auto (fits the window) */
    int pad_sens;     /* controller stick aim speed, percent of the default */
    int prompts;      /* button prompts: 0 auto (last used device), 1 keyboard, 2 + PSP_STYLE_* forced */
    int filter;       /* the game's lavender colour filter (render_gl.c glow_sharp): 0 off, 1 on */
    int filter_blur;  /* 1: soft, as the PSP's quarter-size pass; 0: per pixel */
    int veh_sens;     /* vehicle aim (ships, tanks, turrets, walkers), percent, on top of the mouse / stick one */} psp_settings_t;
psp_settings_t *psp_settings(void);
void psp_settings_save(void);
void psp_settings_reload(void);   /* read [pc] again (settings.ini changed underneath) */
int  psp_menu_open(void);

/* Game controller, for game layers that drive play themselves: buttons in
 * the XInput bit layout (a PlayStation pad's Cross is A, Circle B, Square X,
 * Triangle Y, L1/R1 LB/RB, L2/R2 LT/RT, L3/R3 LS/RS, Options Start, Create
 * Back), sticks -1..1 after the dead zone with y up, triggers 0..1. */
enum {
    PSP_PAD_UP = 0x0001, PSP_PAD_DOWN = 0x0002, PSP_PAD_LEFT = 0x0004, PSP_PAD_RIGHT = 0x0008,
    PSP_PAD_START = 0x0010, PSP_PAD_BACK = 0x0020, PSP_PAD_LS = 0x0040, PSP_PAD_RS = 0x0080,
    PSP_PAD_LB = 0x0100, PSP_PAD_RB = 0x0200, PSP_PAD_LT = 0x0400, PSP_PAD_RT = 0x0800,
    PSP_PAD_A = 0x1000, PSP_PAD_B = 0x2000, PSP_PAD_X = 0x4000, PSP_PAD_Y = 0x8000,
    PSP_PAD_TOUCH = 0x10000,   /* PlayStation touchpad click (acts as Select, like Create) */
    PSP_PAD_HOME = 0x20000,    /* PS button; left to the system, read for completeness */
    PSP_PAD_TOUCH_L = 0x40000, PSP_PAD_TOUCH_R = 0x80000,   /* with TOUCH: the half the (first) finger clicked */
    PSP_PAD_MUTE = 0x100000,                                  /* DualSense mic button */
    PSP_PAD_FN_L = 0x200000, PSP_PAD_FN_R = 0x400000,         /* DualSense Edge function buttons */
    PSP_PAD_PADDLE_L = 0x800000, PSP_PAD_PADDLE_R = 0x1000000, /* DualSense Edge back buttons */
    /* PSP_PAD_HOME is also the Xbox Guide button. Read and reported only:
     * nothing is bound to these. */
};
typedef struct { unsigned btn; float lx, ly, rx, ry, lt, rt; int on; /* 0 none, 1 XInput, 2 PlayStation */ } psp_pad_t;
const psp_pad_t *psp_pad(void);
unsigned psp_pad_poll(void);   /* poll the controller now, return its buttons (for loops that block the game's input read) */
int psp_pad_last(void);   /* 1 if prompts should show pad buttons: the pad was used last, or a pad style is forced */
/* Controller style for prompts (glyphs, button names): forced in F1 > Button
 * prompts, else the connected pad's, from its vendor / product id. */
enum { PSP_STYLE_SERIES, PSP_STYLE_ONE, PSP_STYLE_360, PSP_STYLE_DS5, PSP_STYLE_DS4, PSP_STYLE_SWITCH, PSP_NSTYLE };
int psp_pad_style(void);
void psp_pad_fake(const psp_pad_t *p);   /* test hook: hold this state (on = 0 releases) */
int  psp_window_height(void);   /* client height, for the auto internal resolution */
/* The shown frame at the internal resolution (BGRA, top-down); copies it into
 * out if it fits cap pixels. -1 when rendering at 1x or nothing is ready. */
int  psp_display_read_hires(unsigned int *out, int cap, int *w, int *h);
/* Borrow the frame without copying: 0 and the pixels, held until unlock. */
int  psp_display_hires_lock(const unsigned int **px, int *w, int *h);
void psp_display_hires_unlock(void);
void psp_window_view(int *w, int *h);   /* the size the frame is shown at (0 before the first paint) */

/* Cursor mode for the game's own menus: the game layer turns it on while a
 * menu is up. Then the mouse is a free pointer (no capture); clicks, right
 * clicks and wheel steps are counted for the game layer to take. */
void psp_ui_set(int on);
void psp_ui_esc_start(int on);   /* a menu with only prompts (a map): Esc presses Start, not Circle */
/* Set by a game layer: the key bound to the window's default key vk (W, A, S, D, Q, E, R, F, P,
 * Tab, Enter, Esc, arrows, Shift, Ctrl); 0 = unbound. NULL: the defaults. */
extern int (*psp_key_remap)(int vk);
/* Key binds a game layer keeps in settings.ini [keys], for the PC menu's
 * Controls page: listed, rebound by pressing a key or mouse button, written
 * back (the game layer reloads the file). Key names: a letter or digit,
 * Space, Shift, Ctrl, Alt, Tab, Enter, Esc, Bksp, Caps, Up, Down, Left, Right,
 * F2-F12, LMB, RMB, Mouse3-5, Wheel, or 0x.. . F1 is never taken (PC menu). */
enum { PSP_BIND_MENU = 1,    /* a menu bind: Space and Backspace already select / back there */
       PSP_BIND_WHEEL = 2,   /* may take the mouse wheel */
       PSP_BIND_SPACE = 4,   /* menu bind that may take Space anyway (it is select) */
       PSP_BIND_BKSP = 8 };  /* menu bind that may take Backspace anyway (it is back) */
typedef struct { const char *id, *label, *def; int flags;
                 const char *paddef; /* pad button in [pad] (psp_pad_bind_name), "None"; NULL: no pad bind */ } psp_bind_t;
void psp_binds_set(const psp_bind_t *b, int n);
/* Pad bind names (Xbox naming): A B X Y LB RB LT RT LS RS Up Down Left Right
 * PaddleL PaddleR FnL FnR Start Back, or None. Back + Start still opens the PC menu. */
unsigned    psp_pad_bind_parse(const char *name);   /* 0: None or unknown */
const char *psp_pad_bind_name(unsigned m);          /* "None" for 0 */
extern unsigned psp_pad_fire;   /* pad button passed to the game as its R (fire) in play; RT by default */
/* Set by a game layer: when to pass the game its R as fire (replaces LMB and psp_pad_fire), for
 * controllers it does not write fire to directly. NULL: LMB / psp_pad_fire always. Window thread. */
extern int (*psp_fire_r)(void);
/* Pad buttons for the PSP's up, right, down, left, cross, circle, square, triangle, L, R, start, select. */
extern unsigned psp_pad_map[12];
/* Cheats a game layer offers on the PC menu's Cheats tab: one button each;
 * a press calls run(cmd) on the window thread (queue it for the game). */
typedef struct { const char *label, *cmd; } psp_cheat_t;
extern volatile int psp_netplay;   /* in an ad hoc session: the Cheats tab is hidden, cheats refused */
void psp_cheats_set(const psp_cheat_t *c, int n, void (*run)(const char *cmd));
/* Errors for the player: a panel at the top of the window (hidden while the
 * PC menu is open), lines grouped under their source as a heading, each shown
 * for secs seconds; posting a line again only renews it. Any thread.
 * psp_error_clear drops a source's lines (NULL: all). */
void psp_error(const char *src, const char *msg, int secs);
void psp_error_clear(const char *src);
void psp_key_fake(int vk, int down);
int  psp_key_down(int vk);              /* key held (focused window, PC menu closed) */   /* test hook: hold a key without focus */
int  psp_ui_cursor(float *x, float *y);          /* PSP pixels; 1 if over the picture */
void psp_ui_fake(float x, float y, int clicks, int rclicks);   /* test hook: cursor at PSP pixel, clicks */
void psp_ui_take(int *clicks, int *rclicks, int *wheel, int *moved);
void psp_ctrl_pulse(unsigned bits, unsigned delay, unsigned reads);

/* Set by a game layer: called with each newly decoded texture (RGBA, PSP
 * size); return a replacement image (any size, *ow x *oh) or NULL to keep it. */
#include <stdint.h>
extern const uint32_t *(*psp_tex_post)(const uint32_t *px, int w, int h, int *ow, int *oh);
void psp_tex_refresh(void);   /* psp_tex_post would now answer differently: decode every texture again */
/* Set by a game layer: may change a textured sprite's vertex colour (RGBA as
 * the GE has it), given its texture size and texel rect. For art a game layer
 * replaced that the game tints. */
extern uint32_t (*psp_sprite_color)(int tw, int th, float u0, float v0, float u1, float v1, uint32_t rgba);
#endif
