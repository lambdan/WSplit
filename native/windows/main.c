/* WSplit for Windows: Win32 + GDI+ (flat API) front-end.
 *
 * Borderless popup window like the original WinForms form. Rendering goes
 * through a GDI+ painter into a memory DC; the core works in 96-dpi units
 * and the painter scales by the monitor DPI (per-monitor v2).
 * Built with MinGW-w64: see the Makefile target `windows`. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#include "../core/wsplit.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_APP_OPENFILE (WM_APP + 1)
#define TIMER_ID 1

static struct {
    WsApp app;
    HINSTANCE inst;
    HWND hwnd;
    bool ready;
    double scale; /* DPI / 96 */
    UINT timer_ms;

    GpFontFamily *fam_display;
    GpFontFamily *fam_digit;
    GpFontCollection *private_fonts;
    GpFont *font[WS_FONT_COUNT];
    double fsize[WS_FONT_COUNT];
    double ascent[WS_FONT_COUNT], descent[WS_FONT_COUNT];
    GpFont *ui_bold;
    GpStringFormat *fmt;

    bool hk_registered[WS_HK_COUNT];
    HCURSOR cursor;
} ui;

static void run_command(int id);

/* ================================================================== */
/* UTF-8 <-> UTF-16                                                    */
/* ================================================================== */

static wchar_t *wide(const char *s)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void wide_to(const wchar_t *w, char *out, size_t n)
{
    if (!n) return;
    int r = WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, out, (int)n, NULL, NULL);
    if (r <= 0) out[0] = 0;
    out[n - 1] = 0;
}

static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, NULL, 0, NULL, NULL);
    char *s = malloc((size_t)(n > 0 ? n : 1));
    if (n <= 0) { s[0] = 0; return s; }
    WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, s, n, NULL, NULL);
    return s;
}

/* "\n" -> "\r\n" for edit controls */
static char *to_crlf(const char *s)
{
    size_t n = 0;
    for (const char *p = s; *p; p++) n += (*p == '\n') ? 2 : 1;
    char *o = malloc(n + 1), *q = o;
    for (const char *p = s; *p; p++) {
        if (*p == '\n' && (p == s || p[-1] != '\r')) *q++ = '\r';
        *q++ = *p;
    }
    *q = 0;
    return o;
}

/* ================================================================== */
/* Painter (GDI+)                                                      */
/* ================================================================== */

typedef struct {
    GpGraphics *g;
    GraphicsState stack[64];
    int depth;
} PaintCtx;

static void p_fill_rect(void *ctx, double x, double y, double w, double h, WsColor c)
{
    PaintCtx *pc = ctx;
    GpSolidFill *b;
    GdipCreateSolidFill(c, &b);
    GdipFillRectangle(pc->g, (GpBrush *)b, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
    GdipDeleteBrush((GpBrush *)b);
}

static void p_fill_gradient(void *ctx, double x, double y, double w, double h, WsColor c1, WsColor c2, bool horiz)
{
    PaintCtx *pc = ctx;
    if (w <= 0 || h <= 0) return;
    GpPointF a, b;
    if (horiz) { a.X = (REAL)x; a.Y = 0; b.X = (REAL)(x + w); b.Y = 0; }
    else { a.X = 0; a.Y = (REAL)(y + h); b.X = 0; b.Y = (REAL)y; }
    GpLineGradient *br;
    if (GdipCreateLineBrush(&a, &b, c1, c2, WrapModeTileFlipXY, &br) != Ok) return;
    GdipFillRectangle(pc->g, (GpBrush *)br, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
    GdipDeleteBrush((GpBrush *)br);
}

static void p_stroke_line(void *ctx, double x1, double y1, double x2, double y2, WsColor c)
{
    PaintCtx *pc = ctx;
    GpPen *pen;
    GdipCreatePen1(c, 1.0f, UnitPixel, &pen);
    GdipDrawLine(pc->g, pen, (REAL)x1, (REAL)y1, (REAL)x2, (REAL)y2);
    GdipDeletePen(pen);
}

static void p_draw_text(void *ctx, WsFont f, const char *s, double x, double y, WsColor c)
{
    PaintCtx *pc = ctx;
    if (!ui.font[f] || !s || !*s) return;
    wchar_t *w = wide(s);
    GpSolidFill *b;
    GdipCreateSolidFill(c, &b);
    RectF r = {(REAL)x, (REAL)y, 0, 0};
    GdipDrawString(pc->g, w, -1, ui.font[f], &r, ui.fmt, (GpBrush *)b);
    GdipDeleteBrush((GpBrush *)b);
    free(w);
}

static double measure(GpGraphics *g, GpFont *font, const char *s)
{
    if (!font || !s || !*s) return 0;
    wchar_t *w = wide(s);
    RectF lay = {0, 0, 100000.0f, 10000.0f}, box = {0};
    GdipMeasureString(g, w, -1, font, &lay, ui.fmt, &box, NULL, NULL);
    free(w);
    return box.Width;
}

static double p_text_width(void *ctx, WsFont f, const char *s) { return measure(((PaintCtx *)ctx)->g, ui.font[f], s); }

static void p_font_metrics(void *ctx, WsFont f, double *ascent, double *descent)
{
    (void)ctx;
    *ascent = ui.ascent[f];
    *descent = ui.descent[f];
}

static double p_font_size(void *ctx, WsFont f) { (void)ctx; return ui.fsize[f]; }

static void push_state(PaintCtx *pc)
{
    GraphicsState st = 0;
    GdipSaveGraphics(pc->g, &st);
    if (pc->depth < 64) pc->stack[pc->depth] = st;
    pc->depth++;
}

static void p_pop(void *ctx)
{
    PaintCtx *pc = ctx;
    if (pc->depth <= 0) return;
    pc->depth--;
    if (pc->depth < 64) GdipRestoreGraphics(pc->g, pc->stack[pc->depth]);
}

static void p_push_clip(void *ctx, double x, double y, double w, double h)
{
    PaintCtx *pc = ctx;
    push_state(pc);
    GdipSetClipRect(pc->g, (REAL)x, (REAL)y, (REAL)w, (REAL)h, CombineModeIntersect);
}

static void p_push_transform(void *ctx, double tx, double ty, double s)
{
    PaintCtx *pc = ctx;
    push_state(pc);
    GdipTranslateWorldTransform(pc->g, (REAL)tx, (REAL)ty, MatrixOrderPrepend);
    GdipScaleWorldTransform(pc->g, (REAL)s, (REAL)s, MatrixOrderPrepend);
}

static void draw_placeholder_icon(GpGraphics *g, double x, double y, double s, double alpha)
{
    BYTE a = (BYTE)(alpha * 255);
    GpSolidFill *b;
    GdipCreateSolidFill(((ARGB)a << 24) | 0x59595F, &b);
    GdipFillRectangle(g, (GpBrush *)b, (REAL)(x + 1), (REAL)(y + 1), (REAL)(s - 2), (REAL)(s - 2));
    GdipDeleteBrush((GpBrush *)b);
    GpPen *pen;
    GdipCreatePen1(((ARGB)a << 24) | 0x9999A6, 1.0f, UnitPixel, &pen);
    GdipDrawRectangle(g, pen, (REAL)(x + 1), (REAL)(y + 1), (REAL)(s - 2), (REAL)(s - 2));
    GdipDeletePen(pen);
    if (!ui.fam_display) return;
    GpFont *f;
    if (GdipCreateFont(ui.fam_display, (REAL)(s * 0.7), FontStyleBold, UnitPixel, &f) != Ok) return;
    GpStringFormat *sf;
    GdipCloneStringFormat(ui.fmt, &sf);
    GdipSetStringFormatAlign(sf, StringAlignmentCenter);
    GdipSetStringFormatLineAlign(sf, StringAlignmentCenter);
    GdipCreateSolidFill(((ARGB)a << 24) | 0xD9D9E6, &b);
    RectF r = {(REAL)x, (REAL)y, (REAL)s, (REAL)s};
    GdipDrawString(g, L"?", 1, f, &r, sf, (GpBrush *)b);
    GdipDeleteBrush((GpBrush *)b);
    GdipDeleteStringFormat(sf);
    GdipDeleteFont(f);
}

static void p_draw_icon(void *ctx, void *icon, double x, double y, double size, double alpha, bool gray)
{
    PaintCtx *pc = ctx;
    GpBitmap *bm = icon;
    if (!bm) { draw_placeholder_icon(pc->g, x, y, size, alpha); return; }
    UINT w = 0, h = 0;
    GdipGetImageWidth((GpImage *)bm, &w);
    GdipGetImageHeight((GpImage *)bm, &h);
    if (!w || !h) return;
    /* Segment.Icon setter's grey copy: 4 parts luminance + 1 part colour */
    ColorMatrix m = {{{0}}};
    if (gray) {
        const float lr = 0.3f * 0.8f, lg = 0.59f * 0.8f, lb = 0.11f * 0.8f;
        for (int o = 0; o < 3; o++) { m.m[0][o] = lr; m.m[1][o] = lg; m.m[2][o] = lb; m.m[o][o] += 0.2f; }
    } else {
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    }
    m.m[3][3] = (REAL)alpha;
    m.m[4][4] = 1;
    GpImageAttributes *ia;
    GdipCreateImageAttributes(&ia);
    GdipSetImageAttributesColorMatrix(ia, ColorAdjustTypeDefault, TRUE, &m, NULL, ColorMatrixFlagsDefault);
    GdipDrawImageRectRect(pc->g, (GpImage *)bm, (REAL)x, (REAL)y, (REAL)size, (REAL)size, 0, 0, (REAL)w, (REAL)h,
                          UnitPixel, ia, NULL, NULL);
    GdipDisposeImageAttributes(ia);
}

static const WsPainter *painter_for(PaintCtx *pc)
{
    static WsPainter p;
    p.ctx = pc;
    p.fill_rect = p_fill_rect;
    p.fill_gradient = p_fill_gradient;
    p.stroke_line = p_stroke_line;
    p.draw_text = p_draw_text;
    p.text_width = p_text_width;
    p.font_metrics = p_font_metrics;
    p.font_size = p_font_size;
    p.push_clip = p_push_clip;
    p.pop_clip = p_pop;
    p.push_transform = p_push_transform;
    p.pop_transform = p_pop;
    p.draw_icon = p_draw_icon;
    return &p;
}

static void setup_graphics(GpGraphics *g)
{
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAliasGridFit);
    GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
    GdipSetPageUnit(g, UnitPixel);
}

static void *h_load_icon(void *ud, const char *path)
{
    (void)ud;
    wchar_t *w = wide(path);
    GpBitmap *bm = NULL;
    GpStatus st = GdipCreateBitmapFromFile(w, &bm);
    free(w);
    if (st != Ok || !bm) return NULL;
    /* copy into memory so the file isn't kept locked */
    UINT iw = 0, ih = 0;
    GdipGetImageWidth((GpImage *)bm, &iw);
    GdipGetImageHeight((GpImage *)bm, &ih);
    GpBitmap *copy = NULL;
    if (GdipCreateBitmapFromScan0((INT)iw, (INT)ih, 0, PixelFormat32bppARGB, NULL, &copy) == Ok) {
        GpGraphics *g;
        GdipGetImageGraphicsContext((GpImage *)copy, &g);
        GdipDrawImageRectI(g, (GpImage *)bm, 0, 0, (INT)iw, (INT)ih);
        GdipDeleteGraphics(g);
        GdipDisposeImage((GpImage *)bm);
        return copy;
    }
    return bm;
}

static void h_free_icon(void *ud, void *icon)
{
    (void)ud;
    if (icon) GdipDisposeImage((GpImage *)icon);
}

/* ================================================================== */
/* Fonts                                                               */
/* ================================================================== */

static void load_digit_font(void)
{
    if (ui.private_fonts) return;
    HRSRC r = FindResourceW(ui.inst, MAKEINTRESOURCEW(IDR_CLOCKFONT), (LPCWSTR)RT_RCDATA);
    HGLOBAL h = r ? LoadResource(ui.inst, r) : NULL;
    void *data = h ? LockResource(h) : NULL;
    DWORD size = r ? SizeofResource(ui.inst, r) : 0;
    if (GdipNewPrivateFontCollection(&ui.private_fonts) != Ok) return;
    if (data && size) GdipPrivateAddMemoryFont(ui.private_fonts, data, (INT)size);
    INT n = 0, found = 0;
    GdipGetFontCollectionFamilyCount(ui.private_fonts, &n);
    if (n > 0) GdipGetFontCollectionFamilyList(ui.private_fonts, 1, &ui.fam_digit, &found);
    if (found < 1) ui.fam_digit = NULL;
}

static GpFontFamily *family_or_default(const char *name)
{
    GpFontFamily *f = NULL;
    if (name && *name) {
        wchar_t *w = wide(name);
        GdipCreateFontFamilyFromName(w, NULL, &f);
        free(w);
    }
    if (!f) GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &f);
    if (!f) GdipGetGenericFontFamilySansSerif(&f);
    return f;
}

static void make_font(WsFont which, GpFontFamily *fam, double px, INT style)
{
    if (ui.font[which]) GdipDeleteFont(ui.font[which]);
    ui.font[which] = NULL;
    ui.fsize[which] = px;
    if (!fam || GdipCreateFont(fam, (REAL)px, style, UnitPixel, &ui.font[which]) != Ok) return;
    /* line box = cell ascent + descent, scaled to the em size */
    UINT16 em = 0, asc = 0, desc = 0;
    GdipGetEmHeight(fam, style, &em);
    GdipGetCellAscent(fam, style, &asc);
    GdipGetCellDescent(fam, style, &desc);
    if (!em) em = 2048;
    ui.ascent[which] = px * asc / em;
    ui.descent[which] = px * desc / em;
}

static void rebuild_fonts(void)
{
    const WsSettings *s = &ui.app.settings;
    double m = s->font_multiplier > 0 ? s->font_multiplier : 1.0;
    if (ui.fam_display) GdipDeleteFontFamily(ui.fam_display);
    ui.fam_display = family_or_default(s->font_family);
    make_font(WS_FONT_DISPLAY, ui.fam_display, 10.66667 * m, FontStyleRegular);
    make_font(WS_FONT_TIME, ui.fam_display, 12.0 * m, FontStyleBold);
    make_font(WS_FONT_CLOCK_LARGE, ui.fam_display, 22.66667, FontStyleBold);
    make_font(WS_FONT_CLOCK_MED, ui.fam_display, 18.66667, FontStyleBold);
    load_digit_font();
    GpFontFamily *dig = ui.fam_digit;
    GpFontFamily *mono = NULL;
    if (!dig) { GdipGetGenericFontFamilyMonospace(&mono); dig = mono; }
    make_font(WS_FONT_DIGIT_LARGE, dig, 24.0, ui.fam_digit ? FontStyleRegular : FontStyleBold);
    make_font(WS_FONT_DIGIT_MED, dig, 17.33333, ui.fam_digit ? FontStyleRegular : FontStyleBold);
    if (ui.hwnd) InvalidateRect(ui.hwnd, NULL, FALSE);
}

/* ================================================================== */
/* Drawing                                                             */
/* ================================================================== */

static void paint(HDC hdc)
{
    RECT rc;
    GetClientRect(ui.hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    GpGraphics *g;
    if (GdipCreateFromHDC(mem, &g) == Ok) {
        setup_graphics(g);
        GdipGraphicsClear(g, 0xFF000000);
        GdipScaleWorldTransform(g, (REAL)ui.scale, (REAL)ui.scale, MatrixOrderPrepend);
        PaintCtx pc = {g, {0}, 0};
        ws_render(&ui.app, painter_for(&pc));
        GdipDeleteGraphics(g);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void redraw(void)
{
    if (ui.hwnd) InvalidateRect(ui.hwnd, NULL, FALSE);
}

static bool render_png(const char *path)
{
    int w = ui.app.win_w, h = ui.app.win_h;
    GpBitmap *bm;
    if (GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppARGB, NULL, &bm) != Ok) return false;
    GpGraphics *g;
    GdipGetImageGraphicsContext((GpImage *)bm, &g);
    setup_graphics(g);
    PaintCtx pc = {g, {0}, 0};
    ws_render(&ui.app, painter_for(&pc));
    GdipDeleteGraphics(g);
    static const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    wchar_t *wp = wide(path);
    bool ok = GdipSaveImageToFile((GpImage *)bm, wp, &png, NULL) == Ok;
    free(wp);
    GdipDisposeImage((GpImage *)bm);
    return ok;
}

/* ================================================================== */
/* Keys                                                                */
/* ================================================================== */

static const struct { UINT vk; const char *name; } key_names[] = {
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_TAB, "Tab"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"},
    {VK_DELETE, "Delete"}, {VK_INSERT, "Insert"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"},
    {VK_NEXT, "PageDown"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_UP, "Up"}, {VK_DOWN, "Down"},
    {VK_PAUSE, "Pause"}, {VK_SNAPSHOT, "PrintScreen"}, {VK_SCROLL, "ScrollLock"}, {VK_OEM_MINUS, "Minus"},
    {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LeftBracket"}, {VK_OEM_6, "RightBracket"}, {VK_OEM_1, "Semicolon"},
    {VK_OEM_7, "Quote"}, {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
    {VK_OEM_5, "Backslash"}, {VK_OEM_3, "Grave"}, {VK_ADD, "NumpadAdd"}, {VK_SUBTRACT, "NumpadSubtract"},
    {VK_MULTIPLY, "NumpadMultiply"}, {VK_DIVIDE, "NumpadDivide"}, {VK_DECIMAL, "NumpadDecimal"},
};

static bool vk_name(UINT vk, bool extended, char *out, size_t n)
{
    if (vk >= 'A' && vk <= 'Z') { snprintf(out, n, "%c", (int)vk); return true; }
    if (vk >= '0' && vk <= '9') { snprintf(out, n, "%c", (int)vk); return true; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(out, n, "F%d", (int)(vk - VK_F1 + 1)); return true; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, n, "Numpad%d", (int)(vk - VK_NUMPAD0)); return true; }
    if (vk == VK_RETURN && extended) { snprintf(out, n, "NumpadEnter"); return true; }
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (key_names[i].vk == vk) { snprintf(out, n, "%s", key_names[i].name); return true; }
    return false;
}

static UINT name_vk(const char *name)
{
    if (!name || !*name) return 0;
    if (!name[1] && name[0] >= 'A' && name[0] <= 'Z') return (UINT)name[0];
    if (!name[1] && name[0] >= 'a' && name[0] <= 'z') return (UINT)(name[0] - 'a' + 'A');
    if (!name[1] && name[0] >= '0' && name[0] <= '9') return (UINT)name[0];
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '0' && name[1] <= '9') {
        int k = atoi(name + 1);
        if (k >= 1 && k <= 24) return VK_F1 + (UINT)(k - 1);
    }
    if (!strncmp(name, "Numpad", 6) && name[6] >= '0' && name[6] <= '9' && !name[7]) return VK_NUMPAD0 + (UINT)(name[6] - '0');
    if (!_stricmp(name, "NumpadEnter") || !_stricmp(name, "Return")) return VK_RETURN;
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (!_stricmp(key_names[i].name, name)) return key_names[i].vk;
    return 0;
}

/* GetMessageTime() has GetTickCount resolution (10-16 ms), so only trust
   latencies that are clearly larger than one tick. */
static int64_t msg_latency_ns(void)
{
    DWORD d = GetTickCount() - (DWORD)GetMessageTime();
    if (getenv("WSPLIT_DEBUG_LATENCY")) {
        char b[64];
        snprintf(b, sizeof b, "event latency: %lu ms\n", (unsigned long)d);
        OutputDebugStringA(b);
    }
    return (d > 32 && d < 1000) ? (int64_t)d * 1000000 : 0;
}

static unsigned current_mods(void)
{
    unsigned m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= WS_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= WS_MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= WS_MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= WS_MOD_SUPER;
    return m;
}

static void cmd_exit(void);

static bool on_keydown(WPARAM vk, LPARAM lp)
{
    if (lp & (1 << 30)) return true; /* auto-repeat */
    char name[64];
    if (!vk_name((UINT)vk, (lp >> 24) & 1, name, sizeof name)) return false;
    unsigned mods = current_mods();
    if (mods & (WS_MOD_CTRL | WS_MOD_ALT | WS_MOD_SUPER)) {
        if (mods != WS_MOD_CTRL) return false;
        if (!strcmp(name, "O")) run_command(WS_CMD_OPEN);
        else if (!strcmp(name, "S")) ws_app_save(&ui.app);
        else if (!strcmp(name, "E")) run_command(WS_CMD_EDIT_RUN);
        else if (!strcmp(name, "Q")) cmd_exit();
        else return false;
        redraw();
        return true;
    }
    ws_app_set_input_latency(&ui.app, msg_latency_ns());
    bool handled = ws_app_local_key(&ui.app, name);
    redraw();
    return handled;
}

/* ---------------------------------------------------- global hotkeys */

static void ui_message(const char *title, const char *msg);

static void unregister_hotkeys(void)
{
    for (int i = 0; i < WS_HK_COUNT; i++) {
        if (ui.hk_registered[i]) UnregisterHotKey(ui.hwnd, i + 1);
        ui.hk_registered[i] = false;
    }
}

static void h_hotkeys_changed(void *ud)
{
    (void)ud;
    if (!ui.hwnd) return;
    unregister_hotkeys();
    const WsSettings *s = &ui.app.settings;
    char failed[512] = "";
    for (int i = 0; i < WS_HK_COUNT; i++) {
        if (!s->hotkeys[i][0]) continue;
        if (i != WS_HK_TOGGLE && !s->hotkeys_enabled) continue;
        unsigned mods;
        char key[64];
        if (!ws_hotkey_parse(s->hotkeys[i], &mods, key, sizeof key)) continue;
        UINT vk = name_vk(key);
        if (!vk) continue;
        UINT wm = MOD_NOREPEAT;
        if (mods & WS_MOD_CTRL) wm |= MOD_CONTROL;
        if (mods & WS_MOD_ALT) wm |= MOD_ALT;
        if (mods & WS_MOD_SHIFT) wm |= MOD_SHIFT;
        if (mods & WS_MOD_SUPER) wm |= MOD_WIN;
        if (RegisterHotKey(ui.hwnd, i + 1, wm, vk)) {
            ui.hk_registered[i] = true;
        } else {
            size_t l = strlen(failed);
            snprintf(failed + l, sizeof failed - l, "%s%s", l ? ", " : "", s->hotkeys[i]);
        }
    }
    if (failed[0]) {
        char msg[700];
        snprintf(msg, sizeof msg, "These hotkeys are already taken by another application:\n%s", failed);
        ui_message("Global hotkeys", msg);
    }
    redraw();
}

/* ================================================================== */
/* Dialogs                                                             */
/* ================================================================== */

static int msgbox(const char *title, const char *msg, UINT flags)
{
    wchar_t *t = wide(title), *m = wide(msg);
    bool was = ui.app.modal_open;
    ui.app.modal_open = true;
    int r = MessageBoxW(ui.hwnd, m, t, flags);
    ui.app.modal_open = was;
    free(t);
    free(m);
    return r;
}

static void ui_message(const char *title, const char *msg) { msgbox(title, msg, MB_OK | MB_ICONINFORMATION); }

static WsAnswer ui_ask(const char *title, const char *msg, bool cancel)
{
    int r = msgbox(title, msg, (cancel ? MB_YESNOCANCEL : MB_YESNO) | MB_ICONQUESTION);
    return r == IDYES ? WS_ANSWER_YES : r == IDNO ? WS_ANSWER_NO : WS_ANSWER_CANCEL;
}

/* filter: "Label|*.a;*.b" */
static bool ui_choose_file(bool save, const char *title, const char *filter, const char *def_ext,
                           const char *initial_name, char *out, size_t n)
{
    wchar_t file[WS_PATH_MAX] = L"";
    if (initial_name) {
        wchar_t *w = wide(initial_name);
        wcsncpy(file, w, WS_PATH_MAX - 1);
        free(w);
    }
    /* build "Label\0pattern\0All files\0*.*\0\0" */
    wchar_t flt[512];
    size_t o = 0;
    if (filter) {
        const char *bar = strchr(filter, '|');
        char label[200], pat[200];
        snprintf(label, sizeof label, "%.*s", bar ? (int)(bar - filter) : (int)strlen(filter), filter);
        snprintf(pat, sizeof pat, "%s", bar ? bar + 1 : "*.*");
        wchar_t *wl = wide(label), *wpat = wide(pat);
        o += (size_t)swprintf(flt + o, 512 - o, L"%ls", wl) + 1;
        o += (size_t)swprintf(flt + o, 512 - o, L"%ls", wpat) + 1;
        free(wl);
        free(wpat);
    }
    o += (size_t)swprintf(flt + o, 512 - o, L"All files") + 1;
    o += (size_t)swprintf(flt + o, 512 - o, L"*.*") + 1;
    flt[o] = 0;

    wchar_t *wt = wide(title), *wext = def_ext ? wide(def_ext) : NULL;
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = ui.hwnd;
    ofn.lpstrFilter = flt;
    ofn.lpstrFile = file;
    ofn.nMaxFile = WS_PATH_MAX;
    ofn.lpstrTitle = wt;
    ofn.lpstrDefExt = wext;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    bool was = ui.app.modal_open;
    ui.app.modal_open = true;
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    ui.app.modal_open = was;
    free(wt);
    free(wext);
    if (!ok) return false;
    wide_to(file, out, n);
    return true;
}

/* ---- prompt dialog (IDD_PROMPT) */

typedef struct {
    const char *title, *msg, *check_label;
    char *buf;
    size_t n;
    bool *check;
} PromptData;

static void set_dlg_text(HWND d, int id, const char *s)
{
    wchar_t *w = wide(s);
    SetDlgItemTextW(d, id, w);
    free(w);
}

static char *get_dlg_text(HWND d, int id)
{
    HWND c = GetDlgItem(d, id);
    int len = GetWindowTextLengthW(c);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(len + 1));
    GetWindowTextW(c, w, len + 1);
    char *s = narrow(w);
    free(w);
    return s;
}

static INT_PTR CALLBACK prompt_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    PromptData *pd = (PromptData *)GetWindowLongPtrW(d, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG:
        pd = (PromptData *)lp;
        SetWindowLongPtrW(d, DWLP_USER, (LONG_PTR)pd);
        { wchar_t *t = wide(pd->title); SetWindowTextW(d, t); free(t); }
        set_dlg_text(d, IDC_PROMPT_TEXT, pd->msg);
        set_dlg_text(d, IDC_PROMPT_EDIT, pd->buf);
        if (pd->check_label) {
            set_dlg_text(d, IDC_PROMPT_CHECK, pd->check_label);
            CheckDlgButton(d, IDC_PROMPT_CHECK, *pd->check ? BST_CHECKED : BST_UNCHECKED);
        } else {
            ShowWindow(GetDlgItem(d, IDC_PROMPT_CHECK), SW_HIDE);
        }
        SendDlgItemMessageW(d, IDC_PROMPT_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(d, IDC_PROMPT_EDIT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            char *s = get_dlg_text(d, IDC_PROMPT_EDIT);
            snprintf(pd->buf, pd->n, "%s", s);
            free(s);
            if (pd->check_label) *pd->check = IsDlgButtonChecked(d, IDC_PROMPT_CHECK) == BST_CHECKED;
            EndDialog(d, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static INT_PTR modal_dialog(int id, DLGPROC proc, void *data)
{
    bool was = ui.app.modal_open;
    ui.app.modal_open = true;
    INT_PTR r = DialogBoxParamW(ui.inst, MAKEINTRESOURCEW(id), ui.hwnd, proc, (LPARAM)data);
    ui.app.modal_open = was;
    return r;
}

static bool ui_prompt(const char *title, const char *msg, char *buf, size_t n, const char *check_label, bool *check)
{
    PromptData pd = {title, msg, check_label, buf, n, check};
    return modal_dialog(IDD_PROMPT, prompt_proc, &pd) == IDOK;
}

/* ---- hotkey dialog (IDD_HOTKEY) */

typedef struct {
    int which;
    char result[64];
} HotkeyData;

static INT_PTR CALLBACK hotkey_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    HotkeyData *hd = (HotkeyData *)GetWindowLongPtrW(d, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        hd = (HotkeyData *)lp;
        SetWindowLongPtrW(d, DWLP_USER, (LONG_PTR)hd);
        char m[200];
        snprintf(m, sizeof m, "Press the key combination for \"%s\".", ws_hotkey_labels[hd->which]);
        set_dlg_text(d, IDC_HOTKEY_TEXT, m);
        HWND hk = GetDlgItem(d, IDC_HOTKEY_CTRL);
        SendMessageW(hk, HKM_SETRULES, 0, 0); /* allow keys without modifiers */
        unsigned mods;
        char key[64];
        if (ws_hotkey_parse(ui.app.settings.hotkeys[hd->which], &mods, key, sizeof key)) {
            UINT vk = name_vk(key);
            WORD f = 0;
            if (mods & WS_MOD_CTRL) f |= HOTKEYF_CONTROL;
            if (mods & WS_MOD_ALT) f |= HOTKEYF_ALT;
            if (mods & WS_MOD_SHIFT) f |= HOTKEYF_SHIFT;
            if (!strcmp(key, "NumpadEnter")) f |= HOTKEYF_EXT;
            SendMessageW(hk, HKM_SETHOTKEY, MAKEWORD(vk, f), 0);
            CheckDlgButton(d, IDC_HOTKEY_WIN, (mods & WS_MOD_SUPER) ? BST_CHECKED : BST_UNCHECKED);
        }
        SetFocus(hk);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_HOTKEY_CLEAR:
            hd->result[0] = 0;
            EndDialog(d, IDOK);
            return TRUE;
        case IDOK: {
            LRESULT v = SendDlgItemMessageW(d, IDC_HOTKEY_CTRL, HKM_GETHOTKEY, 0, 0);
            UINT vk = LOBYTE(LOWORD(v)), f = HIBYTE(LOWORD(v));
            char name[64];
            hd->result[0] = 0;
            if (vk && vk_name(vk, (f & HOTKEYF_EXT) != 0, name, sizeof name)) {
                unsigned mods = 0;
                if (f & HOTKEYF_CONTROL) mods |= WS_MOD_CTRL;
                if (f & HOTKEYF_ALT) mods |= WS_MOD_ALT;
                if (f & HOTKEYF_SHIFT) mods |= WS_MOD_SHIFT;
                if (IsDlgButtonChecked(d, IDC_HOTKEY_WIN) == BST_CHECKED) mods |= WS_MOD_SUPER;
                ws_hotkey_format(hd->result, sizeof hd->result, mods, name);
            }
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---- run editor (IDD_RUNEDIT) */

static HFONT mono_font;

static void re_set_segs(HWND d, const char *text)
{
    char *crlf = to_crlf(text ? text : "");
    set_dlg_text(d, IDC_RE_SEGS, crlf);
    free(crlf);
}

static INT_PTR CALLBACK runedit_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
    WsApp *a = &ui.app;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        char num[32], delay[32];
        set_dlg_text(d, IDC_RE_TITLE, a->run.title);
        set_dlg_text(d, IDC_RE_GOAL, a->run.goal);
        snprintf(num, sizeof num, "%d", a->run.attempts);
        set_dlg_text(d, IDC_RE_ATTEMPTS, num);
        ws_format_edit_time(delay, sizeof delay, a->run.start_delay_ms / 1000.0);
        set_dlg_text(d, IDC_RE_DELAY, delay);
        set_dlg_text(d, IDC_RE_HELP,
                     "One segment per line:  Name | Old time | Best time | Best segment | Icon path\n"
                     "Times are h:mm:ss.ff or seconds; leave empty for none. Lines starting with # are ignored.");
        if (!mono_font) {
            HDC dc = GetDC(d);
            int h = -MulDiv(10, GetDeviceCaps(dc, LOGPIXELSY), 72);
            ReleaseDC(d, dc);
            mono_font = CreateFontW(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                    FIXED_PITCH | FF_MODERN, L"Consolas");
        }
        SendDlgItemMessageW(d, IDC_RE_SEGS, WM_SETFONT, (WPARAM)mono_font, TRUE);
        SendDlgItemMessageW(d, IDC_RE_SEGS, EM_SETLIMITTEXT, 0, 0);
        if (ws_run_live(&a->run)) {
            char *t = ws_run_segments_to_text(&a->run);
            re_set_segs(d, t);
            free(t);
        } else {
            re_set_segs(d, "# Segment | Old time | Best time | Best segment | Icon\n"
                           "Segment 1 | | | |\nSegment 2 | | | |\nSegment 3 | | | |\n");
            SetWindowTextW(d, L"New Run");
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_RE_AUTOFILL: {
            if (ui_ask("Auto-fill best segments", "Fill missing or slower best segments from the best splits?", false) !=
                WS_ANSWER_YES)
                return TRUE;
            char *t = get_dlg_text(d, IDC_RE_SEGS);
            WsRun tmp;
            ws_run_init(&tmp);
            ws_run_segments_from_text(&tmp, t);
            free(t);
            ws_run_autofill_best_segments(&tmp);
            char *nt = ws_run_segments_to_text(&tmp);
            re_set_segs(d, nt);
            free(nt);
            ws_run_free(&tmp);
            return TRUE;
        }
        case IDOK: {
            char *text = get_dlg_text(d, IDC_RE_SEGS);
            char *title = get_dlg_text(d, IDC_RE_TITLE);
            char *goal = get_dlg_text(d, IDC_RE_GOAL);
            char *att = get_dlg_text(d, IDC_RE_ATTEMPTS);
            char *dl = get_dlg_text(d, IDC_RE_DELAY);
            char *file = a->run.file ? ws_strdup(a->run.file) : NULL;
            int pw = a->detail_w, ph = a->detail_h;
            ws_app_close_file(a); /* frees icons; clears the path, restored below */
            ws_run_set_title(&a->run, title);
            ws_run_set_goal(&a->run, goal);
            int n = atoi(att);
            a->run.attempts = n < 0 ? 0 : n;
            double dv = ws_parse_time(dl);
            a->run.start_delay_ms = dv > 0 ? (int)llround(dv * 1000.0) : 0;
            ws_run_segments_from_text(&a->run, text);
            if (file) ws_run_set_file(&a->run, file);
            a->detail_w = pw;
            a->detail_h = ph;
            ws_app_run_edited(a);
            free(text); free(title); free(goal); free(att); free(dl); free(file);
            EndDialog(d, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(d, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void cmd_edit_run(void)
{
    WsApp *a = &ui.app;
    /* WSplit updates bests before editing and restores them on cancel */
    ws_run_update_best(&a->run, a->settings.best_as_overall);
    if (modal_dialog(IDD_RUNEDIT, runedit_proc, NULL) != IDOK) ws_run_restore_best(&a->run);
    redraw();
}

/* ================================================================== */
/* Host callbacks                                                      */
/* ================================================================== */

static WsAnswer h_ask(void *ud, const char *t, const char *m, bool c) { (void)ud; return ui_ask(t, m, c); }
static void h_message(void *ud, const char *t, const char *m) { (void)ud; ui_message(t, m); }

static bool h_choose_save_path(void *ud, char *out, size_t n)
{
    (void)ud;
    char def[300];
    snprintf(def, sizeof def, "%s.wsplit", ui.app.run.title[0] ? ui.app.run.title : "splits");
    for (char *p = def; *p; p++)
        if (strchr("\\/:*?\"<>|", *p)) *p = '_';
    return ui_choose_file(true, "Save splits", "WSplit runs|*.wsplit;*.txt", "wsplit", def, out, n);
}

static void h_resize_window(void *ud, int w, int h)
{
    (void)ud;
    if (!ui.hwnd || w <= 0 || h <= 0) return;
    SetWindowPos(ui.hwnd, NULL, 0, 0, (int)lround(w * ui.scale), (int)lround(h * ui.scale),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    redraw();
}

static void h_invalidate(void *ud) { (void)ud; redraw(); }

static void h_apply_window_props(void *ud)
{
    (void)ud;
    if (!ui.hwnd) return;
    const WsSettings *s = &ui.app.settings;
    SetWindowPos(ui.hwnd, s->on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    double op = s->opacity > 0.05 && s->opacity < 1.0 ? s->opacity : 1.0;
    SetLayeredWindowAttributes(ui.hwnd, 0, (BYTE)lround(op * 255), LWA_ALPHA);
}

/* ================================================================== */
/* Commands                                                            */
/* ================================================================== */

static void load_or_complain(const char *path)
{
    if (!ws_app_load_file(&ui.app, path)) ui_message("Open", "That file could not be opened as a WSplit run.");
    redraw();
}

static void cmd_open(void)
{
    if (!ws_app_prompt_for_save(&ui.app)) return;
    char path[WS_PATH_MAX];
    if (!ui_choose_file(false, "Open splits", "WSplit runs|*.wsplit;*.txt", NULL, NULL, path, sizeof path)) return;
    load_or_complain(path);
}

static void cmd_import_lss(void)
{
    WsApp *a = &ui.app;
    if (!ws_app_prompt_for_save(a)) return;
    char path[WS_PATH_MAX];
    if (!ui_choose_file(false, "Import LiveSplit splits", "LiveSplit splits|*.lss", NULL, NULL, path, sizeof path)) return;
    ws_app_close_file(a);
    if (!ws_runfile_import_lss(&a->run, path)) {
        ws_app_close_file(a);
        ui_message("Import", "No segments could be read from that LiveSplit file.");
        return;
    }
    ws_app_run_edited(a);
    ui_message("Import", "Splits imported. Use Save As... to keep them as a .wsplit file.");
}

static void cmd_start_at(void)
{
    char buf[64] = "0:00";
    bool use_delay = false;
    if (!ui_prompt("Start at...", "Start offset (e.g. 1:30 or 12.5):", buf, sizeof buf, "Still use delay", &use_delay))
        return;
    ws_app_start_at(&ui.app, ws_parse_time(buf), use_delay);
}

static void cmd_start_delay(void)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%g", ui.app.run.start_delay_ms / 1000.0);
    if (!ui_prompt("Run start delay", "Delay before the timer starts, in seconds\n(shown as a negative countdown):", buf,
                   sizeof buf, NULL, NULL))
        return;
    double d = ws_parse_time(buf);
    ui.app.run.start_delay_ms = d > 0 ? (int)llround(d * 1000.0) : 0;
    ui.app.run.unsaved = true;
    ws_app_initialize_display(&ui.app);
}

static void cmd_font(void)
{
    LOGFONTW lf = {0};
    wchar_t *cur = wide(ui.app.settings.font_family[0] ? ui.app.settings.font_family : "Segoe UI");
    wcsncpy(lf.lfFaceName, cur, LF_FACESIZE - 1);
    free(cur);
    lf.lfHeight = -11;
    CHOOSEFONTW cf = {0};
    cf.lStructSize = sizeof cf;
    cf.hwndOwner = ui.hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL | CF_NOVERTFONTS;
    bool was = ui.app.modal_open;
    ui.app.modal_open = true;
    BOOL ok = ChooseFontW(&cf);
    ui.app.modal_open = was;
    if (!ok) return;
    wide_to(lf.lfFaceName, ui.app.settings.font_family, sizeof ui.app.settings.font_family);
    rebuild_fonts();
    ws_app_layout_changed(&ui.app);
}

static void cmd_set_hotkey(int which)
{
    HotkeyData hd = {which, ""};
    if (modal_dialog(IDD_HOTKEY, hotkey_proc, &hd) != IDOK) return;
    snprintf(ui.app.settings.hotkeys[which], sizeof ui.app.settings.hotkeys[which], "%s", hd.result);
    if (hd.result[0] && which != WS_HK_TOGGLE && !ui.app.settings.hotkeys_enabled) {
        if (ui_ask("Global hotkeys", "Global hotkeys are currently disabled. Enable them now?", false) == WS_ANSWER_YES)
            ui.app.settings.hotkeys_enabled = true;
    }
    h_hotkeys_changed(NULL);
}

static void cmd_colors(bool save)
{
    char path[WS_PATH_MAX];
    if (!ui_choose_file(save, save ? "Save color scheme" : "Load color scheme", "WSplit colors|*.wscolors;*.ini",
                        save ? "wscolors" : NULL, save ? "colors.wscolors" : NULL, path, sizeof path))
        return;
    if (save) {
        if (!ws_colors_save(&ui.app.settings.colors, path)) ui_message("Colors", "Could not save the color scheme.");
    } else {
        WsColors c = ui.app.settings.colors;
        if (ws_colors_load(&c, path)) { ui.app.settings.colors = c; ws_app_colors_changed(&ui.app); }
        else ui_message("Colors", "No colors could be read from that file.");
    }
}

static void cmd_exit(void)
{
    if (ws_app_prompt_for_save(&ui.app)) DestroyWindow(ui.hwnd);
}

static void run_command(int id)
{
    if (id >= WS_CMD_SET_HOTKEY_BASE && id < WS_CMD_SET_HOTKEY_BASE + WS_HK_COUNT) {
        cmd_set_hotkey(id - WS_CMD_SET_HOTKEY_BASE);
        return;
    }
    switch (id) {
    case WS_CMD_EDIT_RUN: case WS_CMD_NEW_RUN: cmd_edit_run(); break;
    case WS_CMD_OPEN: cmd_open(); break;
    case WS_CMD_IMPORT_LSS: cmd_import_lss(); break;
    case WS_CMD_START_AT: cmd_start_at(); break;
    case WS_CMD_START_DELAY: cmd_start_delay(); break;
    case WS_CMD_FONT: cmd_font(); break;
    case WS_CMD_FONTS_CHANGED: rebuild_fonts(); break;
    case WS_CMD_COLORS_LOAD: cmd_colors(false); break;
    case WS_CMD_COLORS_SAVE: cmd_colors(true); break;
    case WS_CMD_ABOUT:
        ui_message("About WSplit",
                   "WSplit " WS_VERSION " (native Windows build)\n\n"
                   "Speedrun timer originally by Wodanaz (SDA),\n"
                   "updated by Nitrofski and contributors.\n"
                   "Native port of the open-source C# version.\n\n"
                   "Clock font: Digital-7 Mono by Style-7\n(freeware for personal use).");
        break;
    case WS_CMD_EXIT: cmd_exit(); break;
    default: break;
    }
    redraw();
}

/* ================================================================== */
/* Context menu                                                        */
/* ================================================================== */

static HMENU build_hmenu(const WsMenu *m)
{
    HMENU h = CreatePopupMenu();
    for (int i = 0; i < m->count; i++) {
        const WsMenuItem *it = &m->items[i];
        if (it->kind == WS_MI_SEPARATOR) { AppendMenuW(h, MF_SEPARATOR, 0, NULL); continue; }
        /* '&' is the mnemonic marker: escape it */
        char label[700];
        size_t o = 0;
        for (const char *p = it->label; *p && o + 3 < sizeof label; p++) {
            if (*p == '&') label[o++] = '&';
            label[o++] = *p;
        }
        label[o] = 0;
        if (it->shortcut[0]) snprintf(label + o, sizeof label - o, "\t%s", it->shortcut);
        wchar_t *w = wide(label);
        if (it->kind == WS_MI_SUBMENU) {
            HMENU sub = it->sub ? build_hmenu(it->sub) : CreatePopupMenu();
            AppendMenuW(h, MF_POPUP | (it->enabled ? 0 : MF_GRAYED), (UINT_PTR)sub, w);
        } else {
            UINT f = MF_STRING | (it->enabled ? 0 : MF_GRAYED) | (it->checked ? MF_CHECKED : 0);
            AppendMenuW(h, f, (UINT_PTR)it->id, w);
            if (it->kind == WS_MI_RADIO) {
                MENUITEMINFOW mi = {0};
                mi.cbSize = sizeof mi;
                mi.fMask = MIIM_FTYPE;
                mi.fType = MFT_STRING | MFT_RADIOCHECK;
                SetMenuItemInfoW(h, (UINT)it->id, FALSE, &mi);
            }
        }
        free(w);
    }
    return h;
}

static void show_menu(int sx, int sy)
{
    WsMenu *m = ws_menu_build(&ui.app, WS_MENU_PLATFORM_WINDOWS);
    HMENU h = build_hmenu(m);
    ws_menu_free(m);
    SetForegroundWindow(ui.hwnd);
    int id = (int)TrackPopupMenu(h, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, sx, sy, 0, ui.hwnd, NULL);
    DestroyMenu(h);
    PostMessageW(ui.hwnd, WM_NULL, 0, 0);
    if (id > 0) {
        int r = ws_menu_activate(&ui.app, id);
        if (r != WS_CMD_NONE) run_command(r);
    }
    redraw();
}

/* ================================================================== */
/* Mouse: move / resize like the original borderless form              */
/* ================================================================== */

enum { DRAG_NONE, DRAG_MOVE, DRAG_CLOCK, DRAG_DETAIL_SEGS, DRAG_WIDE_SEGS, DRAG_WIDE_SEG_WIDTH };
#define EDGE_L 1
#define EDGE_R 2
#define EDGE_T 4
#define EDGE_B 8

static struct {
    int mode, edges;
    POINT start_screen;
    RECT start_win;
    int start_w, start_h;
    double ref;
    int start_val;
} drag;

static int hit_test(int x, int y, int *edges)
{
    WsApp *a = &ui.app;
    WsRect c = a->clock;
    int cl = c.x, cr = c.x + c.w, ct = c.y, cb = c.y + c.h;
    bool wide_mode = a->cur_mode == WS_MODE_WIDE;
    *edges = 0;
    if (wide_mode && abs(x - (cr + a->wide_seg_w - 3)) < 4) return DRAG_WIDE_SEG_WIDTH;
    if (wide_mode && abs(x - (a->win_w - 117)) < 4) return DRAG_WIDE_SEGS;
    if (a->cur_mode == WS_MODE_DETAILED && abs(y - (ct - 1)) < 4) return DRAG_DETAIL_SEGS;
    if (x >= cr - 5 && x <= cr) {
        *edges = EDGE_R;
        if (!wide_mode && y >= cb - 5 && y <= cb) *edges |= EDGE_B;
        else if (!wide_mode && y <= ct + 5 && y >= ct) *edges |= EDGE_T;
        return DRAG_CLOCK;
    }
    if (x <= cl + 5 && x >= cl) {
        *edges = EDGE_L;
        if (!wide_mode && y >= cb - 5 && y <= cb) *edges |= EDGE_B;
        else if (!wide_mode && y <= ct + 5 && y >= ct) *edges |= EDGE_T;
        return DRAG_CLOCK;
    }
    if (!wide_mode && y >= cb - 5 && y <= cb) { *edges = EDGE_B; return DRAG_CLOCK; }
    if (!wide_mode && y <= ct + 5 && y >= ct) { *edges = EDGE_T; return DRAG_CLOCK; }
    return DRAG_MOVE;
}

static LPCWSTR cursor_for(int mode, int edges)
{
    switch (mode) {
    case DRAG_WIDE_SEG_WIDTH: case DRAG_WIDE_SEGS: return IDC_SIZEWE;
    case DRAG_DETAIL_SEGS: return IDC_SIZENS;
    case DRAG_CLOCK:
        if (edges == (EDGE_R | EDGE_B) || edges == (EDGE_L | EDGE_T)) return IDC_SIZENWSE;
        if (edges == (EDGE_L | EDGE_B) || edges == (EDGE_R | EDGE_T)) return IDC_SIZENESW;
        if (edges & (EDGE_L | EDGE_R)) return IDC_SIZEWE;
        return IDC_SIZENS;
    default: return IDC_ARROW;
    }
}

/* client pixels -> core (96 dpi) units */
static int to_core(int v) { return (int)lround(v / ui.scale); }

static void on_lbutton_down(int x, int y)
{
    int edges;
    drag.mode = hit_test(to_core(x), to_core(y), &edges);
    drag.edges = edges;
    if (drag.mode == DRAG_MOVE) {
        drag.mode = DRAG_NONE;
        ReleaseCapture();
        SendMessageW(ui.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return;
    }
    GetCursorPos(&drag.start_screen);
    GetWindowRect(ui.hwnd, &drag.start_win);
    drag.start_w = ui.app.win_w;
    drag.start_h = ui.app.win_h;
    drag.ref = drag.mode == DRAG_DETAIL_SEGS ? to_core(y) : to_core(x);
    drag.start_val = ui.app.settings.wide_seg_width;
    SetCapture(ui.hwnd);
}

static void on_mouse_move(int x, int y)
{
    WsApp *a = &ui.app;
    if (drag.mode == DRAG_NONE) return;
    POINT p;
    GetCursorPos(&p);
    int dx = to_core(p.x - drag.start_screen.x), dy = to_core(p.y - drag.start_screen.y);
    switch (drag.mode) {
    case DRAG_CLOCK: {
        int w = drag.start_w, h = drag.start_h;
        if (drag.edges & EDGE_R) w += dx;
        if (drag.edges & EDGE_L) w -= dx;
        if (drag.edges & EDGE_B) h += dy;
        if (drag.edges & EDGE_T) h -= dy;
        ws_app_user_resized(a, w, h);
        if (drag.edges & (EDGE_L | EDGE_T)) {
            /* keep the opposite edge where it was */
            RECT r;
            GetWindowRect(ui.hwnd, &r);
            int nx = drag.start_win.left, ny = drag.start_win.top;
            if (drag.edges & EDGE_L) nx = drag.start_win.right - (r.right - r.left);
            if (drag.edges & EDGE_T) ny = drag.start_win.bottom - (r.bottom - r.top);
            SetWindowPos(ui.hwnd, NULL, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        break;
    }
    case DRAG_DETAIL_SEGS: {
        int cy = to_core(y), sh = a->seg_height;
        if (cy >= drag.ref + sh) {
            int before = a->settings.display_segs;
            ws_app_segs_inc(a);
            if (a->settings.display_segs != before) drag.ref += sh;
        } else if (cy <= drag.ref - sh && a->settings.display_segs > 2) {
            ws_app_segs_dec(a);
            drag.ref -= sh;
        }
        break;
    }
    case DRAG_WIDE_SEGS: {
        int cx = to_core(x), sw = a->wide_seg_w;
        if (cx >= drag.ref + sw) {
            int before = a->settings.wide_segs;
            ws_app_segs_inc(a);
            if (a->settings.wide_segs != before) drag.ref += sw;
        } else if (cx <= drag.ref - sw && a->settings.wide_segs > 1) {
            ws_app_segs_dec(a);
            drag.ref -= sw;
        }
        break;
    }
    case DRAG_WIDE_SEG_WIDTH: {
        int v = drag.start_val + dx;
        a->settings.wide_seg_width = v < 60 ? 60 : v;
        ws_app_relayout(a);
        break;
    }
    }
    redraw();
}

/* ================================================================== */
/* Window procedure                                                    */
/* ================================================================== */

static void arm_timer(void)
{
    UINT ms = (UINT)(ui.app.settings.refresh_ms > 0 ? ui.app.settings.refresh_ms : 15);
    if (ms == ui.timer_ms) return;
    ui.timer_ms = ms;
    SetTimer(ui.hwnd, TIMER_ID, ms, NULL);
}

static void save_window_pos(void)
{
    if (!ui.app.settings.save_window_pos) return;
    RECT r;
    if (GetWindowRect(ui.hwnd, &r)) {
        ui.app.settings.window_x = r.left;
        ui.app.settings.window_y = r.top;
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (ui.ready) paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == TIMER_ID && ui.ready) {
            if (ws_app_tick(&ui.app)) redraw();
            arm_timer();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (ui.ready && on_keydown(wp, lp)) return 0;
        break;
    case WM_HOTKEY:
        if (ui.ready && wp >= 1 && wp <= WS_HK_COUNT) {
            ws_app_set_input_latency(&ui.app, msg_latency_ns());
            ws_app_hotkey(&ui.app, (WsHotkeyAction)(wp - 1));
            redraw();
        }
        return 0;
    case WM_LBUTTONDOWN:
        on_lbutton_down((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_MOUSEMOVE:
        on_mouse_move((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP:
        if (drag.mode != DRAG_NONE) { drag.mode = DRAG_NONE; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED:
        drag.mode = DRAG_NONE;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && drag.mode == DRAG_NONE) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            int edges, m = hit_test(to_core(p.x), to_core(p.y), &edges);
            SetCursor(LoadCursorW(NULL, cursor_for(m, edges)));
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x == -1 && y == -1) { /* keyboard: menu key */
            RECT r;
            GetWindowRect(h, &r);
            x = r.left;
            y = r.top;
        }
        show_menu(x, y);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        wchar_t p[WS_PATH_MAX];
        if (DragQueryFileW(d, 0, p, WS_PATH_MAX)) {
            char path[WS_PATH_MAX];
            wide_to(p, path, sizeof path);
            DragFinish(d);
            SetForegroundWindow(h);
            if (ws_app_prompt_for_save(&ui.app)) load_or_complain(path);
        } else {
            DragFinish(d);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        ui.scale = HIWORD(wp) / 96.0;
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, (int)lround(ui.app.win_w * ui.scale), (int)lround(ui.app.win_h * ui.scale),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        redraw();
        return 0;
    }
    case WM_CLOSE:
        cmd_exit();
        return 0;
    case WM_QUERYENDSESSION:
        return ws_app_prompt_for_save(&ui.app);
    case WM_DESTROY:
        if (ui.ready) {
            save_window_pos();
            unregister_hotkeys();
            KillTimer(h, TIMER_ID);
            ws_app_shutdown(&ui.app);
            ui.ready = false;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ================================================================== */
/* Startup                                                             */
/* ================================================================== */

static void settings_path(char *out, size_t n)
{
    wchar_t *appdata = NULL;
    out[0] = 0;
    if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata) == S_OK && appdata) {
        wchar_t dir[WS_PATH_MAX];
        swprintf(dir, WS_PATH_MAX, L"%ls\\WSplit", appdata);
        CreateDirectoryW(dir, NULL);
        char d[WS_PATH_MAX];
        wide_to(dir, d, sizeof d);
        snprintf(out, n, "%s\\settings.ini", d);
    }
    if (appdata) CoTaskMemFree(appdata);
}

static void init_core(const char *settings)
{
    wchar_t dec[8] = L".";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, 8);
    WsHost host = {0};
    host.ask = h_ask;
    host.message = h_message;
    host.choose_save_path = h_choose_save_path;
    host.resize_window = h_resize_window;
    host.invalidate = h_invalidate;
    host.hotkeys_changed = h_hotkeys_changed;
    host.load_icon = h_load_icon;
    host.free_icon = h_free_icon;
    host.apply_window_props = h_apply_window_props;
    ws_app_init(&ui.app, &host, settings);
    wide_to(dec, ui.app.decimal_char, sizeof ui.app.decimal_char);
    rebuild_fonts();
}

static int screenshot_mode(const char *out, int mode, int sim)
{
    WsApp *a = &ui.app;
    if (mode >= 0) ws_app_set_display(a, (WsDisplayMode)mode);
    if (sim > 0 && ws_run_live(&a->run)) {
        a->run.attempts++;
        double t = 0;
        for (int i = 0; i < sim && i < a->run.count; i++) {
            double best = a->run.segs[i].best_time - (i ? a->run.segs[i - 1].best_time : 0);
            if (best <= 0) best = 60;
            t += best + ((i % 3 == 1) ? 2.37 : -1.12);
            ws_run_do_split(&a->run, trunc(t * 100) / 100);
        }
        double prev = a->run.live_index > 0 ? a->run.segs[a->run.live_index - 1].live_time : 0;
        double extra = 20.0;
        if (!ws_run_done(&a->run)) {
            double comp = ws_run_comp_time(&a->run);
            if (comp > prev) extra = (comp - prev) * 0.6;
        }
        a->timer.accumulated_ns = (int64_t)((ws_run_done(&a->run) ? prev : prev + extra) * 1e9);
        a->timer.start_ns = ws_now_ns();
        a->timer.running = true;
        a->refreshing = false;
        ws_app_update_display(a);
    }
    return render_png(out) ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    ui.inst = inst;
    ui.scale = 1.0;

    int argc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char shot[WS_PATH_MAX] = "", file[WS_PATH_MAX] = "";
    int shot_mode = -1, sim = 0;
    for (int i = 1; i < argc; i++) {
        char a[WS_PATH_MAX];
        wide_to(wargv[i], a, sizeof a);
        if (!strcmp(a, "--screenshot") && i + 1 < argc) wide_to(wargv[++i], shot, sizeof shot);
        else if (!strcmp(a, "--mode") && i + 1 < argc) { wide_to(wargv[++i], a, sizeof a); shot_mode = atoi(a); }
        else if (!strcmp(a, "--simulate") && i + 1 < argc) { wide_to(wargv[++i], a, sizeof a); sim = atoi(a); }
        else if (a[0] != '-') snprintf(file, sizeof file, "%s", a);
    }
    LocalFree(wargv);

    GdiplusStartupInput gsi = {1, NULL, FALSE, FALSE};
    ULONG_PTR gtoken;
    if (GdiplusStartup(&gtoken, &gsi, NULL) != Ok) {
        MessageBoxW(NULL, L"GDI+ could not be initialised.", L"WSplit", MB_ICONERROR);
        return 1;
    }
    GdipStringFormatGetGenericTypographic(&ui.fmt);
    GdipCloneStringFormat(ui.fmt, &ui.fmt);
    INT flags = 0;
    GdipGetStringFormatFlags(ui.fmt, &flags);
    GdipSetStringFormatFlags(ui.fmt, flags | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);

    if (shot[0]) {
        init_core(NULL);
        if (file[0] && !ws_app_load_file(&ui.app, file)) return 1;
        int r = screenshot_mode(shot, shot_mode, sim);
        GdiplusShutdown(gtoken);
        return r;
    }

    OleInitialize(NULL);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    char sp[WS_PATH_MAX];
    settings_path(sp, sizeof sp);
    init_core(sp[0] ? sp : NULL);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"WSplitNative";
    RegisterClassExW(&wc);

    const WsSettings *s = &ui.app.settings;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (s->save_window_pos) {
        POINT p = {s->window_x, s->window_y};
        if (MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) { x = p.x; y = p.y; }
    }
    ui.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_ACCEPTFILES, wc.lpszClassName, L"WSplit",
                              WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, x, y, 240, 50, NULL, NULL, inst, NULL);
    if (!ui.hwnd) return 1;
    UINT dpi = GetDpiForWindow(ui.hwnd);
    ui.scale = dpi ? dpi / 96.0 : 1.0;
    ui.ready = true;

    if (file[0]) load_or_complain(file);
    else if (s->load_most_recent && s->last_file[0]) ws_app_load_file(&ui.app, s->last_file);
    h_resize_window(NULL, ui.app.win_w > 0 ? ui.app.win_w : 240, ui.app.win_h > 0 ? ui.app.win_h : 50);
    h_apply_window_props(NULL);
    h_hotkeys_changed(NULL);
    arm_timer();
    ShowWindow(ui.hwnd, show);
    UpdateWindow(ui.hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    ws_app_free(&ui.app);
    for (int i = 0; i < WS_FONT_COUNT; i++)
        if (ui.font[i]) GdipDeleteFont(ui.font[i]);
    if (ui.fmt) GdipDeleteStringFormat(ui.fmt);
    GdiplusShutdown(gtoken);
    OleUninitialize();
    return (int)m.wParam;
}
