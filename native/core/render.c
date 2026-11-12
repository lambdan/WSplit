/* Renderer: port of WSplit.Painter (PaintAll / DrawBackground / DrawClock*). */
#define _POSIX_C_SOURCE 200809L
#include "wsplit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { AL_NEAR = 0, AL_CENTER = 1, AL_FAR = 2 } Align;

typedef struct {
    const WsPainter *p;
    bool horiz;
} R;

static double lh(const R *g, WsFont f)
{
    double a = 0, d = 0;
    g->p->font_metrics(g->p->ctx, f, &a, &d);
    return a + d;
}

static double asc(const R *g, WsFont f)
{
    double a = 0, d = 0;
    g->p->font_metrics(g->p->ctx, f, &a, &d);
    return a;
}

static double tw(const R *g, WsFont f, const char *s) { return (s && *s) ? g->p->text_width(g->p->ctx, f, s) : 0.0; }
static double em(const R *g, WsFont f) { return g->p->font_size(g->p->ctx, f); }

/* GDI+ MeasureString adds ~1/6 em of padding on each side. */
static double measure(const R *g, WsFont f, const char *s)
{
    if (!s || !*s) return 0.0;
    return tw(g, f, s) + em(g, f) / 3.0;
}

/* Graphics.MeasureString-based "MeasureDisplayStringWidth": the original
   measured the exact glyph extent, so no padding here. */
static int display_width(const R *g, WsFont f, const char *s) { return (int)ceil(tw(g, f, s)); }

static void fill(const R *g, double x, double y, double w, double h, WsColor c)
{
    if (w <= 0 || h <= 0) return;
    g->p->fill_rect(g->p->ctx, x, y, w, h, c);
}

static void grad(const R *g, double x, double y, double w, double h, WsColor c1, WsColor c2)
{
    if (w <= 0 || h <= 0) return;
    g->p->fill_gradient(g->p->ctx, x, y, w, h, c1, c2, g->horiz);
}

static void grad_h(const R *g, double x, double y, double w, double h, WsColor c1, WsColor c2)
{
    if (w <= 0 || h <= 0) return;
    g->p->fill_gradient(g->p->ctx, x, y, w, h, c1, c2, true);
}

static void line(const R *g, double x1, double y1, double x2, double y2, WsColor c)
{
    g->p->stroke_line(g->p->ctx, x1, y1, x2, y2, c);
}

/* Truncate s (UTF-8) with an ellipsis so that it fits in maxw. */
static void ellipsize(const R *g, WsFont f, const char *s, double maxw, char *out, size_t n)
{
    snprintf(out, n, "%s", s ? s : "");
    if (tw(g, f, out) <= maxw) return;
    size_t len = strlen(out);
    while (len > 0) {
        /* step back one UTF-8 code point */
        len--;
        while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80) len--;
        char tmp[1024];
        if (len + 4 >= sizeof tmp) continue;
        memcpy(tmp, out, len);
        memcpy(tmp + len, "\xE2\x80\xA6", 4); /* … */
        if (tw(g, f, tmp) <= maxw) { snprintf(out, n, "%s", tmp); return; }
    }
    out[0] = 0;
}

/* DrawString(s, font, brush, layoutRect, format) with LineAlignment=Center. */
static void text_rect(const R *g, WsFont f, const char *s, double x, double y, double w, double h,
                      Align al, bool ellipsis, WsColor c)
{
    if (!s || !*s || w <= 0 || h <= 0) return;
    double pad = em(g, f) / 6.0;
    double avail = w - 2 * pad;
    if (avail <= 0) return;
    char buf[1024];
    if (ellipsis) ellipsize(g, f, s, avail, buf, sizeof buf);
    else snprintf(buf, sizeof buf, "%s", s);
    if (!*buf) return;
    double width = tw(g, f, buf);
    double tx = x + pad;
    if (al == AL_CENTER) tx = x + (w - width) / 2.0;
    else if (al == AL_FAR) tx = x + w - pad - width;
    double ty = y + (h - lh(g, f)) / 2.0;
    g->p->push_clip(g->p->ctx, x, y, w, h);
    g->p->draw_text(g->p->ctx, f, buf, tx, ty, c);
    g->p->pop_clip(g->p->ctx);
}

/* DrawString(s, font, brush, x, y): top-left anchored with GDI+ padding. */
static void text_at(const R *g, WsFont f, const char *s, double x, double y, WsColor c)
{
    if (!s || !*s) return;
    g->p->draw_text(g->p->ctx, f, s, x + em(g, f) / 6.0, y, c);
}

static double measure_max(const R *g, WsFont f, const char *pattern)
{
    char tmp[32];
    double mx = 0;
    for (int d = 0; d <= 9; d++) {
        snprintf(tmp, sizeof tmp, "%s", pattern);
        for (char *q = tmp; *q; q++) if (*q == '8') *q = (char)('0' + d);
        double w = measure(g, f, tmp);
        if (w > mx) mx = w;
    }
    return mx;
}

static WsColor with_alpha(WsColor c, unsigned a) { return (c & 0x00FFFFFFu) | ((WsColor)a << 24); }

static WsColor darken(WsColor c, double k)
{
    unsigned r = (unsigned)(((c >> 16) & 0xFF) * k), gg = (unsigned)(((c >> 8) & 0xFF) * k), b = (unsigned)((c & 0xFF) * k);
    return 0xFF000000u | (r << 16) | (gg << 8) | b;
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

static void draw_icon(const R *g, void *icon, double x, double y, double size, double alpha, bool gray)
{
    if (g->p->draw_icon) g->p->draw_icon(g->p->ctx, icon, x, y, size, alpha, gray);
}

void ws_render(WsApp *a, const WsPainter *p)
{
    const WsSettings *st = &a->settings;
    const WsColors *c = &st->colors;
    WsRun *r = &a->run;
    R G = {p, st->horizontal_gradient};
    const R *g = &G;

    const int W = a->win_w, H = a->win_h;
    const WsRect cr = a->clock;
    const WsDisplayMode mode = a->cur_mode;
    const bool wide = mode == WS_MODE_WIDE, detailed = mode == WS_MODE_DETAILED, compact = mode == WS_MODE_COMPACT;
    const bool started = ws_app_started(a);
    const double elapsed = ws_sw_elapsed_s(&a->timer);
    const int live = r->live_index, last = ws_run_last(r);
    const bool done = ws_run_done(r), liverun = ws_run_live(r);
    const bool black = st->background_black, plain = st->background_plain;
    const bool show_title = r->title[0] && st->show_title;
    const bool show_goal = r->goal[0] && st->show_goal;

    double num3 = ws_run_seg_delta(r, elapsed, live);   /* current segment delta */
    double secs = ws_run_run_delta(r, elapsed, live);   /* run delta */
    int num5 = wide ? wide_seg_count(a) : detail_seg_count(a);

    /* ---------------------------------------------- clock time string */
    int64_t span;
    if (r->start_delay_ms != 0 && !started) {
        span = r->start_delay_ms;
        if (a->delay_active) span -= (ws_now_ns() - a->delay_start_ns) / 1000000;
        if (span < 0) span = 0;
    } else if (done) {
        span = (int64_t)llround(r->segs[last].live_time * 1000.0);
    } else {
        span = ws_sw_elapsed_ns(&a->timer) / 1000000;
    }
    if (span < 0) span = 0;
    int64_t hours = span / 3600000;
    int minutes = (int)((span / 60000) % 60), seconds = (int)((span / 1000) % 60), millis = (int)(span % 1000);

    const bool digital = st->digital_clock;
    const WsFont big = digital ? WS_FONT_DIGIT_LARGE : WS_FONT_CLOCK_LARGE;
    const WsFont med = digital ? WS_FONT_DIGIT_MED : WS_FONT_CLOCK_MED;
    char absb[48], decb[8];
    double absW, decW;
    if (hours >= 100) {
        absW = measure_max(g, big, "888:88:88");
        snprintf(absb, sizeof absb, "%03lld:%02d:%02d", (long long)(hours % 1000), minutes, seconds);
    } else {
        absW = measure_max(g, big, "88:88:88");
        if (hours >= 1) snprintf(absb, sizeof absb, "%lld:%02d:%02d", (long long)hours, minutes, seconds);
        else if (span >= 60000) snprintf(absb, sizeof absb, "%d:%02d", minutes, seconds);
        else snprintf(absb, sizeof absb, "%d", seconds);
    }
    const bool delay_shown = r->start_delay_ms != 0 && !started;
    if (digital) {
        char tmp[40];
        snprintf(tmp, sizeof tmp, "%8s", absb);
        if (delay_shown && tmp[0] == ' ') tmp[0] = '-';
        snprintf(absb, sizeof absb, "%s", tmp);
    } else if (delay_shown && (hours < 10 || (hours < 100 && st->refresh_ms > 42))) {
        char tmp[48];
        snprintf(tmp, sizeof tmp, "-%s", absb);
        snprintf(absb, sizeof absb, "%s", tmp);
    }
    if (hours >= 100 || st->refresh_ms > 42) {
        decW = measure_max(g, med, "8");
        snprintf(decb, sizeof decb, "%d", millis / 100);
    } else {
        decW = measure_max(g, med, "88");
        snprintf(decb, sizeof decb, "%02d", millis / 10);
    }
    double totW = absW + decW;
    double totH = fmax(lh(g, big), lh(g, med));

    int x = st->segment_icons >= 1 ? (st->segment_icons + 1) * 8 : 0;
    int num8 = (x > 16 && compact && liverun && !done) ? x + 6 : 0;
    double scale = fmin((cr.w - num8) / totW, cr.h / totH);
    if (!(scale > 0)) scale = 1;

    /* ---------------------------------------------- clock colours */
    WsColor fore;
    bool running = a->timer.running;
    if (running) {
        if (liverun) {
            double clast = ws_run_comp_time_at(r, last);
            double ct = ws_run_comp_time(r);
#define SET(F, B, B2, BP) do { fore = c->F; a->clock_gr = c->B; a->clock_gr2 = c->B2; a->clock_plain = c->BP; } while (0)
            if (done) {
                if (r->segs[last].live_time < clast || clast == 0.0) SET(RecordFore, RecordBack, RecordBack2, RecordBackPlain);
                else SET(FinishedFore, FinishedBack, FinishedBack2, FinishedBackPlain);
            } else if (ct == 0.0) SET(AheadFore, AheadBack, AheadBack2, AheadBackPlain);
            else if (elapsed < ct) {
                if (num3 < 0.0) SET(AheadFore, AheadBack, AheadBack2, AheadBackPlain);
                else SET(AheadLosingFore, AheadLosingBack, AheadLosingBack2, AheadLosingBackPlain);
            } else if (num3 < 0.0) SET(BehindFore, BehindBack, BehindBack2, BehindBackPlain);
            else SET(BehindLosingFore, BehindLosingBack, BehindLosingBack2, BehindLosingBackPlain);
        } else {
            SET(WatchFore, WatchBack, WatchBack2, WatchBackPlain);
        }
    } else if (started) {
        fore = c->Paused; /* background keeps its previous colours */
    } else if (r->start_delay_ms != 0) {
        SET(DelayFore, DelayBack, DelayBack2, DelayBackPlain);
    } else if (liverun) {
        SET(AheadFore, AheadBack, AheadBack2, AheadBackPlain);
    } else {
        SET(WatchFore, WatchBack, WatchBack2, WatchBackPlain);
    }
#undef SET
    a->clock_fore = fore;

    /* ---------------------------------------------- status deltas */
    char text[48] = "", str4[48] = "";
    double num10 = 0.0;
    WsColor str4c = 0xFFFFFFFF;
    double l_x = 0, l_y = 0, l_w = 0, l_h = 0;   /* layoutRectangle (run delta) */
    double r2x = 0, r2y = 0, r2w = 0, r2h = 0;   /* rectangle2 (segment delta) */
    Align fmt2 = AL_FAR;
    double segDeltaW = 0, runDeltaW = 0;

    if (mode != WS_MODE_TIMER) {
        if (wide) {
            l_x = W - 119; l_y = 2; l_w = 119; l_h = H / 2;
            r2x = W - 119; r2y = l_y + l_h - 2; r2w = 119; r2h = H / 2;
        } else if (detailed) {
            r2x = 1; r2y = cr.y + cr.h + 2; r2w = W - 1; r2h = 16;
        } else {
            r2x = 1; r2y = 2; r2w = W - 1; r2h = 13;
            l_x = 1; l_y = cr.y + cr.h + 2; l_w = W - 1; l_h = 14;
            if (st->segment_icons > 0 && !done) { r2w -= 2 + x; r2x += 2 + x; }
            fmt2 = AL_NEAR;
        }

        a->seg_losing_time = false;
        if (started) {
            if (done) {
                if (ws_run_comp_time_at(r, last) != 0.0) {
                    num10 = ws_run_seg_delta(r, r->segs[last].live_time, last);
                    ws_format_time(text, sizeof text, num10, WS_FMT_DELTA_SHORT);
                }
            } else if (num3 > 0.0) {
                num10 = num3;
                ws_format_time(text, sizeof text, num10, WS_FMT_DELTA_SHORT);
                a->seg_losing_time = true;
            } else if (live > 0) {
                num10 = ws_run_seg_delta(r, r->segs[live - 1].live_time, live - 1);
                if (r->segs[live - 1].live_time > 0.0 && ws_run_comp_time_at(r, live - 1) != 0.0)
                    ws_format_time(text, sizeof text, num10, WS_FMT_DELTA_SHORT);
            }
        }

        if (!detailed) {
            if (done) {
                if (ws_run_comp_time_at(r, last) != 0.0) {
                    double d = ws_run_run_delta_at(r, last);
                    ws_format_time(str4, sizeof str4, d, WS_FMT_DELTA);
                    str4c = d < 0.0 ? c->RecordFore : c->FinishedFore;
                }
            } else if (num3 > 0.0) {
                ws_format_time(str4, sizeof str4, secs, WS_FMT_DELTA);
                str4c = secs < 0.0 ? c->SegAheadGain : c->SegBehindLoss;
            } else if (live > 0 && r->segs[live - 1].live_time > 0.0 && ws_run_comp_time_at(r, live - 1) != 0.0) {
                double d = ws_run_run_delta_at(r, live - 1);
                ws_format_time(str4, sizeof str4, d, WS_FMT_DELTA);
                str4c = d < 0.0 ? c->SegAheadLoss : c->SegBehindGain;
            }
        }
        segDeltaW = display_width(g, WS_FONT_TIME, text);
        runDeltaW = display_width(g, WS_FONT_TIME, str4);
    }

    /* first segment shown */
    int num14 = live - num5 + 2, li = live;
    if (num5 >= 2) li--;
    bool showlast_mode = (detailed && st->show_last_detailed) || (wide && st->show_last_wide);
    if (showlast_mode) num14++;
    int num13 = li;
    if (last - num5 + 1 < num13) num13 = last - num5 + 1;
    if (num14 < num13) num13 = num14;
    if (num13 < 0) num13 = 0;

    /* ============================================== background */
    fill(g, 0, 0, W, H, 0xFF000000);

    if (mode != WS_MODE_TIMER) {
        double sb_x, sb_y, sb_w, sb_h;      /* status bar */
        double hd_x = 0, hd_y = 0, hd_w = 0, hd_h = 0; /* header text */
        double stx, sty, stw, sth;          /* status text */
        double gx = 0, gy = 0, gw = 0, gh = 0; /* goal text */
        double ts_y, sob_y, pb_y, best_y, ps_y = cr.y + cr.h;
        ts_y = ps_y + (st->show_prev_seg ? 18 : 0);
        sob_y = ts_y + (st->show_time_save ? 18 : 0);
        pb_y = sob_y + (st->show_sob ? 18 : 0);
        best_y = pb_y + (st->pred_pb ? 18 : 0);
        bool extras = detailed;

        if (wide) {
            sb_x = W - 120; sb_y = 0; sb_w = 120; sb_h = H;
            if (show_goal) {
                hd_x = W - 119; hd_y = H / 4 - 4; hd_w = 59; hd_h = 12;
                stx = hd_x + hd_w; sty = H / 4 - 4; stw = 59; sth = 12;
                gx = W - 119; gy = H / 2; gw = 118; gh = H / 2;
            } else {
                hd_x = W - 119; hd_y = H / 4 - 4; hd_w = 118; hd_h = 12;
                stx = W - 119; sty = H / 2; stw = 118; sth = H / 2;
            }
        } else if (detailed) {
            sb_x = 0; sb_y = cr.y + cr.h; sb_w = W; sb_h = 18;
            stx = 1; sty = cr.y + cr.h + 2; stw = W - 2; sth = 16;
        } else {
            sb_x = 0; sb_y = cr.y + cr.h; sb_w = W; sb_h = 16;
            hd_x = 1; hd_y = 2; hd_w = W / 2 - 2; hd_h = 13;
            stx = 1; sty = cr.y + cr.h + 2; stw = W - 2; sth = 14;
            if (show_goal) { gx = hd_x + hd_w; gy = 2; gw = W / 2 - 2; gh = 13; }
            if (st->segment_icons > 0 && !done) { hd_w -= 4 + x; hd_x += 4 + x; }
        }

        if (!black) {
            if (plain) {
                fill(g, sb_x, sb_y, sb_w, sb_h, c->StatusBackPlain);
                if (extras) {
                    if (st->show_time_save) fill(g, 0, ts_y, W, 18, c->StatusBackPlain);
                    if (st->show_sob) fill(g, 0, sob_y, W, 18, c->StatusBackPlain);
                    if (st->pred_pb) fill(g, 0, pb_y, W, 18, c->StatusBackPlain);
                    if (st->pred_best) fill(g, 0, best_y, W, 18, c->StatusBackPlain);
                }
            } else {
                grad(g, sb_x, sb_y, sb_w, sb_h, c->StatusBack, c->StatusBack2);
                if (extras) {
                    if (st->show_time_save) grad(g, 0, ts_y, W, 18, c->StatusBack, c->StatusBack2);
                    if (st->show_sob) grad(g, 0, sob_y, W, 18, c->StatusBack, c->StatusBack2);
                    if (st->pred_pb) grad(g, 0, pb_y, W, 18, c->StatusBack, c->StatusBack2);
                    if (st->pred_best) grad(g, 0, best_y, W, 18, c->StatusBack, c->StatusBack2);
                }
            }

            if (detailed) {
                if (show_title && show_goal && !plain) {
                    fill(g, 0, 0, W, 32, c->TitleBack);
                    fill(g, 0, 0, W, 16, c->TitleBack2);
                } else {
                    if (show_title) {
                        if (plain) fill(g, 0, 0, W, 16, c->TitleBackPlain);
                        else { fill(g, 0, 0, W, 16, c->TitleBack); fill(g, 0, 0, W, 8, c->TitleBack2); }
                    }
                    if (show_goal) {
                        int goalY = show_title ? 16 : 0;
                        if (plain) fill(g, 0, goalY, W, 16, c->TitleBackPlain);
                        else { fill(g, 0, goalY, W, 16, c->TitleBack); fill(g, 0, goalY, W, 8, c->TitleBack2); }
                    }
                }
            } else if (compact) {
                if (plain) fill(g, 0, 0, W, 18, c->TitleBackPlain);
                else { fill(g, 0, 0, W, 15, c->TitleBack); fill(g, 0, 0, W, 7, c->TitleBack2); }
            }
        }

        /* status text */
        Align fmt4 = compact ? AL_FAR : AL_NEAR;
        char s[512] = "", status[256] = "";
        if (!started) {
            if (!wide) fmt4 = AL_FAR;
            if (a->delay_active) snprintf(status, sizeof status, "Delay");
            else {
                snprintf(status, sizeof status, "Ready");
                if (st->show_attempts && (!detailed || !st->show_title))
                    snprintf(status, sizeof status, "Ready, Attempt #%d", r->attempts + 1);
            }
        } else if (done) {
            double clast = ws_run_comp_time_at(r, last);
            if (clast == 0.0) { if (!wide) fmt4 = AL_FAR; snprintf(status, sizeof status, "Done"); }
            else if (r->segs[last].live_time < clast) snprintf(status, sizeof status, "New Record");
            else snprintf(status, sizeof status, "Done");
        } else if (compact && ws_run_comp_time_at(r, live) != 0.0) {
            char t[48];
            ws_format_time(t, sizeof t, ws_run_comp_time_at(r, live), WS_FMT_LONG);
            snprintf(status, sizeof status, "%s: %s", ws_compare_name(ws_run_comparing_type(r)), t);
        } else if (a->seg_losing_time) {
            snprintf(status, sizeof status, "Live Segment");
        } else if (live > 0 && r->segs[live - 1].live_time > 0.0 && ws_run_comp_time_at(r, live - 1) != 0.0) {
            snprintf(status, sizeof status, "Previous Segment");
        }

        if (detailed) {
            int goalTextY = 0;
            bool has_toggle = st->hotkeys[WS_HK_TOGGLE][0] != 0;
            if (show_title) {
                double rw = W;
                goalTextY = 16;
                if (has_toggle) {
                    fill(g, W - 10, 4, 6, 6, st->hotkeys_enabled ? 0xFFADFF2F : 0xFFFF4500);
                    rw -= 10;
                }
                char t[600];
                if (st->show_attempts)
                    snprintf(t, sizeof t, "#%d / %s", a->timer.running ? r->attempts : r->attempts + 1, r->title);
                else
                    snprintf(t, sizeof t, "%s", r->title);
                text_rect(g, WS_FONT_DISPLAY, t, 0, 1, rw, 17, AL_CENTER, true, c->TitleFore);
            }
            if (show_goal) {
                double rw = W - (has_toggle ? 10 : 0);
                char t[600];
                snprintf(t, sizeof t, "Goal: %s", r->goal);
                text_rect(g, WS_FONT_DISPLAY, t, 0, goalTextY, rw, 17, AL_CENTER, true, c->TitleFore);
            }
        } else if (done) {
            if (wide) { snprintf(s, sizeof s, "%s", status); snprintf(status, sizeof status, "Final"); }
            else snprintf(s, sizeof s, "Final");
        } else if (compact) {
            snprintf(s, sizeof s, "%s", r->segs[live].name);
        } else if (show_title) {
            snprintf(s, sizeof s, "%s", r->title);
        } else {
            snprintf(s, sizeof s, "Run");
        }

        if (compact) {
            hd_w -= segDeltaW;
            stw -= runDeltaW;
            stx += runDeltaW;
            gw -= segDeltaW;          /* port: keep the goal clear of the segment delta */
        } else if (wide && show_goal) {
            hd_w -= runDeltaW;
            stw -= runDeltaW;         /* status shares the top row with the run delta */
            gw -= segDeltaW;
        } else {
            hd_w -= runDeltaW;
            stw -= segDeltaW;
        }
        if (!detailed) text_rect(g, WS_FONT_DISPLAY, s, hd_x, hd_y, hd_w, hd_h, AL_NEAR, true, c->StatusFore);
        text_rect(g, WS_FONT_DISPLAY, status, stx, sty, stw, sth, fmt4, true, c->StatusFore);

        if (detailed) {
            char t[48];
            if (st->show_sob) {
                text_rect(g, WS_FONT_DISPLAY, "Sum of Best", 0, sob_y + 2, W - 2, 16, AL_NEAR, true, c->StatusFore);
                double sob = ws_run_sum_of_bests(r, last);
                if (sob > 0.0) ws_format_time(t, sizeof t, sob, WS_FMT_SHORT); else snprintf(t, sizeof t, "-");
                text_rect(g, WS_FONT_DISPLAY, t, 0, sob_y + 2, W - 2, 16, AL_FAR, true, c->StatusFore);
            }
            if (st->show_time_save) {
                double segtime = 0.0;
                snprintf(t, sizeof t, "0:00");
                if (live > 0 && live <= last) segtime = r->segs[live].best_time - r->segs[live - 1].best_time;
                else if (live == 0 && liverun) segtime = r->segs[0].best_time;
                if (segtime > 0.0 && live <= last && r->segs[live].best_segment > 0.0 && segtime >= r->segs[live].best_segment)
                    ws_format_time(t, sizeof t, fabs(segtime - r->segs[live].best_segment), WS_FMT_SHORT);
                if (live > 0 && live <= last && (r->segs[live].best_time == 0.0 || r->segs[live - 1].best_time == 0.0))
                    snprintf(t, sizeof t, "-");
                text_rect(g, WS_FONT_DISPLAY, "Possible Gain", 0, ts_y + 2, W - 2, 16, AL_NEAR, true, c->StatusFore);
                text_rect(g, WS_FONT_DISPLAY, t, 0, ts_y + 2, W - 2, 16, AL_FAR, true, c->StatusFore);
            }
            if (st->pred_pb) {
                text_rect(g, WS_FONT_DISPLAY, "Predicted (PB)", 0, pb_y + 2, W - 2, 16, AL_NEAR, true, c->StatusFore);
                if (ws_run_comp_time_at(r, last) > 0.0)
                    ws_format_time(t, sizeof t, ws_run_comp_time_at(r, last) + ws_run_last_delta(r, live), WS_FMT_SHORT);
                else
                    snprintf(t, sizeof t, "-");
                text_rect(g, WS_FONT_DISPLAY, t, 0, pb_y + 2, W - 2, 16, AL_FAR, true, c->StatusFore);
            }
            if (st->pred_best) {
                text_rect(g, WS_FONT_DISPLAY, "Predicted (Best)", 0, best_y + 2, W - 2, 16, AL_NEAR, true, c->StatusFore);
                double best = ws_run_sum_of_bests(r, last);
                if (best > 0.0) {
                    if (live > 0 && live <= last) {
                        for (int i = live; i >= 0; i--) {
                            if (r->segs[i].live_time != 0) { best += r->segs[i].live_time - ws_run_sum_of_bests(r, i); break; }
                        }
                    }
                    ws_format_time(t, sizeof t, best, WS_FMT_SHORT);
                } else {
                    snprintf(t, sizeof t, "-");
                }
                text_rect(g, WS_FONT_DISPLAY, t, 0, best_y + 2, W - 2, 16, AL_FAR, true, c->StatusFore);
            }
        }
        if (!detailed && r->goal[0] && st->show_goal) {
            char t[600];
            snprintf(t, sizeof t, "Goal: %s", r->goal);
            text_rect(g, WS_FONT_DISPLAY, t, gx, gy, gw, gh, fmt4, true, c->StatusFore);
        }
    }

    /* ============================================== segment list */
    double hl_x = 0, hl_y = 0, hl_w = 0, hl_h = 1;
    if (wide || detailed) {
        double num16 = cr.x + cr.w + 2;
        double y = 0;
        if (show_title && show_goal) y += 32;
        else if (show_title || show_goal) y += 18;

        double r8x, r8y, r8w, r8h;
        if (wide) {
            r8x = num16; r8y = 0; r8w = W - cr.w - 122; r8h = H;
            fill(g, cr.x + cr.w, 0, 2, H, c->StatusBack2);
        } else {
            r8x = 0; r8y = y; r8w = W; r8h = cr.y - y; /* original: clockRect.Bottom - 18 (overlaps the status bar when a goal is shown) */
        }
        if (!black) {
            if (plain) fill(g, r8x, r8y, r8w, r8h, c->SegBackPlain);
            else grad(g, r8x, r8y, r8w, r8h, c->SegBack, c->SegBack2);
        }

        bool flag2 = num5 > 3 && (num13 + num5 - 1) < last && showlast_mode;
        if (wide) {
            if (flag2) fill(g, W - 122 - a->wide_seg_w, 0, 2, H, c->StatusBack2);
            else fill(g, W - 122, 0, 2, H, c->StatusBack2);
        } else if (flag2) {
            fill(g, 0, cr.y - 3 - a->seg_height, W, 3, c->StatusBackPlain);
        } else if (plain || black) {
            fill(g, 0, cr.y - 3, W, 3, c->StatusBackPlain);
        } else {
            grad_h(g, 0, cr.y - 3, W, 3, c->StatusBack, c->StatusBack2);
        }

        Align fmt7 = detailed ? AL_FAR : AL_NEAR;
        int segH = a->seg_height;
        int n18 = 0;
        for (int i = num13; n18 < num5 && i <= last; i++) {
            if (n18 + 1 >= num5 && flag2) { i = last; y += 3; num16 += 2; }
            WsSegment *sg = &r->segs[i];
            WsColor nameC = c->FutureSegName;
            double alpha = 0.65;
            int segTimeW = display_width(g, WS_FONT_TIME, sg->time_string);

            double r10x, r10y, r10w, r10h, r11x, r11y, r11w, r11h, r12x, r12y, r12w, r12h;
            if (wide) {
                r10x = num16; r10y = 0; r10w = a->wide_seg_w; r10h = H;
                r11x = num16 + 2; r11y = H / 4 - 4; r11w = a->wide_seg_w - x - 2; r11h = 12;
                r12x = r11x; r12y = H / 2; r12w = r11w; r12h = H / 2;
            } else {
                r10x = 0; r10y = y; r10w = W; r10h = segH;
                r11x = x; r11y = y + 2; r11w = W - x; r11h = segH;
                r12x = x; r12y = y + 1; r12w = W - x; r12h = segH;
                if (segH <= 24) {
                    r11w -= segTimeW + 2;
                    r11y = y + segH / 2 - 5;
                    r11h = 13;
                } else {
                    r11y = y + 2;
                    r11h = segH / 2;
                    r12y = r11y + r11h - 2;
                    r12h = segH / 2;
                }
            }

            bool past = i < live && started;
            if (past) {
                nameC = c->PastSeg;
            } else if (i == live) {
                nameC = c->LiveSeg;
                hl_x = r10x; hl_y = r10y; hl_w = r10w; hl_h = r10h;
                if (plain || black) fill(g, r10x, r10y, r10w, r10h, c->SegHighlightPlain);
                else grad(g, r10x, r10y, r10w, r10h, c->SegHighlight, c->SegHighlight2);
                alpha = 1.0;
            }
            if (x != 0) {
                if (past) alpha = 0.85;
                double dx = r10x, dy = r10y;
                if (wide) { dx += a->wide_seg_w - x; dy += H - x; }
                draw_icon(g, sg->icon, dx, dy, x, alpha, past);
            }
            text_rect(g, WS_FONT_DISPLAY, sg->name, r11x, r11y, r11w, r11h, AL_NEAR, true, nameC);
            if (i != live)
                text_rect(g, WS_FONT_TIME, sg->time_string, r12x, r12y, r12w, r12h, fmt7, false, sg->time_color);

            num16 += a->wide_seg_w;
            y += segH;
            n18++;
        }

        if (liverun && !done) {
            WsColor bc = c->SegHighlightBorder;
            if (wide) {
                hl_h--;
                line(g, hl_x + 0.5, hl_y + 0.5, hl_x + hl_w + 0.5, hl_y + 0.5, bc);
                line(g, hl_x + hl_w + 0.5, hl_y + 0.5, hl_x + hl_w + 0.5, hl_y + hl_h + 0.5, bc);
                line(g, hl_x + hl_w + 0.5, hl_y + hl_h + 0.5, hl_x + 0.5, hl_y + hl_h + 0.5, bc);
                line(g, hl_x + 0.5, hl_y + hl_h + 0.5, hl_x + 0.5, hl_y + 0.5, bc);
            } else {
                line(g, hl_x, hl_y + 0.5, hl_x + hl_w, hl_y + 0.5, bc);
                line(g, hl_x, hl_y + hl_h + 0.5, hl_x + hl_w, hl_y + hl_h + 0.5, bc);
            }
        }
    }

    /* ============================================== clock back */
    double tx = num8 + (cr.w - scale * totW - num8) / 2.0;
    double ty = cr.y + (cr.h - scale * totH) / 2.0;
    if (!black) {
        if (plain) {
            fill(g, cr.x, cr.y, cr.w, cr.h, a->clock_plain);
        } else {
            grad(g, cr.x, cr.y, cr.w, cr.h, a->clock_gr, a->clock_gr2);
            if (st->horizontal_gradient && st->clock_accent)
                fill(g, cr.x, cr.y, cr.w, cr.h / 2, with_alpha(a->clock_gr2, 0x56));
            if (digital) {
                /* "ghost" segments behind the digital clock */
                WsColor gc = with_alpha(a->clock_gr2, 86);
                char ghost[40];
                size_t al = strlen(absb);
                snprintf(ghost, sizeof ghost, "%s", al > 8 ? "888:88:88" : "88:88:88");
                p->push_clip(p->ctx, cr.x, cr.y, cr.w, cr.h);
                p->push_transform(p->ctx, tx, ty, scale);
                text_at(g, WS_FONT_DIGIT_LARGE, ghost, 0, 0.15, gc);
                char dg[8];
                snprintf(dg, sizeof dg, "%s", strlen(decb) == 1 ? "8" : "88");
                if (st->show_decimal_separator) text_at(g, WS_FONT_DIGIT_MED, a->decimal_char, absW - 7, 4.5, gc);
                text_at(g, WS_FONT_DIGIT_MED, dg, absW, 4.5, gc);
                p->pop_transform(p->ctx);
                p->pop_clip(p->ctx);
            }
        }
    }

    /* compact: current segment icon */
    if (x > 0 && compact && liverun && !done) {
        draw_icon(g, r->segs[live].icon, 3, 3, x, 1.0, false);
        WsColor bc = darken(a->clock_fore, 0.7);
        line(g, 3.5, 3.5, 3.5 + x, 3.5, bc);
        line(g, 3.5 + x, 3.5, 3.5 + x, 3.5 + x, bc);
        line(g, 3.5 + x, 3.5 + x, 3.5, 3.5 + x, bc);
        line(g, 3.5, 3.5 + x, 3.5, 3.5, bc);
    }

    /* ============================================== clock fore */
    p->push_clip(p->ctx, cr.x, cr.y, cr.w, cr.h);
    p->push_transform(p->ctx, tx, ty, scale);
    if (digital) {
        text_at(g, WS_FONT_DIGIT_LARGE, absb, 0, 0.15, fore);
        if (st->show_decimal_separator) text_at(g, WS_FONT_DIGIT_MED, a->decimal_char, absW - 7, 4.5, fore);
        text_at(g, WS_FONT_DIGIT_MED, decb, absW, 4.5, fore);
    } else {
        double largeBase = asc(g, WS_FONT_CLOCK_LARGE), medBase = asc(g, WS_FONT_CLOCK_MED);
        double pad = em(g, WS_FONT_CLOCK_LARGE) / 6.0;
        /* abs part right-aligned in [0, absW] */
        double aw = tw(g, WS_FONT_CLOCK_LARGE, absb);
        p->draw_text(p->ctx, WS_FONT_CLOCK_LARGE, absb, absW - pad - aw, 0, fore);
        double decx;
        if (st->show_decimal_separator) {
            text_at(g, WS_FONT_CLOCK_MED, a->decimal_char, absW - 7, largeBase - medBase, fore);
            decx = absW;
        } else {
            decx = absW - 2;
        }
        text_at(g, WS_FONT_CLOCK_MED, decb, decx, largeBase - medBase, fore);
    }
    p->pop_transform(p->ctx);
    p->pop_clip(p->ctx);

    /* ============================================== deltas */
    if (mode != WS_MODE_TIMER) {
        if (st->show_prev_seg) {
            WsColor tc;
            if (!a->seg_losing_time && live > 0 && live - 1 <= last && ws_run_live_segment(r, live - 1) != 0.0 &&
                (r->segs[live - 1].best_segment == 0.0 || ws_run_live_segment(r, live - 1) < r->segs[live - 1].best_segment))
                tc = c->SegBestSegment;
            else if (num10 > 0.0)
                tc = c->SegBehindLoss;
            else
                tc = c->SegAheadGain;
            text_rect(g, WS_FONT_TIME, text, r2x, r2y, r2w, r2h, AL_FAR, true, tc);
        }
        if (!detailed) text_rect(g, WS_FONT_TIME, str4, l_x, l_y, l_w, l_h, fmt2, true, str4c);
    }

    /* live segment time */
    if ((wide || detailed) && !done && liverun) {
        WsSegment *cs = &r->segs[live];
        double rx, ry, rw, rh;
        if (wide) {
            rx = cr.x + cr.w + a->wide_seg_w * (live - num13) + 4;
            ry = H / 2; rw = a->wide_seg_w - x - 2; rh = H / 2;
        } else {
            rx = x; ry = a->seg_height * (live - num13); rw = W - x; rh = a->seg_height;
            if (show_title && show_goal) ry += 32;
            else if (show_title || show_goal) ry += 18;
            /* "show last" slot moves the last segment down by 3px */
            bool flag2 = num5 > 3 && (num13 + num5 - 1) < last && showlast_mode;
            if (flag2 && live == last) ry = a->seg_height * (num5 - 1) + 3 + (show_title && show_goal ? 32 : (show_title || show_goal) ? 18 : 0);
            if (a->seg_height > 24) { rh /= 2; ry += rh; }
            else ry += 1;
        }
        if (!(wide && (live - num13) >= num5))
            text_rect(g, WS_FONT_TIME, cs->time_string, rx, ry, rw, rh, wide ? AL_NEAR : AL_FAR, false, cs->time_color);
    }
}
