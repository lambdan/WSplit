/* WSplit for Linux: X11 + cairo front-end. */
#define _GNU_SOURCE
#include "x11.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <cairo/cairo-ft.h>
#include <cairo/cairo-xlib.h>
#include <errno.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

Ui ui;

static FT_Library ft_lib;
static FT_Face digit_ft;

/* ================================================================== */
/* Painter (cairo)                                                     */
/* ================================================================== */

typedef struct {
    cairo_t *cr;
} PaintCtx;

static void set_argb(cairo_t *cr, WsColor c)
{
    cairo_set_source_rgba(cr, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0,
                          ((c >> 24) & 0xFF) / 255.0);
}

void ui_set_color(cairo_t *cr, unsigned rgb) { set_argb(cr, 0xFF000000u | rgb); }

static void use_font(cairo_t *cr, WsFont f)
{
    cairo_set_font_face(cr, ui.face[f]);
    cairo_set_font_size(cr, ui.fsize[f]);
}

static void p_fill_rect(void *ctx, double x, double y, double w, double h, WsColor c)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    set_argb(cr, c);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
}

static void p_fill_gradient(void *ctx, double x, double y, double w, double h, WsColor c1, WsColor c2, bool horiz)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    cairo_pattern_t *pat = horiz ? cairo_pattern_create_linear(x, 0, x + w, 0)
                                 : cairo_pattern_create_linear(0, y + h, 0, y);
    cairo_pattern_add_color_stop_rgba(pat, 0, ((c1 >> 16) & 0xFF) / 255.0, ((c1 >> 8) & 0xFF) / 255.0,
                                      (c1 & 0xFF) / 255.0, ((c1 >> 24) & 0xFF) / 255.0);
    cairo_pattern_add_color_stop_rgba(pat, 1, ((c2 >> 16) & 0xFF) / 255.0, ((c2 >> 8) & 0xFF) / 255.0,
                                      (c2 & 0xFF) / 255.0, ((c2 >> 24) & 0xFF) / 255.0);
    cairo_set_source(cr, pat);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
}

static void p_stroke_line(void *ctx, double x1, double y1, double x2, double y2, WsColor c)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    set_argb(cr, c);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_stroke(cr);
}

static void p_draw_text(void *ctx, WsFont f, const char *s, double x, double y, WsColor c)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    cairo_font_extents_t fe;
    use_font(cr, f);
    cairo_font_extents(cr, &fe);
    set_argb(cr, c);
    cairo_move_to(cr, x, y + fe.ascent);
    cairo_show_text(cr, s);
    cairo_new_path(cr);
}

static double p_text_width(void *ctx, WsFont f, const char *s)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    cairo_text_extents_t te;
    use_font(cr, f);
    cairo_text_extents(cr, s, &te);
    return te.x_advance;
}

static void p_font_metrics(void *ctx, WsFont f, double *ascent, double *descent)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    cairo_font_extents_t fe;
    use_font(cr, f);
    cairo_font_extents(cr, &fe);
    *ascent = fe.ascent;
    *descent = fe.descent;
}

static double p_font_size(void *ctx, WsFont f) { (void)ctx; return ui.fsize[f]; }

static void p_push_clip(void *ctx, double x, double y, double w, double h)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
}

static void p_pop(void *ctx) { cairo_restore(((PaintCtx *)ctx)->cr); }

static void p_push_transform(void *ctx, double tx, double ty, double s)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    cairo_save(cr);
    cairo_translate(cr, tx, ty);
    cairo_scale(cr, s, s);
}

typedef struct {
    cairo_surface_t *img, *gray;
} Icon;

static void draw_placeholder_icon(cairo_t *cr, double x, double y, double s, double alpha)
{
    cairo_save(cr);
    cairo_rectangle(cr, x + 1, y + 1, s - 2, s - 2);
    cairo_set_source_rgba(cr, 0.35, 0.35, 0.38, alpha);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, alpha);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_set_font_face(cr, ui.ui_bold);
    cairo_set_font_size(cr, s * 0.7);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "?", &te);
    cairo_set_source_rgba(cr, 0.85, 0.85, 0.9, alpha);
    cairo_move_to(cr, x + (s - te.width) / 2 - te.x_bearing, y + (s - te.height) / 2 - te.y_bearing);
    cairo_show_text(cr, "?");
    cairo_restore(cr);
}

static void p_draw_icon(void *ctx, void *icon, double x, double y, double size, double alpha, bool gray)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    Icon *ic = icon;
    if (!ic) { draw_placeholder_icon(cr, x, y, size, alpha); return; }
    cairo_surface_t *s = gray ? ic->gray : ic->img;
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    if (w <= 0 || h <= 0) return;
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / w, size / h);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

static void *h_load_icon(void *ud, const char *path)
{
    (void)ud;
    size_t l = strlen(path);
    if (l < 4 || strcasecmp(path + l - 4, ".png") != 0) return NULL; /* cairo loads PNG only */
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return NULL; }
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    cairo_surface_t *g = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(g);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(g);
    unsigned char *d = cairo_image_surface_get_data(g);
    int stride = cairo_image_surface_get_stride(g);
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = (uint32_t *)(d + yy * stride);
        for (int xx = 0; xx < w; xx++) {
            uint32_t p = row[xx];
            unsigned a = p >> 24, r = (p >> 16) & 0xFF, gg = (p >> 8) & 0xFF, b = p & 0xFF;
            /* Segment.Icon setter: mix 4 parts luminance with 1 part colour */
            unsigned lum = (unsigned)(r * 0.3 + gg * 0.59 + b * 0.11);
            r = (lum * 4 + r) / 5; gg = (lum * 4 + gg) / 5; b = (lum * 4 + b) / 5;
            row[xx] = (a << 24) | (r << 16) | (gg << 8) | b;
        }
    }
    cairo_surface_mark_dirty(g);
    Icon *ic = calloc(1, sizeof *ic);
    ic->img = s;
    ic->gray = g;
    return ic;
}

static void h_free_icon(void *ud, void *icon)
{
    (void)ud;
    Icon *ic = icon;
    if (!ic) return;
    cairo_surface_destroy(ic->img);
    cairo_surface_destroy(ic->gray);
    free(ic);
}

static const WsPainter *painter_for(PaintCtx *pc)
{
    static WsPainter p;
    p.ctx = pc;
    p.fill_rect = p_fill_rect;
    p.fill_gradient = p_fill_gradient;
    p.stroke_line = p_stroke_line;
    p.draw_text = p_draw_text;
    p.text_width = p_text_width;
    p.font_metrics = p_font_metrics;
    p.font_size = p_font_size;
    p.push_clip = p_push_clip;
    p.pop_clip = p_pop;
    p.push_transform = p_push_transform;
    p.pop_transform = p_pop;
    p.draw_icon = p_draw_icon;
    return &p;
}

/* ================================================================== */
/* Fonts                                                               */
/* ================================================================== */

void ui_rebuild_fonts(void)
{
    for (int i = 0; i < WS_FONT_COUNT; i++) {
        if (ui.face[i]) cairo_font_face_destroy(ui.face[i]);
        ui.face[i] = NULL;
    }
    const WsSettings *s = &ui.app.settings;
    const char *fam = s->font_family[0] ? s->font_family : "Sans";
    double m = s->font_multiplier > 0 ? s->font_multiplier : 1.0;

    ui.face[WS_FONT_DISPLAY] = cairo_toy_font_face_create(fam, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    ui.fsize[WS_FONT_DISPLAY] = 10.66667 * m;
    ui.face[WS_FONT_TIME] = cairo_toy_font_face_create(fam, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    ui.fsize[WS_FONT_TIME] = 12.0 * m;
    ui.face[WS_FONT_CLOCK_LARGE] = cairo_toy_font_face_create(fam, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    ui.fsize[WS_FONT_CLOCK_LARGE] = 22.66667;
    ui.face[WS_FONT_CLOCK_MED] = cairo_toy_font_face_create(fam, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    ui.fsize[WS_FONT_CLOCK_MED] = 18.66667;

    if (!digit_ft) {
        char path[WS_PATH_MAX + 64];
        snprintf(path, sizeof path, "%s/digital-7-mono.ttf", ui.asset_dir);
        if (!ft_lib) FT_Init_FreeType(&ft_lib);
        if (ft_lib && FT_New_Face(ft_lib, path, 0, &digit_ft) != 0) digit_ft = NULL;
    }
    for (int i = WS_FONT_DIGIT_LARGE; i <= WS_FONT_DIGIT_MED; i++) {
        if (digit_ft) ui.face[i] = cairo_ft_font_face_create_for_ft_face(digit_ft, 0);
        else ui.face[i] = cairo_toy_font_face_create("Monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
    ui.fsize[WS_FONT_DIGIT_LARGE] = 24.0;
    ui.fsize[WS_FONT_DIGIT_MED] = 17.33333;

    if (!ui.ui_face) ui.ui_face = cairo_toy_font_face_create("Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    if (!ui.ui_bold) ui.ui_bold = cairo_toy_font_face_create("Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    ui.need_redraw = true;
}

/* ================================================================== */
/* Drawing                                                             */
/* ================================================================== */

void ui_draw_main(void)
{
    if (!ui.mapped) return;
    int w = ui.app.win_w, h = ui.app.win_h;
    if (w <= 0 || h <= 0) return;
    if (!ui.surf) {
        ui.surf = cairo_xlib_surface_create(ui.dpy, ui.win, ui.visual, w, h);
    } else if (ui.surf_w != w || ui.surf_h != h) {
        cairo_xlib_surface_set_size(ui.surf, w, h);
    }
    ui.surf_w = w;
    ui.surf_h = h;
    cairo_t *cr = cairo_create(ui.surf);
    cairo_push_group(cr);
    PaintCtx pc = {cr};
    ws_render(&ui.app, painter_for(&pc));
    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(ui.surf);
    XFlush(ui.dpy);
    ui.need_redraw = false;
}

/* Render the current state to a PNG (used for screenshots/tests). */
static bool render_png(const char *path)
{
    int w = ui.app.win_w, h = ui.app.win_h;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    PaintCtx pc = {cr};
    ws_render(&ui.app, painter_for(&pc));
    cairo_destroy(cr);
    bool ok = cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(s);
    return ok;
}

/* ================================================================== */
/* Keys                                                                */
/* ================================================================== */

static const struct { KeySym ks; const char *name; } key_names[] = {
    {XK_space, "Space"}, {XK_Return, "Enter"}, {XK_Tab, "Tab"}, {XK_Escape, "Escape"},
    {XK_BackSpace, "Backspace"}, {XK_Delete, "Delete"}, {XK_Insert, "Insert"}, {XK_Home, "Home"},
    {XK_End, "End"}, {XK_Prior, "PageUp"}, {XK_Next, "PageDown"}, {XK_Left, "Left"}, {XK_Right, "Right"},
    {XK_Up, "Up"}, {XK_Down, "Down"}, {XK_Pause, "Pause"}, {XK_Print, "PrintScreen"},
    {XK_Scroll_Lock, "ScrollLock"}, {XK_minus, "Minus"}, {XK_equal, "Equals"},
    {XK_bracketleft, "LeftBracket"}, {XK_bracketright, "RightBracket"}, {XK_semicolon, "Semicolon"},
    {XK_apostrophe, "Quote"}, {XK_comma, "Comma"}, {XK_period, "Period"}, {XK_slash, "Slash"},
    {XK_backslash, "Backslash"}, {XK_grave, "Grave"},
    {XK_KP_Add, "NumpadAdd"}, {XK_KP_Subtract, "NumpadSubtract"}, {XK_KP_Multiply, "NumpadMultiply"},
    {XK_KP_Divide, "NumpadDivide"}, {XK_KP_Decimal, "NumpadDecimal"}, {XK_KP_Enter, "NumpadEnter"},
    /* keypad without NumLock */
    {XK_KP_Insert, "Numpad0"}, {XK_KP_End, "Numpad1"}, {XK_KP_Down, "Numpad2"}, {XK_KP_Next, "Numpad3"},
    {XK_KP_Left, "Numpad4"}, {XK_KP_Begin, "Numpad5"}, {XK_KP_Right, "Numpad6"}, {XK_KP_Home, "Numpad7"},
    {XK_KP_Up, "Numpad8"}, {XK_KP_Prior, "Numpad9"}, {XK_KP_Delete, "NumpadDecimal"},
};

bool ui_keysym_name(KeySym ks, char *out, size_t n)
{
    if (ks >= XK_a && ks <= XK_z) { snprintf(out, n, "%c", (int)('A' + (ks - XK_a))); return true; }
    if (ks >= XK_A && ks <= XK_Z) { snprintf(out, n, "%c", (int)('A' + (ks - XK_A))); return true; }
    if (ks >= XK_0 && ks <= XK_9) { snprintf(out, n, "%c", (int)('0' + (ks - XK_0))); return true; }
    if (ks >= XK_F1 && ks <= XK_F35) { snprintf(out, n, "F%d", (int)(ks - XK_F1 + 1)); return true; }
    if (ks >= XK_KP_0 && ks <= XK_KP_9) { snprintf(out, n, "Numpad%d", (int)(ks - XK_KP_0)); return true; }
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (key_names[i].ks == ks) { snprintf(out, n, "%s", key_names[i].name); return true; }
    const char *s = XKeysymToString(ks);
    if (!s) return false;
    snprintf(out, n, "%s", s);
    return true;
}

KeySym ui_name_keysym(const char *name)
{
    if (!name || !*name) return NoSymbol;
    if (!name[1] && name[0] >= 'A' && name[0] <= 'Z') return XK_a + (name[0] - 'A');
    if (!name[1] && name[0] >= 'a' && name[0] <= 'z') return XK_a + (name[0] - 'a');
    if (!name[1] && name[0] >= '0' && name[0] <= '9') return XK_0 + (name[0] - '0');
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '0' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 35) return XK_F1 + (n - 1);
    }
    if (!strncmp(name, "Numpad", 6) && name[6] >= '0' && name[6] <= '9' && !name[7]) return XK_KP_0 + (name[6] - '0');
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (!strcasecmp(key_names[i].name, name)) return key_names[i].ks;
    return XStringToKeysym(name);
}

unsigned ui_x_mods_to_ws(unsigned st)
{
    unsigned m = 0;
    if (st & ControlMask) m |= WS_MOD_CTRL;
    if (st & Mod1Mask) m |= WS_MOD_ALT;
    if (st & ShiftMask) m |= WS_MOD_SHIFT;
    if (st & Mod4Mask) m |= WS_MOD_SUPER;
    return m;
}

static unsigned ws_mods_to_x(unsigned m)
{
    unsigned st = 0;
    if (m & WS_MOD_CTRL) st |= ControlMask;
    if (m & WS_MOD_ALT) st |= Mod1Mask;
    if (m & WS_MOD_SHIFT) st |= ShiftMask;
    if (m & WS_MOD_SUPER) st |= Mod4Mask;
    return st;
}

/* ---------------------------------------------------- global hotkeys */

static bool grab_failed;
static int grab_error_handler(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess) grab_failed = true;
    return 0;
}

static const unsigned lock_combos[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

static void ungrab_hotkeys(void)
{
    for (int i = 0; i < WS_HK_COUNT; i++) {
        if (!ui.grabs[i].grabbed) continue;
        for (size_t j = 0; j < 4; j++) XUngrabKey(ui.dpy, ui.grabs[i].code, ui.grabs[i].mods | lock_combos[j], ui.root);
        ui.grabs[i].grabbed = false;
    }
}

static void h_hotkeys_changed(void *ud)
{
    (void)ud;
    ungrab_hotkeys();
    const WsSettings *s = &ui.app.settings;
    XSync(ui.dpy, False);
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(grab_error_handler);
    char failed[512] = "";
    for (int i = 0; i < WS_HK_COUNT; i++) {
        if (!s->hotkeys[i][0]) continue;
        if (i != WS_HK_TOGGLE && !s->hotkeys_enabled) continue;
        unsigned mods;
        char key[64];
        if (!ws_hotkey_parse(s->hotkeys[i], &mods, key, sizeof key)) continue;
        KeySym ks = ui_name_keysym(key);
        KeyCode kc = ks != NoSymbol ? XKeysymToKeycode(ui.dpy, ks) : 0;
        if (!kc) continue;
        grab_failed = false;
        unsigned xm = ws_mods_to_x(mods);
        for (size_t j = 0; j < 4; j++) XGrabKey(ui.dpy, kc, xm | lock_combos[j], ui.root, True, GrabModeAsync, GrabModeAsync);
        XSync(ui.dpy, False);
        ui.grabs[i].code = kc;
        ui.grabs[i].mods = xm;
        ui.grabs[i].grabbed = true;
        if (grab_failed) {
            size_t l = strlen(failed);
            snprintf(failed + l, sizeof failed - l, "%s%s", l ? ", " : "", s->hotkeys[i]);
        }
    }
    XSetErrorHandler(old);
    if (failed[0]) {
        char msg[700];
        snprintf(msg, sizeof msg, "These hotkeys are already taken by another application:\n%s", failed);
        ui_message("Global hotkeys", msg);
    }
    ui.need_redraw = true;
}

static bool handle_hotkey_event(XKeyEvent *ke)
{
    unsigned st = ke->state & (ControlMask | Mod1Mask | ShiftMask | Mod4Mask);
    for (int i = 0; i < WS_HK_COUNT; i++) {
        if (ui.grabs[i].grabbed && ui.grabs[i].code == ke->keycode && ui.grabs[i].mods == st) {
            ws_app_hotkey(&ui.app, (WsHotkeyAction)i);
            ui.need_redraw = true;
            return true;
        }
    }
    return false;
}

/* ================================================================== */
/* Host callbacks                                                      */
/* ================================================================== */

static WsAnswer h_ask(void *ud, const char *t, const char *m, bool c) { (void)ud; return ui_ask(t, m, c); }
static void h_message(void *ud, const char *t, const char *m) { (void)ud; ui_message(t, m); }

static bool h_choose_save_path(void *ud, char *out, size_t n)
{
    (void)ud;
    char def[300];
    snprintf(def, sizeof def, "%s.wsplit", ui.app.run.title[0] ? ui.app.run.title : "splits");
    for (char *p = def; *p; p++) if (*p == '/') *p = '_';
    return ui_choose_file(true, "Save splits", "WSplit runs|*.wsplit *.txt", def, out, n);
}

static void set_size_hints(int w, int h)
{
    XSizeHints *sh = XAllocSizeHints();
    sh->flags = PMinSize | PMaxSize;
    sh->min_width = sh->max_width = w;
    sh->min_height = sh->max_height = h;
    XSetWMNormalHints(ui.dpy, ui.win, sh);
    XFree(sh);
}

static void h_resize_window(void *ud, int w, int h)
{
    (void)ud;
    if (!ui.win) return;
    set_size_hints(w, h);
    XResizeWindow(ui.dpy, ui.win, (unsigned)w, (unsigned)h);
    ui.need_redraw = true;
}

static void h_invalidate(void *ud) { (void)ud; ui.need_redraw = true; }

static void send_net_wm_state(bool add, Atom a)
{
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = ui.win;
    ev.xclient.message_type = ui.a_net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = add ? 1 : 0;
    ev.xclient.data.l[1] = (long)a;
    ev.xclient.data.l[3] = 1;
    XSendEvent(ui.dpy, ui.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

static void h_apply_window_props(void *ud)
{
    (void)ud;
    const WsSettings *s = &ui.app.settings;
    if (ui.mapped) {
        send_net_wm_state(s->on_top, ui.a_net_wm_state_above);
    } else if (s->on_top) {
        XChangeProperty(ui.dpy, ui.win, ui.a_net_wm_state, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&ui.a_net_wm_state_above, 1);
    }
    if (s->opacity < 0.999) {
        unsigned long op = (unsigned long)(s->opacity * 0xFFFFFFFFul);
        XChangeProperty(ui.dpy, ui.win, ui.a_net_wm_opacity, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&op, 1);
    } else {
        XDeleteProperty(ui.dpy, ui.win, ui.a_net_wm_opacity);
    }
    XFlush(ui.dpy);
}

/* ================================================================== */
/* Commands that need UI                                               */
/* ================================================================== */

static long long file_mtime(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

/* bool run_capture-like helper lives in ui.c; use a simple popen here (fast commands only). */
static bool capture_line(const char *cmd, char *out, size_t n)
{
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    pclose(f);
    if (ok) { char *nl = strchr(out, '\n'); if (nl) *nl = 0; }
    return ok && out[0];
}

static void open_in_editor(const char *path)
{
    /* .wsplit files have no MIME type and contain commas, so xdg-open tends to
       pick a spreadsheet. Prefer the default *text* editor. */
    const char *env = getenv("WSPLIT_EDITOR");
    char desktop[256];
    if (env && *env) {
        const char *argv[] = {"/bin/sh", "-c", "exec $WSPLIT_EDITOR \"$0\"", path, NULL};
        ui_spawn_detached(argv);
    } else if (ui_have_program("xdg-mime") && ui_have_program("gtk-launch") &&
               capture_line("xdg-mime query default text/plain 2>/dev/null", desktop, sizeof desktop)) {
        const char *argv[] = {"gtk-launch", desktop, path, NULL};
        ui_spawn_detached(argv);
    } else {
        static const char *eds[] = {"gnome-text-editor", "gedit", "kate", "kwrite", "mousepad", "xed", "pluma",
                                    "featherpad", "leafpad", "code", NULL};
        bool done = false;
        for (int i = 0; eds[i] && !done; i++) {
            if (ui_have_program(eds[i])) {
                const char *argv[] = {eds[i], path, NULL};
                ui_spawn_detached(argv);
                done = true;
            }
        }
        if (!done && ui_have_program("xdg-open")) {
            const char *argv[] = {"xdg-open", path, NULL};
            ui_spawn_detached(argv);
        }
    }
    snprintf(ui.watch_path, sizeof ui.watch_path, "%s", path);
    ui.watch_mtime = file_mtime(path);
    ui.watch_pending = false;
}

static void cmd_edit_run(void)
{
    WsApp *a = &ui.app;
    if (!ws_run_live(&a->run)) {
        char path[WS_PATH_MAX];
        if (!ui_choose_file(true, "Create new splits file", "WSplit runs|*.wsplit *.txt", "new_run.wsplit", path, sizeof path))
            return;
        FILE *f = fopen(path, "w");
        if (!f) { ui_message("New run", "Could not create the file."); return; }
        fputs("Title=New Run\nGoal=\nAttempts=0\nOffset=0\nSize=0,0\n"
              "Segment 1,0,0,0\nSegment 2,0,0,0\nSegment 3,0,0,0\nIcons=\n", f);
        fclose(f);
        ws_app_load_file(a, path);
    } else {
        if (!a->run.file || a->run.unsaved) {
            WsAnswer r = a->run.file ? ui_ask("Edit run", "Save your splits before editing them?", true) : WS_ANSWER_YES;
            if (r == WS_ANSWER_CANCEL) return;
            if (r == WS_ANSWER_YES && !ws_app_save(a)) return;
        }
        if (!a->run.file) return;
    }
    ui_message("Edit run",
               "The splits file will open in your text editor.\n\n"
               "One segment per line:  Name,OldTime,BestTime,BestSegment\n"
               "Times are seconds (e.g. 83.45) or h:mm:ss.ff; use 0 for none.\n"
               "Icons= lists icon paths in quotes, separated by commas.\n\n"
               "WSplit reloads the file automatically when you save it\n"
               "(while the timer is not running).");
    open_in_editor(a->run.file);
}

static void cmd_open(void)
{
    WsApp *a = &ui.app;
    if (!ws_app_prompt_for_save(a)) return;
    char path[WS_PATH_MAX];
    if (!ui_choose_file(false, "Open splits", "WSplit runs|*.wsplit *.txt", NULL, path, sizeof path)) return;
    if (!ws_app_load_file(a, path)) ui_message("Open", "That file could not be opened as a WSplit run.");
}

static void cmd_import_lss(void)
{
    WsApp *a = &ui.app;
    if (!ws_app_prompt_for_save(a)) return;
    char path[WS_PATH_MAX];
    if (!ui_choose_file(false, "Import LiveSplit splits", "LiveSplit splits|*.lss", NULL, path, sizeof path)) return;
    ws_app_close_file(a);
    if (!ws_runfile_import_lss(&a->run, path)) {
        ws_app_close_file(a);
        ui_message("Import", "No segments could be read from that LiveSplit file.");
        return;
    }
    ws_app_run_edited(a);
    ui_message("Import", "Splits imported. Use Save As... to keep them as a .wsplit file.");
}

static void cmd_start_at(void)
{
    UiDialog d = {0};
    d.title = "Start at...";
    d.message = "Start offset (e.g. 1:30 or 12.5):";
    d.buttons[0] = "Start";
    d.buttons[1] = "Cancel";
    d.nbuttons = 2;
    d.default_button = 0;
    d.cancel_button = 1;
    d.has_entry = true;
    snprintf(d.entry, sizeof d.entry, "0:00");
    d.check_label = "Still use delay";
    d.check = false;
    if (ui_dialog_run(&d) != 0) return;
    ws_app_start_at(&ui.app, ws_parse_time(d.entry), d.check);
}

static void cmd_start_delay(void)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%g", ui.app.run.start_delay_ms / 1000.0);
    if (!ui_prompt("Run start delay", "Delay before the timer starts, in seconds\n(shown as a negative countdown):", buf, sizeof buf))
        return;
    ui.app.run.start_delay_ms = (int)llround(ws_parse_time(buf) * 1000.0);
    ui.app.run.unsaved = true;
    ws_app_initialize_display(&ui.app);
}

static void cmd_font(void)
{
    char buf[128];
    snprintf(buf, sizeof buf, "%s", ui.app.settings.font_family);
    if (!ui_prompt("Segment font", "Font family for segment names and times\n(leave empty for the default sans-serif):", buf, sizeof buf))
        return;
    snprintf(ui.app.settings.font_family, sizeof ui.app.settings.font_family, "%s", buf);
    ui_rebuild_fonts();
}

static void cmd_set_hotkey(int which)
{
    UiDialog d = {0};
    char msg[256];
    snprintf(msg, sizeof msg, "Press the key combination for \"%s\".\n\nEscape cancels, Backspace clears.", ws_hotkey_labels[which]);
    d.title = "Set global hotkey";
    d.message = msg;
    d.buttons[0] = "Cancel";
    d.nbuttons = 1;
    d.default_button = 0;
    d.cancel_button = 0;
    d.capture_key = true;
    if (ui_dialog_run(&d) != 1) return; /* 1 = captured */
    snprintf(ui.app.settings.hotkeys[which], sizeof ui.app.settings.hotkeys[which], "%s", d.captured);
    if (d.captured[0] && which != WS_HK_TOGGLE && !ui.app.settings.hotkeys_enabled) {
        if (ui_ask("Global hotkeys", "Global hotkeys are currently disabled. Enable them now?", false) == WS_ANSWER_YES)
            ui.app.settings.hotkeys_enabled = true;
    }
    h_hotkeys_changed(NULL);
}

static void cmd_colors(bool save)
{
    char path[WS_PATH_MAX];
    if (!ui_choose_file(save, save ? "Save color scheme" : "Load color scheme", "WSplit colors|*.wscolors *.ini",
                        save ? "colors.wscolors" : NULL, path, sizeof path))
        return;
    if (save) {
        if (!ws_colors_save(&ui.app.settings.colors, path)) ui_message("Colors", "Could not save the color scheme.");
    } else {
        WsColors c = ui.app.settings.colors;
        if (ws_colors_load(&c, path)) { ui.app.settings.colors = c; ws_app_colors_changed(&ui.app); }
        else ui_message("Colors", "No colors could be read from that file.");
    }
}

static void cmd_exit(void)
{
    if (ws_app_prompt_for_save(&ui.app)) ui.quit = true;
}

static void run_command(int id)
{
    if (id >= WS_CMD_SET_HOTKEY_BASE && id < WS_CMD_SET_HOTKEY_BASE + WS_HK_COUNT) {
        cmd_set_hotkey(id - WS_CMD_SET_HOTKEY_BASE);
        return;
    }
    switch (id) {
    case WS_CMD_EDIT_RUN: case WS_CMD_NEW_RUN: cmd_edit_run(); break;
    case WS_CMD_OPEN: cmd_open(); break;
    case WS_CMD_IMPORT_LSS: cmd_import_lss(); break;
    case WS_CMD_START_AT: cmd_start_at(); break;
    case WS_CMD_START_DELAY: cmd_start_delay(); break;
    case WS_CMD_FONT: cmd_font(); break;
    case WS_CMD_FONTS_CHANGED: ui_rebuild_fonts(); break;
    case WS_CMD_COLORS_LOAD: cmd_colors(false); break;
    case WS_CMD_COLORS_SAVE: cmd_colors(true); break;
    case WS_CMD_ABOUT:
        ui_message("About WSplit",
                   "WSplit " WS_VERSION " for Linux\n\n"
                   "Speedrun timer originally by Wodanaz (SDA),\n"
                   "updated by Nitrofski and contributors.\n"
                   "Native port of the open-source C# version.\n\n"
                   "Clock font: Digital-7 Mono by Style-7\n(freeware for personal use).");
        break;
    case WS_CMD_EXIT: cmd_exit(); break;
    default: break;
    }
}

/* ================================================================== */
/* Mouse: move / resize like the original borderless form              */
/* ================================================================== */

enum { DRAG_NONE, DRAG_MOVE, DRAG_CLOCK, DRAG_DETAIL_SEGS, DRAG_WIDE_SEGS, DRAG_WIDE_SEG_WIDTH };
#define EDGE_L 1
#define EDGE_R 2
#define EDGE_T 4
#define EDGE_B 8

static struct {
    int mode, edges;
    int start_rx, start_ry, start_wx, start_wy, start_w, start_h;
    int ref; /* for seg-count drags */
    int start_val;
} drag;

static Cursor cursors[8];
static int cur_cursor = -1;

static void set_cursor(int which)
{
    if (which == cur_cursor) return;
    cur_cursor = which;
    static const unsigned shapes[] = {XC_left_ptr, XC_sb_h_double_arrow, XC_sb_v_double_arrow,
                                      XC_bottom_right_corner, XC_bottom_left_corner, XC_top_right_corner,
                                      XC_top_left_corner, XC_fleur};
    if (!cursors[which]) cursors[which] = XCreateFontCursor(ui.dpy, shapes[which]);
    XDefineCursor(ui.dpy, ui.win, cursors[which]);
}

static int hit_test(int x, int y, int *edges)
{
    WsApp *a = &ui.app;
    WsRect c = a->clock;
    int cl = c.x, cr = c.x + c.w, ct = c.y, cb = c.y + c.h;
    bool wide = a->cur_mode == WS_MODE_WIDE;
    *edges = 0;
    if (wide && abs(x - (cr + a->wide_seg_w - 3)) < 4) return DRAG_WIDE_SEG_WIDTH;
    if (wide && abs(x - (a->win_w - 117)) < 4) return DRAG_WIDE_SEGS;
    if (a->cur_mode == WS_MODE_DETAILED && abs(y - (ct - 1)) < 4) return DRAG_DETAIL_SEGS;
    if (x >= cr - 5 && x <= cr) {
        *edges = EDGE_R;
        if (!wide && y >= cb - 5 && y <= cb) *edges |= EDGE_B;
        else if (!wide && y <= ct + 5 && y >= ct) *edges |= EDGE_T;
        return DRAG_CLOCK;
    }
    if (x <= cl + 5 && x >= cl) {
        *edges = EDGE_L;
        if (!wide && y >= cb - 5 && y <= cb) *edges |= EDGE_B;
        else if (!wide && y <= ct + 5 && y >= ct) *edges |= EDGE_T;
        return DRAG_CLOCK;
    }
    if (!wide && y >= cb - 5 && y <= cb) { *edges = EDGE_B; return DRAG_CLOCK; }
    if (!wide && y <= ct + 5 && y >= ct) { *edges = EDGE_T; return DRAG_CLOCK; }
    return DRAG_MOVE;
}

static int cursor_for(int mode, int edges)
{
    switch (mode) {
    case DRAG_WIDE_SEG_WIDTH: case DRAG_WIDE_SEGS: return 1;
    case DRAG_DETAIL_SEGS: return 2;
    case DRAG_CLOCK:
        if (edges == (EDGE_R | EDGE_B) || edges == (EDGE_L | EDGE_T)) return 3;
        if (edges == (EDGE_L | EDGE_B) || edges == (EDGE_R | EDGE_T)) return 4;
        if (edges & (EDGE_L | EDGE_R)) return 1;
        return 2;
    default: return 0;
    }
}

static void window_pos(int *x, int *y)
{
    Window child;
    XTranslateCoordinates(ui.dpy, ui.win, ui.root, 0, 0, x, y, &child);
}

static void on_button_press(XButtonEvent *be)
{
    if (be->button == Button3) {
        WsMenu *m = ws_menu_build(&ui.app, WS_MENU_PLATFORM_LINUX);
        int id = ui_popup_menu(m, be->x_root, be->y_root);
        ws_menu_free(m);
        if (id > 0) {
            int r = ws_menu_activate(&ui.app, id);
            if (r != WS_CMD_NONE) run_command(r);
        }
        ui.need_redraw = true;
        return;
    }
    if (be->button == Button4 || be->button == Button5) return;
    if (be->button != Button1) return;
    int edges;
    drag.mode = hit_test(be->x, be->y, &edges);
    drag.edges = edges;
    drag.start_rx = be->x_root;
    drag.start_ry = be->y_root;
    window_pos(&drag.start_wx, &drag.start_wy);
    drag.start_w = ui.app.win_w;
    drag.start_h = ui.app.win_h;
    drag.ref = drag.mode == DRAG_DETAIL_SEGS ? be->y : be->x;
    drag.start_val = ui.app.settings.wide_seg_width;
}

static void on_motion(XMotionEvent *me)
{
    WsApp *a = &ui.app;
    if (drag.mode == DRAG_NONE) {
        int edges, m = hit_test(me->x, me->y, &edges);
        set_cursor(cursor_for(m, edges));
        return;
    }
    int dx = me->x_root - drag.start_rx, dy = me->y_root - drag.start_ry;
    switch (drag.mode) {
    case DRAG_MOVE:
        XMoveWindow(ui.dpy, ui.win, drag.start_wx + dx, drag.start_wy + dy);
        break;
    case DRAG_CLOCK: {
        int w = drag.start_w, h = drag.start_h;
        if (drag.edges & EDGE_R) w += dx;
        if (drag.edges & EDGE_L) w -= dx;
        if (drag.edges & EDGE_B) h += dy;
        if (drag.edges & EDGE_T) h -= dy;
        ws_app_user_resized(a, w, h);
        if (drag.edges & (EDGE_L | EDGE_T)) {
            int nx = drag.start_wx, ny = drag.start_wy;
            if (drag.edges & EDGE_L) nx = drag.start_wx + drag.start_w - a->win_w;
            if (drag.edges & EDGE_T) ny = drag.start_wy + drag.start_h - a->win_h;
            XMoveWindow(ui.dpy, ui.win, nx, ny);
        }
        break;
    }
    case DRAG_DETAIL_SEGS: {
        int y = me->y, sh = a->seg_height;
        if (y >= drag.ref + sh) {
            int before = a->settings.display_segs;
            ws_app_segs_inc(a);
            if (a->settings.display_segs != before) drag.ref += sh;
        } else if (y <= drag.ref - sh && a->settings.display_segs > 2) {
            ws_app_segs_dec(a);
            drag.ref -= sh;
        }
        break;
    }
    case DRAG_WIDE_SEGS: {
        int x = me->x, sw = a->wide_seg_w;
        if (x >= drag.ref + sw) {
            int before = a->settings.wide_segs;
            ws_app_segs_inc(a);
            if (a->settings.wide_segs != before) drag.ref += sw;
        } else if (x <= drag.ref - sw && a->settings.wide_segs > 1) {
            ws_app_segs_dec(a);
            drag.ref -= sw;
        }
        break;
    }
    case DRAG_WIDE_SEG_WIDTH: {
        int v = drag.start_val + dx;
        a->settings.wide_seg_width = v < 60 ? 60 : v;
        ws_app_relayout(a);
        break;
    }
    }
    ui.need_redraw = true;
}

/* ================================================================== */
/* Event handling                                                      */
/* ================================================================== */

static void on_key(XKeyEvent *ke, bool press)
{
    if (ke->keycode < 256) {
        if (!press) { ui.keydown[ke->keycode] = false; return; }
        if (ui.keydown[ke->keycode]) return; /* auto-repeat */
        ui.keydown[ke->keycode] = true;
    }
    if (ke->window == ui.root) { handle_hotkey_event(ke); return; }
    if (handle_hotkey_event(ke)) return;
    KeySym ks = XLookupKeysym(ke, 0);
    char name[64];
    if (!ui_keysym_name(ks, name, sizeof name)) return;
    if (ke->state & (ControlMask | Mod1Mask | Mod4Mask)) {
        /* convenience shortcuts */
        if ((ke->state & ControlMask) && !strcmp(name, "O")) run_command(WS_CMD_OPEN);
        else if ((ke->state & ControlMask) && !strcmp(name, "S")) ws_app_save(&ui.app);
        else if ((ke->state & ControlMask) && !strcmp(name, "Q")) cmd_exit();
        return;
    }
    if (ws_app_local_key(&ui.app, name)) ui.need_redraw = true;
}

/* Events for the main window that must be handled even in modal loops. */
void ui_pump_background_event(XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.window == ui.win && ev->xexpose.count == 0) ui.need_redraw = true;
        break;
    case ConfigureNotify:
        if (ev->xconfigure.window == ui.win) ui.need_redraw = true;
        break;
    case MapNotify:
        if (ev->xmap.window == ui.win) { ui.mapped = true; ui.need_redraw = true; }
        break;
    case KeyRelease:
        if (ev->xkey.keycode < 256) ui.keydown[ev->xkey.keycode] = false;
        break;
    default:
        break;
    }
}

static void check_editor_watch(void)
{
    if (!ui.watch_path[0]) return;
    WsApp *a = &ui.app;
    if (!a->run.file || strcmp(a->run.file, ui.watch_path)) { ui.watch_path[0] = 0; return; }
    long long m = file_mtime(ui.watch_path);
    if (m != ui.watch_mtime && m > 0) { ui.watch_mtime = m; ui.watch_pending = true; }
    if (ui.watch_pending && !ws_app_started(a) && !a->delay_active) {
        ui.watch_pending = false;
        char p[WS_PATH_MAX];
        snprintf(p, sizeof p, "%s", ui.watch_path);
        ws_app_load_file(a, p);
        ui.need_redraw = true;
    }
}

void ui_tick_background(void)
{
    if (ws_app_tick(&ui.app)) ui.need_redraw = true;
    static long long last_watch;
    long long now = ws_now_ns();
    if (now - last_watch > 500000000LL) { last_watch = now; check_editor_watch(); }
    if (ui.need_redraw) ui_draw_main();
}

int ui_next_timeout_ms(void)
{
    WsApp *a = &ui.app;
    if (a->refreshing || a->delay_active) return a->settings.refresh_ms;
    return ui.watch_path[0] ? 500 : -1;
}

static void handle_event(XEvent *ev)
{
    if (XFilterEvent(ev, None)) return;
    switch (ev->type) {
    case ButtonPress:
        if (ev->xbutton.window == ui.win) on_button_press(&ev->xbutton);
        break;
    case ButtonRelease:
        drag.mode = DRAG_NONE;
        break;
    case MotionNotify:
        if (ev->xmotion.window == ui.win) {
            while (XCheckTypedWindowEvent(ui.dpy, ui.win, MotionNotify, ev)) {}
            on_motion(&ev->xmotion);
        }
        break;
    case LeaveNotify:
        if (drag.mode == DRAG_NONE) set_cursor(0);
        break;
    case KeyPress:
        on_key(&ev->xkey, true);
        break;
    case KeyRelease:
        on_key(&ev->xkey, false);
        break;
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == ui.a_wm_delete) cmd_exit();
        break;
    case MappingNotify:
        XRefreshKeyboardMapping(&ev->xmapping);
        h_hotkeys_changed(NULL);
        break;
    default:
        ui_pump_background_event(ev);
        break;
    }
}

/* ================================================================== */
/* Setup                                                               */
/* ================================================================== */

static bool dir_has_font(const char *dir)
{
    char p[WS_PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/digital-7-mono.ttf", dir);
    return access(p, R_OK) == 0;
}

static void find_asset_dir(void)
{
    const char *env = getenv("WSPLIT_DATA");
    if (env && dir_has_font(env)) { snprintf(ui.asset_dir, sizeof ui.asset_dir, "%s", env); return; }
    char exe[WS_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *s = strrchr(exe, '/');
        if (s) *s = 0;
        const char *rel[] = {"/assets", "/../assets", "/../share/wsplit", "/../../assets"};
        for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
            char c[WS_PATH_MAX + 32];
            snprintf(c, sizeof c, "%s%s", exe, rel[i]);
            if (dir_has_font(c)) { snprintf(ui.asset_dir, sizeof ui.asset_dir, "%s", c); return; }
        }
    }
    const char *sys[] = {"/usr/local/share/wsplit", "/usr/share/wsplit"};
    for (size_t i = 0; i < 2; i++)
        if (dir_has_font(sys[i])) { snprintf(ui.asset_dir, sizeof ui.asset_dir, "%s", sys[i]); return; }
    snprintf(ui.asset_dir, sizeof ui.asset_dir, "assets");
}

static void mkdir_p(const char *path)
{
    char tmp[WS_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void find_config_dir(void)
{
    const char *x = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (x && *x) snprintf(ui.config_dir, sizeof ui.config_dir, "%s/wsplit", x);
    else snprintf(ui.config_dir, sizeof ui.config_dir, "%s/.config/wsplit", home ? home : "/tmp");
    mkdir_p(ui.config_dir);
}

static void set_window_icon(void)
{
    static const char *sizes[] = {"016", "032", "048", "128"};
    unsigned long *data = NULL;
    size_t len = 0;
    for (size_t i = 0; i < 4; i++) {
        char p[WS_PATH_MAX + 64];
        snprintf(p, sizeof p, "%s/icons/icon_%s.png", ui.asset_dir, sizes[i]);
        cairo_surface_t *s = cairo_image_surface_create_from_png(p);
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); continue; }
        cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cairo_image_surface_get_width(s),
                                                          cairo_image_surface_get_height(s));
        cairo_t *cr = cairo_create(img);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_flush(img);
        int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
        int stride = cairo_image_surface_get_stride(img);
        unsigned long *nd = realloc(data, (len + 2 + (size_t)w * h) * sizeof *nd);
        if (!nd) { cairo_surface_destroy(img); cairo_surface_destroy(s); break; }
        data = nd;
        data[len++] = (unsigned long)w;
        data[len++] = (unsigned long)h;
        unsigned char *px = cairo_image_surface_get_data(img);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) data[len++] = ((uint32_t *)(px + y * stride))[x];
        cairo_surface_destroy(img);
        cairo_surface_destroy(s);
    }
    if (data) {
        XChangeProperty(ui.dpy, ui.win, ui.a_net_wm_icon, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)data, (int)len);
        free(data);
    }
}

static void usage(void)
{
    fprintf(stderr,
            "Usage: wsplit [options] [file.wsplit]\n"
            "  --screenshot OUT.png  render one frame and exit (no window)\n"
            "  --mode N              display mode for --screenshot (0-3)\n"
            "  --simulate N          for --screenshot: pretend N splits were done\n");
}

static int screenshot_mode(const char *out, int mode, int sim)
{
    WsApp *a = &ui.app;
    if (mode >= 0) ws_app_set_display(a, (WsDisplayMode)mode);
    if (sim > 0 && ws_run_live(&a->run)) {
        /* deterministic fake run: run each segment slightly differently than PB */
        a->run.attempts++;
        double t = 0;
        for (int i = 0; i < sim && i < a->run.count; i++) {
            double best = a->run.segs[i].best_time - (i ? a->run.segs[i - 1].best_time : 0);
            if (best <= 0) best = 60;
            t += best + ((i % 3 == 1) ? 2.37 : -1.12);
            ws_run_do_split(&a->run, trunc(t * 100) / 100);
        }
        double prev = a->run.live_index > 0 ? a->run.segs[a->run.live_index - 1].live_time : 0;
        double extra = 20.0;
        if (!ws_run_done(&a->run)) {
            double comp = ws_run_comp_time(&a->run);
            if (comp > prev) extra = (comp - prev) * 0.6;
        }
        a->timer.accumulated_ns = (int64_t)((ws_run_done(&a->run) ? prev : prev + extra) * 1e9);
        a->timer.running = false;
        a->refreshing = false;
        /* fake "running" for colours: start it and rewind the start point */
        a->timer.start_ns = ws_now_ns();
        a->timer.running = true;
        ws_app_update_display(a);
    }
    bool ok = render_png(out);
    fprintf(stderr, "%s %s (%dx%d)\n", ok ? "wrote" : "failed", out, a->win_w, a->win_h);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    const char *shot = NULL, *file = NULL;
    int shot_mode = -1, sim = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) shot_mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--simulate") && i + 1 < argc) sim = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else file = argv[i];
    }

    find_asset_dir();
    find_config_dir();

    /* decimal separator from the locale, like CultureInfo.CurrentCulture */
    struct lconv *lc = localeconv();
    char dec[8];
    snprintf(dec, sizeof dec, "%s", (lc && lc->decimal_point && *lc->decimal_point) ? lc->decimal_point : ".");
    setlocale(LC_NUMERIC, "C");

    WsHost host = {0};
    host.ask = h_ask;
    host.message = h_message;
    host.choose_save_path = h_choose_save_path;
    host.resize_window = h_resize_window;
    host.invalidate = h_invalidate;
    host.hotkeys_changed = h_hotkeys_changed;
    host.load_icon = h_load_icon;
    host.free_icon = h_free_icon;
    host.apply_window_props = h_apply_window_props;

    char settings_path[WS_PATH_MAX + 32];
    snprintf(settings_path, sizeof settings_path, "%s/settings.ini", ui.config_dir);

    if (shot) {
        /* offscreen rendering, no X connection required */
        ws_app_init(&ui.app, &host, NULL);
        snprintf(ui.app.decimal_char, sizeof ui.app.decimal_char, "%s", dec);
        ui_rebuild_fonts();
        if (file && !ws_app_load_file(&ui.app, file)) { fprintf(stderr, "could not load %s\n", file); return 1; }
        return screenshot_mode(shot, shot_mode, sim);
    }

    XInitThreads();
    ui.dpy = XOpenDisplay(NULL);
    if (!ui.dpy) {
        fprintf(stderr, "wsplit: cannot open X display (on Wayland, make sure XWayland is available)\n");
        return 1;
    }
    ui.screen = DefaultScreen(ui.dpy);
    ui.root = RootWindow(ui.dpy, ui.screen);
    ui.visual = DefaultVisual(ui.dpy, ui.screen);
    ui.depth = DefaultDepth(ui.dpy, ui.screen);
    ui.cmap = DefaultColormap(ui.dpy, ui.screen);

    ui.a_wm_delete = XInternAtom(ui.dpy, "WM_DELETE_WINDOW", False);
    ui.a_wm_protocols = XInternAtom(ui.dpy, "WM_PROTOCOLS", False);
    ui.a_motif_hints = XInternAtom(ui.dpy, "_MOTIF_WM_HINTS", False);
    ui.a_net_wm_state = XInternAtom(ui.dpy, "_NET_WM_STATE", False);
    ui.a_net_wm_state_above = XInternAtom(ui.dpy, "_NET_WM_STATE_ABOVE", False);
    ui.a_net_wm_opacity = XInternAtom(ui.dpy, "_NET_WM_WINDOW_OPACITY", False);
    ui.a_net_wm_icon = XInternAtom(ui.dpy, "_NET_WM_ICON", False);
    ui.a_net_wm_name = XInternAtom(ui.dpy, "_NET_WM_NAME", False);
    ui.a_utf8 = XInternAtom(ui.dpy, "UTF8_STRING", False);
    ui.a_net_wm_window_type = XInternAtom(ui.dpy, "_NET_WM_WINDOW_TYPE", False);
    ui.a_net_wm_type_dialog = XInternAtom(ui.dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    ui.a_net_wm_type_menu = XInternAtom(ui.dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    ui.a_net_wm_type_popup = XInternAtom(ui.dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);

    XkbSetDetectableAutoRepeat(ui.dpy, True, NULL);
    ui.xim = XOpenIM(ui.dpy, NULL, NULL, NULL);

    ws_app_init(&ui.app, &host, settings_path);
    snprintf(ui.app.decimal_char, sizeof ui.app.decimal_char, "%s", dec);

    XSetWindowAttributes swa = {0};
    swa.background_pixel = BlackPixel(ui.dpy, ui.screen);
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | LeaveWindowMask | FocusChangeMask;
    int w = ui.app.win_w > 0 ? ui.app.win_w : 240, h = ui.app.win_h > 0 ? ui.app.win_h : 50;
    int x = ui.app.settings.save_window_pos ? ui.app.settings.window_x : 20;
    int y = ui.app.settings.save_window_pos ? ui.app.settings.window_y : 20;
    ui.win = XCreateWindow(ui.dpy, ui.root, x, y, (unsigned)w, (unsigned)h, 0, ui.depth, InputOutput, ui.visual,
                           CWBackPixel | CWEventMask, &swa);

    /* no decorations, like FormBorderStyle.None */
    struct { unsigned long flags, functions, decorations; long input_mode; unsigned long status; } mh = {2, 0, 0, 0, 0};
    XChangeProperty(ui.dpy, ui.win, ui.a_motif_hints, ui.a_motif_hints, 32, PropModeReplace, (unsigned char *)&mh, 5);
    XSetWMProtocols(ui.dpy, ui.win, &ui.a_wm_delete, 1);
    XStoreName(ui.dpy, ui.win, "WSplit");
    XChangeProperty(ui.dpy, ui.win, ui.a_net_wm_name, ui.a_utf8, 8, PropModeReplace, (const unsigned char *)"WSplit", 6);
    XClassHint ch = {"wsplit", "WSplit"};
    XSetClassHint(ui.dpy, ui.win, &ch);
    XSizeHints *sh = XAllocSizeHints();
    sh->flags = PMinSize | PMaxSize | (ui.app.settings.save_window_pos ? (USPosition | PPosition) : 0);
    sh->min_width = sh->max_width = w;
    sh->min_height = sh->max_height = h;
    sh->x = x;
    sh->y = y;
    XSetWMNormalHints(ui.dpy, ui.win, sh);
    XFree(sh);
    XWMHints *wmh = XAllocWMHints();
    wmh->flags = InputHint;
    wmh->input = True;
    XSetWMHints(ui.dpy, ui.win, wmh);
    XFree(wmh);
    set_window_icon();
    if (ui.xim)
        ui.xic = XCreateIC(ui.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, ui.win,
                           XNFocusWindow, ui.win, NULL);

    ui_rebuild_fonts();
    h_apply_window_props(NULL);

    /* load a run: command line first, then the last file */
    if (file) ws_app_load_file(&ui.app, file);
    else if (ui.app.settings.load_most_recent && ui.app.settings.last_file[0]) ws_app_load_file(&ui.app, ui.app.settings.last_file);

    XMapWindow(ui.dpy, ui.win);
    XMoveWindow(ui.dpy, ui.win, x, y);
    h_hotkeys_changed(NULL);

    int xfd = ConnectionNumber(ui.dpy);
    while (!ui.quit) {
        while (XPending(ui.dpy) && !ui.quit) {
            XEvent ev;
            XNextEvent(ui.dpy, &ev);
            handle_event(&ev);
        }
        if (ui.quit) break;
        ui_tick_background();
        int timeout = ui.need_redraw ? 0 : ui_next_timeout_ms();
        struct pollfd pfd = {xfd, POLLIN, 0};
        if (timeout != 0) poll(&pfd, 1, timeout);
    }

    if (ui.app.settings.save_window_pos) window_pos(&ui.app.settings.window_x, &ui.app.settings.window_y);
    ungrab_hotkeys();
    ws_app_shutdown(&ui.app);
    ws_app_free(&ui.app);
    if (ui.surf) cairo_surface_destroy(ui.surf);
    XDestroyWindow(ui.dpy, ui.win);
    XCloseDisplay(ui.dpy);
    return 0;
}
