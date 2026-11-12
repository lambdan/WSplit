/* .wsplit load/save (port of WSplit.loadFile/saveFile) and LiveSplit import. */
#define _POSIX_C_SOURCE 200809L
#include "wsplit.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *read_all(const char *path, size_t *len_out, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || (size_t)st.st_size > max) { fclose(f); return NULL; }
    size_t len = (size_t)st.st_size;
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

/* Port of IsTextFile: reject files with NUL bytes / lots of control chars. */
static bool is_text(const char *buf, size_t len)
{
    size_t ctrl = 0;
    for (size_t i = 0; i < len && i < 8192; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) return false;
        if (c < 8 || (c > 13 && c < 32)) ctrl++;
    }
    return ctrl * 10 < (len < 8192 ? len : 8192) + 1;
}

static int parse_int(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    while (*end == ' ' || *end == '\r') end++;
    return *end ? 0 : (int)v; /* int.TryParse failure -> 0 */
}

static int count_char(const char *s, char c)
{
    int n = 0;
    for (; *s; s++) if (*s == c) n++;
    return n;
}

bool ws_runfile_load(WsRun *r, const char *path)
{
    size_t len = 0;
    char *buf = read_all(path, &len, 10u * 1024u * 1024u);
    ws_run_clear(r);
    ws_run_set_title(r, "");
    ws_run_set_goal(r, "");
    r->attempts = 0;
    r->start_delay_ms = 0;
    r->pref_w = r->pref_h = 0;
    if (!buf) return false;
    if (!is_text(buf, len)) { free(buf); return false; }

    char *icons_line = NULL;
    char *p = buf;
    /* skip UTF-8 BOM */
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;

    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r') p[--l] = 0;

        if (strncmp(p, "Title=", 6) == 0) {
            ws_run_set_title(r, p + 6);
        } else if (strncmp(p, "Goal=", 5) == 0) {
            ws_run_set_goal(r, p + 5);
        } else if (strncmp(p, "Attempts=", 9) == 0) {
            r->attempts = parse_int(p + 9);
        } else if (strncmp(p, "Offset=", 7) == 0) {
            r->start_delay_ms = parse_int(p + 7);
        } else if (strncmp(p, "Width=", 6) == 0) {
            r->pref_w = parse_int(p + 6);
            r->pref_h = 0;
        } else if (strncmp(p, "Size=", 5) == 0 && count_char(p, ',') == 1) {
            char *c = strchr(p, ',');
            *c = 0;
            r->pref_w = parse_int(p + 5);
            r->pref_h = parse_int(c + 1);
        } else if (strncmp(p, "Icons=", 6) == 0) {
            icons_line = p + 6;
        } else if (count_char(p, ',') == 3) {
            char *f[4];
            f[0] = p;
            for (int i = 1; i < 4; i++) { f[i] = strchr(f[i - 1], ','); *f[i]++ = 0; }
            double v[3];
            for (int i = 0; i < 3; i++) {
                bool ok = false;
                if (strchr(f[i + 1], ':')) { v[i] = ws_parse_time(f[i + 1]); ok = true; }
                else v[i] = ws_parse_double(f[i + 1], &ok);
                if (!ok) v[i] = 0.0;
            }
            ws_run_add(r, f[0], v[0], v[1], v[2]);
        }
        p = nl ? nl + 1 : NULL;
    }

    /* Icons="a","b",... split on '",' then strip quotes */
    if (icons_line) {
        int idx = 0;
        char *q = icons_line;
        while (q && idx < r->count) {
            char *sep = strstr(q, "\",");
            if (sep) *sep = 0;
            char tmp[WS_PATH_MAX];
            size_t o = 0;
            for (char *c = q; *c && o + 1 < sizeof tmp; c++) if (*c != '"') tmp[o++] = *c;
            tmp[o] = 0;
            ws_seg_set_icon_path(&r->segs[idx++], tmp);
            q = sep ? sep + 2 : NULL;
        }
    }

    free(buf);
    ws_run_set_file(r, path);
    r->unsaved = false;
    return true;
}

bool ws_runfile_save(const WsRun *r, const char *path, char *err, size_t errlen)
{
    char tmp_path[WS_PATH_MAX + 8];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        if (err) snprintf(err, errlen, "Could not write %s: %s", path, strerror(errno));
        return false;
    }
    fprintf(f, "Title=%s\r\n", r->title ? r->title : "");
    fprintf(f, "Goal=%s\r\n", r->goal ? r->goal : "");
    fprintf(f, "Attempts=%d\r\n", r->attempts);
    fprintf(f, "Offset=%d\r\n", r->start_delay_ms);
    fprintf(f, "Size=%d,%d\r\n", r->pref_w, r->pref_h);
    for (int i = 0; i < r->count; i++) {
        const WsSegment *s = &r->segs[i];
        if (!s->name) continue;
        char a[40], b[40], c[40];
        ws_print_double(a, sizeof a, s->old_time);
        ws_print_double(b, sizeof b, s->best_time);
        ws_print_double(c, sizeof c, s->best_segment);
        fprintf(f, "%s,%s,%s,%s\r\n", s->name, a, b, c);
    }
    fputs("Icons=", f);
    for (int i = 0; i < r->count; i++) {
        if (!r->segs[i].name) continue;
        fprintf(f, "%s\"%s\"", i ? "," : "", r->segs[i].icon_path ? r->segs[i].icon_path : "");
    }
    fputs("\r\n", f);
    if (fclose(f) != 0 || rename(tmp_path, path) != 0) {
        if (err) snprintf(err, errlen, "Could not write %s: %s", path, strerror(errno));
        remove(tmp_path);
        return false;
    }
    return true;
}

/* ----------------------------------------------------------- LiveSplit */

static void xml_unescape(char *s)
{
    static const struct { const char *e; char c; } ents[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    char *w = s;
    for (char *r = s; *r;) {
        bool done = false;
        if (*r == '&') {
            for (size_t i = 0; i < sizeof ents / sizeof ents[0]; i++) {
                size_t l = strlen(ents[i].e);
                if (strncmp(r, ents[i].e, l) == 0) { *w++ = ents[i].c; r += l; done = true; break; }
            }
        }
        if (!done) *w++ = *r++;
    }
    *w = 0;
}

/* Copy inner text of the first <tag ...>...</tag> found in [p, end). */
static bool xml_child(const char *p, const char *end, const char *tag, char *out, size_t n,
                      const char **after)
{
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    size_t ol = strlen(open);
    for (const char *q = p; q && q < end; q++) {
        q = strstr(q, open);
        if (!q || q >= end) return false;
        char nc = q[ol];
        if (nc != '>' && nc != ' ' && nc != '/' && nc != '\t' && nc != '\n' && nc != '\r') continue;
        const char *gt = strchr(q, '>');
        if (!gt || gt >= end) return false;
        if (gt[-1] == '/') { /* <Tag/> */
            if (out && n) out[0] = 0;
            if (after) *after = gt + 1;
            return true;
        }
        const char *c = strstr(gt + 1, close);
        if (!c || c > end) return false;
        if (out) {
            size_t l = (size_t)(c - (gt + 1));
            if (l >= n) l = n - 1;
            memcpy(out, gt + 1, l);
            out[l] = 0;
            xml_unescape(out);
        }
        if (after) *after = c + strlen(close);
        return true;
    }
    return false;
}

static double trunc_cs(double v) { return trunc(v * 100.0) / 100.0; }

static double lss_time(const char *s)
{
    /* hh:mm:ss.fffffff, possibly with a day prefix "d.hh:mm:ss" */
    if (!s || !*s) return 0.0;
    int neg = (*s == '-');
    if (neg) s++;
    double t = ws_parse_time(s);
    return neg ? -t : t;
}

bool ws_runfile_import_lss(WsRun *r, const char *path)
{
    size_t len = 0;
    char *buf = read_all(path, &len, 50u * 1024u * 1024u);
    if (!buf) return false;
    const char *end = buf + len;
    if (!strstr(buf, "<Run")) { free(buf); return false; }

    ws_run_clear(r);
    char game[512] = "", cat[512] = "", tmp[512] = "";
    xml_child(buf, end, "GameName", game, sizeof game, NULL);
    xml_child(buf, end, "CategoryName", cat, sizeof cat, NULL);
    char title[1100];
    snprintf(title, sizeof title, "%s%s%s", game, *game && *cat ? " " : "", cat);
    ws_run_set_title(r, title);
    ws_run_set_goal(r, "");
    r->attempts = 0;
    if (xml_child(buf, end, "AttemptCount", tmp, sizeof tmp, NULL)) r->attempts = atoi(tmp);
    r->start_delay_ms = 0;
    if (xml_child(buf, end, "Offset", tmp, sizeof tmp, NULL) && tmp[0] == '-')
        r->start_delay_ms = (int)(ws_parse_time(tmp + 1) * 1000.0 + 0.5);

    const char *segs_start = strstr(buf, "<Segments>");
    const char *segs_end = segs_start ? strstr(segs_start, "</Segments>") : NULL;
    if (segs_start && segs_end) {
        const char *q = segs_start;
        while (1) {
            const char *s = strstr(q, "<Segment>");
            if (!s || s >= segs_end) break;
            const char *e = strstr(s, "</Segment>");
            if (!e || e > segs_end) break;
            char name[512] = "", pb[128] = "", best[128] = "";
            xml_child(s, e, "Name", name, sizeof name, NULL);
            /* Personal Best split: <SplitTime name="Personal Best"><RealTime> */
            const char *pbtag = strstr(s, "name=\"Personal Best\"");
            if (pbtag && pbtag < e) {
                const char *pe = strstr(pbtag, "</SplitTime>");
                if (pe && pe < e) xml_child(pbtag, pe, "RealTime", pb, sizeof pb, NULL);
            }
            const char *bs = strstr(s, "<BestSegmentTime");
            if (bs && bs < e) {
                const char *be = strstr(bs, "</BestSegmentTime>");
                if (be && be < e) xml_child(bs, be, "RealTime", best, sizeof best, NULL);
            }
            ws_run_add(r, name, 0.0, trunc_cs(lss_time(pb)), trunc_cs(lss_time(best)));
            q = e + 10;
        }
    }
    free(buf);
    ws_run_set_file(r, NULL);
    r->unsaved = true;
    return r->count > 0;
}
