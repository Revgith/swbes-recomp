/* psprecomp -- the PC menu and error panel (Dear ImGui, ui.cpp), drawn in
 * software onto the window's frame. Private to window.c / ui.cpp. */
#ifndef PSPRECOMP_UI_H
#define PSPRECOMP_UI_H
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "psprecomp/window.h"
/* ui.cpp, for window.c */
void ui_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp);   /* input for the menu */
void ui_frame(HWND h, uint32_t *px, int w, int hgt);        /* draw onto BGRA top-down pixels */
int  ui_back(void);    /* Esc / B: 1 if the menu used it (closed a list, left a page) */
void ui_toggled(void); /* the menu was opened or closed */

/* window.c, for ui.cpp */
int  ui_menu(void);
void ui_menu_close(void);
const psp_settings_t *ui_defaults(void);
void ui_fullscreen(void);
int  ui_fps(void);
int  ui_nbinds(void);
int  ui_ncheats(void);   /* 0 in multiplayer */
const char *ui_version(void);   /* "" when unset */
const char *ui_cheat(int i);
void ui_cheat_run(int i);
const psp_bind_t *ui_bind(int i);
const char *ui_bval(int i);
int  ui_bind_clash(int i);
void ui_binds_read(void);
void ui_binds_reset(void);
const char *ui_pval(int i);  /* pad button name, NULL: no pad bind */
int  ui_pad_clash(int i);
void ui_pad_clear(int i);
extern int ui_capt;          /* bind waiting for a key, or -1 */
extern int ui_capt_pad;      /* ...for a pad button instead */
extern char ui_kmsg[96];     /* the last refusal */
typedef struct { char src[48], msg[160]; int more; } ui_err_t;
int  ui_errors(ui_err_t *e, int cap);   /* live errors, grouped by source */
int  psp_res_auto(int ch);
#ifdef __cplusplus
}
#endif
#endif
