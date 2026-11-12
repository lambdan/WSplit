/* WSplit Linux front-end (X11 + cairo): shared declarations. */
#ifndef WSPLIT_X11_H
#define WSPLIT_X11_H

#include "../core/wsplit.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <stdbool.h>

typedef struct {
    Display *dpy;
    int screen;
    Window root, win;
    Visual *visual;
    int depth;
    Colormap cmap;
    XIM xim;
    XIC xic;
    cairo_surface_t *surf;
    int surf_w, surf_h;

    Atom a_wm_delete, a_wm_protocols, a_motif_hints, a_net_wm_state, a_net_wm_state_above,
        a_net_wm_opacity, a_net_wm_icon, a_net_wm_name, a_utf8, a_net_wm_window_type,
        a_net_wm_type_dialog, a_net_wm_type_menu, a_net_wm_type_popup;

    WsApp app;
    bool need_redraw;
    bool quit;
    bool mapped;

    char asset_dir[WS_PATH_MAX];
    char config_dir[WS_PATH_MAX];

    /* fonts */
    cairo_font_face_t *face[WS_FONT_COUNT];
    double fsize[WS_FONT_COUNT];
    cairo_font_face_t *ui_face, *ui_bold;

    /* external editor watch */
    char watch_path[WS_PATH_MAX];
    long long watch_mtime;
    bool watch_pending;

    /* key repeat filter */
    bool keydown[256];

    /* hotkey grab bookkeeping */
    struct { KeyCode code; unsigned mods; bool grabbed; } grabs[WS_HK_COUNT];
} Ui;

extern Ui ui;

/* main.c */
void ui_draw_main(void);
void ui_pump_background_event(XEvent *ev); /* handle main-window events during modal loops */
void ui_tick_background(void);
int ui_next_timeout_ms(void);
void ui_rebuild_fonts(void);
bool ui_keysym_name(KeySym ks, char *out, size_t n);
KeySym ui_name_keysym(const char *name);
unsigned ui_x_mods_to_ws(unsigned state);

/* ui.c - menus and dialogs */
int ui_popup_menu(WsMenu *m, int root_x, int root_y);

typedef struct {
    const char *title;
    const char *message;
    const char *buttons[4];
    int nbuttons;
    int default_button;
    int cancel_button;
    bool has_entry;
    char entry[WS_PATH_MAX];
    const char *check_label;
    bool check;
    bool capture_key;     /* hotkey capture mode */
    char captured[64];    /* result in capture mode ("" = cleared) */
} UiDialog;

int ui_dialog_run(UiDialog *d);
void ui_message(const char *title, const char *msg);
WsAnswer ui_ask(const char *title, const char *msg, bool cancel);
bool ui_prompt(const char *title, const char *msg, char *inout, size_t n);
/* filters: "Name|*.a *.b" ; save=true for save dialogs */
bool ui_choose_file(bool save, const char *title, const char *filter, const char *default_name,
                    char *out, size_t n);
void ui_spawn_detached(const char *const argv[]);
bool ui_have_program(const char *name);

/* text helpers for UI drawing */
void ui_set_color(cairo_t *cr, unsigned rgb);

#endif
