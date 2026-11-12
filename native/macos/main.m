/* WSplit for macOS: app delegate, timer window, painter, hotkeys. */
#import "WSplitMac.h"
#import <Carbon/Carbon.h>
#import <CoreText/CoreText.h>

WsApp gApp;
WSAppDelegate *gDelegate;

/* ================================================================== */
/* Colours                                                             */
/* ================================================================== */

NSColor *WSNSColor(WsColor c)
{
    return [NSColor colorWithSRGBRed:((c >> 16) & 0xFF) / 255.0
                               green:((c >> 8) & 0xFF) / 255.0
                                blue:(c & 0xFF) / 255.0
                               alpha:((c >> 24) & 0xFF) / 255.0];
}

WsColor WSColorFromNSColor(NSColor *c)
{
    NSColor *s = [c colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!s) return 0xFFFFFFFF;
    unsigned r = (unsigned)lround([s redComponent] * 255.0), g = (unsigned)lround([s greenComponent] * 255.0),
             b = (unsigned)lround([s blueComponent] * 255.0);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void setFill(CGContextRef ctx, WsColor c)
{
    CGContextSetRGBFillColor(ctx, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0,
                             ((c >> 24) & 0xFF) / 255.0);
}

/* ================================================================== */
/* Keys                                                                */
/* ================================================================== */

static const struct { int code; const char *name; } kKeys[] = {
    {kVK_ANSI_A, "A"}, {kVK_ANSI_B, "B"}, {kVK_ANSI_C, "C"}, {kVK_ANSI_D, "D"}, {kVK_ANSI_E, "E"},
    {kVK_ANSI_F, "F"}, {kVK_ANSI_G, "G"}, {kVK_ANSI_H, "H"}, {kVK_ANSI_I, "I"}, {kVK_ANSI_J, "J"},
    {kVK_ANSI_K, "K"}, {kVK_ANSI_L, "L"}, {kVK_ANSI_M, "M"}, {kVK_ANSI_N, "N"}, {kVK_ANSI_O, "O"},
    {kVK_ANSI_P, "P"}, {kVK_ANSI_Q, "Q"}, {kVK_ANSI_R, "R"}, {kVK_ANSI_S, "S"}, {kVK_ANSI_T, "T"},
    {kVK_ANSI_U, "U"}, {kVK_ANSI_V, "V"}, {kVK_ANSI_W, "W"}, {kVK_ANSI_X, "X"}, {kVK_ANSI_Y, "Y"},
    {kVK_ANSI_Z, "Z"},
    {kVK_ANSI_0, "0"}, {kVK_ANSI_1, "1"}, {kVK_ANSI_2, "2"}, {kVK_ANSI_3, "3"}, {kVK_ANSI_4, "4"},
    {kVK_ANSI_5, "5"}, {kVK_ANSI_6, "6"}, {kVK_ANSI_7, "7"}, {kVK_ANSI_8, "8"}, {kVK_ANSI_9, "9"},
    {kVK_F1, "F1"}, {kVK_F2, "F2"}, {kVK_F3, "F3"}, {kVK_F4, "F4"}, {kVK_F5, "F5"}, {kVK_F6, "F6"},
    {kVK_F7, "F7"}, {kVK_F8, "F8"}, {kVK_F9, "F9"}, {kVK_F10, "F10"}, {kVK_F11, "F11"}, {kVK_F12, "F12"},
    {kVK_F13, "F13"}, {kVK_F14, "F14"}, {kVK_F15, "F15"}, {kVK_F16, "F16"}, {kVK_F17, "F17"},
    {kVK_F18, "F18"}, {kVK_F19, "F19"}, {kVK_F20, "F20"},
    {kVK_ANSI_Keypad0, "Numpad0"}, {kVK_ANSI_Keypad1, "Numpad1"}, {kVK_ANSI_Keypad2, "Numpad2"},
    {kVK_ANSI_Keypad3, "Numpad3"}, {kVK_ANSI_Keypad4, "Numpad4"}, {kVK_ANSI_Keypad5, "Numpad5"},
    {kVK_ANSI_Keypad6, "Numpad6"}, {kVK_ANSI_Keypad7, "Numpad7"}, {kVK_ANSI_Keypad8, "Numpad8"},
    {kVK_ANSI_Keypad9, "Numpad9"}, {kVK_ANSI_KeypadDecimal, "NumpadDecimal"},
    {kVK_ANSI_KeypadMultiply, "NumpadMultiply"}, {kVK_ANSI_KeypadPlus, "NumpadAdd"},
    {kVK_ANSI_KeypadDivide, "NumpadDivide"}, {kVK_ANSI_KeypadEnter, "NumpadEnter"},
    {kVK_ANSI_KeypadMinus, "NumpadSubtract"},
    {kVK_Space, "Space"}, {kVK_Return, "Enter"}, {kVK_Tab, "Tab"}, {kVK_Escape, "Escape"},
    {kVK_Delete, "Backspace"}, {kVK_ForwardDelete, "Delete"}, {kVK_Help, "Insert"}, {kVK_Home, "Home"},
    {kVK_End, "End"}, {kVK_PageUp, "PageUp"}, {kVK_PageDown, "PageDown"}, {kVK_LeftArrow, "Left"},
    {kVK_RightArrow, "Right"}, {kVK_UpArrow, "Up"}, {kVK_DownArrow, "Down"},
    {kVK_ANSI_Minus, "Minus"}, {kVK_ANSI_Equal, "Equals"}, {kVK_ANSI_LeftBracket, "LeftBracket"},
    {kVK_ANSI_RightBracket, "RightBracket"}, {kVK_ANSI_Semicolon, "Semicolon"}, {kVK_ANSI_Quote, "Quote"},
    {kVK_ANSI_Comma, "Comma"}, {kVK_ANSI_Period, "Period"}, {kVK_ANSI_Slash, "Slash"},
    {kVK_ANSI_Backslash, "Backslash"}, {kVK_ANSI_Grave, "Grave"},
};

const char *WSKeyNameForKeyCode(unsigned short code)
{
    for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
        if (kKeys[i].code == code) return kKeys[i].name;
    return NULL;
}

int WSKeyCodeForName(const char *name)
{
    if (!name) return -1;
    for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
        if (strcasecmp(kKeys[i].name, name) == 0) return kKeys[i].code;
    return -1;
}

unsigned WSModsFromFlags(NSEventModifierFlags f)
{
    unsigned m = 0;
    if (f & NSEventModifierFlagControl) m |= WS_MOD_CTRL;
    if (f & NSEventModifierFlagOption) m |= WS_MOD_ALT;
    if (f & NSEventModifierFlagShift) m |= WS_MOD_SHIFT;
    if (f & NSEventModifierFlagCommand) m |= WS_MOD_SUPER;
    return m;
}

NSString *WSPrettyHotkey(const char *hk)
{
    unsigned mods;
    char key[64];
    if (!ws_hotkey_parse(hk, &mods, key, sizeof key)) return @"None";
    NSMutableString *s = [NSMutableString string];
    if (mods & WS_MOD_CTRL) [s appendString:@"⌃"];
    if (mods & WS_MOD_ALT) [s appendString:@"⌥"];
    if (mods & WS_MOD_SHIFT) [s appendString:@"⇧"];
    if (mods & WS_MOD_SUPER) [s appendString:@"⌘"];
    [s appendString:[NSString stringWithUTF8String:key]];
    return s;
}

/* ================================================================== */
/* Fonts & painter                                                     */
/* ================================================================== */

static NSFont *gFonts[WS_FONT_COUNT];
static CGFontRef gDigitCG;

typedef struct {
    CGContextRef ctx;
} PaintCtx;

static CTFontRef ctfont(WsFont f) { return (__bridge CTFontRef)gFonts[f]; }

static CTLineRef make_line(WsFont f, const char *s)
{
    NSString *str = [NSString stringWithUTF8String:s ? s : ""];
    if (!str) str = @"";
    NSDictionary *attrs = @{
        (__bridge NSString *)kCTFontAttributeName : gFonts[f],
        (__bridge NSString *)kCTForegroundColorFromContextAttributeName : @YES
    };
    NSAttributedString *as = [[NSAttributedString alloc] initWithString:str attributes:attrs];
    return CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
}

static void p_fill_rect(void *c, double x, double y, double w, double h, WsColor col)
{
    CGContextRef ctx = ((PaintCtx *)c)->ctx;
    setFill(ctx, col);
    CGContextFillRect(ctx, CGRectMake(x, y, w, h));
}

static void p_fill_gradient(void *c, double x, double y, double w, double h, WsColor c1, WsColor c2, bool horiz)
{
    CGContextRef ctx = ((PaintCtx *)c)->ctx;
    CGFloat comps[8] = {((c1 >> 16) & 0xFF) / 255.0, ((c1 >> 8) & 0xFF) / 255.0, (c1 & 0xFF) / 255.0, ((c1 >> 24) & 0xFF) / 255.0,
                        ((c2 >> 16) & 0xFF) / 255.0, ((c2 >> 8) & 0xFF) / 255.0, (c2 & 0xFF) / 255.0, ((c2 >> 24) & 0xFF) / 255.0};
    CGFloat locs[2] = {0, 1};
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
    CGContextSaveGState(ctx);
    CGContextClipToRect(ctx, CGRectMake(x, y, w, h));
    CGPoint a = horiz ? CGPointMake(x, y) : CGPointMake(x, y + h);
    CGPoint b = horiz ? CGPointMake(x + w, y) : CGPointMake(x, y);
    CGContextDrawLinearGradient(ctx, g, a, b, kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGContextRestoreGState(ctx);
    CGGradientRelease(g);
    CGColorSpaceRelease(cs);
}

static void p_stroke_line(void *c, double x1, double y1, double x2, double y2, WsColor col)
{
    CGContextRef ctx = ((PaintCtx *)c)->ctx;
    CGContextSetRGBStrokeColor(ctx, ((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0, (col & 0xFF) / 255.0,
                               ((col >> 24) & 0xFF) / 255.0);
    CGContextSetLineWidth(ctx, 1.0);
    CGContextBeginPath(ctx);
    CGContextMoveToPoint(ctx, x1, y1);
    CGContextAddLineToPoint(ctx, x2, y2);
    CGContextStrokePath(ctx);
}

static void p_draw_text(void *c, WsFont f, const char *s, double x, double y, WsColor col)
{
    CGContextRef ctx = ((PaintCtx *)c)->ctx;
    CTLineRef line = make_line(f, s);
    CGContextSaveGState(ctx);
    setFill(ctx, col);
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1.0, -1.0));
    CGContextSetTextPosition(ctx, x, y + CTFontGetAscent(ctfont(f)));
    CTLineDraw(line, ctx);
    CGContextRestoreGState(ctx);
    CFRelease(line);
}

static double p_text_width(void *c, WsFont f, const char *s)
{
    CTLineRef line = make_line(f, s);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    return w;
}

static void p_font_metrics(void *c, WsFont f, double *a, double *d)
{
    *a = CTFontGetAscent(ctfont(f));
    *d = CTFontGetDescent(ctfont(f));
}

static double p_font_size(void *c, WsFont f) { return CTFontGetSize(ctfont(f)); }

static void p_push_clip(void *c, double x, double y, double w, double h)
{
    CGContextRef ctx = ((PaintCtx *)c)->ctx;
    CGContextSaveGState(ctx);
    CGContextClipToRect(ctx, CGRectMake(x, y, w, h));
}

static void p_pop(void *c) { CGContextRestoreGState(((PaintCtx *)c)->ctx); }

static void p_push_transform(void *c, double tx, double ty, double s)
{
    CGContextRef ctx = ((PaintCtx *)c)->ctx;
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, tx, ty);
    CGContextScaleCTM(ctx, s, s);
}

/* Icons: an NSArray of @[normal, gray] NSImages, retained via CFBridgingRetain. */
static void *h_load_icon(void *ud, const char *path)
{
    NSImage *img = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:path]];
    if (!img) return NULL;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:64 pixelsHigh:64
                                                                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                                                               colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:64 * 4 bitsPerPixel:32];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
    [img drawInRect:NSMakeRect(0, 0, 64, 64) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1.0];
    [NSGraphicsContext restoreGraphicsState];
    unsigned char *px = [rep bitmapData];
    for (int i = 0; i < 64 * 64; i++) {
        unsigned char *p = px + i * 4;
        unsigned lum = (unsigned)(p[0] * 0.3 + p[1] * 0.59 + p[2] * 0.11);
        p[0] = (unsigned char)((lum * 4 + p[0]) / 5);
        p[1] = (unsigned char)((lum * 4 + p[1]) / 5);
        p[2] = (unsigned char)((lum * 4 + p[2]) / 5);
    }
    NSImage *gray = [[NSImage alloc] initWithSize:NSMakeSize(64, 64)];
    [gray addRepresentation:rep];
    return (void *)CFBridgingRetain(@[ img, gray ]);
}

static void h_free_icon(void *ud, void *icon)
{
    if (icon) CFBridgingRelease(icon);
}

static void p_draw_icon(void *c, void *icon, double x, double y, double size, double alpha, bool gray)
{
    NSRect r = NSMakeRect(x, y, size, size);
    if (!icon) {
        CGContextRef ctx = ((PaintCtx *)c)->ctx;
        CGContextSetRGBFillColor(ctx, 0.35, 0.35, 0.38, alpha);
        CGContextFillRect(ctx, CGRectInset(r, 1, 1));
        NSDictionary *at = @{NSFontAttributeName : [NSFont boldSystemFontOfSize:size * 0.65],
                             NSForegroundColorAttributeName : [NSColor colorWithWhite:0.88 alpha:alpha]};
        NSSize ts = [@"?" sizeWithAttributes:at];
        [@"?" drawAtPoint:NSMakePoint(x + (size - ts.width) / 2, y + (size - ts.height) / 2) withAttributes:at];
        return;
    }
    NSArray *pair = (__bridge NSArray *)icon;
    NSImage *img = pair[gray ? 1 : 0];
    [img drawInRect:r fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:alpha respectFlipped:YES hints:nil];
}

static WsPainter makePainter(PaintCtx *pc)
{
    WsPainter p = {0};
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
    return p;
}

/* ================================================================== */
/* Window + view                                                       */
/* ================================================================== */

@implementation WSWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

enum { DRAG_NONE, DRAG_MOVE, DRAG_CLOCK, DRAG_DETAIL_SEGS, DRAG_WIDE_SEGS, DRAG_WIDE_SEG_WIDTH };
#define EDGE_L 1
#define EDGE_R 2
#define EDGE_T 4
#define EDGE_B 8

@implementation WSView {
    int _drag, _edges, _ref, _startVal;
    NSPoint _startMouse; /* screen coords */
    NSRect _startFrame;
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)f
{
    if ((self = [super initWithFrame:f])) {
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)isOpaque { return YES; }

- (void)drawRect:(NSRect)dirty
{
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    PaintCtx pc = {ctx};
    WsPainter p = makePainter(&pc);
    ws_render(&gApp, &p);
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:self.bounds
                                             options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited
                                               owner:self
                                            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

- (int)hitTest:(NSPoint)p edges:(int *)edges
{
    WsApp *a = &gApp;
    WsRect c = a->clock;
    int x = (int)p.x, y = (int)p.y;
    int cl = c.x, cr = c.x + c.w, ct = c.y, cb = c.y + c.h;
    BOOL wide = a->cur_mode == WS_MODE_WIDE;
    *edges = 0;
    if (wide && abs(x - (cr + a->wide_seg_w - 3)) < 4) return DRAG_WIDE_SEG_WIDTH;
    if (wide && abs(x - (a->win_w - 117)) < 4) return DRAG_WIDE_SEGS;
    if (a->cur_mode == WS_MODE_DETAILED && abs(y - (ct - 1)) < 4) return DRAG_DETAIL_SEGS;
    if (x >= cr - 5 && x <= cr) {
        *edges = EDGE_R;
        if (!wide && y >= cb - 5 && y <= cb) *edges |= EDGE_B;
        else if (!wide && y <= ct + 5 && y >= ct) *edges |= EDGE_T;
        return DRAG_CLOCK;
    }
    if (x <= cl + 5 && x >= cl) {
        *edges = EDGE_L;
        if (!wide && y >= cb - 5 && y <= cb) *edges |= EDGE_B;
        else if (!wide && y <= ct + 5 && y >= ct) *edges |= EDGE_T;
        return DRAG_CLOCK;
    }
    if (!wide && y >= cb - 5 && y <= cb) { *edges = EDGE_B; return DRAG_CLOCK; }
    if (!wide && y <= ct + 5 && y >= ct) { *edges = EDGE_T; return DRAG_CLOCK; }
    return DRAG_MOVE;
}

- (void)mouseMoved:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int edges, m = [self hitTest:p edges:&edges];
    if (m == DRAG_WIDE_SEG_WIDTH || m == DRAG_WIDE_SEGS || (m == DRAG_CLOCK && (edges & (EDGE_L | EDGE_R))))
        [[NSCursor resizeLeftRightCursor] set];
    else if (m == DRAG_DETAIL_SEGS || m == DRAG_CLOCK)
        [[NSCursor resizeUpDownCursor] set];
    else
        [[NSCursor arrowCursor] set];
}

- (void)mouseExited:(NSEvent *)e { [[NSCursor arrowCursor] set]; }

- (void)mouseDown:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int edges;
    _drag = [self hitTest:p edges:&edges];
    _edges = edges;
    _startMouse = [NSEvent mouseLocation];
    _startFrame = self.window.frame;
    _ref = _drag == DRAG_DETAIL_SEGS ? (int)p.y : (int)p.x;
    _startVal = gApp.settings.wide_seg_width;
    if (_drag == DRAG_MOVE) {
        [self.window performWindowDragWithEvent:e];
        _drag = DRAG_NONE;
    }
}

- (void)mouseDragged:(NSEvent *)e
{
    WsApp *a = &gApp;
    NSPoint m = [NSEvent mouseLocation];
    int dx = (int)lround(m.x - _startMouse.x);
    int dy = (int)lround(_startMouse.y - m.y); /* positive = down */
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    switch (_drag) {
    case DRAG_CLOCK: {
        int w = (int)_startFrame.size.width, h = (int)_startFrame.size.height;
        if (_edges & EDGE_R) w += dx;
        if (_edges & EDGE_L) w -= dx;
        if (_edges & EDGE_B) h += dy;
        if (_edges & EDGE_T) h -= dy;
        ws_app_user_resized(a, w, h);
        NSRect f = self.window.frame;
        if (_edges & EDGE_L) f.origin.x = NSMaxX(_startFrame) - f.size.width;
        if (_edges & EDGE_T) f.origin.y = _startFrame.origin.y; /* bottom stays */
        else f.origin.y = NSMaxY(_startFrame) - f.size.height;  /* top stays */
        [self.window setFrame:f display:YES];
        break;
    }
    case DRAG_DETAIL_SEGS: {
        int y = (int)p.y, sh = a->seg_height;
        if (y >= _ref + sh) {
            int before = a->settings.display_segs;
            ws_app_segs_inc(a);
            if (a->settings.display_segs != before) _ref += sh;
        } else if (y <= _ref - sh && a->settings.display_segs > 2) {
            ws_app_segs_dec(a);
            _ref -= sh;
        }
        break;
    }
    case DRAG_WIDE_SEGS: {
        int x = (int)p.x, sw = a->wide_seg_w;
        if (x >= _ref + sw) {
            int before = a->settings.wide_segs;
            ws_app_segs_inc(a);
            if (a->settings.wide_segs != before) _ref += sw;
        } else if (x <= _ref - sw && a->settings.wide_segs > 1) {
            ws_app_segs_dec(a);
            _ref -= sw;
        }
        break;
    }
    case DRAG_WIDE_SEG_WIDTH: {
        int v = _startVal + dx;
        a->settings.wide_seg_width = v < 60 ? 60 : v;
        ws_app_relayout(a);
        break;
    }
    default:
        break;
    }
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)e { _drag = DRAG_NONE; }

- (NSMenu *)menuForEvent:(NSEvent *)e
{
    WsMenu *m = ws_menu_build(&gApp, WS_MENU_PLATFORM_MAC);
    NSMenu *menu = WSBuildNSMenu(m, gDelegate);
    ws_menu_free(m);
    return menu;
}

- (void)keyDown:(NSEvent *)e
{
    if (e.isARepeat) return;
    const char *name = WSKeyNameForKeyCode(e.keyCode);
    NSEventModifierFlags f = e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
    if (!name || f) { [super keyDown:e]; return; }
    if (ws_app_local_key(&gApp, name)) [self setNeedsDisplay:YES];
    else [super keyDown:e];
}

/* drag & drop a .wsplit onto the timer */
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)s { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)s
{
    NSArray *urls = [[s draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                          options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    NSURL *u = urls.firstObject;
    if (!u) return NO;
    if (!ws_app_prompt_for_save(&gApp)) return NO;
    BOOL ok = ws_app_load_file(&gApp, u.fileSystemRepresentation);
    if (!ok) WSMessage(@"Open", @"That file could not be opened as a WSplit run.");
    return ok;
}
@end

/* ================================================================== */
/* Host callbacks                                                      */
/* ================================================================== */

static WsAnswer h_ask(void *ud, const char *t, const char *m, bool c)
{
    return WSAsk([NSString stringWithUTF8String:t], [NSString stringWithUTF8String:m], c);
}

static void h_message(void *ud, const char *t, const char *m)
{
    WSMessage([NSString stringWithUTF8String:t], [NSString stringWithUTF8String:m]);
}

static bool h_choose_save_path(void *ud, char *out, size_t n)
{
    NSSavePanel *sp = [NSSavePanel savePanel];
    sp.title = @"Save Splits";
    sp.allowedFileTypes = @[ @"wsplit", @"txt" ];
    sp.allowsOtherFileTypes = YES;
    NSString *name = gApp.run.title[0] ? [NSString stringWithUTF8String:gApp.run.title] : @"splits";
    name = [name stringByReplacingOccurrencesOfString:@"/" withString:@"-"];
    sp.nameFieldStringValue = [name stringByAppendingPathExtension:@"wsplit"];
    if ([sp runModal] != NSModalResponseOK) return false;
    snprintf(out, n, "%s", sp.URL.fileSystemRepresentation);
    return true;
}

static void h_resize_window(void *ud, int w, int h) { [gDelegate resizeContentTo:NSMakeSize(w, h)]; }
static void h_invalidate(void *ud) { [gDelegate.view setNeedsDisplay:YES]; }
static void h_hotkeys_changed(void *ud) { [gDelegate registerHotkeys]; [gDelegate.view setNeedsDisplay:YES]; }
static void h_apply_window_props(void *ud) { [gDelegate applyWindowProps]; }

/* ================================================================== */
/* Carbon global hotkeys (no Accessibility permission needed)          */
/* ================================================================== */

static EventHotKeyRef gHotkeyRefs[WS_HK_COUNT];
static EventHandlerRef gHotkeyHandler;

static OSStatus HotkeyHandler(EventHandlerCallRef next, EventRef ev, void *ud)
{
    EventHotKeyID hkid;
    if (GetEventParameter(ev, kEventParamDirectObject, typeEventHotKeyID, NULL, sizeof hkid, NULL, &hkid) != noErr)
        return eventNotHandledErr;
    if (hkid.signature != 'WSpl' || hkid.id >= WS_HK_COUNT) return eventNotHandledErr;
    ws_app_hotkey(&gApp, (WsHotkeyAction)hkid.id);
    [gDelegate.view setNeedsDisplay:YES];
    return noErr;
}

/* ================================================================== */
/* App delegate                                                        */
/* ================================================================== */

@implementation WSAppDelegate {
    NSTimer *_timer;
    int _timerInterval;
    NSString *_pendingOpen;
}

- (void)resizeContentTo:(NSSize)size
{
    if (!self.window) return;
    NSRect f = self.window.frame;
    CGFloat top = NSMaxY(f);
    f.size = size;
    f.origin.y = top - size.height;
    [self.window setFrame:f display:YES];
    [self.view setNeedsDisplay:YES];
}

- (void)applyWindowProps
{
    self.window.level = gApp.settings.on_top ? NSFloatingWindowLevel : NSNormalWindowLevel;
    self.window.alphaValue = gApp.settings.opacity;
}

- (void)unregisterHotkeys
{
    for (int i = 0; i < WS_HK_COUNT; i++) {
        if (gHotkeyRefs[i]) UnregisterEventHotKey(gHotkeyRefs[i]);
        gHotkeyRefs[i] = NULL;
    }
}

- (void)registerHotkeys
{
    [self unregisterHotkeys];
    if (!gHotkeyHandler) {
        EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
        InstallEventHandler(GetApplicationEventTarget(), NewEventHandlerUPP(HotkeyHandler), 1, &spec, NULL, &gHotkeyHandler);
    }
    const WsSettings *s = &gApp.settings;
    NSMutableArray *failed = [NSMutableArray array];
    for (int i = 0; i < WS_HK_COUNT; i++) {
        if (!s->hotkeys[i][0]) continue;
        if (i != WS_HK_TOGGLE && !s->hotkeys_enabled) continue;
        unsigned mods;
        char key[64];
        if (!ws_hotkey_parse(s->hotkeys[i], &mods, key, sizeof key)) continue;
        int code = WSKeyCodeForName(key);
        if (code < 0) continue;
        UInt32 cm = 0;
        if (mods & WS_MOD_CTRL) cm |= controlKey;
        if (mods & WS_MOD_ALT) cm |= optionKey;
        if (mods & WS_MOD_SHIFT) cm |= shiftKey;
        if (mods & WS_MOD_SUPER) cm |= cmdKey;
        EventHotKeyID hkid = {'WSpl', (UInt32)i};
        if (RegisterEventHotKey((UInt32)code, cm, hkid, GetApplicationEventTarget(), 0, &gHotkeyRefs[i]) != noErr) {
            gHotkeyRefs[i] = NULL;
            [failed addObject:WSPrettyHotkey(s->hotkeys[i])];
        }
    }
    if (failed.count)
        WSMessage(@"Global Hotkeys", [NSString stringWithFormat:@"These hotkeys are already in use by another application:\n%@",
                                                                [failed componentsJoinedByString:@", "]]);
}

- (void)rebuildFonts
{
    const WsSettings *s = &gApp.settings;
    double m = s->font_multiplier > 0 ? s->font_multiplier : 1.0;
    NSFontManager *fm = [NSFontManager sharedFontManager];
    NSString *fam = s->font_family[0] ? [NSString stringWithUTF8String:s->font_family] : nil;
    NSFont *(^make)(double, BOOL) = ^NSFont *(double size, BOOL bold) {
        NSFont *f = nil;
        if (fam) f = [fm fontWithFamily:fam traits:(bold ? NSBoldFontMask : 0) weight:5 size:size];
        if (!f) f = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        return f;
    };
    /* GDI+ pixel sizes; macOS system font renders a touch larger, so use points 1:1 */
    gFonts[WS_FONT_DISPLAY] = make(10.66667 * m, NO);
    gFonts[WS_FONT_TIME] = make(12.0 * m, YES);
    gFonts[WS_FONT_CLOCK_LARGE] = make(22.66667, YES);
    gFonts[WS_FONT_CLOCK_MED] = make(18.66667, YES);

    if (!gDigitCG) {
        NSURL *url = [[NSBundle mainBundle] URLForResource:@"digital-7-mono" withExtension:@"ttf"];
        if (!url) {
            /* running the bare binary from the source tree */
            NSString *exe = [[NSBundle mainBundle] executablePath];
            for (NSString *rel in @[ @"../../../assets", @"../assets", @"assets" ]) {
                NSString *p = [[[exe stringByDeletingLastPathComponent] stringByAppendingPathComponent:rel]
                    stringByAppendingPathComponent:@"digital-7-mono.ttf"];
                if ([[NSFileManager defaultManager] fileExistsAtPath:p]) { url = [NSURL fileURLWithPath:p]; break; }
            }
        }
        if (url) {
            CGDataProviderRef dp = CGDataProviderCreateWithURL((__bridge CFURLRef)url);
            if (dp) {
                gDigitCG = CGFontCreateWithDataProvider(dp);
                CGDataProviderRelease(dp);
            }
        }
    }
    if (gDigitCG) {
        gFonts[WS_FONT_DIGIT_LARGE] = (__bridge_transfer NSFont *)CTFontCreateWithGraphicsFont(gDigitCG, 24.0, NULL, NULL);
        gFonts[WS_FONT_DIGIT_MED] = (__bridge_transfer NSFont *)CTFontCreateWithGraphicsFont(gDigitCG, 17.33333, NULL, NULL);
    } else {
        gFonts[WS_FONT_DIGIT_LARGE] = [NSFont monospacedDigitSystemFontOfSize:24 weight:NSFontWeightBold];
        gFonts[WS_FONT_DIGIT_MED] = [NSFont monospacedDigitSystemFontOfSize:17.33333 weight:NSFontWeightBold];
    }
    [self.view setNeedsDisplay:YES];
}

- (void)startTimerIfNeeded
{
    int iv = gApp.settings.refresh_ms;
    if (_timer && _timerInterval == iv) return;
    [_timer invalidate];
    _timerInterval = iv;
    __weak WSAppDelegate *weakSelf = self;
    _timer = [NSTimer timerWithTimeInterval:iv / 1000.0
                                    repeats:YES
                                      block:^(NSTimer *t) {
                                          if (ws_app_tick(&gApp)) [weakSelf.view setNeedsDisplay:YES];
                                          [weakSelf startTimerIfNeeded];
                                      }];
    _timer.tolerance = 0.001;
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}

/* ---------------------------------------------------------------- menus */

NSMenu *WSBuildNSMenu(WsMenu *m, id target)
{
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    for (int i = 0; i < m->count; i++) {
        WsMenuItem *it = &m->items[i];
        if (it->kind == WS_MI_SEPARATOR) { [menu addItem:[NSMenuItem separatorItem]]; continue; }
        NSString *title = [NSString stringWithUTF8String:it->label];
        NSString *key = @"";
        NSEventModifierFlags mask = NSEventModifierFlagCommand;
        if (it->id == WS_CMD_OPEN) key = @"o";
        else if (it->id == WS_CMD_EDIT_RUN) key = @"e";
        else if (it->id == WS_CMD_SETTINGS) key = @",";
        else if ([title isEqualToString:@"Save"]) key = @"s";
        else if ([title isEqualToString:@"Save As..."]) { key = @"s"; mask |= NSEventModifierFlagShift; }
        NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:title action:@selector(menuItemClicked:) keyEquivalent:key];
        mi.keyEquivalentModifierMask = mask;
        mi.target = target;
        mi.tag = it->id;
        mi.enabled = it->enabled;
        mi.state = it->checked ? NSControlStateValueOn : NSControlStateValueOff;
        if (it->kind == WS_MI_SUBMENU && it->sub) {
            mi.action = nil;
            mi.submenu = WSBuildNSMenu(it->sub, target);
            mi.submenu.title = title;
        }
        [menu addItem:mi];
    }
    return menu;
}

- (void)menuItemClicked:(id)sender
{
    int id_ = (int)[sender tag];
    if (id_ <= 0) return;
    int r = ws_menu_activate(&gApp, id_);
    if (r != WS_CMD_NONE) [self runCommand:r];
    [self startTimerIfNeeded];
    [self.view setNeedsDisplay:YES];
}

- (void)menuNeedsUpdate:(NSMenu *)menu
{
    if (menu != self.splitsMenu) return;
    [menu removeAllItems];
    WsMenu *m = ws_menu_build(&gApp, WS_MENU_PLATFORM_MAC);
    NSMenu *built = WSBuildNSMenu(m, self);
    ws_menu_free(m);
    NSArray *items = [built.itemArray copy];
    for (NSMenuItem *it in items) {
        if (it.tag == WS_CMD_ABOUT || it.tag == WS_CMD_EXIT || it.tag == WS_CMD_SETTINGS) continue;
        [built removeItem:it];
        [menu addItem:it];
    }
    /* drop a trailing separator */
    while (menu.numberOfItems && menu.itemArray.lastObject.isSeparatorItem) [menu removeItemAtIndex:menu.numberOfItems - 1];
}

- (void)buildMainMenu
{
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"WSplit"];
    NSMenuItem *about = [appMenu addItemWithTitle:@"About WSplit" action:@selector(menuItemClicked:) keyEquivalent:@""];
    about.tag = WS_CMD_ABOUT;
    about.target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *prefs = [appMenu addItemWithTitle:@"Settings..." action:@selector(menuItemClicked:) keyEquivalent:@","];
    prefs.tag = WS_CMD_SETTINGS;
    prefs.target = self;
    NSMenuItem *colors = [appMenu addItemWithTitle:@"Colors..." action:@selector(menuItemClicked:) keyEquivalent:@""];
    colors.tag = WS_CMD_COLORS_EDIT;
    colors.target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide WSplit" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *ho = [appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    ho.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit WSplit" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    /* Edit menu so copy/paste work in text fields */
    NSMenuItem *editItem = [[NSMenuItem alloc] init];
    [bar addItem:editItem];
    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem *redo = [edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit;

    NSMenuItem *splitsItem = [[NSMenuItem alloc] init];
    [bar addItem:splitsItem];
    self.splitsMenu = [[NSMenu alloc] initWithTitle:@"Splits"];
    self.splitsMenu.delegate = self;
    self.splitsMenu.autoenablesItems = NO;
    splitsItem.submenu = self.splitsMenu;
    [self menuNeedsUpdate:self.splitsMenu];

    NSMenuItem *winItem = [[NSMenuItem alloc] init];
    [bar addItem:winItem];
    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    winItem.submenu = win;
    NSApp.windowsMenu = win;

    NSApp.mainMenu = bar;
}

/* ---------------------------------------------------------- commands */

- (void)openPath:(NSString *)path
{
    if (!ws_app_prompt_for_save(&gApp)) return;
    if (!ws_app_load_file(&gApp, path.fileSystemRepresentation))
        WSMessage(@"Open", [NSString stringWithFormat:@"%@ could not be opened as a WSplit run.", path.lastPathComponent]);
    [self.view setNeedsDisplay:YES];
}

- (void)cmdOpen
{
    if (!ws_app_prompt_for_save(&gApp)) return;
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.title = @"Open Splits";
    op.allowedFileTypes = @[ @"wsplit", @"txt" ];
    op.allowsOtherFileTypes = YES;
    if ([op runModal] != NSModalResponseOK) return;
    if (!ws_app_load_file(&gApp, op.URL.fileSystemRepresentation))
        WSMessage(@"Open", @"That file could not be opened as a WSplit run.");
}

- (void)cmdImportLSS
{
    if (!ws_app_prompt_for_save(&gApp)) return;
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.title = @"Import LiveSplit Splits";
    op.allowedFileTypes = @[ @"lss" ];
    if ([op runModal] != NSModalResponseOK) return;
    ws_app_close_file(&gApp);
    if (!ws_runfile_import_lss(&gApp.run, op.URL.fileSystemRepresentation)) {
        ws_app_close_file(&gApp);
        WSMessage(@"Import", @"No segments could be read from that LiveSplit file.");
        return;
    }
    ws_app_run_edited(&gApp);
    WSMessage(@"Import", @"Splits imported. Use Save As... to keep them as a .wsplit file.");
}

- (void)cmdStartAt
{
    NSAlert *al = [[NSAlert alloc] init];
    al.messageText = @"Start at...";
    al.informativeText = @"Start the timer at an offset (e.g. 1:30 or 12.5).";
    [al addButtonWithTitle:@"Start"];
    [al addButtonWithTitle:@"Cancel"];
    NSView *acc = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 240, 52)];
    NSTextField *tf = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 28, 240, 22)];
    tf.stringValue = @"0:00";
    NSButton *cb = [NSButton checkboxWithTitle:@"Still use delay" target:nil action:nil];
    cb.frame = NSMakeRect(0, 0, 240, 20);
    [acc addSubview:tf];
    [acc addSubview:cb];
    al.accessoryView = acc;
    [al.window setInitialFirstResponder:tf];
    gApp.modal_open = true;
    NSModalResponse r = [al runModal];
    gApp.modal_open = false;
    if (r != NSAlertFirstButtonReturn) return;
    ws_app_start_at(&gApp, ws_parse_time(tf.stringValue.UTF8String), cb.state == NSControlStateValueOn);
}

- (void)cmdColorsFile:(BOOL)save
{
    if (save) {
        NSSavePanel *sp = [NSSavePanel savePanel];
        sp.nameFieldStringValue = @"colors.wscolors";
        if ([sp runModal] != NSModalResponseOK) return;
        if (!ws_colors_save(&gApp.settings.colors, sp.URL.fileSystemRepresentation)) WSMessage(@"Colors", @"Could not save the color scheme.");
    } else {
        NSOpenPanel *op = [NSOpenPanel openPanel];
        if ([op runModal] != NSModalResponseOK) return;
        WsColors c = gApp.settings.colors;
        if (ws_colors_load(&c, op.URL.fileSystemRepresentation)) { gApp.settings.colors = c; ws_app_colors_changed(&gApp); }
        else WSMessage(@"Colors", @"No colors could be read from that file.");
    }
}

- (void)runCommand:(int)cmd
{
    switch (cmd) {
    case WS_CMD_EDIT_RUN: case WS_CMD_NEW_RUN: WSShowRunEditor(); break;
    case WS_CMD_OPEN: [self cmdOpen]; break;
    case WS_CMD_IMPORT_LSS: [self cmdImportLSS]; break;
    case WS_CMD_START_AT: [self cmdStartAt]; break;
    case WS_CMD_SETTINGS: case WS_CMD_START_DELAY: case WS_CMD_FONT: WSShowSettings(); break;
    case WS_CMD_FONTS_CHANGED: [self rebuildFonts]; break;
    case WS_CMD_COLORS_EDIT: WSShowColors(); break;
    case WS_CMD_COLORS_LOAD: [self cmdColorsFile:NO]; break;
    case WS_CMD_COLORS_SAVE: [self cmdColorsFile:YES]; break;
    case WS_CMD_ABOUT:
        [NSApp orderFrontStandardAboutPanelWithOptions:@{
            @"ApplicationVersion" : @WS_VERSION,
            @"Credits" : [[NSAttributedString alloc]
                initWithString:@"Speedrun timer originally by Wodanaz (SDA), updated by Nitrofski and contributors.\n"
                               @"Native macOS port of the open-source C# version.\n"
                               @"Clock font: Digital-7 Mono by Style-7 (freeware for personal use)."]
        }];
        break;
    case WS_CMD_EXIT: [NSApp terminate:nil]; break;
    default: break;
    }
    [self startTimerIfNeeded];
    [self.view setNeedsDisplay:YES];
}

/* ------------------------------------------------------- lifecycle */

- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls
{
    NSURL *u = urls.firstObject;
    if (!u) return;
    if (!self.window) { _pendingOpen = u.path; return; }
    [self openPath:u.path];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSString *support = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
        stringByAppendingPathComponent:@"WSplit"];
    [[NSFileManager defaultManager] createDirectoryAtPath:support withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *settingsPath = [support stringByAppendingPathComponent:@"settings.ini"];

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
    ws_app_init(&gApp, &host, settingsPath.fileSystemRepresentation);
    NSString *dec = [[NSLocale currentLocale] decimalSeparator] ?: @".";
    snprintf(gApp.decimal_char, sizeof gApp.decimal_char, "%s", dec.UTF8String);

    int w = gApp.win_w > 0 ? gApp.win_w : 240, h = gApp.win_h > 0 ? gApp.win_h : 50;
    self.window = [[WSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                              styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"WSplit";
    self.window.opaque = YES;
    self.window.backgroundColor = NSColor.blackColor;
    self.window.hasShadow = YES;
    self.window.releasedWhenClosed = NO;
    self.window.collectionBehavior = NSWindowCollectionBehaviorManaged | NSWindowCollectionBehaviorParticipatesInCycle;
    self.view = [[WSView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    self.window.contentView = self.view;
    [self.window makeFirstResponder:self.view];

    [self rebuildFonts];
    [self buildMainMenu];

    /* window position: settings store the top-left corner in screen coordinates */
    BOOL placed = NO;
    if (gApp.settings.save_window_pos) {
        NSRect f = NSMakeRect(gApp.settings.window_x, gApp.settings.window_y - h, w, h);
        for (NSScreen *s in NSScreen.screens)
            if (NSIntersectsRect(NSInsetRect(f, 10, 10), s.visibleFrame)) placed = YES;
        if (placed) [self.window setFrame:f display:NO];
    }
    if (!placed) [self.window center];

    if (_pendingOpen) ws_app_load_file(&gApp, _pendingOpen.fileSystemRepresentation);
    else if (gApp.settings.load_most_recent && gApp.settings.last_file[0]) ws_app_load_file(&gApp, gApp.settings.last_file);
    _pendingOpen = nil;

    [self applyWindowProps];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self registerHotkeys];
    [self startTimerIfNeeded];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    return ws_app_prompt_for_save(&gApp) ? NSTerminateNow : NSTerminateCancel;
}

- (void)applicationWillTerminate:(NSNotification *)n
{
    if (gApp.settings.save_window_pos && self.window) {
        NSRect f = self.window.frame;
        gApp.settings.window_x = (int)f.origin.x;
        gApp.settings.window_y = (int)NSMaxY(f);
    }
    [self unregisterHotkeys];
    ws_app_shutdown(&gApp);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app { return YES; }
@end

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        gDelegate = [[WSAppDelegate alloc] init];
        app.delegate = gDelegate;
        /* command line: wsplit file.wsplit */
        if (argc > 1 && argv[1][0] != '-')
            [gDelegate application:app openURLs:@[ [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] ]];
        [app run];
    }
    return 0;
}
