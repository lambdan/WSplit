/* WSplit for macOS: alerts, run editor, settings and colour editor. */
#import "WSplitMac.h"
#include <stddef.h>

/* ================================================================== */
/* Alerts                                                              */
/* ================================================================== */

WsAnswer WSAsk(NSString *title, NSString *msg, BOOL cancel)
{
    NSAlert *al = [[NSAlert alloc] init];
    al.messageText = title;
    al.informativeText = msg;
    [al addButtonWithTitle:@"Yes"];
    [al addButtonWithTitle:@"No"];
    if (cancel) [al addButtonWithTitle:@"Cancel"];
    bool was = gApp.modal_open;
    gApp.modal_open = true;
    NSModalResponse r = [al runModal];
    gApp.modal_open = was;
    if (r == NSAlertFirstButtonReturn) return WS_ANSWER_YES;
    if (r == NSAlertSecondButtonReturn) return WS_ANSWER_NO;
    return WS_ANSWER_CANCEL;
}

void WSMessage(NSString *title, NSString *msg)
{
    NSAlert *al = [[NSAlert alloc] init];
    al.messageText = title;
    al.informativeText = msg;
    [al addButtonWithTitle:@"OK"];
    bool was = gApp.modal_open;
    gApp.modal_open = true;
    [al runModal];
    gApp.modal_open = was;
}

/* ================================================================== */
/* Small layout helpers                                                */
/* ================================================================== */

static NSTextField *label(NSString *s, NSRect r)
{
    NSTextField *t = [NSTextField labelWithString:s];
    t.frame = r;
    return t;
}

static NSTextField *field(NSString *s, NSRect r)
{
    NSTextField *t = [[NSTextField alloc] initWithFrame:r];
    t.stringValue = s ?: @"";
    return t;
}

static NSButton *button(NSString *title, NSRect r, id target, SEL action)
{
    NSButton *b = [NSButton buttonWithTitle:title target:target action:action];
    b.frame = r;
    return b;
}

static NSString *editTime(double v)
{
    if (v == 0.0) return @"";
    char buf[48];
    ws_format_edit_time(buf, sizeof buf, v);
    return [NSString stringWithUTF8String:buf];
}

static NSString *timeChars(NSString *s)
{
    NSCharacterSet *bad = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789:.,"] invertedSet];
    return [[s componentsSeparatedByCharactersInSet:bad] componentsJoinedByString:@""];
}

/* ================================================================== */
/* Run editor (port of RunEditorDialog)                                */
/* ================================================================== */

@interface WSRunEditor : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@property (strong) NSWindow *win;
@property (strong) NSTableView *table;
@property (strong) NSMutableArray<NSMutableDictionary *> *rows;
@property (strong) NSTextField *titleF, *goalF, *attemptsF, *delayF, *offsetF;
@end

@implementation WSRunEditor

- (void)loadFromRun
{
    self.rows = [NSMutableArray array];
    for (int i = 0; i < gApp.run.count; i++) {
        WsSegment *s = &gApp.run.segs[i];
        [self.rows addObject:[@{
            @"name" : [NSString stringWithUTF8String:s->name ?: ""] ?: @"",
            @"old" : editTime(s->old_time),
            @"best" : editTime(s->best_time),
            @"bseg" : editTime(s->best_segment),
            @"icon" : [NSString stringWithUTF8String:s->icon_path ?: ""] ?: @""
        } mutableCopy]];
    }
    [self.table reloadData];
}

- (void)build
{
    NSRect frame = NSMakeRect(0, 0, 720, 520);
    self.win = [[NSWindow alloc] initWithContentRect:frame
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    self.win.title = @"Run Editor";
    self.win.minSize = NSMakeSize(640, 360);
    self.win.delegate = self;
    self.win.releasedWhenClosed = NO;
    NSView *cv = self.win.contentView;
    CGFloat W = frame.size.width, H = frame.size.height;

    /* top fields */
    NSView *top = [[NSView alloc] initWithFrame:NSMakeRect(0, H - 70, W, 70)];
    top.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [top addSubview:label(@"Run title:", NSMakeRect(16, 40, 70, 20))];
    self.titleF = field([NSString stringWithUTF8String:gApp.run.title], NSMakeRect(90, 38, 330, 22));
    self.titleF.autoresizingMask = NSViewWidthSizable;
    [top addSubview:self.titleF];
    [top addSubview:label(@"Attempts:", NSMakeRect(436, 40, 66, 20))];
    self.attemptsF = field([NSString stringWithFormat:@"%d", gApp.run.attempts], NSMakeRect(504, 38, 60, 22));
    self.attemptsF.autoresizingMask = NSViewMinXMargin;
    [top addSubview:self.attemptsF];
    [top addSubview:label(@"Goal:", NSMakeRect(16, 10, 70, 20))];
    self.goalF = field([NSString stringWithUTF8String:gApp.run.goal], NSMakeRect(90, 8, 330, 22));
    self.goalF.autoresizingMask = NSViewWidthSizable;
    [top addSubview:self.goalF];
    NSTextField *dl = label(@"Start delay (s):", NSMakeRect(436, 10, 100, 20));
    dl.autoresizingMask = NSViewMinXMargin;
    [top addSubview:dl];
    char dbuf[32];
    snprintf(dbuf, sizeof dbuf, "%g", gApp.run.start_delay_ms / 1000.0);
    self.delayF = field([NSString stringWithUTF8String:dbuf], NSMakeRect(540, 8, 60, 22));
    self.delayF.autoresizingMask = NSViewMinXMargin;
    [top addSubview:self.delayF];
    /* attempts/delay are anchored right; keep labels too */
    for (NSView *v in top.subviews)
        if (v.frame.origin.x >= 430) v.autoresizingMask = NSViewMinXMargin;
    [cv addSubview:top];

    /* table */
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 88, W - 32, H - 70 - 92)];
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    self.table = [[NSTableView alloc] initWithFrame:sv.bounds];
    NSArray *cols = @[ @[ @"name", @"Segment", @240 ], @[ @"old", @"Old Time", @90 ], @[ @"best", @"Best Time", @90 ],
                       @[ @"bseg", @"Best Seg.", @90 ], @[ @"icon", @"Icon Path", @160 ] ];
    for (NSArray *c in cols) {
        NSTableColumn *tc = [[NSTableColumn alloc] initWithIdentifier:c[0]];
        tc.title = c[1];
        tc.width = [c[2] doubleValue];
        tc.editable = YES;
        [self.table addTableColumn:tc];
    }
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.usesAlternatingRowBackgroundColors = YES;
    self.table.allowsMultipleSelection = NO;
    self.table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    sv.documentView = self.table;
    [cv addSubview:sv];

    /* row buttons */
    CGFloat x = 16, y = 52;
    NSArray *rowBtns = @[ @[ @"Add", @"addRow:" ], @[ @"Insert", @"insertRow:" ], @[ @"Remove", @"removeRow:" ],
                          @[ @"Move Up", @"moveUp:" ], @[ @"Move Down", @"moveDown:" ], @[ @"Set Icon...", @"setIcon:" ],
                          @[ @"Clear Icon", @"clearIcon:" ] ];
    for (NSArray *b in rowBtns) {
        NSButton *bt = button(b[0], NSMakeRect(x, y, 92, 28), self, NSSelectorFromString(b[1]));
        [cv addSubview:bt];
        x += 96;
    }

    /* bottom row */
    [cv addSubview:label(@"Old time offset:", NSMakeRect(16, 18, 104, 20))];
    self.offsetF = field(@"", NSMakeRect(120, 16, 70, 22));
    [cv addSubview:self.offsetF];
    [cv addSubview:button(@"Apply", NSMakeRect(194, 12, 70, 28), self, @selector(applyOffset:))];
    [cv addSubview:button(@"Auto-fill Best Segments", NSMakeRect(272, 12, 180, 28), self, @selector(autofill:))];
    NSButton *reset = button(@"Reset", NSMakeRect(W - 16 - 3 * 92 - 8, 12, 88, 28), self, @selector(reset:));
    NSButton *cancel = button(@"Cancel", NSMakeRect(W - 16 - 2 * 92, 12, 88, 28), self, @selector(cancel:));
    NSButton *ok = button(@"OK", NSMakeRect(W - 16 - 92, 12, 88, 28), self, @selector(ok:));
    ok.keyEquivalent = @"\r";
    cancel.keyEquivalent = @"\033";
    for (NSButton *b in @[ reset, cancel, ok ]) {
        b.autoresizingMask = NSViewMinXMargin;
        [cv addSubview:b];
    }
    [self.win center];
}

/* data source */
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return (NSInteger)self.rows.count; }
- (id)tableView:(NSTableView *)tv objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    return self.rows[(NSUInteger)row][col.identifier];
}
- (void)tableView:(NSTableView *)tv setObjectValue:(id)obj forTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    NSString *v = [obj description] ?: @"";
    NSString *key = col.identifier;
    if ([key isEqualToString:@"name"]) {
        /* commas would break the .wsplit format */
        v = [v stringByReplacingOccurrencesOfString:@"," withString:@" "];
    } else if (![key isEqualToString:@"icon"]) {
        v = timeChars(v);
        double t = ws_parse_time(v.UTF8String);
        v = editTime(t);
    }
    self.rows[(NSUInteger)row][key] = v;
}

- (NSMutableDictionary *)blankRow
{
    return [@{@"name" : @"New Segment", @"old" : @"", @"best" : @"", @"bseg" : @"", @"icon" : @""} mutableCopy];
}

- (void)addRow:(id)s
{
    [self.rows addObject:[self blankRow]];
    [self.table reloadData];
    NSInteger r = (NSInteger)self.rows.count - 1;
    [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r] byExtendingSelection:NO];
    [self.table editColumn:0 row:r withEvent:nil select:YES];
}

- (void)insertRow:(id)s
{
    NSInteger r = self.table.selectedRow;
    if (r < 0) r = 0;
    [self.rows insertObject:[self blankRow] atIndex:(NSUInteger)r];
    [self.table reloadData];
    [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r] byExtendingSelection:NO];
    [self.table editColumn:0 row:r withEvent:nil select:YES];
}

- (void)removeRow:(id)s
{
    NSInteger r = self.table.selectedRow;
    if (r < 0) return;
    [self.rows removeObjectAtIndex:(NSUInteger)r];
    [self.table reloadData];
}

- (void)moveBy:(NSInteger)d
{
    NSInteger r = self.table.selectedRow, t = r + d;
    if (r < 0 || t < 0 || t >= (NSInteger)self.rows.count) return;
    [self.rows exchangeObjectAtIndex:(NSUInteger)r withObjectAtIndex:(NSUInteger)t];
    [self.table reloadData];
    [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)t] byExtendingSelection:NO];
}
- (void)moveUp:(id)s { [self moveBy:-1]; }
- (void)moveDown:(id)s { [self moveBy:1]; }

- (void)setIcon:(id)s
{
    NSInteger r = self.table.selectedRow;
    if (r < 0) return;
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.title = [NSString stringWithFormat:@"Set Icon for %@...", self.rows[(NSUInteger)r][@"name"]];
    op.allowedFileTypes = @[ @"png", @"bmp", @"gif", @"jpg", @"jpeg", @"tif", @"tiff" ];
    if ([op runModal] != NSModalResponseOK) return;
    self.rows[(NSUInteger)r][@"icon"] = op.URL.path;
    [self.table reloadData];
}

- (void)clearIcon:(id)s
{
    NSInteger r = self.table.selectedRow;
    if (r < 0) return;
    self.rows[(NSUInteger)r][@"icon"] = @"";
    [self.table reloadData];
}

- (void)applyOffset:(id)s
{
    [self.win makeFirstResponder:nil];
    NSString *o = timeChars(self.offsetF.stringValue);
    if (!o.length) return;
    double off = ws_parse_time(o.UTF8String);
    for (NSMutableDictionary *row in self.rows) {
        NSString *v = row[@"old"];
        if (!v.length) continue;
        double t = ws_parse_time(v.UTF8String) - off;
        row[@"old"] = editTime(t > 0 ? t : 0);
    }
    self.offsetF.stringValue = @"";
    [self.table reloadData];
}

- (void)autofill:(id)s
{
    if (WSAsk(@"Auto-fill best segments", @"Are you sure?", NO) != WS_ANSWER_YES) return;
    [self.win makeFirstResponder:nil];
    for (NSUInteger i = 0; i < self.rows.count; i++) {
        double best = ws_parse_time([self.rows[i][@"best"] UTF8String]);
        double prev = i ? ws_parse_time([self.rows[i - 1][@"best"] UTF8String]) : 0;
        double bseg = ws_parse_time([self.rows[i][@"bseg"] UTF8String]);
        double seg = 0;
        if (i == 0) seg = best;
        else if (best != 0 && prev != 0) seg = best - prev;
        if (bseg == 0 || (seg != 0 && seg < bseg)) self.rows[i][@"bseg"] = editTime(seg);
    }
    [self.table reloadData];
}

- (void)reset:(id)s { [self loadFromRun]; }

- (void)cancel:(id)s { [NSApp stopModalWithCode:NSModalResponseCancel]; }

- (void)ok:(id)s
{
    [self.win makeFirstResponder:nil]; /* commit the cell being edited */
    [NSApp stopModalWithCode:NSModalResponseOK];
}

- (BOOL)windowShouldClose:(NSWindow *)w
{
    [NSApp stopModalWithCode:NSModalResponseCancel];
    return YES;
}

- (void)commit
{
    WsRun *r = &gApp.run;
    ws_app_close_file(&gApp); /* frees icons, keeps nothing */
    /* close_file clears the file path: keep the old one so Save still works */
    ws_run_set_title(r, self.titleF.stringValue.UTF8String);
    ws_run_set_goal(r, self.goalF.stringValue.UTF8String);
    r->attempts = self.attemptsF.intValue;
    r->start_delay_ms = (int)lround(ws_parse_time(timeChars(self.delayF.stringValue).UTF8String) * 1000.0);
    for (NSDictionary *row in self.rows) {
        NSString *name = row[@"name"];
        if (!name.length) continue;
        WsSegment *sg = ws_run_add(r, name.UTF8String, ws_parse_time([row[@"old"] UTF8String]),
                                   ws_parse_time([row[@"best"] UTF8String]), ws_parse_time([row[@"bseg"] UTF8String]));
        if (sg) ws_seg_set_icon_path(sg, [row[@"icon"] UTF8String]);
    }
}
@end

void WSShowRunEditor(void)
{
    WsApp *a = &gApp;
    /* WSplit updates bests before editing and restores them on cancel */
    ws_run_update_best(&a->run, a->settings.best_as_overall);
    char *file = a->run.file ? ws_strdup(a->run.file) : NULL;
    int pw = a->detail_w, ph = a->detail_h;

    WSRunEditor *ed = [[WSRunEditor alloc] init];
    [ed build];
    [ed loadFromRun];
    bool was = a->modal_open;
    a->modal_open = true;
    [gDelegate.window setLevel:NSNormalWindowLevel]; /* keep the editor above the timer */
    NSModalResponse r = [NSApp runModalForWindow:ed.win];
    [ed.win orderOut:nil];
    a->modal_open = was;
    [gDelegate applyWindowProps];

    if (r == NSModalResponseOK) {
        [ed commit];
        if (file) ws_run_set_file(&a->run, file);
        a->detail_w = pw;
        a->detail_h = ph;
        ws_app_run_edited(a);
    } else {
        ws_run_restore_best(&a->run);
    }
    free(file);
    [gDelegate.view setNeedsDisplay:YES];
}

/* ================================================================== */
/* Settings                                                            */
/* ================================================================== */

@interface WSSettings : NSObject <NSWindowDelegate>
@property (strong) NSWindow *win;
@property (strong) NSMutableArray *appliers; /* blocks run on OK */
@property (strong) NSMutableArray<NSButton *> *hkButtons;
@property (strong) id monitor;
@property (assign) int capturing;
@property (assign) WsSettings work;
@end

@implementation WSSettings

- (NSView *)page
{
    return [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 540, 400)];
}

/* vertical cursor helper */
- (NSButton *)check:(NSString *)t on:(NSView *)v y:(CGFloat *)y value:(bool *)val
{
    NSButton *b = [NSButton checkboxWithTitle:t target:nil action:nil];
    b.frame = NSMakeRect(20, *y, 480, 20);
    b.state = *val ? NSControlStateValueOn : NSControlStateValueOff;
    [v addSubview:b];
    *y -= 24;
    [self.appliers addObject:^{ *val = b.state == NSControlStateValueOn; }];
    return b;
}

- (NSTextField *)number:(NSString *)t on:(NSView *)v y:(CGFloat *)y value:(int *)val suffix:(NSString *)suf
{
    [v addSubview:label(t, NSMakeRect(20, *y, 220, 20))];
    NSTextField *f = field([NSString stringWithFormat:@"%d", *val], NSMakeRect(244, *y - 1, 70, 22));
    [v addSubview:f];
    if (suf) [v addSubview:label(suf, NSMakeRect(320, *y, 120, 20))];
    *y -= 30;
    [self.appliers addObject:^{ *val = f.intValue; }];
    return f;
}

- (NSPopUpButton *)popup:(NSString *)t items:(NSArray *)items on:(NSView *)v y:(CGFloat *)y value:(int *)val
{
    [v addSubview:label(t, NSMakeRect(20, *y, 220, 20))];
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(240, *y - 3, 200, 26) pullsDown:NO];
    [p addItemsWithTitles:items];
    [p selectItemAtIndex:*val];
    [v addSubview:p];
    *y -= 32;
    [self.appliers addObject:^{ *val = (int)p.indexOfSelectedItem; }];
    return p;
}

- (void)build
{
    _work = gApp.settings;
    WsSettings *w = &_work;
    self.appliers = [NSMutableArray array];
    self.hkButtons = [NSMutableArray array];

    self.win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 580, 500)
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    self.win.title = @"WSplit Settings";
    self.win.delegate = self;
    self.win.releasedWhenClosed = NO;
    NSView *cv = self.win.contentView;
    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(12, 50, 556, 440)];
    [cv addSubview:tabs];

    /* --- General --- */
    NSView *g = [self page];
    CGFloat y = 360;
    [self check:@"Reload previous session's run on start" on:g y:&y value:&w->load_most_recent];
    [self check:@"Remember window position between sessions" on:g y:&y value:&w->save_window_pos];
    [self check:@"Always on top" on:g y:&y value:&w->on_top];
    y -= 8;
    [self number:@"Double tap prevention:" on:g y:&y value:&w->double_tap_guard_ms suffix:@"ms (0 = off)"];
    [self number:@"Display refresh interval:" on:g y:&y value:&w->refresh_ms suffix:@"ms (>42 = 1 decimal)"];
    [g addSubview:label(@"Window opacity:", NSMakeRect(20, y, 220, 20))];
    NSSlider *op = [NSSlider sliderWithValue:w->opacity * 100 minValue:20 maxValue:100 target:nil action:nil];
    op.frame = NSMakeRect(244, y - 2, 200, 24);
    [g addSubview:op];
    [self.appliers addObject:^{ w->opacity = op.doubleValue / 100.0; }];
    y -= 34;
    [self popup:@"Compare against:" items:@[ @"Fastest", @"Old run", @"Personal best", @"Sum of best segments" ] on:g y:&y value:&w->compare_against];
    static int dummy;
    NSPopUpButton *tb = [self popup:@"Track best as:" items:@[ @"Fastest overall run", @"Fastest to each split" ] on:g y:&y value:&dummy];
    [tb selectItemAtIndex:w->best_as_overall ? 0 : 1];
    [self.appliers removeLastObject];
    [self.appliers addObject:^{ w->best_as_overall = tb.indexOfSelectedItem == 0; }];
    if (gApp.run.count > 0) {
        y -= 6;
        [g addSubview:label(@"Run start delay (s):", NSMakeRect(20, y, 220, 20))];
        char dbuf[32];
        snprintf(dbuf, sizeof dbuf, "%g", gApp.run.start_delay_ms / 1000.0);
        NSTextField *df = field([NSString stringWithUTF8String:dbuf], NSMakeRect(244, y - 1, 70, 22));
        [g addSubview:df];
        [self.appliers addObject:^{
            int ms = (int)lround(ws_parse_time(timeChars(df.stringValue).UTF8String) * 1000.0);
            if (ms != gApp.run.start_delay_ms) { gApp.run.start_delay_ms = ms; gApp.run.unsaved = true; }
        }];
    }
    NSTabViewItem *ti = [[NSTabViewItem alloc] initWithIdentifier:@"general"];
    ti.label = @"General";
    ti.view = g;
    [tabs addTabViewItem:ti];

    /* --- Hotkeys --- */
    NSView *h = [self page];
    y = 360;
    [self check:@"Global hotkeys enabled (work while other apps are focused)" on:h y:&y value:&w->hotkeys_enabled];
    y -= 6;
    for (int i = 0; i < WS_HK_COUNT; i++) {
        [h addSubview:label([NSString stringWithUTF8String:ws_hotkey_labels[i]], NSMakeRect(20, y, 200, 20))];
        NSButton *b = button(WSPrettyHotkey(w->hotkeys[i]), NSMakeRect(224, y - 4, 180, 28), self, @selector(captureHotkey:));
        b.tag = i;
        [h addSubview:b];
        NSButton *clr = button(@"Clear", NSMakeRect(410, y - 4, 70, 28), self, @selector(clearHotkey:));
        clr.tag = i;
        [h addSubview:clr];
        [self.hkButtons addObject:b];
        y -= 32;
    }
    [h addSubview:label(@"Local keys (WSplit focused): Space/Return split, P pause, R reset, S stop,\n"
                        @"Left/Right unsplit/skip, Up/Down rows, 0-3 display mode, Tab comparison.",
                        NSMakeRect(20, 6, 500, 34))];
    ti = [[NSTabViewItem alloc] initWithIdentifier:@"hotkeys"];
    ti.label = @"Hotkeys";
    ti.view = h;
    [tabs addTabViewItem:ti];

    /* --- Display --- */
    NSView *d = [self page];
    y = 366;
    [self popup:@"Display mode:" items:@[ @"Timer only", @"Compact", @"Wide", @"Detailed" ] on:d y:&y value:&w->display_mode];
    static int dummy2;
    NSPopUpButton *bg = [self popup:@"Background:" items:@[ @"Default", @"Plain", @"Black" ] on:d y:&y value:&dummy2];
    [bg selectItemAtIndex:w->background_black ? 2 : w->background_plain ? 1 : 0];
    [self.appliers removeLastObject];
    [self.appliers addObject:^{
        w->background_plain = bg.indexOfSelectedItem == 1;
        w->background_black = bg.indexOfSelectedItem == 2;
    }];
    [self popup:@"Segment icons:" items:@[ @"None", @"16x16", @"24x24", @"32x32" ] on:d y:&y value:&w->segment_icons];
    [self number:@"Detailed mode rows:" on:d y:&y value:&w->display_segs suffix:nil];
    [self number:@"Wide mode segments:" on:d y:&y value:&w->wide_segs suffix:nil];
    CGFloat y2 = y;
    [self check:@"Show run title" on:d y:&y value:&w->show_title];
    [self check:@"Show run goal" on:d y:&y value:&w->show_goal];
    [self check:@"Show attempt count" on:d y:&y value:&w->show_attempts];
    [self check:@"Detailed: always show last" on:d y:&y value:&w->show_last_detailed];
    [self check:@"Detailed: show blank rows" on:d y:&y value:&w->display_blank_segs];
    [self check:@"Wide: always show last" on:d y:&y value:&w->show_last_wide];
    [self check:@"Wide: show blank segments" on:d y:&y value:&w->wide_seg_blanks];
    /* second column */
    NSArray *extras = @[ @"Previous segment", @"Possible time save", @"Sum of best", @"Predicted time (PB)", @"Predicted time (Best)" ];
    bool *ex[] = {&w->show_prev_seg, &w->show_time_save, &w->show_sob, &w->pred_pb, &w->pred_best};
    for (NSUInteger i = 0; i < extras.count; i++) {
        NSButton *b = [self check:extras[i] on:d y:&y2 value:ex[i]];
        b.frame = NSMakeRect(290, y2 + 24, 240, 20);
    }
    ti = [[NSTabViewItem alloc] initWithIdentifier:@"display"];
    ti.label = @"Display";
    ti.view = d;
    [tabs addTabViewItem:ti];

    /* --- Clock & fonts --- */
    NSView *f = [self page];
    y = 360;
    [self check:@"Use the digital font for the clock" on:f y:&y value:&w->digital_clock];
    [self check:@"Show decimal separator" on:f y:&y value:&w->show_decimal_separator];
    [self check:@"Horizontal gradients" on:f y:&y value:&w->horizontal_gradient];
    [self check:@"Clock accent (glossy top half)" on:f y:&y value:&w->clock_accent];
    y -= 10;
    [f addSubview:label(@"Segment font:", NSMakeRect(20, y, 220, 20))];
    NSPopUpButton *fp = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(240, y - 3, 260, 26) pullsDown:NO];
    [fp addItemWithTitle:@"System Default"];
    [fp.menu addItem:[NSMenuItem separatorItem]];
    [fp addItemsWithTitles:[[NSFontManager sharedFontManager] availableFontFamilies]];
    if (w->font_family[0]) [fp selectItemWithTitle:[NSString stringWithUTF8String:w->font_family]];
    else [fp selectItemAtIndex:0];
    [f addSubview:fp];
    [self.appliers addObject:^{
        NSString *t = fp.indexOfSelectedItem <= 0 ? @"" : fp.titleOfSelectedItem;
        snprintf(w->font_family, sizeof w->font_family, "%s", t.UTF8String);
    }];
    y -= 34;
    [f addSubview:label(@"Size multiplier:", NSMakeRect(20, y, 220, 20))];
    NSTextField *mf = field([NSString stringWithFormat:@"%.2f", w->font_multiplier], NSMakeRect(244, y - 1, 70, 22));
    [f addSubview:mf];
    [self.appliers addObject:^{
        double m = ws_parse_double(mf.stringValue.UTF8String, NULL);
        w->font_multiplier = m >= 0.5 && m <= 4 ? m : 1.0;
    }];
    ti = [[NSTabViewItem alloc] initWithIdentifier:@"fonts"];
    ti.label = @"Clock & Fonts";
    ti.view = f;
    [tabs addTabViewItem:ti];

    NSButton *defaults = button(@"Restore Defaults", NSMakeRect(16, 12, 140, 30), self, @selector(defaults:));
    NSButton *cancel = button(@"Cancel", NSMakeRect(580 - 16 - 2 * 92, 12, 88, 30), self, @selector(cancel:));
    NSButton *ok = button(@"OK", NSMakeRect(580 - 16 - 92, 12, 88, 30), self, @selector(ok:));
    ok.keyEquivalent = @"\r";
    cancel.keyEquivalent = @"\033";
    [cv addSubview:defaults];
    [cv addSubview:cancel];
    [cv addSubview:ok];
    [self.win center];
}

- (void)endCapture
{
    if (self.monitor) [NSEvent removeMonitor:self.monitor];
    self.monitor = nil;
    if (self.capturing >= 0 && self.capturing < (int)self.hkButtons.count)
        self.hkButtons[(NSUInteger)self.capturing].title = WSPrettyHotkey(_work.hotkeys[self.capturing]);
    self.capturing = -1;
}

- (void)captureHotkey:(NSButton *)b
{
    [self endCapture];
    self.capturing = (int)b.tag;
    b.title = @"Press keys... (Esc cancels)";
    __weak WSSettings *weakSelf = self;
    self.monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                         handler:^NSEvent *(NSEvent *e) {
                                                             WSSettings *s = weakSelf;
                                                             if (!s || s.capturing < 0) return e;
                                                             unsigned mods = WSModsFromFlags(e.modifierFlags);
                                                             if (e.keyCode == 53 && !mods) { [s endCapture]; return nil; } /* Esc */
                                                             const char *name = WSKeyNameForKeyCode(e.keyCode);
                                                             if (!name) return nil;
                                                             WsSettings w = s.work;
                                                             ws_hotkey_format(w.hotkeys[s.capturing], sizeof w.hotkeys[0], mods, name);
                                                             s.work = w;
                                                             [s endCapture];
                                                             return nil;
                                                         }];
}

- (void)clearHotkey:(NSButton *)b
{
    [self endCapture];
    WsSettings w = self.work;
    w.hotkeys[b.tag][0] = 0;
    self.work = w;
    self.hkButtons[(NSUInteger)b.tag].title = WSPrettyHotkey(w.hotkeys[b.tag]);
}

- (void)defaults:(id)s
{
    if (WSAsk(@"Restore defaults", @"Reset all settings (except colors and recent files) to their defaults?", NO) != WS_ANSWER_YES) return;
    WsSettings d;
    ws_settings_default(&d);
    d.colors = gApp.settings.colors;
    memcpy(d.recent, gApp.settings.recent, sizeof d.recent);
    d.recent_count = gApp.settings.recent_count;
    memcpy(d.last_file, gApp.settings.last_file, sizeof d.last_file);
    gApp.settings = d;
    [NSApp stopModalWithCode:1001];
}

- (void)cancel:(id)s { [self endCapture]; [NSApp stopModalWithCode:NSModalResponseCancel]; }

- (void)ok:(id)s
{
    [self endCapture];
    [self.win makeFirstResponder:nil];
    WsSettings keep = _work; /* hotkeys captured into _work */
    for (void (^blk)(void) in self.appliers) blk();
    for (int i = 0; i < WS_HK_COUNT; i++) memcpy(_work.hotkeys[i], keep.hotkeys[i], sizeof keep.hotkeys[i]);
    /* sanity */
    if (_work.display_segs < 2) _work.display_segs = 2;
    if (_work.display_segs > 40) _work.display_segs = 40;
    if (_work.wide_segs < 1) _work.wide_segs = 1;
    if (_work.wide_segs > 20) _work.wide_segs = 20;
    if (_work.refresh_ms < 5) _work.refresh_ms = 5;
    if (_work.double_tap_guard_ms < 0) _work.double_tap_guard_ms = 0;
    gApp.settings = _work;
    [NSApp stopModalWithCode:NSModalResponseOK];
}

- (BOOL)windowShouldClose:(NSWindow *)w
{
    [self endCapture];
    [NSApp stopModalWithCode:NSModalResponseCancel];
    return YES;
}
@end

void WSShowSettings(void)
{
    WSSettings *st = [[WSSettings alloc] init];
    st.capturing = -1;
    [st build];
    [gDelegate unregisterHotkeys]; /* so captured keys reach the dialog */
    bool was = gApp.modal_open;
    gApp.modal_open = true;
    [gDelegate.window setLevel:NSNormalWindowLevel];
    NSModalResponse r = [NSApp runModalForWindow:st.win];
    [st.win orderOut:nil];
    gApp.modal_open = was;
    if (r == NSModalResponseOK || r == 1001) {
        ws_app_layout_changed(&gApp);
        [gDelegate rebuildFonts];
    }
    [gDelegate applyWindowProps];
    [gDelegate registerHotkeys];
    [gDelegate.view setNeedsDisplay:YES];
}

/* ================================================================== */
/* Colour editor (port of CustomizeColors)                             */
/* ================================================================== */

typedef struct { const char *name; size_t off; } ColorField;
static const ColorField kColorFields[] = {
#define X(n, d) {#n, offsetof(WsColors, n)},
    WS_COLOR_FIELDS(X)
#undef X
};

@interface WSFlippedView : NSView
@end
@implementation WSFlippedView
- (BOOL)isFlipped { return YES; }
@end

@interface WSColors : NSObject <NSWindowDelegate>
@property (strong) NSWindow *win;
@property (strong) NSMutableArray<NSColorWell *> *wells;
@property (assign) WsColors backup;
@end

@implementation WSColors

- (WsColor *)slot:(NSInteger)i { return (WsColor *)((char *)&gApp.settings.colors + kColorFields[i].off); }

- (void)refreshWells
{
    for (NSUInteger i = 0; i < self.wells.count; i++) self.wells[i].color = WSNSColor(*[self slot:(NSInteger)i]);
}

- (void)build
{
    self.backup = gApp.settings.colors;
    self.wells = [NSMutableArray array];
    self.win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 560)
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    self.win.title = @"Set Colors";
    self.win.delegate = self;
    self.win.releasedWhenClosed = NO;
    NSView *cv = self.win.contentView;
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 52, 460, 508)];
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    sv.hasVerticalScroller = YES;
    size_t n = sizeof kColorFields / sizeof kColorFields[0];
    WSFlippedView *doc = [[WSFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 440, 12 + n * 30)];
    for (size_t i = 0; i < n; i++) {
        CGFloat y = 8 + i * 30;
        /* split CamelCase into words for the label */
        NSMutableString *t = [NSMutableString string];
        const char *nm = kColorFields[i].name;
        for (const char *c = nm; *c; c++) {
            if (c != nm && ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9' && !(c[-1] >= '0' && c[-1] <= '9'))))
                [t appendString:@" "];
            [t appendFormat:@"%c", *c];
        }
        NSTextField *l = label(t, NSMakeRect(16, y + 3, 280, 20));
        [doc addSubview:l];
        NSColorWell *w = [[NSColorWell alloc] initWithFrame:NSMakeRect(320, y, 80, 24)];
        w.tag = (NSInteger)i;
        w.target = self;
        w.action = @selector(colorChanged:);
        [doc addSubview:w];
        [self.wells addObject:w];
    }
    sv.documentView = doc;
    [cv addSubview:sv];
    [self refreshWells];
    [NSColorPanel sharedColorPanel].showsAlpha = NO;

    NSArray *btns = @[ @[ @"Load...", @"load:" ], @[ @"Save...", @"save:" ], @[ @"Defaults", @"defaults:" ] ];
    CGFloat x = 12;
    for (NSArray *b in btns) {
        [cv addSubview:button(b[0], NSMakeRect(x, 12, 84, 28), self, NSSelectorFromString(b[1]))];
        x += 88;
    }
    NSButton *cancel = button(@"Cancel", NSMakeRect(460 - 12 - 2 * 88, 12, 84, 28), self, @selector(cancel:));
    NSButton *ok = button(@"OK", NSMakeRect(460 - 12 - 88, 12, 84, 28), self, @selector(ok:));
    cancel.autoresizingMask = ok.autoresizingMask = NSViewMinXMargin;
    ok.keyEquivalent = @"\r";
    cancel.keyEquivalent = @"\033";
    [cv addSubview:cancel];
    [cv addSubview:ok];
    [self.win center];
}

- (void)colorChanged:(NSColorWell *)w
{
    *[self slot:w.tag] = WSColorFromNSColor(w.color);
    ws_app_colors_changed(&gApp);
    [gDelegate.view setNeedsDisplay:YES];
}

- (void)load:(id)s
{
    NSOpenPanel *op = [NSOpenPanel openPanel];
    if ([op runModal] != NSModalResponseOK) return;
    if (!ws_colors_load(&gApp.settings.colors, op.URL.fileSystemRepresentation)) WSMessage(@"Colors", @"No colors could be read from that file.");
    [self refreshWells];
    ws_app_colors_changed(&gApp);
}

- (void)save:(id)s
{
    NSSavePanel *sp = [NSSavePanel savePanel];
    sp.nameFieldStringValue = @"colors.wscolors";
    if ([sp runModal] != NSModalResponseOK) return;
    if (!ws_colors_save(&gApp.settings.colors, sp.URL.fileSystemRepresentation)) WSMessage(@"Colors", @"Could not save the color scheme.");
}

- (void)defaults:(id)s
{
    if (WSAsk(@"Restore Defaults", @"Are you sure?", NO) != WS_ANSWER_YES) return;
    ws_colors_default(&gApp.settings.colors);
    [self refreshWells];
    ws_app_colors_changed(&gApp);
}

- (void)cancel:(id)s
{
    gApp.settings.colors = self.backup;
    ws_app_colors_changed(&gApp);
    [NSApp stopModalWithCode:NSModalResponseCancel];
}

- (void)ok:(id)s { [NSApp stopModalWithCode:NSModalResponseOK]; }

- (BOOL)windowShouldClose:(NSWindow *)w
{
    [self cancel:nil];
    return YES;
}
@end

void WSShowColors(void)
{
    WSColors *c = [[WSColors alloc] init];
    [c build];
    [gDelegate.window setLevel:NSNormalWindowLevel];
    [NSApp runModalForWindow:c.win];
    [c.win orderOut:nil];
    [[NSColorPanel sharedColorPanel] orderOut:nil];
    [gDelegate applyWindowProps];
    [gDelegate.view setNeedsDisplay:YES];
}
