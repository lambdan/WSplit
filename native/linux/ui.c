/* WSplit Linux: popup menus, modal dialogs, file chooser. */
#define _GNU_SOURCE
#include "x11.h"

#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define UI_FONT_SIZE 13.0

/* ================================================================== */
/* helpers                                                             */
/* ================================================================== */

static void ui_font(cairo_t *cr, bool bold)
{
    cairo_set_font_face(cr, bold ? ui.ui_bold : ui.ui_face);
    cairo_set_font_size(cr, UI_FONT_SIZE);
}

static double text_w(cairo_t *cr, const char *s)
{
    cairo_text_extents_t te;
    cairo_text_extents(cr, s, &te);
    return te.x_advance;
}

static cairo_t *measure_cr(void)
{
    static cairo_surface_t *s;
    static cairo_t *cr;
    if (!cr) {
        s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cr = cairo_create(s);
    }
    return cr;
}

static Window make_popup(int x, int y, int w, int h, bool override, Atom type)
{
    XSetWindowAttributes swa = {0};
    swa.override_redirect = override;
    swa.background_pixel = WhitePixel(ui.dpy, ui.screen);
    swa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                     KeyReleaseMask | StructureNotifyMask | LeaveWindowMask;
    swa.save_under = True;
    Window w_ = XCreateWindow(ui.dpy, ui.root, x, y, (unsigned)w, (unsigned)h, 0, ui.depth, InputOutput, ui.visual,
                              CWOverrideRedirect | CWBackPixel | CWEventMask | CWSaveUnder, &swa);
    if (type) XChangeProperty(ui.dpy, w_, ui.a_net_wm_window_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&type, 1);
    return w_;
}

static void screen_size(int *w, int *h)
{
    *w = DisplayWidth(ui.dpy, ui.screen);
    *h = DisplayHeight(ui.dpy, ui.screen);
}

/* Wait for the next X event while keeping the timer display alive. */
static bool next_event_modal(XEvent *ev)
{
    while (!XPending(ui.dpy)) {
        ui_tick_background();
        int t = ui_next_timeout_ms();
        if (t < 0 || t > 100) t = 100;
        struct pollfd pfd = {ConnectionNumber(ui.dpy), POLLIN, 0};
        poll(&pfd, 1, t);
    }
    XNextEvent(ui.dpy, ev);
    return true;
}

/* ================================================================== */
/* Popup menu                                                          */
/* ================================================================== */

#define MI_H 22
#define SEP_H 9
#define MENU_PAD 4
#define MAX_LEVELS 6

typedef struct {
    WsMenu *m;
    Window w;
    int x, y, wd, ht;
    int hover;
    cairo_surface_t *surf;
} Level;

static Level lv[MAX_LEVELS];
static int nlv;

static int item_y(const WsMenu *m, int idx)
{
    int y = MENU_PAD;
    for (int i = 0; i < idx; i++) y += m->items[i].kind == WS_MI_SEPARATOR ? SEP_H : MI_H;
    return y;
}

static void menu_measure(WsMenu *m, int *w, int *h)
{
    cairo_t *cr = measure_cr();
    ui_font(cr, false);
    double mw = 120;
    for (int i = 0; i < m->count; i++) {
        double tw = text_w(cr, m->items[i].label);
        if (tw > mw) mw = tw;
    }
    *w = (int)mw + 28 + 30;
    *h = item_y(m, m->count) + MENU_PAD;
}

static void menu_draw(Level *L)
{
    cairo_t *cr = cairo_create(L->surf);
    ui_set_color(cr, 0xF4F4F4);
    cairo_paint(cr);
    ui_set_color(cr, 0x9A9A9A);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, L->wd - 1, L->ht - 1);
    cairo_stroke(cr);
    ui_font(cr, false);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    for (int i = 0; i < L->m->count; i++) {
        WsMenuItem *it = &L->m->items[i];
        int y = item_y(L->m, i);
        if (it->kind == WS_MI_SEPARATOR) {
            ui_set_color(cr, 0xD0D0D0);
            cairo_move_to(cr, 6, y + SEP_H / 2 + 0.5);
            cairo_line_to(cr, L->wd - 6, y + SEP_H / 2 + 0.5);
            cairo_stroke(cr);
            continue;
        }
        bool hot = i == L->hover && it->enabled;
        if (hot) {
            ui_set_color(cr, 0x2F6FD0);
            cairo_rectangle(cr, 2, y, L->wd - 4, MI_H);
            cairo_fill(cr);
        }
        unsigned fg = !it->enabled ? 0x9A9A9A : hot ? 0xFFFFFF : 0x1A1A1A;
        ui_set_color(cr, fg);
        double by = y + (MI_H - (fe.ascent + fe.descent)) / 2 + fe.ascent;
        if (it->checked) {
            if (it->kind == WS_MI_RADIO) {
                cairo_arc(cr, 13, y + MI_H / 2.0, 3.2, 0, 6.2832);
                cairo_fill(cr);
            } else {
                cairo_set_line_width(cr, 1.8);
                cairo_move_to(cr, 8, y + MI_H / 2.0);
                cairo_line_to(cr, 12, y + MI_H / 2.0 + 4);
                cairo_line_to(cr, 18, y + MI_H / 2.0 - 5);
                cairo_stroke(cr);
                cairo_set_line_width(cr, 1);
            }
        }
        cairo_move_to(cr, 26, by);
        cairo_show_text(cr, it->label);
        if (it->kind == WS_MI_SUBMENU) {
            double ax = L->wd - 14, ay = y + MI_H / 2.0;
            cairo_move_to(cr, ax, ay - 4);
            cairo_line_to(cr, ax + 4, ay);
            cairo_line_to(cr, ax, ay + 4);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
    cairo_destroy(cr);
    cairo_surface_flush(L->surf);
}

static void level_open(WsMenu *m, int x, int y, int parent_right, int parent_left)
{
    if (nlv >= MAX_LEVELS) return;
    Level *L = &lv[nlv];
    memset(L, 0, sizeof *L);
    L->m = m;
    L->hover = -1;
    menu_measure(m, &L->wd, &L->ht);
    int sw, sh;
    screen_size(&sw, &sh);
    if (parent_right >= 0) {
        x = parent_right - 2;
        if (x + L->wd > sw) x = parent_left - L->wd + 2;
    } else if (x + L->wd > sw) {
        x = sw - L->wd;
    }
    if (y + L->ht > sh) y = sh - L->ht;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    L->x = x;
    L->y = y;
    L->w = make_popup(x, y, L->wd, L->ht, true, ui.a_net_wm_type_menu);
    L->surf = cairo_xlib_surface_create(ui.dpy, L->w, ui.visual, L->wd, L->ht);
    XMapRaised(ui.dpy, L->w);
    nlv++;
    menu_draw(L);
}

static void level_close_from(int k)
{
    while (nlv > k) {
        Level *L = &lv[--nlv];
        cairo_surface_destroy(L->surf);
        XDestroyWindow(ui.dpy, L->w);
    }
}

static int item_at(Level *L, int rx, int ry)
{
    int x = rx - L->x, y = ry - L->y;
    if (x < 0 || y < 0 || x >= L->wd || y >= L->ht) return -2; /* outside */
    for (int i = 0; i < L->m->count; i++) {
        int y0 = item_y(L->m, i), y1 = item_y(L->m, i + 1);
        if (y >= y0 && y < y1) return L->m->items[i].kind == WS_MI_SEPARATOR ? -1 : i;
    }
    return -1;
}

static void set_hover(int level, int idx)
{
    Level *L = &lv[level];
    if (L->hover == idx) return;
    L->hover = idx;
    level_close_from(level + 1);
    menu_draw(L);
    if (idx >= 0) {
        WsMenuItem *it = &L->m->items[idx];
        if (it->kind == WS_MI_SUBMENU && it->enabled && it->sub)
            level_open(it->sub, 0, L->y + item_y(L->m, idx) - MENU_PAD, L->x + L->wd, L->x);
    }
}

static int activate(int level, int idx)
{
    WsMenuItem *it = &lv[level].m->items[idx];
    if (!it->enabled) return 0;
    if (it->kind == WS_MI_SUBMENU) { set_hover(level, idx); return 0; }
    if (it->kind == WS_MI_SEPARATOR) return 0;
    return it->id > 0 ? it->id : -1;
}

int ui_popup_menu(WsMenu *m, int rx, int ry)
{
    nlv = 0;
    level_open(m, rx, ry, -1, -1);
    XSync(ui.dpy, False);
    int result = 0;
    for (int tries = 0; tries < 20; tries++) {
        if (XGrabPointer(ui.dpy, lv[0].w, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                         GrabModeAsync, None, None, CurrentTime) == GrabSuccess)
            break;
        usleep(5000);
    }
    XGrabKeyboard(ui.dpy, lv[0].w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    long long opened = ws_now_ns();
    bool moved = false;
    int start_rx = rx, start_ry = ry;
    int kl = 0; /* menu level the keyboard acts on */

    while (result == 0) {
        XEvent ev;
        next_event_modal(&ev);
        switch (ev.type) {
        case Expose:
            for (int i = 0; i < nlv; i++) if (lv[i].w == ev.xexpose.window) menu_draw(&lv[i]);
            ui_pump_background_event(&ev);
            break;
        case MotionNotify: {
            int x = ev.xmotion.x_root, y = ev.xmotion.y_root;
            if (abs(x - start_rx) > 3 || abs(y - start_ry) > 3) moved = true;
            for (int k = nlv - 1; k >= 0; k--) {
                int idx = item_at(&lv[k], x, y);
                if (idx != -2) {
                    if (k < nlv - 1 && idx >= 0 && lv[k].m->items[idx].kind == WS_MI_SUBMENU && lv[k].hover == idx) break;
                    set_hover(k, idx);
                    kl = k;
                    break;
                }
            }
            break;
        }
        case ButtonPress: {
            int x = ev.xbutton.x_root, y = ev.xbutton.y_root;
            bool inside = false;
            for (int k = nlv - 1; k >= 0; k--) if (item_at(&lv[k], x, y) != -2) { inside = true; break; }
            if (!inside) result = -1;
            break;
        }
        case ButtonRelease: {
            if (!moved && ws_now_ns() - opened < 400000000LL) break; /* release of the opening click */
            int x = ev.xbutton.x_root, y = ev.xbutton.y_root;
            for (int k = nlv - 1; k >= 0; k--) {
                int idx = item_at(&lv[k], x, y);
                if (idx >= 0) { result = activate(k, idx); break; }
                if (idx == -1) break;
            }
            break;
        }
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (kl > nlv - 1) kl = nlv - 1;
            Level *L = &lv[kl];
            if (ks == XK_Escape || (ks == XK_Left && kl > 0)) {
                if (kl > 0) {
                    level_close_from(kl);
                    kl--;
                } else {
                    result = -1;
                }
            } else if (ks == XK_Down || ks == XK_Up) {
                int n = L->m->count, i = L->hover;
                for (int step = 0; step < n; step++) {
                    i = ks == XK_Down ? (i + 1 + n) % n : (i - 1 + n) % n;
                    if (L->m->items[i].kind != WS_MI_SEPARATOR) break;
                }
                set_hover(kl, i);
                level_close_from(kl + 1); /* submenus open with Right/Enter */
            } else if ((ks == XK_Right || ks == XK_Return || ks == XK_space) && L->hover >= 0 &&
                       L->m->items[L->hover].kind == WS_MI_SUBMENU && L->m->items[L->hover].enabled) {
                int h = L->hover;
                L->hover = -2;
                set_hover(kl, h);
                if (nlv > kl + 1) { kl++; set_hover(kl, 0); }
            } else if ((ks == XK_Return || ks == XK_space) && L->hover >= 0) {
                result = activate(kl, L->hover);
            }
            break;
        }
        default:
            ui_pump_background_event(&ev);
            break;
        }
    }
    XUngrabPointer(ui.dpy, CurrentTime);
    XUngrabKeyboard(ui.dpy, CurrentTime);
    level_close_from(0);
    XFlush(ui.dpy);
    return result > 0 ? result : 0;
}

/* ================================================================== */
/* Dialogs                                                             */
/* ================================================================== */

#define DLG_W_MIN 360
#define DLG_MARGIN 16
#define BTN_W 88
#define BTN_H 28
#define ENTRY_H 26

typedef struct {
    UiDialog *d;
    Window w;
    cairo_surface_t *surf;
    int wd, ht;
    char lines[24][256];
    int nlines;
    int msg_h, entry_y, check_y, btn_y;
    size_t cursor;
    int hot_btn;
    int pressed_btn;
} DlgState;

static void wrap_message(DlgState *s, cairo_t *cr, const char *msg, double maxw)
{
    s->nlines = 0;
    const char *p = msg ? msg : "";
    while (*p && s->nlines < 24) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char para[1024];
        if (len >= sizeof para) len = sizeof para - 1;
        memcpy(para, p, len);
        para[len] = 0;
        /* greedy word wrap */
        char *q = para;
        if (!*q) snprintf(s->lines[s->nlines++], 256, "%s", "");
        while (*q && s->nlines < 24) {
            size_t best = strlen(q);
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s", q);
            while (text_w(cr, tmp) > maxw) {
                char *sp = strrchr(tmp, ' ');
                if (!sp) break;
                *sp = 0;
                best = strlen(tmp);
            }
            snprintf(s->lines[s->nlines++], 256, "%.*s", (int)best, q);
            q += best;
            while (*q == ' ') q++;
        }
        p = nl ? nl + 1 : p + len;
    }
}

static void dlg_layout(DlgState *s)
{
    cairo_t *cr = measure_cr();
    ui_font(cr, false);
    double maxw = 0;
    const char *p = s->d->message ? s->d->message : "";
    char line[1024];
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        double w = text_w(cr, line);
        if (w > maxw) maxw = w;
        p = nl ? nl + 1 : p + len;
    }
    int nb = s->d->nbuttons;
    int btns = nb * (BTN_W + 8);
    s->wd = (int)maxw + 2 * DLG_MARGIN;
    if (s->wd < DLG_W_MIN) s->wd = DLG_W_MIN;
    if (s->wd < btns + 2 * DLG_MARGIN) s->wd = btns + 2 * DLG_MARGIN;
    if (s->wd > 620) s->wd = 620;
    wrap_message(s, cr, s->d->message, s->wd - 2 * DLG_MARGIN);
    s->msg_h = s->nlines * 18;
    int y = DLG_MARGIN + s->msg_h + 10;
    if (s->d->has_entry) { s->entry_y = y; y += ENTRY_H + 10; }
    if (s->d->check_label) { s->check_y = y; y += 24; }
    s->btn_y = y + 4;
    s->ht = s->btn_y + BTN_H + DLG_MARGIN;
}

static int btn_x(DlgState *s, int i)
{
    int nb = s->d->nbuttons;
    return s->wd - DLG_MARGIN - (nb - i) * (BTN_W + 8) + 8;
}

static void dlg_draw(DlgState *s)
{
    cairo_t *cr = cairo_create(s->surf);
    ui_set_color(cr, 0xEDEDED);
    cairo_paint(cr);
    ui_font(cr, false);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    ui_set_color(cr, 0x1A1A1A);
    for (int i = 0; i < s->nlines; i++) {
        cairo_move_to(cr, DLG_MARGIN, DLG_MARGIN + i * 18 + fe.ascent);
        cairo_show_text(cr, s->lines[i]);
    }
    if (s->d->has_entry) {
        int ex = DLG_MARGIN, ew = s->wd - 2 * DLG_MARGIN;
        ui_set_color(cr, 0xFFFFFF);
        cairo_rectangle(cr, ex, s->entry_y, ew, ENTRY_H);
        cairo_fill(cr);
        ui_set_color(cr, 0x3B78D8);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, ex + 0.5, s->entry_y + 0.5, ew - 1, ENTRY_H - 1);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_rectangle(cr, ex + 4, s->entry_y, ew - 8, ENTRY_H);
        cairo_clip(cr);
        char pre[WS_PATH_MAX];
        snprintf(pre, sizeof pre, "%.*s", (int)s->cursor, s->d->entry);
        double cw = text_w(cr, pre);
        double scroll = cw > ew - 16 ? cw - (ew - 16) : 0;
        double by = s->entry_y + (ENTRY_H - (fe.ascent + fe.descent)) / 2 + fe.ascent;
        ui_set_color(cr, 0x111111);
        cairo_move_to(cr, ex + 6 - scroll, by);
        cairo_show_text(cr, s->d->entry);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, ex + 6 - scroll + cw + 0.5, s->entry_y + 5);
        cairo_line_to(cr, ex + 6 - scroll + cw + 0.5, s->entry_y + ENTRY_H - 5);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    if (s->d->check_label) {
        int cy = s->check_y + 4;
        ui_set_color(cr, 0xFFFFFF);
        cairo_rectangle(cr, DLG_MARGIN, cy, 14, 14);
        cairo_fill(cr);
        ui_set_color(cr, 0x777777);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, DLG_MARGIN + 0.5, cy + 0.5, 13, 13);
        cairo_stroke(cr);
        if (s->d->check) {
            ui_set_color(cr, 0x2F6FD0);
            cairo_set_line_width(cr, 2);
            cairo_move_to(cr, DLG_MARGIN + 3, cy + 7);
            cairo_line_to(cr, DLG_MARGIN + 6, cy + 10);
            cairo_line_to(cr, DLG_MARGIN + 11, cy + 3);
            cairo_stroke(cr);
        }
        ui_set_color(cr, 0x1A1A1A);
        cairo_move_to(cr, DLG_MARGIN + 22, cy + 7 + (fe.ascent - fe.descent) / 2);
        cairo_show_text(cr, s->d->check_label);
    }
    for (int i = 0; i < s->d->nbuttons; i++) {
        int bx = btn_x(s, i);
        bool def = i == s->d->default_button;
        ui_set_color(cr, def ? 0x2F6FD0 : (i == s->hot_btn ? 0xE4E4E4 : 0xFAFAFA));
        cairo_rectangle(cr, bx, s->btn_y, BTN_W, BTN_H);
        cairo_fill(cr);
        ui_set_color(cr, def ? 0x245BB0 : 0xA8A8A8);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, bx + 0.5, s->btn_y + 0.5, BTN_W - 1, BTN_H - 1);
        cairo_stroke(cr);
        ui_font(cr, def);
        double tw = text_w(cr, s->d->buttons[i]);
        ui_set_color(cr, def ? 0xFFFFFF : 0x1A1A1A);
        cairo_move_to(cr, bx + (BTN_W - tw) / 2, s->btn_y + (BTN_H - (fe.ascent + fe.descent)) / 2 + fe.ascent);
        cairo_show_text(cr, s->d->buttons[i]);
        ui_font(cr, false);
    }
    cairo_destroy(cr);
    cairo_surface_flush(s->surf);
    XFlush(ui.dpy);
}

static int btn_at(DlgState *s, int x, int y)
{
    for (int i = 0; i < s->d->nbuttons; i++) {
        int bx = btn_x(s, i);
        if (x >= bx && x < bx + BTN_W && y >= s->btn_y && y < s->btn_y + BTN_H) return i;
    }
    return -1;
}

static size_t utf8_prev(const char *s, size_t i)
{
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static size_t utf8_next(const char *s, size_t i)
{
    size_t n = strlen(s);
    if (i >= n) return n;
    i++;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static bool is_modifier(KeySym ks)
{
    return ks == XK_Shift_L || ks == XK_Shift_R || ks == XK_Control_L || ks == XK_Control_R || ks == XK_Alt_L ||
           ks == XK_Alt_R || ks == XK_Super_L || ks == XK_Super_R || ks == XK_Meta_L || ks == XK_Meta_R ||
           ks == XK_ISO_Level3_Shift || ks == XK_Caps_Lock || ks == XK_Num_Lock || ks == XK_Mode_switch;
}

int ui_dialog_run(UiDialog *d)
{
    bool was_modal = ui.app.modal_open;
    ui.app.modal_open = true;
    DlgState s = {0};
    s.d = d;
    s.cursor = strlen(d->entry);
    s.hot_btn = -1;
    s.pressed_btn = -1;
    dlg_layout(&s);

    /* centre over the main window */
    int mx = 0, my = 0, sw, sh;
    Window child;
    if (ui.win) XTranslateCoordinates(ui.dpy, ui.win, ui.root, 0, 0, &mx, &my, &child);
    screen_size(&sw, &sh);
    int x = mx + ui.app.win_w / 2 - s.wd / 2, y = my + ui.app.win_h / 2 - s.ht / 2;
    if (x + s.wd > sw) x = sw - s.wd;
    if (y + s.ht > sh) y = sh - s.ht;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    s.w = make_popup(x, y, s.wd, s.ht, false, ui.a_net_wm_type_dialog);
    XStoreName(ui.dpy, s.w, d->title ? d->title : "WSplit");
    XChangeProperty(ui.dpy, s.w, ui.a_net_wm_name, ui.a_utf8, 8, PropModeReplace, (const unsigned char *)(d->title ? d->title : "WSplit"),
                    (int)strlen(d->title ? d->title : "WSplit"));
    if (ui.win) XSetTransientForHint(ui.dpy, s.w, ui.win);
    XSetWMProtocols(ui.dpy, s.w, &ui.a_wm_delete, 1);
    XSizeHints *hints = XAllocSizeHints();
    hints->flags = PMinSize | PMaxSize | PPosition | USPosition;
    hints->min_width = hints->max_width = s.wd;
    hints->min_height = hints->max_height = s.ht;
    hints->x = x;
    hints->y = y;
    XSetWMNormalHints(ui.dpy, s.w, hints);
    XFree(hints);
    XWMHints *wmh = XAllocWMHints();
    wmh->flags = InputHint;
    wmh->input = True;
    XSetWMHints(ui.dpy, s.w, wmh);
    XFree(wmh);
    XIC xic = NULL;
    if (ui.xim)
        xic = XCreateIC(ui.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, s.w, XNFocusWindow, s.w, NULL);
    s.surf = cairo_xlib_surface_create(ui.dpy, s.w, ui.visual, s.wd, s.ht);
    XMapRaised(ui.dpy, s.w);
    XMoveWindow(ui.dpy, s.w, x, y);

    bool kb_grabbed = false;
    int result = -1;
    while (result < 0) {
        XEvent ev;
        next_event_modal(&ev);
        if (XFilterEvent(&ev, None)) continue;
        if (ev.xany.window != s.w) {
            /* global hotkey presses go to the root window: keep their state sane */
            ui_pump_background_event(&ev);
            continue;
        }
        switch (ev.type) {
        case MapNotify:
            XSetInputFocus(ui.dpy, s.w, RevertToParent, CurrentTime);
            if (xic) XSetICFocus(xic);
            if (d->capture_key && !kb_grabbed) {
                for (int i = 0; i < 50 && !kb_grabbed; i++) {
                    kb_grabbed = XGrabKeyboard(ui.dpy, s.w, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
                    if (!kb_grabbed) usleep(10000);
                }
            }
            break;
        case Expose:
            if (ev.xexpose.count == 0) dlg_draw(&s);
            break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == ui.a_wm_delete) result = d->cancel_button;
            break;
        case MotionNotify: {
            int b = btn_at(&s, ev.xmotion.x, ev.xmotion.y);
            if (b != s.hot_btn) { s.hot_btn = b; dlg_draw(&s); }
            break;
        }
        case ButtonPress:
            s.pressed_btn = btn_at(&s, ev.xbutton.x, ev.xbutton.y);
            if (d->check_label && ev.xbutton.y >= s.check_y && ev.xbutton.y < s.check_y + 24) {
                d->check = !d->check;
                dlg_draw(&s);
            }
            break;
        case ButtonRelease: {
            int b = btn_at(&s, ev.xbutton.x, ev.xbutton.y);
            if (b >= 0 && b == s.pressed_btn) result = b;
            s.pressed_btn = -1;
            break;
        }
        case KeyPress: {
            KeySym ks = NoSymbol;
            char buf[64] = "";
            int n = 0;
            if (xic) {
                Status st;
                n = Xutf8LookupString(xic, &ev.xkey, buf, sizeof buf - 1, &ks, &st);
                if (st != XLookupChars && st != XLookupBoth) n = 0;
                if (st == XLookupChars) ks = NoSymbol;
            } else {
                n = XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, NULL);
            }
            buf[n > 0 ? n : 0] = 0;
            if (d->capture_key) {
                KeySym base = XLookupKeysym(&ev.xkey, 0);
                if (is_modifier(base)) break;
                unsigned mods = ui_x_mods_to_ws(ev.xkey.state);
                if (base == XK_Escape && !mods) { result = d->cancel_button; break; }
                if (base == XK_BackSpace && !mods) { d->captured[0] = 0; result = 1; break; }
                char name[64];
                if (ui_keysym_name(base, name, sizeof name)) {
                    ws_hotkey_format(d->captured, sizeof d->captured, mods, name);
                    result = 1;
                }
                break;
            }
            if (ks == XK_Escape) { result = d->cancel_button; break; }
            if (ks == XK_Return || ks == XK_KP_Enter) { result = d->default_button; break; }
            if (!d->has_entry) {
                if (ks == XK_Left || ks == XK_Right) {
                    int nb = d->nbuttons;
                    d->default_button = (d->default_button + (ks == XK_Right ? 1 : nb - 1)) % nb;
                    dlg_draw(&s);
                }
                break;
            }
            size_t len = strlen(d->entry);
            if (ks == XK_BackSpace) {
                if (s.cursor > 0) {
                    size_t p = utf8_prev(d->entry, s.cursor);
                    memmove(d->entry + p, d->entry + s.cursor, len - s.cursor + 1);
                    s.cursor = p;
                }
            } else if (ks == XK_Delete) {
                if (s.cursor < len) {
                    size_t nx = utf8_next(d->entry, s.cursor);
                    memmove(d->entry + s.cursor, d->entry + nx, len - nx + 1);
                }
            } else if (ks == XK_Left) {
                s.cursor = utf8_prev(d->entry, s.cursor);
            } else if (ks == XK_Right) {
                s.cursor = utf8_next(d->entry, s.cursor);
            } else if (ks == XK_Home) {
                s.cursor = 0;
            } else if (ks == XK_End) {
                s.cursor = len;
            } else if ((ev.xkey.state & ControlMask) && (ks == XK_u || ks == XK_U)) {
                d->entry[0] = 0;
                s.cursor = 0;
            } else if (n > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7F && !(ev.xkey.state & ControlMask)) {
                if (len + (size_t)n < sizeof d->entry) {
                    memmove(d->entry + s.cursor + n, d->entry + s.cursor, len - s.cursor + 1);
                    memcpy(d->entry + s.cursor, buf, (size_t)n);
                    s.cursor += (size_t)n;
                }
            }
            dlg_draw(&s);
            break;
        }
        default:
            break;
        }
    }
    if (kb_grabbed) XUngrabKeyboard(ui.dpy, CurrentTime);
    if (xic) XDestroyIC(xic);
    cairo_surface_destroy(s.surf);
    XDestroyWindow(ui.dpy, s.w);
    XFlush(ui.dpy);
    if (ui.win) XSetInputFocus(ui.dpy, ui.win, RevertToParent, CurrentTime);
    ui.app.modal_open = was_modal;
    memset(ui.keydown, 0, sizeof ui.keydown);
    ui.need_redraw = true;
    return result;
}

void ui_message(const char *title, const char *msg)
{
    UiDialog d = {0};
    d.title = title;
    d.message = msg;
    d.buttons[0] = "OK";
    d.nbuttons = 1;
    ui_dialog_run(&d);
}

WsAnswer ui_ask(const char *title, const char *msg, bool cancel)
{
    UiDialog d = {0};
    d.title = title;
    d.message = msg;
    d.buttons[0] = "Yes";
    d.buttons[1] = "No";
    d.nbuttons = 2;
    if (cancel) { d.buttons[2] = "Cancel"; d.nbuttons = 3; }
    d.default_button = 0;
    d.cancel_button = cancel ? 2 : 1;
    int r = ui_dialog_run(&d);
    return r == 0 ? WS_ANSWER_YES : r == 1 ? WS_ANSWER_NO : WS_ANSWER_CANCEL;
}

bool ui_prompt(const char *title, const char *msg, char *inout, size_t n)
{
    UiDialog d = {0};
    d.title = title;
    d.message = msg;
    d.buttons[0] = "OK";
    d.buttons[1] = "Cancel";
    d.nbuttons = 2;
    d.default_button = 0;
    d.cancel_button = 1;
    d.has_entry = true;
    snprintf(d.entry, sizeof d.entry, "%s", inout);
    if (ui_dialog_run(&d) != 0) return false;
    snprintf(inout, n, "%s", d.entry);
    return true;
}

/* ================================================================== */
/* Processes / file chooser                                            */
/* ================================================================== */

bool ui_have_program(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    char buf[WS_PATH_MAX];
    const char *p = path;
    while (*p) {
        const char *c = strchr(p, ':');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        snprintf(buf, sizeof buf, "%.*s/%s", (int)l, p, name);
        if (access(buf, X_OK) == 0) return true;
        if (!c) break;
        p = c + 1;
    }
    return false;
}

void ui_spawn_detached(const char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
            execvp(argv[0], (char *const *)argv);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* Run argv, capture first line of stdout, keep the UI alive meanwhile. */
static bool run_capture(const char *const argv[], char *out, size_t n)
{
    int pfd[2];
    if (pipe(pfd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return false; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]);
        close(pfd[1]);
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) dup2(fd, 2);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pfd[1]);
    bool was_modal = ui.app.modal_open;
    ui.app.modal_open = true;
    size_t got = 0;
    out[0] = 0;
    int xfd = ConnectionNumber(ui.dpy);
    for (;;) {
        while (XPending(ui.dpy)) {
            XEvent ev;
            XNextEvent(ui.dpy, &ev);
            ui_pump_background_event(&ev);
        }
        ui_tick_background();
        struct pollfd fds[2] = {{pfd[0], POLLIN, 0}, {xfd, POLLIN, 0}};
        int t = ui_next_timeout_ms();
        if (t < 0 || t > 100) t = 100;
        poll(fds, 2, t);
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char tmp[1024];
            ssize_t r = read(pfd[0], tmp, sizeof tmp);
            if (r <= 0) break;
            size_t take = (size_t)r;
            if (got + take >= n) take = n - got - 1;
            memcpy(out + got, tmp, take);
            got += take;
            out[got] = 0;
        }
    }
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    ui.app.modal_open = was_modal;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && out[0];
}

bool ui_choose_file(bool save, const char *title, const char *filter, const char *default_name, char *out, size_t n)
{
    char fname[256] = "*", fpat[256] = "*";
    const char *bar = filter ? strchr(filter, '|') : NULL;
    if (bar) {
        snprintf(fname, sizeof fname, "%.*s", (int)(bar - filter), filter);
        snprintf(fpat, sizeof fpat, "%s", bar + 1);
    }
    const char *home = getenv("HOME");
    char start[WS_PATH_MAX];
    if (ui.app.run.file) {
        snprintf(start, sizeof start, "%s", ui.app.run.file);
        char *sl = strrchr(start, '/');
        if (sl) sl[1] = 0;
    } else {
        snprintf(start, sizeof start, "%s/", home ? home : "/tmp");
    }
    char startfile[WS_PATH_MAX + 256];
    snprintf(startfile, sizeof startfile, "%s%s", start, default_name ? default_name : "");

    char f1[600], f2[64] = "All files | *";
    if (ui_have_program("zenity") || ui_have_program("qarma") || ui_have_program("yad")) {
        const char *prog = ui_have_program("zenity") ? "zenity" : ui_have_program("qarma") ? "qarma" : "yad";
        char targ[300];
        snprintf(f1, sizeof f1, "--file-filter=%s | %s", fname, fpat);
        snprintf(targ, sizeof targ, "--title=%s", title);
        char farg[WS_PATH_MAX + 300];
        snprintf(farg, sizeof farg, "--filename=%s", startfile);
        char fall[80];
        snprintf(fall, sizeof fall, "--file-filter=%s", f2);
        const char *argv_save[] = {prog, "--file-selection", "--save", "--confirm-overwrite", targ, farg, f1, fall, NULL};
        const char *argv_open[] = {prog, "--file-selection", targ, farg, f1, fall, NULL};
        return run_capture(save ? argv_save : argv_open, out, n);
    }
    if (ui_have_program("kdialog")) {
        snprintf(f1, sizeof f1, "%s (%s)", fname, fpat);
        const char *argv[] = {"kdialog", "--title", title, save ? "--getsavefilename" : "--getopenfilename", startfile, f1, NULL};
        return run_capture(argv, out, n);
    }
    /* fallback: type a path */
    char buf[WS_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", startfile);
    char msg[512];
    snprintf(msg, sizeof msg, "%s\n(No file chooser found - install zenity or kdialog for a file browser.)\nPath:", title);
    if (!ui_prompt(title, msg, buf, sizeof buf)) return false;
    if (buf[0] == '~' && home) {
        char tmp[WS_PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s%s", home, buf + 1);
        snprintf(buf, sizeof buf, "%s", tmp);
    }
    snprintf(out, n, "%s", buf);
    return buf[0] != 0;
}
