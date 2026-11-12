/*
 * WSplit - cross-platform port.
 *
 * Shared core: run model, stopwatch, .wsplit file format, settings,
 * application controller and renderer. Ported from the C# sources of
 * WSplit 1.5.x (Wodanaz, Nitrofski and contributors,
 * https://github.com/Nitrofski/WSplit).
 *
 * The core is plain C11 and knows nothing about any windowing system.
 * Front-ends (macOS/AppKit, Linux/X11) provide:
 *   - a WsHost: dialogs, window resizing, invalidation
 *   - a WsPainter: a handful of drawing primitives
 */
#ifndef WSPLIT_H
#define WSPLIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_VERSION "1.5.2-port"
#define WS_MAX_RECENT 10
#define WS_PATH_MAX 4096

/* Colours are 0xAARRGGBB. */
typedef uint32_t WsColor;

/* ------------------------------------------------------------------ */
/* Run model (port of Segment.cs / Split.cs)                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;
    char *icon_path;
    double old_time;
    double best_time;
    double best_segment;
    double backup_best;
    double backup_best_segment;
    double live_time;

    /* Computed by ws_app_update_detailed() for display. */
    char time_string[48];
    WsColor time_color;

    /* Front-end owned icon handle (may be NULL). */
    void *icon;
} WsSegment;

typedef enum {
    WS_CMP_FASTEST = 0,
    WS_CMP_OLD = 1,
    WS_CMP_BEST = 2,
    WS_CMP_SOB = 3
} WsCompare;

typedef struct {
    char *title;
    char *goal;
    int attempts;
    int start_delay_ms;
    WsCompare comp;
    int live_index;
    bool new_best_time;
    bool old_faster;
    WsSegment *segs;
    int count;
    int cap;
    char *file;        /* NULL if never saved */
    bool unsaved;
    int pref_w, pref_h; /* "Size=" from the file (detailed clock size) */
} WsRun;

void ws_run_init(WsRun *r);
void ws_run_free(WsRun *r);
void ws_run_clear(WsRun *r);            /* drop segments + Reset() */
WsSegment *ws_run_add(WsRun *r, const char *name, double old_t, double best_t, double best_seg);
void ws_run_remove(WsRun *r, int index);
void ws_run_set_title(WsRun *r, const char *s);
void ws_run_set_goal(WsRun *r, const char *s);
void ws_run_set_file(WsRun *r, const char *s);
void ws_seg_set_name(WsSegment *s, const char *name);
void ws_seg_set_icon_path(WsSegment *s, const char *path);

static inline int ws_run_last(const WsRun *r) { return r->count - 1; }
static inline bool ws_run_live(const WsRun *r) { return r->count > 0; }
static inline bool ws_run_done(const WsRun *r) { return ws_run_live(r) && r->live_index > ws_run_last(r); }

double ws_run_comp_time_at(const WsRun *r, int index);
double ws_run_comp_time(const WsRun *r);
double ws_run_sum_of_bests(const WsRun *r, int index);
void ws_run_do_split(WsRun *r, double time);
double ws_run_last_delta(const WsRun *r, int index);
double ws_run_live_segment(const WsRun *r, int index);
void ws_run_live_to_old(WsRun *r);
void ws_run_next(WsRun *r);
void ws_run_previous(WsRun *r);
void ws_run_reset(WsRun *r);
void ws_run_restore_best(WsRun *r);
double ws_run_run_delta(const WsRun *r, double time, int index);
double ws_run_run_delta_at(const WsRun *r, int index);
double ws_run_seg_delta(const WsRun *r, double time, int index);
bool ws_run_need_update(const WsRun *r, bool best_overall);
void ws_run_update_best(WsRun *r, bool best_overall);
WsCompare ws_run_comparing_type(const WsRun *r);
const char *ws_compare_name(WsCompare c);
void ws_run_recompute_old_faster(WsRun *r);
/* Run editor helper: fill missing/worse best segments from best splits. */
void ws_run_autofill_best_segments(WsRun *r);

/* ------------------------------------------------------------------ */
/* Stopwatch (monotonic clock; replaces DualStopwatch)                 */
/* ------------------------------------------------------------------ */

typedef struct {
    bool running;
    int64_t start_ns;      /* monotonic timestamp when last started */
    int64_t accumulated_ns;/* time accumulated across pauses */
    int64_t started_at_ns; /* offset ("Start at...") */
} WsStopwatch;

/* Monotonic clock in ns: QueryPerformanceCounter on Windows,
   CLOCK_MONOTONIC elsewhere. */
int64_t ws_now_ns(void);
void ws_sw_reset(WsStopwatch *sw);
/* The *_t variants take the moment the action happened (ws_now_ns base),
   so a key press can be timed from its event timestamp. */
void ws_sw_start_t(WsStopwatch *sw, int64_t t);
void ws_sw_start_at_t(WsStopwatch *sw, int64_t offset_ns, int64_t t);
void ws_sw_stop_t(WsStopwatch *sw, int64_t t);
int64_t ws_sw_elapsed_ns_t(const WsStopwatch *sw, int64_t t);
static inline void ws_sw_start(WsStopwatch *sw) { ws_sw_start_t(sw, ws_now_ns()); }
static inline void ws_sw_start_at(WsStopwatch *sw, int64_t off) { ws_sw_start_at_t(sw, off, ws_now_ns()); }
static inline void ws_sw_stop(WsStopwatch *sw) { ws_sw_stop_t(sw, ws_now_ns()); }
static inline int64_t ws_sw_elapsed_ns(const WsStopwatch *sw) { return ws_sw_elapsed_ns_t(sw, ws_now_ns()); }
static inline double ws_sw_elapsed_s(const WsStopwatch *sw) { return (double)ws_sw_elapsed_ns(sw) / 1e9; }

/* ------------------------------------------------------------------ */
/* Time formatting / parsing                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    WS_FMT_SECONDS,
    WS_FMT_SHORT,
    WS_FMT_LONG,
    WS_FMT_DELTA,
    WS_FMT_DELTA_SHORT
} WsTimeFormat;

/* Port of WSplit.timeFormatter. */
void ws_format_time(char *out, size_t n, double secs, WsTimeFormat fmt);
/* Port of RunEditor.timeFormat: h:mm:ss.ff / m:ss.ff / s.ff */
void ws_format_edit_time(char *out, size_t n, double secs);
/* Port of timeParse: "1:02:03.45" -> seconds. Accepts '.' or ','. */
double ws_parse_time(const char *s);
/* Locale-independent number parse / print used by the file format. */
double ws_parse_double(const char *s, bool *ok);
void ws_print_double(char *out, size_t n, double v);

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    WS_MODE_TIMER = 0,
    WS_MODE_COMPACT = 1,
    WS_MODE_WIDE = 2,
    WS_MODE_DETAILED = 3
} WsDisplayMode;

typedef enum {
    WS_HK_SPLIT = 0,
    WS_HK_PAUSE,
    WS_HK_STOP,
    WS_HK_RESET,
    WS_HK_PREV,
    WS_HK_NEXT,
    WS_HK_COMPARE,
    WS_HK_TOGGLE,
    WS_HK_COUNT
} WsHotkeyAction;

extern const char *const ws_hotkey_labels[WS_HK_COUNT];

/* Field list for colours: X(name, default) */
#define WS_COLOR_FIELDS(X)                                   \
    X(AheadBack, 0xFF243108) X(AheadBack2, 0xFF486310)       \
    X(AheadBackPlain, 0xFF30420A) X(AheadFore, 0xFFADFF2F)   \
    X(AheadLosingBack, 0xFF2D2B1A) X(AheadLosingBack2, 0xFF5A5635) \
    X(AheadLosingBackPlain, 0xFF3C3923) X(AheadLosingFore, 0xFFF0E68C) \
    X(BehindBack, 0xFF301F00) X(BehindBack2, 0xFF603E00)     \
    X(BehindBackPlain, 0xFF402900) X(BehindFore, 0xFFFFA500) \
    X(BehindLosingBack, 0xFF2D1818) X(BehindLosingBack2, 0xFF5A3030) \
    X(BehindLosingBackPlain, 0xFF3C2020) X(BehindLosingFore, 0xFFF08080) \
    X(DelayBack, 0xFF272727) X(DelayBack2, 0xFF4F4F4F)       \
    X(DelayBackPlain, 0xFF343434) X(DelayFore, 0xFFD3D3D3)   \
    X(FinishedBack, 0xFF29040B) X(FinishedBack2, 0xFF530817) \
    X(FinishedBackPlain, 0xFF37050F) X(FinishedFore, 0xFFDC143C) \
    X(Flash, 0xFFFFFFFF)                                     \
    X(FutureSegName, 0xFFDCDCDC) X(FutureSegTime, 0xFFA9A9A9) \
    X(LiveSeg, 0xFFFFFFFF) X(PastSeg, 0xFFA9A9A9)            \
    X(Paused, 0xFFFFFF00)                                    \
    X(RecordBack, 0xFF002430) X(RecordBack2, 0xFF004860)     \
    X(RecordBackPlain, 0xFF003040) X(RecordFore, 0xFF00BFFF) \
    X(SegAheadGain, 0xFFADFF2F) X(SegAheadLoss, 0xFF9ACD32)  \
    X(SegBack, 0xFF0D0D0D) X(SegBack2, 0xFF282828)           \
    X(SegBackPlain, 0xFF1A1A1A)                              \
    X(SegBehindGain, 0xFFCD5C5C) X(SegBehindLoss, 0xFFFF4500) \
    X(SegBestSegment, 0xFFF0B012)                            \
    X(SegHighlight, 0xFF07233F) X(SegHighlight2, 0xFF0E487F) \
    X(SegHighlightBorder, 0xFF166CBF) X(SegHighlightPlain, 0xFF0B365F) \
    X(SegMissingTime, 0xFF696969) X(SegNewTime, 0xFFEEE8AA)  \
    X(StatusBack, 0xFF1C201C) X(StatusBack2, 0xFF394039)     \
    X(StatusBackPlain, 0xFF2A302A) X(StatusFore, 0xFFDCDCDC) \
    X(TitleBack, 0xFF343434) X(TitleBack2, 0xFF3E3E3E)       \
    X(TitleBackPlain, 0xFF2A302A) X(TitleFore, 0xFFFFFFFF)   \
    X(WatchBack, 0xFF1A2308) X(WatchBack2, 0xFF354611)       \
    X(WatchBackPlain, 0xFF232E0B) X(WatchFore, 0xFF9ACD32)

typedef struct {
#define X(name, def) WsColor name;
    WS_COLOR_FIELDS(X)
#undef X
} WsColors;

void ws_colors_default(WsColors *c);

typedef struct {
    /* general */
    bool best_as_overall;
    int compare_against;   /* WsCompare */
    int double_tap_guard_ms;
    int refresh_ms;
    bool load_most_recent;
    bool save_window_pos;
    int window_x, window_y;
    bool on_top;
    double opacity;

    /* display */
    int display_mode;      /* WsDisplayMode */
    int clock_w, clock_h;  /* timer/compact clock size */
    bool digital_clock;
    bool clock_accent;
    bool show_decimal_separator;
    bool background_black;
    bool background_plain;
    bool horizontal_gradient;
    int display_segs;
    bool display_blank_segs;
    bool show_last_detailed;
    int wide_segs;
    bool wide_seg_blanks;
    bool show_last_wide;
    int wide_height;
    int wide_seg_width;
    int segment_icons;     /* 0 none, 1 16px, 2 24px, 3 32px */
    bool show_title;
    bool show_goal;
    bool show_attempts;
    bool show_prev_seg;
    bool show_time_save;
    bool show_sob;
    bool pred_pb;
    bool pred_best;

    /* fonts */
    char font_family[128];
    double font_multiplier;

    /* hotkeys: textual descriptions like "Ctrl+Shift+F1"; "" = none */
    bool hotkeys_enabled;
    char hotkeys[WS_HK_COUNT][64];

    /* files */
    char last_file[WS_PATH_MAX];
    char recent[WS_MAX_RECENT][WS_PATH_MAX];
    int recent_count;

    WsColors colors;
} WsSettings;

void ws_settings_default(WsSettings *s);
bool ws_settings_load(WsSettings *s, const char *path);
bool ws_settings_save(const WsSettings *s, const char *path);
void ws_settings_add_recent(WsSettings *s, const char *path);
void ws_settings_clear_recent(WsSettings *s);
/* Colour scheme import/export (same keys as the settings file). */
bool ws_colors_save(const WsColors *c, const char *path);
bool ws_colors_load(WsColors *c, const char *path);

/* ------------------------------------------------------------------ */
/* .wsplit files                                                       */
/* ------------------------------------------------------------------ */

/* Returns false (and leaves r cleared) when the file isn't a valid run. */
bool ws_runfile_load(WsRun *r, const char *path);
/* Writes r to path. Returns false and sets errbuf on failure. */
bool ws_runfile_save(const WsRun *r, const char *path, char *errbuf, size_t errlen);
/* Import a LiveSplit .lss file (subset: segments, PB, best segments). */
bool ws_runfile_import_lss(WsRun *r, const char *path);

/* ------------------------------------------------------------------ */
/* Painter interface (implemented by each front-end)                   */
/* ------------------------------------------------------------------ */

typedef enum {
    WS_FONT_DISPLAY = 0,  /* segment names, status text */
    WS_FONT_TIME,         /* bold, segment times / deltas */
    WS_FONT_CLOCK_LARGE,  /* bold, clock when not digital */
    WS_FONT_CLOCK_MED,
    WS_FONT_DIGIT_LARGE,  /* Digital-7 Mono */
    WS_FONT_DIGIT_MED,
    WS_FONT_COUNT
} WsFont;

typedef struct {
    void *ctx;
    /* Coordinates are in window points with y growing downward. */
    void (*fill_rect)(void *ctx, double x, double y, double w, double h, WsColor c);
    /* Linear gradient. horizontal: c1 at left -> c2 at right;
       vertical: c1 at bottom -> c2 at top (GDI+ angle 270). */
    void (*fill_gradient)(void *ctx, double x, double y, double w, double h,
                          WsColor c1, WsColor c2, bool horizontal);
    void (*stroke_line)(void *ctx, double x1, double y1, double x2, double y2, WsColor c);
    /* Text: y is the top of the line box (ascent above baseline). */
    void (*draw_text)(void *ctx, WsFont f, const char *utf8, double x, double y, WsColor c);
    double (*text_width)(void *ctx, WsFont f, const char *utf8);
    /* ascent + descent = line height */
    void (*font_metrics)(void *ctx, WsFont f, double *ascent, double *descent);
    /* Font pixel size (em) after multipliers, used for GDI+-style padding. */
    double (*font_size)(void *ctx, WsFont f);
    void (*push_clip)(void *ctx, double x, double y, double w, double h);
    void (*pop_clip)(void *ctx);
    /* Translate + uniform scale, nested. */
    void (*push_transform)(void *ctx, double tx, double ty, double scale);
    void (*pop_transform)(void *ctx);
    /* Optional, may be NULL. Draws segment icon (seg->icon handle). */
    void (*draw_icon)(void *ctx, void *icon, double x, double y, double size,
                      double alpha, bool gray);
} WsPainter;

/* ------------------------------------------------------------------ */
/* Host interface (implemented by each front-end)                      */
/* ------------------------------------------------------------------ */

typedef enum { WS_ANSWER_YES = 0, WS_ANSWER_NO = 1, WS_ANSWER_CANCEL = 2 } WsAnswer;

typedef struct {
    void *ud;
    /* Modal Yes/No/Cancel (cancel_allowed=false -> Yes/No only). */
    WsAnswer (*ask)(void *ud, const char *title, const char *message, bool cancel_allowed);
    void (*message)(void *ud, const char *title, const char *message);
    /* Ask for a path to save a run. Return false if cancelled. */
    bool (*choose_save_path)(void *ud, char *out, size_t n);
    /* The window should be resized (content size in points). */
    void (*resize_window)(void *ud, int w, int h);
    void (*invalidate)(void *ud);
    /* Global hotkey bindings or enabled-state changed. */
    void (*hotkeys_changed)(void *ud);
    /* Load/free icon handles for segments (may be NULL). */
    void *(*load_icon)(void *ud, const char *path);
    void (*free_icon)(void *ud, void *icon);
    /* Recent files list changed (rebuild menu). May be NULL. */
    void (*recent_changed)(void *ud);
    /* on_top / opacity changed. May be NULL. */
    void (*apply_window_props)(void *ud);
} WsHost;

/* ------------------------------------------------------------------ */
/* Application controller (port of the WSplit form logic)              */
/* ------------------------------------------------------------------ */

typedef struct { int x, y, w, h; } WsRect;

typedef struct {
    WsRun run;
    WsStopwatch timer;
    WsSettings settings;
    WsHost host;

    char settings_path[WS_PATH_MAX];
    char decimal_char[8];

    /* stopwatch "Enabled": refreshing while running */
    bool refreshing;

    /* start delay */
    bool delay_active;
    int64_t delay_start_ns;
    int64_t delay_offset_ns;   /* Start at... offset applied after delay */

    /* double tap guard */
    int64_t last_tap_ns;

    /* layout */
    WsDisplayMode cur_mode;
    WsRect clock;
    int clock_min_w, clock_min_h;
    int detail_w, detail_h;    /* detailPreferredSize */
    int seg_height;
    int wide_seg_w;
    int win_w, win_h;

    /* paint state carried between frames */
    WsColor clock_fore, clock_gr, clock_gr2, clock_plain;
    bool seg_losing_time;
    bool run_losing_time;

    bool modal_open;

    /* Timestamp (ws_now_ns base) of the input event being handled, 0 = now.
       Set with ws_app_set_input_time() right before an action. */
    int64_t input_ns;
} WsApp;

/* settings_path: where settings.ini lives. host must be filled. */
void ws_app_init(WsApp *a, const WsHost *host, const char *settings_path);
void ws_app_shutdown(WsApp *a); /* saves settings */
void ws_app_free(WsApp *a);

/* Load a run from disk. Returns false on failure (run is closed). */
bool ws_app_load_file(WsApp *a, const char *path);
void ws_app_reload(WsApp *a);
void ws_app_close_file(WsApp *a);
bool ws_app_save(WsApp *a);           /* save to current file or ask */
bool ws_app_save_as(WsApp *a);
/* Returns false if the user cancelled. */
bool ws_app_prompt_for_save(WsApp *a);

/* Timer actions */
void ws_app_timer_control(WsApp *a);  /* Start/Split */
void ws_app_pause_resume(WsApp *a);
void ws_app_stop(WsApp *a);
void ws_app_reset(WsApp *a);
void ws_app_prev(WsApp *a);
void ws_app_next(WsApp *a);
void ws_app_switch_comparison(WsApp *a);
void ws_app_set_compare(WsApp *a, WsCompare c);
void ws_app_toggle_hotkeys(WsApp *a);
void ws_app_set_as_old(WsApp *a);
bool ws_app_start_at(WsApp *a, double seconds, bool use_delay);
void ws_app_hotkey(WsApp *a, WsHotkeyAction act);
/* Tell the core that the next action was triggered `latency_ns` ago
   (from the OS event timestamp). Ignored if outside 0..1 s. */
void ws_app_set_input_latency(WsApp *a, int64_t latency_ns);
/* Local (focused) key handling. key is a name as in hotkey strings
   ("Space", "Enter", "Left", "Up", "0", "P", "Tab", ...). Returns true if handled. */
bool ws_app_local_key(WsApp *a, const char *key);

/* Call every settings.refresh_ms. Returns true if a redraw is needed. */
bool ws_app_tick(WsApp *a);

/* Layout */
void ws_app_set_display(WsApp *a, WsDisplayMode mode);
void ws_app_relayout(WsApp *a);  /* re-run set_display for current mode */
void ws_app_segs_inc(WsApp *a);
void ws_app_segs_dec(WsApp *a);
/* The user resized the window (via a window edge) to w x h. */
void ws_app_user_resized(WsApp *a, int w, int h);
/* Called after the run was edited in the run editor. */
void ws_app_run_edited(WsApp *a);
void ws_app_update_display(WsApp *a);
void ws_app_update_detailed(WsApp *a);
/* Settings that affect layout changed (icons, title, extras...). */
void ws_app_layout_changed(WsApp *a);
void ws_app_initialize_display(WsApp *a);
void ws_app_reload_icons(WsApp *a);

/* Is the timer actually started (elapsed > 0)? */
bool ws_app_started(const WsApp *a);
bool ws_app_timer_running(const WsApp *a);

/* Render one frame. */
void ws_render(WsApp *a, const WsPainter *p);

/* Hotkey string helpers */
/* Parse "Ctrl+Alt+Shift+Cmd+KEY". Returns false if empty/invalid.
   mods: bit0 ctrl, bit1 alt, bit2 shift, bit3 super/cmd. */
#define WS_MOD_CTRL 1
#define WS_MOD_ALT 2
#define WS_MOD_SHIFT 4
#define WS_MOD_SUPER 8
bool ws_hotkey_parse(const char *s, unsigned *mods, char *key, size_t keylen);
void ws_hotkey_format(char *out, size_t n, unsigned mods, const char *key);

/* ------------------------------------------------------------------ */
/* Context menu model (shared; rendered natively by each front-end)    */
/* ------------------------------------------------------------------ */

typedef enum { WS_MI_ACTION, WS_MI_CHECK, WS_MI_RADIO, WS_MI_SUBMENU, WS_MI_SEPARATOR } WsMenuKind;

typedef struct WsMenu WsMenu;
typedef struct {
    WsMenuKind kind;
    char label[300];
    char shortcut[24];   /* informational, e.g. "Space" */
    bool checked;
    bool enabled;
    int id;
    WsMenu *sub;
} WsMenuItem;

struct WsMenu {
    WsMenuItem *items;
    int count, cap;
};

/* Commands the front-end must implement itself (dialogs etc). */
enum {
    WS_CMD_NONE = 0,
    WS_CMD_EDIT_RUN = 1,
    WS_CMD_OPEN,
    WS_CMD_IMPORT_LSS,
    WS_CMD_START_AT,
    WS_CMD_START_DELAY,
    WS_CMD_FONT,
    WS_CMD_SETTINGS,       /* macOS settings window */
    WS_CMD_COLORS_EDIT,    /* macOS colour editor */
    WS_CMD_COLORS_LOAD,
    WS_CMD_COLORS_SAVE,
    WS_CMD_ABOUT,
    WS_CMD_EXIT,
    WS_CMD_NEW_RUN,
    WS_CMD_FONTS_CHANGED = 90,    /* font settings changed: rebuild fonts */
    WS_CMD_SET_HOTKEY_BASE = 100, /* + WsHotkeyAction */
    WS_CMD_CORE_BASE = 1000       /* ids >= this are handled by the core */
};

#define WS_MENU_PLATFORM_MAC 1
#define WS_MENU_PLATFORM_LINUX 2

WsMenu *ws_menu_build(WsApp *a, int platform);
void ws_menu_free(WsMenu *m);
/* Execute a menu id. Returns the id if the front-end must handle it,
   or WS_CMD_NONE if the core handled it. */
int ws_menu_activate(WsApp *a, int id);
/* Apply a colour scheme / restore defaults and redraw. */
void ws_app_colors_changed(WsApp *a);

/* misc */
char *ws_strdup(const char *s);

#ifdef __cplusplus
}
#endif
#endif
