/* Shared context menu: mirrors the original WSplit right-click menu. */
#define _POSIX_C_SOURCE 200809L
#include "wsplit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    C = WS_CMD_CORE_BASE,
    C_SAVE, C_SAVE_AS, C_RELOAD, C_CLOSE, C_STOP, C_RESET, C_SET_OLD,
    C_ON_TOP, C_SHOW_TITLE, C_SHOW_GOAL, C_SHOW_ATTEMPTS,
    C_MODE_TIMER, C_MODE_COMPACT, C_MODE_WIDE, C_MODE_DETAILED,
    C_DEC_SEP, C_DIGITAL, C_ACCENT, C_HGRAD,
    C_BG_DEFAULT, C_BG_PLAIN, C_BG_BLACK,
    C_X_PREV, C_X_GAIN, C_X_SOB, C_X_PREDPB, C_X_PREDBEST,
    C_ICONS_0, C_ICONS_1, C_ICONS_2, C_ICONS_3,
    C_BLANKS_DETAILED, C_LAST_DETAILED, C_BLANKS_WIDE, C_LAST_WIDE,
    C_CMP_OLD, C_CMP_BEST, C_CMP_FASTEST, C_CMP_SOB,
    C_BEST_OVERALL, C_BEST_SPLITS,
    C_HK_ENABLED, C_HK_CLEAR_ALL,
    C_RELOAD_ON_START, C_SAVE_POS,
    C_COLORS_DEFAULT, C_CLEAR_RECENT,
    C_OPACITY_BASE = C + 200,     /* +10..+100 (percent / 10) */
    C_DOUBLETAP_BASE = C + 300,   /* + index */
    C_REFRESH_BASE = C + 400,     /* + index */
    C_FONTMULT_BASE = C + 500,    /* + index */
    C_RECENT_BASE = C + 600       /* + index */
};

static const int doubletap_vals[] = {0, 100, 250, 500, 750, 1000};
static const int refresh_vals[] = {8, 15, 30, 50};
static const double fontmult_vals[] = {0.8, 1.0, 1.2, 1.5, 2.0};

static WsMenu *menu_new(void) { return calloc(1, sizeof(WsMenu)); }

static WsMenuItem *add(WsMenu *m, WsMenuKind kind, const char *label, int id, bool checked, bool enabled)
{
    if (m->count == m->cap) {
        int nc = m->cap ? m->cap * 2 : 16;
        WsMenuItem *n = realloc(m->items, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        m->items = n;
        m->cap = nc;
    }
    WsMenuItem *it = &m->items[m->count++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    snprintf(it->label, sizeof it->label, "%s", label ? label : "");
    it->id = id;
    it->checked = checked;
    it->enabled = enabled;
    return it;
}

static void sep(WsMenu *m) { add(m, WS_MI_SEPARATOR, "", 0, false, true); }
static void act(WsMenu *m, const char *l, int id, bool en) { add(m, WS_MI_ACTION, l, id, false, en); }
static void chk(WsMenu *m, const char *l, int id, bool on) { add(m, WS_MI_CHECK, l, id, on, true); }
static void rad(WsMenu *m, const char *l, int id, bool on) { add(m, WS_MI_RADIO, l, id, on, true); }
static WsMenu *sub(WsMenu *m, const char *l, bool en)
{
    WsMenuItem *it = add(m, WS_MI_SUBMENU, l, 0, false, en);
    it->sub = menu_new();
    return it->sub;
}

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

WsMenu *ws_menu_build(WsApp *a, int platform)
{
    const WsSettings *s = &a->settings;
    const bool live = ws_run_live(&a->run);
    const bool started = ws_app_started(a);
    const bool mac = platform == WS_MENU_PLATFORM_MAC;
    WsMenu *m = menu_new();

    act(m, mac ? "New/Edit Run..." : "New/Edit Run (text editor)...", WS_CMD_EDIT_RUN, true);
    act(m, "Open...", WS_CMD_OPEN, true);
    WsMenu *rec = sub(m, "Open Recent", true);
    if (s->recent_count == 0) {
        act(rec, "None", 0, false);
    } else {
        for (int i = 0; i < s->recent_count; i++) {
            WsMenuItem *it = add(rec, WS_MI_ACTION, basename_of(s->recent[i]), C_RECENT_BASE + i, false, true);
            (void)it;
        }
        sep(rec);
        act(rec, "Clear History", C_CLEAR_RECENT, true);
    }
    act(m, "Import LiveSplit Splits (.lss)...", WS_CMD_IMPORT_LSS, true);
    act(m, "Save", C_SAVE, live && a->run.file);
    act(m, "Save As...", C_SAVE_AS, live);
    act(m, "Reload", C_RELOAD, live && a->run.file);
    act(m, "Close", C_CLOSE, live);
    sep(m);
    act(m, "Start at...", WS_CMD_START_AT, !a->timer.running && !started);
    act(m, "Stop", C_STOP, started);
    act(m, "Reset", C_RESET, started);
    act(m, "Set This Run as Old", C_SET_OLD, live && ws_run_done(&a->run));
    sep(m);

    if (mac) act(m, "Settings...", WS_CMD_SETTINGS, true);

    WsMenu *d = sub(m, "Display", true);
    chk(d, "Always on Top", C_ON_TOP, s->on_top);
    chk(d, "Show Run Title", C_SHOW_TITLE, s->show_title);
    chk(d, "Show Run Goal", C_SHOW_GOAL, s->show_goal);
    chk(d, "Show Attempt Count", C_SHOW_ATTEMPTS, s->show_attempts);
    sep(d);
    rad(d, "Timer Only", C_MODE_TIMER, s->display_mode == WS_MODE_TIMER);
    rad(d, "Compact", C_MODE_COMPACT, s->display_mode == WS_MODE_COMPACT);
    rad(d, "Wide", C_MODE_WIDE, s->display_mode == WS_MODE_WIDE);
    rad(d, "Detailed", C_MODE_DETAILED, s->display_mode == WS_MODE_DETAILED);
    sep(d);
    WsMenu *ck = sub(d, "Clock Appearance", true);
    chk(ck, "Show Decimal Separator", C_DEC_SEP, s->show_decimal_separator);
    chk(ck, "Digital Clock Font", C_DIGITAL, s->digital_clock);
    chk(ck, "Accent on Horizontal Gradient", C_ACCENT, s->clock_accent);
    chk(ck, "Horizontal Gradients", C_HGRAD, s->horizontal_gradient);
    WsMenu *bg = sub(d, "Background", true);
    rad(bg, "Default", C_BG_DEFAULT, !s->background_plain && !s->background_black);
    rad(bg, "Plain", C_BG_PLAIN, s->background_plain && !s->background_black);
    rad(bg, "Black", C_BG_BLACK, s->background_black);
    WsMenu *ex = sub(d, "Detailed Mode Extras", true);
    chk(ex, "Previous Segment", C_X_PREV, s->show_prev_seg);
    chk(ex, "Possible Time Save", C_X_GAIN, s->show_time_save);
    chk(ex, "Sum of Best", C_X_SOB, s->show_sob);
    chk(ex, "Predicted Time (PB)", C_X_PREDPB, s->pred_pb);
    chk(ex, "Predicted Time (Best)", C_X_PREDBEST, s->pred_best);
    sep(ex);
    chk(ex, "Detailed: Always Show Last", C_LAST_DETAILED, s->show_last_detailed);
    chk(ex, "Detailed: Show Blank Rows", C_BLANKS_DETAILED, s->display_blank_segs);
    chk(ex, "Wide: Always Show Last", C_LAST_WIDE, s->show_last_wide);
    chk(ex, "Wide: Show Blank Segments", C_BLANKS_WIDE, s->wide_seg_blanks);
    WsMenu *ic = sub(d, "Segment Icons", true);
    rad(ic, "None", C_ICONS_0, s->segment_icons == 0);
    rad(ic, "16x16", C_ICONS_1, s->segment_icons == 1);
    rad(ic, "24x24", C_ICONS_2, s->segment_icons == 2);
    rad(ic, "32x32", C_ICONS_3, s->segment_icons == 3);
    if (!mac) {
        WsMenu *op = sub(d, "Opacity", true);
        for (int pct = 100; pct >= 40; pct -= 10) {
            char l[16];
            snprintf(l, sizeof l, "%d%%", pct);
            rad(op, l, C_OPACITY_BASE + pct / 10, (int)(s->opacity * 100 + 0.5) / 10 == pct / 10);
        }
        WsMenu *fm = sub(d, "Font", true);
        act(fm, "Segment Font Family...", WS_CMD_FONT, true);
        sep(fm);
        for (size_t i = 0; i < sizeof fontmult_vals / sizeof fontmult_vals[0]; i++) {
            char l[32];
            snprintf(l, sizeof l, "Size x%.1f", fontmult_vals[i]);
            double dd = s->font_multiplier - fontmult_vals[i];
            rad(fm, l, C_FONTMULT_BASE + (int)i, dd < 0.01 && dd > -0.01);
        }
    }

    WsMenu *cmp = sub(m, "Compare Against", true);
    rad(cmp, "Old Run", C_CMP_OLD, s->compare_against == WS_CMP_OLD);
    rad(cmp, "Personal Best", C_CMP_BEST, s->compare_against == WS_CMP_BEST);
    rad(cmp, "Fastest", C_CMP_FASTEST, s->compare_against == WS_CMP_FASTEST);
    rad(cmp, "Sum of Best Segments", C_CMP_SOB, s->compare_against == WS_CMP_SOB);

    WsMenu *tb = sub(m, "Track Best As", true);
    rad(tb, "Fastest Overall Run", C_BEST_OVERALL, s->best_as_overall);
    rad(tb, "Fastest to Each Split", C_BEST_SPLITS, !s->best_as_overall);

    if (!mac) {
        WsMenu *hk = sub(m, "Global Hotkeys", true);
        chk(hk, "Enabled", C_HK_ENABLED, s->hotkeys_enabled);
        sep(hk);
        for (int i = 0; i < WS_HK_COUNT; i++) {
            char l[200];
            snprintf(l, sizeof l, "%s: %s", ws_hotkey_labels[i], s->hotkeys[i][0] ? s->hotkeys[i] : "None");
            act(hk, l, WS_CMD_SET_HOTKEY_BASE + i, true);
        }
        sep(hk);
        act(hk, "Clear All", C_HK_CLEAR_ALL, true);

        WsMenu *opt = sub(m, "Options", true);
        chk(opt, "Reload Previous Run on Start", C_RELOAD_ON_START, s->load_most_recent);
        chk(opt, "Remember Window Position", C_SAVE_POS, s->save_window_pos);
        act(opt, "Run Start Delay...", WS_CMD_START_DELAY, live);
        WsMenu *dt = sub(opt, "Double Tap Prevention", true);
        for (size_t i = 0; i < sizeof doubletap_vals / sizeof doubletap_vals[0]; i++) {
            char l[32];
            if (doubletap_vals[i] == 0) snprintf(l, sizeof l, "Off");
            else snprintf(l, sizeof l, "%d ms", doubletap_vals[i]);
            rad(dt, l, C_DOUBLETAP_BASE + (int)i, s->double_tap_guard_ms == doubletap_vals[i]);
        }
        WsMenu *rf = sub(opt, "Display Refresh Interval", true);
        for (size_t i = 0; i < sizeof refresh_vals / sizeof refresh_vals[0]; i++) {
            char l[48];
            snprintf(l, sizeof l, "%d ms%s", refresh_vals[i], refresh_vals[i] > 42 ? " (1 decimal)" : "");
            rad(rf, l, C_REFRESH_BASE + (int)i, s->refresh_ms == refresh_vals[i]);
        }
        WsMenu *col = sub(m, "Colors", true);
        act(col, "Load Color Scheme...", WS_CMD_COLORS_LOAD, true);
        act(col, "Save Color Scheme...", WS_CMD_COLORS_SAVE, true);
        act(col, "Restore Default Colors", C_COLORS_DEFAULT, true);
    } else {
        act(m, "Set Colors...", WS_CMD_COLORS_EDIT, true);
    }
    sep(m);
    act(m, "About WSplit", WS_CMD_ABOUT, true);
    act(m, mac ? "Quit WSplit" : "Exit", WS_CMD_EXIT, true);
    return m;
}

void ws_menu_free(WsMenu *m)
{
    if (!m) return;
    for (int i = 0; i < m->count; i++) ws_menu_free(m->items[i].sub);
    free(m->items);
    free(m);
}

void ws_app_colors_changed(WsApp *a)
{
    ws_app_update_display(a);
    if (a->host.invalidate) a->host.invalidate(a->host.ud);
}

static void window_props(WsApp *a)
{
    if (a->host.apply_window_props) a->host.apply_window_props(a->host.ud);
}

int ws_menu_activate(WsApp *a, int id)
{
    WsSettings *s = &a->settings;
    if (id < WS_CMD_CORE_BASE) return id;

    if (id >= C_RECENT_BASE && id < C_RECENT_BASE + WS_MAX_RECENT) {
        int i = id - C_RECENT_BASE;
        if (i < s->recent_count && ws_app_prompt_for_save(a)) {
            char p[WS_PATH_MAX];
            snprintf(p, sizeof p, "%s", s->recent[i]);
            if (!ws_app_load_file(a, p) && a->host.message) {
                char msg[WS_PATH_MAX + 64];
                snprintf(msg, sizeof msg, "Could not open %s", p);
                a->host.message(a->host.ud, "Open", msg);
            }
        }
        return WS_CMD_NONE;
    }
    if (id >= C_OPACITY_BASE + 1 && id <= C_OPACITY_BASE + 10) {
        s->opacity = (id - C_OPACITY_BASE) / 10.0;
        window_props(a);
        return WS_CMD_NONE;
    }
    if (id >= C_DOUBLETAP_BASE && id < C_DOUBLETAP_BASE + (int)(sizeof doubletap_vals / sizeof doubletap_vals[0])) {
        s->double_tap_guard_ms = doubletap_vals[id - C_DOUBLETAP_BASE];
        return WS_CMD_NONE;
    }
    if (id >= C_REFRESH_BASE && id < C_REFRESH_BASE + (int)(sizeof refresh_vals / sizeof refresh_vals[0])) {
        s->refresh_ms = refresh_vals[id - C_REFRESH_BASE];
        if (a->host.invalidate) a->host.invalidate(a->host.ud);
        return WS_CMD_NONE;
    }
    if (id >= C_FONTMULT_BASE && id < C_FONTMULT_BASE + (int)(sizeof fontmult_vals / sizeof fontmult_vals[0])) {
        s->font_multiplier = fontmult_vals[id - C_FONTMULT_BASE];
        return WS_CMD_FONTS_CHANGED;
    }

    switch (id) {
    case C_SAVE: ws_app_save(a); break;
    case C_SAVE_AS: ws_app_save_as(a); break;
    case C_RELOAD: ws_app_reload(a); break;
    case C_CLOSE: if (ws_app_prompt_for_save(a)) ws_app_close_file(a); break;
    case C_STOP: ws_app_stop(a); break;
    case C_RESET: ws_app_reset(a); break;
    case C_SET_OLD: ws_app_set_as_old(a); break;
    case C_ON_TOP: s->on_top = !s->on_top; window_props(a); break;
    case C_SHOW_TITLE: s->show_title = !s->show_title; ws_app_layout_changed(a); break;
    case C_SHOW_GOAL: s->show_goal = !s->show_goal; ws_app_layout_changed(a); break;
    case C_SHOW_ATTEMPTS: s->show_attempts = !s->show_attempts; ws_app_layout_changed(a); break;
    case C_MODE_TIMER: ws_app_set_display(a, WS_MODE_TIMER); break;
    case C_MODE_COMPACT: ws_app_set_display(a, WS_MODE_COMPACT); break;
    case C_MODE_WIDE: ws_app_set_display(a, WS_MODE_WIDE); break;
    case C_MODE_DETAILED: ws_app_set_display(a, WS_MODE_DETAILED); break;
    case C_DEC_SEP: s->show_decimal_separator = !s->show_decimal_separator; break;
    case C_DIGITAL: s->digital_clock = !s->digital_clock; break;
    case C_ACCENT: s->clock_accent = !s->clock_accent; break;
    case C_HGRAD: s->horizontal_gradient = !s->horizontal_gradient; break;
    case C_BG_DEFAULT: s->background_plain = s->background_black = false; break;
    case C_BG_PLAIN: s->background_plain = true; s->background_black = false; break;
    case C_BG_BLACK: s->background_black = true; s->background_plain = false; break;
    case C_X_PREV: s->show_prev_seg = !s->show_prev_seg; ws_app_layout_changed(a); break;
    case C_X_GAIN: s->show_time_save = !s->show_time_save; ws_app_layout_changed(a); break;
    case C_X_SOB: s->show_sob = !s->show_sob; ws_app_layout_changed(a); break;
    case C_X_PREDPB: s->pred_pb = !s->pred_pb; ws_app_layout_changed(a); break;
    case C_X_PREDBEST: s->pred_best = !s->pred_best; ws_app_layout_changed(a); break;
    case C_LAST_DETAILED: s->show_last_detailed = !s->show_last_detailed; ws_app_layout_changed(a); break;
    case C_BLANKS_DETAILED: s->display_blank_segs = !s->display_blank_segs; ws_app_layout_changed(a); break;
    case C_LAST_WIDE: s->show_last_wide = !s->show_last_wide; ws_app_layout_changed(a); break;
    case C_BLANKS_WIDE: s->wide_seg_blanks = !s->wide_seg_blanks; ws_app_layout_changed(a); break;
    case C_ICONS_0: case C_ICONS_1: case C_ICONS_2: case C_ICONS_3:
        s->segment_icons = id - C_ICONS_0;
        ws_app_layout_changed(a);
        break;
    case C_CMP_OLD: ws_app_set_compare(a, WS_CMP_OLD); break;
    case C_CMP_BEST: ws_app_set_compare(a, WS_CMP_BEST); break;
    case C_CMP_FASTEST: ws_app_set_compare(a, WS_CMP_FASTEST); break;
    case C_CMP_SOB: ws_app_set_compare(a, WS_CMP_SOB); break;
    case C_BEST_OVERALL: s->best_as_overall = true; break;
    case C_BEST_SPLITS: s->best_as_overall = false; break;
    case C_HK_ENABLED: ws_app_toggle_hotkeys(a); break;
    case C_HK_CLEAR_ALL:
        for (int i = 0; i < WS_HK_COUNT; i++) s->hotkeys[i][0] = 0;
        if (a->host.hotkeys_changed) a->host.hotkeys_changed(a->host.ud);
        break;
    case C_RELOAD_ON_START: s->load_most_recent = !s->load_most_recent; break;
    case C_SAVE_POS: s->save_window_pos = !s->save_window_pos; break;
    case C_COLORS_DEFAULT: ws_colors_default(&s->colors); ws_app_colors_changed(a); break;
    case C_CLEAR_RECENT:
        ws_settings_clear_recent(s);
        if (a->host.recent_changed) a->host.recent_changed(a->host.ud);
        break;
    default: break;
    }
    if (a->host.invalidate) a->host.invalidate(a->host.ud);
    return WS_CMD_NONE;
}
