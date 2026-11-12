/* Text-grid form of a run's segments, used by the Windows and Linux run editors:
 *
 *   # Segment | Old time | Best time | Best segment | Icon
 *   Bob-omb Battlefield | 1:18.32 | 1:16.10 | 1:14.85 |
 *
 * Times use h:mm:ss.ff (or plain seconds); empty or "-" means none. */
#define _POSIX_C_SOURCE 200809L
#include "wsplit.h"

#include <stdlib.h>
#include <string.h>

typedef struct { char *p; size_t len, cap; } Buf;

static void put(Buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t nc = (b->cap + n + 1) * 2;
        char *np = realloc(b->p, nc);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}

static void put_time(Buf *b, double t)
{
    char tmp[48];
    if (t > 0) { ws_format_edit_time(tmp, sizeof tmp, t); put(b, tmp); }
}

char *ws_run_segments_to_text(const WsRun *r)
{
    Buf b = {0};
    put(&b, "# Segment | Old time | Best time | Best segment | Icon\n");
    for (int i = 0; i < r->count; i++) {
        const WsSegment *s = &r->segs[i];
        put(&b, s->name ? s->name : "");
        put(&b, " | ");
        put_time(&b, s->old_time);
        put(&b, " | ");
        put_time(&b, s->best_time);
        put(&b, " | ");
        put_time(&b, s->best_segment);
        put(&b, " | ");
        put(&b, s->icon_path ? s->icon_path : "");
        put(&b, "\n");
    }
    if (!b.p) return ws_strdup("");
    return b.p;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static double cell_time(char *s)
{
    s = trim(s);
    if (!*s || !strcmp(s, "-")) return 0.0;
    return ws_parse_time(s);
}

int ws_run_segments_from_text(WsRun *r, const char *text)
{
    /* keep title/goal/attempts/delay; replace the segments */
    for (int i = 0; i < r->count; i++) {
        free(r->segs[i].name);
        free(r->segs[i].icon_path);
    }
    r->count = 0;
    char *copy = ws_strdup(text ? text : "");
    char *save = NULL;
    for (char *line = copy; line; line = save) {
        char *nl = strchr(line, '\n');
        save = nl ? nl + 1 : NULL;
        if (nl) *nl = 0;
        char *l = trim(line);
        if (!*l || *l == '#') continue;

        /* split on '|' */
        char *cols[64];
        int n = 0;
        cols[n++] = l;
        for (char *c = l; *c && n < 64; c++)
            if (*c == '|') { *c = 0; cols[n++] = c + 1; }

        /* Name may itself contain '|': the last 4 columns are fixed */
        char *icon = "", *bseg = "", *best = "", *old = "";
        int name_cols = n;
        if (n >= 5) { icon = cols[n - 1]; bseg = cols[n - 2]; best = cols[n - 3]; old = cols[n - 4]; name_cols = n - 4; }
        else if (n == 4) { bseg = cols[3]; best = cols[2]; old = cols[1]; name_cols = 1; }
        else if (n == 3) { best = cols[2]; old = cols[1]; name_cols = 1; }
        else if (n == 2) { old = cols[1]; name_cols = 1; }
        char name[1024] = "";
        size_t o = 0;
        for (int k = 0; k < name_cols; k++) {
            const char *part = cols[k];
            if (k && o + 1 < sizeof name) name[o++] = '|';
            for (const char *c = part; *c && o + 1 < sizeof name; c++)
                name[o++] = (*c == ',') ? ' ' : *c; /* commas would break .wsplit */
            name[o] = 0;
        }
        char *nm = trim(name);
        if (!*nm) continue;
        WsSegment *s = ws_run_add(r, nm, cell_time(old), cell_time(best), cell_time(bseg));
        if (s) ws_seg_set_icon_path(s, trim(icon));
    }
    free(copy);
    ws_run_recompute_old_faster(r);
    return r->count;
}
