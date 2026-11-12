/* WSplit for macOS - shared declarations. */
#import <Cocoa/Cocoa.h>
#include "../core/wsplit.h"

@interface WSView : NSView
@end

@interface WSWindow : NSWindow
@end

@interface WSAppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
@property (strong) WSWindow *window;
@property (strong) WSView *view;
@property (strong) NSMenu *splitsMenu;
- (void)registerHotkeys;
- (void)unregisterHotkeys;
- (void)rebuildFonts;
- (void)runCommand:(int)cmd;
- (void)menuItemClicked:(id)sender;
- (void)applyWindowProps;
- (void)resizeContentTo:(NSSize)size;
@end

extern WsApp gApp;
extern WSAppDelegate *gDelegate;

/* keys */
const char *WSKeyNameForKeyCode(unsigned short code); /* NULL if unknown */
int WSKeyCodeForName(const char *name);               /* -1 if unknown */
unsigned WSModsFromFlags(NSEventModifierFlags f);
NSString *WSPrettyHotkey(const char *hk);

/* colours */
NSColor *WSNSColor(WsColor c);
WsColor WSColorFromNSColor(NSColor *c);

/* dialogs (Dialogs.m) */
void WSShowRunEditor(void);
void WSShowSettings(void);
void WSShowColors(void);
WsAnswer WSAsk(NSString *title, NSString *msg, BOOL cancel);
void WSMessage(NSString *title, NSString *msg);
NSMenu *WSBuildNSMenu(WsMenu *m, id target);
