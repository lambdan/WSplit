/* Port of Segment.cs, Split.cs, DualStopwatch.cs and the time helpers. */
#define _POSIX_C_SOURCE 200809L
#include "wsplit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

char *ws_strdup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void set_str(char **dst, const char *s)
{
    char *n = ws_strdup(s);
    free(*dst);
    *dst = n;
}

/* ---------------------------------------------------------------- run */

void ws_run_init(WsRun *r)
{
    memset(r, 0, sizeof *r);
    r->title = ws_strdup("");
    r->goal = ws_strdup("");
}

static void seg_free(WsSegment *s)
{
    free(s->name);
    free(s->icon_path);
    s->name = s->icon_path = NULL;
}

void ws_run_free(WsRun *r)
{
    for (int i = 0; i < r->count; i++) seg_free(&r->segs[i]);
    free(r->segs);
    free(r->title);
    free(r->goal);
    free(r->file);
    memset(r, 0, sizeof *r);
}

void ws_run_clear(WsRun *r)
{
    for (int i = 0; i < r->count; i++) seg_free(&r->segs[i]);
    r->count = 0;
    ws_run_reset(r);
}

void ws_run_set_title(WsRun *r, const char *s) { set_str(&r->title, s); }
void ws_run_set_goal(WsRun *r, const char *s) { set_str(&r->goal, s); }
void ws_run_set_file(WsRun *r, const char *s)
{
    if (!s) { free(r->file); r->file = NULL; return; }
    set_str(&r->file, s);
}
void ws_seg_set_name(WsSegment *s, const char *name) { set_str(&s->name, name); }
void ws_seg_set_icon_path(WsSegment *s, const char *p) { set_str(&s->icon_path, p); }

static bool last_old_faster(const WsSegment *s)
{
    /* oldFaster = !(BestTime != 0 && (BestTime < OldTime || OldTime == 0)) */
    return !((s->best_time != 0.0) && ((s->best_time < s->old_time) || (s->old_time == 0.0)));
}

WsSegment *ws_run_add(WsRun *r, const char *name, double old_t, double best_t, double best_seg)
{
    if (r->count == r->cap) {
        int nc = r->cap ? r->cap * 2 : 16;
        WsSegment *n = realloc(r->segs, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        r->segs = n;
        r->cap = nc;
    }
    WsSegment *s = &r->segs[r->count++];
    memset(s, 0, sizeof *s);
    s->name = ws_strdup(name);
    s->icon_path = ws_strdup("");
    s->old_time = old_t;
    s->best_time = best_t;
    s->backup_best = best_t;
    s->best_segment = best_seg;
    s->backup_best_segment = best_seg;
    s->time_color = 0xFFFFFFFF;
    r->old_faster = last_old_faster(s);
    return s;
}

void ws_run_remove(WsRun *r, int index)
{
    if (index < 0 || index >= r->count) return;
    seg_free(&r->segs[index]);
    memmove(&r->segs[index], &r->segs[index + 1], (size_t)(r->count - index - 1) * sizeof(WsSegment));
    r->count--;
}

void ws_run_recompute_old_faster(WsRun *r)
{
    if (r->count > 0) r->old_faster = last_old_faster(&r->segs[r->count - 1]);
}

double ws_run_comp_time_at(const WsRun *r, int index)
{
    if (index < 0 || index > ws_run_last(r)) return 0.0;
    if (r->comp != WS_CMP_BEST) {
        if (r->comp == WS_CMP_OLD) return r->segs[index].old_time;
        if (r->comp == WS_CMP_SOB) return ws_run_sum_of_bests(r, index);
        if (r->old_faster) return r->segs[index].old_time;
    }
    return r->segs[index].best_time;
}

double ws_run_comp_time(const WsRun *r)
{
    int i = r->live_index < ws_run_last(r) ? r->live_index : ws_run_last(r);
    return ws_run_comp_time_at(r, i);
}

double ws_run_sum_of_bests(const WsRun *r, int index)
{
    double sum = 0.0;
    for (int i = 0; i <= index && i < r->count; ++i) {
        if (r->segs[i].best_segment == 0.0) return 0.0;
        sum += r->segs[i].best_segment;
    }
    return sum;
}

void ws_run_do_split(WsRun *r, double time)
{
    if (ws_run_live(r) && !ws_run_done(r)) {
        int last = ws_run_last(r);
        r->segs[r->live_index].live_time = time;
        if (r->live_index == last &&
            (r->segs[last].live_time < r->segs[last].best_time || r->segs[last].best_time == 0.0))
            r->new_best_time = true;
        r->live_index++;
    }
}

double ws_run_last_delta(const WsRun *r, int index)
{
    if (index > 0 && index <= ws_run_last(r)) {
        for (int i = index - 1; i >= 0; i--) {
            if (r->segs[i].live_time != 0.0 && ws_run_comp_time_at(r, i) != 0.0)
                return r->segs[i].live_time - ws_run_comp_time_at(r, i);
        }
    }
    return 0.0;
}

double ws_run_live_segment(const WsRun *r, int index)
{
    if (index >= 0 && index <= ws_run_last(r)) {
        double lt = r->segs[index].live_time;
        if (index <= 0 || lt <= 0.0) return lt;
        if (r->segs[index - 1].live_time > 0.0) return lt - r->segs[index - 1].live_time;
    }
    return 0.0;
}

void ws_run_live_to_old(WsRun *r)
{
    for (int i = 0; i < r->count; i++) r->segs[i].old_time = r->segs[i].live_time;
}

void ws_run_next(WsRun *r)
{
    if (r->live_index < ws_run_last(r)) ws_run_do_split(r, 0.0);
}

void ws_run_previous(WsRun *r)
{
    r->live_index--;
    if (ws_run_live(r) && !ws_run_done(r) && r->live_index >= 0)
        r->segs[r->live_index].live_time = 0.0;
    r->new_best_time = false;
}

void ws_run_reset(WsRun *r)
{
    for (int i = 0; i < r->count; i++) {
        WsSegment *s = &r->segs[i];
        s->backup_best = s->best_time;
        s->backup_best_segment = s->best_segment;
        s->live_time = 0.0;
    }
    r->live_index = 0;
    r->new_best_time = false;
}

void ws_run_restore_best(WsRun *r)
{
    for (int i = 0; i < r->count; i++) {
        r->segs[i].best_time = r->segs[i].backup_best;
        r->segs[i].best_segment = r->segs[i].backup_best_segment;
    }
    ws_run_recompute_old_faster(r);
}

double ws_run_run_delta(const WsRun *r, double time, int index)
{
    double c = ws_run_comp_time_at(r, index);
    if (c > 0.0 && time > 0.0) return time - c;
    return 0.0;
}

double ws_run_run_delta_at(const WsRun *r, int index)
{
    if (index >= 0 && index <= ws_run_last(r)) return ws_run_run_delta(r, r->segs[index].live_time, index);
    return 0.0;
}

double ws_run_seg_delta(const WsRun *r, double time, int index)
{
    if (ws_run_comp_time_at(r, index) > 0.0) return ws_run_run_delta(r, time, index) - ws_run_last_delta(r, index);
    return 0.0;
}

bool ws_run_need_update(const WsRun *r, bool best_overall)
{
    if (r->new_best_time) return true;
    for (int i = 0; i <= ws_run_last(r); ++i) {
        const WsSegment *s = &r->segs[i];
        if ((!best_overall && s->live_time != 0.0 && (s->live_time < s->best_time || s->best_time == 0.0)) ||
            (s->live_time != 0.0 && (i == 0 || r->segs[i - 1].live_time != 0.0) &&
             (s->best_segment == 0.0 || ws_run_live_segment(r, i) < s->best_segment)))
            return true;
    }
    return false;
}

void ws_run_update_best(WsRun *r, bool best_overall)
{
    int last = ws_run_last(r);
    for (int i = 0; i < r->count; i++) {
        WsSegment *s = &r->segs[i];
        if ((best_overall && r->new_best_time) ||
            (!best_overall && s->live_time != 0.0 && (s->live_time < s->best_time || s->best_time == 0.0)))
            s->best_time = s->live_time;
    }

    /* Get rid of incoherences */
    double best = 0.0;
    for (int i = last; i >= 0; i--) {
        if (r->segs[i].best_time != 0.0) {
            if (best == 0.0) best = r->segs[last].best_time;
            if (r->segs[i].best_time > best)
                r->segs[i].best_time = 0.0;
            else
                best = r->segs[i].best_time;
        }
    }

    for (int j = 0; j <= last; j++) {
        WsSegment *s = &r->segs[j];
        if (s->live_time != 0.0 && (j == 0 || r->segs[j - 1].live_time != 0.0) &&
            (ws_run_live_segment(r, j) < s->best_segment || s->best_segment == 0.0))
            s->best_segment = ws_run_live_segment(r, j);
    }
    ws_run_recompute_old_faster(r);
    r->new_best_time = false;
}

WsCompare ws_run_comparing_type(const WsRun *r)
{
    if (r->comp != WS_CMP_FASTEST) return r->comp;
    return r->old_faster ? WS_CMP_OLD : WS_CMP_BEST;
}

const char *ws_compare_name(WsCompare c)
{
    switch (c) {
    case WS_CMP_OLD: return "Old";
    case WS_CMP_BEST: return "Best";
    case WS_CMP_SOB: return "SumOfBests";
    default: return "Fastest";
    }
}

void ws_run_autofill_best_segments(WsRun *r)
{
    for (int i = 0; i < r->count; ++i) {
        double seg = 0.0;
        if (i == 0)
            seg = r->segs[i].best_time;
        else if (r->segs[i].best_time != 0.0 && r->segs[i - 1].best_time != 0.0)
            seg = r->segs[i].best_time - r->segs[i - 1].best_time;
        if (r->segs[i].best_segment == 0.0 || (seg != 0.0 && seg < r->segs[i].best_segment))
            r->segs[i].best_segment = seg;
    }
}

/* ---------------------------------------------------------- stopwatch */

int64_t ws_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void ws_sw_reset(WsStopwatch *sw) { memset(sw, 0, sizeof *sw); }

void ws_sw_start(WsStopwatch *sw)
{
    if (sw->running) return;
    sw->start_ns = ws_now_ns();
    sw->running = true;
}

void ws_sw_start_at(WsStopwatch *sw, int64_t offset_ns)
{
    if (!sw->running && sw->accumulated_ns == 0) sw->started_at_ns = offset_ns;
    ws_sw_start(sw);
}

void ws_sw_stop(WsStopwatch *sw)
{
    if (!sw->running) return;
    sw->accumulated_ns += ws_now_ns() - sw->start_ns;
    sw->running = false;
}

int64_t ws_sw_elapsed_ns(const WsStopwatch *sw)
{
    int64_t e = sw->accumulated_ns;
    if (sw->running) e += ws_now_ns() - sw->start_ns;
    if (e == 0 && !sw->running) return 0; /* not started: ElapsedTicks == 0 */
    return e + sw->started_at_ns;
}

/* ------------------------------------------------------- time format */

/* Equivalent of TimeSpan.FromSeconds (rounds to the millisecond). */
static int64_t span_ms(double secs) { return (int64_t)llround(secs * 1000.0); }

void ws_format_time(char *out, size_t n, double secs, WsTimeFormat fmt)
{
    int64_t ms = span_ms(fabs(trunc(secs * 10.0) / 10.0));
    const char *sign = "";
    if ((fmt == WS_FMT_DELTA || fmt == WS_FMT_DELTA_SHORT) && secs >= 0.0)
        sign = "+";
    else if (secs < 0.0)
        sign = "-";

    if (fmt == WS_FMT_SECONDS) {
        snprintf(out, n, "%.15g", trunc(secs * 100.0) / 100.0);
        return;
    }

    if (fmt == WS_FMT_LONG) {
        int64_t h = ms / 3600000, m = (ms / 60000) % 60;
        double s = (double)((ms / 1000) % 60) + (double)(ms % 1000) / 1000.0;
        if (ms >= 3600000)
            snprintf(out, n, "%s%lld:%02lld:%04.1f", sign, (long long)h, (long long)m, s);
        else
            snprintf(out, n, "%s%lld:%04.1f", sign, (long long)m, s);
        return;
    }

    if (ms >= 60000 || fmt == WS_FMT_SHORT) ms = span_ms(fabs(trunc(secs)));
    if (ms >= 6000000 && fmt == WS_FMT_DELTA_SHORT) ms = (99 * 60 + 59) * 1000LL;
    if (ms >= 360000000LL) ms = (ms / 60000) * 60000;

    int64_t th = ms / 3600000, tm = ms / 60000, mm = (ms / 60000) % 60, ss = (ms / 1000) % 60;
    if (ms >= 360000000LL && fmt != WS_FMT_DELTA_SHORT) {
        snprintf(out, n, "%s%lldh%02lld", sign, (long long)th, (long long)mm);
        return;
    }
    if (ms >= 3600000 && fmt != WS_FMT_DELTA_SHORT) {
        snprintf(out, n, "%s%lld:%02lld:%02lld", sign, (long long)th, (long long)mm, (long long)ss);
        return;
    }
    if (ms >= 60000 || fmt == WS_FMT_SHORT) {
        snprintf(out, n, "%s%lld:%02lld", sign, (long long)tm, (long long)ss);
        return;
    }
    snprintf(out, n, "%s%.1f", sign, (double)ms / 1000.0);
}

void ws_format_edit_time(char *out, size_t n, double secs)
{
    int64_t ms = span_ms(trunc(secs * 100.0) / 100.0);
    if (ms < 0) ms = 0;
    double s = (double)((ms / 1000) % 60) + (double)(ms % 1000) / 1000.0;
    if (ms >= 3600000)
        snprintf(out, n, "%lld:%02lld:%05.2f", (long long)(ms / 3600000), (long long)((ms / 60000) % 60), s);
    else if (ms >= 60000)
        snprintf(out, n, "%lld:%05.2f", (long long)(ms / 60000), s);
    else
        snprintf(out, n, "%.2f", (double)ms / 1000.0);
}

double ws_parse_double(const char *s, bool *ok)
{
    /* NumberStyles.AllowDecimalPoint: digits with optional single '.', no sign. */
    double ip = 0.0, fp = 0.0, scale = 1.0;
    bool any = false, dot = false;
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            any = true;
            if (dot) { scale /= 10.0; fp += (*p - '0') * scale; }
            else ip = ip * 10.0 + (*p - '0');
        } else if ((*p == '.' || *p == ',') && !dot) {
            dot = true;
        } else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            break;
        } else {
            if (ok) *ok = false;
            return 0.0;
        }
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p) { if (ok) *ok = false; return 0.0; }
    if (ok) *ok = any;
    return any ? ip + fp : 0.0;
}

void ws_print_double(char *out, size_t n, double v)
{
    /* .NET's default double.ToString() gives up to 15 significant digits. */
    snprintf(out, n, "%.15g", v);
    /* never emit locale commas */
    for (char *p = out; *p; p++) if (*p == ',') *p = '.';
}

double ws_parse_time(const char *s)
{
    double num = 0.0;
    char buf[64];
    const char *p = s;
    while (1) {
        const char *c = strchr(p, ':');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, p, len);
        buf[len] = 0;
        bool ok = false;
        double v = ws_parse_double(buf, &ok);
        if (ok) num = num * 60.0 + v;
        if (!c) break;
        p = c + 1;
    }
    return num;
}
