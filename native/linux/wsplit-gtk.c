/* WSplit for Linux: GTK 4 front-end (Wayland-native).
 *
 * The window is an undecorated, non-resizable GtkWindow holding one
 * GtkDrawingArea that the core renders into with cairo. The core owns the
 * window size; host resize requests set the drawing area's content size.
 * Global hotkeys go through the xdg-desktop-portal GlobalShortcuts API. */
#define _GNU_SOURCE
#include "../core/wsplit.h"

#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <gtk/gtk.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define APP_ID "io.github.lambdan.WSplit"

static struct {
    WsApp app;
    GtkApplication *gapp;
    GtkWindow *win;
    GtkWidget *area;
    bool app_ready;

    cairo_font_face_t *face[WS_FONT_COUNT];
    double fsize[WS_FONT_COUNT];
    cairo_font_face_t *ui_bold;

    char asset_dir[WS_PATH_MAX];
    char config_dir[WS_PATH_MAX];

    guint tick_id;
    int tick_ms;

    bool keydown[512];
} ui;

static FT_Library ft_lib;
static FT_Face digit_ft;

static void run_command(int id);
static void hotkeys_rebind(void);

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

static void add_stop(cairo_pattern_t *pat, double off, WsColor c)
{
    cairo_pattern_add_color_stop_rgba(pat, off, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
                                      (c & 0xFF) / 255.0, ((c >> 24) & 0xFF) / 255.0);
}

static void p_fill_gradient(void *ctx, double x, double y, double w, double h, WsColor c1, WsColor c2, bool horiz)
{
    cairo_t *cr = ((PaintCtx *)ctx)->cr;
    cairo_pattern_t *pat = horiz ? cairo_pattern_create_linear(x, 0, x + w, 0)
                                 : cairo_pattern_create_linear(0, y + h, 0, y);
    add_stop(pat, 0, c1);
    add_stop(pat, 1, c2);
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

/* Icons: any format gdk-pixbuf can read (PNG, JPEG, BMP, GIF, ...). Without
   a display (--screenshot) GdkTexture still works, it's just pixel data. */
static cairo_surface_t *surface_from_texture(GdkTexture *tex)
{
    int w = gdk_texture_get_width(tex), h = gdk_texture_get_height(tex);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(s);
    /* GdkTexture's default download format is premultiplied BGRA/ARGB32 in
       native endianness, which is exactly CAIRO_FORMAT_ARGB32. */
    gdk_texture_download(tex, cairo_image_surface_get_data(s), (gsize)cairo_image_surface_get_stride(s));
    cairo_surface_mark_dirty(s);
    return s;
}

static void *h_load_icon(void *ud, const char *path)
{
    (void)ud;
    GError *err = NULL;
    GdkTexture *tex = gdk_texture_new_from_filename(path, &err);
    if (!tex) { g_clear_error(&err); return NULL; }
    cairo_surface_t *s = surface_from_texture(tex);
    g_object_unref(tex);
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

/* ================================================================== */
/* Fonts                                                               */
/* ================================================================== */

static void rebuild_fonts(void)
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

    if (!ui.ui_bold) ui.ui_bold = cairo_toy_font_face_create("Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    if (ui.area) gtk_widget_queue_draw(ui.area);
}

/* ================================================================== */
/* Drawing                                                             */
/* ================================================================== */

static void draw_func(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    (void)area; (void)w; (void)h; (void)ud;
    PaintCtx pc = {cr};
    ws_render(&ui.app, painter_for(&pc));
}

static void redraw(void)
{
    if (ui.area) gtk_widget_queue_draw(ui.area);
}

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

static const struct { guint kv; const char *name; } key_names[] = {
    {GDK_KEY_space, "Space"}, {GDK_KEY_Return, "Enter"}, {GDK_KEY_Tab, "Tab"}, {GDK_KEY_ISO_Left_Tab, "Tab"},
    {GDK_KEY_Escape, "Escape"}, {GDK_KEY_BackSpace, "Backspace"}, {GDK_KEY_Delete, "Delete"},
    {GDK_KEY_Insert, "Insert"}, {GDK_KEY_Home, "Home"}, {GDK_KEY_End, "End"}, {GDK_KEY_Page_Up, "PageUp"},
    {GDK_KEY_Page_Down, "PageDown"}, {GDK_KEY_Left, "Left"}, {GDK_KEY_Right, "Right"}, {GDK_KEY_Up, "Up"},
    {GDK_KEY_Down, "Down"}, {GDK_KEY_Pause, "Pause"}, {GDK_KEY_Print, "PrintScreen"},
    {GDK_KEY_Scroll_Lock, "ScrollLock"}, {GDK_KEY_minus, "Minus"}, {GDK_KEY_equal, "Equals"},
    {GDK_KEY_bracketleft, "LeftBracket"}, {GDK_KEY_bracketright, "RightBracket"},
    {GDK_KEY_semicolon, "Semicolon"}, {GDK_KEY_apostrophe, "Quote"}, {GDK_KEY_comma, "Comma"},
    {GDK_KEY_period, "Period"}, {GDK_KEY_slash, "Slash"}, {GDK_KEY_backslash, "Backslash"},
    {GDK_KEY_grave, "Grave"}, {GDK_KEY_KP_Add, "NumpadAdd"}, {GDK_KEY_KP_Subtract, "NumpadSubtract"},
    {GDK_KEY_KP_Multiply, "NumpadMultiply"}, {GDK_KEY_KP_Divide, "NumpadDivide"},
    {GDK_KEY_KP_Decimal, "NumpadDecimal"}, {GDK_KEY_KP_Enter, "NumpadEnter"},
    /* keypad without NumLock */
    {GDK_KEY_KP_Insert, "Numpad0"}, {GDK_KEY_KP_End, "Numpad1"}, {GDK_KEY_KP_Down, "Numpad2"},
    {GDK_KEY_KP_Page_Down, "Numpad3"}, {GDK_KEY_KP_Left, "Numpad4"}, {GDK_KEY_KP_Begin, "Numpad5"},
    {GDK_KEY_KP_Right, "Numpad6"}, {GDK_KEY_KP_Home, "Numpad7"}, {GDK_KEY_KP_Up, "Numpad8"},
    {GDK_KEY_KP_Page_Up, "Numpad9"}, {GDK_KEY_KP_Delete, "NumpadDecimal"},
};

static bool keyval_name(guint kv, char *out, size_t n)
{
    guint up = gdk_keyval_to_upper(kv);
    if (up >= GDK_KEY_A && up <= GDK_KEY_Z) { snprintf(out, n, "%c", (int)('A' + (up - GDK_KEY_A))); return true; }
    if (kv >= GDK_KEY_0 && kv <= GDK_KEY_9) { snprintf(out, n, "%c", (int)('0' + (kv - GDK_KEY_0))); return true; }
    if (kv >= GDK_KEY_F1 && kv <= GDK_KEY_F35) { snprintf(out, n, "F%d", (int)(kv - GDK_KEY_F1 + 1)); return true; }
    if (kv >= GDK_KEY_KP_0 && kv <= GDK_KEY_KP_9) { snprintf(out, n, "Numpad%d", (int)(kv - GDK_KEY_KP_0)); return true; }
    for (size_t i = 0; i < G_N_ELEMENTS(key_names); i++)
        if (key_names[i].kv == kv) { snprintf(out, n, "%s", key_names[i].name); return true; }
    const char *s = gdk_keyval_name(kv);
    if (!s) return false;
    snprintf(out, n, "%s", s);
    return true;
}

/* Hotkey key name -> keyval. Prefers the NumLock-on keypad keyvals. */
static guint name_keyval(const char *name)
{
    if (!name || !*name) return 0;
    if (!name[1] && name[0] >= 'A' && name[0] <= 'Z') return GDK_KEY_a + (guint)(name[0] - 'A');
    if (!name[1] && name[0] >= 'a' && name[0] <= 'z') return GDK_KEY_a + (guint)(name[0] - 'a');
    if (!name[1] && name[0] >= '0' && name[0] <= '9') return GDK_KEY_0 + (guint)(name[0] - '0');
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '0' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 35) return GDK_KEY_F1 + (guint)(n - 1);
    }
    if (!strncmp(name, "Numpad", 6) && name[6] >= '0' && name[6] <= '9' && !name[7]) return GDK_KEY_KP_0 + (guint)(name[6] - '0');
    for (size_t i = 0; i < G_N_ELEMENTS(key_names); i++)
        if (!strcasecmp(key_names[i].name, name)) return key_names[i].kv;
    guint kv = gdk_keyval_from_name(name);
    return kv == GDK_KEY_VoidSymbol ? 0 : kv;
}

static unsigned gdk_mods_to_ws(GdkModifierType st)
{
    unsigned m = 0;
    if (st & GDK_CONTROL_MASK) m |= WS_MOD_CTRL;
    if (st & GDK_ALT_MASK) m |= WS_MOD_ALT;
    if (st & GDK_SHIFT_MASK) m |= WS_MOD_SHIFT;
    if (st & GDK_SUPER_MASK) m |= WS_MOD_SUPER;
    return m;
}

static bool is_modifier_key(guint kv)
{
    switch (kv) {
    case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: case GDK_KEY_Control_L: case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L: case GDK_KEY_Alt_R: case GDK_KEY_Meta_L: case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L: case GDK_KEY_Super_R: case GDK_KEY_Hyper_L: case GDK_KEY_Hyper_R:
    case GDK_KEY_ISO_Level3_Shift: case GDK_KEY_Caps_Lock: case GDK_KEY_Num_Lock:
        return true;
    default:
        return false;
    }
}

/* The keyval with no modifiers applied (so Shift+1 is "1", not "exclam"). */
static guint base_keyval(GtkEventController *c, guint keyval, guint keycode)
{
    GdkEvent *ev = gtk_event_controller_get_current_event(c);
    if (!ev || gdk_event_get_event_type(ev) != GDK_KEY_PRESS) return keyval;
    GdkDisplay *d = gdk_event_get_display(ev);
    guint kv = 0;
    if (d && gdk_display_translate_key(d, keycode, 0, (int)gdk_key_event_get_layout(ev), &kv, NULL, NULL, NULL) && kv)
        return kv;
    return keyval;
}

/* Wayland/X11 event times are CLOCK_MONOTONIC milliseconds on practically
   every compositor. Turn one into "how long ago"; 0 if it doesn't look sane. */
static int64_t event_latency_ns(guint32 t)
{
    if (!t) return 0;
    uint32_t now_ms = (uint32_t)(ws_now_ns() / 1000000);
    int32_t d = (int32_t)(now_ms - t);
    if (getenv("WSPLIT_DEBUG_LATENCY")) fprintf(stderr, "event latency: %d ms\n", d);
    return (d >= 0 && d < 1000) ? (int64_t)d * 1000000 : 0;
}

static void cmd_exit(void);

static gboolean on_key_pressed(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType st, gpointer ud)
{
    (void)ud;
    GtkEventController *c = GTK_EVENT_CONTROLLER(k);
    if (keycode < G_N_ELEMENTS(ui.keydown)) {
        if (ui.keydown[keycode]) return TRUE; /* auto-repeat */
        ui.keydown[keycode] = true;
    }
    char name[64];
    if (!keyval_name(base_keyval(c, keyval, keycode), name, sizeof name)) return FALSE;
    if (st & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) {
        if (!(st & GDK_CONTROL_MASK)) return FALSE;
        if (!strcmp(name, "O")) run_command(WS_CMD_OPEN);
        else if (!strcmp(name, "S")) ws_app_save(&ui.app);
        else if (!strcmp(name, "E")) run_command(WS_CMD_EDIT_RUN);
        else if (!strcmp(name, "Q")) cmd_exit();
        else return FALSE;
        redraw();
        return TRUE;
    }
    ws_app_set_input_latency(&ui.app, event_latency_ns(gtk_event_controller_get_current_event_time(c)));
    bool handled = ws_app_local_key(&ui.app, name);
    redraw();
    return handled;
}

static void on_key_released(GtkEventControllerKey *k, guint keyval, guint keycode, GdkModifierType st, gpointer ud)
{
    (void)k; (void)keyval; (void)st; (void)ud;
    if (keycode < G_N_ELEMENTS(ui.keydown)) ui.keydown[keycode] = false;
}

static void on_active_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    (void)o; (void)ps; (void)ud;
    /* releases are lost while unfocused */
    memset(ui.keydown, 0, sizeof ui.keydown);
}

/* ================================================================== */
/* Synchronous dialogs                                                 */
/* ================================================================== */
/* The core expects ask() etc. to return an answer, so these spin a nested
   main loop until the async GTK dialog finishes. The refresh timeout keeps
   firing inside the loop, so the clock never stops. */

typedef struct {
    bool done;
    GAsyncResult *res;
} Wait;

static void wait_cb(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)src;
    Wait *w = ud;
    w->res = g_object_ref(res);
    w->done = true;
}

static void wait_run(Wait *w)
{
    bool was = ui.app.modal_open;
    ui.app.modal_open = true;
    while (!w->done) g_main_context_iteration(NULL, TRUE);
    ui.app.modal_open = was;
}

static int alert(const char *title, const char *msg, const char *const *buttons, int def, int cancel)
{
    GtkAlertDialog *d = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(d, msg);
    gtk_alert_dialog_set_buttons(d, buttons);
    gtk_alert_dialog_set_default_button(d, def);
    gtk_alert_dialog_set_cancel_button(d, cancel);
    gtk_alert_dialog_set_modal(d, TRUE);
    Wait w = {0};
    gtk_alert_dialog_choose(d, ui.win, NULL, wait_cb, &w);
    wait_run(&w);
    int r = gtk_alert_dialog_choose_finish(d, w.res, NULL);
    g_object_unref(w.res);
    g_object_unref(d);
    return r < 0 ? cancel : r;
}

static void ui_message(const char *title, const char *msg)
{
    if (!ui.win) { fprintf(stderr, "%s: %s\n", title, msg); return; }
    static const char *const ok[] = {"OK", NULL};
    alert(title, msg, ok, 0, 0);
}

static WsAnswer ui_ask(const char *title, const char *msg, bool cancel_allowed)
{
    if (!ui.win) return WS_ANSWER_NO;
    static const char *const ync[] = {"Yes", "No", "Cancel", NULL};
    static const char *const yn[] = {"Yes", "No", NULL};
    int r = cancel_allowed ? alert(title, msg, ync, 0, 2) : alert(title, msg, yn, 0, 1);
    return r == 0 ? WS_ANSWER_YES : r == 1 ? WS_ANSWER_NO : WS_ANSWER_CANCEL;
}

/* filter: "Label|*.a *.b" */
static bool ui_choose_file(bool save, const char *title, const char *filter, const char *initial_name, char *out, size_t n)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, title);
    gtk_file_dialog_set_modal(d, TRUE);
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    if (filter) {
        const char *bar = strchr(filter, '|');
        GtkFileFilter *f = gtk_file_filter_new();
        char *label = g_strndup(filter, bar ? (gsize)(bar - filter) : strlen(filter));
        gtk_file_filter_set_name(f, label);
        g_free(label);
        if (bar) {
            char **pats = g_strsplit(bar + 1, " ", -1);
            for (char **p = pats; *p; p++)
                if (**p) gtk_file_filter_add_pattern(f, *p);
            g_strfreev(pats);
        }
        g_list_store_append(filters, f);
        g_object_unref(f);
    }
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, all);
    g_object_unref(all);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
    g_object_unref(filters);
    if (save && initial_name) gtk_file_dialog_set_initial_name(d, initial_name);
    if (ui.app.run.file) {
        GFile *cur = g_file_new_for_path(ui.app.run.file);
        GFile *dir = g_file_get_parent(cur);
        if (dir) { gtk_file_dialog_set_initial_folder(d, dir); g_object_unref(dir); }
        g_object_unref(cur);
    }

    Wait w = {0};
    if (save) gtk_file_dialog_save(d, ui.win, NULL, wait_cb, &w);
    else gtk_file_dialog_open(d, ui.win, NULL, wait_cb, &w);
    wait_run(&w);
    GFile *f = save ? gtk_file_dialog_save_finish(d, w.res, NULL) : gtk_file_dialog_open_finish(d, w.res, NULL);
    g_object_unref(w.res);
    g_object_unref(d);
    bool ok = false;
    if (f) {
        char *p = g_file_get_path(f);
        if (p) { snprintf(out, n, "%s", p); ok = true; g_free(p); }
        g_object_unref(f);
    }
    return ok;
}

/* A small modal window built by the caller; buttons call dlg_respond. */
typedef struct {
    GtkWindow *w;
    int response; /* -2 = running, -1 = closed */
} Dlg;

static void dlg_respond(Dlg *d, int r)
{
    if (d->response == -2) d->response = r;
}

static void dlg_btn_cb(GtkButton *b, gpointer ud)
{
    Dlg *d = ud;
    dlg_respond(d, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "resp")));
}

static gboolean dlg_close_cb(GtkWindow *w, gpointer ud)
{
    (void)w;
    dlg_respond(ud, -1);
    return TRUE; /* destroyed by dlg_run */
}

static gboolean dlg_escape_cb(GtkEventControllerKey *k, guint kv, guint kc, GdkModifierType st, gpointer ud)
{
    (void)k; (void)kc; (void)st;
    if (kv == GDK_KEY_Escape) { dlg_respond(ud, -1); return TRUE; }
    return FALSE;
}

static void dlg_init(Dlg *d, const char *title, int w, int h)
{
    d->response = -2;
    d->w = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(d->w, title);
    gtk_window_set_modal(d->w, TRUE);
    if (ui.win) gtk_window_set_transient_for(d->w, ui.win);
    gtk_window_set_default_size(d->w, w, h);
    g_signal_connect(d->w, "close-request", G_CALLBACK(dlg_close_cb), d);
    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(dlg_escape_cb), d);
    gtk_widget_add_controller(GTK_WIDGET(d->w), k);
}

static GtkWidget *dlg_button(Dlg *d, const char *label, int resp, bool suggested)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    g_object_set_data(G_OBJECT(b), "resp", GINT_TO_POINTER(resp));
    g_signal_connect(b, "clicked", G_CALLBACK(dlg_btn_cb), d);
    if (suggested) gtk_widget_add_css_class(b, "suggested-action");
    return b;
}

static GtkWidget *vbox(int spacing, int margin)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
    gtk_widget_set_margin_start(b, margin);
    gtk_widget_set_margin_end(b, margin);
    gtk_widget_set_margin_top(b, margin);
    gtk_widget_set_margin_bottom(b, margin);
    return b;
}

static GtkWidget *button_row(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 6);
    return row;
}

static int dlg_run(Dlg *d)
{
    gtk_window_present(d->w);
    bool was = ui.app.modal_open;
    ui.app.modal_open = true;
    while (d->response == -2) g_main_context_iteration(NULL, TRUE);
    ui.app.modal_open = was;
    return d->response;
}

static void dlg_destroy(Dlg *d)
{
    gtk_window_destroy(d->w);
    d->w = NULL;
}

static void entry_activate_cb(GtkEntry *e, gpointer ud)
{
    (void)e;
    dlg_respond(ud, 0);
}

/* Text prompt; optional check box. Returns true on OK. */
static bool ui_prompt(const char *title, const char *msg, char *buf, size_t n, const char *check_label, bool *check,
                      const char *ok_label)
{
    Dlg d;
    dlg_init(&d, title, 360, -1);
    GtkWidget *box = vbox(10, 16);
    GtkWidget *l = gtk_label_new(msg);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_box_append(GTK_BOX(box), l);
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), buf);
    g_signal_connect(e, "activate", G_CALLBACK(entry_activate_cb), &d);
    gtk_box_append(GTK_BOX(box), e);
    GtkWidget *cb = NULL;
    if (check_label) {
        cb = gtk_check_button_new_with_label(check_label);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), *check);
        gtk_box_append(GTK_BOX(box), cb);
    }
    GtkWidget *row = button_row();
    gtk_box_append(GTK_BOX(row), dlg_button(&d, "_Cancel", -1, false));
    gtk_box_append(GTK_BOX(row), dlg_button(&d, ok_label ? ok_label : "_OK", 0, true));
    gtk_box_append(GTK_BOX(box), row);
    gtk_window_set_child(d.w, box);
    gtk_widget_grab_focus(e);
    bool ok = dlg_run(&d) == 0;
    if (ok) {
        snprintf(buf, n, "%s", gtk_editable_get_text(GTK_EDITABLE(e)));
        if (cb) *check = gtk_check_button_get_active(GTK_CHECK_BUTTON(cb));
    }
    dlg_destroy(&d);
    return ok;
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

static void h_resize_window(void *ud, int w, int h)
{
    (void)ud;
    if (!ui.area || w <= 0 || h <= 0) return;
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(ui.area), w);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.area), h);
    gtk_widget_set_size_request(ui.area, w, h);
    /* a non-resizable window doesn't shrink on its own: reset its size */
    gtk_window_set_default_size(ui.win, w, h);
    redraw();
}

static void h_invalidate(void *ud) { (void)ud; redraw(); }

static void h_apply_window_props(void *ud)
{
    (void)ud;
    if (!ui.win) return;
    double op = ui.app.settings.opacity;
    gtk_widget_set_opacity(GTK_WIDGET(ui.win), op > 0.05 && op < 1.0 ? op : 1.0);
}

static void h_hotkeys_changed(void *ud)
{
    (void)ud;
    hotkeys_rebind();
    redraw();
}

/* ================================================================== */
/* Global hotkeys: xdg-desktop-portal GlobalShortcuts                  */
/* ================================================================== */

static const char *const hk_ids[WS_HK_COUNT] = {"split", "pause", "stop", "reset", "previous", "next", "compare", "toggle"};

static struct {
    GDBusConnection *bus;   /* private connection, so Registry.Register can go first */
    char sender[128];       /* unique name with ':' dropped and '.' -> '_' */
    char *session;          /* session object path, NULL if none */
    char bound_sig[WS_HK_COUNT * 64 + 16]; /* triggers we last bound */
    guint activated_sub;
    guint pending_sub;
    bool unavailable;
    bool warned;
    bool busy;              /* a CreateSession/BindShortcuts is in flight */
    bool again;             /* settings changed while busy */
    int token;
} portal;

/* ws hotkey string -> XDG shortcut trigger ("CTRL+SHIFT+F1"). */
static bool hotkey_trigger(const char *hk, char *out, size_t n)
{
    unsigned mods;
    char key[64];
    if (!hk || !*hk || !ws_hotkey_parse(hk, &mods, key, sizeof key)) return false;
    guint kv = name_keyval(key);
    const char *kn = kv ? gdk_keyval_name(gdk_keyval_to_lower(kv)) : NULL;
    if (!kn) return false;
    snprintf(out, n, "%s%s%s%s%s", (mods & WS_MOD_CTRL) ? "CTRL+" : "", (mods & WS_MOD_ALT) ? "ALT+" : "",
             (mods & WS_MOD_SHIFT) ? "SHIFT+" : "", (mods & WS_MOD_SUPER) ? "LOGO+" : "", kn);
    return true;
}

static void portal_warn(const char *why)
{
    if (portal.warned) return;
    portal.warned = true;
    char msg[600];
    snprintf(msg, sizeof msg,
             "Global hotkeys need the xdg-desktop-portal GlobalShortcuts interface "
             "(GNOME 48+, KDE Plasma 6 or Hyprland).\n\n%s\n\n"
             "Hotkeys still work while the WSplit window is focused.",
             why);
    ui_message("Global hotkeys", msg);
}

static void on_activated(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *signal, GVariant *params, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)ud;
    const char *session, *id;
    guint64 ts;
    g_variant_get(params, "(&o&st@a{sv})", &session, &id, &ts, NULL);
    if (!portal.session || strcmp(session, portal.session)) return;
    for (int i = 0; i < WS_HK_COUNT; i++) {
        if (strcmp(id, hk_ids[i])) continue;
        /* the timestamp is in ms; same monotonic base as input events on
           the implementations checked, otherwise the sanity check drops it */
        ws_app_set_input_latency(&ui.app, event_latency_ns((guint32)ts));
        ws_app_hotkey(&ui.app, (WsHotkeyAction)i);
        redraw();
        return;
    }
}

typedef void (*ResponseFn)(guint32 code, GVariant *results);
static ResponseFn pending_fn;

static void on_response(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *signal, GVariant *params, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)ud;
    guint32 code;
    GVariant *results;
    g_variant_get(params, "(u@a{sv})", &code, &results);
    g_dbus_connection_signal_unsubscribe(portal.bus, portal.pending_sub);
    portal.pending_sub = 0;
    ResponseFn fn = pending_fn;
    pending_fn = NULL;
    if (fn) fn(code, results);
    g_variant_unref(results);
}

/* Subscribe to the Request's Response before calling, then return the token. */
static char *portal_expect(ResponseFn fn)
{
    char *token = g_strdup_printf("wsplit%d", ++portal.token);
    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", portal.sender, token);
    if (portal.pending_sub) g_dbus_connection_signal_unsubscribe(portal.bus, portal.pending_sub);
    pending_fn = fn;
    portal.pending_sub = g_dbus_connection_signal_subscribe(portal.bus, "org.freedesktop.portal.Desktop",
                                                            "org.freedesktop.portal.Request", "Response", path, NULL,
                                                            G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE, on_response, NULL, NULL);
    g_free(path);
    return token;
}

static void portal_call_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    const char *what = ud;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r) { g_variant_unref(r); return; }
    if (portal.pending_sub) g_dbus_connection_signal_unsubscribe(portal.bus, portal.pending_sub);
    portal.pending_sub = 0;
    pending_fn = NULL;
    portal.busy = false;
    portal.unavailable = true;
    char why[400];
    snprintf(why, sizeof why, "%s failed: %s", what, err ? err->message : "unknown error");
    g_clear_error(&err);
    portal_warn(why);
}

static void portal_done(void)
{
    portal.busy = false;
    if (portal.again) { portal.again = false; hotkeys_rebind(); }
}

static void bind_response(guint32 code, GVariant *results)
{
    (void)results;
    if (code == 2) portal_warn("Binding the shortcuts failed.");
    /* code 1 = user cancelled the system dialog; nothing to say */
    portal_done();
}

static void build_sig(char *sig, size_t n)
{
    size_t o = 0;
    sig[0] = 0;
    for (int i = 0; i < WS_HK_COUNT; i++) {
        char t[64] = "";
        hotkey_trigger(ui.app.settings.hotkeys[i], t, sizeof t);
        o += (size_t)snprintf(sig + o, n > o ? n - o : 0, "%s;", t);
    }
}

static void portal_bind(void)
{
    GVariantBuilder sc;
    g_variant_builder_init(&sc, G_VARIANT_TYPE("a(sa{sv})"));
    for (int i = 0; i < WS_HK_COUNT; i++) {
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        char desc[96];
        snprintf(desc, sizeof desc, "WSplit: %s", ws_hotkey_labels[i]);
        g_variant_builder_add(&props, "{sv}", "description", g_variant_new_string(desc));
        char t[64];
        if (hotkey_trigger(ui.app.settings.hotkeys[i], t, sizeof t))
            g_variant_builder_add(&props, "{sv}", "preferred_trigger", g_variant_new_string(t));
        g_variant_builder_add(&sc, "(sa{sv})", hk_ids[i], &props);
    }
    char *token = portal_expect(bind_response);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
    g_free(token);
    build_sig(portal.bound_sig, sizeof portal.bound_sig);
    g_dbus_connection_call(portal.bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.GlobalShortcuts", "BindShortcuts",
                           g_variant_new("(oa(sa{sv})sa{sv})", portal.session, &sc, "", &opts), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, portal_call_done, (gpointer) "BindShortcuts");
}

static void session_response(guint32 code, GVariant *results)
{
    const char *h = NULL;
    if (code != 0 || !g_variant_lookup(results, "session_handle", "&s", &h) || !h) {
        portal_warn("Creating a shortcuts session was refused.");
        portal.unavailable = true;
        portal.busy = false;
        return;
    }
    g_free(portal.session);
    portal.session = g_strdup(h);
    portal_bind();
}

static void portal_close_session(void)
{
    if (!portal.session) return;
    g_dbus_connection_call(portal.bus, "org.freedesktop.portal.Desktop", portal.session,
                           "org.freedesktop.portal.Session", "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                           NULL, NULL);
    g_free(portal.session);
    portal.session = NULL;
}

static bool portal_connect(void)
{
    if (portal.bus) return true;
    GError *err = NULL;
    char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (addr)
        portal.bus = g_dbus_connection_new_for_address_sync(
            addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL,
            NULL, &err);
    g_free(addr);
    if (!portal.bus) {
        portal.unavailable = true;
        char why[300];
        snprintf(why, sizeof why, "No D-Bus session bus: %s", err ? err->message : "?");
        g_clear_error(&err);
        portal_warn(why);
        return false;
    }
    const char *u = g_dbus_connection_get_unique_name(portal.bus);
    size_t o = 0;
    for (const char *p = u ? u : ""; *p && o + 1 < sizeof portal.sender; p++) {
        if (*p == ':') continue;
        portal.sender[o++] = *p == '.' ? '_' : *p;
    }
    portal.sender[o] = 0;

    /* Host (non-Flatpak) apps: tell the portal who we are, so the shortcuts
       get attributed to the .desktop file. Must be the first portal call on
       this connection; fails harmlessly on older portals. */
    GVariant *r = g_dbus_connection_call_sync(portal.bus, "org.freedesktop.portal.Desktop",
                                              "/org/freedesktop/portal/desktop", "org.freedesktop.host.portal.Registry",
                                              "Register", g_variant_new("(sa{sv})", APP_ID, NULL), NULL,
                                              G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (r) g_variant_unref(r);

    portal.activated_sub = g_dbus_connection_signal_subscribe(
        portal.bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.GlobalShortcuts", "Activated",
        "/org/freedesktop/portal/desktop", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_activated, NULL, NULL);
    return true;
}

static bool any_hotkey_set(void)
{
    for (int i = 0; i < WS_HK_COUNT; i++)
        if (ui.app.settings.hotkeys[i][0]) return true;
    return false;
}

static void hotkeys_rebind(void)
{
    if (!ui.win || portal.unavailable) return;
    if (!any_hotkey_set() && !portal.session) return; /* nothing configured: don't bother the user */
    if (portal.busy) { portal.again = true; return; }
    /* The core calls this for enable/disable too, but it ignores non-toggle
       hotkeys while disabled itself, so only rebind when triggers change. */
    char sig[sizeof portal.bound_sig];
    build_sig(sig, sizeof sig);
    if (portal.session && !strcmp(sig, portal.bound_sig)) return;
    if (!portal_connect()) return;
    portal.busy = true;
    /* BindShortcuts may only be called once per session: start a new one */
    portal_close_session();
    char *token = portal_expect(session_response);
    char *stoken = g_strdup_printf("wsplit_session%d", portal.token);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&opts, "{sv}", "session_handle_token", g_variant_new_string(stoken));
    g_free(token);
    g_free(stoken);
    g_dbus_connection_call(portal.bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.GlobalShortcuts", "CreateSession", g_variant_new("(a{sv})", &opts),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, portal_call_done, (gpointer) "CreateSession");
}

/* ================================================================== */
/* Run editor                                                          */
/* ================================================================== */

typedef struct {
    Dlg d;
    GtkTextBuffer *buf;
} RunEd;

static char *buffer_text(GtkTextBuffer *b)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    return gtk_text_buffer_get_text(b, &s, &e, FALSE);
}

static void runed_autofill(GtkButton *btn, gpointer ud)
{
    (void)btn;
    RunEd *ed = ud;
    if (ui_ask("Auto-fill best segments", "Fill missing or slower best segments from the best splits?", false) != WS_ANSWER_YES)
        return;
    char *t = buffer_text(ed->buf);
    WsRun tmp;
    ws_run_init(&tmp);
    ws_run_segments_from_text(&tmp, t);
    g_free(t);
    ws_run_autofill_best_segments(&tmp);
    char *nt = ws_run_segments_to_text(&tmp);
    gtk_text_buffer_set_text(ed->buf, nt ? nt : "", -1);
    free(nt);
    ws_run_free(&tmp);
}

static void runed_icon(GtkButton *btn, gpointer ud)
{
    (void)btn;
    RunEd *ed = ud;
    char path[WS_PATH_MAX];
    if (!ui_choose_file(false, "Choose segment icon", "Images|*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff", NULL, path,
                        sizeof path))
        return;
    gtk_text_buffer_insert_at_cursor(ed->buf, path, -1);
}

static GtkWidget *grid_entry(GtkGrid *g, int row, const char *label, const char *val)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 1);
    gtk_grid_attach(g, l, 0, row, 1, 1);
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), val);
    gtk_widget_set_hexpand(e, TRUE);
    gtk_grid_attach(g, e, 1, row, 1, 1);
    return e;
}

static void cmd_edit_run(void)
{
    WsApp *a = &ui.app;
    /* WSplit updates bests before editing and restores them on cancel */
    ws_run_update_best(&a->run, a->settings.best_as_overall);
    char *file = a->run.file ? ws_strdup(a->run.file) : NULL;
    int pw = a->detail_w, ph = a->detail_h;

    RunEd ed;
    dlg_init(&ed.d, ws_run_live(&a->run) ? "Edit run" : "New run", 720, 520);
    GtkWidget *box = vbox(8, 14);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    char num[32], delay[32];
    snprintf(num, sizeof num, "%d", a->run.attempts);
    ws_format_edit_time(delay, sizeof delay, a->run.start_delay_ms / 1000.0);
    GtkWidget *e_title = grid_entry(GTK_GRID(grid), 0, "Title", a->run.title);
    GtkWidget *e_goal = grid_entry(GTK_GRID(grid), 1, "Goal", a->run.goal);
    GtkWidget *e_att = grid_entry(GTK_GRID(grid), 2, "Attempts", num);
    GtkWidget *e_delay = grid_entry(GTK_GRID(grid), 3, "Start delay", delay);
    gtk_box_append(GTK_BOX(box), grid);

    GtkWidget *hint = gtk_label_new("One segment per line:  Name | Old time | Best time | Best segment | Icon path\n"
                                    "Times are h:mm:ss.ff or seconds; leave empty for none. Lines starting with # are ignored.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(box), hint);

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 4);
    ed.buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    if (ws_run_live(&a->run)) {
        char *t = ws_run_segments_to_text(&a->run);
        gtk_text_buffer_set_text(ed.buf, t ? t : "", -1);
        free(t);
    } else {
        gtk_text_buffer_set_text(ed.buf,
                                 "# Segment | Old time | Best time | Best segment | Icon\n"
                                 "Segment 1 | | | |\nSegment 2 | | | |\nSegment 3 | | | |\n",
                                 -1);
    }
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_widget_add_css_class(sw, "frame");
    gtk_box_append(GTK_BOX(box), sw);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *b_auto = gtk_button_new_with_mnemonic("_Auto-fill best segments");
    g_signal_connect(b_auto, "clicked", G_CALLBACK(runed_autofill), &ed);
    gtk_box_append(GTK_BOX(row), b_auto);
    GtkWidget *b_icon = gtk_button_new_with_mnemonic("Insert _icon path...");
    g_signal_connect(b_icon, "clicked", G_CALLBACK(runed_icon), &ed);
    gtk_box_append(GTK_BOX(row), b_icon);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row), spacer);
    gtk_box_append(GTK_BOX(row), dlg_button(&ed.d, "_Cancel", -1, false));
    gtk_box_append(GTK_BOX(row), dlg_button(&ed.d, "_OK", 0, true));
    gtk_box_append(GTK_BOX(box), row);
    gtk_window_set_child(ed.d.w, box);
    gtk_widget_grab_focus(tv);

    int r = dlg_run(&ed.d);
    if (r == 0) {
        char *text = buffer_text(ed.buf);
        char *title = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e_title)));
        char *goal = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e_goal)));
        int att = atoi(gtk_editable_get_text(GTK_EDITABLE(e_att)));
        double dl = ws_parse_time(gtk_editable_get_text(GTK_EDITABLE(e_delay)));
        ws_app_close_file(a); /* frees icons; clears the path, restored below */
        ws_run_set_title(&a->run, title);
        ws_run_set_goal(&a->run, goal);
        a->run.attempts = att < 0 ? 0 : att;
        a->run.start_delay_ms = dl > 0 ? (int)llround(dl * 1000.0) : 0;
        ws_run_segments_from_text(&a->run, text);
        if (file) ws_run_set_file(&a->run, file);
        a->detail_w = pw;
        a->detail_h = ph;
        ws_app_run_edited(a);
        g_free(text);
        g_free(title);
        g_free(goal);
    } else {
        ws_run_restore_best(&a->run);
    }
    dlg_destroy(&ed.d);
    free(file);
    redraw();
}

/* ================================================================== */
/* Other commands                                                      */
/* ================================================================== */

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
    char buf[64] = "0:00";
    bool use_delay = false;
    if (!ui_prompt("Start at...", "Start offset (e.g. 1:30 or 12.5):", buf, sizeof buf, "Still use delay", &use_delay,
                   "_Start"))
        return;
    ws_app_start_at(&ui.app, ws_parse_time(buf), use_delay);
}

static void cmd_start_delay(void)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%g", ui.app.run.start_delay_ms / 1000.0);
    if (!ui_prompt("Run start delay", "Delay before the timer starts, in seconds (shown as a negative countdown):", buf,
                   sizeof buf, NULL, NULL, NULL))
        return;
    double d = ws_parse_time(buf);
    ui.app.run.start_delay_ms = d > 0 ? (int)llround(d * 1000.0) : 0;
    ui.app.run.unsaved = true;
    ws_app_initialize_display(&ui.app);
}

static void cmd_font(void)
{
    GtkFontDialog *d = gtk_font_dialog_new();
    gtk_font_dialog_set_title(d, "Segment font");
    gtk_font_dialog_set_modal(d, TRUE);
    Wait w = {0};
    gtk_font_dialog_choose_family(d, ui.win, NULL, NULL, wait_cb, &w);
    wait_run(&w);
    PangoFontFamily *fam = gtk_font_dialog_choose_family_finish(d, w.res, NULL);
    g_object_unref(w.res);
    g_object_unref(d);
    if (!fam) return;
    snprintf(ui.app.settings.font_family, sizeof ui.app.settings.font_family, "%s", pango_font_family_get_name(fam));
    g_object_unref(fam);
    rebuild_fonts();
    ws_app_layout_changed(&ui.app);
}

typedef struct {
    Dlg d;
    char captured[64];
    GtkWidget *label;
} HkDlg;

static gboolean hk_key_cb(GtkEventControllerKey *k, guint kv, guint kc, GdkModifierType st, gpointer ud)
{
    HkDlg *h = ud;
    if (is_modifier_key(kv)) return TRUE;
    unsigned mods = gdk_mods_to_ws(st);
    if (!mods && kv == GDK_KEY_Escape) { dlg_respond(&h->d, -1); return TRUE; }
    if (!mods && kv == GDK_KEY_BackSpace) { h->captured[0] = 0; dlg_respond(&h->d, 1); return TRUE; }
    char name[64];
    if (!keyval_name(base_keyval(GTK_EVENT_CONTROLLER(k), kv, kc), name, sizeof name)) return TRUE;
    ws_hotkey_format(h->captured, sizeof h->captured, mods, name);
    dlg_respond(&h->d, 1);
    return TRUE;
}

static void cmd_set_hotkey(int which)
{
    HkDlg h = {0};
    dlg_init(&h.d, "Set global hotkey", 380, -1);
    /* capture before the window's own Escape handler */
    GtkEventController *k = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(k, GTK_PHASE_CAPTURE);
    g_signal_connect(k, "key-pressed", G_CALLBACK(hk_key_cb), &h);
    gtk_widget_add_controller(GTK_WIDGET(h.d.w), k);
    GtkWidget *box = vbox(10, 16);
    char msg[300];
    snprintf(msg, sizeof msg,
             "Press the key combination for \"%s\".\n\nEscape cancels, Backspace clears.\n"
             "Your desktop may ask you to confirm the shortcut.",
             ws_hotkey_labels[which]);
    h.label = gtk_label_new(msg);
    gtk_label_set_xalign(GTK_LABEL(h.label), 0);
    gtk_box_append(GTK_BOX(box), h.label);
    GtkWidget *row = button_row();
    GtkWidget *cancel = dlg_button(&h.d, "Cancel", -1, false);
    gtk_widget_set_focusable(cancel, FALSE); /* keep Space/Enter capturable */
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(box), row);
    gtk_window_set_child(h.d.w, box);
    int r = dlg_run(&h.d);
    dlg_destroy(&h.d);
    if (r != 1) return;
    snprintf(ui.app.settings.hotkeys[which], sizeof ui.app.settings.hotkeys[which], "%s", h.captured);
    if (h.captured[0] && which != WS_HK_TOGGLE && !ui.app.settings.hotkeys_enabled) {
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

static void quit_now(void)
{
    if (!ui.app_ready) return;
    ui.app_ready = false;
    portal_close_session();
    ws_app_shutdown(&ui.app);
    if (ui.tick_id) g_source_remove(ui.tick_id);
    ui.tick_id = 0;
    GtkWindow *w = ui.win;
    ui.win = NULL;
    ui.area = NULL;
    if (w) gtk_window_destroy(w);
}

static void cmd_exit(void)
{
    if (ws_app_prompt_for_save(&ui.app)) quit_now();
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
    case WS_CMD_FONTS_CHANGED: rebuild_fonts(); break;
    case WS_CMD_COLORS_LOAD: cmd_colors(false); break;
    case WS_CMD_COLORS_SAVE: cmd_colors(true); break;
    case WS_CMD_ABOUT:
        ui_message("About WSplit",
                   "WSplit " WS_VERSION " for Linux (GTK 4)\n\n"
                   "Speedrun timer originally by Wodanaz (SDA),\n"
                   "updated by Nitrofski and contributors.\n"
                   "Native port of the open-source C# version.\n\n"
                   "Clock font: Digital-7 Mono by Style-7\n(freeware for personal use).");
        break;
    case WS_CMD_EXIT: cmd_exit(); break;
    default: break;
    }
    redraw();
}

/* ================================================================== */
/* Context menu: WsMenu -> GMenu + GtkPopoverMenu                      */
/* ================================================================== */

static GtkWidget *cur_popover;

static gboolean menu_activate_idle(gpointer p)
{
    if (!ui.app_ready) return G_SOURCE_REMOVE;
    int r = ws_menu_activate(&ui.app, GPOINTER_TO_INT(p));
    if (r != WS_CMD_NONE) run_command(r);
    redraw();
    return G_SOURCE_REMOVE;
}

static void menu_action_cb(GSimpleAction *act, GVariant *param, gpointer ud)
{
    (void)act; (void)param;
    /* core actions may prompt (nested loop): run once the popover is gone */
    g_idle_add(menu_activate_idle, ud);
}

static char *escape_mnemonic(const char *s)
{
    GString *g = g_string_new(NULL);
    for (; *s; s++) {
        if (*s == '_') g_string_append(g, "__");
        else g_string_append_c(g, *s);
    }
    return g_string_free(g, FALSE);
}

/* informational shortcut text -> accelerator string GTK can display */
static void shortcut_accel(const char *sc, char *out, size_t n)
{
    out[0] = 0;
    if (!sc || !*sc) return;
    unsigned mods;
    char key[64];
    if (!ws_hotkey_parse(sc, &mods, key, sizeof key)) return;
    guint kv = name_keyval(key);
    if (!kv) return;
    char *a = gtk_accelerator_name(kv, (mods & WS_MOD_CTRL ? GDK_CONTROL_MASK : 0) | (mods & WS_MOD_ALT ? GDK_ALT_MASK : 0) |
                                           (mods & WS_MOD_SHIFT ? GDK_SHIFT_MASK : 0) |
                                           (mods & WS_MOD_SUPER ? GDK_SUPER_MASK : 0));
    snprintf(out, n, "%s", a);
    g_free(a);
}

static GMenu *build_gmenu(const WsMenu *m, GSimpleActionGroup *grp)
{
    GMenu *root = g_menu_new();
    GMenu *sec = g_menu_new();
    for (int i = 0; i < m->count; i++) {
        const WsMenuItem *it = &m->items[i];
        if (it->kind == WS_MI_SEPARATOR) {
            if (g_menu_model_get_n_items(G_MENU_MODEL(sec)) > 0) {
                g_menu_append_section(root, NULL, G_MENU_MODEL(sec));
                g_object_unref(sec);
                sec = g_menu_new();
            }
            continue;
        }
        char *label = escape_mnemonic(it->label);
        if (it->kind == WS_MI_SUBMENU) {
            GMenu *sub = it->sub ? build_gmenu(it->sub, grp) : g_menu_new();
            g_menu_append_submenu(sec, label, G_MENU_MODEL(sub));
            g_object_unref(sub);
            g_free(label);
            continue;
        }
        char name[32];
        snprintf(name, sizeof name, "i%d", it->id);
        GSimpleAction *act;
        if (it->kind == WS_MI_CHECK || it->kind == WS_MI_RADIO)
            act = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(it->checked));
        else act = g_simple_action_new(name, NULL);
        g_simple_action_set_enabled(act, it->enabled);
        g_signal_connect(act, "activate", G_CALLBACK(menu_action_cb), GINT_TO_POINTER(it->id));
        g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(act));
        g_object_unref(act);
        char detailed[40];
        snprintf(detailed, sizeof detailed, "ws.%s", name);
        GMenuItem *mi = g_menu_item_new(label, detailed);
        char accel[64];
        shortcut_accel(it->shortcut, accel, sizeof accel);
        if (accel[0]) g_menu_item_set_attribute(mi, "accel", "s", accel);
        g_menu_append_item(sec, mi);
        g_object_unref(mi);
        g_free(label);
    }
    if (g_menu_model_get_n_items(G_MENU_MODEL(sec)) > 0) g_menu_append_section(root, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
    return root;
}

static gboolean unparent_idle(gpointer p)
{
    GtkWidget *w = p;
    if (gtk_widget_get_parent(w)) gtk_widget_unparent(w);
    g_object_unref(w);
    return G_SOURCE_REMOVE;
}

static void popover_closed(GtkPopover *p, gpointer ud)
{
    (void)ud;
    if (cur_popover == GTK_WIDGET(p)) cur_popover = NULL;
    /* actions fire after "closed"; unparent later */
    g_idle_add(unparent_idle, g_object_ref(p));
}

static void show_menu(double x, double y)
{
    if (cur_popover) gtk_popover_popdown(GTK_POPOVER(cur_popover));
    WsMenu *m = ws_menu_build(&ui.app, WS_MENU_PLATFORM_LINUX);
    GSimpleActionGroup *grp = g_simple_action_group_new();
    GMenu *model = build_gmenu(m, grp);
    ws_menu_free(m);
    gtk_widget_insert_action_group(GTK_WIDGET(ui.win), "ws", G_ACTION_GROUP(grp));
    g_object_unref(grp);

    GtkWidget *pop = gtk_popover_menu_new_from_model_full(G_MENU_MODEL(model), GTK_POPOVER_MENU_NESTED);
    g_object_unref(model);
    gtk_widget_set_parent(pop, ui.area);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_widget_set_halign(pop, GTK_ALIGN_START);
    GdkRectangle r = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    g_signal_connect(pop, "closed", G_CALLBACK(popover_closed), NULL);
    cur_popover = pop;
    gtk_popover_popup(GTK_POPOVER(pop));
}

static void on_right_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    (void)n; (void)ud;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    show_menu(x, y);
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
    double sx, sy;
    int start_w, start_h;
    double ref;
    int start_val;
} drag;

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

static const char *cursor_for(int mode, int edges)
{
    switch (mode) {
    case DRAG_WIDE_SEG_WIDTH: case DRAG_WIDE_SEGS: return "ew-resize";
    case DRAG_DETAIL_SEGS: return "ns-resize";
    case DRAG_CLOCK:
        if (edges == (EDGE_R | EDGE_B) || edges == (EDGE_L | EDGE_T)) return "nwse-resize";
        if (edges == (EDGE_L | EDGE_B) || edges == (EDGE_R | EDGE_T)) return "nesw-resize";
        if (edges & (EDGE_L | EDGE_R)) return "ew-resize";
        return "ns-resize";
    default: return NULL;
    }
}

static const char *cur_cursor;

static void set_cursor(const char *name)
{
    if (name == cur_cursor) return;
    cur_cursor = name;
    gtk_widget_set_cursor_from_name(ui.area, name);
}

static void on_motion(GtkEventControllerMotion *m, double x, double y, gpointer ud)
{
    (void)m; (void)ud;
    if (drag.mode != DRAG_NONE) return;
    int edges, mode = hit_test((int)x, (int)y, &edges);
    set_cursor(cursor_for(mode, edges));
}

static void on_leave(GtkEventControllerMotion *m, gpointer ud)
{
    (void)m; (void)ud;
    if (drag.mode == DRAG_NONE) set_cursor(NULL);
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer ud)
{
    (void)ud;
    int edges;
    drag.mode = hit_test((int)x, (int)y, &edges);
    drag.edges = edges;
    drag.sx = x;
    drag.sy = y;
    drag.start_w = ui.app.win_w;
    drag.start_h = ui.app.win_h;
    drag.ref = drag.mode == DRAG_DETAIL_SEGS ? y : x;
    drag.start_val = ui.app.settings.wide_seg_width;
    if (drag.mode != DRAG_MOVE) return;

    /* let the compositor move the window (the only way on Wayland) */
    GtkNative *native = gtk_widget_get_native(ui.area);
    GdkSurface *surf = native ? gtk_native_get_surface(native) : NULL;
    GdkEvent *ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(g));
    if (surf && GDK_IS_TOPLEVEL(surf) && ev) {
        graphene_point_t p = GRAPHENE_POINT_INIT((float)x, (float)y), q;
        double nx = 0, ny = 0;
        if (!gtk_widget_compute_point(ui.area, GTK_WIDGET(native), &p, &q)) q = p;
        gtk_native_get_surface_transform(native, &nx, &ny);
        gdk_toplevel_begin_move(GDK_TOPLEVEL(surf), gdk_event_get_device(ev),
                                (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)), q.x + nx, q.y + ny,
                                gdk_event_get_time(ev));
    }
    drag.mode = DRAG_NONE;
    gtk_event_controller_reset(GTK_EVENT_CONTROLLER(g));
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    (void)g; (void)ud;
    WsApp *a = &ui.app;
    switch (drag.mode) {
    case DRAG_CLOCK: {
        /* Wayland clients can't move themselves, so left/top edges grow the
           window to the right/bottom instead (mirrored drag). */
        int w = drag.start_w, h = drag.start_h;
        if (drag.edges & EDGE_R) w += (int)dx;
        if (drag.edges & EDGE_L) w -= (int)dx;
        if (drag.edges & EDGE_B) h += (int)dy;
        if (drag.edges & EDGE_T) h -= (int)dy;
        ws_app_user_resized(a, w, h);
        break;
    }
    case DRAG_DETAIL_SEGS: {
        double y = drag.sy + dy;
        int sh = a->seg_height;
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
        double x = drag.sx + dx;
        int sw = a->wide_seg_w;
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
        int v = drag.start_val + (int)dx;
        a->settings.wide_seg_width = v < 60 ? 60 : v;
        ws_app_relayout(a);
        break;
    }
    default:
        return;
    }
    redraw();
}

static void on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    (void)g; (void)dx; (void)dy; (void)ud;
    drag.mode = DRAG_NONE;
}

/* ================================================================== */
/* Drag & drop of .wsplit files                                        */
/* ================================================================== */

static gboolean load_path_idle(gpointer p)
{
    char *path = p;
    if (ui.app_ready && ws_app_prompt_for_save(&ui.app)) {
        if (!ws_app_load_file(&ui.app, path)) ui_message("Open", "That file could not be opened as a WSplit run.");
        redraw();
    }
    g_free(path);
    return G_SOURCE_REMOVE;
}

static gboolean on_drop(GtkDropTarget *t, const GValue *v, double x, double y, gpointer ud)
{
    (void)t; (void)x; (void)y; (void)ud;
    if (!G_VALUE_HOLDS(v, G_TYPE_FILE)) return FALSE;
    char *path = g_file_get_path(g_value_get_object(v));
    if (!path) return FALSE;
    g_idle_add(load_path_idle, path);
    return TRUE;
}

/* ================================================================== */
/* Timer                                                               */
/* ================================================================== */

static gboolean tick(gpointer ud)
{
    (void)ud;
    if (!ui.app_ready) { ui.tick_id = 0; return G_SOURCE_REMOVE; }
    if (ws_app_tick(&ui.app)) redraw();
    if (ui.app.settings.refresh_ms != ui.tick_ms) {
        ui.tick_ms = ui.app.settings.refresh_ms;
        ui.tick_id = g_timeout_add_full(G_PRIORITY_HIGH, (guint)ui.tick_ms, tick, NULL, NULL);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
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

static void find_asset_dir(const char *argv0)
{
    const char *env = getenv("WSPLIT_DATA");
    if (env && dir_has_font(env)) { snprintf(ui.asset_dir, sizeof ui.asset_dir, "%s", env); return; }
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe && argv0 && strchr(argv0, '/')) exe = g_canonicalize_filename(argv0, NULL);
    if (exe) {
        char *dir = g_path_get_dirname(exe);
        const char *rel[] = {"/assets", "/../assets", "/../share/wsplit", "/../../assets"};
        for (size_t i = 0; i < G_N_ELEMENTS(rel); i++) {
            char c[WS_PATH_MAX + 32];
            snprintf(c, sizeof c, "%s%s", dir, rel[i]);
            if (dir_has_font(c)) {
                snprintf(ui.asset_dir, sizeof ui.asset_dir, "%s", c);
                g_free(dir);
                g_free(exe);
                return;
            }
        }
        g_free(dir);
        g_free(exe);
    }
    const char *sys[] = {"/usr/local/share/wsplit", "/usr/share/wsplit"};
    for (size_t i = 0; i < G_N_ELEMENTS(sys); i++)
        if (dir_has_font(sys[i])) { snprintf(ui.asset_dir, sizeof ui.asset_dir, "%s", sys[i]); return; }
    snprintf(ui.asset_dir, sizeof ui.asset_dir, "assets");
}

static void init_core(const char *settings_path)
{
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
    ws_app_init(&ui.app, &host, settings_path);
    snprintf(ui.app.decimal_char, sizeof ui.app.decimal_char, "%s", dec);
    rebuild_fonts();
}

static gboolean on_close_request(GtkWindow *w, gpointer ud)
{
    (void)w; (void)ud;
    cmd_exit();
    return TRUE; /* quit_now destroys the window */
}

static void create_window(GtkApplication *gapp)
{
    ui.win = GTK_WINDOW(gtk_application_window_new(gapp));
    gtk_window_set_title(ui.win, "WSplit");
    gtk_window_set_icon_name(ui.win, APP_ID);
    gtk_window_set_decorated(ui.win, FALSE);
    gtk_window_set_resizable(ui.win, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(ui.win), "wsplit");
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window.wsplit { background: black; border-radius: 0; box-shadow: none; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    ui.area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.area), draw_func, NULL, NULL);
    gtk_widget_set_focusable(ui.area, TRUE);
    gtk_window_set_child(ui.win, ui.area);

    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(k, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(GTK_WIDGET(ui.win), k);
    g_signal_connect(ui.win, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    g_signal_connect(rc, "pressed", G_CALLBACK(on_right_click), NULL);
    gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(rc));

    GtkGesture *dg = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dg), GDK_BUTTON_PRIMARY);
    g_signal_connect(dg, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(dg, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(dg, "drag-end", G_CALLBACK(on_drag_end), NULL);
    gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(dg));

    GtkEventController *mo = gtk_event_controller_motion_new();
    g_signal_connect(mo, "motion", G_CALLBACK(on_motion), NULL);
    g_signal_connect(mo, "leave", G_CALLBACK(on_leave), NULL);
    gtk_widget_add_controller(ui.area, mo);

    GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect(dt, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(dt));

    g_signal_connect(ui.win, "close-request", G_CALLBACK(on_close_request), NULL);

    ui.app_ready = true;
    int w = ui.app.win_w > 0 ? ui.app.win_w : 240, h = ui.app.win_h > 0 ? ui.app.win_h : 50;
    h_resize_window(NULL, w, h);
    h_apply_window_props(NULL);
    ui.tick_ms = ui.app.settings.refresh_ms > 0 ? ui.app.settings.refresh_ms : 15;
    ui.tick_id = g_timeout_add_full(G_PRIORITY_HIGH, (guint)ui.tick_ms, tick, NULL, NULL);
}

static void ensure_window(GtkApplication *gapp, bool load_last)
{
    if (ui.win) return;
    create_window(gapp);
    if (load_last && ui.app.settings.load_most_recent && ui.app.settings.last_file[0])
        ws_app_load_file(&ui.app, ui.app.settings.last_file);
    gtk_window_present(ui.win);
    gtk_widget_grab_focus(ui.area);
    hotkeys_rebind();
}

static void on_activate(GtkApplication *gapp, gpointer ud)
{
    (void)ud;
    if (ui.win) { gtk_window_present(ui.win); return; }
    ensure_window(gapp, true);
}

static void on_open(GApplication *gapp, GFile **files, int n, const char *hint, gpointer ud)
{
    (void)hint; (void)ud;
    bool fresh = !ui.win;
    ensure_window(GTK_APPLICATION(gapp), false);
    if (n < 1) return;
    char *path = g_file_get_path(files[0]);
    if (!path) return;
    if (fresh) {
        if (!ws_app_load_file(&ui.app, path)) ui_message("Open", "That file could not be opened as a WSplit run.");
        g_free(path);
    } else {
        gtk_window_present(ui.win);
        g_idle_add(load_path_idle, path); /* second launch: ask to save first */
    }
    redraw();
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
        /* "running" for colours, starting now so elapsed ~ accumulated */
        a->timer.start_ns = ws_now_ns();
        a->timer.running = true;
        a->refreshing = false;
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
        else if (argv[i][0] != '-') file = argv[i];
    }
    find_asset_dir(argv[0]);

    if (shot) {
        /* offscreen rendering, no display connection required */
        init_core(NULL);
        if (file && !ws_app_load_file(&ui.app, file)) { fprintf(stderr, "could not load %s\n", file); return 1; }
        return screenshot_mode(shot, shot_mode, sim);
    }

    snprintf(ui.config_dir, sizeof ui.config_dir, "%s/wsplit", g_get_user_config_dir());
    g_mkdir_with_parents(ui.config_dir, 0755);
    char settings_path[WS_PATH_MAX + 32];
    snprintf(settings_path, sizeof settings_path, "%s/settings.ini", ui.config_dir);

    g_set_prgname(APP_ID);
    g_set_application_name("WSplit");
    ui.gapp = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(ui.gapp, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(ui.gapp, "open", G_CALLBACK(on_open), NULL);

    /* Only the primary instance initialises the core (a second launch just
       forwards its file over D-Bus and exits). */
    GError *err = NULL;
    if (!g_application_register(G_APPLICATION(ui.gapp), NULL, &err)) {
        fprintf(stderr, "wsplit: %s\n", err ? err->message : "could not register application");
        g_clear_error(&err);
    }
    if (!g_application_get_is_remote(G_APPLICATION(ui.gapp))) init_core(settings_path);

    int st = g_application_run(G_APPLICATION(ui.gapp), argc, argv);
    if (ui.app_ready) quit_now();
    if (!g_application_get_is_remote(G_APPLICATION(ui.gapp))) ws_app_free(&ui.app);
    g_object_unref(ui.gapp);
    return st;
}
