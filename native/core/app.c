/* Application controller: port of the WSplit form logic (WSplit.cs). */
#define _POSIX_C_SOURCE 200809L
#include "wsplit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CLOCK_MIN_W 120
#define CLOCK_MIN_H 25
#define WS_MODE_NULL ((WsDisplayMode)-1)

static void invalidate(WsApp *a)
{
    if (a->host.invalidate) a->host.invalidate(a->host.ud);
}

static WsAnswer ask(WsApp *a, const char *title, const char *msg, bool cancel)
{
    if (!a->host.ask) return WS_ANSWER_NO;
    a->modal_open = true;
    WsAnswer r = a->host.ask(a->host.ud, title, msg, cancel);
    a->modal_open = false;
    return r;
}

static void notify_recent(WsApp *a)
{
    if (a->host.recent_changed) a->host.recent_changed(a->host.ud);
}

bool ws_app_started(const WsApp *a) { return ws_sw_elapsed_ns(&a->timer) != 0; }
bool ws_app_timer_running(const WsApp *a) { return a->timer.running; }

/* ------------------------------------------------------------- init */

void ws_app_init(WsApp *a, const WsHost *host, const char *settings_path)
{
    memset(a, 0, sizeof *a);
    a->host = *host;
    snprintf(a->settings_path, sizeof a->settings_path, "%s", settings_path ? settings_path : "");
    snprintf(a->decimal_char, sizeof a->decimal_char, ".");
    ws_settings_default(&a->settings);
    if (*a->settings_path) ws_settings_load(&a->settings, a->settings_path);
    ws_run_init(&a->run);
    ws_sw_reset(&a->timer);
    a->cur_mode = WS_MODE_NULL;
    a->clock_min_w = CLOCK_MIN_W;
    a->clock_min_h = CLOCK_MIN_H;
    a->detail_w = CLOCK_MIN_W;
    a->detail_h = CLOCK_MIN_H;
    a->seg_height = 14;
    a->wide_seg_w = a->settings.wide_seg_width;
    a->clock = (WsRect){0, 0, a->settings.clock_w, a->settings.clock_h};
    a->clock_fore = a->clock_gr = a->clock_gr2 = a->clock_plain = 0xFFFFFFFF;
    ws_app_initialize_display(a);
}

void ws_app_shutdown(WsApp *a)
{
    snprintf(a->settings.last_file, sizeof a->settings.last_file, "%s", a->run.file ? a->run.file : "");
    if (*a->settings_path) ws_settings_save(&a->settings, a->settings_path);
}

static void free_icons(WsApp *a)
{
    for (int i = 0; i < a->run.count; i++) {
        if (a->run.segs[i].icon && a->host.free_icon) a->host.free_icon(a->host.ud, a->run.segs[i].icon);
        a->run.segs[i].icon = NULL;
    }
}

void ws_app_reload_icons(WsApp *a)
{
    free_icons(a);
    if (!a->host.load_icon) return;
    for (int i = 0; i < a->run.count; i++) {
        const char *p = a->run.segs[i].icon_path;
        a->run.segs[i].icon = (p && *p) ? a->host.load_icon(a->host.ud, p) : NULL;
    }
}

void ws_app_free(WsApp *a)
{
    free_icons(a);
    ws_run_free(&a->run);
}

/* ----------------------------------------------------------- layout */

static int title_offset(const WsApp *a)
{
    bool t = a->run.title[0] && a->settings.show_title;
    bool g = a->run.goal[0] && a->settings.show_goal;
    if (t && g) return 32;
    if (t || g) return 18;
    return 0;
}

static int detail_seg_count(const WsApp *a)
{
    int n = a->settings.display_segs;
    if (!a->settings.display_blank_segs && a->run.count < n) n = a->run.count;
    return n;
}

static int wide_seg_count(const WsApp *a)
{
    int n = a->settings.wide_segs;
    if (!a->settings.wide_seg_blanks && a->run.count < n) n = a->run.count;
    return n;
}

static void clamp_clock(WsApp *a)
{
    if (a->clock.h < a->clock_min_h || a->clock.w < a->clock_min_w) {
        a->clock.w = a->clock_min_w;
        a->clock.h = a->clock_min_h;
    }
}

static void resize_to(WsApp *a, int w, int h)
{
    a->win_w = w;
    a->win_h = h;
    if (a->host.resize_window) a->host.resize_window(a->host.ud, w, h);
}

static void display_timer(WsApp *a)
{
    a->clock.x = a->clock.y = 0;
    a->clock_min_w = CLOCK_MIN_W;
    a->clock_min_h = CLOCK_MIN_H;
    if (a->cur_mode != WS_MODE_TIMER) { a->clock.w = a->settings.clock_w; a->clock.h = a->settings.clock_h; }
    clamp_clock(a);
    a->cur_mode = WS_MODE_TIMER;
    resize_to(a, a->clock.w, a->clock.h);
}

static int icon_extra(const WsApp *a) { return (a->settings.segment_icons + 1) * 8; }

static void display_compact(WsApp *a)
{
    a->clock.x = 0;
    a->clock.y = 15;
    a->clock_min_w = CLOCK_MIN_W;
    a->clock_min_h = CLOCK_MIN_H;
    if (a->settings.segment_icons > 1) a->clock_min_w = CLOCK_MIN_W + icon_extra(a) + 6;
    if (a->cur_mode != WS_MODE_COMPACT) {
        a->clock.w = a->settings.clock_w + (a->clock_min_w - CLOCK_MIN_W);
        a->clock.h = a->settings.clock_h;
    }
    clamp_clock(a);
    a->cur_mode = WS_MODE_COMPACT;
    resize_to(a, a->clock.w, a->clock.h + 31);
}

static int detail_extras_height(const WsApp *a)
{
    const WsSettings *s = &a->settings;
    return 18 * (s->show_prev_seg + s->show_time_save + s->show_sob + s->pred_pb + s->pred_best);
}

static void display_detail(WsApp *a)
{
    a->seg_height = icon_extra(a) > 14 ? icon_extra(a) : 14;
    a->clock_min_w = CLOCK_MIN_W;
    a->clock_min_h = CLOCK_MIN_H;
    if (a->cur_mode != WS_MODE_DETAILED) { a->clock.w = a->detail_w; a->clock.h = a->detail_h; }
    clamp_clock(a);
    int hh = detail_extras_height(a);
    int height = a->clock.h + detail_seg_count(a) * a->seg_height + 21 + title_offset(a) + hh - 18;
    a->clock.x = 0;
    a->clock.y = height - a->clock.h - hh;
    a->cur_mode = WS_MODE_DETAILED;
    resize_to(a, a->clock.w, height);
}

static void display_wide(WsApp *a)
{
    a->wide_seg_w = a->settings.wide_seg_width + (a->settings.segment_icons >= 1 ? icon_extra(a) : 0);
    a->clock_min_w = CLOCK_MIN_W;
    a->clock_min_h = a->settings.segment_icons == 3 ? 32 : a->settings.wide_height;
    a->clock.x = a->clock.y = 0;
    if (a->cur_mode != WS_MODE_WIDE || a->clock.h != a->clock_min_h || a->clock.w < a->clock_min_w) {
        a->clock.w = a->clock_min_w;
        a->clock.h = a->clock_min_h;
    }
    a->cur_mode = WS_MODE_WIDE;
    resize_to(a, a->clock.w + 124 + wide_seg_count(a) * a->wide_seg_w, a->clock.h);
}

void ws_app_set_display(WsApp *a, WsDisplayMode mode)
{
    a->settings.display_mode = mode;
    if (ws_run_live(&a->run)) {
        switch (mode) {
        case WS_MODE_COMPACT: display_compact(a); break;
        case WS_MODE_WIDE: display_wide(a); break;
        case WS_MODE_DETAILED: display_detail(a); break;
        default: display_timer(a); break;
        }
    } else {
        display_timer(a);
    }
    invalidate(a);
}

void ws_app_relayout(WsApp *a)
{
    WsDisplayMode m = (WsDisplayMode)a->settings.display_mode;
    ws_app_set_display(a, m);
}

/* Force a full layout recompute (e.g. icon size changed). */
static void relayout_hard(WsApp *a)
{
    WsDisplayMode m = (WsDisplayMode)a->settings.display_mode;
    if (a->cur_mode == WS_MODE_DETAILED) { a->detail_w = a->clock.w; a->detail_h = a->clock.h; }
    a->cur_mode = WS_MODE_NULL;
    ws_app_set_display(a, m);
}

void ws_app_segs_dec(WsApp *a)
{
    if (a->cur_mode == WS_MODE_WIDE) {
        if (a->settings.wide_segs > 1) {
            if (!a->settings.wide_seg_blanks && a->settings.wide_segs > a->run.count)
                a->settings.wide_segs = a->run.count > 1 ? a->run.count - 1 : 1;
            else
                a->settings.wide_segs--;
        }
    } else if (a->settings.display_segs > 2) {
        if (!a->settings.display_blank_segs && a->settings.display_segs > a->run.count)
            a->settings.display_segs = a->run.count - 1 < 2 ? 2 : a->run.count - 1;
        else
            a->settings.display_segs--;
    }
    ws_app_relayout(a);
    ws_app_update_display(a);
}

void ws_app_segs_inc(WsApp *a)
{
    if (a->cur_mode == WS_MODE_WIDE) {
        int max = a->settings.wide_seg_blanks ? 20 : (a->run.count < 20 ? a->run.count : 20);
        if (a->settings.wide_segs < max) a->settings.wide_segs++;
    } else {
        int max = a->settings.display_blank_segs ? 40 : (a->run.count < 40 ? a->run.count : 40);
        if (a->settings.display_segs < max) a->settings.display_segs++;
    }
    ws_app_relayout(a);
    ws_app_update_display(a);
}

void ws_app_user_resized(WsApp *a, int w, int h)
{
    int dw = w - a->win_w, dh = h - a->win_h;
    if (dw == 0 && dh == 0) return;
    a->clock.w += dw;
    if (a->cur_mode != WS_MODE_WIDE) a->clock.h += dh;
    if (a->clock.w < a->clock_min_w) a->clock.w = a->clock_min_w;
    if (a->clock.h < a->clock_min_h) a->clock.h = a->clock_min_h;

    if (a->cur_mode == WS_MODE_DETAILED) {
        a->detail_w = a->clock.w;
        a->detail_h = a->clock.h;
        if (ws_run_live(&a->run)) { a->run.pref_w = a->detail_w; a->run.pref_h = a->detail_h; }
    } else if (a->cur_mode == WS_MODE_TIMER || a->cur_mode == WS_MODE_COMPACT) {
        int cw = a->clock.w;
        if (a->settings.segment_icons > 1 && a->cur_mode == WS_MODE_COMPACT) cw -= a->clock_min_w - CLOCK_MIN_W;
        a->settings.clock_w = cw;
        a->settings.clock_h = a->clock.h;
    }
    ws_app_relayout(a);
}

/* ---------------------------------------------------- display update */

void ws_app_update_detailed(WsApp *a)
{
    const WsColors *c = &a->settings.colors;
    WsRun *r = &a->run;
    bool started = ws_app_started(a);
    for (int i = 0; i <= ws_run_last(r); i++) {
        WsSegment *s = &r->segs[i];
        if (i < r->live_index && started) {
            double segd = ws_run_seg_delta(r, s->live_time, i);
            double rund = ws_run_run_delta(r, s->live_time, i);
            double ls = ws_run_live_segment(r, i);
            bool best_seg = ls != 0.0 && (s->best_segment == 0.0 || ls < s->best_segment);
            if (s->live_time > 0.0 && ws_run_comp_time_at(r, i) > 0.0) {
                ws_format_time(s->time_string, sizeof s->time_string, ws_run_run_delta_at(r, i), WS_FMT_DELTA);
                if (best_seg) s->time_color = c->SegBestSegment;
                else if (rund < 0.0) s->time_color = segd < 0.0 ? c->SegAheadGain : c->SegAheadLoss;
                else s->time_color = segd > 0.0 ? c->SegBehindLoss : c->SegBehindGain;
            } else if (s->live_time == 0.0) {
                snprintf(s->time_string, sizeof s->time_string, "-");
                s->time_color = c->SegMissingTime;
            } else {
                ws_format_time(s->time_string, sizeof s->time_string, s->live_time, WS_FMT_SHORT);
                s->time_color = best_seg ? c->SegBestSegment : c->SegNewTime;
            }
        } else {
            s->time_color = (i == r->live_index) ? c->LiveSeg : c->FutureSegTime;
            double ct = ws_run_comp_time_at(r, i);
            if (ct > 0.0) ws_format_time(s->time_string, sizeof s->time_string, ct, WS_FMT_SHORT);
            else snprintf(s->time_string, sizeof s->time_string, "-");
        }
    }
    invalidate(a);
}

void ws_app_update_display(WsApp *a)
{
    a->run.comp = (WsCompare)a->settings.compare_against;
    ws_app_update_detailed(a);
}

void ws_app_initialize_display(WsApp *a)
{
    a->delay_active = false;
    ws_run_reset(&a->run);
    if (!ws_run_live(&a->run)) ws_run_set_file(&a->run, NULL);
    a->refreshing = false;
    ws_sw_reset(&a->timer);
    ws_app_update_display(a);
    ws_app_set_display(a, (WsDisplayMode)a->settings.display_mode);
}

/* ----------------------------------------------------------- files */

void ws_app_close_file(WsApp *a)
{
    free_icons(a);
    ws_run_clear(&a->run);
    ws_run_set_title(&a->run, "");
    ws_run_set_goal(&a->run, "");
    ws_run_set_file(&a->run, NULL);
    a->run.attempts = 0;
    a->run.start_delay_ms = 0;
    a->run.unsaved = false;
    a->detail_w = a->clock_min_w;
    a->detail_h = a->clock_min_h;
    ws_app_initialize_display(a);
}

bool ws_app_load_file(WsApp *a, const char *path)
{
    char p[WS_PATH_MAX];
    snprintf(p, sizeof p, "%s", path ? path : "");
    free_icons(a);
    if (!*p || !ws_runfile_load(&a->run, p)) {
        ws_app_close_file(a);
        return false;
    }
    a->detail_w = a->run.pref_w > CLOCK_MIN_W ? a->run.pref_w : CLOCK_MIN_W;
    a->detail_h = a->run.pref_h > CLOCK_MIN_H ? a->run.pref_h : CLOCK_MIN_H;
    ws_app_reload_icons(a);
    a->cur_mode = WS_MODE_NULL;
    ws_app_initialize_display(a);
    ws_settings_add_recent(&a->settings, p);
    notify_recent(a);
    a->run.unsaved = false;
    return true;
}

void ws_app_reload(WsApp *a)
{
    if (a->run.file) {
        char p[WS_PATH_MAX];
        snprintf(p, sizeof p, "%s", a->run.file);
        ws_app_load_file(a, p);
    }
}

static bool save_file(WsApp *a)
{
    if (!a->run.file) return false;
    ws_run_update_best(&a->run, a->settings.best_as_overall);
    if (a->cur_mode == WS_MODE_DETAILED) { a->detail_w = a->clock.w; a->detail_h = a->clock.h; }
    a->run.pref_w = a->detail_w;
    a->run.pref_h = a->detail_h;
    char err[WS_PATH_MAX + 128];
    if (!ws_runfile_save(&a->run, a->run.file, err, sizeof err)) {
        if (a->host.message) {
            a->modal_open = true;
            a->host.message(a->host.ud, "Save error", err);
            a->modal_open = false;
        }
        return false;
    }
    a->run.unsaved = false;
    ws_settings_add_recent(&a->settings, a->run.file);
    notify_recent(a);
    ws_app_update_display(a);
    return true;
}

bool ws_app_save_as(WsApp *a)
{
    if (!ws_run_live(&a->run) || !a->host.choose_save_path) return false;
    char p[WS_PATH_MAX] = "";
    a->modal_open = true;
    bool ok = a->host.choose_save_path(a->host.ud, p, sizeof p);
    a->modal_open = false;
    if (!ok || !*p) return false;
    ws_run_set_file(&a->run, p);
    return save_file(a);
}

bool ws_app_save(WsApp *a)
{
    if (!ws_run_live(&a->run)) return false;
    if (!a->run.file) return ws_app_save_as(a);
    return save_file(a);
}

bool ws_app_prompt_for_save(WsApp *a)
{
    if (a->run.unsaved) {
        WsAnswer r = ask(a, "Save splits?",
                         "Your splits have been updated but not yet saved.\nDo you want to save your splits now?", true);
        if (r == WS_ANSWER_CANCEL) return false;
        if (r == WS_ANSWER_YES) ws_app_save(a);
        a->run.unsaved = false;
    } else if (ws_run_need_update(&a->run, a->settings.best_as_overall)) {
        WsAnswer r = ask(a, "Save splits?",
                         "You have beaten some of your best times.\nDo you want to update and save your splits now?", true);
        if (r == WS_ANSWER_CANCEL) return false;
        if (r == WS_ANSWER_YES) ws_app_save(a);
    }
    return true;
}

void ws_app_run_edited(WsApp *a)
{
    ws_run_recompute_old_faster(&a->run);
    ws_app_reload_icons(a);
    a->cur_mode = WS_MODE_NULL;
    ws_app_initialize_display(a);
    a->run.unsaved = true;
}

/* ----------------------------------------------------------- timer */

static void start_timer(WsApp *a, int64_t offset_ns, bool use_delay)
{
    if (a->delay_active) return;
    if (use_delay && a->run.start_delay_ms > 0) {
        a->delay_active = true;
        a->delay_start_ns = ws_now_ns();
        a->delay_offset_ns = offset_ns;
    } else {
        if (ws_run_live(&a->run)) a->run.attempts++;
        ws_sw_start_at(&a->timer, offset_ns);
    }
    invalidate(a);
}

void ws_app_pause_resume(WsApp *a)
{
    if (a->refreshing) {
        a->refreshing = false;
        if (a->delay_active) ws_app_initialize_display(a);
        ws_sw_stop(&a->timer);
        invalidate(a);
    } else if (!ws_run_done(&a->run) && !a->delay_active) {
        if (!ws_app_started(a)) {
            if (!a->timer.running) ws_app_initialize_display(a);
            start_timer(a, 0, true);
        } else {
            ws_sw_start(&a->timer);
        }
        a->refreshing = true;
        invalidate(a);
    }
}

static void do_split(WsApp *a)
{
    double t = trunc(ws_sw_elapsed_s(&a->timer) * 100.0) / 100.0;
    ws_run_do_split(&a->run, t);
    if (ws_run_done(&a->run)) a->refreshing = false;
    ws_app_update_display(a);
}

void ws_app_stop(WsApp *a)
{
    if (ws_run_need_update(&a->run, a->settings.best_as_overall)) {
        ws_run_update_best(&a->run, a->settings.best_as_overall);
        a->run.unsaved = true;
    }
    ws_app_initialize_display(a);
}

void ws_app_timer_control(WsApp *a)
{
    int64_t now = ws_now_ns();
    if (a->settings.double_tap_guard_ms > 0 && a->last_tap_ns &&
        now - a->last_tap_ns < (int64_t)a->settings.double_tap_guard_ms * 1000000LL)
        return;
    a->last_tap_ns = now;

    if (ws_run_live(&a->run)) {
        if (a->refreshing && !a->delay_active) do_split(a);
        else if (ws_run_done(&a->run)) ws_app_stop(a);
        else ws_app_pause_resume(a);
    } else {
        ws_app_pause_resume(a);
    }
}

void ws_app_reset(WsApp *a)
{
    if (ws_run_need_update(&a->run, a->settings.best_as_overall)) {
        WsAnswer r = ask(a, "Update times?", "You have beaten some of your best times.\nDo you want to update them?", true);
        if (r == WS_ANSWER_YES) {
            ws_run_update_best(&a->run, a->settings.best_as_overall);
            a->run.unsaved = true;
        }
        if (r != WS_ANSWER_CANCEL) ws_app_initialize_display(a);
    } else {
        ws_app_initialize_display(a);
    }
}

void ws_app_prev(WsApp *a)
{
    if (a->run.live_index >= 1) {
        if (a->run.live_index > ws_run_last(&a->run)) a->refreshing = true;
        ws_run_previous(&a->run);
        ws_app_update_display(a);
    }
}

void ws_app_next(WsApp *a)
{
    ws_run_next(&a->run);
    ws_app_update_display(a);
}

void ws_app_set_compare(WsApp *a, WsCompare c)
{
    a->settings.compare_against = c;
    ws_app_update_display(a);
}

void ws_app_switch_comparison(WsApp *a)
{
    switch (a->settings.compare_against) {
    case WS_CMP_OLD: ws_app_set_compare(a, WS_CMP_BEST); break;
    case WS_CMP_SOB: ws_app_set_compare(a, WS_CMP_OLD); break;
    default: ws_app_set_compare(a, WS_CMP_SOB); break;
    }
}

void ws_app_toggle_hotkeys(WsApp *a)
{
    a->settings.hotkeys_enabled = !a->settings.hotkeys_enabled;
    if (a->host.hotkeys_changed) a->host.hotkeys_changed(a->host.ud);
    invalidate(a);
}

void ws_app_set_as_old(WsApp *a)
{
    ws_run_live_to_old(&a->run);
    a->run.unsaved = true;
    ws_app_update_display(a);
}

bool ws_app_start_at(WsApp *a, double seconds, bool use_delay)
{
    if (a->timer.running || a->refreshing) return false;
    if (ws_run_done(&a->run) || a->delay_active) return false;
    if (ws_app_started(a)) return false; /* paused */
    ws_app_initialize_display(a);
    start_timer(a, (int64_t)llround(seconds * 1e9), use_delay);
    a->refreshing = true;
    return true;
}

void ws_app_hotkey(WsApp *a, WsHotkeyAction act)
{
    if (a->modal_open) return;
    if (act != WS_HK_TOGGLE && !a->settings.hotkeys_enabled) return;
    switch (act) {
    case WS_HK_SPLIT: ws_app_timer_control(a); break;
    case WS_HK_PAUSE: ws_app_pause_resume(a); break;
    case WS_HK_STOP: ws_app_stop(a); break;
    case WS_HK_RESET: ws_app_reset(a); break;
    case WS_HK_PREV: ws_app_prev(a); break;
    case WS_HK_NEXT: ws_app_next(a); break;
    case WS_HK_COMPARE: ws_app_switch_comparison(a); break;
    case WS_HK_TOGGLE: ws_app_toggle_hotkeys(a); break;
    default: break;
    }
}

bool ws_app_local_key(WsApp *a, const char *k)
{
    if (a->modal_open) return false;
    if (!strcasecmp(k, "Space") || !strcasecmp(k, "Enter") || !strcasecmp(k, "Return")) ws_app_timer_control(a);
    else if (!strcasecmp(k, "Left")) ws_app_prev(a);
    else if (!strcasecmp(k, "Right")) ws_app_next(a);
    else if (!strcasecmp(k, "Up")) ws_app_segs_inc(a);
    else if (!strcasecmp(k, "Down")) ws_app_segs_dec(a);
    else if (!strcmp(k, "0")) ws_app_set_display(a, WS_MODE_TIMER);
    else if (!strcmp(k, "1")) ws_app_set_display(a, WS_MODE_COMPACT);
    else if (!strcmp(k, "2")) ws_app_set_display(a, WS_MODE_WIDE);
    else if (!strcmp(k, "3")) ws_app_set_display(a, WS_MODE_DETAILED);
    else if (!strcasecmp(k, "P")) ws_app_pause_resume(a);
    else if (!strcasecmp(k, "R")) ws_app_reset(a);
    else if (!strcasecmp(k, "S")) ws_app_stop(a);
    else if (!strcasecmp(k, "Tab")) ws_app_switch_comparison(a);
    else return false;
    return true;
}

bool ws_app_tick(WsApp *a)
{
    if (a->delay_active) {
        int64_t now = ws_now_ns();
        if (now - a->delay_start_ns >= (int64_t)a->run.start_delay_ms * 1000000LL) {
            a->delay_active = false;
            a->run.attempts++;
            ws_sw_start_at(&a->timer, a->delay_offset_ns);
            ws_app_update_display(a);
        }
        return true;
    }
    return a->refreshing;
}

/* exposed for the settings dialogs */
void ws_app_layout_changed(WsApp *a) { relayout_hard(a); ws_app_update_display(a); }
