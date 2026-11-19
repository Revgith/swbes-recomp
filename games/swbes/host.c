/* Star Wars Battlefront: Elite Squadron (ULES-01214) -- recomp host.
 *
 * Loads the decrypted module, opens a window that shows the game and reads the
 * keyboard and a controller, and runs module_start. The game's own threads do
 * the rest; this thread just keeps the scheduler turning until the window is
 * closed.
 *
 *   wtf_<id>.exe [path\to\<id>.elf]
 *
 * Controls are listed in psprecomp/window.h: arrows, Z = Cross, X = Circle,
 * A = Square, S = Triangle, Q/W = L/R, Enter = Start, Backspace = Select, or
 * an Xbox-style controller. */

#include <stdarg.h>
#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/module.h>
#include <psprecomp/window.h>
#include <psprecomp/render.h>

#include <windows.h>
#include <io.h>
int psp_cmd_changed(unsigned *seen);
static unsigned g_seen_shot, g_seen_ram; static int g_zoom_wheel;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void psp_recomp_register(void);   /* in the generated <id>_register.c */

/* .text, .data, .bss, and what a module linked at 0 expects above them. */
#define MODULE_SPAN 0x03000000u

/* Mouse look. The soldier update (0x00211A24) turns the stick into this
 * frame's pitch/yaw deltas and hands pointers to both to the aim-assist step
 * at 0x002738A8 (a2 = &pitch, a3 = &yaw) before adding them to the view
 * angles. Only the local player's soldier takes that path, so adding the raw
 * mouse movement here rotates the view directly: no stick, no acceleration.
 * SWBES_MOUSE_SENS = radians per count (default 0.0025); SWBES_MOUSE_INVERT=1
 * flips vertical. */
/* ---- Weapon keys ---------------------------------------------------------
 * The soldier points to its inventory at +0x1D0: primaries at +0, secondaries
 * (grenades, equipment) at +48, current primary/secondary at +120/+121 and
 * their counts at +122/+123. The game's own selectors take (inv, _, slot),
 * slot -1 = next; they refuse while the current weapon is busy, as the d-pad
 * does: 0x2DC830 primaries, 0x2DCB0C secondaries. 1/2 pick a primary, 3/4/5 a
 * secondary, the wheel cycles primaries. */
static psp_cpu_state g_saved_cpu;
static uint32_t game_call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    g_saved_cpu = psp_cpu;
    psp_cpu.r[4] = a0; psp_cpu.r[5] = a1; psp_cpu.r[6] = a2;
    psp_cpu.r[29] = (psp_cpu.r[29] - 0x100u) & ~15u;   /* below the hooked frame */
    psp_cpu.r[31] = 0;
    psp_call_from_host(fn);
    const uint32_t v0 = psp_cpu.r[2];
    psp_cpu = g_saved_cpu;
    return v0;
}

/* Spawn the local player at its team's n-th command post (0-based), the way
 * the spawn map's Select does (SWB.FE.RequestRespawn): 0x2EECC4(n) picks the
 * n-th CP the player's team owns and stores its id, 0x2EEE1C spawns there,
 * then the two spawn-menu pages are closed. 0 if there is no such CP yet. */
static int spawn_at(uint32_t n) {
    if (!game_call(0x002EECC4u, n, 0, 0)) return 0;
    game_call(0x002EEE1Cu, 0, 0, 0);
    game_call(0x000A677Cu, 0xBF679E15u, 1, 0);
    game_call(0x000A677Cu, 0xD0C953CCu, 1, 0);
    fprintf(stderr, "spawn: command post %u (id %08X)\n", n, psp_read32(0x00452178u));
    return 1;
}

/* ---- The game's console ---------------------------------------------------
 * console.cmd (work/run): one command per line, "Name [int args]", run through
 * the game's own handlers (table in console_cmds.h): e.g. "Cheat.KillFlyer",
 * "KillSelf", "SWB.FE.RestartMap". "help Name" prints the handler's own help,
 * "list [text]" the names. Lines are queued by the host loop and run from a
 * per-frame game hook, where calling into the game is safe. */
#include "console_cmds.h"
static char g_con[1024]; static volatile int g_con_ready; static int g_con_echo;
/* The game's console output (0x000B9170, a1 = UTF-16 text; 0xB92E8 formats into
 * it): echoed to stderr while a console.cmd command runs, e.g. its help. */
static void con_print(uint32_t addr) { (void)addr;
    if (!g_con_echo) return;
    char t[512]; int n = 0; uint32_t a = psp_cpu.r[5];
    for (uint16_t c; n < 511 && (c = (uint16_t)psp_read16(a)) != 0; a += 2) t[n++] = c < 128 ? (char)c : '?';
    t[n] = 0; int ok = 0; for (int i = 0; i < n; i++) ok += t[i] != '?';
    if (ok > n / 2) fprintf(stderr, "game: %s\n", t);   /* skip the binary twin it also prints */
}
/* Game entities (soldiers and vehicles) keep their id at +0x434. Vehicles are
 * found by id in the controllable table at 0x5CBBC8 (soldiers are in it too,
 * vtable 0x426E08; 0x3BC5BC looks one up). 0x269620(vehicle, soldier id)
 * is what the game runs when a soldier uses a vehicle: it seats him and hands
 * his controller over. */
extern uint32_t g_player_soldier; extern volatile long psp_game_frames;
/* The PC menu's Cheats tab: the game's own console commands, run like a console.cmd line. */
static const psp_cheat_t k_cheats[] = {
    { "Become a hero", "Cheat.MakeHero" },
    { "All bots become heroes", "HeroAllBots" },
    { "Refill ammo", "SWB.Restock" },
    { "Republic / Rebels win", "Cheat.GoodWin" },
    { "CIS / Empire win", "Cheat.EvilWin" },
    { "Restart map", "SWB.FE.RestartMap" },
    { "Next map", "NextMap" },
    { "Unlock all levels", "Cheat.UnlockAllLevels" },
};
static void cheat_run(const char *cmd) {
    if (g_con_ready) return;   /* one at a time: the last is still queued */
    snprintf(g_con, sizeof g_con, "%s\n", cmd); g_con_ready = 1;
}
static uint32_t obj_id(uint32_t o) { return psp_read32(o + 0x434); }
static uint32_t g_want_vt = 0x00428A60u;   /* the class "board" without an id takes: flyers, or 0x42AA00 hover tanks ("board tank") */
static void list_vehicles(uint32_t want, uint32_t *found) {
    const uint32_t T = 0x005CBBC8u, b = psp_read32(T), n = psp_read32(T + 4);
    for (uint32_t k = 0; k < n && k < 4096; k++)
        for (uint32_t nd = psp_read32(b + 4 * k), g = 0; nd && g < 256; nd = psp_read32(nd + 4), g++) {
            if (psp_read8(nd + 8)) continue;
            const uint32_t o = psp_read32(nd), id = obj_id(o); if (psp_read32(o) == 0x00426E08u) continue;
            if (found) { if (want ? id == want : (!*found && (id || g_want_vt != 0x00428A60u) && psp_read32(o) == g_want_vt)) *found = o; }
            else fprintf(stderr, "vehicle %08X obj %08X vt %08X\n", id, o, psp_read32(o));
        }
}
/* The vehicle after `after` by address (the list reorders as things move), wrapping; 0 if none. */
static uint32_t next_vehicle(uint32_t after) {
    const uint32_t T = 0x005CBBC8u, b = psp_read32(T), n = psp_read32(T + 4);
    uint32_t lo = 0, up = 0;
    for (uint32_t k = 0; k < n && k < 4096; k++)
        for (uint32_t nd = psp_read32(b + 4 * k), g = 0; nd && g < 256; nd = psp_read32(nd + 4), g++) {
            if (psp_read8(nd + 8)) continue;
            const uint32_t o = psp_read32(nd); if (psp_read32(o) == 0x00426E08u) continue;
            if (!lo || o < lo) lo = o;
            if (o > after && (!up || o < up)) up = o;
        }
    return up ? up : lo;
}
static int board(uint32_t id) {
    if (!g_player_soldier) return 0;
    uint32_t v = 0; if (id >= 0x08800000u && id < 0x0A000000u) v = id; else list_vehicles(id, &v);   /* an id, or the object itself */
    if (!v) return 0;
    game_call(0x00269620u, v, obj_id(g_player_soldier), 0);
    fprintf(stderr, "board: vehicle %08X (%08X)\n", obj_id(v), v); return 1;
}
/* SWBES_QM_BOARD=<hex vehicle id, 1 for any flyer, "tank" or "turret">: board it a second after
 * spawning. Noticed in the soldier's per-frame update, done from con_run. */
static uint32_t g_gnd_map;
uint32_t g_hum_map; static long g_force_ch = -1, g_force_until, g_use_until = -1; static int g_block, g_use_pend; static long g_use_frame = -1; static long g_fly_pend_ch = -1;
/* PC key binds, one per game action, read from the [keys] section of
 * settings.ini (missing entries are written back with their default, so the
 * file always lists every bind). Values are key names: a letter or digit,
 * Space, Shift, Ctrl, Alt, Tab, Enter, Esc, Bksp, Caps, Up, Down, Left, Right, F1-F12,
 * LMB, RMB, Wheel (next_weapon only), Mouse3 (middle), Mouse4, Mouse5, or a raw
 * hex VK as 0x.. . Prompts in the game's text show whatever is bound. */
enum { B_JUMP, B_SPRINT, B_CROUCH, B_EQUIP, B_THROW, B_USE, B_CYCLE2, B_SEAT, B_TARGET, B_STUNT, B_AUTOPILOT, B_W1, B_W2, B_W3, B_W4, B_W5, B_HERO_YES, B_HERO_NO,
       B_FWD, B_BACK, B_LEFT, B_RIGHT, B_FIRE, B_AIM, B_CYCLE1,   /* movement, mouse buttons, next weapon */
       B_RECENTER, B_TSEARCH, B_WACTION, B_SPRINT_TOG, B_PREVW, B_THOLD,   /* PSP actions with no default key (None) */
       B_PAUSE, B_SELECT, B_CONFIRM, B_CANCEL, B_TAB_L, B_TAB_R, B_MENU_X, B_MENU_Y, B_NAV_UP, B_NAV_DOWN, B_NAV_LEFT, B_NAV_RIGHT,   /* menus */
       B_COUNT };
static struct { const char *id, *def; int vk; char name[16]; } g_bind[B_COUNT] = {
    { "jump", "Space" }, { "sprint", "Shift" }, { "crouch", "Ctrl" }, { "equipment", "C" }, { "secondary_fire", "G" },
    { "use", "E" }, { "cycle_secondary", "X" }, { "change_seat", "V" }, { "cycle_target", "T" }, { "stunt", "Q" }, { "autopilot", "R" },
    { "weapon1", "1" }, { "weapon2", "2" }, { "weapon3", "3" }, { "weapon4", "4" }, { "weapon5", "5" },
    { "hero_accept", "Y" }, { "hero_decline", "N" },
    { "forward", "W" }, { "back", "S" }, { "left", "A" }, { "right", "D" }, { "fire", "LMB" }, { "aim", "RMB" }, { "next_weapon", "Wheel" },
    { "recenter_camera", "None" }, { "target_search", "None" }, { "weapon_action", "R" }, { "sprint_toggle", "None" }, { "prev_weapon", "None" }, { "target_hold", "None" },
    { "pause", "P" }, { "select", "Tab" }, { "menu_confirm", "Enter" }, { "menu_back", "Esc" }, { "menu_tab_left", "Q" }, { "menu_tab_right", "E" },
    { "menu_option_1", "R" }, { "menu_option_2", "F" }, { "menu_up", "Up" }, { "menu_down", "Down" }, { "menu_left", "Left" }, { "menu_right", "Right" },
};
enum { VK_WHEEL = 0xFF };   /* "Wheel": not a key; a step of the mouse wheel */
static const struct { const char *n; int vk; } g_keynames[] = {
    { "Space", VK_SPACE }, { "Shift", VK_SHIFT }, { "Ctrl", VK_CONTROL }, { "Alt", VK_MENU }, { "Tab", VK_TAB },
    { "Enter", VK_RETURN }, { "Caps", VK_CAPITAL }, { "Mouse3", VK_MBUTTON }, { "Mouse4", VK_XBUTTON1 }, { "Mouse5", VK_XBUTTON2 },
    { "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 }, { "F5", VK_F5 }, { "F6", VK_F6 },
    { "F7", VK_F7 }, { "F8", VK_F8 }, { "F9", VK_F9 }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
    { "Esc", VK_ESCAPE }, { "Bksp", VK_BACK }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
};
static int key_parse(const char *t, char *name) {
    static const struct { const char *n; int vk; } M[] = { { "LMB", VK_LBUTTON }, { "RMB", VK_RBUTTON }, { "Wheel", VK_WHEEL } };   /* mouse: buttons read as captured clicks, the wheel as steps */
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) if (!_stricmp(t, M[i].n)) { strcpy(name, M[i].n); return M[i].vk; }
    for (size_t i = 0; i < sizeof g_keynames / sizeof g_keynames[0]; i++)
        if (!_stricmp(t, g_keynames[i].n)) { strcpy(name, g_keynames[i].n); return g_keynames[i].vk; }
    if (t[0] && !t[1] && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= '0' && t[0] <= '9'))) {
        name[0] = (char)(t[0] >= 'a' ? t[0] - 32 : t[0]); name[1] = 0; return name[0]; }
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { const int v = (int)strtol(t, NULL, 16); snprintf(name, 16, "VK%02X", v); return v; }
    return 0;
}
/* A bind problem, for the log (the F1 Controls page shows them itself). */
static void bind_note(const char *fmt, ...) {
    char s[160]; va_list a; va_start(a, fmt); vsnprintf(s, sizeof s, fmt, a); va_end(a);
    fprintf(stderr, "settings.ini: %s\n", s);
}
static const unsigned g_pad_def[B_COUNT]; static unsigned g_pad_of[B_COUNT];   /* below */
/* Every action takes a pad button ([pad]); the sticks move and aim as well. */
static int pad_bindable(int b) { (void)b; return 1; }
static void binds_load(void) {
    char ini[MAX_PATH], v[32]; GetCurrentDirectoryA(MAX_PATH - 16, ini); strcat(ini, "\\settings.ini");
    for (int i = 0; i < B_COUNT; i++) {
        if (!pad_bindable(i)) { g_pad_of[i] = 0; continue; }
        GetPrivateProfileStringA("pad", g_bind[i].id, "", v, sizeof v, ini);
        if (!v[0]) WritePrivateProfileStringA("pad", g_bind[i].id, psp_pad_bind_name(g_pad_def[i]), ini);
        g_pad_of[i] = v[0] ? psp_pad_bind_parse(v) : g_pad_def[i];
        if (v[0] && !g_pad_of[i] && _stricmp(v, "None")) { bind_note("Unknown pad button \"%s\" for %s: using %s", v, g_bind[i].id, psp_pad_bind_name(g_pad_def[i])); g_pad_of[i] = g_pad_def[i]; } }
    psp_pad_fire = g_pad_of[B_FIRE];
    {   /* the game's menus: PSP up, right, down, left, cross, circle, square, triangle, L, R, start, select */
        static const int M[12] = { B_NAV_UP, B_NAV_RIGHT, B_NAV_DOWN, B_NAV_LEFT, B_CONFIRM, B_CANCEL, B_MENU_X, B_MENU_Y, B_TAB_L, B_TAB_R, B_PAUSE, B_SELECT };
        for (int i = 0; i < 12; i++) psp_pad_map[i] = g_pad_of[M[i]]; }
    for (int i = 0; i < B_COUNT; i++) {
        GetPrivateProfileStringA("keys", g_bind[i].id, "", v, sizeof v, ini);
        if (!v[0]) WritePrivateProfileStringA("keys", g_bind[i].id, g_bind[i].def, ini);
        g_bind[i].vk = v[0] ? key_parse(v, g_bind[i].name) : 0;
        if (!_stricmp(v[0] ? v : g_bind[i].def, "None")) { strcpy(g_bind[i].name, "None"); g_bind[i].vk = 0; continue; }   /* unbound */
        {   /* keys the window layer keeps: F1 opens the PC menu; in menus Space selects and Backspace backs out */
            const int vk = g_bind[i].vk, menu = i >= B_PAUSE;
            if (vk == VK_F1 || (menu && vk == VK_SPACE && i != B_CONFIRM) || (menu && vk == VK_BACK && i != B_CANCEL)) {
                bind_note("%s is reserved (%s): %s uses %s", g_bind[i].name,
                        vk == VK_F1 ? "PC menu" : vk == VK_SPACE ? "menu select" : "menu back", g_bind[i].id, g_bind[i].def);
                v[0] = 0; g_bind[i].vk = 0; } }
        if (!g_bind[i].vk) { if (v[0]) bind_note("Unknown key \"%s\" for %s: using %s", v, g_bind[i].id, g_bind[i].def);
            g_bind[i].vk = key_parse(g_bind[i].def, g_bind[i].name); } }
    for (int i = 0; i < B_COUNT; i++) for (int j = i + 1; j < B_COUNT; j++)   /* one key, two actions in the same mode (play / menus) */
        if (g_bind[i].vk && g_bind[i].vk == g_bind[j].vk && (i >= B_PAUSE) == (j >= B_PAUSE)
            && !(i == B_AUTOPILOT && j == B_WACTION))   /* starfighter only / on foot only */
            bind_note("%s is bound to both %s and %s", g_bind[i].name, g_bind[i].id, g_bind[j].id);
}
/* The F1 menu's Controls page: every bind, in an order a player reads. */
static void binds_menu(void) {
    static const struct { int b; const char *label; int flags; } L[] = {
        { B_FWD, "Move forward" }, { B_BACK, "Move back" }, { B_LEFT, "Move left" }, { B_RIGHT, "Move right" },
        { B_JUMP, "Jump / boost" }, { B_SPRINT, "Sprint / throttle up" }, { B_SPRINT_TOG, "Sprint (toggle)" }, { B_CROUCH, "Crouch / throttle down" },
        { B_FIRE, "Fire" }, { B_AIM, "Aim / zoom" }, { B_CYCLE1, "Next weapon", PSP_BIND_WHEEL }, { B_PREVW, "Previous weapon (turret)" }, { B_THROW, "Secondary fire" },
        { B_CYCLE2, "Next secondary" }, { B_W1, "Weapon 1" }, { B_W2, "Weapon 2" }, { B_W3, "Weapon 3" }, { B_W4, "Weapon 4" }, { B_W5, "Weapon 5" },
        { B_USE, "Use / enter / land" }, { B_EQUIP, "Change equipment" }, { B_SEAT, "Change seat" },
        { B_TARGET, "Cycle target" }, { B_STUNT, "Stunt" }, { B_AUTOPILOT, "Autopilot" }, { B_HERO_YES, "Accept hero" }, { B_HERO_NO, "Decline hero" },
        { B_RECENTER, "Recenter camera (hold)" }, { B_TSEARCH, "Target search" }, { B_THOLD, "Target hold" }, { B_WACTION, "Reload" },
        { B_PAUSE, "Pause", PSP_BIND_MENU }, { B_SELECT, "Select (map)", PSP_BIND_MENU }, { B_CONFIRM, "Menu: confirm", PSP_BIND_MENU | PSP_BIND_SPACE },
        { B_CANCEL, "Menu: back", PSP_BIND_MENU | PSP_BIND_BKSP }, { B_TAB_L, "Menu: tab left", PSP_BIND_MENU }, { B_TAB_R, "Menu: tab right", PSP_BIND_MENU },
        { B_MENU_X, "Menu: option 1", PSP_BIND_MENU }, { B_MENU_Y, "Menu: option 2", PSP_BIND_MENU },
        { B_NAV_UP, "Menu: up", PSP_BIND_MENU }, { B_NAV_DOWN, "Menu: down", PSP_BIND_MENU }, { B_NAV_LEFT, "Menu: left", PSP_BIND_MENU }, { B_NAV_RIGHT, "Menu: right", PSP_BIND_MENU },
    };
    static psp_bind_t m[sizeof L / sizeof L[0]];
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) m[i] = (psp_bind_t){ g_bind[L[i].b].id, L[i].label, g_bind[L[i].b].def, L[i].flags,
        pad_bindable(L[i].b) ? psp_pad_bind_name(g_pad_def[L[i].b]) : NULL };
    psp_binds_set(m, (int)(sizeof L / sizeof L[0]));
    psp_cheats_set(k_cheats, (int)(sizeof k_cheats / sizeof *k_cheats), cheat_run);
}
/* Controller: each action also has a pad button (Xbox names; a PlayStation
 * pad's Cross is A, Circle B, Square X, Triangle Y). Right trigger fires and
 * left trigger aims (zoom) like the mouse buttons, the sticks move and aim
 * through the same direct paths as WASD and the mouse, Y cycles the primary
 * weapon like the wheel, and Start pauses (Back + Start: PC settings). */
static const unsigned g_pad_def[B_COUNT] = {   /* defaults; g_pad_of has them as bound */
    [B_JUMP] = PSP_PAD_A, [B_SPRINT] = PSP_PAD_LS, [B_CROUCH] = PSP_PAD_B, [B_EQUIP] = PSP_PAD_UP,
    [B_THROW] = PSP_PAD_LB, [B_USE] = PSP_PAD_X, [B_CYCLE2] = PSP_PAD_RB, [B_SEAT] = PSP_PAD_DOWN,
    [B_TARGET] = PSP_PAD_RIGHT, [B_STUNT] = PSP_PAD_RS, [B_AUTOPILOT] = PSP_PAD_LEFT, [B_WACTION] = PSP_PAD_LEFT,
    [B_HERO_YES] = PSP_PAD_LEFT, [B_HERO_NO] = PSP_PAD_RIGHT,
    [B_FIRE] = PSP_PAD_RT, [B_AIM] = PSP_PAD_LT, [B_CYCLE1] = PSP_PAD_Y,
    [B_PAUSE] = PSP_PAD_START, [B_SELECT] = PSP_PAD_BACK, [B_CONFIRM] = PSP_PAD_A, [B_CANCEL] = PSP_PAD_B,
    [B_TAB_L] = PSP_PAD_LB, [B_TAB_R] = PSP_PAD_RB, [B_MENU_X] = PSP_PAD_X, [B_MENU_Y] = PSP_PAD_Y,
    [B_NAV_UP] = PSP_PAD_UP, [B_NAV_DOWN] = PSP_PAD_DOWN, [B_NAV_LEFT] = PSP_PAD_LEFT, [B_NAV_RIGHT] = PSP_PAD_RIGHT,
};
static int pad_held(unsigned m) { return m && !psp_menu_open() && (psp_pad()->btn & m) != 0; }
/* The window layer's default keys (its PSP-button and stick keys) as bound here. */
static int key_remap(int vk) {
    static const struct { int vk, b; } R[] = {
        { 'W', B_FWD }, { 'S', B_BACK }, { 'A', B_LEFT }, { 'D', B_RIGHT }, { VK_SHIFT, B_SPRINT }, { VK_CONTROL, B_CROUCH },
        { 'Q', B_TAB_L }, { 'E', B_TAB_R }, { 'R', B_MENU_X }, { 'F', B_MENU_Y }, { 'P', B_PAUSE }, { VK_TAB, B_SELECT },
        { VK_RETURN, B_CONFIRM }, { VK_ESCAPE, B_CANCEL }, { VK_UP, B_NAV_UP }, { VK_DOWN, B_NAV_DOWN }, { VK_LEFT, B_NAV_LEFT }, { VK_RIGHT, B_NAV_RIGHT },
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) if (R[i].vk == vk) return g_bind[R[i].b].vk == VK_WHEEL ? 0 : g_bind[R[i].b].vk;
    return vk;
}
/* A bound key held: mouse buttons as the captured mouse has them, the wheel never (it steps). */
static int key_held(int b) { const int vk = g_bind[b].vk; if (!vk) return 0;
    return vk == VK_LBUTTON ? psp_mouse_left() : vk == VK_RBUTTON ? psp_mouse_right() : vk == VK_WHEEL ? 0 : psp_key_down(vk); }
#define BIND_DOWN(b) (key_held(b) || pad_held(g_pad_of[b]))
static int fire_held(void) { return BIND_DOWN(B_FIRE); }
/* A tap-style bind: 1 once per press (at the next read of its channel), as a PSP tap source gives one read. */
static int take_press(int b) { static long f[B_COUNT]; static int was[B_COUNT], pend[B_COUNT];
    if (f[b] != psp_game_frames) { f[b] = psp_game_frames; const int d = BIND_DOWN(b); if (d && !was[b]) pend[b] = 1; was[b] = d; }
    if (pend[b]) { pend[b] = 0; return 1; } return 0; }
/* Fire goes straight to the soldier (ch 8), ship (10) and ground vehicle (11) channels; anything
 * else (turrets, other seats) still gets the PSP's R, from the same bind: the window asks here. */
static volatile long g_fire_direct = -100; static int g_in_ui;
static int fire_r(void) { return !g_in_ui && psp_game_frames - g_fire_direct > 3 && fire_held(); }
static int aim_held(void)  { return BIND_DOWN(B_AIM); }
/* Movement: WASD, else the left stick (x right, y forward). */
static float move_x(void) { const int k = BIND_DOWN(B_RIGHT) - BIND_DOWN(B_LEFT); return k ? (float)k : psp_menu_open() ? 0.0f : psp_pad()->lx; }
static float move_y(void) { const int k = BIND_DOWN(B_FWD) - BIND_DOWN(B_BACK); return k ? (float)k : psp_menu_open() ? 0.0f : psp_pad()->ly; }
/* Right stick as an aim rate, radians per second at full tilt, with a
 * response curve (fine aim near the centre) and the stick sensitivity setting.
 * dt is the time since this caller's last frame. */
static void pad_aim(float *yaw, float *pitch, float dt) {
    *yaw = *pitch = 0.0f;
    if (psp_menu_open()) return;
    const psp_pad_t *p = psp_pad(); const psp_settings_t *st = psp_settings();
    const float k = (float)st->pad_sens / 100.0f;
    const float x = p->rx, y = p->ry;
    *yaw   = x * fabsf(x) * 3.4f * k * dt;
    *pitch = y * fabsf(y) * 2.2f * k * dt * (st->invert ? -1.0f : 1.0f);
}
static float frame_dt(LARGE_INTEGER *last) {
    LARGE_INTEGER n, f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f);
    float dt = last->QuadPart ? (float)(n.QuadPart - last->QuadPart) / (float)f.QuadPart : 0.0f;
    *last = n; return dt > 0.05f ? 0.05f : dt;
}
static uint32_t g_fly_obj; static float g_fix_yaw;   /* debug: constant flyer yaw input, console "fixyaw <percent>" */
static uint32_t g_board_pending; static int g_board_want;   /* run from con_run: not safe inside the soldier update */
static void qm_board_tick(void) {
    static long born = -1, done; const char *bv = getenv("SWBES_QM_BOARD"); const long f = psp_game_frames;
    if (!bv || done || !g_player_soldier) return;
    if (born < 0) { born = f; return; }
    if (f - born > 60) { const unsigned id = (unsigned)strtoul(bv, NULL, 16); g_board_pending = id == 1 ? 0 : id; g_board_want = 1; done = 1; }
}
/* The local player's object (not the soldier): 0xD19EC gives its id, looked
 * up in the player table at 0x5CA1CC the way Cheat.MakeHero does. */
static uint32_t local_player(void) {
    const uint32_t id = game_call(0x000D19ECu, 0, 0, 0);
    if (id == 999) return 0;
    const uint32_t tab = 0x005CA1CCu, buckets = psp_read32(tab), n = psp_read32(tab + 4);
    if (!buckets || !n) return 0;
    for (uint32_t e = psp_read32(buckets + 4 * (id & (n - 1))); e; e = psp_read32(e + 4)) {
        if (psp_read8(e + 8)) continue;
        const uint32_t o = psp_read32(e);
        if (o && psp_read32(o + 4) == id && !(psp_read16(o + 10) & 1)) return o;
    }
    return 0;
}
static void con_run(void) {
    if (g_board_want) { g_board_want = 0;
        const int tank = !_stricmp(getenv("SWBES_QM_BOARD") ? getenv("SWBES_QM_BOARD") : "", "tank");
        if (tank) g_want_vt = 0x0042AA00u; if (!_stricmp(getenv("SWBES_QM_BOARD") ? getenv("SWBES_QM_BOARD") : "", "turret")) { g_want_vt = 0x0042C998u; g_board_pending = 0; }
        if (!board(tank ? 0 : g_board_pending)) fprintf(stderr, "board: no vehicle\n");
        g_want_vt = 0x00428A60u; }
    if (!g_con_ready) return;
    char buf[1024]; memcpy(buf, g_con, sizeof buf); g_con_ready = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char name[128] = ""; long a1 = 0, a2 = 0; int help = 0;
        if (sscanf(line, "%127s %ld %ld", name, &a1, &a2) < 1) continue;
        if (!strcmp(name, "list")) { char f[128] = ""; sscanf(line, "%*s %127s", f);
            for (size_t i = 0; i < sizeof k_console_cmds / sizeof *k_console_cmds; i++)
                if (!*f || strstr(k_console_cmds[i].name, f)) fprintf(stderr, "console: %s\n", k_console_cmds[i].name);
            continue; }
        if (!strcmp(name, "spawn")) { if (!spawn_at((uint32_t)a1)) fprintf(stderr, "spawn: no command post %ld\n", a1); continue; }
        if (!strcmp(name, "curve")) {   /* debug: the flyer steering response 0x323C50(flyer, f12 = x) over x */
            for (int i = -10; i <= 10; i++) { g_saved_cpu = psp_cpu; psp_cpu.r[4] = g_fly_obj; psp_cpu.f[12] = (float)i / 10.0f;
                psp_cpu.r[29] = (psp_cpu.r[29] - 0x100u) & ~15u; psp_cpu.r[31] = 0; psp_call_from_host(0x00323C50u);
                const float o = psp_cpu.f[0]; psp_cpu = g_saved_cpu; fprintf(stderr, "curve %.1f -> %.3f\n", (float)i / 10.0f, o); }
            continue; }
        if (!strcmp(name, "pad")) { psp_ctrl_pulse((uint32_t)a1, 0, (int)a2); continue; }   /* debug: press PSP buttons a1 for a2 frames */
        if (!strcmp(name, "zwheel")) { g_zoom_wheel = (int)a1 ? 1 : -1; continue; }   /* debug: RMB+wheel step, 1 in / 0 out */
        if (!strcmp(name, "force")) { g_force_ch = a1; g_force_until = psp_game_frames + a2; continue; }   /* debug: hold soldier channel a1 for a2 frames */
        if (!strcmp(name, "chans")) {   /* debug: the soldier mapper's channel sources */
            const uint32_t m = a1 >= 2 ? g_gnd_map + 24u * (uint32_t)(a1 - 3) : g_hum_map ? g_hum_map - 24u * (uint32_t)a1 : 0, n = m ? psp_read32(m + 4) : 0, arr = m ? psp_read32(m + 8) : 0;   /* "chans 1": the Standard scheme mapper */
            for (uint32_t i = 0; i < n && i < 64; i++) { const uint32_t s = psp_read32(arr + 4 * i);
                fprintf(stderr, "chan %2u src %08X:", i, s);
                if (s) for (int w = 0; w < 10; w++) fprintf(stderr, " %08X", psp_read32(s + 4 * w));
                fprintf(stderr, "\n"); }
            continue; }
        if (!strcmp(name, "fixyaw")) { g_fix_yaw = (float)a1 / 100.0f; continue; }
        if (!strcmp(name, "vehicles")) { list_vehicles(0, NULL); continue; }
        if (!strcmp(name, "board")) { unsigned id = 0; char w[16] = "";
            if (sscanf(line, "%*s %15s", w) == 1 && !strcmp(w, "next")) {   /* cycle every vehicle on the map */
                static uint32_t last; const uint32_t v = next_vehicle(last);
                if (v) { last = v; fprintf(stderr, "board next: vt %08X\n", psp_read32(v)); }
                if (!v || !board(v)) fprintf(stderr, "board: no vehicle\n"); continue; }
            if (sscanf(line, "%*s %15s", w) == 1 && (!strcmp(w, "tank") || !strcmp(w, "turret"))) { g_want_vt = w[1] == 'u' ? 0x0042C998u : 0x0042AA00u; const int ok = board(0); g_want_vt = 0x00428A60u; if (!ok) fprintf(stderr, "board: no tank\n"); continue; }
            sscanf(line, "%*s %x", &id); if (!board(id)) fprintf(stderr, "board: no vehicle %X\n", id); continue; }
        if (!strcmp(name, "hero")) {   /* debug: local player object's hero-offer state (+272); "hero <n>" sets it */
            const uint32_t p = local_player();
            if (p && sscanf(line, "%*s %ld", &a1) == 1) psp_write32(p + 272, (uint32_t)a1);
            fprintf(stderr, "hero: player %08X state %d flags %08X\n", p, p ? (int)psp_read32(p + 272) : -1, p ? psp_read32(p + 140) : 0); continue; }
        if (!strcmp(name, "call")) { unsigned f = 0, x = 0, y = 0, z = 0; sscanf(line, "%*s %x %x %x %x", &f, &x, &y, &z);
            const uint32_t r = game_call(f, x, y, z); fprintf(stderr, "call: %08X(%08X, %08X, %08X) = %08X\n", f, x, y, z, r); continue; }
        if (!strcmp(name, "watchs")) { extern uint32_t g_player_soldier; void psp_mem_watch_write(uint32_t);
            psp_mem_watch_write(g_player_soldier + (uint32_t)a1); fprintf(stderr, "watch: soldier+%ld = %08X\n", a1, g_player_soldier + (uint32_t)a1); continue; }
        if (!strcmp(name, "help")) { help = 1; sscanf(line, "%*s %127s", name); }
        size_t i = 0, n = sizeof k_console_cmds / sizeof *k_console_cmds;
        while (i < n && _stricmp(k_console_cmds[i].name, name)) i++;
        if (i == n) { fprintf(stderr, "console: no command '%s'\n", name); continue; }
        g_con_echo = 1; game_call(k_console_cmds[i].fn, (uint32_t)help, (uint32_t)a1, (uint32_t)a2); g_con_echo = 0;
        fprintf(stderr, "console: ran %s%s\n", help ? "help " : "", k_console_cmds[i].name);
    }
}
static int g_play_wheel, g_wheel21, g_throw_ok, g_in_ui, g_tut_popup;   /* g_tut_popup: a tutorial popup, whose hints name play controls */
/* entry of the per-frame controller read (0x12C330): while G is held, the read reports the
 * input bound to the game's own throw-secondary action (d-pad Down) */
/* Jump and sprint, fed straight into the soldier's controls. The player's
 * control mapper (0xB720C updates it) turns pad buttons into numbered
 * channels; on foot channel 10 is "L tapped" (jump) and channel 20 "L held"
 * (sprint). The game reads a channel with GetValue (0xB73D0: a0 mapper, a1
 * channel, value at mapper+16[ch]); this runs at its entry and sets the value
 * there, so Space and Shift are their own controls and work together. */
static long g_jump_until = -1, g_crouch_until = -1, g_roll_ch = -1;

static int hum_mapper(uint32_t m) {
    if (m == g_hum_map) return 1;
    if (psp_read32(m + 4) < 21) return 0;
    const uint32_t arr = psp_read32(m + 8); if (!arr) return 0;
    const uint32_t s10 = psp_read32(arr + 40), s20 = psp_read32(arr + 80);
    if (!s10 || !s20 || psp_read32(s10) != 0x0041F180u || psp_read32(s10 + 16) != 459
        || psp_read32(s20) != 0x0041F120u || psp_read32(s20 + 16) != 459) return 0;
    g_hum_map = m; return 1;
}
/* Flyers (starfighters, gunships) steer by turn rate: the flyer input step
 * (0x002364D4, a0 = flyer) reads steering channels 0 (yaw) and 1 (pitch) from
 * its controller's mapper and turns them into angular velocity. The mouse
 * drives those channels with its own speed -- moving it turns the ship, at a
 * rate set by how fast it moves; still, the ship flies straight. */
static uint32_t g_fly_map; static long g_fly_frame = -100; static int g_fly_use, g_fly_board;
static void flyer_hook(uint32_t addr) { (void)addr;
    const uint32_t ctl = psp_read32(psp_cpu.r[4] + 68), m = ctl ? psp_read32(ctl + 8) : 0;
    if (!m) return;   /* AI flyers have no input mapper */
    if (psp_game_frames - g_fly_frame > 30) { g_use_pend = 0; g_fly_board = 1; }   /* just boarded: the E that got in must be let go before it can get out */
    g_fly_map = m; g_fly_frame = psp_game_frames; g_fly_obj = psp_cpu.r[4];
    if (g_fix_yaw != 0.0f && getenv("SWBES_VEHDBG")) { static long last; if (psp_game_frames - last >= 30) { last = psp_game_frames;
        fprintf(stderr, "fly turn in %.2f -> pitch %.3f yaw %.3f\n", g_fix_yaw, psp_read_f32(g_fly_obj + 736), psp_read_f32(g_fly_obj + 740)); } }
}
/* SWBES_MAPDUMP=1: each mapper the game reads (and its scheme twins at +-0x18), once:
 * every channel's source, then each new (mapper, channel, reader) as it happens. */
static int src_ok(uint32_t s) { if (!s || !psp_mem_ptr(s, 24)) return 0; const uint32_t v = psp_read32(s); return v >= 0x0041E000u && v < 0x00420000u; }
static void src_desc(uint32_t s, char *o, size_t n) {
    static const char *const bn[] = { [0x1C3 - 0x1C3] = "tri", "cir", "cro", "sq", [0x1CB - 0x1C3] = "L", "R", [0x1CF - 0x1C3] = "up", "down", "left", "right" };
    const uint32_t v = psp_read32(s), b = psp_read32(s + 16), t = psp_read32(s + 20);
    const char *nm = b >= 0x1C3 && b <= 0x1D2 && bn[b - 0x1C3] ? bn[b - 0x1C3] : NULL; char tmp[16];
    if (!nm) { snprintf(tmp, sizeof tmp, "%X", b); nm = tmp; }
    if (v == 0x0041F120u) snprintf(o, n, "%s hold", nm);
    else if (v == 0x0041F180u) snprintf(o, n, "%s %s", nm, t == 2 ? "double tap" : "tap");
    else if (v == 0x0041F0C0u) snprintf(o, n, "axis %u", b);
    else if (v == 0x0041F100u && src_ok(b) && src_ok(t)) { char a[48], c[48]; src_desc(b, a, sizeof a); src_desc(t, c, sizeof c); snprintf(o, n, "(%s + %s)", a, c); }
    else snprintf(o, n, "vt %08X b %X t %X", v, b, t);
}
static void map_dump(uint32_t m, uint32_t ch, uint32_t ra) {
    static uint32_t seen[64]; static int ns; int i;
    for (i = 0; i < ns && seen[i] != m; i++) {}
    if (i == ns && ns < 64) { seen[ns++] = m;
        for (uint32_t mm = m - 0x18u; mm <= m + 0x18u; mm += 0x18u) { const uint32_t src = psp_read32(mm + 8); if (!psp_mem_ptr(src, 4)) continue;
            int any = 0; for (int c = 0; c < 40; c++) { const uint32_t s = psp_read32(src + 4 * c); if (!src_ok(s)) continue; char d[128]; src_desc(s, d, sizeof d);
                fprintf(stderr, "mapdump %08X%s ch %d %s\n", mm, mm == m ? "*" : "", c, d); any = 1; }
            if (any) fprintf(stderr, "mapdump %08X end (first read ra %08X)\n", mm, ra); } }
    static uint32_t rs[1024]; static int nr; const uint32_t key = m ^ (ch << 24) ^ (ra << 3); int j;
    for (j = 0; j < nr && rs[j] != key; j++) {}
    if (j == nr && nr < 1024) { rs[nr++] = key; fprintf(stderr, "mapread %08X ch %u ra %08X f%ld\n", m, ch, ra, psp_game_frames); }
}
/* The upgrade-offer object (0x1E8788 updates it each frame, a0): state +4 (pending +8, set through
 * 0x1EBE00), offer timer +76, flags +128, mapper +36. SWBES_VEHDBG logs its changes. */
static uint32_t g_upg_obj;
static long g_seat_frame = -100;   /* last frame a turret or walker read its controller */
static void upg_update(uint32_t addr) { (void)addr; const uint32_t o = psp_cpu.r[4]; g_upg_obj = o;
    if (!getenv("SWBES_VEHDBG")) return;
    static uint32_t ls, lp, lf, lo; static int lt; const uint32_t s = psp_read32(o + 4), p = psp_read32(o + 8), fl = psp_read32(o + 128); const int t = psp_read_f32(o + 76) > 0.0f;
    if (o != lo || s != ls || p != lp || fl != lf || t != lt) { lo = o; ls = s; lp = p; lf = fl; lt = t;
        fprintf(stderr, "upg obj %08X state %u pend %u +48 %08X +76 %.2f +128 %08X +36 %08X get %08X f%ld\n", o, s, p, psp_read32(o + 48), psp_read_f32(o + 76), fl, psp_read32(o + 36), psp_read32(psp_read32(psp_read32(o + 36)) + 16), psp_game_frames); }}
static void upg_setstate(uint32_t addr) { (void)addr;
    if (getenv("SWBES_VEHDBG")) fprintf(stderr, "upg setstate obj %08X -> %u (from %u) ra %08X f%ld\n", psp_cpu.r[4], psp_cpu.r[5], psp_read32(psp_cpu.r[4] + 4), psp_cpu.r[31], psp_game_frames); }
static void ctl_get2(uint32_t addr);
static void ctl_get(uint32_t addr) { ctl_get2(addr);   /* SWBES_CHLOG: each channel's value after the PC writes, on change */
    if (getenv("SWBES_CHLOG") && psp_cpu.r[5] < 64) { const uint32_t m = psp_cpu.r[4], ch = psp_cpu.r[5]; static uint32_t ms[32]; static uint64_t nz[32]; int i;
        for (i = 0; i < 32 && ms[i] && ms[i] != m; i++) {}
        if (i < 32) { ms[i] = m; const float v = psp_read_f32(psp_read32(m + 16) + 4 * ch); const int on = v != 0.0f;
            if (on != (int)(nz[i] >> ch & 1)) { nz[i] ^= 1ull << ch; fprintf(stderr, "ch %08X %u %s %.2f ra %08X f%ld\n", m, ch, on ? "on" : "off", v, psp_read32(psp_cpu.r[29] + 8), psp_game_frames); } } } }
static long g_seat_any = -100;   /* last frame a turret or walker updated, menu or not */
static void ctl_get2(uint32_t addr) { (void)addr;
    const uint32_t m = psp_cpu.r[4], ch = psp_cpu.r[5];
    if (getenv("SWBES_MAPDUMP")) map_dump(m, ch, psp_read32(psp_cpu.r[29] + 8));
    if (getenv("SWBES_OTHERLOG") && m != g_hum_map && ch < 32) {   /* other controllers (turrets, vehicles): log each channel turning on / off */
        static uint32_t ms[16], nz[16]; int i; for (i = 0; i < 16 && ms[i] && ms[i] != m; i++) {}
        if (i < 16) { if (!ms[i]) { ms[i] = m; fprintf(stderr, "other new m %08X ra %08X f%ld\n", m, psp_read32(psp_cpu.r[29] + 8), psp_game_frames); }
            const int on = psp_read_f32(psp_read32(m + 16) + 4 * ch) != 0.0f;
            if (on != (int)(nz[i] >> ch & 1)) { nz[i] ^= 1u << ch; fprintf(stderr, "other m %08X ch %u %s %.2f ra %08X f%ld\n", m, ch, on ? "on" : "off", psp_read_f32(psp_read32(m + 16) + 4 * ch), psp_read32(psp_cpu.r[29] + 8), psp_game_frames); } } }
    if (getenv("SWBES_READLOG"))fprintf(stderr, "rd f%ld m %08X%s ch %u v %.2f ra %08X\n", psp_game_frames, m, m == g_hum_map ? "*" : "", ch, psp_read_f32(psp_read32(m + 16) + 4 * ch), psp_read32(psp_cpu.r[29] + 8));
    if (getenv("SWBES_VEHDBG") && (m == g_fly_map || m == g_fly_map + 0x18u) && ch < 64) {   /* each new reader / channel pair of the ship's controls */
        static uint32_t seen[256]; static int ns; const uint32_t key = (psp_read32(psp_cpu.r[29] + 8) << 6) ^ ch; int i;
        for (i = 0; i < ns && seen[i] != key; i++) {}
        if (i == ns && ns < 256) { seen[ns++] = key; fprintf(stderr, "flyer read ch %u ra %08X f%ld\n", ch, psp_read32(psp_cpu.r[29] + 8), psp_game_frames); } }
    if (getenv("SWBES_VEHDBG") && (m == g_fly_map || m == g_fly_map + 0x18u) && ch < 32) { static uint32_t nz;
        const int on = psp_read_f32(psp_read32(m + 16) + 4 * ch) != 0.0f;
        if (on != (int)(nz >> ch & 1)) { nz ^= 1u << ch; fprintf(stderr, "flyer ch %u %s (%.2f)\n", ch, on ? "on" : "off", psp_read_f32(psp_read32(m + 16) + 4 * ch)); } }
    /* The player's flyer controller holds two mappers, the default scheme's
     * at +0 and the Alternate scheme's at +0x18; the ship reads whichever the
     * profile picked. Channels mean the same in both: 0 yaw, 1 pitch,
     * 3 boost (L held), 10 primary fire (R). */
    /* the player's ship controller, whoever reads it: other updates (0x20EBxx, 0x2508xx) read it too, e.g. around the transition */
    const int fly = (m == g_fly_map || m == g_fly_map + 0x18u) && psp_game_frames - g_fly_frame <= 600 && !g_in_ui;
    if (getenv("SWBES_VEHDBG") && fly && ch == 6 && psp_game_frames - g_fly_frame > 2) fprintf(stderr, "flyer ch6 read off-update ra %08X\n", psp_read32(psp_cpu.r[29] + 8));
    if (fly && (long)ch == g_force_ch && psp_game_frames <= g_force_until) psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);   /* debug force, ship */
    /* Manned turrets (the ion cannon): their own controller, read from 0x2472xx-0x2477xx,
     * aiming on channels 0 / 1 like a ship. Steered the same way: mouse + WASD. */
    static long tur_frame = -100; psp_fire_r = fire_r;
    const uint32_t caller = psp_read32(psp_cpu.r[29] + 8);
    /* Upgrade offer ("You can now upgrade your flyer", E803 accept / E804 decline): the offer object's
     * controller (+36, GetValue 0x321DC8) passes the read on to this one, so its caller's return is at sp+0:
     * 0x1E893C (ch 3, accept: event 0x80B0) and 0x1E89CC (ch 4, decline); 0x1E8A08 / 0x1E8A2C read the
     * same pair in the other offer state. One read per press. */
    const uint32_t outer = psp_read32(psp_cpu.r[29]);
    if (!g_in_ui && ((ch == 3 && (outer == 0x001E893Cu || outer == 0x001E8A08u) && take_press(B_HERO_YES))
                  || (ch == 4 && (outer == 0x001E89CCu || outer == 0x001E8A2Cu) && take_press(B_HERO_NO)))) {
        psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f); if (getenv("SWBES_VEHDBG")) fprintf(stderr, "upgrade offer: %s\n", ch == 3 ? "accept" : "decline"); }
    /* Turret (0x2472B0) and walker (0x2507A0) updates: same actions, different channels. */
    const int tur_upd = caller >= 0x00247200u && caller < 0x00247900u && !g_in_ui, walk = caller >= 0x00250800u && caller < 0x00250B00u && !g_in_ui;
    if ((caller >= 0x00247200u && caller < 0x00247900u) || (caller >= 0x00250800u && caller < 0x00250B00u)) g_seat_any = psp_game_frames;
    if (tur_upd || walk) {
        static int was[2][8]; if (psp_game_frames - tur_frame > 30) { was[walk][0] = 1; g_use_pend = 0; }   /* just got in: the E that did it must be let go first */
        tur_frame = g_seat_frame = psp_game_frames;
        static const int T[2][8] = { { 7, 8, 9, 11, 12, 6, 15, 16 }, { 3, 4, 10, 6, 7, 5, -1, -1 } };   /* get out, seat, target, fire, secondary, hold, prev, next */
        const int *c = T[walk]; int on = 0, i;
        for (i = 0; i < 8 && c[i] != (int)ch; i++) {}
        if (i < 8) {
            const int d = i == 0 ? BIND_DOWN(B_USE) : i == 1 ? BIND_DOWN(B_SEAT) : i == 2 ? BIND_DOWN(B_TARGET) || BIND_DOWN(B_TSEARCH) : i == 3 ? fire_held()
                        : i == 4 ? BIND_DOWN(B_THROW) : i == 5 ? BIND_DOWN(B_THOLD) || aim_held() : i == 6 ? BIND_DOWN(B_PREVW) || BIND_DOWN(B_CYCLE2) : 0;   /* prev: also next secondary (X, pad RB) */
            if (i <= 2 || i == 6) { on = d && !was[walk][i]; was[walk][i] = d; } else on = d;   /* taps: one read per press */
            if (i == 0 && on) { g_use_pend = 0; g_use_frame = -1; }   /* this press is the exit: on foot it would re-enter */
            if (i == 7 && g_play_wheel) { g_play_wheel = 0; on = 1; }   /* next weapon: wheel / its key / pad */
            if (i == 3) g_fire_direct = psp_game_frames;
            if (on) { psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f); if (getenv("SWBES_VEHDBG") && (i <= 2 || i >= 6)) fprintf(stderr, "%s ch %u (%d)\n", walk ? "walker" : "turret", ch, i); } } }
    static float walk_pitch;   /* walker: 11 is pitch (+736), 1 drives (+748); the aim block below fills 1 with mouse y, moved here */
    if (walk && ch == 11) { if (walk_pitch != 0.0f) psp_write_f32(psp_read32(m + 16) + 4 * ch, walk_pitch); walk_pitch = 0.0f; return; }
    const int tur = tur_upd || walk;
    /* Ground vehicles (0x2409D4): aim on channels 0 / 1 like a ship, drive from the PSP stick (WASD).
     * 6 = target lock; 7 = get out (up tap), read outside 0x2409D4, so matched by mapper. */
    static long gnd_frame = -100;
    const int gnd = caller >= 0x00240A00u && caller < 0x00240B20u && !g_in_ui;
    static int gnd_was;   /* use held: get out on the next press, not the one that got in */
    if (gnd) { if (psp_game_frames - gnd_frame > 30) { gnd_was = 1; g_use_pend = 0; } g_gnd_map = m; gnd_frame = psp_game_frames; }
    if (gnd && (long)ch == g_force_ch && psp_game_frames <= g_force_until) { psp_write_f32(psp_read32(m + 16) + 4 * ch, g_force_ch == 1 ? -1.0f : 1.0f); return; }   /* debug force */
    if (ch == 7 && m == g_gnd_map && psp_game_frames - gnd_frame <= 2 && !g_in_ui) { const int u = BIND_DOWN(B_USE);   /* use: get out, on the press */
        if (u && !gnd_was) { g_use_pend = 0; g_use_frame = -1;   /* this press is the exit: on foot it would re-enter */
            psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f); if (getenv("SWBES_VEHDBG")) fprintf(stderr, "ground vehicle: get out\n"); } gnd_was = u; }
    if (ch == 12 && m == g_gnd_map && psp_game_frames - gnd_frame <= 2 && !g_in_ui && BIND_DOWN(B_THROW))
        psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);   /* secondary fire (missiles): the PSP's down hold */
    if (ch == 4 && m == g_gnd_map && psp_game_frames - gnd_frame <= 2 && !g_in_ui && (BIND_DOWN(B_JUMP) || aim_held()))
        psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);   /* the PSP's L hold (sets +1164 bit 8), as the ship's boost: Space */
    if (ch == 8 && m == g_gnd_map && psp_game_frames - gnd_frame <= 2 && !g_in_ui) { static int was; const int s = BIND_DOWN(B_SEAT);   /* change seat: the PSP's left tap */
        if (s && !was) psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f); was = s; }
    if ((fly && ch == 10) || (ch == 11 && m == g_gnd_map && psp_game_frames - gnd_frame <= 2 && !g_in_ui)) g_fire_direct = psp_game_frames;
    if (ch == 11 && m == g_gnd_map && psp_game_frames - gnd_frame <= 2 && !g_in_ui && fire_held())
        psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);   /* ground vehicle: primary fire (the PSP's R hold) */
    if (fly && ch == 10 && fire_held()) psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);   /* left button: fire */
    if (fly && (long)ch == g_fly_pend_ch) { g_fly_pend_ch = -1; psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f); }   /* stunt / autopilot, one read */
    /* use: the PSP's up tap feeds channel 6 and 18 both; 18 is what transitions in a transition region */
    static int use18_was;   /* a tap on the PSP: one read per press, or holding E would keep toggling autopilot */
    if (fly && ch == 18) { const int first = g_fly_use && !use18_was; use18_was = g_fly_use; if (first) { psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);
        if (getenv("SWBES_VEHDBG")) fprintf(stderr, "flyer ch18 (use) f%ld\n", psp_game_frames); } }
    if (fly && ch == 6) {   /* use: land / get out (d-pad up): on from the press while held, every reader sees it */
        static int on; const int u = BIND_DOWN(B_USE);
        if (g_fly_board) { if (u) { g_use_pend = 0; on = 0; } else g_fly_board = 0; }   /* the press that boarded */
        if (g_use_pend) { g_use_pend = 0; on = 1; }   /* a tap shorter than a frame still gives one read */
        else if (!u) on = 0;
        g_fly_use = on;   /* ch 7 is read after 6 in the same update */
        if (on) psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);
        if (getenv("SWBES_VEHDBG") && on) fprintf(stderr, "flyer use ra %08X f%ld obj %08X 1164 %08X 1168 %08X 1172 %08X\n", psp_read32(psp_cpu.r[29] + 8), psp_game_frames,
            g_fly_obj, psp_read32(g_fly_obj + 1164), psp_read32(g_fly_obj + 1168), psp_read32(g_fly_obj + 1172)); }
    if (fly && ((ch == 11 && BIND_DOWN(B_THROW)) || (ch == 8 && BIND_DOWN(B_SEAT))))
        psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);   /* secondary fire / change seat */
    if (fly && (ch == 17 || ch == 19)) {   /* T as the PSP's d-pad right: a tap (released inside 20 frames) cycles the target (19, one read), holding it is roll correct (17) */
        static long down_at = -1, seen = -1; static int was, tap; const int t = BIND_DOWN(B_TARGET);
        if (seen != psp_game_frames) { seen = psp_game_frames;
            if (t && !was) down_at = psp_game_frames;
            if (!t && was && psp_game_frames - down_at < 20) tap = 1;
            was = t; }
        if (ch == 19 && tap) { tap = 0; psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f); }
        if (ch == 17 && t && psp_game_frames - down_at >= 20) psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f); }
    if (fly && ch == 3 && (BIND_DOWN(B_JUMP) || aim_held())) psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);   /* Space, or aim (LT / RMB, unused in a ship): boost */
    if (ch < 2 && (fly || tur || gnd)) {
        static long took = -1; static float v[2];
        if (took != psp_game_frames) { took = psp_game_frames;
            const psp_settings_t *st = psp_settings();
            const float k = (float)st->sens_pct / 100.0f * (float)st->veh_sens / 100.0f / 10.0f;   /* 10 px per frame = full turn */
            long dx, dy; psp_mouse_take(&dx, &dy);
            /* A virtual stick the mouse pushes and that eases back to centre:
             * raw per-frame deltas made the ship jerk on each tick and stop
             * dead between them. Steady motion settles at 3x the raw rate; up to 2x a full stick, past
             * the PSP limit, since its full-stick turn felt stiff with a mouse. */
            v[0] = v[0] * 0.85f + (float)dx * k * 0.45f;
            v[1] = v[1] * 0.85f - (float)dy * k * 0.45f * (st->invert ? -1.0f : 1.0f);   /* mouse up = nose up */
            for (int i = 0; i < 2; i++) if (v[i] > -0.01f && v[i] < 0.01f) v[i] = 0.0f;
            for (int i = 0; i < 2; i++) v[i] = v[i] > 2.0f ? 2.0f : v[i] < -2.0f ? -2.0f : v[i];
            if (getenv("SWBES_VEHDBG") && (dx || dy)) fprintf(stderr, "flyer m %08X in %.2f %.2f\n", m, v[0], v[1]); }
        if (ch == 0 && g_fix_yaw != 0.0f) { psp_write_f32(psp_read32(m + 16), g_fix_yaw); return; }   /* debug: console "fixyaw <percent>" */
        /* WASD steer on top of the mouse: A / D yaw, W / S nose up / down.
         * While flying the window keeps WASD off the stick and puts the
         * throttle on Shift / Ctrl instead (psp_keys_fly). */
        if (!gnd && !walk) psp_keys_fly();   /* walker: WASD stay on the PSP stick (it drives) */
        const float key = gnd ? 0.0f : ch == 0 ? (float)(BIND_DOWN(B_RIGHT) - BIND_DOWN(B_LEFT))
                                  : (float)(BIND_DOWN(B_FWD) - BIND_DOWN(B_BACK));
        /* Right stick: these channels are turn rates, so the stick goes in as
         * one (curved, up to 1.5x a full PSP stick). The left stick is the
         * throttle here, through the PSP stick. */
        float ps = 0.0f;
        if (!psp_menu_open()) { const psp_pad_t *p = psp_pad(); const float a = ch == 0 ? p->rx : p->ry;
            ps = a * fabsf(a) * 1.5f * (float)psp_settings()->pad_sens / 100.0f * (float)psp_settings()->veh_sens / 100.0f * (ch == 1 && psp_settings()->invert ? -1.0f : 1.0f); }
        if (walk && ch == 1) { walk_pitch = (v[1] + ps) * 3.0f;   /* same sign as 1 (a minus inverted it); 11 turns slowly, so 3x */   /* W / S, left stick: drive */
            const float d = (float)(BIND_DOWN(B_FWD) - BIND_DOWN(B_BACK)) + (psp_menu_open() ? 0.0f : psp_pad()->ly);
            if (d != 0.0f) psp_write_f32(psp_read32(m + 16) + 4, d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d);
            return; }
        const float out = v[ch] + key + ps;
        if (out != 0.0f) psp_write_f32(psp_read32(m + 16) + 4 * ch, out);
        return;
    }
    if (getenv("SWBES_HUMDBG") && m == g_hum_map && ch < 64) { static uint64_t seen;
        if (!(seen >> ch & 1)) { seen |= 1ull << ch; fprintf(stderr, "hum read ch %u ra %08X caller %08X\n", ch, psp_cpu.r[31], psp_read32(psp_cpu.r[29] + 8)); } }
    if (getenv("SWBES_HUMDBG") && m == g_hum_map && ch < 64) { static uint64_t nz;
        const int on = psp_read_f32(psp_read32(m + 16) + 4 * ch) != 0.0f;
        if (on != (int)(nz >> ch & 1)) { nz ^= 1ull << ch; fprintf(stderr, "hum ch %u %s (%.2f) f%ld\n", ch, on ? "on" : "off", psp_read_f32(psp_read32(m + 16) + 4 * ch), psp_game_frames); } }
    if (m == g_hum_map && (long)ch == g_force_ch && psp_game_frames <= g_force_until) psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f);
    if (getenv("SWBES_HUMDBG") && m == g_hum_map && ch == 7) { static long last; if (psp_game_frames - last > 20 || psp_game_frames <= g_crouch_until || psp_game_frames <= g_force_until + 2) { last = psp_game_frames; fprintf(stderr, "crouch s2 %08X +796 %08X\n", psp_cpu.r[18], psp_read32(psp_cpu.r[18] + 796)); } }
    if (m == g_hum_map && ch == 8 && !g_in_ui && psp_game_frames - tur_frame > 2) g_fire_direct = psp_game_frames;   /* on foot, not in a turret */
    if (m == g_hum_map && !g_in_ui && ((ch == 23 && ((g_use_pend && !(g_use_pend = 0)) || psp_game_frames == g_use_frame)) || (ch == 21 && g_wheel21 && !(g_wheel21 = 0)) || (ch == 28 && g_block) || (ch == 9 && BIND_DOWN(B_THROW)) || (ch == 22 && take_press(B_CYCLE2)) || (ch == 24 && (BIND_DOWN(B_EQUIP) || psp_game_frames == g_use_frame)) || (ch == 8 && fire_held())
        || (ch == 18 && BIND_DOWN(B_RECENTER)) || (ch == 19 && take_press(B_TSEARCH)) || (ch == 27 && take_press(B_WACTION))))
        {
        psp_write_f32(psp_read32(m + 16) + 4 * ch, 1.0f); }   /* use (hold = melee block) / secondary fire / change equipment (Alternate: d-pad left) */
    if ((ch > 5 && ch != 10 && ch != 20 && ch != 7 && ch != 17 && ch != 29 && ch != 30 && ch != 31 && (ch < 11 || ch > 14)) || ch < 4 || g_in_ui || !hum_mapper(m)) return;
    const uint32_t out = psp_read32(m + 16) + 4 * ch;
    /* WASD move the soldier directly, full speed at once,
     * no stick smoothing (4 = forward, 5 = right). SWBES_STICK_MOVE=1 restores the stick path; direct once felt
     * worse than the game's own ramp through the stick. */
    if (getenv("SWBES_MOVELOG") && (ch == 4 || ch == 5)) fprintf(stderr, "move ch %u %.3f f%ld\n", ch, psp_read_f32(out), psp_game_frames);
    if ((ch == 4 || ch == 5 || ch == 30 || ch == 31) && getenv("SWBES_STICK_MOVE")) return;
    if ((ch == 4 || ch == 5 || ch == 30 || ch == 31) && psp_game_frames - tur_frame <= 2) return;   /* in a turret: WASD aim it (the soldier still reads its movement) */
    if (ch == 4 || ch == 5 || ch == 30 || ch == 31) {   /* 30 / 31: the same axes, read instead of 4 / 5 when 0x275C44 says so (zoomed) */
        psp_keys_foot();
        const float x = move_x(), y = move_y();
        /* no diagonal normalising: the game's own stick gives (1, 1) on W+D */
        if (x != 0.0f || y != 0.0f) psp_write_f32(out, ch == 4 || ch == 30 ? y : x);
        return; }
    if (ch == 10 && psp_game_frames <= g_jump_until) psp_write_f32(out, 1.0f);
    if (ch == 7 && g_crouch_until >= 0) { g_crouch_until = -1; psp_write_f32(out, 1.0f); }   /* Ctrl: crouch toggle (moving sideways = roll) */
    /* Ctrl + direction: roll. The Standard scheme rolls on Circle tapped with a
     * stick direction (channels 11 left, 12 right, 13 / 14 stick y); the soldier
     * reads them under either scheme. */
    if ((long)ch == g_roll_ch) { g_roll_ch = -1; psp_write_f32(out, 1.0f); }
    if (ch == 17) {   /* right mouse held = zoomed: press the zoom toggle whenever the weapon disagrees (a reload drops the zoom) */
        /* Zoomed is the soldier's camera, soldier(s2)+72 -> +8 (0x212F4C). The
         * weapon's +332 is only the zoom LEVEL: 0 on a one-level scope even
         * while zoomed, which made this press the toggle every 12 frames. */
        static long last = -100; const uint32_t cam = psp_read32(psp_cpu.r[18] + 72);
        const int zoomed = cam >= 0x08800000u && cam < 0x0A000000u && psp_read32(cam + 8) != 0;
        if (aim_held() != zoomed && psp_game_frames - last >= 6) { last = psp_game_frames; psp_write_f32(out, 1.0f); }
        /* Scope levels: weapon +312 = level count, +332 = level, +316[] = factors.
         * Zoom (17) while zoomed steps a level in but wraps to unzoomed after the
         * last; 29 steps one out but unzooms from level 0. RMB + wheel uses each
         * only where it stays zoomed. */
        else if (g_zoom_wheel > 0) { g_zoom_wheel = 0;
            const uint32_t inv = psp_read32(psp_cpu.r[18] + 464), w = inv ? psp_read32(inv + 4 * psp_read8(inv + 120)) : 0;
            if (zoomed && aim_held() && w && psp_read32(w + 332) + 1 < psp_read32(w + 312)) psp_write_f32(out, 1.0f); }
        if (getenv("SWBES_ZOOMLOG")) { const uint32_t s2 = psp_cpu.r[18], inv = psp_read32(s2 + 464);
            const uint32_t w = inv ? psp_read32(inv + 4 * psp_read8(inv + 120)) : 0;
            fprintf(stderr, "zoom f%ld zoomed %d rmb %d v %.0f s216 %d s636 %d s640 %d s644 %d b10 %u c20 %u lv %u wz %u\n", psp_game_frames, zoomed, psp_mouse_right(), psp_read_f32(out),
                (int)psp_read32(s2 + 216), (int)psp_read32(s2 + 636), (int)psp_read32(s2 + 640), (int)psp_read32(s2 + 644),
                (psp_read32(s2 + 796) >> 10) & 1, psp_read32(psp_read32(s2 + 68) + 20), w ? psp_read32(w + 312) : 99, w ? psp_read32(w + 332) : 99); } }
    if (ch == 29 && g_zoom_wheel < 0) { g_zoom_wheel = 0;   /* scope one level out (never past level 0, which would unzoom) */
        const uint32_t s2 = psp_cpu.r[18], cam = psp_read32(s2 + 72), inv = psp_read32(s2 + 464), w = inv ? psp_read32(inv + 4 * psp_read8(inv + 120)) : 0;
        if (cam >= 0x08800000u && cam < 0x0A000000u && psp_read32(cam + 8) && w && psp_read32(w + 332) > 0) psp_write_f32(out, 1.0f); }
    if (ch == 20) { static int tog;   /* toggle sprint: a press flips it; stopping or the hold bind ends it */
        if (take_press(B_SPRINT_TOG)) tog ^= 1;
        if ((move_x() == 0.0f && move_y() == 0.0f) || BIND_DOWN(B_SPRINT)) tog = 0;
        if (tog || BIND_DOWN(B_SPRINT)) psp_write_f32(out, 1.0f); }
}
/* PC key prompts. The game's text carries button glyphs as private-use
 * characters that name an ACTION ("Press U+E848 to shoot"); the draw-text
 * function 0xF0A10 (a0 = UTF-16 string, a2 = length or -1) turns them into
 * PSP button icons. Rewrite the string first, swapping each known action for
 * the PC key that does it, into a ring of buffers in RAM the game never uses
 * (below its load address). Unknown codes keep their PSP icon. */
/* PC key caps on the button-icon sheet. Menu prompts ("(X) Select") and any
 * hint code we don't rewrite draw a cell of one 256x128 icon texture; the
 * renderer hands us each decoded texture, and for that one we return a 4x
 * copy with the PSP buttons replaced by key caps naming the PC key bound to
 * that button (window.c). Recognised by size and its alpha footprint. */
/* One pixel of a key cap: a rounded rectangle (radius a quarter of the short
 * side) with a cyan border and a navy fill, content coverage t (0..255) in
 * white. Edges are anti-aliased from the exact distance to the outline, so
 * the corners stay round when the game scales the cap. tint: as iconcap's. */
static uint32_t cap_px(int x, int y, int cw, int ch, int t, uint32_t tint) {
    /* border a fixed share of the cap: the sheets' caps are 21 to 34 texels and drawn at
     * different scales, so a fixed width came out thick on some and thin on others */
    const float r = (cw < ch ? cw : ch) / 4.0f, bw = (cw < ch ? cw : ch) * 0.05f, px = x + .5f, py = y + .5f;
    const float qx = fabsf(px - cw * .5f) - (cw * .5f - r), qy = fabsf(py - ch * .5f) - (ch * .5f - r);
    const float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0, d = sqrtf(ax * ax + ay * ay) + fminf(fmaxf(qx, qy), 0) - r;
    const float cov = fminf(fmaxf(.5f - d, 0), 1), e = fminf(fmaxf(d + bw + .5f, 0), 1);   /* inside; on the border band */
    if (cov <= 0) return 0;
    float R = 0x0C + (255 - 0x0C) * t / 255.f, G = 0x1C + (255 - 0x1C) * t / 255.f, B = 0x2C + (255 - 0x2C) * t / 255.f;
    R += (0x40 - R) * e; G += (0xC0 - G) * e; B += (0xF0 - B) * e;
    if (tint) { R = R * 255 / (tint & 255); G = G * 255 / (tint >> 8 & 255); B = B * 255 / (tint >> 16 & 255); }
    const int ri = R > 255 ? 255 : (int)(R + .5f), gi = G > 255 ? 255 : (int)(G + .5f), bi = B > 255 ? 255 : (int)(B + .5f);
    return (uint32_t)ri | (uint32_t)gi << 8 | (uint32_t)bi << 16 | (uint32_t)(cov * (235 + 20 * e) + .5f) << 24;
}
static int g_kc_txt = 62;   /* key cap text height, percent of the cap (hint caps are flatter and use more) */
static void keycap(uint32_t *img, int iw, int x0, int y0, int cw, int ch, const char *txt) {
    /* text drawn by GDI at 4x and averaged down: at 13-21 texel caps GDI's hinted
     * small sizes came out visibly smaller than the same rule on bigger caps.
     * One rule for all caps: font height 62% of the cap, shrunk only to fit the width. */
    enum { SS = 4 };
    const int W = cw * SS, H = ch * SS;
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) img[(y0 + y) * iw + x0 + x] = 0;
    BITMAPINFO bi = { 0 }; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL; HDC dc = CreateCompatibleDC(NULL); HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dc || !bm) { if (dc) DeleteDC(dc); return; }
    HGDIOBJ ob = SelectObject(dc, bm); memset(bits, 0, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    HFONT f = NULL; SIZE sz = { 0 }; wchar_t w[32]; const int n = MultiByteToWideChar(CP_UTF8, 0, txt, -1, w, 32) - 1;   /* UTF-8: arrows */
    for (int fh = H * g_kc_txt / 100; fh > 6; fh--) {
        if (f) { SelectObject(dc, GetStockObject(SYSTEM_FONT)); DeleteObject(f); }
        f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI Symbol");
        SelectObject(dc, f); GetTextExtentPoint32W(dc, w, n, &sz);
        if (sz.cx <= W - W / 5 && sz.cy <= H) break;
    }
    RECT rc = { 0, 0, W, H }; DrawTextW(dc, w, n, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    const uint8_t *p = bits;
    /* centre the ink, not the font's line box (it sits high, and symbols sit anywhere in it) */
    int ix0 = W, iy0 = H, ix1 = -1, iy1 = -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (p[(y * W + x) * 4 + 1] > 24) {
        if (x < ix0) ix0 = x; if (x > ix1) ix1 = x; if (y < iy0) iy0 = y; if (y > iy1) iy1 = y; }
    int ox = ix1 < 0 ? 0 : (ix0 + ix1 + 1 - W) / 2, oy = ix1 < 0 ? 0 : (iy0 + iy1 + 1 - H) / 2;
    /* Letters and digits centre on the capital height, baseline to cap top,
     * like printed key legends: by ink, "Space" (its p hangs down) and
     * "Shift" (no descender) sat at different heights. Symbols keep ink centring. */
    int alnum = n > 0; for (int i = 0; i < n; i++) if (!iswalnum(w[i])) alnum = 0;
    if (alnum) {   /* measured: an "H" drawn the same way spans exactly cap top to baseline */
        uint8_t *keep = malloc((size_t)W * H * 4);
        if (keep) { memcpy(keep, bits, (size_t)W * H * 4); memset(bits, 0, (size_t)W * H * 4);
            DrawTextW(dc, L"H", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); GdiFlush();
            int t = H, b = -1; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (p[(y * W + x) * 4 + 1] > 24) { if (y < t) t = y; b = y; }
            if (b >= 0) oy = (t + b + 1 - H) / 2;
            memcpy(bits, keep, (size_t)W * H * 4); free(keep); } }
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
        int s = 0;
        for (int j = 0; j < SS; j++) for (int i = 0; i < SS; i++) {
            const int sx = x * SS + i + ox, sy = y * SS + j + oy;
            if (sx >= 0 && sy >= 0 && sx < W && sy < H) s += p[(sy * W + sx) * 4 + 1]; }
        img[(y0 + y) * iw + x0 + x] = cap_px(x, y, cw, ch, s / (SS * SS), 0);
    }
    SelectObject(dc, ob); if (f) DeleteObject(f); DeleteObject(bm); DeleteDC(dc);
}
/* A key cap showing an icon instead of text: keycap()'s frame around a 56x56
 * coverage table (icon_*.h, third-party icons, see THIRD_PARTY.md), scaled to
 * pct percent of the cap's height. tint: the colour the game will multiply the
 * sprite by (0 = none); the cap's colours are divided by it so they land right
 * where they can (a tint below white can't be undone for white). */
#include "icon_enter_key.h"
#include "icon_mouse_scroll.h"
static void iconcap(uint32_t *img, int iw, int x0, int y0, int cw, int ch, const char (*tab)[57], int pct, uint32_t tint) {
    enum { N = 56 };
    const int sz = ch * pct / 100, ox = (cw - sz) / 2, oy = (ch - sz) / 2;
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
        int t = 0;
        if (x >= ox && y >= oy && x < ox + sz && y < oy + sz) {   /* bilinear sample of the table */
            const float u = (x - ox + .5f) * N / sz - .5f, v = (y - oy + .5f) * N / sz - .5f;
            const int iu = (int)floorf(u), iv = (int)floorf(v); const float fu = u - iu, fv = v - iv; float s = 0;
            for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) { const int a = iu + i, b = iv + j;
                if (a < 0 || b < 0 || a >= N || b >= N) continue;
                const char c = tab[b][a]; s += (c <= '9' ? c - '0' : c - 'a' + 10) * (i ? fu : 1 - fu) * (j ? fv : 1 - fv); }
            t = (int)(s * 17 + .5f);
        }
        img[(y0 + y) * iw + x0 + x] = cap_px(x, y, cw, ch, t, tint);
    }
}
/* The legend a key name is drawn with: the arrow keys as arrows. */
static const char *key_glyph(const char *k) {
    return !strcmp(k, "Up") ? "↑" : !strcmp(k, "Down") ? "↓" : !strcmp(k, "Left") ? "←" : !strcmp(k, "Right") ? "→" : k;
}
static void capdraw(uint32_t *img, int iw, int x0, int y0, int cw, int ch, const char *k) {   /* keycap, or an icon cap for keys that have one */
    if (!strcmp(k, "Enter")) iconcap(img, iw, x0, y0, cw, ch, icon_enter_key, 64, 0);
    else keycap(img, iw, x0, y0, cw, ch, key_glyph(k));
}
/* Pad glyphs, drawn into the sheet's free right half (x 150.., y 26..). Text
 * uses them as codes PG_CODE + cell. */
enum { PG_A, PG_B, PG_X, PG_Y, PG_LB, PG_RB, PG_LT, PG_RT, PG_LS, PG_RS, PG_UP, PG_DOWN, PG_LEFT, PG_RIGHT,
       PG_START, PG_BACK, PG_LSTICK, PG_RSTICK, PG_DPAD,
       PG_HOME, PG_TOUCH_L, PG_TOUCH_R, PG_MUTE, PG_FN_L, PG_FN_R, PG_PADDLE_L, PG_PADDLE_R, PG_N };
/* DualSense-only buttons (touchpad halves, mute, Edge Fn / back buttons):
 * drawn in the DualSense style in every set. */
static int pg_ps_only(int g) { return g >= PG_TOUCH_L && g <= PG_PADDLE_R; }
/* Cells of the glyph atlas (our own texture, pgt_*), PG_COLS to a row: one
 * full set of PG_N glyphs per controller style (PSP_STYLE_*), in order. */
enum { PG_COLS = 7, PG_CODE = 0xEA20, PG_CELLS = PG_N * PSP_NSTYLE };
/* Key caps for in-game hints, after the pad glyphs in the same atlas: the
 * mouse, the named keys (g_keynames), A-Z and 0-9. Text uses them as codes
 * KC_CODE + index; the glyph lookup sees them as pad glyph PG_N + index. */
enum { KC_FIXED = 4 + (int)(sizeof g_keynames / sizeof g_keynames[0]), KC_N = KC_FIXED + 36, KC_CODE = 0xEA60, KC_ROW0 = (PG_CELLS + PG_COLS - 1) / PG_COLS, KC_PER_ROW = 3, PG_ROWS = KC_ROW0 + (KC_N + KC_PER_ROW - 1) / KC_PER_ROW };
static const char *kc_name(int i);
/* Key caps sit two cells wide, left-aligned, from row KC_ROW0; a cap is as wide as its name needs. */
/* A hint cap's width in cells: its name's measured width plus the same side
 * padding on every cap (by letter count, "Space" came out padded far wider
 * than "Shift"); one letter or less gets a square cap. Sizes match the atlas
 * drawing: cap 62% of the cell high, text 66% of that, 7% cell margins. */
static float kc_w(int i) {
    static float cache[KC_N]; if (cache[i]) return cache[i];
    const int cell = 96, ch = cell * 62 / 100, fh = ch * 66 / 100, m = cell * 7 / 100;
    wchar_t w[32]; const int n = MultiByteToWideChar(CP_UTF8, 0, key_glyph(kc_name(i)), -1, w, 32) - 1;
    HDC dc = CreateCompatibleDC(NULL); SIZE sz = { 0 };
    HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI Symbol");
    if (dc && f) { HGDIOBJ o = SelectObject(dc, f); GetTextExtentPoint32W(dc, w, n > 0 ? n : 0, &sz); SelectObject(dc, o); }
    if (f) DeleteObject(f); if (dc) DeleteDC(dc);
    float cw = sz.cx / 0.8f + ch * 0.2f;   /* keycap() fits text in 80% of the width */
    if (!strcmp(kc_name(i), "Enter") || !strcmp(kc_name(i), "Wheel")) cw = 0;   /* icon caps: square, like a letter (their names are not drawn) */
    if (cw < ch * 1.25f) cw = ch * 1.25f;
    float c = (cw + 2 * m) / cell; if (c > 2) c = 2;
    return cache[i] = c;
}
static void kc_cell(int i, int *col, int *row) { *col = (i % KC_PER_ROW) * 2; *row = KC_ROW0 + i / KC_PER_ROW; }
static const char *kc_name(int i) {
    static const char *const F[4] = { "LMB", "RMB", "Wheel", "Mouse" }; static char one[36][2];
    if (i < 4) return F[i];
    if (i < KC_FIXED) return g_keynames[i - 4].n;
    i -= KC_FIXED; one[i][0] = (char)(i < 26 ? 'A' + i : '0' + i - 26); return one[i];
}
static int kc_index(const char *k, size_t len) {
    for (int i = 0; i < KC_N; i++) { const char *n = kc_name(i); if (strlen(n) == len && !strncmp(n, k, len)) return i; }
    return -1;
}
/* Write key name k as caps. Pieces split on '+', ',' and '/': "RMB+Wheel" /
 * "RMB, +Wheel" give two caps with "+" between, "A / D" two with "/"; "WASD"
 * is four letter caps. A piece without a cap stays as [piece]. */
static void kc_emit(uint16_t *o, int *n, int cap, const char *k) {
    if (!strcmp(k, "WASD")) { for (const char *s = k; *s && *n < cap; s++) o[(*n)++] = (uint16_t)(KC_CODE + kc_index(s, 1)); return; }
    const char *p = k;
    while (*p && *n < cap - 4) {
        while (*p == ' ') p++;
        const char *e = p; while (*e && *e != '+' && *e != ',' && *e != '/') e++;
        size_t len = (size_t)(e - p); while (len && p[len - 1] == ' ') len--;
        const int i = kc_index(p, len);
        if (i >= 0) o[(*n)++] = (uint16_t)(KC_CODE + i);
        else { o[(*n)++] = '['; for (size_t j = 0; j < len && *n < cap - 2; j++) o[(*n)++] = (uint16_t)p[j]; o[(*n)++] = ']'; }
        while (*e == ',' || *e == ' ') e++;
        if (*e == '+') { o[(*n)++] = '+'; e++; }
        else if (*e == '/') { o[(*n)++] = ' '; o[(*n)++] = '/'; o[(*n)++] = ' '; e++; }
        p = e;
    }
}
static int pg_cell(int g, int st) { return st * PG_N + g; }
/* style groups */
static int st_ps(int st) { return st == PSP_STYLE_DS5 || st == PSP_STYLE_DS4; }
static int st_xbox(int st) { return st == PSP_STYLE_SERIES || st == PSP_STYLE_ONE || st == PSP_STYLE_360; }
static int pg_of_mask(unsigned m) {
    static const unsigned M[PG_N] = { PSP_PAD_A, PSP_PAD_B, PSP_PAD_X, PSP_PAD_Y, PSP_PAD_LB, PSP_PAD_RB, PSP_PAD_LT, PSP_PAD_RT,
        PSP_PAD_LS, PSP_PAD_RS, PSP_PAD_UP, PSP_PAD_DOWN, PSP_PAD_LEFT, PSP_PAD_RIGHT, PSP_PAD_START, PSP_PAD_BACK, 0, 0, 0,
        PSP_PAD_HOME, PSP_PAD_TOUCH_L, PSP_PAD_TOUCH_R, PSP_PAD_MUTE, PSP_PAD_FN_L, PSP_PAD_FN_R, PSP_PAD_PADDLE_L, PSP_PAD_PADDLE_R };
    for (int i = 0; i < PG_N; i++) if (m && M[i] == m) return i;
    return -1;
}
/* Pad glyphs in the game's own style: its button icons are a dark navy shape,
 * a dark rim, a cyan ring, and a cyan symbol (see the PSP cells on the same
 * sheet). The same look, drawn smoothly at the 4x sheet's resolution:
 * shapes as signed distances (supersampled), labels from GDI. */
enum { GS_CIRCLE, GS_TAB, GS_TRIG, GS_PILL, GS_DPAD, GS_MIC, GS_PADDLE };
static const float GS_LIT[3] = { 150, 230, 255 }, GS_RIM[3] = { 4, 16, 26 }, GS_RING[3] = { 42, 176, 232 }, GS_FILL[3] = { 10, 34, 54 }, GS_INK[3] = { 255, 255, 255 };
static float gs_seg(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay; float t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t; const float dx = px - ax - vx * t, dy = py - ay - vy * t; return sqrtf(dx * dx + dy * dy);
}
static float gs_box(float px, float py, float hw, float hh, float r) {   /* rounded box, centred */
    const float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
    return sqrtf(fmaxf(qx, 0) * fmaxf(qx, 0) + fmaxf(qy, 0) * fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
}
/* cyan ring width (pixels at the cell's size): thinner on the small shoulder
 * shapes so their labels get room, thinnest on the d-pad */
static float gs_ring(int sh) { return sh == GS_DPAD ? 2.5f : sh == GS_TAB || sh == GS_TRIG || sh == GS_MIC || sh == GS_PADDLE ? 3.0f : 4.5f; }
/* DualSense d-pad key: a pentagon pointing at the centre (a = along the arm, out; b = across) */
static float gs_parm(float a, float b) {
    return fmaxf(gs_box(a - 0.56f, b, 0.39f, 0.33f, 0.07f), (0.12f - (a - fabsf(b))) * 0.7071f);
}
/* rounded box with a radius per corner (tl, tr, bl, br; y grows down) */
static float gs_box4(float px, float py, float hw, float hh, float tl, float tr, float bl, float br) {
    return gs_box(px, py, hw, hh, px > 0 ? (py > 0 ? br : tr) : (py > 0 ? bl : tl));
}
/* filled triangle pointing along +a: base at a = a0, apex at a1, half width hw at the base (b across) */
static float gs_tri(float a, float b, float a0, float a1, float hw) {
    const float h = a1 - a0;
    return fmaxf(a0 - a, ((a - a0) * hw + (fabsf(b) - hw) * h) / sqrtf(hw * hw + h * h));
}
/* Xbox / Switch d-pad: one plus, rounded ends */
static float gs_plus(float x, float y) { const float a = 0.94f, b = 0.33f; return fminf(gs_box(x, y, b, a, 0.14f), gs_box(x, y, a, b, 0.14f)); }
/* d-pads set in a round dish (Xbox Series' faceted dish, the 360's disc): the plus is drawn inside, smaller */
static int gs_dish(int st) { return st == PSP_STYLE_SERIES || st == PSP_STYLE_360; }
enum { GS_DISH_K = 125 };   /* plus scale inside a dish, percent */
/* Base shape distance, coordinates in -1..1 (cell units), < 0 inside.
 * Bumpers and triggers follow the pad they name (drawn for the right-hand
 * button, outer side +x; the left ones mirror):
 *   Xbox One / Series: bumper with a big rounded outer top corner and a
 *   slanted inner side; trigger with a straight outer side, a scooped inner
 *   side and a big rounded bottom outer corner. Xbox 360: a plain rounded bumper.
 *   PlayStation, Switch: L1 / R1 (L / R) a flat rounded bar; L2 / R2 (ZL / ZR)
 *   a body with a domed top. */
static float gs_shape(int sh, int g, int st, float x, float y) {
    const int ps = st_ps(st), flat = !st_xbox(st);
    if (g == PG_LB || g == PG_LT || g == PG_PADDLE_L) x = -x;
    switch (sh) {
    case GS_CIRCLE: return sqrtf(x * x + y * y) - 0.94f;
    case GS_MIC: return gs_box(x, y, 0.86f, 0.5f, 0.5f);   /* DualSense mute: a small flat pill */
    case GS_PADDLE: return gs_box(x - y * 0.22f, y, 0.52f, 0.92f, 0.46f);   /* DualSense Edge back button: a leaning lever */
    case GS_TAB:
        if (flat) return gs_box(x, y, 0.94f, 0.52f, 0.22f);
        if (st == PSP_STYLE_360) return gs_box4(x, y, 0.94f, 0.5f, 0.3f, 0.46f, 0.2f, 0.2f);
        return gs_box4(x < 0 ? x - (y + 0.54f) * 0.16f : x, y, 0.94f, 0.54f, 0.2f, 0.52f, 0.32f, 0.32f);
    case GS_TRIG:
        if (flat) return fmaxf(gs_box(x, y, 0.64f, 0.92f, 0.16f), sqrtf(x * x + (y - 0.75f) * (y - 0.75f)) - 1.6f);
        return fmaxf(gs_box4(x, y, 0.7f, 0.92f, 0.14f, 0.24f, 0.14f, 0.62f), 1.75f - sqrtf((x + 2.3f) * (x + 2.3f) + (y - 0.1f) * (y - 0.1f)));
    case GS_PILL:
        if (!ps) return sqrtf(x * x + y * y) - 0.94f;   /* Xbox Menu / View / Start / Back, Switch + / - are round */
        /* DualSense Select: the touchpad (it and Create both act as Select;
         * the pad's face has the touchpad), wide, narrowing toward the bottom */
        if ((g == PG_BACK && st == PSP_STYLE_DS5) || g == PG_TOUCH_L || g == PG_TOUCH_R)
            return gs_box4(x * (1.0f + (y + 0.5f) * 0.14f), y + 0.06f, 0.94f, 0.54f, 0.1f, 0.1f, 0.36f, 0.36f);
        return fmaxf(gs_box(x, y + 0.2f, 0.94f, 0.62f, 0.5f), y - 0.5f);   /* half pill, like START / SELECT */
    default:
        if (ps) return fminf(fminf(gs_parm(-y, x), gs_parm(y, x)), fminf(gs_parm(-x, y), gs_parm(x, y)));   /* four separate arrow keys */
        if (gs_dish(st)) return sqrtf(x * x + y * y) - 0.94f;
        return gs_plus(x, y);
    }
}
/* symbol distance (< 0 = ink): PlayStation face symbols, the lit d-pad arm,
 * Menu / View, Start / Back arrows, home buttons, etc. */
static float gs_symbol(int g, int st, float x, float y) {
    const float w = 0.085f; const int ps = st_ps(st);
    if (ps && g == PG_A) return fminf(gs_seg(x, y, -.36f, -.36f, .36f, .36f), gs_seg(x, y, -.36f, .36f, .36f, -.36f)) - w;
    if (ps && g == PG_B) return fabsf(sqrtf(x * x + y * y) - 0.4f) - w;
    if (ps && g == PG_X) return fabsf(gs_box(x, y, .36f, .36f, .02f)) - w;
    if (ps && g == PG_Y) { const float t = 0.44f; y -= 0.04f;   /* between the triangle's box and mass centres */
        return fminf(fminf(gs_seg(x, y, 0, -t, -t * .95f, t * .62f), gs_seg(x, y, -t * .95f, t * .62f, t * .95f, t * .62f)), gs_seg(x, y, t * .95f, t * .62f, 0, -t)) - w; }
    if (g >= PG_UP && g <= PG_RIGHT && ps)   /* the lit key */
        return g == PG_UP ? gs_parm(-y, x) : g == PG_DOWN ? gs_parm(y, x) : g == PG_LEFT ? gs_parm(-x, y) : gs_parm(x, y);
    if (g >= PG_UP && g <= PG_RIGHT) {   /* the lit arm */
        const float b = 0.34f, k = gs_dish(st) ? GS_DISH_K / 100.0f : 1; x *= k; y *= k;
        return (g == PG_UP ? gs_box(x, y + .6f, b, .34f, .08f) : g == PG_DOWN ? gs_box(x, y - .6f, b, .34f, .08f)
             : g == PG_LEFT ? gs_box(x + .6f, y, .34f, b, .08f) : gs_box(x - .6f, y, .34f, b, .08f)) / k; }
    if (st == PSP_STYLE_360 && (g == PG_START || g == PG_BACK))   /* Start / Back: an arrow pointing away from the Guide button */
        return g == PG_START ? gs_tri(x + .04f, y, -.26f, .34f, .32f) : gs_tri(-x + .04f, y, -.26f, .34f, .32f);
    if (st_xbox(st) && g == PG_START)   /* Menu: three bars */
        return fminf(fminf(gs_box(x, y + .26f, .36f, 0, 0), gs_box(x, y, .36f, 0, 0)), gs_box(x, y - .26f, .36f, 0, 0)) - .07f;
    if (st_xbox(st) && g == PG_BACK) {   /* View: two overlapping windows, the front one solid-edged over the back */
        const float front = gs_box(x - .1f, y - .1f, .22f, .18f, .03f), back = gs_box(x + .1f, y + .1f, .22f, .18f, .03f);
        return fminf(fabsf(front) - .06f, fmaxf(fabsf(back) - .06f, -(front - .1f))); }
    if (st == PSP_STYLE_DS5 && g == PG_BACK) {   /* touchpad: a fingertip and its ripple */
        const float r = sqrtf(x * x + (y - .02f) * (y - .02f));
        return fminf(r - .13f, fabsf(r - .3f) - .05f); }
    if (g == PG_TOUCH_L || g == PG_TOUCH_R)   /* the clicked half, lit */
        return gs_box(x + (g == PG_TOUCH_L ? .44f : -.44f), y + .06f, .34f, .3f, .06f);
    if (g == PG_MUTE)   /* microphone: capsule, cradle, stem */
        return fminf(fminf(gs_box(x, y + .07f, .09f, .17f, .09f) - .0f, fabsf(fmaxf(sqrtf(x * x + (y + .05f) * (y + .05f)) - .2f, -(y + .05f))) - .035f),
                     gs_box(x, y - .23f, 0, .07f, 0) - .035f);
    if (g == PG_HOME) {
        const float r = sqrtf(x * x + y * y);
        if (st == PSP_STYLE_360)   /* Guide: a ring around an X */
            return fminf(fabsf(r - .56f) - .05f, fmaxf(fminf(gs_seg(x, y, -.5f, -.5f, .5f, .5f), gs_seg(x, y, -.5f, .5f, .5f, -.5f)) - .13f, r - .44f));   /* a bold X cut by the ring's gap: the 360 orb */
        if (st_xbox(st))   /* Guide: a sphere crossed by two arcs */
            return fminf(fabsf(r - .44f) - .06f, fmaxf(fminf(fabsf(fabsf(x) - fabsf(y) * .55f - .08f) - .06f, 1), r - .44f));
        if (st == PSP_STYLE_SWITCH)   /* HOME: a house */
            return fminf(gs_tri(-y, x, .02f, .46f, .46f), gs_box(x, y - .2f, .27f, .2f, .02f));
    }
    if (g == PG_LSTICK || g == PG_RSTICK) return fabsf(sqrtf(x * x + y * y) - 0.66f) - 0.04f;   /* the stick's cap */
    return 1;
}
static void pad_glyph(uint32_t *img, int iw, int x0, int y0, int c, int g, int st) {
    /* Transparent margin inside the cell: the text layout spaces by the cell,
     * so stacked lines get a gap without changing word spacing. */
    { const int ci = c * 86 / 100, m = (c - ci) / 2; x0 += m; y0 += m; c = ci; }
    const int sh = g <= PG_Y || g == PG_LS || g == PG_RS || g == PG_LSTICK || g == PG_RSTICK || g == PG_HOME || g == PG_FN_L || g == PG_FN_R ? GS_CIRCLE
                 : g == PG_LB || g == PG_RB ? GS_TAB : g == PG_LT || g == PG_RT ? GS_TRIG
                 : g == PG_START || g == PG_BACK || g == PG_TOUCH_L || g == PG_TOUCH_R ? GS_PILL
                 : g == PG_MUTE ? GS_MIC : g == PG_PADDLE_L || g == PG_PADDLE_R ? GS_PADDLE : GS_DPAD;
    /* label (Switch face buttons by position: the bottom one is B, the right A, the left Y, the top X) */
    static const wchar_t *const XL[PG_N] = { L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"LS", L"RS", 0, 0, 0, 0, 0, 0, L"L", L"R", 0 };
    static const wchar_t *const PL[PG_N] = { 0, 0, 0, 0, L"L1", L"R1", L"L2", L"R2", L"L3", L"R3", 0, 0, 0, 0, L"OPTIONS", 0, L"L", L"R", 0,
        L"PS", 0, 0, 0, L"Fn", L"Fn", L"L", L"R" };
    static const wchar_t *const NL[PG_N] = { L"B", L"A", L"Y", L"X", L"L", L"R", L"ZL", L"ZR", L"LS", L"RS", 0, 0, 0, 0, L"+", L"\x2212", L"L", L"R", 0 };
#define PG_LABEL(g) (st == PSP_STYLE_SWITCH ? NL[g] : st_ps(st) ? (st == PSP_STYLE_DS4 && (g) == PG_BACK ? L"SHARE" : PL[g]) : XL[g])
    const wchar_t *lab = PG_LABEL(g);
    /* a left / right pair shares one label size (an R is wider than an L, so R2 alone fits smaller than L2) */
    static const int PAIR[][2] = { { PG_LB, PG_RB }, { PG_LT, PG_RT }, { PG_LS, PG_RS }, { PG_LSTICK, PG_RSTICK }, { PG_FN_L, PG_FN_R }, { PG_PADDLE_L, PG_PADDLE_R } };
    const wchar_t *alt = NULL;
    for (size_t k = 0; k < sizeof PAIR / sizeof PAIR[0]; k++) { if (PAIR[k][0] == g) alt = PG_LABEL(PAIR[k][1]); if (PAIR[k][1] == g) alt = PG_LABEL(PAIR[k][0]); }
#undef PG_LABEL
    uint8_t *cov = calloc((size_t)c * c, 1); if (!cov) return;
    if (lab) {
        /* The label fills the shape's inner (navy) area: the biggest bold size
         * whose actual ink fits it, drawn at 4x and averaged down for clean
         * edges. Single letters centre optically (box centre and centre of
         * mass averaged: an L or R box-centred looks pushed left). */
        const int L = 4, C = c * L, n = (int)wcslen(lab);
        const float px1 = 2.0f / c, inner = (2.2f * 1.8f + gs_ring(sh)) * px1;
        int ix0 = c, ix1 = -1, iy0 = c, iy1 = -1;   /* inner area, from the shape */
        for (int y = 0; y < c; y++) for (int x = 0; x < c; x++) {
            const float u = (x + .5f) / c * 2 - 1, v = (y + .5f) / c * 2 - 1;
            if (gs_shape(sh, g, st, u, v) < -inner) { if (x < ix0) ix0 = x; if (x > ix1) ix1 = x; if (y < iy0) iy0 = y; if (y > iy1) iy1 = y; } }
        if (sh == GS_CIRCLE) { const int r = (int)((ix1 - ix0) * 0.15f); ix0 += r; ix1 -= r; iy0 += r; iy1 -= r; }   /* a circle's inscribed square */
        else if (ix1 > ix0) {   /* the width every row of the middle band has (a trigger's scoop narrows it) */
            const int b0 = iy0 + (iy1 - iy0) / 4, b1 = iy1 - (iy1 - iy0) / 4; int l = 0, r = c;
            for (int y = b0; y <= b1; y++) { int a = -1, z = -1;
                for (int x = 0; x < c; x++) if (gs_shape(sh, g, st, (x + .5f) / c * 2 - 1, (y + .5f) / c * 2 - 1) < -inner) { if (a < 0) a = x; z = x; }
                if (a >= 0) { if (a > l) l = a; if (z < r) r = z; } }
            if (r > l) { ix0 = l; ix1 = r; } }
        const float aw = (ix1 - ix0 + 1) * L * (n == 1 ? 0.96f : 0.92f), ah = (iy1 - iy0 + 1) * L * (n == 1 ? 0.96f : sh == GS_TAB ? 0.92f : n <= 2 ? 0.74f : 0.86f);
        const wchar_t *face = n >= 4 ? L"Bahnschrift SemiBold SemiConden" : L"Tahoma";   /* long words: condensed */
        BITMAPINFO bi = { 0 }; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = C; bi.bmiHeader.biHeight = -C;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL; HDC dc = CreateCompatibleDC(NULL); HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (dc && bm && ix1 > ix0) {
            HGDIOBJ ob = SelectObject(dc, bm); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            const uint8_t *b = bits; int bx0 = 0, bx1 = -1, by0 = 0, by1 = -1;
            for (int fh = (int)(ah * 1.6f); fh > 8; fh -= 2) {
                HFONT f = CreateFontW(-fh, 0, 0, 0, n >= 4 ? FW_SEMIBOLD : FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, face);
                HGDIOBJ of = SelectObject(dc, f); int fits = 1;
                for (int pass = alt ? 0 : 1; pass < 2; pass++) {   /* the partner's label first, then ours (left in the bitmap) */
                    const wchar_t *s = pass ? lab : alt; memset(bits, 0, (size_t)C * C * 4);
                    RECT rc = { 0, 0, C, C }; DrawTextW(dc, s, (int)wcslen(s), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); GdiFlush();
                    bx0 = C; bx1 = -1; by0 = C; by1 = -1;
                    for (int y = 0; y < C; y++) for (int x = 0; x < C; x++) if (b[(y * C + x) * 4 + 1] > 60) {
                        if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
                    if (!(bx1 >= 0 && bx1 - bx0 + 1 <= aw && by1 - by0 + 1 <= ah)) fits = 0;
                }
                SelectObject(dc, of); DeleteObject(f);
                if (fits) break;
            }
            if (bx1 >= 0) {
                float mx = (bx0 + bx1) * 0.5f;
                if (n == 1) { double sw = 0, sx = 0; for (int y = 0; y < C; y++) for (int x = 0; x < C; x++) { const int a = b[(y * C + x) * 4 + 1]; sw += a; sx += (double)a * x; }
                    if (sw > 0) mx = (mx + (float)(sx / sw)) * 0.5f; }
                const float tx = (ix0 + ix1 + 1) * 0.5f * L, ty = (iy0 + iy1 + 1) * 0.5f * L;   /* centre of the inner area */
                const int dx = (int)lroundf(tx - mx - .5f), dy = (int)lroundf(ty - (by0 + by1 + 1) * 0.5f);
                for (int y = 0; y < c; y++) for (int x = 0; x < c; x++) { int s = 0;
                    for (int j = 0; j < L; j++) for (int i = 0; i < L; i++) { const int sx = x * L + i - dx, sy = y * L + j - dy;
                        if (sx >= 0 && sy >= 0 && sx < C && sy < C) s += b[(sy * C + sx) * 4 + 1]; }
                    cov[y * c + x] = (uint8_t)(s / (L * L)); }
            }
            SelectObject(dc, ob);
        }
        if (bm) DeleteObject(bm); if (dc) DeleteDC(dc);
    }
    const float px1 = 2.0f / c, rim = 2.2f * px1, ring = gs_ring(sh) * px1;   /* rim and ring widths, in cell units */
    for (int y = 0; y < c; y++) for (int x = 0; x < c; x++) {
        float acc[4] = { 0 };
        for (int sj = 0; sj < 4; sj++) for (int si = 0; si < 4; si++) {   /* 4x4 supersampling */
            const float u = ((x + (si + .5f) / 4) / c) * 2 - 1, v = ((y + (sj + .5f) / 4) / c) * 2 - 1;
            const float d = gs_shape(sh, g, st, u, v); if (d > 0) continue;
            const float *col = -d < rim ? GS_RIM : -d < rim + ring ? GS_RING : -d < rim + ring + rim * .8f ? GS_RIM : GS_FILL;
            if (gs_symbol(g, st, u, v) < 0) col = sh == GS_DPAD ? (-d < rim ? GS_RIM : GS_LIT) : GS_INK;
            else if (sh == GS_DPAD && gs_dish(st) && !st_ps(st)) {   /* the plus in its dish: outlined, like the plain plus */
                const float pd = gs_plus(u * GS_DISH_K / 100.0f, v * GS_DISH_K / 100.0f) * 100.0f / GS_DISH_K;
                if (pd < 0 && pd > -ring) col = GS_RING; else if (pd >= 0 && pd < rim) col = GS_RIM; }
            for (int k = 0; k < 3; k++) acc[k] += col[k]; acc[3] += 1; }
        if (acc[3] == 0) { img[(y0 + y) * iw + x0 + x] = 0; continue; }
        float r = acc[0] / acc[3], gg = acc[1] / acc[3], bl = acc[2] / acc[3]; const float t = cov[y * c + x] / 255.0f;
        r += (GS_INK[0] - r) * t; gg += (GS_INK[1] - gg) * t; bl += (GS_INK[2] - bl) * t;
        img[(y0 + y) * iw + x0 + x] = (uint32_t)r | (uint32_t)gg << 8 | (uint32_t)bl << 16 | (uint32_t)(acc[3] / 16 * 255) << 24;
    }
    free(cov);
}
static const char *psp_icon_key(uint32_t c, int play);
static char g_sheet_keys[1024];   /* the bound keys the icon sheet was last drawn with */
static void sheet_keys(char *s, size_t n);
static const uint32_t *icon_sheet_post(const uint32_t *px, int w, int h, int *ow, int *oh) {
    if (w != 256 || h != 128 || getenv("SWBES_PSP_PROMPTS")) return NULL;
    int n = 0; for (int i = 0; i < w * h; i++) n += (px[i] >> 24) > 40;
    if (n != 9861) return NULL;
    { static int dumped; if (!dumped && getenv("SWBES_DUMPSHEET")) { dumped = 1; FILE *o = fopen("iconsheet.ppm", "wb");   /* debug: the original sheet, alpha as grey on magenta */
        if (o) { fprintf(o, "P6\n%d %d\n255\n", w, h); for (int i = 0; i < w * h; i++) { const uint32_t c = px[i], a = c >> 24;
            fputc((int)(((c & 255) * a + 255 * (255 - a)) / 255), o); fputc((int)(((c >> 8 & 255) * a) / 255), o); fputc((int)(((c >> 16 & 255) * a + 255 * (255 - a)) / 255), o); } fclose(o); } } }
    enum { S = 4 }; static uint32_t big[256 * S * 128 * S];
    for (int y = 0; y < h * S; y++) for (int x = 0; x < w * S; x++) big[y * w * S + x] = px[(y / S) * w + x / S];
    /* Button pictures take their key from psp_icon_key (code set): the same
     * table as icons looked up by code, so a rebind redraws both. */
    static const struct { int x, y, w, h; const char *k; uint32_t code; } C[] = {
        { 2, 2, 33, 34, 0, 0xEA04 }, { 40, 2, 33, 34, 0, 0xEA06 }, { 2, 40, 33, 34, 0, 0xEA07 }, { 2, 78, 33, 34, 0, 0xEA05 },   /* X O Triangle Square */
        { 39, 38, 32, 20, 0, 0xEA10 }, { 39, 59, 32, 20, 0, 0xEA11 },   /* Start, Select */
        /* d-pads, lit arms -> the arrow keys */
        { 127, 0, 21, 21, "\xE2\x86\x91" }, { 127, 22, 21, 21, "\xE2\x86\x91" }, { 149, 0, 21, 21, "\xE2\x86\x90" }, { 171, 0, 21, 21, "\xE2\x86\x93" }, { 193, 0, 21, 21, "\xE2\x86\x93" },
        { 127, 45, 21, 21, "\xE2\x86\x92" }, { 127, 67, 21, 21, "\xE2\x86\x92" }, { 127, 89, 21, 21, "\xE2\x86\x90" },
        { 102, 48, 21, 21, "\xE2\x86\x95" }, { 102, 70, 21, 21, "\xE2\x9C\xA5" }, { 102, 92, 21, 21, "\xE2\x86\x94" },
        { 38, 110, 26, 13, 0, 0xEA09 }, { 76, 1, 26, 13, 0, 0xEA08 },     /* R, L */
        { 78, 65, 21, 21, 0, 0xEA04 }, { 78, 89, 21, 21, 0, 0xEA07 }, { 104, 2, 21, 21, 0, 0xEA05 }, { 104, 26, 21, 21, 0, 0xEA06 },   /* small X Triangle Square O */
    };
    g_kc_txt = 66;
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        const char *k = C[i].code ? psp_icon_key(C[i].code, 1) : C[i].k;
        { static const struct { const char *a; int b; } A[] = { { "↑", B_NAV_UP }, { "↓", B_NAV_DOWN }, { "←", B_NAV_LEFT }, { "→", B_NAV_RIGHT } };
          for (size_t j = 0; j < 4; j++) if (!strcmp(k, A[j].a)) k = g_bind[A[j].b].name; }   /* d-pad arms: the bound menu keys */
        { static char pair[64]; const char *u = g_bind[B_NAV_UP].name, *d = g_bind[B_NAV_DOWN].name, *l = g_bind[B_NAV_LEFT].name, *r = g_bind[B_NAV_RIGHT].name;
          const int ud = !strcmp(u, "Up") && !strcmp(d, "Down"), lr = !strcmp(l, "Left") && !strcmp(r, "Right");   /* both pairs on the arrows: keep the arrow glyph */
          if (!strcmp(k, "\xE2\x86\x95") && !ud) { snprintf(pair, sizeof pair, "%s/%s", u, d); k = pair; }   /* up-down */
          else if (!strcmp(k, "\xE2\x86\x94") && !lr) { snprintf(pair, sizeof pair, "%s/%s", l, r); k = pair; }   /* left-right */
          else if (!strcmp(k, "\xE2\x9C\xA5") && !(ud && lr)) { snprintf(pair, sizeof pair, "%s%s%s%s", u, l, d, r); k = pair; } }   /* all four */
        int x = C[i].x * S, y = C[i].y * S, cw = C[i].w * S, ch = C[i].h * S;
        if (C[i].w - C[i].h <= 2 && C[i].h - C[i].w <= 2) {   /* square button sprites: the hint caps' proportions (pgt_atlas), the sprite as the glyph cell */
            for (int yy = y; yy < y + ch; yy++) for (int xx = x; xx < x + cw; xx++) big[yy * w * S + xx] = 0;
            const int cell = ch, kc = kc_index(k, strlen(k)), m = cell * 7 / 100;
            ch = cell * 62 / 100; int kw = kc >= 0 ? (int)(kc_w(kc) * cell) - 2 * m : ch * 5 / 4; if (kw > cw) kw = cw;
            x += (cw - kw) / 2; y += (cell - ch) / 2; cw = kw; }
        capdraw(big, w * S, x, y, cw, ch, k); }
    g_kc_txt = 62;
    sheet_keys(g_sheet_keys, sizeof g_sheet_keys);
    if (getenv("SWBES_DUMPCAPS")) { FILE *o = fopen("capsheet.ppm", "wb");   /* debug: the sheet with our caps, over grey */
        if (o) { fprintf(o, "P6\n%d %d\n255\n", w * S, h * S); for (int k = 0; k < w * S * h * S; k++) { const uint32_t c = big[k], a = c >> 24;
            for (int j = 0; j < 3; j++) fputc((int)(((c >> (8 * j) & 255) * a + 90 * (255 - a)) / 255), o); } fclose(o); } }
    *ow = w * S; *oh = h * S; return big;
}
/* Our own texture: the pad glyph atlas. The game binds textures by handle
 * (table of 500 object pointers at 0x54324C; 0x13C1A0 binds, copying the
 * texels into its VRAM cache). Object: +0 u16 w, +2 u16 h, +6 format (GE
 * TPSM), +7 mip levels - 1, +8 texels, +12 CLUT, +16 VRAM cache slot (-1
 * none), +20 cached size. We claim the last handle with a 512 x 512 4-bit
 * texture of one magic colour (its CLUT's entry 1); the renderer hands that
 * to tex_post, which swaps in the atlas at any resolution (UVs are
 * normalised). 512 texels (the GE's largest): sprite UVs reach the GE
 * quantised to whole texels, so a smaller texture crops the cells (at 256 the
 * atlas's 45 rows got under 6 texels each and lost a texel top and bottom). */
enum { PGT_HANDLE = 499, PGT_TABLE = 0x54324C, PGT_SIDE = 512, PGT_CELL = 96 };
#define PGT_MAGIC 0x7E15A3C9u
static uint32_t g_pgt_obj;
static void pgt_install(void) {
    if (!g_pgt_obj) {   /* object (64 bytes), CLUT (16 x 8888), texels (w * h bytes: the bind's copy may read that much) */
        const uint32_t m = psp_sysmem_alloc(128 + PGT_SIDE * PGT_SIDE, 1); if (!m) return;
        for (uint32_t k = 0; k < 128; k += 4) psp_write32(m + k, 0);
        psp_write16(m + 0, PGT_SIDE); psp_write16(m + 2, PGT_SIDE); psp_write8(m + 6, 4); psp_write8(m + 7, 0);
        psp_write32(m + 8, m + 128); psp_write32(m + 12, m + 64); psp_write32(m + 16, 0xFFFFFFFFu); psp_write32(m + 28, 0x4051);
        for (uint32_t k = 0; k < 16; k++) psp_write32(m + 64 + 4 * k, PGT_MAGIC);
        for (uint32_t k = 0; k < PGT_SIDE * PGT_SIDE; k += 4) psp_write32(m + 128 + k, 0x11111111u);
        g_pgt_obj = m;
    }
    if (psp_read32(PGT_TABLE + 4 * PGT_HANDLE) != g_pgt_obj) psp_write32(PGT_TABLE + 4 * PGT_HANDLE, g_pgt_obj);
}
static const uint32_t *pgt_atlas(int *ow, int *oh) {
    enum { W = PG_COLS * PGT_CELL, H = PG_ROWS * PGT_CELL };
    static uint32_t img[W * H]; static int done;
    if (!done) { done = 1;
        for (int i = 0; i < PG_CELLS; i++) pad_glyph(img, W, (i % PG_COLS) * PGT_CELL, (i / PG_COLS) * PGT_CELL, PGT_CELL, i % PG_N, pg_ps_only(i % PG_N) ? PSP_STYLE_DS5 : i / PG_N);
        g_kc_txt = 66;   /* flat caps: room for a descender ("Space") above the bottom border */
        for (int i = 0; i < KC_N; i++) {   /* key caps: inset like the pad shapes (86% of the cell) at the sides, shorter (62%): hint lines are about a glyph apart, so a full-height cap meets the one below */
            int col, row; kc_cell(i, &col, &row); const int ci = PGT_CELL * 62 / 100, m = PGT_CELL * 7 / 100, x = col * PGT_CELL + m, y = row * PGT_CELL + (PGT_CELL - ci) / 2, cw = (int)(kc_w(i) * PGT_CELL) - 2 * m;
            if (!strcmp(kc_name(i), "Wheel")) iconcap(img, W, x, y, cw, ci, icon_mouse_scroll, 66, 0); else capdraw(img, W, x, y, cw, ci, kc_name(i)); }
        g_kc_txt = 62;
        if (getenv("SWBES_DUMPGLYPHS")) { FILE *o = fopen("padglyphs.ppm", "wb");   /* debug: the atlas over grey */
            if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); for (int k = 0; k < W * H; k++) { const uint32_t c = img[k], a = c >> 24;
                for (int j = 0; j < 3; j++) fputc((int)(((c >> (8 * j) & 255) * a + 90 * (255 - a)) / 255), o); } fclose(o); } } }
    *ow = W; *oh = H; return img;
}
static const uint32_t *tex_post(const uint32_t *px, int w, int h, int *ow, int *oh) {
    if (w == PGT_SIDE && h == PGT_SIDE) {
        int same = 1; for (int i = 1; i < w * h && same; i++) same = px[i] == px[0];
        if (same && px[0] == PGT_MAGIC) return pgt_atlas(ow, oh);
    }
    if (w == 256 && h == 256) {
        if (!getenv("SWBES_PSP_PROMPTS")) {   /* the HUD sheet: its up/down d-pad at (46,223) is "Scroll list" on the loadout and faction pages */
            int n = 0; for (int i = 0; i < w * h; i++) n += (px[i] >> 24) > 40;
            if (n == 21457) { enum { S = 4 }; static uint32_t src[256 * 256], big[256 * S * 256 * S]; static int have = -1;
                const int mode = psp_pad_last() ? 1 + psp_pad_style() : 0;   /* the icon follows the input in use (scroll_icon_watch redecodes on a change) */
                if (have != mode || memcmp(src, px, sizeof src)) {   /* drawn once per sheet and input */
                    memcpy(src, px, sizeof src); have = mode;
                    for (int y = 0; y < h * S; y++) for (int x = 0; x < w * S; x++) big[y * w * S + x] = px[(y / S) * w + x / S];
                    for (int y = 223 * S; y < 256 * S; y++) for (int x = 46 * S; x < 80 * S; x++) big[y * w * S + x] = 0;
                    if (mode) pad_glyph(big, w * S, 50 * S, 226 * S, 26 * S, PG_DPAD, mode - 1);   /* the pad's d-pad, at the size of the other glyphs */
                    else iconcap(big, w * S, 50 * S, 226 * S, 26 * S, 26 * S, icon_mouse_scroll, 66, 0); }   /* 26 texels: on screen, the height of the other caps */   /* drawn untinted: sprite_color */
                *ow = w * S; *oh = h * S; return big; } }
    }
    return icon_sheet_post(px, w, h, ow, oh);
}
/* The game draws the HUD sheet's "Scroll list" d-pad (texels 46,223 - 80,256)
 * tinted cyan (vertex colour 0x3CB5F6). Our cap / pad glyph there has its own colours, so that
 * sprite is drawn white, like every other prompt. */
static uint32_t sprite_color(int tw, int th, float u0, float v0, float u1, float v1, uint32_t rgba) {
    static int psp = -1; if (psp < 0) psp = getenv("SWBES_PSP_PROMPTS") != NULL;
    if (tw == 256 && th == 128 && u0 >= 100 && u1 <= 216 && !psp)   /* icon sheet d-pads: the game tints them blue; our caps carry their own colours */
        return (rgba & 0xFF000000u) | 0xFFFFFFu;
    if (tw != 256 || th != 256 || u0 != 46 || v0 != 223 || u1 != 80 || v1 != 256) return rgba;
    return psp ? rgba : (rgba & 0xFF000000u) | 0xFFFFFFu;
}
/* Icon codes resolve through 0x3380EC (code -> sub-image name hash) and
 * 0x45C60 (hash -> registry entry). The registry (*0x45FCB4) is a fixed array
 * of 256 56-byte slots: +16 u0 v0 u1 v1, +32 texture w h (sizes the glyph),
 * +40 name hash, +44 texture hash, +52 in-use; 0x45FCB8 = slots in use high
 * mark, 0x45FCBC = slots in use. Each pad glyph claims a free slot, a copy of
 * Cross's entry with its own name hash and the pad cell's rect. A pad glyph
 * code, or a PSP icon while the pad is in use, is looked up as Cross and the
 * hash swapped for the pad glyph's on its way into 0x45C60. */
enum { PG_HASH = 0x7AD50000 };
static int g_pg_pending = -1;
static int psp_icon_pad_cell(uint32_t c) {
    const int play = !g_in_ui;
    switch (c) {
    case 0xEA04: return PG_A; case 0xEA05: return PG_X; case 0xEA06: return PG_B; case 0xEA07: return PG_Y;
    case 0xEA08: case 0xEA0A: return play ? PG_LT : PG_LB;
    case 0xEA09: case 0xEA0B: return play ? PG_RT : PG_RB;
    case 0xEA10: return PG_START; case 0xEA11: return PG_BACK;
    case 0xEA00: case 0xEA01: case 0xEA02: case 0xEA03: case 0xEA12: case 0xEA13: return PG_LSTICK;
    case 0xEA14: return PG_DPAD;
    default: return -1;
    }
}
/* Keyboard prompts: the key each PSP button icon stands for. One table for
 * every icon the game draws by name (menu prompts, hint icons): a fixed key
 * for menu-only controls, else the binding, so a rebind changes the icon
 * everywhere at once. Defaults match what the icon sheet used to show. */
static const char *psp_icon_key(uint32_t c, int play) {
    static const struct { uint32_t code; int ui, play; } T[] = {   /* the bind shown in menus, and while playing */
        { 0xEA04, B_CONFIRM, B_CONFIRM }, { 0xEA06, B_CANCEL, B_CANCEL },   /* Cross, Circle */
        { 0xEA07, B_MENU_Y, B_MENU_Y }, { 0xEA05, B_MENU_X, B_AUTOPILOT },   /* Triangle, Square */
        { 0xEA08, B_TAB_L, B_STUNT }, { 0xEA0A, B_TAB_L, B_STUNT }, { 0xEA09, B_TAB_R, B_USE }, { 0xEA0B, B_TAB_R, B_USE },   /* L, R */
        { 0xEA10, B_PAUSE, B_PAUSE }, { 0xEA11, B_SELECT, B_SELECT },   /* Start, Select */
    };
    if (play && (c == 0xEA00 || c == 0xEA01 || c == 0xEA02 || c == 0xEA03 || c == 0xEA12 || c == 0xEA13)) return "A / D";   /* the stick, pulled left / right in hints: steer keys */
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i].code == c) return g_bind[play ? T[i].play : T[i].ui].name;
    return NULL;
}
static void sheet_keys(char *s, size_t n) { size_t k = 0; s[0] = 0; for (int b = 0; b < B_COUNT && k + 18 < n; b++) k += (size_t)snprintf(s + k, n - k, "%s|", g_bind[b].name); }
static uint32_t g_pg_learn;   /* PSP icon code being looked up (its hash is learned) */
static void glyph_code_hook(uint32_t addr) { (void)addr;   /* 0x3380EC entry, a0 = code */
    const uint32_t c = psp_cpu.r[4] & 0xFFFF;
    if (c >= 0xE800 && getenv("SWBES_PGLOG")) { static uint32_t seen[64]; static int ns; int k = 0; for (; k < ns; k++) if (seen[k] == c) break; if (k == ns && ns < 64) { seen[ns++] = c; fprintf(stderr, "pg: code %04X ra %08X last %d\n", c, psp_cpu.r[31], psp_pad_last()); fflush(stderr); } }
    const int cell = c >= PG_CODE && c < PG_CODE + PG_N ? (int)(c - PG_CODE) : c >= KC_CODE && c < KC_CODE + KC_N ? PG_N + (int)(c - KC_CODE) : -1;
    g_pg_pending = cell; g_pg_learn = cell < 0 && psp_icon_pad_cell(c) >= 0 ? c : 0;
    if (cell >= 0) psp_cpu.r[4] = 0xEA04;
}
/* The registry slot holding pad glyph g, claiming one (a copy of Cross's
 * entry) if it has none; 0 if the sheet isn't loaded or the registry is full. */
static uint32_t g_pg_slot[PG_N + KC_N];
static uint32_t pg_slot(int g) {
    const uint32_t arr = psp_read32(0x45FCB4u), cap = psp_read32(0x45FCB0u); uint32_t n = psp_read32(0x45FCB8u);
    if (arr < 0x08800000u || arr >= 0x0A000000u || cap > 4096 || n > cap) return 0;
    uint32_t s = g_pg_slot[g];
    if (s && s >= arr && s < arr + 56 * n && psp_read8(s + 52) && psp_read32(s + 40) == PG_HASH + (uint32_t)g) return s;
    uint32_t cross = 0; s = 0;
    for (uint32_t i = 0; i < n; i++) { const uint32_t e = arr + 56 * i; if (!psp_read8(e + 52)) continue;
        if (psp_read32(e + 40) == PG_HASH + (uint32_t)g) { s = e; break; }
        if (!cross && psp_read32(e + 44) == 0x0E62BF81u && !psp_read32(e + 16) && !psp_read32(e + 20)) cross = e; }
    if (!s) {
        if (!cross) return 0;
        for (uint32_t i = 0; i < cap; i++) if (i >= n || !psp_read8(arr + 56 * i + 52)) { s = arr + 56 * i; if (i >= n) psp_write32(0x45FCB8u, i + 1); break; }
        if (!s) return 0;
        for (int k = 0; k < 56; k += 4) psp_write32(s + k, psp_read32(cross + k));   /* rect, size and texture are set per lookup */
        psp_write32(s + 40, PG_HASH + (uint32_t)g); psp_write32(0x45FCBCu, psp_read32(0x45FCBCu) + 1);
    }
    return g_pg_slot[g] = s;
}
static void glyph_hash_hook(uint32_t addr) { (void)addr;   /* 0x45C60 entry, a0 = name hash */
    int g = g_pg_pending; g_pg_pending = -1;
    const uint32_t h = psp_cpu.r[4], learn = g_pg_learn; g_pg_learn = 0; float size = 24;   /* glyph size, PSP pixels: what the game's own button icons in hint text measure (larger ones are clipped by the line) */
    if (g < 0) {
        /* A PSP button's sub-image, reached by its icon code (learned here)
         * or by an action code the control scheme maps to that button. */
        static struct { uint32_t h, code; } M[32]; static int nm;
        int i = 0; for (; i < nm; i++) if (M[i].h == h) break;
        if (i == nm && learn && nm < 32) { M[nm].h = h; M[nm].code = learn; nm++; }
        if (i == nm || getenv("SWBES_PSP_PROMPTS")) return;
        if (psp_pad_last()) { g = psp_icon_pad_cell(M[i].code); if (g < 0) return; }
        else { const char *k = psp_icon_key(M[i].code, !g_in_ui || g_tut_popup); const int kc = k ? kc_index(k, strlen(k)) : -1;
            if (kc < 0) return;   /* no live cap (arrows): the icon sheet's own picture stays */
            g = PG_N + kc; }
        /* drawn at the size of the PSP icon it replaces */
        const uint32_t arr = psp_read32(0x45FCB4u), n = psp_read32(0x45FCB8u); float px = 0;
        if (arr >= 0x08800000u && arr < 0x0A000000u && n <= 4096)
            for (uint32_t j = 0; j < n; j++) { const uint32_t e = arr + 56 * j;
                if (psp_read8(e + 52) && psp_read32(e + 40) == h) {
                    const float w = (psp_read_f32(e + 24) - psp_read_f32(e + 16)) * psp_read_f32(e + 32), hh = (psp_read_f32(e + 28) - psp_read_f32(e + 20)) * psp_read_f32(e + 36);
                    px = w > hh ? w : hh; break; } }
        size = px > 4 && px < 128 ? px : 24;    }
    const uint32_t s = pg_slot(g); if (!s) return;
    pgt_install(); if (!g_pgt_obj) return;
    enum { ROWS = PG_ROWS };
    psp_write32(s + 12, PGT_HANDLE);   /* our atlas texture */
    psp_write_f32(s + 32, size * PG_COLS); psp_write_f32(s + 36, size * ROWS);
    float x, y, wc = 1;   /* the cell follows the pad's style; a key cap has its own, as wide as its name */
    if (g >= PG_N) { int col, row; kc_cell(g - PG_N, &col, &row); x = (float)col; y = (float)row; wc = kc_w(g - PG_N); }
    else { const int c = pg_cell(g, psp_pad_style()); x = (float)(c % PG_COLS); y = (float)(c / PG_COLS); }
    psp_write_f32(s + 16, x / PG_COLS); psp_write_f32(s + 20, y / ROWS); psp_write_f32(s + 24, (x + wc) / PG_COLS); psp_write_f32(s + 28, (y + 1) / ROWS);
    psp_cpu.r[4] = PG_HASH + (uint32_t)g;
}
static int g_text_equip, g_text_thr;   /* g_text_thr: 1 accelerate, -1 decelerate (the ship hints name the throttle by the take-off code) */   /* the string being rewritten mentions equipment */
static int g_text_hero;    /* ... is the hero offer ("<left> Accept or Decline <right>") */
/* What to call an action's control in on-screen text: its pad button, in the
 * prompt style's own names, while the pad is the last thing used (or a pad
 * style is forced), else its key. */
static const char *pad_btn_name(unsigned m) {
    /* Xbox One / Series, Xbox 360, DualSense, DualShock 4, Switch (by position) */
    static const struct { unsigned m; const char *n[PSP_NSTYLE]; } N[] = {
        { PSP_PAD_A, { "A", "A", "A", "Cross", "Cross", "B" } }, { PSP_PAD_B, { "B", "B", "B", "Circle", "Circle", "A" } },
        { PSP_PAD_X, { "X", "X", "X", "Square", "Square", "Y" } }, { PSP_PAD_Y, { "Y", "Y", "Y", "Triangle", "Triangle", "X" } },
        { PSP_PAD_LB, { "LB", "LB", "LB", "L1", "L1", "L" } }, { PSP_PAD_RB, { "RB", "RB", "RB", "R1", "R1", "R" } },
        { PSP_PAD_LT, { "LT", "LT", "LT", "L2", "L2", "ZL" } }, { PSP_PAD_RT, { "RT", "RT", "RT", "R2", "R2", "ZR" } },
        { PSP_PAD_LS, { "LS", "LS", "LS", "L3", "L3", "LS" } }, { PSP_PAD_RS, { "RS", "RS", "RS", "R3", "R3", "RS" } },
        { PSP_PAD_UP, { "Up", "Up", "Up", "Up", "Up", "Up" } }, { PSP_PAD_DOWN, { "Down", "Down", "Down", "Down", "Down", "Down" } },
        { PSP_PAD_LEFT, { "Left", "Left", "Left", "Left", "Left", "Left" } }, { PSP_PAD_RIGHT, { "Right", "Right", "Right", "Right", "Right", "Right" } },
        { PSP_PAD_START, { "Menu", "Menu", "Start", "Options", "Options", "+" } }, { PSP_PAD_BACK, { "View", "View", "Back", "Create", "Share", "-" } },
        { PSP_PAD_PADDLE_L, { "Paddle L", "Paddle L", "Paddle L", "LB", "LB", "Paddle L" } }, { PSP_PAD_PADDLE_R, { "Paddle R", "Paddle R", "Paddle R", "RB", "RB", "Paddle R" } },
        { PSP_PAD_FN_L, { "Fn L", "Fn L", "Fn L", "LFn", "LFn", "Fn L" } }, { PSP_PAD_FN_R, { "Fn R", "Fn R", "Fn R", "RFn", "RFn", "Fn R" } },
    };
    const int st = psp_pad_style();
    for (size_t i = 0; i < sizeof N / sizeof N[0]; i++) if (N[i].m == m) return N[i].n[st];
    return "?";
}
static const char *bname(int b) { return psp_pad_last() && g_pad_of[b] ? pad_btn_name(g_pad_of[b]) : g_bind[b].name; }
static const char *pc_key_name(uint32_t c) {
    /* Only actions the game layer drives directly (no PSP button behind them
     * on PC) become text; every other code keeps its icon, which the icon
     * sheet override (icon_sheet_post) already draws as the PC key cap. */
    switch (c) {
    case 0xE848: return bname(B_FIRE);      /* shoot */
    case 0xE84A: return bname(B_JUMP);    /* jump / jetpack */
    case 0xE849: case 0xE88C: return bname(B_THROW);   /* secondary fire / grenades; ship missiles */
    case 0xE88B: return bname(B_FIRE);   /* ship lasers */
    case 0xE851: return bname(B_AIM);   /* zoom */
    case 0xE856: return bname(B_CYCLE2);   /* switch secondary */
    case 0xE85C: return bname(B_USE);   /* block / parry: use held */
    case 0xE8C3: return bname(B_JUMP);   /* boost */
    case 0xE8C5: return bname(B_CROUCH);   /* decelerate */
    case 0xE8C9: return bname(B_AUTOPILOT);   /* autopilot */
    case 0xE8D3: return bname(B_TARGET);   /* acquire target */
    case 0xE854: return bname(B_SPRINT);    /* sprint */
    case 0xE855: return bname(B_CYCLE1);    /* switch weapon */
    case 0xE85A: return bname(B_CROUCH);     /* descend (crouch) */
    case 0xE8C4: return g_text_thr ? bname(g_text_thr > 0 ? B_SPRINT : B_CROUCH) : bname(B_JUMP);   /* take off (boost); the throttle in the ship hints */
    case 0xE858: case 0xE887: case 0xE8C6: case 0xE8D2:   /* take weapon, get out, exit vehicle / auto-dock, dock / transition */
        if (getenv("SWBES_VEHDBG") && c >= 0xE8C6) { static long at; if (psp_game_frames - at > 60) { at = psp_game_frames; fprintf(stderr, "prompt %04X f%ld\n", c, psp_game_frames); } }
        if (c == 0xE8D2 && getenv("SWBES_AUTOUSE")) { static int done; if (!done) { done = 1; g_use_pend = 1; fprintf(stderr, "autouse f%ld\n", psp_game_frames); } }   /* test: press use at the first transition prompt */
        return bname(B_USE);
    case 0xE888: return bname(B_SEAT);   /* change seat */
    case 0xE857: return g_text_equip ? bname(B_EQUIP) : bname(B_USE);   /* d-pad up: "Use", but the hint "Press .. to change equipment" uses it too */
    case 0xE803: return g_text_hero ? bname(B_HERO_YES) : NULL;   /* d-pad left: accept hero */
    case 0xE804: return g_text_hero ? bname(B_HERO_NO) : NULL;    /* d-pad right: decline hero */
    default: return NULL;
    }
}
/* The game's Controls pages (Options > Controls > Infantry / Vehicle /
 * Starfighter) list each action next to a PSP button icon. On PC that icon
 * named the wrong key, so there the icons are blanked and each row label
 * gets the PC key in front of it. Labels are matched line by line, exactly. */
static int g_ctrl_page;   /* a Controls list is shown (set by the menu scan) */
/* A Controls-page fixed entry (mouse, WASD) as the pad does it. */
/* A Controls-page fixed entry on the keyboard: the keys bound to it. */
static const char *key_text(const char *k) {
    static char s[64];
#define BN(b) g_bind[b].name
    if (!strcmp(k, "WASD")) snprintf(s, sizeof s, "%s,%s,%s,%s", BN(B_FWD), BN(B_LEFT), BN(B_BACK), BN(B_RIGHT));
    else if (!strcmp(k, "W")) return BN(B_FWD);
    else if (!strcmp(k, "S")) return BN(B_BACK);
    else if (!strcmp(k, "A / D")) snprintf(s, sizeof s, "%s / %s", BN(B_LEFT), BN(B_RIGHT));
    else if (!strcmp(k, "LMB")) return BN(B_FIRE);
    else if (!strcmp(k, "RMB")) return BN(B_AIM);
    else if (!strcmp(k, "Wheel")) return BN(B_CYCLE1);
    else if (!strcmp(k, "RMB+Wheel") || !strcmp(k, "RMB, +Wheel")) snprintf(s, sizeof s, "%s+%s", BN(B_AIM), BN(B_CYCLE1));
    else return k;   /* "Mouse": looking around is the mouse itself */
#undef BN
    return s;
}
static const char *fixed_pad(const char *k) {
    if (!psp_pad_last()) return key_text(k);
    static const struct { const char *k, *fixed; unsigned m; } M[] = {   /* a name, or a pad button named in the prompt style */
        { "WASD", "Left stick", 0 }, { "Mouse", "Right stick", 0 }, { "LMB", 0, B_FIRE },
        { "Wheel", 0, B_CYCLE1 }, { "RMB", 0, B_AIM }, { "RMB, +Wheel", 0, B_AIM }, { "RMB+Wheel", 0, B_AIM },
        { "W", "Left stick", 0 }, { "S", "Left stick", 0 }, { "A / D", 0, B_STUNT },
    };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) if (!strcmp(k, M[i].k)) {
        if (M[i].fixed) return M[i].fixed;
        if (M[i].m == B_STUNT) { static char s[32]; snprintf(s, sizeof s, "Stick + %s", pad_btn_name(g_pad_of[B_STUNT])); return s; }
        return pad_btn_name(g_pad_of[M[i].m]); }
    return k;
}
/* With the pad in use, prompts show a pad glyph (PG_CODE + cell) instead. */
static int bcell(int b) { return g_pad_of[b] ? pg_of_mask(g_pad_of[b]) : -1; }
static int pc_key_cell(uint32_t c) {
    switch (c) {
    case 0xE848: return PG_RT;
    case 0xE84A: return bcell(B_JUMP);
    case 0xE8C4: return g_text_thr ? PG_LSTICK : bcell(B_JUMP);   /* throttle in the ship hints: the stick, as on the PSP */
    case 0xE8C5: return PG_LSTICK;
    case 0xE849: case 0xE88C: return bcell(B_THROW);
    case 0xE88B: return PG_RT;
    case 0xE851: return bcell(B_AIM);
    case 0xE856: return bcell(B_CYCLE2);
    case 0xE85C: return bcell(B_USE);
    case 0xE8C3: return bcell(B_JUMP);
    case 0xE8C9: return bcell(B_AUTOPILOT);
    case 0xE8D3: return bcell(B_TARGET);
    case 0xE854: return bcell(B_SPRINT);
    case 0xE855: return PG_Y;
    case 0xE85A: return bcell(B_CROUCH);
    case 0xE858: case 0xE887: case 0xE8C6: case 0xE8D2: return bcell(B_USE);
    case 0xE888: return bcell(B_SEAT);
    case 0xE857: return g_text_equip ? bcell(B_EQUIP) : bcell(B_USE);
    case 0xE803: return g_text_hero ? bcell(B_HERO_YES) : -1;
    case 0xE804: return g_text_hero ? bcell(B_HERO_NO) : -1;
    default: return -1;
    }
}
static int fixed_cell(const char *k) {
    static const struct { const char *k; int c; } M[] = {
        { "WASD", PG_LSTICK }, { "W", PG_LSTICK }, { "S", PG_LSTICK }, { "A / D", PG_LSTICK }, { "Mouse", PG_RSTICK },
        { "LMB", PG_RT }, { "Wheel", PG_Y }, { "RMB", PG_LT }, { "RMB, +Wheel", PG_LT }, { "RMB+Wheel", PG_LT },
    };
    for (size_t i = 0; k && i < sizeof M / sizeof M[0]; i++) if (!strcmp(k, M[i].k)) return M[i].c;
    return -1;
}
static int g_lc[2];   /* the label's pad glyph cells (ctrl_label_key), -1 = none */
static const char *ctrl_label_key(const char *l) {
    static const struct { const char *label; int bind; const char *fixed; } T[] = {
        { "Movement", -1, "WASD" }, { "Aim Up", -1, "Mouse" }, { "Aim Down", -1, "Mouse" },
        { "Turn Left", -1, "Mouse" }, { "Turn Right", -1, "Mouse" },
        { "Use Primary Weapon", -1, "LMB" }, { "Melee Attack", -1, "LMB" },
        { "(Tap) Jump", B_JUMP }, { "Jump", B_JUMP }, { "(Hold) Sprint", B_SPRINT },
        { "Use Secondary Weapon", B_THROW }, { "(Tap) Fire Secondary", B_THROW },
        { "Cycle Primary Weapon", -1, "Wheel" }, { "Cycle Secondary Weapon", B_CYCLE2 }, { "(Hold) Cycle Secondary", B_CYCLE2 },
        { "(Tap) Use / Enter Vehicle", B_USE }, { "(Hold) Melee Block", B_USE },
        { "(Double Tap) Zoom", -1, "RMB, +Wheel" }, { "(Double-Tap) Zoom", -1, "RMB, +Wheel" }, { "(Tap) Zoom", -1, "RMB, +Wheel" }, { "Zoom", -1, "RMB, +Wheel" },
        { "Zoom (Turret)", -1, "RMB" },
        { "Accelerate", -1, "W" }, { "Up - Accelerate", -1, "W" }, { "Decelerate", -1, "S" }, { "Down - Decelerate", -1, "S" },
        { "Boost", B_JUMP }, { "Change Seat", B_SEAT },
        { "Leave Vehicle", B_USE }, { "(Tap) Leave Vehicle", B_USE }, { "Land Vehicle", B_USE },
        { "(Tap) Cycle Target", B_TARGET }, { "Cycle Target", B_TARGET }, { "(Hold) Roll Correct", B_TARGET }, { "Roll correct", B_TARGET },
        { "Left / Right - Roll", -1, "A / D" },
        /* Default (Standard) scheme labels */
        { "Enter Vehicle / Use", B_USE }, { "Enter / Leave Vehicle", B_USE }, { "Use / Zoom / Enter Vehicle", B_USE },
        { "Jump (Tap) / Sprint (Hold)", -1, NULL }, { "Secondary Attack", B_THROW },
        { "Next Primary Weapon", -1, "Wheel" }, { "Next Secondary Weapon", B_CYCLE2 },
        { "Roll (+ Direction) / Jetpack Down", B_CROUCH }, { "(Tap+Direction) Roll", B_CROUCH },
        { "Zoom / Jetpack Up", -1, "RMB" }, { "Zoom in", -1, "RMB+Wheel" }, { "Zoom In", -1, "RMB+Wheel" },
        { "Zoom out", -1, "RMB+Wheel" }, { "Zoom Out", -1, "RMB+Wheel" },
        { "Zoom In (Turret)", -1, "RMB+Wheel" }, { "Zoom Out (Turret)", -1, "RMB+Wheel" },
        { "Boost (where applicable)", B_JUMP }, { "Accelerate / Thrust (Double-Tap)", -1, "W" },
        { "Match Target Speed", -1, NULL }, { "Stunt (+ Direction)", B_STUNT }, { "Lock On", B_AUTOPILOT }, { "Aim / Turn", -1, "Mouse" },
        { "Force Jump", B_JUMP }, { "(Tap+Direction) Stunt", B_STUNT }, { "Stunt (+ Direction)", B_STUNT },
    };
    g_lc[0] = g_lc[1] = -1;
    if (!strcmp(l, "Jump (Tap) / Sprint (Hold)")) { g_lc[0] = bcell(B_JUMP); g_lc[1] = bcell(B_SPRINT);
        static char b[40]; snprintf(b, sizeof b, "%s / %s", bname(B_JUMP), bname(B_SPRINT)); return b; }
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(l, T[i].label)) { g_lc[0] = T[i].bind >= 0 ? bcell(T[i].bind) : fixed_cell(T[i].fixed);
            return T[i].bind >= 0 ? bname(T[i].bind) : fixed_pad(T[i].fixed); }
    return NULL;
}
static void text_keys_hook(uint32_t addr) {
    static int off = -1; if (off < 0) off = getenv("SWBES_PSP_PROMPTS") != NULL;
    { const char *gt = getenv("SWBES_GLYPHTEST");   /* debug: a hint shows 12 codes from <hex start>, numbered */
      if (gt && addr == 0x0009CDBCu && psp_cpu.r[5] >= 0x08800000u && psp_cpu.r[5] < 0x0A000000u && psp_read16(psp_cpu.r[5]) == 'P') {
        const uint32_t st = (uint32_t)strtoul(gt, NULL, 16), dst = 0x08780000u; uint32_t n = 0;
        for (uint32_t i = 0; i < 12; i++) { psp_write16(dst + 2 * n++, (uint16_t)("0123456789AB"[i])); psp_write16(dst + 2 * n++, (uint16_t)(st + i)); psp_write16(dst + 2 * n++, ' '); }
        psp_write16(dst + 2 * n, 0); psp_cpu.r[5] = dst; return; }
      if (gt) return; }
    if (off) return;
    /* 0x9CDBC: a widget's SetText (a0 widget, a1 string). It keeps its own
     * copy, measured for the box, so rewriting here sizes the box right;
     * 0xF0A10 still catches text drawn straight from the tables. */
    const int reg = addr == 0x0009CDBCu ? 5 : 4;
    const uint32_t src = psp_cpu.r[reg];
    if (src >= 0x08800000u && src < 0x0A000000u) {   /* save warnings name the Memory Stick and the PSP: say it the PC way */
        char t[400]; uint32_t i = 0; for (; i < 399; i++) { const uint32_t c = psp_read16(src + 2 * i); if (!c) break; t[i] = c < 128 ? (char)c : ' '; } t[i] = 0;
        if (strstr(t, "Memory Stick") && (strstr(t, "remove") || strstr(t, "switch off"))) {
            static const char M[] = "Do not close the game while it is saving.";
            const uint32_t dst = 0x08720100u; uint32_t j = 0; for (; M[j]; j++) psp_write16(dst + 2 * j, (uint16_t)M[j]); psp_write16(dst + 2 * j, 0);
            psp_cpu.r[reg] = dst; return; }
    }
    if (src >= 0x08800000u && src < 0x0A000000u) {   /* title screen (drawn fading, so any colour): "Press START button" -- Cross (Enter) starts it too, and is what a PC player reaches for */
        static const char T[] = "Press START button"; uint32_t i = 0;
        for (; T[i]; i++) if (psp_read16(src + 2 * i) != (uint32_t)(unsigned char)T[i]) break;
        if (!T[i] && !psp_read16(src + 2 * i)) {
            uint16_t o[24]; int k = 0; for (const char *s = "Press "; *s; s++) o[k++] = (uint16_t)*s;
            if (psp_pad_last()) o[k++] = (uint16_t)(PG_CODE + PG_A); else { int c = kc_index(g_bind[B_CONFIRM].name, strlen(g_bind[B_CONFIRM].name)); if (c >= 0) o[k++] = (uint16_t)(KC_CODE + c); else for (const char *s = g_bind[B_CONFIRM].name; *s; s++) o[k++] = (uint16_t)*s; }
            o[k++] = 0; const uint32_t dst = 0x08720000u; for (int j = 0; j < k; j++) psp_write16(dst + 2u * (uint32_t)j, o[j]);
            psp_cpu.r[reg] = dst; return; }
    }
    if (src < 0x08800000u || src >= 0x0A000000u || (addr == 0x000F0A10u && psp_cpu.r[6] != 0xFFFFFFFFu)) return;
    uint16_t out[512]; int n = 0, hit = 0;
    { char t[400]; uint32_t i = 0; for (; i < 399; i++) { const uint32_t c = psp_read16(src + 2 * i); if (!c) break; t[i] = c < 128 ? (char)c : ' '; } t[i] = 0; g_text_equip = strstr(t, "equipment") != NULL; g_text_thr = strstr(t, "decelerate") ? -1 : strstr(t, "accelerate") ? 1 : 0; g_text_hero = strstr(t, "Decline") != NULL; }
    { const char *gt = getenv("SWBES_GLYPHTEST");   /* debug: the equipment hint shows codes <base>..<base>+15, each after its low hex digit */
      if (gt && g_text_equip) { const unsigned base = (unsigned)strtoul(gt, NULL, 16); uint16_t o[64]; int k = 0;
        for (unsigned i = 0; i < 16; i++) { o[k++] = (uint16_t)"0123456789ABCDEF"[i]; o[k++] = (uint16_t)(base + i); if (i == 7) o[k++] = 10; }
        o[k++] = 0; for (int i = 0; i < k; i++) psp_write16(0x08700000u + 2u * (uint32_t)i, o[i]); psp_cpu.r[reg] = 0x08700000u; return; } }
    if (getenv("SWBES_GLYPHLOG")) { char t[256]; int k = 0, g = 0;   /* debug: hint strings that carry button codes */
        for (uint32_t i = 0; i < 120 && k < 240; i++) { const uint32_t c = psp_read16(src + 2 * i); if (!c) break;
            if (c >= 0xE800 && c < 0xEB00) { g = 1; k += snprintf(t + k, 256 - k, "<%04X>", c); } else t[k++] = c < 128 ? (char)c : '?'; }
        t[k] = 0; if (g) { static char last[64][64]; static int nl; int seen = 0; for (int i = 0; i < nl; i++) if (!strncmp(last[i], t, 63)) seen = 1;
            if (!seen && nl < 64) { strncpy(last[nl++], t, 63); fprintf(stderr, "glyph: %s\n", t); } } }
    if (g_ctrl_page) {   /* Controls page: blank lone button icons, key-prefix known labels */
        const uint32_t c0 = psp_read16(src);
        if (c0 >= 0xEA00 && c0 < 0xEA20 && !psp_read16(src + 2)) { static uint32_t blank; if (!blank) { blank = 0x0870FFF0u; psp_write16(blank, ' '); psp_write16(blank + 2, 0); } psp_cpu.r[reg] = blank; return; }
        char line[96]; int ln = 0, any = 0; uint16_t o2[512]; int m = 0;
        for (uint32_t i = 0; i < 300; i++) {
            const uint32_t c = psp_read16(src + 2 * i);
            if (c == '\n' || !c) {
                line[ln] = 0; const char *k = ctrl_label_key(line);
                if (k && psp_pad_last() && g_lc[0] >= 0) { any = 1; o2[m++] = (uint16_t)(PG_CODE + g_lc[0]);
                    if (g_lc[1] >= 0) { o2[m++] = ' '; o2[m++] = '/'; o2[m++] = ' '; o2[m++] = (uint16_t)(PG_CODE + g_lc[1]); }
                    o2[m++] = ' '; }
                else if (k) { any = 1; kc_emit(o2, &m, 480, k); o2[m++] = ' '; }
                for (int j = 0; j < ln && m < 500; j++) o2[m++] = (uint16_t)(unsigned char)line[j];
                if (!c) break; o2[m++] = '\n'; ln = 0; continue; }
            if (ln < 95) line[ln++] = c < 128 ? (char)c : '?';
        }
        if (any) { o2[m++] = 0; static uint32_t slot2; const uint32_t dst = 0x08710000u + (slot2++ & 31u) * 0x400u;
            for (int i = 0; i < m; i++) psp_write16(dst + 2u * (uint32_t)i, o2[i]);
            psp_cpu.r[reg] = dst; return; }
    }
    for (uint32_t i = 0; i < 400; i++) {
        const uint32_t c = psp_read16(src + 2 * i);
        if (!c) break;
        const int pc = psp_pad_last() && c >= 0xE800 && c < 0xEB00 ? pc_key_cell(c) : -1;
        const char *k = pc < 0 && c >= 0xE800 && c < 0xEB00 ? pc_key_name(c) : NULL;
        if (pc >= 0) { hit = 1; out[n++] = (uint16_t)(PG_CODE + pc); }
        else if (k) { hit = 1; kc_emit(out, &n, 500, k); }
        else if (n < 508) out[n++] = (uint16_t)c;
    }
    if (!hit) return;
    out[n++] = 0;
    static uint32_t slot; const uint32_t dst = 0x08700000u + (slot++ & 63u) * 0x400u;
    for (int i = 0; i < n; i++) psp_write16(dst + 2u * (uint32_t)i, out[i]);
    psp_cpu.r[reg] = dst;
}
static void throw_key(uint32_t addr) { (void)addr;
    con_run();
    static int sp_was;
    const int sp = !g_in_ui && BIND_DOWN(B_JUMP);
    if (sp && !sp_was) g_jump_until = psp_game_frames + 1;
    { static int c_was; const int c = !g_in_ui && BIND_DOWN(B_CROUCH);
      if (c && !c_was && psp_game_frames - g_fly_frame > 2) {   /* on foot; one-shot: the next read takes it */
        const float mx = move_x(), my = move_y(); const int x = mx < -0.5f ? -1 : mx > 0.5f, y = my < -0.5f ? -1 : my > 0.5f;
        if (x) g_roll_ch = x < 0 ? 11 : 12; else if (y) g_roll_ch = y > 0 ? 13 : 14; else g_crouch_until = 1; }
      c_was = c; }
    sp_was = sp;
    { static int k_was; const int k = !g_in_ui && key_held(B_CYCLE1); if (k && !k_was) g_play_wheel = 1; k_was = k; }   /* next_weapon on a key */
    { static int y_was; const int y = !g_in_ui && pad_held(g_pad_of[B_CYCLE1]); if (y && !y_was) g_play_wheel = 1; y_was = y; }   /* pad: next primary weapon */
    { static int q_was, r_was; const int flying = psp_game_frames - g_fly_frame <= 2;   /* on foot R reloads: no autopilot queued for later */
      const int q = !g_in_ui && flying && BIND_DOWN(B_STUNT), r = !g_in_ui && flying && BIND_DOWN(B_AUTOPILOT);
      /* starfighter: stunt = 15 / 16 with A / D (barrel rolls), else 14 (flip); autopilot = 18 */
      if (q && !q_was) { const float rx = psp_menu_open() ? 0.0f : psp_pad()->rx, sx = fabsf(rx) > fabsf(move_x()) ? rx : move_x();   /* either stick: the right one steers a ship */
          g_fly_pend_ch = sx < -0.5f ? 15 : sx > 0.5f ? 16 : 14; }
      if (r && !r_was) g_fly_pend_ch = 18;
      q_was = q; r_was = r; }
    { static long down_at = -1;   /* use: a tap is channel 23 (use / enter vehicle), held past the tap window it is 28 (melee block) */
      const int u = !g_in_ui && BIND_DOWN(B_USE);
      { static int raw_was; const int raw = BIND_DOWN(B_USE);
        if (raw != raw_was && getenv("SWBES_VEHDBG")) fprintf(stderr, "use key %s f%ld in_ui %d fly_age %ld\n", raw ? "down" : "up", psp_game_frames, g_in_ui, psp_game_frames - g_fly_frame);
        raw_was = raw; }
      /* the press waits for whichever seat reads use (a starfighter may not read it every frame) */
      if (u && down_at < 0) { down_at = psp_game_frames; g_use_pend = 1; g_use_frame = psp_game_frames; }   /* every ch 23 read this frame sees it, as a PSP tap: the weapon pickup reads it before the use handler */
      g_block = u && psp_game_frames - down_at > 15;
      if (!u) down_at = -1; }
    { static int y_was, n_was; const int y = !g_in_ui && BIND_DOWN(B_HERO_YES), n = !g_in_ui && BIND_DOWN(B_HERO_NO);
      /* hero offer: player +272 is 2 while "play as hero?" shows; 3 asks to accept, 4 to decline */
      if ((y && !y_was) || (n && !n_was)) { const uint32_t p = local_player();
          if (p && psp_read32(p + 272) == 2) psp_write32(p + 272, y && !y_was ? 3 : 4); }
      y_was = y; n_was = n; } }
static void weapon_keys(uint32_t soldier) {
    static uint8_t was[6];
    
    const uint32_t inv = psp_read32(soldier + 0x1D0u);
    if (inv < 0x08800000u || inv >= 0x0A000000u) return;
    const uint32_t np = psp_read8(inv + 122), ns = psp_read8(inv + 123);
    if (np > 8 || ns > 8) return;
    for (int i = 0; i < 5; i++) {
        const uint8_t d = (uint8_t)BIND_DOWN(B_W1 + i);
        if (d && !was[i]) {
            if (i < 2) { if ((uint32_t)i < np) game_call(0x002DC830u, inv, 0, (uint32_t)i); }
            else if ((uint32_t)(i - 2) < ns) game_call(0x002DCB0Cu, inv, 0, (uint32_t)(i - 2));
        }
        was[i] = d;
    }
    if (g_play_wheel && psp_game_frames - g_seat_frame > 2) { if (np > 1) g_wheel21 = 1; g_play_wheel = 0; }   /* as the PSP's tap on ch 21, so the tutorial sees the swap */
    g_throw_ok = ns != 0;
}

/* Movies: the game's player (0x12C6A0) opens the file in 0x12CD38 (a1 = path
 * like "fmv\EN\CREDITS.pmf", relative to USRDIR) after muting its own sound.
 * Play it natively here; the original then fails at sceMpegInit and returns
 * as if the movie ended. */
int psp_movie_play(const char *path);
static void movie_hook(uint32_t addr) {
    (void)addr; char g[256], h[400]; uint32_t i = 0;
    for (; i < 255; i++) { const char c = (char)psp_read8(psp_cpu.r[5] + i); if (!c) break; g[i] = c == 92 ? '/' : c; }   /* backslash */ g[i] = 0;
    if (strchr(g, ':')) { const char *p = strchr(g, ':') + 1; while (*p == '/') p++; snprintf(h, sizeof h, "%s/%s", psp_io_disc(), p); }
    else snprintf(h, sizeof h, "%s/PSP_GAME/USRDIR/%s", psp_io_disc(), g);
    psp_movie_play(h);
}
uint32_t g_player_soldier; static long g_soldier_frame = -100; static uint32_t g_last_page;
/* Soft lock (auto target): each frame the targeting routine 0x275C98 ranks
 * enemies in a cone and hands the best to the soldier's SetTarget (vtable
 * +352, 999 = none); aim slowdown and shot aiming follow that target. With
 * aim assist off the player's soldier is told "none". */
static uint32_t g_set_target_fn;
static void set_target_hook(uint32_t addr) {
    (void)addr;
    if (getenv("SWBES_TGTDBG") && psp_cpu.r[4] == g_player_soldier && psp_cpu.r[5] != 999) { static uint32_t last; if (psp_cpu.r[5] != last) { last = psp_cpu.r[5]; fprintf(stderr, "soft lock: target %08X\n", last); } }
    if (psp_cpu.r[4] != g_player_soldier || psp_settings()->aim_assist) return;
    psp_cpu.r[5] = 999;
}
/* While firing, 0x275C98 takes another branch: it keeps its pick at sp+564,
 * hands it to SetTarget at 0x275F9C and then goes on using the local copy
 * (the recent-targets list at +368 that shot aiming reads). Clearing the
 * local pick there stops the shots bending toward it. */
static void soft_lock_pick_hook(uint32_t addr) {
    (void)addr;
    if (psp_cpu.r[17] != g_player_soldier || psp_settings()->aim_assist) return;
    if (getenv("SWBES_TGTDBG") && psp_read32(psp_cpu.r[29] + 564) != 999) fprintf(stderr, "soft lock: cleared fire pick %08X\n", psp_read32(psp_cpu.r[29] + 564));
    psp_write32(psp_cpu.r[29] + 564, 999);
}
static void mouse_look_hook(uint32_t addr) {
    (void)addr;
    if (!g_set_target_fn) { g_set_target_fn = psp_read32(psp_read32(psp_cpu.r[4]) + 352);
        void psp_trace_hook(uint32_t, void (*)(uint32_t)); psp_trace_hook(g_set_target_fn, set_target_hook);
        fprintf(stderr, "soft lock: SetTarget %08X\n", g_set_target_fn);
        void psp_trace_watch_label(uint32_t, void (*)(uint32_t)); psp_trace_watch_label(0x00275F9Cu, soft_lock_pick_hook); psp_trace_hook(0x00275F9Cu, soft_lock_pick_hook); }
    if (getenv("SWBES_TGTDBG")) { static uint32_t last = 1; const uint32_t t = game_call(psp_read32(psp_read32(psp_cpu.r[4]) + 348), psp_cpu.r[4], 0, 0);
        if (t != last) { last = t; fprintf(stderr, "soft lock: current %08X\n", t); } }
    if (psp_cpu.r[4] != g_player_soldier) { g_player_soldier = psp_cpu.r[4]; fprintf(stderr, "player soldier %08X\n", g_player_soldier); }
    g_soldier_frame = psp_game_frames; qm_board_tick();
    weapon_keys(psp_cpu.r[4]);
    if (psp_game_frames - g_fly_frame <= 2) return;   /* piloting: the mouse steers the ship */
    const psp_settings_t *st = psp_settings();   /* PC settings menu (Esc / F1) */
    /* Aim assist off: a1 is the weapon whose assist tuning the step reads
     * (all zero on sniper rifles); 0 takes the step's own early return. */
    if (!st->aim_assist) psp_cpu.r[5] = 0;
    const float sens = 0.0025f * (float)st->sens_pct / 100.0f, inv = st->invert ? -1.0f : 1.0f;
    long dx, dy; psp_mouse_take(&dx, &dy);
    static LARGE_INTEGER t_last; float sy, sp; pad_aim(&sy, &sp, frame_dt(&t_last));   /* right stick */
    if (g_in_ui) sy = sp = 0.0f;
    if (!dx && !dy && sy == 0.0f && sp == 0.0f) return;
    const uint32_t pp = psp_cpu.r[6], yp = psp_cpu.r[7];
    psp_write_f32(yp, psp_read_f32(yp) + (float)dx * sens + sy);
    if (dy || sp != 0.0f) {
        psp_write_f32(pp, psp_read_f32(pp) - (float)dy * sens * inv + sp);
        /* The update applies pitch only while the soldier's manual-look timer
         * (+0x2D4) is running -- normally armed by stick/button pitch input,
         * tested before this hook runs -- else it auto-levels the view. Arm it
         * the same way (default hold time at 0x00411C34). */
        psp_write32(psp_cpu.r[4] + 0x2D4u, psp_read32(0x00411C34u));
    }
}

/* Mouse aim in vehicles. The vehicle input step (0x0020E85C, a0 = vehicle)
 * reads its controller (+68) and adds this frame's aim-axis input to the aim
 * angles it keeps at +468 (pitch, clamped by the step) and +472 (yaw). Adding
 * the raw mouse movement to those angles first turns the aim 1:1. Only a
 * controller with an input mapper (+8) is a player's; AI ones have none. */
static void vehicle_aim_hook(uint32_t addr) {
    (void)addr;
    const uint32_t v = psp_cpu.r[4], ctl = psp_read32(v + 68);
    if (!ctl || !psp_read32(ctl + 8) || g_in_ui) return;
    const psp_settings_t *st = psp_settings();
    const float vk = (float)psp_settings()->veh_sens / 100.0f, sens = 0.0025f * (float)st->sens_pct / 100.0f * vk, inv = st->invert ? -1.0f : 1.0f;
    long dx, dy; psp_mouse_take(&dx, &dy);
    static LARGE_INTEGER t_last; float sy, sp; pad_aim(&sy, &sp, frame_dt(&t_last));   /* right stick */
    if (g_in_ui) sy = sp = 0.0f; sy *= vk; sp *= vk;
    if (getenv("SWBES_VEHDBG")) { static long n; if (n++ % 60 == 0) fprintf(stderr, "vehicle %08X ctl %08X pitch %.3f yaw %.3f\n", v, ctl, psp_read_f32(v + 468), psp_read_f32(v + 472)); }
    if (!dx && !dy && sy == 0.0f && sp == 0.0f) return;
    psp_write_f32(v + 472, psp_read_f32(v + 472) + (float)dx * sens + sy);
    psp_write_f32(v + 468, psp_read_f32(v + 468) - (float)dy * sens * inv + sp);
}

/* ---- Mouse in the game's menus -----------------------------------------
 * The menus are the Asura engine's GUIMenu: MenuPage objects (vtable 0x41E3F8)
 * hold child widgets (array at +0x18, count in the low half of +0x1C). Each
 * widget: +0x04 type (0x7D1 = button), +0x60..+0x6C its rectangle in PSP
 * pixels (x0 y0 x1 y1, floats), +0x90 its name hash, +0x98 nonzero when it
 * takes part in navigation. The page keeps the focused widget's hash at +0xA0.
 * Navigation is GUI events (0x18 bytes) queued at *0x46361C, count 0x463624
 * (max 128): {4, 0, to, from} focuses `to`, {5, 0, w, w} unfocuses `w` -- the
 * pair the d-pad posts. A page's update (0x9ED20) runs once per frame while it
 * is up; that tells us which pages are on screen. Clicking focuses the button
 * and then presses Cross, so the game's own select logic and sounds run. */
/* SWBES_QM_MAP=<index>[,era,mode]: quick match on a chosen map instead of a
 * random one. Does what SWB.FE.SelectRandomMaps (0x2E8A00) does with its dice
 * fixed: the map is the index-th node of the list at 0x5C7EDC (node +0 the
 * map, +8 the next node), era 0..4 and mode 0..1 are checked by 0x1D8ECC,
 * variant 2/4/8 by 0x1D8F68, then 0x1E0BCC(map, era, mode, variant, 1) adds
 * it to the rotation. Era/mode left out = the first valid ones. Every map is
 * listed on stderr with the strings found in its record. */
static uint32_t game_call5(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t t0) {
    g_saved_cpu = psp_cpu;
    psp_cpu.r[4] = a0; psp_cpu.r[5] = a1; psp_cpu.r[6] = a2; psp_cpu.r[7] = a3; psp_cpu.r[8] = t0;
    psp_cpu.r[29] = (psp_cpu.r[29] - 0x100u) & ~15u;
    psp_cpu.r[31] = 0;
    psp_call_from_host(fn);
    const uint32_t v0 = psp_cpu.r[2];
    psp_cpu = g_saved_cpu;
    return v0;
}
static void qm_strings(uint32_t a, int depth) {   /* print ASCII strings at / pointed to by a record */
    for (uint32_t o = 0; o < 0x40; o += 4) {
        const uint32_t p = psp_read32(a + o);
        char s[40]; int n = 0;
        for (; n < 39; n++) { const uint8_t c = psp_read8(p + n); if (c < 32 || c > 126) break; s[n] = (char)c; }
        s[n] = 0;
        if (n >= 3 && psp_read8(p + n) == 0) fprintf(stderr, " +%02X\"%s\"", o, s);
        else if (depth && p >= 0x00010000u && p != a) { fprintf(stderr, " +%02X[", o); qm_strings(p, depth - 1); fprintf(stderr, "]"); }
    }
}
static int qm_pick_map(void) {
    const char *e = getenv("SWBES_QM_MAP");
    const uint32_t lst = 0x005C7EDCu, cnt = psp_read32(lst + 16);
    int want = -1, era = -1, mode = -1;
    if (e) sscanf(e, "%d,%d,%d", &want, &era, &mode);
    uint32_t node = psp_read32(lst), map = 0;
    for (uint32_t i = 0; node && i < cnt && i < 64; i++, node = psp_read32(node + 8)) {
        const uint32_t m = psp_read32(node);
        fprintf(stderr, "qm map %u: %08X", i, m); qm_strings(m, 1); fprintf(stderr, "\n");
        if ((int)i == want) map = m;
    }
    if (!map) return 0;
    for (int er = 0; er < 5; er++) for (int mo = 0; mo < 2; mo++) {
        if ((era >= 0 && er != era) || (mode >= 0 && mo != mode)) continue;
        if (!game_call(0x001D8ECCu, map, (uint32_t)er, (uint32_t)mo)) continue;
        for (uint32_t var = 2; var <= 8; var <<= 1) {
            if (!game_call5(0x001D8F68u, map, (uint32_t)er, (uint32_t)mo, var, 0)) continue;
            psp_write32(0x005CE2F4u, map); psp_write32(0x0045214Cu, (uint32_t)er);
            psp_write32(0x00452150u, (uint32_t)mo); psp_write32(0x005CE2F8u, var);
            game_call5(0x001E0BCCu, map, (uint32_t)er, (uint32_t)mo, var, 1);
            fprintf(stderr, "quickmatch: map %d era %d mode %d variant %u\n", want, er, mo, var);
            return 1;
        }
    }
    fprintf(stderr, "quickmatch: map %d has no valid era/mode\n", want);
    return 0;
}

static uint32_t g_pg[8]; static long g_pg_frame[8]; static int g_pgn;
static void page_tick(uint32_t addr) {
    (void)addr; const uint32_t p = psp_cpu.r[4]; const long f = psp_game_frames; int slot = -1;
    g_last_page = p; con_run();
    /* Debug quick match: SWBES_QUICKMATCH=1 at launch, or a match.cmd file. Runs the
     * game's own "Starts a game with predefined settings" menu command (0x2E7BC4),
     * the one Instant Action uses, once a menu page is up. */
    { static int armed = -1; if (armed < 0) armed = getenv("SWBES_QUICKMATCH") != NULL;
      static long since = -1; if (since < 0) since = f;
      if (!armed && f % 30 == 0) { FILE *mf = fopen("match.cmd", "rb"); if (mf) { fclose(mf); remove("match.cmd"); armed = 1; since = f; } }
      /* Quick match skips the boot's "load an existing profile?" prompt, so load
       * it here (SWB.FE.LoadPlayerProfile) for the saved options -- the
       * control scheme above all -- then give the front end time to settle
       * before starting, or it drops back to the title screen. */
      if (armed == 1 && f - since > 30) { armed = 3; since = f;
          if (!getenv("SWBES_QM_NOPROFILE")) game_call(0x002E8208u, 0, 0, 0);
          fprintf(stderr, "quickmatch: profile load result %u\n", psp_read32(0x005CF26Cu)); }
      if (armed == 3 && f - since > 90 && g_last_page) { armed = 2; fprintf(stderr, "quickmatch: starting\n");
          game_call(0x002E87A0u, 0, 0, 0);   /* empty the map rotation */
          { const char *qm = getenv("SWBES_QUICKMATCH"); const unsigned seed = qm ? (unsigned)atoi(qm) : 0;
            if (seed > 1) psp_write32(0x0045EC80u, seed); }   /* SWBES_QUICKMATCH=<n>, n>1: same map every run */
          if (!qm_pick_map()) game_call(0x002E8A00u, 0, 0, 0);   /* add a random map */
          game_call(0x002E7BC4u, 0, 0, 0); }   /* start with the predefined settings */
    /* ...then confirm team / spawn point with the game's own Select until the soldier is live */
    /* SWBES_QM_CP=<n>: spawn straight at the team's n-th command post (0-based) once one is available */
    if (armed == 2 && !g_player_soldier && f % 45 == 0 && !getenv("SWBES_QM_NOSPAWN")) {
        const char *cp = getenv("SWBES_QM_CP");
        if (!cp || !spawn_at((uint32_t)atoi(cp))) psp_ctrl_pulse(0x4000u, 0, 6); } }
    for (int i = 0; i < g_pgn; i++) if (g_pg[i] == p) { g_pg_frame[i] = f; return; }
    for (int i = 0; i < g_pgn; i++) if (g_pg_frame[i] < f - 2) { slot = i; break; }
    if (slot < 0) { if (g_pgn >= 8) return; slot = g_pgn++; }
    g_pg[slot] = p; g_pg_frame[slot] = f;
}
/* Events are held here and put in the game's queue when its dispatcher
 * (0xA5044) starts, so none land between dispatch and the queue reset. */
static uint32_t g_ev[8][5]; static int g_nev;
static void gui_post5(uint32_t type, uint32_t kind, uint32_t to, uint32_t from, uint32_t arg) {
    if (g_nev < 8) { uint32_t *e = g_ev[g_nev++]; e[0] = type; e[1] = kind; e[2] = to; e[3] = from; e[4] = arg; }
}
static void gui_post(uint32_t type, uint32_t to, uint32_t from) { gui_post5(type, 0, to, from, 0); }
/* {3, 0x7D1, id, id, 0x10}: button `id` activated -- what Cross on a focused
 * button posts. Back (Circle) is the same event on a page's hidden, zero-size
 * back button. */
static void gui_activate(uint32_t id) { gui_post5(3, 0x7D1, id, id, 0x10); }
static void gui_flush(uint32_t addr) {
    (void)addr;
    for (int i = 0; i < g_nev; i++) {
        const uint32_t base = psp_read32(0x0046361Cu), n = psp_read32(0x00463624u);
        if (!base || n >= 0x80) break;
        const uint32_t q = base + n * 0x18u;
        for (int k = 0; k < 5; k++) psp_write32(q + 4u * (uint32_t)k, g_ev[i][k]);
        psp_write32(q + 20, 0);
        psp_write32(0x00463624u, n + 1);
    }
    g_nev = 0;
}
static void menu_mouse(void) {
    static long last = -1; const long f = psp_game_frames;
    if (f == last) return;
    last = f;
    float mx, my; const int in = psp_ui_cursor(&mx, &my);
    uint32_t hit = 0, hit_pg = 0, back = 0, lst = 0, lst_pg = 0, prompt = 0, dot = 0, dot_pg = 0; int ui = 0, tut = 0, ctrl = 0, lst_row = -1; float prompt_x0 = 0.0f;
    /* Buttons the game refuses focus to (enemy or empty spawn points on the
     * spawn map): it snaps focus back to its default, so hovering across one
     * jumped the selection to the first command post. Remember them per page. */
    static uint32_t rej_pg, rej[32], pend_id, pend_pg, pend_prev; static int nrej; static long pend_f;
    if (pend_id && f - pend_f >= 6) {
        const uint32_t now = psp_read32(pend_pg + 0xA0);
        if (now != pend_id) { if (nrej < 32) rej[nrej++] = pend_id;
            fprintf(stderr, "mouse: focus %08X refused on page %08X (now %08X)\n", pend_id, pend_pg, now);
            if (pend_prev && now != pend_prev) { gui_post(4, pend_prev, now); if (now) gui_post(5, now, now); } }   /* put the old selection back */
        pend_id = 0;
    }
    /* The loadout editor (buy menu) draws over the class page, which stays
     * live underneath: while its equipment picker is shown, only its own page
     * takes hover and clicks (prompts still work from any page). */
    uint32_t modal = 0;
    for (int i = 0; i < g_pgn && !modal; i++) {
        if (g_pg_frame[i] < f - 2) continue;
        const uint32_t p = g_pg[i], arr = psp_read32(p + 0x18), n = psp_read32(p + 0x1C) & 0xFFFF;
        if (!arr || n > 256) continue;
        for (uint32_t k = 0; k < n; k++) { const uint32_t w = psp_read32(arr + 4 * k);
            if (w && psp_read32(w + 4) == 0x7DC && (psp_read32(w + 0xCC) & 0xFF)) { modal = p; break; }
            /* the editor without the picker open: its "Done" prompt */
            if (w && psp_read32(w + 4) == 0x7D1 && psp_read32(w + 0x34) == 0x61A5B0FEu && (psp_read32(w + 0xCC) & 0xFF)) { modal = p; break; } }
    }
    for (int i = 0; i < g_pgn; i++) {
        if (g_pg_frame[i] < f - 2) continue;
        const uint32_t p = g_pg[i], arr = psp_read32(p + 0x18), n = psp_read32(p + 0x1C) & 0xFFFF;
        if (!arr || n > 256) continue;
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t w = psp_read32(arr + 4 * k);
            if (modal && p != modal && w && (psp_read32(w + 0x98) || psp_read32(w + 4) != 0x7D1)) continue;   /* under the buy menu */
            if (w && psp_read32(w + 4) == 0x7D1 && !psp_read32(w + 0x98) && psp_read32(w + 0x60) == psp_read32(w + 0x68)
                && psp_read32(w + 0x64) == psp_read32(w + 0x6C) && psp_read32(w + 0x60)) back = psp_read32(w + 0x90);   /* hidden back button */
            /* Scrolling list (0x7DA, e.g. Instant Action's map / playlist):
             * rows are drawn by the widget itself, not separate buttons.
             * +0x1E8 rows, +0x200 selected row, +0x20C top row (float),
             * +0x21C rows visible. */
            if (w && (psp_read32(w + 4) == 0x7DA || psp_read32(w + 4) == 0x7DC) && (psp_read32(w + 0xCC) & 0xFF)) {   /* 0x7DC: the loadout editor's equipment picker, same layout */
                ui = 1;
                const float x0 = psp_read_f32(w + 0x60), y0 = psp_read_f32(w + 0x64), x1 = psp_read_f32(w + 0x68), y1 = psp_read_f32(w + 0x6C);
                const uint32_t rows = psp_read32(w + 0x1E8), vis = psp_read32(w + 0x21C);
                if (in && vis && vis < 64 && rows < 4096 && mx >= x0 && mx < x1 && my >= y0 && my < y1) {
                    const int slot = (int)((my - y0) * (float)vis / (y1 - y0));
                    int r = (int)(psp_read_f32(w + 0x20C) + 0.5f) + slot;
                    /* The picker wraps round and keeps its selection in the
                     * second card: the card above is the previous item. */
                    if (psp_read32(w + 4) == 0x7DC && rows) r = (int)((psp_read32(w + 0x200) + (uint32_t)slot + rows - 1) % rows);
                    lst = w; lst_pg = p; lst_row = r < (int)rows ? r : -1;
                }
                continue;
            }
            /* Button prompts ("(O) Back", "[] Edit"...): nav-0 buttons along the
             * edges. Activating them does nothing; the game polls the pad. So
             * a click presses the PSP button the label stands for, looked up
             * by the label's text hash (+0x34). */
            if (w && psp_read32(w + 4) == 0x7D1 && !psp_read32(w + 0x98) && (psp_read32(w + 0xCC) & 0xFF)) {
                static const struct { uint32_t txt, btn; } P[] = {
                    { 0x4BDF11F8u, 0x4000 },   /* Select: Cross */
                    { 0x9ADB57E3u, 0x2000 },   /* Back: Circle */
                    { 0x4454465Fu, 0x8000 },   /* Edit: Square */
                    { 0x367E2383u, 0x8000 },   /* Customisation Menu: Square */
                    { 0xD8EF3BA8u, 0x1000 },   /* Personalisation Menu: Triangle */
                    { 0x61A5B0FEu, 0x1000 },   /* loadout editor: Done, Triangle */
                    { 0x63D1F4CEu, 0x8000 },   /*   Sell Item, Square */
                    { 0x1F251C76u, 0x2000 },   /*   Cancel, Circle */
                    { 0x7BED7786u, 0x4000 },   /*   Edit, Cross */
                    { 0x1594B8A3u, 0x4000 },   /* in-match tutorial popup: Continue, Cross */
                    { 0x2CA28F67u, 0 }, { 0xDB4653CDu, 0 },   /* "Scroll list", "Select Spawn Point": the d-pad, nothing to click */
                };
                const uint32_t txt = psp_read32(w + 0x34); uint32_t btn = 0; int known = 0;
                ui = 1;   /* a prompt bar alone (the conquest map) is still a menu */
                if (txt == 0x1594B8A3u) tut = 1;   /* the popup pauses play: Enter must be Cross, not Start */
                for (size_t q = 0; q < sizeof P / sizeof P[0]; q++) if (P[q].txt == txt) { btn = P[q].btn; known = 1; }
                const float x0 = psp_read_f32(w + 0x60), y0 = psp_read_f32(w + 0x64), x1 = psp_read_f32(w + 0x68), y1 = psp_read_f32(w + 0x6C);
                if (in && x1 > x0 && mx >= x0 && mx < x1 && my >= y0 && my < y1) {
                    if (!known) { static uint32_t told[32]; static int nt; int seen = 0; for (int q = 0; q < nt; q++) seen |= told[q] == txt;
                        if (!seen && nt < 32) { told[nt++] = txt; fprintf(stderr, "mouse: prompt with unknown label %08X (id %08X)\n", txt, psp_read32(w + 0x90)); } }
                    else if (btn && (!prompt || x0 > prompt_x0)) { prompt = btn; prompt_x0 = x0; }   /* overlapping left-aligned prompts: the one starting nearest */
                }
                continue;
            }
            /* A shown slider / toggle (Advanced Controls has nothing else) or a
             * Back / Select prompt bar also means a menu is up: without this
             * Esc / Enter acted as Start there and play binds stayed live. */
            if (w && psp_read32(w + 4) == 0x7D6 && (psp_read32(w + 0xCC) & 0xFF)) { const uint32_t id = psp_read32(w + 0x90);   /* Controls page lists: infantry, vehicle, starfighter (both schemes) */
                if (id == 0xAD91E01Cu || id == 0x161D7AD6u || id == 0xDCEA980Cu || id == 0xDDD604E6u || id == 0xEFE2E42Eu || id == 0xDE72AC84u) ctrl = 1; }
            if (w && (psp_read32(w + 0xCC) & 0xFF) && (psp_read32(w + 4) == 0x7D2 || psp_read32(w + 4) == 0x7D3
                      || (psp_read32(w + 4) == 0x7D1 && !psp_read32(w + 0x98)))) ui = 1;
            if (!w || psp_read32(w + 4) != 0x7D1 || !psp_read32(w + 0x98) || !(psp_read32(w + 0xCC) & 0xFF)) continue;   /* +0xCC: shown (+0xCF: focused) */
            /* nav 9: a label drawn as a button (the spawn map's command-post info
             * panel). Focusing it makes the game snap to its default command post. */
            /* The loadout screen's class list is nav 9 too, but focusable
             * (+0xCC bit 0x100); the spawn map's panels are not. */
            if (psp_read32(w + 0x98) == 9 && !(psp_read32(w + 0xCC) & 0x100)) continue;
            /* The campaign timeline's mission dots (by id). Focusing one directly
             * picked an empty mission, so the mouse walks the d-pad to it. */
            const uint32_t wid = psp_read32(w + 0x90);
            if ((wid - 0x9EC04F7Du < 10u || wid == 0x3949A072u || wid == 0x3949A073u || wid == 0x382CCFB8u || wid == 0x8135382Fu)) { ui = 1;
                const float x0 = psp_read_f32(w + 0x60), y0 = psp_read_f32(w + 0x64), x1 = psp_read_f32(w + 0x68), y1 = psp_read_f32(w + 0x6C);
                if (in && mx >= x0 - 4 && mx < x1 + 4 && my >= y0 - 4 && my < y1 + 4) { dot = w; dot_pg = p; }
                continue; }
            /* +0x4C bit 0x100: disabled. The spawn map keeps 8 command-post
             * buttons; the unused ones stay "shown" but carry this bit, and two
             * of them sit over the real posts, so hovering a post focused a
             * phantom and the game snapped back to its default. */
            if (psp_read32(w + 0x4C) & 0x100) continue;
            ui = 1;
            const float x0 = psp_read_f32(w + 0x60), y0 = psp_read_f32(w + 0x64), x1 = psp_read_f32(w + 0x68), y1 = psp_read_f32(w + 0x6C);
            int bad = 0; for (int r = 0; r < nrej; r++) bad |= p == rej_pg && rej[r] == psp_read32(w + 0x90);
            if (!bad && in && mx >= x0 && mx < x1 && my >= y0 && my < y1) { hit = w; hit_pg = p; }
        }
    }
    int bare = 0;   /* no match running and no menu page (the conquest map): a click is Cross */
    if (ui && g_player_soldier && f - g_soldier_frame > 120) g_player_soldier = 0;   /* a menu after the battle: its soldier no longer updates */
    if (!ui && !g_player_soldier && f - g_soldier_frame > 30 && f - g_fly_frame > 30 && f - g_seat_any > 30) ui = bare = 1;
    psp_ui_set(ui); g_in_ui = ui; g_tut_popup = tut; g_ctrl_page = ctrl;
    { static long dchk; if (f - dchk > 30) { dchk = f; FILE *df = fopen("widgets.cmd", "rb");   /* debug: dump the live pages' widgets */
        if (df) { fclose(df); remove("widgets.cmd");
          for (int i = 0; i < g_pgn; i++) { if (g_pg_frame[i] < f - 2) continue;
            const uint32_t p = g_pg[i], arr = psp_read32(p + 0x18), n = psp_read32(p + 0x1C) & 0xFFFF;
            fprintf(stderr, "wdump page %08X n %u focus %08X\n", p, n, psp_read32(p + 0xA0));
            for (uint32_t k = 0; arr && k < n && k < 256; k++) { const uint32_t w = psp_read32(arr + 4 * k); if (!w) continue;
              fprintf(stderr, "  w %08X type %X id %08X nav %X cc %08X rect %.1f %.1f %.1f %.1f\n", w, psp_read32(w + 4), psp_read32(w + 0x90), psp_read32(w + 0x98), psp_read32(w + 0xCC),
                psp_read_f32(w + 0x60), psp_read_f32(w + 0x64), psp_read_f32(w + 0x68), psp_read_f32(w + 0x6C));
              if (psp_read32(w + 0x98) == 0xA || (getenv("SWBES_WDUMP0") && psp_read32(w + 0x98) == 0) || getenv("SWBES_WDUMPALL")) { fprintf(stderr, "   "); for (uint32_t o = 0; o < 0x300; o += 4) fprintf(stderr, "%08X%s", psp_read32(w + o), (o & 0x3C) == 0x3C ? "\n   " : " "); fprintf(stderr, "\n"); } } } } } }
    if (ui) {   /* the Scroll list icon (tex_post) follows keyboard / pad and the pad's style: redecode on a change, in menus only (a full redecode hitches) */
        static int was = -1; const int mode = psp_pad_last() ? 1 + psp_pad_style() : 0;
        if (mode != was) { if (was >= 0) psp_tex_refresh(); was = mode; } }
    {   /* [keys] edited while running: load them again (about once a second) */
        static long kchk; static FILETIME was;
        if (f - kchk > 60) { kchk = f; WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExA("settings.ini", GetFileExInfoStandard, &a)) {
                if ((was.dwLowDateTime | was.dwHighDateTime) && CompareFileTime(&a.ftLastWriteTime, &was)) binds_load();
                was = a.ftLastWriteTime; } } }
    {   /* the icon sheet's caps show bound keys (psp_icon_key): redecode when one is rebound */
        char now[1024]; sheet_keys(now, sizeof now);
        if (g_sheet_keys[0] && strcmp(now, g_sheet_keys)) { psp_tex_refresh(); strcpy(g_sheet_keys, now); } }
    { FILE *ef = fopen("ev.cmd", "rb"); if (ef) { unsigned a[5] = {0}; if (fscanf(ef, "%x %x %x %x %x", &a[0], &a[1], &a[2], &a[3], &a[4]) >= 3) gui_post5(a[0], a[1], a[2], a[3], a[4]); fclose(ef); remove("ev.cmd"); } }   /* debug: post a GUI event */
    { FILE *af = fopen("act.cmd", "rb"); if (af) { unsigned id; if (fscanf(af, "%x", &id) == 1) gui_activate(id); fclose(af); remove("act.cmd"); } }   /* debug: activate button id */
    int clicks, rclicks, wheel, moved; psp_ui_take(&clicks, &rclicks, &wheel, &moved);
    if (!ui) { if (wheel) { if (aim_held()) g_zoom_wheel = wheel > 0 ? 1 : -1; else if (g_bind[B_CYCLE1].vk == VK_WHEEL) g_play_wheel = 1; } return; }   /* RMB held: the wheel steps the scope */
    if (getenv("SWBES_HITLOG") && (moved || clicks)) fprintf(stderr, "hit %08X pg %08X lst %08X pg %08X modal %08X\n", hit ? psp_read32(hit + 0x90) : 0, hit_pg, lst ? psp_read32(lst + 0x90) : 0, lst_pg, modal);
    {   /* the campaign era arrows: focusing one empties the mission panel; a click steps the d-pad to the previous / next mission */
        const uint32_t hid = hit ? psp_read32(hit + 0x90) : 0;
        if (hid == 0xA79AB4E7u || hid == 0xA79AB4E8u) { if (clicks) psp_ctrl_pulse(hid == 0xA79AB4E7u ? 0x80u : 0x20u, 0, 3); hit = 0; clicks = 0; } }
    if (hit && (moved || clicks)) {
        const uint32_t id = psp_read32(hit + 0x90), cur = psp_read32(hit_pg + 0xA0);
        if (id != cur) { gui_post(4, id, cur); if (cur) gui_post(5, cur, cur);
            game_call5(0x00096244u, 0, 0, 0, 0, 0);   /* the focus-move sound the d-pad plays (0xA8EA8) */
            if (rej_pg != hit_pg) { rej_pg = hit_pg; nrej = 0; }
            /* judge only the latest hover: an earlier one moved past quickly
             * is not a refusal (that wrongly locked out main-menu buttons) */
            if (!pend_id) pend_prev = cur;
            pend_id = id; pend_pg = hit_pg; pend_f = f; }
    }
    if (lst && (clicks || (moved && psp_read32(lst + 4) != 0x7DC))) {   /* the picker scrolls to keep its selection second from the top: hover would chase it */
        const uint32_t id = psp_read32(lst + 0x90), cur = psp_read32(lst_pg + 0xA0);
        if (id != cur) { gui_post(4, id, cur); if (cur) gui_post(5, cur, cur); game_call5(0x00096244u, 0, 0, 0, 0, 0); }
        if (lst_row >= 0) psp_write32(lst + 0x200, (uint32_t)lst_row);
    }
    {   /* walk to the hovered mission dot along its d-pad links (+0xD4 left, +0xD8 right); a click then presses Cross */
        static uint32_t tgt, tpg; static int go; static long tf;
        if (dot && (moved || clicks)) { tgt = psp_read32(dot + 0x90); tpg = dot_pg; go |= clicks != 0; }
        if (tgt) {
            const uint32_t cur = psp_read32(tpg + 0xA0); int live = 0;
            for (int i = 0; i < g_pgn; i++) live |= g_pg[i] == tpg && g_pg_frame[i] >= f - 2;
            if (!live) tgt = go = 0;
            else if (cur == tgt) { if (go) psp_ctrl_pulse(0x4000u, 2, 3); tgt = go = 0; }
            else if (f - tf >= 6) {
                const uint32_t arr = psp_read32(tpg + 0x18), n = psp_read32(tpg + 0x1C) & 0xFFFF; unsigned dir = 0;
                for (int side = 0; side < 2 && !dir; side++) {
                    uint32_t id = cur;
                    for (int s = 0; s < 32 && id && !dir; s++) { uint32_t w = 0;
                        for (uint32_t k = 0; k < n && k < 256 && !w; k++) { const uint32_t c = psp_read32(arr + 4 * k); if (c && psp_read32(c + 0x90) == id) w = c; }
                        if (!w) break;
                        id = psp_read32(w + (side ? 0xD4 : 0xD8));
                        if (id == tgt) dir = side ? 0x80u : 0x20u; } }
                if (dir) { psp_ctrl_pulse(dir, 0, 3); tf = f; } else tgt = go = 0;
            }
        }
    }
    if (dot) clicks = 0;   /* handled above */
    if (prompt && clicks && !hit && !lst) psp_ctrl_pulse(prompt, 0, 4);   /* clicked a button prompt */
    else if (lst && clicks && lst_row >= 0) psp_ctrl_pulse(0x4000u, 1, 2);   /* Cross on the hovered row */
    else if (hit && clicks) gui_activate(psp_read32(hit + 0x90));
    else if (bare && clicks) psp_ctrl_pulse(0x4000u, 0, 4);
    else if (rclicks) { if (back) gui_activate(back); else psp_ctrl_pulse(0x2000, 0, 4); }   /* no back button: press Circle */
    else if (wheel)    psp_ctrl_pulse(wheel > 0 ? 0x10 : 0x40, 0, 3);   /* d-pad up / down */
}

/* GUI event post (0x9FEB8, a0 = 0x18-byte event): log every one (evwatch.cmd). */
static void ev_log(uint32_t addr) {
    (void)addr; const uint32_t e = psp_cpu.r[4];
    fprintf(stderr, "guiev ra %08X vbl %llu: %08X %08X %08X %08X %08X %08X\n", psp_cpu.r[31], (unsigned long long)psp_display_vblanks(),
            psp_read32(e), psp_read32(e + 4), psp_read32(e + 8), psp_read32(e + 12), psp_read32(e + 16), psp_read32(e + 20));
}
static void fnwatch_log(uint32_t addr) {
    static uint32_t seen[64]; static int ns; uint32_t ra = psp_cpu.r[31];
    uint32_t key = ra ^ (psp_cpu.r[4] * 2654435761u);
    for (int i = 0; i < ns; i++) if (seen[i] == key) return;
    if (ns < 64) seen[ns++] = key;
    fprintf(stderr, "fnwatch: %08X from ra %08X a0 %08X a1 %08X a2 %08X a3 %08X sp %08X\n", addr, ra, psp_cpu.r[4], psp_cpu.r[5], psp_cpu.r[6], psp_cpu.r[7], psp_cpu.r[29]);
}
int swbes_find_game(char *elf, size_t elf_cap, char *disc, size_t disc_cap);
void psp_trace_dump(void);

/* The exe is a windowed app: started from Explorer it has no console, so all
 * output goes to swbes.log beside it. Output a caller already redirected (the
 * test scripts, a terminal) is left alone. */
static char g_log_path[MAX_PATH];
static void log_open(void) {
    const HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    if (e && e != INVALID_HANDLE_VALUE && GetFileType(e) != FILE_TYPE_UNKNOWN) return;
    GetModuleFileNameA(NULL, g_log_path, MAX_PATH);
    char *s = strrchr(g_log_path, '\\'); snprintf(s ? s + 1 : g_log_path, MAX_PATH - (s ? (size_t)(s + 1 - g_log_path) : 0), "swbes.log");
    if (!freopen(g_log_path, "w", stderr)) { g_log_path[0] = 0; return; }
    _dup2(_fileno(stderr), _fileno(stdout));
    setvbuf(stderr, NULL, _IONBF, 0);
}
/* A crash leaves the exception and the last guest functions in the log. */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *x) {
    static volatile LONG once; if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "\ncrash: exception %08lX at %p\n", x->ExceptionRecord->ExceptionCode, x->ExceptionRecord->ExceptionAddress);
    psp_trace_dump(); fflush(stdout); fflush(stderr);
    char msg[MAX_PATH + 128];
    snprintf(msg, sizeof msg, "The game crashed.%s%s", g_log_path[0] ? "\n\nDetails are in " : "", g_log_path);
    MessageBoxA(NULL, msg, "Battlefront: Elite Squadron", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
    log_open();
    SetUnhandledExceptionFilter(crash_filter);
    /* With an argument: that module, its disc in <its folder>/disc (development).
     * Without: the player's game_data folder (gamedata.c). */
    static char found_elf[1024], found_disc[1024];
    const char *elf = argc > 1 ? argv[1] : NULL;
    if (!elf) { if (swbes_find_game(found_elf, sizeof found_elf, found_disc, sizeof found_disc)) return 1; elf = found_elf; }

    setvbuf(stdout, NULL, _IONBF, 0);
    { void psp_prof_start(void); psp_prof_start(); }   /* PSPRECOMP_PROF: sampling profiler (prof.c) */
    /* Frame rate: the game measures its own frame time from the system clock
     * and paces itself with a limiter (frame-time cap at 0x45FE40, else
     * 1/target-fps at 0x45FE4C = 20). With the emulated clock = wall time and
     * vblank waits not blocking, raising that cap gives a higher frame rate at
     * the same game speed. SWBES_FPS=N (default 60, 0 = uncapped);
     * SWBES_VCLOCK=1 restores the old virtual clock (PSP pacing, game at 20). */
    {   /* environment overrides the saved settings (not saved unless changed in the menu) */
        psp_settings_t *st = psp_settings(); const char *e;
        if ((e = getenv("SWBES_FPS")))          st->fps = atoi(e);
        if ((e = getenv("SWBES_MOUSE_SENS")))   st->sens_pct = (int)(atof(e) / 0.0025 * 100 + 0.5);
        if ((e = getenv("SWBES_MOUSE_INVERT"))) st->invert = *e == '1';
    }
    if (!getenv("SWBES_VCLOCK")) { _putenv("PSPRECOMP_REALCLOCK=1"); _putenv("PSPRECOMP_VBLFREE=1"); }
    if (psp_mem_init() != 0) { fprintf(stderr, "memory init failed\n"); return 1; }
    psp_module m;
    if (psp_module_load(elf, MODULE_SPAN, &m) != 0) {
        fprintf(stderr, "usage: %s [decrypted module .elf]\n", argv[0]);
        return 1;
    }
    psp_cpu_reset();
    psp_hle_init();

    /* Files the game opens resolve beside its module. */
    static char root[1024];
    snprintf(root, sizeof root, "%s", elf);
    char *slash = strrchr(root, '/'), *bslash = strrchr(root, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) *slash = '\0'; else strcpy(root, ".");
    if (found_disc[0]) { psp_io_set_root("."); psp_io_set_disc(found_disc); }   /* game_data is the working directory */
    else psp_io_set_root(root);

    psp_recomp_register();

    /* SWBES_RENDERER=gl|software|null; the GPU backend is the default. */
    const char *rname = getenv("SWBES_RENDERER");
    if (psp_render_select(rname ? rname : "gl") != 0) fprintf(stderr, "unknown renderer %s\n", rname);
    printf("renderer: %s\n", psp_render_current()->name);

#ifndef SWBES_VERSION
#define SWBES_VERSION "dev"
#endif
    printf("swbes %s\n", SWBES_VERSION);
    psp_window_version("v" SWBES_VERSION);
    if (psp_window_start("Battlefront: Elite Squadron", 0) != 0) {
        fprintf(stderr, "could not open a window\n");
        return 1;
    }
    printf("SWBES -- module 0x%08X, entry 0x%08X, gp 0x%08X, %d constructors\n",
           m.base, m.entry, m.gp, m.nctors);

    { void psp_trace_watch(uint32_t, void (*)(uint32_t)); psp_trace_watch(0x002738A8u, mouse_look_hook); }
    { extern int psp_direct_play; psp_direct_play = 1; binds_load(); psp_key_remap = key_remap; binds_menu(); void psp_trace_hook(uint32_t, void (*)(uint32_t)); psp_trace_hook(0x0009ED20u, page_tick); psp_trace_hook(0x000A5044u, gui_flush); psp_trace_hook(0x0012C330u, throw_key); psp_trace_hook(0x000B73D0u, ctl_get); psp_trace_hook(0x001E8788u, upg_update); psp_trace_hook(0x001EBE00u, upg_setstate); psp_trace_hook(0x0020E85Cu, vehicle_aim_hook); psp_trace_hook(0x002364D4u, flyer_hook); psp_trace_hook(0x000B9170u, con_print); psp_trace_hook(0x000F0A10u, text_keys_hook); psp_trace_hook(0x0009CDBCu, text_keys_hook); psp_trace_hook(0x0012CD38u, movie_hook); psp_trace_hook(0x003380ECu, glyph_code_hook); psp_trace_hook(0x00045C60u, glyph_hash_hook);
    }
    psp_tex_post = tex_post;
    psp_sprite_color = sprite_color;
    psp_module_start(&m);
    /* SWBES_SHOT=<vblank>[,<vblank>...]: write shot_<n>.ppm when the frame counter passes each. */
    const char *shots = getenv("SWBES_SHOT");
    while (!psp_window_closed()) {
        /* Live driving: "shot.cmd" in the run dir requests a capture (live.ppm). */
        { static unsigned long long lastv; FILE *cf;
          if (psp_display_vblanks() >= lastv + 10 && (lastv = psp_display_vblanks(), 1) && (cf = (psp_cmd_changed(&g_seen_shot) ? fopen("shot.cmd", "rb") : NULL)) != NULL) {
              fclose(cf); remove("shot.cmd");
              int r = psp_display_capture("live_tmp.ppm"); remove("live.ppm"); rename("live_tmp.ppm", "live.ppm");
              {   /* above 1x internal resolution: also the full-size frame (live_hi.ppm) */
                  static unsigned int *hb; int hw, hh; if (!hb) hb = (unsigned int *)malloc(480 * 8 * 272 * 8 * 4);
                  remove("live_hi.ppm");
                  if (hb && psp_display_read_hires(hb, 480 * 8 * 272 * 8, &hw, &hh) == 0) {
                      FILE *o = fopen("live_hi.ppm", "wb");
                      if (o) { fprintf(o, "P6\n%d %d\n255\n", hw, hh);
                          for (int i = 0; i < hw * hh; i++) { const unsigned c = hb[i]; fputc((c >> 16) & 255, o); fputc((c >> 8) & 255, o); fputc(c & 255, o); }
                          fclose(o); }
                  } }
              printf("live capture at vblank %llu -> %d\n", (unsigned long long)psp_display_vblanks(), r); fflush(stdout);
          } }
        { static unsigned long long lr; FILE *rf;
          if (psp_display_vblanks() >= lr + 10 && (lr = psp_display_vblanks(), 1) && (rf = (psp_cmd_changed(&g_seen_ram) ? fopen("ram.cmd", "rb") : NULL)) != NULL) {
              fclose(rf); remove("ram.cmd");
              FILE *o = fopen("ram.bin", "wb");
              if (o) { for (uint32_t a2 = 0x08800000u; a2 < 0x0A000000u; a2 += 4) { uint32_t w = psp_read32(a2); fwrite(&w, 4, 1, o); } fclose(o); }
              o = fopen("mod.bin", "wb");   /* the module's own image, linked at 0 */
              if (o) { for (uint32_t a2 = 0x00100000u; a2 < 0x00700000u; a2 += 4) { uint32_t w = psp_mem_ptr(a2, 4) ? psp_read32(a2) : 0; fwrite(&w, 4, 1, o); } fclose(o); }
              printf("ram dump at vblank %llu\n", (unsigned long long)psp_display_vblanks()); fflush(stdout);
          } }
        /* Debug command files: polled every 100 ms, not every pass (each
         * fopen is a syscall; together they showed up in the frame profile). */
        static ULONGLONG next_poll; ULONGLONG now_ms = GetTickCount64();
        menu_mouse();
        /* Alternate controls by default: with no save yet, hold the profile's
         * scheme flag (0x5C85D0: 0 Default, 1 Alternate) at Alternate until the
         * game has written its first save, which then keeps it. A player who
         * picks Default later keeps that. */
        { static int fresh = -1; static ULONGLONG chk;
          if (fresh < 0) { FILE *sv = fopen("ms/PSP/SAVEDATA/ULES012140000/DATA.BIN", "rb"); fresh = !sv; if (sv) fclose(sv); }
          if (fresh) {
              if (psp_read32(0x005C85D0u) == 0) psp_write32(0x005C85D0u, 1);
              if (now_ms >= chk) { chk = now_ms + 1000; FILE *sv = fopen("ms/PSP/SAVEDATA/ULES012140000/DATA.BIN", "rb"); if (sv) { fclose(sv); fresh = 0; if (psp_read32(0x005C85D0u) == 0) psp_write32(0x005C85D0u, 1); } }   /* in case profile creation reset it: the next save keeps it */
          } }
        { FILE *uf = fopen("ui.cmd", "rb"); if (uf) { float x, y; int c = 0, rc = 0;   /* "x y [clicks] [rclicks]": menu cursor at PSP pixel */
            if (fscanf(uf, "%f %f %d %d", &x, &y, &c, &rc) >= 2) psp_ui_fake(x, y, c, rc); fclose(uf); remove("ui.cmd"); } }
        { FILE *kf = fopen("key.cmd", "rb"); if (kf) { unsigned vk; int dn; while (fscanf(kf, "%x %d", &vk, &dn) == 2) psp_key_fake((int)vk, dn); fclose(kf); remove("key.cmd"); } }
        { FILE *cf = g_con_ready ? NULL : fopen("console.cmd", "rb");
          if (cf) { size_t n = fread(g_con, 1, sizeof g_con - 1, cf); g_con[n] = 0; fclose(cf); remove("console.cmd"); g_con_ready = 1; } }
        /* look.cmd "dx dy [n]": mouse movement, dx/dy every game frame for n frames (default 1) */
        { static long ldx, ldy, ln, lf = -1; void psp_mouse_inject(long, long);
          FILE *mf = fopen("look.cmd", "rb"); if (mf) { long dx, dy, n = 1; int c = fscanf(mf, "%ld %ld %ld", &dx, &dy, &n); if (c >= 2) { ldx = dx; ldy = dy; ln = n > 0 ? n : 1; lf = -1; } fclose(mf); remove("look.cmd"); }
          if (ln > 0 && psp_game_frames != lf) { lf = psp_game_frames; psp_mouse_inject(ldx, ldy); ln--; } }
        /* status.txt, every 250 ms: what the player is doing, for test scripts */
        { static ULONGLONG st_next; if (now_ms >= st_next) { st_next = now_ms + 250; FILE *sf = fopen("status.tmp", "w");
            if (sf) { const long gf = psp_game_frames;
                extern volatile unsigned long long psp_game_cycles; const unsigned long long gcyc = psp_game_cycles;
                const char *mode = gf - g_fly_frame <= 3 ? "fly" : gf - g_soldier_frame <= 3 ? "foot" : "none";
                fprintf(sf, "altctl %u %u\nframe %ld\nmode %s\nui %d\nsoldier %08X\nflyer_map %08X\nfly_obj %08X\npage %08X\nfocus %08X\nhum_map %08X\n",
                        psp_read32(0x005C85D0u), psp_read32(0x005C85D0u + 28), gf, mode, g_in_ui, g_player_soldier, g_fly_map, g_fly_obj, g_last_page, g_last_page ? psp_read32(g_last_page + 0xA0) : 0, g_hum_map);
                fprintf(sf, "cycles %llu\n", gcyc);   /* game-thread CPU cycles at frame end (scripts/bench.ps1) */
                fclose(sf); remove("status.txt"); rename("status.tmp", "status.txt"); } } }
        if (!getenv("SWBES_VCLOCK")) {   /* game frame-time cap, seconds (see main); settings menu or fps.cmd "N" */
            const int f = psp_settings()->fps;
            /* The game's limiter overshoots its target by most of a millisecond
             * (it polls the clock), so 60 gave 57; aim that much short. */
            static float trim = -1.0f; if (trim < 0.0f) { const char *e = getenv("SWBES_FPS_TRIM_US"); trim = (e ? (float)atof(e) : 650.0f) * 1e-6f; }
            float cap = f > 0 ? 1.0f / (float)f - trim : 1e-6f; if (cap < 1e-6f) cap = 1e-6f;
            psp_write_f32(0x0045FE40u, cap); }
        static unsigned seen_poll; const int ch = now_ms >= next_poll && psp_cmd_changed(&seen_poll);
        if (now_ms >= next_poll) { next_poll = now_ms + 100;
        if (ch) { FILE *ef = fopen("evwatch.cmd", "rb"); if (ef) { void psp_trace_watch(uint32_t, void (*)(uint32_t)); psp_trace_watch(0x0009FEB8u, ev_log); fclose(ef); remove("evwatch.cmd"); } }
        { FILE *wf = ch ? fopen("fnwatch.cmd", "rb") : NULL;   /* hex fn addr: log distinct callers */
          if (wf) { unsigned a; if (fscanf(wf, "%x", &a) == 1) { void psp_trace_watch(uint32_t, void (*)(uint32_t)); psp_trace_watch(a, fnwatch_log); } fclose(wf); remove("fnwatch.cmd"); } }
        { extern uint32_t psp_rwatch_live; FILE *wf = ch ? fopen("rwatch.cmd", "rb") : NULL;   /* hex addr: log float readers */
          if (wf) { unsigned a; if (fscanf(wf, "%x", &a) == 1) psp_rwatch_live = a; fclose(wf); remove("rwatch.cmd"); } }
        { FILE *wf = ch ? fopen("rwatchall.cmd", "rb") : NULL;   /* hex addr: log readers of any width (slow) */
          if (wf) { unsigned a; void psp_mem_watch_read(uint32_t); if (fscanf(wf, "%x", &a) == 1) psp_mem_watch_read(a); fclose(wf); remove("rwatchall.cmd"); } }
        { FILE *wf = ch ? fopen("wwatchall.cmd", "rb") : NULL;   /* hex addr: log writers of any width (slow) */
          if (wf) { unsigned a; void psp_mem_watch_write(uint32_t); if (fscanf(wf, "%x", &a) == 1) psp_mem_watch_write(a); fclose(wf); remove("wwatchall.cmd"); } }
        { extern uint32_t psp_watch_live; FILE *wf = ch ? fopen("watch.cmd", "rb") : NULL;   /* hex addr: log writers of that word */
          if (wf) { unsigned a; if (fscanf(wf, "%x", &a) == 1) psp_watch_live = a; fclose(wf); remove("watch.cmd"); } }
        { FILE *pf = ch ? fopen("poke.cmd", "rb") : NULL;     /* lines "addr value" (hex) -> written each frame until removed */
          static uint32_t pa[16], pv[16]; static int pn;
          if (pf) { pn = 0; while (pn < 16 && fscanf(pf, "%x %x", &pa[pn], &pv[pn]) == 2) pn++; fclose(pf); remove("poke.cmd"); }
          for (int i = 0; i < pn; i++) psp_write32(pa[i], pv[i]); }
        { static long hend = -1; FILE *hf = ch ? fopen("fnhist.cmd", "rb") : NULL;   /* "N": function-entry counts over N vblanks -> fnhist.txt */
          void psp_fnhist_start(void); void psp_fnhist_stop(FILE *);
          if (hf) { long n = 0; if (fscanf(hf, "%ld", &n) == 1 && n > 0) { hend = (long)psp_display_vblanks() + n; psp_fnhist_start(); } fclose(hf); remove("fnhist.cmd"); }
          if (hend >= 0 && (long)psp_display_vblanks() >= hend) { FILE *o = fopen("fnhist.tmp", "w"); hend = -1;
              if (o) { psp_fnhist_stop(o); fclose(o); remove("fnhist.txt"); rename("fnhist.tmp", "fnhist.txt"); } } }
        { FILE *ff = ch ? fopen("fps.cmd", "rb") : NULL;
          if (ff) { int f; if (fscanf(ff, "%d", &f) == 1) psp_settings()->fps = f; fclose(ff); remove("fps.cmd"); } }
        { FILE *pf = ch ? fopen("pad.cmd", "rb") : NULL;   /* fake controller: "<hex buttons> lx ly rx ry [lt rt [ps]]" (sticks -1..1, y up; ps 1 = a PlayStation pad); "off" releases */
          if (pf) { psp_pad_t p; memset(&p, 0, sizeof p); unsigned b;
              int ps = 0; if (fscanf(pf, "%x %f %f %f %f %f %f %d", &b, &p.lx, &p.ly, &p.rx, &p.ry, &p.lt, &p.rt, &ps) >= 1) { p.btn = b; p.on = ps ? 2 : 1; }
              psp_pad_fake(&p); fclose(pf); remove("pad.cmd"); } }
        { FILE *rf = ch ? fopen("res.cmd", "rb") : NULL;   /* internal resolution: 0 = auto, 1-8 */
          if (rf) { int v; if (fscanf(rf, "%d", &v) == 1) psp_settings()->res_scale = v; fclose(rf); remove("res.cmd"); } }
        { FILE *sf = ch ? fopen("stick.cmd", "rb") : NULL;   /* "x y" 0-255 (128 centre), held until changed; "-1 -1" = off */
          if (sf) { extern int psp_stick_x, psp_stick_y; int x, y; if (fscanf(sf, "%d %d", &x, &y) == 2) { psp_stick_x = x; psp_stick_y = y;
              fprintf(stderr, "stick %d %d at %llu ms\n", x, y, (unsigned long long)GetTickCount64()); } fclose(sf); remove("stick.cmd"); } }
        { FILE *mf = ch ? fopen("mouse.cmd", "rb") : NULL;    /* "dx dy": inject mouse movement (testing) */
          if (mf) { long dx, dy; if (fscanf(mf, "%ld %ld", &dx, &dy) == 2) psp_mouse_inject(dx, dy); fclose(mf); remove("mouse.cmd"); } }
        { FILE *cf = ch ? fopen("cursor.cmd", "rb") : NULL;   /* "x y [click]": menu cursor at a PSP pixel (testing) */
          if (cf) { int x, y, c = 0; if (fscanf(cf, "%d %d %d", &x, &y, &c) >= 2) psp_ui_cursor_inject(x, y, c); fclose(cf); remove("cursor.cmd"); } }
        { FILE *pf = ch ? fopen("peek.cmd", "rb") : NULL;     /* hex addresses -> peek.txt */
          if (pf) { char ln[64]; FILE *o = fopen("peek_tmp.txt", "wb");
              while (fgets(ln, sizeof ln, pf)) { uint32_t a3 = (uint32_t)strtoul(ln, NULL, 16);
                  if (o) fprintf(o, "%08X: %08X %08X %08X %08X\n", a3, psp_read32(a3), psp_read32(a3 + 4), psp_read32(a3 + 8), psp_read32(a3 + 12)); }
              fclose(pf); remove("peek.cmd"); if (o) fclose(o); remove("peek.txt"); rename("peek_tmp.txt", "peek.txt"); } }
        }
        if (shots && *shots &&psp_display_vblanks() >= strtoull(shots, NULL, 10)) {
            char name[64]; snprintf(name, sizeof name, "shot_%llu.ppm", (unsigned long long)psp_display_vblanks());
            printf("capture %s -> %d\n", name, psp_display_capture(name));
            for (int b = 0; b < 2; b++) {   /* both screen buffers, raw */
                snprintf(name, sizeof name, "vram_%llu_%d.ppm", (unsigned long long)psp_display_vblanks(), b);
                FILE *fp = fopen(name, "wb"); if (!fp) continue;
                fprintf(fp, "P6\n480 272\n255\n");
                for (int i = 0; i < 480 * 272; i++) {
                    uint32_t c = psp_read32(0x04000000u + b * 0x88000u + (uint32_t)((i / 480) * 512 + i % 480) * 4u);
                    uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
                    fwrite(rgb, 1, 3, fp);
                }
                fclose(fp);
            }
            printf("cam ptr %08X fov %08X\n", psp_read32(0x44B51Cu), psp_read32(psp_read32(0x44B51Cu) + 0xFC));
            for (uint32_t o = 0x3C; o <= 0x100; o += 4) {   /* camera block floats */
                uint32_t b = psp_read32(psp_read32(0x44B51Cu) + o); float fv; memcpy(&fv, &b, 4);
                printf("  cam+%03X = %g\n", o, (double)fv);
            }
            for (uint32_t k = 0; k < 8; k++) {   /* first text vertices: u v x y z floats */
                float fv[5];
                for (int j = 0; j < 5; j++) { uint32_t b = psp_read32(0x5C1068u + k * 20u + j * 4u); memcpy(&fv[j], &b, 4); }
                printf("  tv%u uv %g %g pos %g %g %g\n", k, fv[0], fv[1], fv[2], fv[3], fv[4]);
            }
            printf("  clut:");
            for (uint32_t k = 0; k < 16; k++) printf(" %08X", psp_read32(0x09665D10u + k * 4u));
            printf("\n");
            shots = strchr(shots, ','); if (shots) shots++;
        }
        psp_threadman_yield();
    }
    /* The game's threads are still running; leave without unwinding them. */
    fflush(stdout);
    _Exit(0);
}
