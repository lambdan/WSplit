/* Settings persistence: a flat key=value file (replaces .NET user settings). */
#define _POSIX_C_SOURCE 200809L
#include "wsplit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *const ws_hotkey_labels[WS_HK_COUNT] = {
    "Start/Split", "Pause/Resume", "Stop", "Reset",
    "Previous (Unsplit)", "Next (Skip)", "Switch comparison", "Toggle Global Hotkeys"};

static const char *const hk_keys[WS_HK_COUNT] = {
    "SplitKey", "PauseKey", "StopKey", "ResetKey", "PrevKey", "NextKey", "CompTypeKey", "HotkeyToggleKey"};

void ws_colors_default(WsColors *c)
{
#define X(name, def) c->name = def;
    WS_COLOR_FIELDS(X)
#undef X
}

void ws_settings_default(WsSettings *s)
{
    memset(s, 0, sizeof *s);
    s->best_as_overall = true;
    s->compare_against = 0;
    s->double_tap_guard_ms = 500;
    s->refresh_ms = 15;
    s->load_most_recent = true;
    s->save_window_pos = true;
    s->window_x = 20;
    s->window_y = 20;
    s->on_top = false;
    s->opacity = 1.0;

    s->display_mode = WS_MODE_DETAILED;
    s->clock_w = 240;
    s->clock_h = 50;
    s->digital_clock = true;
    s->clock_accent = true;
    s->show_decimal_separator = true;
    s->horizontal_gradient = true;
    s->display_segs = 10;
    s->show_last_detailed = true;
    s->wide_segs = 3;
    s->show_last_wide = true;
    s->wide_height = 36;
    s->wide_seg_width = 100;
    s->segment_icons = 0;
    s->show_title = true;
    s->show_goal = true;
    s->show_attempts = false;
    s->show_prev_seg = true;

    strcpy(s->font_family, "");
    s->font_multiplier = 1.0;

    s->hotkeys_enabled = false;
    ws_colors_default(&s->colors);
}

/* ------------------------------------------------------------ helpers */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static bool parse_bool(const char *v) { return !strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "yes"); }

static bool parse_color(const char *v, WsColor *out)
{
    if (*v == '#') v++;
    size_t l = strlen(v);
    if (l != 6 && l != 8) return false;
    for (size_t i = 0; i < l; i++) if (!isxdigit((unsigned char)v[i])) return false;
    unsigned long x = strtoul(v, NULL, 16);
    *out = l == 6 ? (WsColor)(0xFF000000u | x) : (WsColor)x;
    return true;
}

static void put_color(FILE *f, const char *k, WsColor c)
{
    if ((c >> 24) == 0xFF) fprintf(f, "%s=#%06X\n", k, c & 0xFFFFFF);
    else fprintf(f, "%s=#%08X\n", k, c);
}

static bool apply_color(WsColors *c, const char *k, const char *v)
{
#define X(name, def) if (!strcmp(k, #name)) return parse_color(v, &c->name);
    WS_COLOR_FIELDS(X)
#undef X
    return false;
}

static void write_colors(FILE *f, const WsColors *c)
{
#define X(name, def) put_color(f, #name, c->name);
    WS_COLOR_FIELDS(X)
#undef X
}

/* Table of scalar settings. */
typedef enum { T_BOOL, T_INT, T_DBL, T_STR } Ty;
typedef struct { const char *key; Ty ty; size_t off; size_t sz; } Field;
#define FB(k, m) {k, T_BOOL, offsetof(WsSettings, m), 0}
#define FI(k, m) {k, T_INT, offsetof(WsSettings, m), 0}
#define FD(k, m) {k, T_DBL, offsetof(WsSettings, m), 0}
#define FS(k, m) {k, T_STR, offsetof(WsSettings, m), sizeof(((WsSettings *)0)->m)}

static const Field fields[] = {
    FB("BestAsOverall", best_as_overall), FI("CompareAgainst", compare_against),
    FI("DoubleTapGuard", double_tap_guard_ms), FI("RefreshRate", refresh_ms),
    FB("LoadMostRecent", load_most_recent), FB("SaveWindowPos", save_window_pos),
    FI("WindowX", window_x), FI("WindowY", window_y), FB("OnTop", on_top), FD("Opacity", opacity),
    FI("DisplayMode", display_mode), FI("ClockWidth", clock_w), FI("ClockHeight", clock_h),
    FB("DigitalClock", digital_clock), FB("ClockAccent", clock_accent),
    FB("ShowDecimalSeparator", show_decimal_separator), FB("BackgroundBlack", background_black),
    FB("BackgroundPlain", background_plain), FB("HGrad", horizontal_gradient),
    FI("DisplaySegs", display_segs), FB("DisplayBlankSegs", display_blank_segs),
    FB("ShowLastDetailed", show_last_detailed), FI("WideSegs", wide_segs),
    FB("WideSegBlanks", wide_seg_blanks), FB("ShowLastWide", show_last_wide),
    FI("WideHeight", wide_height), FI("WideSegWidth", wide_seg_width),
    FI("SegmentIcons", segment_icons), FB("ShowTitle", show_title), FB("ShowGoal", show_goal),
    FB("ShowAttempts", show_attempts), FB("ShowPrevSeg", show_prev_seg),
    FB("ShowTimeSave", show_time_save), FB("ShowSoB", show_sob), FB("PredPB", pred_pb),
    FB("PredBest", pred_best), FS("FontFamilySegments", font_family),
    FD("FontMultiplierSegments", font_multiplier), FB("EnabledHotkeys", hotkeys_enabled),
    FS("LastFile", last_file),
};

static void clamp(WsSettings *s)
{
#define CL(v, lo, hi) do { if ((v) < (lo)) (v) = (lo); if ((v) > (hi)) (v) = (hi); } while (0)
    CL(s->compare_against, 0, 3);
    CL(s->double_tap_guard_ms, 0, 5000);
    CL(s->refresh_ms, 5, 1000);
    CL(s->display_mode, 0, 3);
    CL(s->display_segs, 2, 40);
    CL(s->wide_segs, 1, 20);
    CL(s->segment_icons, 0, 3);
    CL(s->clock_w, 60, 4000);
    CL(s->clock_h, 20, 2000);
    CL(s->wide_height, 20, 200);
    CL(s->wide_seg_width, 60, 1000);
    CL(s->opacity, 0.1, 1.0);
    CL(s->font_multiplier, 0.5, 4.0);
#undef CL
}

bool ws_settings_load(WsSettings *s, const char *path)
{
    FILE *f = ws_fopen(path, "r");
    if (!f) return false;
    char line[WS_PATH_MAX + 128];
    s->recent_count = 0;
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';' || *l == '[') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);

        bool handled = false;
        for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
            const Field *fd = &fields[i];
            if (strcmp(fd->key, k)) continue;
            char *p = (char *)s + fd->off;
            switch (fd->ty) {
            case T_BOOL: *(bool *)p = parse_bool(v); break;
            case T_INT: *(int *)p = atoi(v); break;
            case T_DBL: *(double *)p = strtod(v, NULL); break;
            case T_STR: snprintf(p, fd->sz, "%s", v); break;
            }
            handled = true;
            break;
        }
        if (handled) continue;
        for (int i = 0; i < WS_HK_COUNT; i++) {
            if (!strcmp(k, hk_keys[i])) {
                snprintf(s->hotkeys[i], sizeof s->hotkeys[i], "%s", v);
                handled = true;
            }
        }
        if (handled) continue;
        if (!strncmp(k, "Recent", 6) && s->recent_count < WS_MAX_RECENT && *v) {
            snprintf(s->recent[s->recent_count++], WS_PATH_MAX, "%s", v);
            continue;
        }
        apply_color(&s->colors, k, v);
    }
    fclose(f);
    clamp(s);
    return true;
}

bool ws_settings_save(const WsSettings *s, const char *path)
{
    char tmp[WS_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = ws_fopen(tmp, "w");
    if (!f) return false;
    fputs("# WSplit settings\n[General]\n", f);
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const Field *fd = &fields[i];
        const char *p = (const char *)s + fd->off;
        switch (fd->ty) {
        case T_BOOL: fprintf(f, "%s=%s\n", fd->key, *(const bool *)p ? "true" : "false"); break;
        case T_INT: fprintf(f, "%s=%d\n", fd->key, *(const int *)p); break;
        case T_DBL: fprintf(f, "%s=%g\n", fd->key, *(const double *)p); break;
        case T_STR: fprintf(f, "%s=%s\n", fd->key, p); break;
        }
    }
    fputs("\n[Hotkeys]\n", f);
    for (int i = 0; i < WS_HK_COUNT; i++) fprintf(f, "%s=%s\n", hk_keys[i], s->hotkeys[i]);
    fputs("\n[Recent]\n", f);
    for (int i = 0; i < s->recent_count; i++) fprintf(f, "Recent%d=%s\n", i, s->recent[i]);
    fputs("\n[Colors]\n", f);
    write_colors(f, &s->colors);
    bool ok = fclose(f) == 0;
    if (ok) ok = ws_replace_file(tmp, path) == 0;
    return ok;
}

void ws_settings_add_recent(WsSettings *s, const char *path)
{
    if (!path || !*path) return;
    int found = -1;
    for (int i = 0; i < s->recent_count; i++)
        if (!strcmp(s->recent[i], path)) { found = i; break; }
    if (found < 0) {
        found = s->recent_count < WS_MAX_RECENT ? s->recent_count++ : WS_MAX_RECENT - 1;
    }
    for (int i = found; i > 0; i--) memcpy(s->recent[i], s->recent[i - 1], WS_PATH_MAX);
    snprintf(s->recent[0], WS_PATH_MAX, "%s", path);
}

void ws_settings_clear_recent(WsSettings *s) { s->recent_count = 0; }

bool ws_colors_save(const WsColors *c, const char *path)
{
    FILE *f = ws_fopen(path, "w");
    if (!f) return false;
    fputs("# WSplit colour scheme\n[Colors]\n", f);
    write_colors(f, c);
    return fclose(f) == 0;
}

bool ws_colors_load(WsColors *c, const char *path)
{
    FILE *f = ws_fopen(path, "r");
    if (!f) return false;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line), *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        if (apply_color(c, trim(l), trim(eq + 1))) n++;
    }
    fclose(f);
    return n > 0;
}

/* ------------------------------------------------------------ hotkeys */

bool ws_hotkey_parse(const char *s, unsigned *mods, char *key, size_t keylen)
{
    *mods = 0;
    if (key && keylen) key[0] = 0;
    if (!s || !*s || !strcasecmp(s, "None")) return false;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    char *tok = buf;
    while (tok) {
        char *plus = strchr(tok, '+');
        /* A literal '+' key: "Ctrl++" */
        if (plus && plus[1] == 0 && plus == tok) plus = NULL;
        if (plus) *plus = 0;
        char *t = trim(tok);
        if (plus) {
            if (!strcasecmp(t, "Ctrl") || !strcasecmp(t, "Control")) *mods |= WS_MOD_CTRL;
            else if (!strcasecmp(t, "Alt") || !strcasecmp(t, "Option")) *mods |= WS_MOD_ALT;
            else if (!strcasecmp(t, "Shift")) *mods |= WS_MOD_SHIFT;
            else if (!strcasecmp(t, "Cmd") || !strcasecmp(t, "Super") || !strcasecmp(t, "Win")) *mods |= WS_MOD_SUPER;
            else return false;
            tok = plus + 1;
        } else {
            if (!*t) return false;
            snprintf(key, keylen, "%s", t);
            return true;
        }
    }
    return false;
}

void ws_hotkey_format(char *out, size_t n, unsigned mods, const char *key)
{
    snprintf(out, n, "%s%s%s%s%s", (mods & WS_MOD_CTRL) ? "Ctrl+" : "", (mods & WS_MOD_ALT) ? "Alt+" : "",
             (mods & WS_MOD_SHIFT) ? "Shift+" : "", (mods & WS_MOD_SUPER) ? "Cmd+" : "", key ? key : "");
}
