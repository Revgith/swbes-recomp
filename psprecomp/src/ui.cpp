/* psprecomp -- the PC menu (F1 / pad Back + Start) and the error panel, in
 * Dear ImGui with its default look. ImGui's triangles are drawn in software
 * onto the window's frame (a DIB section), so no GPU context is involved. */
#include "ui.h"
extern "C" {
#include "psprecomp/mods.h"
}
#include "imgui.h"
#include "imgui_internal.h"   /* ImMin / ImMax */
#include "imgui_impl_win32.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

/* ---- software rasterizer for ImDrawData --------------------------------- */
static const uint8_t *g_tex; static int g_tw, g_th;   /* font atlas, alpha8 */

static inline float edge(const ImVec2 &a, const ImVec2 &b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}
/* Shared edges are drawn once: on a tie a pixel belongs to the edge going
 * one way, never the other. */
static inline bool owns(const ImVec2 &a, const ImVec2 &b) { const float dx = b.x - a.x, dy = b.y - a.y; return dy > 0 || (dy == 0 && dx < 0); }

static void tri(uint32_t *px, int pitch, const ImDrawVert *a, const ImDrawVert *b, const ImDrawVert *c, int cl, int ct, int cr, int cb) {
    float area = edge(a->pos, b->pos, c->pos.x, c->pos.y);
    if (fabsf(area) < 1e-6f) return;
    if (area < 0) { const ImDrawVert *t = b; b = c; c = t; area = -area; }
    int x0 = (int)floorf(ImMin(a->pos.x, ImMin(b->pos.x, c->pos.x))), x1 = (int)ceilf(ImMax(a->pos.x, ImMax(b->pos.x, c->pos.x)));
    int y0 = (int)floorf(ImMin(a->pos.y, ImMin(b->pos.y, c->pos.y))), y1 = (int)ceilf(ImMax(a->pos.y, ImMax(b->pos.y, c->pos.y)));
    if (x0 < cl) x0 = cl; if (y0 < ct) y0 = ct; if (x1 > cr) x1 = cr; if (y1 > cb) y1 = cb;
    if (x0 >= x1 || y0 >= y1) return;
    const float inv = 1.0f / area;
    const bool o0 = owns(b->pos, c->pos), o1 = owns(c->pos, a->pos), o2 = owns(a->pos, b->pos);
    const bool solid = a->uv.x == b->uv.x && a->uv.x == c->uv.x && a->uv.y == b->uv.y && a->uv.y == c->uv.y;
    const bool flat = a->col == b->col && a->col == c->col;
    float ca[4], cb_[4], cc[4];
    for (int k = 0; k < 4; k++) { ca[k] = (float)(a->col >> (k * 8) & 255); cb_[k] = (float)(b->col >> (k * 8) & 255); cc[k] = (float)(c->col >> (k * 8) & 255); }
    int solid_t = 255;
    if (solid && g_tex) { int u = (int)(a->uv.x * g_tw), v = (int)(a->uv.y * g_th); u = u < 0 ? 0 : u >= g_tw ? g_tw - 1 : u; v = v < 0 ? 0 : v >= g_th ? g_th - 1 : v; solid_t = g_tex[v * g_tw + u]; }
    for (int y = y0; y < y1; y++) {
        const float py = y + 0.5f; uint32_t *row = px + (size_t)y * pitch;
        for (int x = x0; x < x1; x++) {
            const float pxf = x + 0.5f;
            const float w0 = edge(b->pos, c->pos, pxf, py), w1 = edge(c->pos, a->pos, pxf, py), w2 = edge(a->pos, b->pos, pxf, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !o0) || (w1 == 0 && !o1) || (w2 == 0 && !o2)) continue;
            const float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
            float r, g, bl, al;
            if (flat) { r = ca[0]; g = ca[1]; bl = ca[2]; al = ca[3]; }
            else { r = l0 * ca[0] + l1 * cb_[0] + l2 * cc[0]; g = l0 * ca[1] + l1 * cb_[1] + l2 * cc[1];
                   bl = l0 * ca[2] + l1 * cb_[2] + l2 * cc[2]; al = l0 * ca[3] + l1 * cb_[3] + l2 * cc[3]; }
            int t = solid_t;
            if (!solid && g_tex) {
                int u = (int)((l0 * a->uv.x + l1 * b->uv.x + l2 * c->uv.x) * g_tw), v = (int)((l0 * a->uv.y + l1 * b->uv.y + l2 * c->uv.y) * g_th);
                u = u < 0 ? 0 : u >= g_tw ? g_tw - 1 : u; v = v < 0 ? 0 : v >= g_th ? g_th - 1 : v;
                t = g_tex[v * g_tw + u];
            }
            const int A = (int)(al * t / 255.0f + 0.5f);
            if (A <= 0) continue;
            uint32_t &d = row[x];
            const int dr = d >> 16 & 255, dg = d >> 8 & 255, db = d & 255;
            const int R = dr + (((int)r - dr) * A + 127) / 255, G = dg + (((int)g - dg) * A + 127) / 255, B = db + (((int)bl - db) * A + 127) / 255;
            d = 0xFF000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
        }
    }
}
/* ImGui's filled rectangles (panels, frames, the menu's background): two
 * triangles a b c, a c d over one axis-aligned quad, one colour, one texel.
 * Filled a row at a time, the same pixels tri() would cover (centres inside,
 * left / top edges in, right / bottom out); 0 when the pair is not such a quad. */
static bool rect(uint32_t *px, int pitch, const ImDrawVert *vb, const ImDrawIdx *t, int cl, int ct, int cr, int cb) {
    if (t[3] != t[0] || t[4] != t[2]) return false;
    const ImDrawVert *a = vb + t[0], *b = vb + t[1], *c = vb + t[2], *d = vb + t[5];
    if (a->col != b->col || a->col != c->col || a->col != d->col) return false;
    if (a->uv.x != b->uv.x || a->uv.x != c->uv.x || a->uv.x != d->uv.x || a->uv.y != b->uv.y || a->uv.y != c->uv.y || a->uv.y != d->uv.y) return false;
    const bool h = a->pos.y == b->pos.y && b->pos.x == c->pos.x && c->pos.y == d->pos.y && d->pos.x == a->pos.x;
    const bool v = a->pos.x == b->pos.x && b->pos.y == c->pos.y && c->pos.x == d->pos.x && d->pos.y == a->pos.y;
    if (!h && !v) return false;
    const float fx0 = ImMin(a->pos.x, c->pos.x), fx1 = ImMax(a->pos.x, c->pos.x), fy0 = ImMin(a->pos.y, c->pos.y), fy1 = ImMax(a->pos.y, c->pos.y);
    int x0 = (int)ceilf(fx0 - 0.5f), x1 = (int)ceilf(fx1 - 0.5f), y0 = (int)ceilf(fy0 - 0.5f), y1 = (int)ceilf(fy1 - 0.5f);
    if (x0 < cl) x0 = cl; if (y0 < ct) y0 = ct; if (x1 > cr) x1 = cr; if (y1 > cb) y1 = cb;
    int tx = 255;
    if (g_tex) { int u = (int)(a->uv.x * g_tw), q = (int)(a->uv.y * g_th); u = u < 0 ? 0 : u >= g_tw ? g_tw - 1 : u; q = q < 0 ? 0 : q >= g_th ? g_th - 1 : q; tx = g_tex[q * g_tw + u]; }
    const int r = a->col & 255, g = a->col >> 8 & 255, bl = a->col >> 16 & 255;
    const int A = (int)((float)(a->col >> 24) * tx / 255.0f + 0.5f);
    if (A <= 0 || x0 >= x1 || y0 >= y1) return true;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = px + (size_t)y * pitch;
        for (int x = x0; x < x1; x++) {
            uint32_t &o = row[x];
            const int dr = o >> 16 & 255, dg = o >> 8 & 255, db = o & 255;
            o = 0xFF000000u | (uint32_t)(dr + ((r - dr) * A + 127) / 255) << 16 | (uint32_t)(dg + ((g - dg) * A + 127) / 255) << 8 | (uint32_t)(db + ((bl - db) * A + 127) / 255);
        }
    }
    return true;
}
static void raster(uint32_t *px, int w, int h, const ImDrawData *dd) {
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *l = dd->CmdLists[n];
        const ImDrawVert *vb = l->VtxBuffer.Data; const ImDrawIdx *ib = l->IdxBuffer.Data;
        for (int i = 0; i < l->CmdBuffer.Size; i++) {
            const ImDrawCmd &c = l->CmdBuffer[i];
            if (c.UserCallback) { if (c.UserCallback != ImDrawCallback_ResetRenderState) c.UserCallback(l, &c); continue; }
            const int cl = ImMax(0, (int)c.ClipRect.x), ct = ImMax(0, (int)c.ClipRect.y), cr = ImMin(w, (int)ceilf(c.ClipRect.z)), cb = ImMin(h, (int)ceilf(c.ClipRect.w));
            if (cl >= cr || ct >= cb) continue;
            for (unsigned k = 0; k + 2 < c.ElemCount; k += 3) {
                const ImDrawIdx *t = ib + c.IdxOffset + k;
                if (k + 5 < c.ElemCount && rect(px, w, vb + c.VtxOffset, t, cl, ct, cr, cb)) { k += 3; continue; }
                tri(px, w, vb + c.VtxOffset + t[0], vb + c.VtxOffset + t[1], vb + c.VtxOffset + t[2], cl, ct, cr, cb);
            }
        }
    }
}

/* ---- setup -------------------------------------------------------------- */
static int g_fs;   /* font size the atlas was built at */
static void fonts(int fs) {
    if (fs == g_fs) return;
    g_fs = fs;
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    char path[MAX_PATH]; GetWindowsDirectoryA(path, MAX_PATH); strcat(path, "\\Fonts\\seguisb.ttf");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(path, (float)fs)) {
        ImFontConfig c; c.SizePixels = (float)fs; io.Fonts->AddFontDefault(&c); }
    unsigned char *p; io.Fonts->GetTexDataAsAlpha8(&p, &g_tw, &g_th);
    g_tex = p; io.Fonts->SetTexID((ImTextureID)1);
    ImGuiStyle &s = ImGui::GetStyle(); s = ImGuiStyle(); ImGui::StyleColorsDark(); s.ScaleAllSizes(fs / 13.0f);
}
static void init(HWND h) {
    static bool done; if (done) return; done = true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL; io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(h);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;   /* fed from psp_pad below (XInput and PlayStation pads) */
}
void ui_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) { if (ImGui::GetCurrentContext()) ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp); }
static int g_page, g_want = -1;   /* tab: 0 options, 1 mods, 2 controls; g_want: select it next frame */
void ui_toggled(void) {
    g_want = 0;
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO &io = ImGui::GetIO(); io.ClearEventsQueue(); io.ClearInputKeys();
}

/* ---- pages -------------------------------------------------------------- */
static bool g_dirty, g_full;   /* settings to save; fullscreen to apply, after the frame */
int ui_back(void) {
    if (!ImGui::GetCurrentContext()) return 0;
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return 1;   /* ImGui closes it with the same key */
    if (ui_capt >= 0) return 1;
    return 0;
}
static void cheats(void) {
    const float bw = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < ui_ncheats(); i++) {
        ImGui::PushID(i);
        if (ImGui::Button(ui_cheat(i), ImVec2(bw, 0))) ui_cheat_run(i);
        ImGui::PopID();
    }
}
static void page(int p) { if (p == g_page) return; g_page = p; if (p == 2) { ui_binds_read(); ui_capt = -1; ui_kmsg[0] = 0; } }

static const int FPS_OPT[] = { 30, 60, 90, 120, 144, 0 };
static const char *const PROMPT_NAME[] = { "Auto", "Keyboard", "Xbox Series", "Xbox One", "Xbox 360", "DualSense", "DualShock 4", "Nintendo Switch" };
static const char *const TEXF_NAME[] = { "Original", "Nearest", "Bilinear", "Trilinear 16x" };
enum { NFPS = 6, NPROMPT = 8, NTEXF = 4, NUIF = 3, MAXRES = 8 };
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void row(const char *label) {
    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-FLT_MIN);
}
typedef void (*namer)(int k, char *b, size_t n);
static bool combo(const char *id, int *cur, int n, namer name) {
    char b[48]; bool ch = false;
    if (*cur >= 0 && *cur < n) name(*cur, b, sizeof b); else b[0] = 0;
    if (ImGui::BeginCombo(id, b)) {
        for (int k = 0; k < n; k++) {
            name(k, b, sizeof b); const bool sel = k == *cur;
            ImGui::PushID(k);
            if (ImGui::Selectable(b, sel)) { *cur = k; ch = true; }
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return ch;
}
static void fps_name(int k, char *b, size_t n) { if (FPS_OPT[k]) snprintf(b, n, "%d", FPS_OPT[k]); else snprintf(b, n, "Uncapped"); }
static void prompt_name(int k, char *b, size_t n) { snprintf(b, n, "%s", PROMPT_NAME[k]); }
static void texf_name(int k, char *b, size_t n) { snprintf(b, n, "%s", TEXF_NAME[k]); }
static int g_ch;   /* window height, for Auto resolution */
static void res_name(int k, char *b, size_t n) {
    const int s = k ? k : psp_res_auto(g_ch);
    if (k) snprintf(b, n, "%dx  (%dx%d)", s, 480 * s, 272 * s); else snprintf(b, n, "Auto  (%dx%d)", 480 * s, 272 * s);
}
static bool sens(const char *id, int *pct) {
    float f = *pct / 100.0f;
    if (!ImGui::SliderFloat(id, &f, 0.1f, 5.0f, "%.2fx")) return false;
    *pct = clampi((int)lroundf(f * 20.0f) * 5, 10, 500); return true;
}
static bool check(const char *id, int *v) { bool b = *v != 0; if (!ImGui::Checkbox(id, &b)) return false; *v = b; return true; }

static void options(psp_settings_t *s) {
    if (!ImGui::BeginTable("o", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    bool ch = false;
    { int k = -1; for (int i = 0; i < NFPS; i++) if (FPS_OPT[i] == s->fps) k = i;
      row("Frame rate"); if (combo("##fps", &k, NFPS, fps_name)) { s->fps = FPS_OPT[k]; ch = true; } }
    row("Mouse sensitivity"); ch |= sens("##sens", &s->sens_pct);
    row("Stick sensitivity"); ch |= sens("##pads", &s->pad_sens);
    row("Vehicle sensitivity"); ch |= sens("##vehs", &s->veh_sens);
    row("Invert look"); ch |= check("##inv", &s->invert);
    row("Aim assist"); ch |= check("##aim", &s->aim_assist);
    { int k = clampi(s->prompts, 0, NPROMPT - 1); row("Button prompts"); if (combo("##prompts", &k, NPROMPT, prompt_name)) { s->prompts = k; ch = true; } }
    row("Volume"); ch |= ImGui::SliderInt("##vol", &s->volume, 0, 100, "%d%%");
    { int k = clampi(s->res_scale, 0, MAXRES); row("Internal resolution"); if (combo("##res", &k, MAXRES + 1, res_name)) { s->res_scale = k; ch = true; } }
    { int k = clampi(s->tex_filter, 0, NTEXF - 1); row("Texture filtering"); if (combo("##texf", &k, NTEXF, texf_name)) { s->tex_filter = k; ch = true; } }
    { int k = clampi(s->ui_filter, 0, NUIF - 1); row("UI filtering"); if (combo("##uif", &k, NUIF, texf_name)) { s->ui_filter = k; ch = true; } }
    row("Color grade"); ch |= check("##filter", &s->filter);
    row("Grade blur"); ch |= check("##fblur", &s->filter_blur);
    row("Smooth scaling"); ch |= check("##smooth", &s->smooth);
    row("Fullscreen"); if (check("##full", &s->fullscreen)) { ch = true; g_full = true; }
    row("Show FPS"); ch |= check("##showfps", &s->show_fps);
    ImGui::EndTable();
    ImGui::Spacing();
    if (ImGui::Button("Default settings")) { *s = *ui_defaults(); ch = true; g_full = true; }
    if (ch) g_dirty = true;
}

static void mods(void) {
    const int n = psp_mods_count();
    if (!n) ImGui::TextWrapped("No mods. Put each mod in its own folder in %s", psp_mods_root());
    for (int i = 0; i < n; i++) {
        const psp_mod_t *m = psp_mods_get(i);
        bool on = m->on != 0;
        ImGui::PushID(i);
        if (ImGui::Checkbox(m->name, &on)) psp_mods_set(i, on);
        if (m->author[0] || m->desc[0] || m->files || m->textures)
            ImGui::SetItemTooltip("%s%s%s%s%s%s", m->author[0] ? "by " : "", m->author, m->author[0] && m->desc[0] ? "\n" : "", m->desc,
                                  m->files ? "\n[files]" : "", m->textures ? "\n[textures]" : "");
        if (m->version[0]) { ImGui::SameLine(); ImGui::TextDisabled("%s", m->version); }
        ImGui::PopID();
    }
    ImGui::Separator();
    bool d = psp_mods_dump() != 0;
    if (ImGui::Checkbox("Dump textures", &d)) psp_mods_set_dump(d);
    ImGui::SetItemTooltip("dump\\textures\\<hash>.png");
    if (ImGui::Button("Open mods folder")) psp_mods_open_folder();
    ImGui::TextDisabled("File changes apply on the next load.");
}

static void controls(float list_h) {
    const ImVec4 amber(0.95f, 0.70f, 0.15f, 1.0f);
    if (ImGui::BeginChild("keys", ImVec2(0, list_h), ImGuiChildFlags_NavFlattened)) {
        if (ImGui::BeginTable("k", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("p", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            for (int i = 0; i < ui_nbinds(); i++) {
                const int clash = ui_bind_clash(i);
                row(ui_bind(i)->label);
                char b[48]; snprintf(b, sizeof b, "%s##%d", i == ui_capt && !ui_capt_pad ? "Press a key" : ui_bval(i), i);
                if (clash >= 0) ImGui::PushStyleColor(ImGuiCol_Text, amber);
                if (ImGui::Button(b, ImVec2(-FLT_MIN, 0))) { ui_capt = i; ui_capt_pad = 0; ui_kmsg[0] = 0; }
                if (clash >= 0) { ImGui::PopStyleColor(); ImGui::SetItemTooltip("Also %s", ui_bind(clash)->label); }
                ImGui::TableNextColumn();
                if (const char *pv = ui_pval(i)) {
                    const int pc = ui_pad_clash(i);
                    snprintf(b, sizeof b, "%s##p%d", i == ui_capt && ui_capt_pad ? "Press a button" : pv, i);
                    if (pc >= 0) ImGui::PushStyleColor(ImGuiCol_Text, amber);
                    if (ImGui::Button(b, ImVec2(-FLT_MIN, 0))) { ui_capt = i; ui_capt_pad = 1; ui_kmsg[0] = 0; }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { ui_pad_clear(i); ui_capt = -1; }
                    if (pc >= 0) { ImGui::PopStyleColor(); ImGui::SetItemTooltip("Also %s", ui_bind(pc)->label); }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    if (ui_kmsg[0]) ImGui::TextColored(amber, "%s", ui_kmsg);
    if (ImGui::Button("Default binds")) ui_binds_reset();
}

/* Errors (psp_error), at the top of the picture, while the menu is closed. */
static void errors(int w, int h) {
    static ui_err_t e[16]; const int n = ui_errors(e, 16);
    if (!n) return;
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h / 14.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##errors", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(w * 0.7f);
        for (int i = 0; i < n; i++) {
            if (i && strcmp(e[i].src, e[i - 1].src)) ImGui::Spacing();
            ImGui::TextUnformatted(e[i].msg);
            if (e[i].more) ImGui::TextDisabled("+%d more", e[i].more);
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

/* The pad, as ImGui gamepad keys (not while a bind waits for a key). */
static unsigned g_padb;   /* the pad's buttons for the menu this frame: none while a bind waits, and one
                             * held through a capture stays out until released */
static void pad_live(void) {
    static unsigned mute; const psp_pad_t *p = psp_pad(); const unsigned held = p->on ? p->btn : 0;
    if (ui_capt >= 0) { mute = ~0u; g_padb = 0; return; }
    mute &= held; g_padb = held & ~mute;
}
static void pad_keys(void) {
    const psp_pad_t *p = psp_pad(); ImGuiIO &io = ImGui::GetIO();
    const unsigned b = g_padb; const float k = p->on && ui_capt < 0 ? 1.0f : 0.0f;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (b & PSP_PAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (b & PSP_PAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (b & PSP_PAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (b & PSP_PAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (b & PSP_PAD_A) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (b & PSP_PAD_B) != 0);
    const float ly = p->ly * k, lx = p->lx * k;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > 0.5f, ly > 0 ? ly : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < -0.5f, ly < 0 ? -ly : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lx < -0.5f, lx < 0 ? -lx : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > 0.5f, lx > 0 ? lx : 0);
}

void ui_frame(HWND h, uint32_t *px, int w, int hgt) {
    psp_settings_t *s = psp_settings();
    const int menu = ui_menu(); ui_err_t probe;
    if (!menu && !s->show_fps && !ui_errors(&probe, 1)) return;   /* nothing to draw */
    init(h);
    g_ch = hgt;
    fonts(clampi(hgt / 32, 14, 40));
    ImGui_ImplWin32_NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)hgt);
    if (menu) { pad_live(); pad_keys(); }
    ImGui::NewFrame();

    if (s->show_fps) {   /* the picture's corner, same letterbox as window.c */
        int vw = w, vh = w * 272 / 480; if (vh > hgt) { vh = hgt; vw = hgt * 480 / 272; }
        const ImVec2 p((float)((w - vw) / 2 + 8), (float)((hgt - vh) / 2 + 6));
        char t[32]; snprintf(t, sizeof t, "%d fps", ui_fps());
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 255), t); dl->AddText(p, IM_COL32_WHITE, t);
    }
    if (!menu) errors(w, hgt);
    else {
        const float fs = (float)g_fs, pw = ImMin(fs * 32.0f, w - 16.0f);
        const float top = ImMax(8.0f, hgt * 0.5f - fs * 15.0f);
        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, top), ImGuiCond_Always, ImVec2(0.5f, 0.0f));   /* fixed top: tabs differ in height */
        ImGui::SetNextWindowSize(ImVec2(pw, 0), ImGuiCond_Always);   /* 0: fit the height each frame */
        ImGui::SetNextWindowSizeConstraints(ImVec2(pw, 0), ImVec2(pw, hgt - top - 8.0f));   /* never past the bottom: scrolls */
        bool open = true;
        char title[64]; snprintf(title, sizeof title, "Settings%s%s###Settings", *ui_version() ? "   " : "", ui_version());
        if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings)) {
            /* Tabs; pad LB / RB step through them. */
            static unsigned was; const unsigned b = g_padb, hit = b & ~was; was = b;
            static const char *const TAB[] = { "Options", "Controls", "Mods", "Cheats" }; static const int PG[] = { 0, 2, 1, 3 };
            const int NT = ui_ncheats() ? 4 : 3;
            int t = 0; while (t < NT - 1 && PG[t] != g_page) t++;
            if (hit & PSP_PAD_LB) g_want = PG[(t + NT - 1) % NT];
            if (hit & PSP_PAD_RB) g_want = PG[(t + 1) % NT];
            if (ImGui::BeginTabBar("tabs")) {
                for (int i = 0; i < NT; i++) {
                    if (PG[i] == 2 && !ui_nbinds()) continue;
                    const bool sel = g_want == PG[i];
                    if (ImGui::BeginTabItem(TAB[i], NULL, sel ? ImGuiTabItemFlags_SetSelected : 0)) {
                        page(PG[i]);
                        if (PG[i] == 0) options(s);
                        else if (PG[i] == 1) mods();
                        else if (PG[i] == 3) cheats();
                        else controls(ImMin(ImGui::GetFrameHeightWithSpacing() * 13.0f, hgt - fs * 9.0f));
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            g_want = -1;
        }
        ImGui::End();
        if (!open) ui_menu_close();
    }
    ImGui::Render();
    raster(px, w, hgt, ImGui::GetDrawData());
    if (g_dirty && !ImGui::IsAnyItemActive()) { g_dirty = false; psp_settings_save(); }
    if (g_full) { g_full = false; ui_fullscreen(); }
}
