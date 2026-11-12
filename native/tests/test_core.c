#define _POSIX_C_SOURCE 200809L
/* Core unit tests: expected values follow the C# WSplit semantics. */
#include "../core/wsplit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static int fails = 0, total = 0;
#define EQS(got, exp) do { total++; if (strcmp((got), (exp))) { fails++; printf("FAIL %s:%d: got '%s' expected '%s'\n", __FILE__, __LINE__, (got), (exp)); } } while (0)
#define EQD(got, exp) do { total++; if (fabs((got) - (exp)) > 1e-9) { fails++; printf("FAIL %s:%d: got %.6f expected %.6f\n", __FILE__, __LINE__, (double)(got), (double)(exp)); } } while (0)
#define OK(c) do { total++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *fmt(double s, WsTimeFormat f) { static char b[64]; ws_format_time(b, sizeof b, s, f); return b; }
static const char *efmt(double s) { static char b[64]; ws_format_edit_time(b, sizeof b, s); return b; }

int main(void)
{
    /* timeFormatter */
    EQS(fmt(65.37, WS_FMT_SHORT), "1:05");
    EQS(fmt(5.37, WS_FMT_SHORT), "0:05");
    EQS(fmt(5.37, WS_FMT_DELTA), "+5.3");
    EQS(fmt(-5.37, WS_FMT_DELTA), "-5.3");
    EQS(fmt(0.0, WS_FMT_DELTA), "+0.0");
    EQS(fmt(-65.0, WS_FMT_DELTA), "-1:05");
    EQS(fmt(3725.5, WS_FMT_SHORT), "1:02:05");
    EQS(fmt(3725.5, WS_FMT_LONG), "1:02:05.5");
    EQS(fmt(65.37, WS_FMT_LONG), "1:05.3");
    EQS(fmt(360061.0, WS_FMT_SHORT), "100h01");
    EQS(fmt(6100.0, WS_FMT_DELTA_SHORT), "+99:59");
    EQS(fmt(3725.0, WS_FMT_DELTA_SHORT), "+62:05");
    EQS(fmt(12.345, WS_FMT_SECONDS), "12.34");
    EQS(efmt(65.37), "1:05.37");
    EQS(efmt(3725.456), "1:02:05.45");
    EQS(efmt(5.4), "5.40");
    EQD(ws_parse_time("1:05.37"), 65.37);
    EQD(ws_parse_time("1:02:05,45"), 3725.45);
    EQD(ws_parse_time("42"), 42.0);
    EQD(ws_parse_time("x:10"), 10.0); /* bad parts are skipped like TryParse */

    /* hotkeys */
    unsigned m; char k[32];
    OK(ws_hotkey_parse("Ctrl+Shift+F1", &m, k, sizeof k) && m == (WS_MOD_CTRL | WS_MOD_SHIFT) && !strcmp(k, "F1"));
    OK(ws_hotkey_parse("Numpad1", &m, k, sizeof k) && m == 0 && !strcmp(k, "Numpad1"));
    OK(!ws_hotkey_parse("", &m, k, sizeof k));

    /* split logic */
    WsRun r; ws_run_init(&r);
    ws_run_add(&r, "A", 11, 10, 9);
    ws_run_add(&r, "B", 22, 20, 9);
    ws_run_add(&r, "C", 33, 30, 9);
    OK(!r.old_faster);
    r.comp = WS_CMP_FASTEST;
    EQD(ws_run_comp_time_at(&r, 1), 20);
    r.comp = WS_CMP_OLD; EQD(ws_run_comp_time_at(&r, 1), 22);
    r.comp = WS_CMP_SOB; EQD(ws_run_comp_time_at(&r, 2), 27);
    r.comp = WS_CMP_BEST;
    ws_run_do_split(&r, 9.5);
    EQD(ws_run_run_delta_at(&r, 0), -0.5);
    EQD(ws_run_seg_delta(&r, 21.0, 1), 1.5); /* run delta +1, last delta -0.5 */
    ws_run_do_split(&r, 21);
    ws_run_do_split(&r, 29);
    OK(ws_run_done(&r));
    OK(r.new_best_time);
    OK(ws_run_need_update(&r, true));
    EQD(ws_run_live_segment(&r, 2), 8);
    ws_run_update_best(&r, true);
    EQD(r.segs[0].best_time, 9.5); EQD(r.segs[1].best_time, 21); EQD(r.segs[2].best_time, 29);
    EQD(r.segs[2].best_segment, 8); EQD(r.segs[1].best_segment, 9);
    OK(!ws_run_need_update(&r, true));
    ws_run_previous(&r);
    OK(!ws_run_done(&r) && r.live_index == 2 && r.segs[2].live_time == 0);

    /* best-to-each-split mode with a skipped split */
    ws_run_reset(&r);
    ws_run_do_split(&r, 9.0);   /* faster split 1 */
    ws_run_next(&r);            /* skip split 2 */
    ws_run_do_split(&r, 40.0);  /* slow finish */
    OK(ws_run_need_update(&r, false));
    ws_run_update_best(&r, false);
    EQD(r.segs[0].best_time, 9.0);
    EQD(r.segs[1].best_time, 21);
    EQD(r.segs[2].best_time, 29);
    EQD(r.segs[0].best_segment, 9.0);
    EQD(r.segs[2].best_segment, 8); /* no best segment from a skipped split */

    /* file round trip */
    char path[512];
    {
        const char *td = getenv("TEMP"); /* Windows */
        if (!td || !*td) td = getenv("TMPDIR");
        if (!td || !*td) td = "/tmp";
        snprintf(path, sizeof path, "%s/wsplit_test_%ld_%d.wsplit", td, (long)time(NULL), rand());
    }
    ws_run_set_title(&r, "Super Metroid 100%");
    ws_run_set_goal(&r, "sub 1:20");
    r.attempts = 42; r.start_delay_ms = 1500; r.pref_w = 240; r.pref_h = 60;
    ws_seg_set_icon_path(&r.segs[1], "/icons/b.png");
    char err[256];
    OK(ws_runfile_save(&r, path, err, sizeof err));
    WsRun q; ws_run_init(&q);
    OK(ws_runfile_load(&q, path));
    EQS(q.title, "Super Metroid 100%");
    EQS(q.goal, "sub 1:20");
    OK(q.attempts == 42 && q.start_delay_ms == 1500 && q.pref_w == 240 && q.pref_h == 60 && q.count == 3);
    EQS(q.segs[1].name, "B"); EQS(q.segs[1].icon_path, "/icons/b.png"); EQS(q.segs[0].icon_path, "");
    EQD(q.segs[0].best_time, 9.0); EQD(q.segs[2].best_segment, 8);

    /* a file written by the original Windows WSplit 1.5.x */
    FILE *f = fopen(path, "wb");
    fputs("Title=Test Run\r\nAttempts=7\r\nOffset=0\r\nSize=152,25\r\nLevel 1,62.5,60.12,60.12\r\nLevel 2,0,125.4,61.03\r\nIcons=\"C:\\a.png\",\"\"\r\n", f);
    fclose(f);
    OK(ws_runfile_load(&q, path));
    OK(q.count == 2 && q.attempts == 7 && q.pref_w == 152);
    EQD(q.segs[0].old_time, 62.5); EQD(q.segs[1].best_time, 125.4);
    EQS(q.segs[0].icon_path, "C:\\a.png");
    remove(path);

    /* settings round trip */
    WsSettings s; ws_settings_default(&s);
    s.display_mode = WS_MODE_WIDE; s.colors.AheadFore = 0xFF112233;
    snprintf(s.hotkeys[WS_HK_SPLIT], sizeof s.hotkeys[0], "Ctrl+Numpad1");
    ws_settings_add_recent(&s, "/a.wsplit"); ws_settings_add_recent(&s, "/b.wsplit"); ws_settings_add_recent(&s, "/a.wsplit");
    OK(ws_settings_save(&s, path));
    WsSettings t; ws_settings_default(&t);
    OK(ws_settings_load(&t, path));
    OK(t.display_mode == WS_MODE_WIDE && t.colors.AheadFore == 0xFF112233 && t.recent_count == 2);
    EQS(t.recent[0], "/a.wsplit"); EQS(t.hotkeys[WS_HK_SPLIT], "Ctrl+Numpad1");
    remove(path);

    /* ---- run editor text grid */
    {
        WsRun e; ws_run_init(&e);
        ws_run_add(&e, "Level 1", 62.5, 60.12, 60.12);
        ws_run_add(&e, "A|B", 0, 125.4, 61.03);
        ws_seg_set_icon_path(&e.segs[1], "C:\\icons\\b.png");
        char *txt = ws_run_segments_to_text(&e);
        OK(strstr(txt, "Level 1 | 1:02.50 | 1:00.12 | 1:00.12 | \n") != NULL);
        OK(ws_run_segments_from_text(&e, txt) == 2);
        EQS(e.segs[1].name, "A|B"); EQD(e.segs[1].best_time, 125.4); EQD(e.segs[1].old_time, 0);
        EQS(e.segs[1].icon_path, "C:\\icons\\b.png");
        free(txt);
        OK(ws_run_segments_from_text(&e, "# hdr\nFirst, part | 90 | 1:30\n\nSecond\n") == 2);
        EQS(e.segs[0].name, "First  part"); EQD(e.segs[0].old_time, 90); EQD(e.segs[0].best_time, 90);
        EQS(e.segs[1].name, "Second"); EQD(e.segs[1].best_time, 0);
        ws_run_free(&e);
    }

    /* ---- timing: event-timestamp back-dating and exact start delay */
    {
        WsHost h = {0};
        WsApp app; ws_app_init(&app, &h, NULL);
        ws_run_add(&app.run, "A", 0, 0, 0); ws_run_add(&app.run, "B", 0, 0, 0);
        app.settings.double_tap_guard_ms = 0;
        ws_app_initialize_display(&app);
        ws_app_timer_control(&app);              /* start now */
        struct timespec ts = {0, 300000000};    /* 300 ms */
        nanosleep(&ts, NULL);
        ws_app_set_input_latency(&app, 100000000); /* key was pressed 100 ms ago */
        ws_app_timer_control(&app);              /* split */
        double t = app.run.segs[0].live_time;
        OK(t >= 0.19 && t <= 0.21);              /* ~0.20 s, not ~0.30 s */
        OK(app.input_ns == 0);
        ws_app_free(&app);

        ws_app_init(&app, &h, NULL);
        ws_run_add(&app.run, "A", 0, 0, 0);
        app.run.start_delay_ms = 50;
        ws_app_initialize_display(&app);
        ws_app_pause_resume(&app);               /* begin delay */
        int64_t d0 = app.delay_start_ns;
        struct timespec ts2 = {0, 80000000};    /* tick arrives 30 ms after the delay ended */
        nanosleep(&ts2, NULL);
        ws_app_tick(&app);
        OK(!app.delay_active && app.timer.running);
        OK(app.timer.start_ns == d0 + 50000000LL); /* started exactly at the end of the delay */
        ws_app_free(&app);
    }

    ws_run_free(&r); ws_run_free(&q);
    printf("%d/%d tests passed\n", total - fails, total);
    return fails != 0;
}
