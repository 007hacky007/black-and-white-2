/*
 * mac_window.m - the game's window on the Mac (plan 6 Task 4): the NSWindow
 * a D3D9 device draws into, its layer-hosting content view (BW2View, which
 * holds the CAMetalLayer and turns every mouse, scroll, key and modifier
 * event into the game's input), the window delegate (close and focus) and
 * the app delegate (Cmd-Q).
 *
 * Rules (plan 6 Global Constraints):
 *   - windowed: a resizable window whose first size is the back buffer in
 *     points, scaled down to fit 90% of the screen's visible frame; full
 *     screen: macOS full screen (toggleFullScreen:, its own Space). A
 *     full-screen device starts there, a windowed one windowed; Cmd+Ctrl+F
 *     switches at any time without telling the game. Either way plan 4's
 *     present letterboxes the back buffer into the drawable, which follows
 *     the view's bounds times the backing scale;
 *   - each event goes through bw2_translate (translate.c) with the view's
 *     geometry, then onto the host queue (bw2_in_push). keyDown never calls
 *     interpretKeyEvents:, so no press-and-hold accent popup appears;
 *   - the close button and Cmd-Q push CLOSE (the game gets WM_CLOSE) and
 *     never close or quit by themselves while a game window exists;
 *   - becoming key pushes ACTIVATE 1, resigning key ACTIVATE 0 (and the
 *     translator forgets what it gave down: input.c gives the game its ups).
 * Everything here runs on the main thread, except bw2_mac_window_open/mode/
 * close's own first lines, which run on the guest thread and hand the rest
 * over with bw2_main_sync. The main thread never touches guest memory or
 * win32hle state: it only pushes to the host queue.
 */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "bw2.h"
#include "mtl.h"
#include "mtl_internal.h"
#include "input.h"

static MtlCtx *ctx_of(struct mtl_ctx *c) { return (__bridge MtlCtx *)(void *)c; }

static void push_kind(enum bw2_in_kind kind, int down) {
    struct bw2_in_event e = { .kind = kind, .down = down };
    bw2_in_push(&e);
}

/* ---- the view ---- */
@interface BW2View : NSView {
@public
    struct bw2_translator tr;            /* main thread only */
}
@property uint32_t bbW, bbH;             /* the back buffer, in pixels */
@property(strong) CAMetalLayer *metal;
@end

@implementation BW2View
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)isOpaque { return YES; }

/* The drawable follows the view's bounds times the backing scale. */
- (void)syncDrawable {
    CAMetalLayer *l = self.metal;
    if (!l) return;
    CGFloat k = self.window ? self.window.backingScaleFactor : 1;
    NSSize s = self.bounds.size;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    l.frame = self.bounds;
    l.contentsScale = k;
    l.drawableSize = CGSizeMake(fmax(1, round(s.width * k)), fmax(1, round(s.height * k)));
    [CATransaction commit];
}
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self syncDrawable]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self syncDrawable]; }
- (void)viewDidMoveToWindow { [super viewDidMoveToWindow]; [self syncDrawable]; }

- (void)updateTrackingAreas {
    for (NSTrackingArea *a in [self.trackingAreas copy]) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                       options:NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                                               NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                         owner:self userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)cursorUpdate:(NSEvent *)e { (void)e; [[NSCursor arrowCursor] set]; }   /* the cursor policy is Task 5's */

/* An event's point in view points. An event with no window (one made from a CGEvent) carries it on
 * the screen. */
- (NSPoint)pointOf:(NSEvent *)e {
    NSPoint p = e.locationInWindow;
    if (!e.window && self.window) p = [self.window convertPointFromScreen:p];
    return [self convertPoint:p fromView:nil];
}
/* One Mac event through the translator onto the host queue. */
- (void)feed:(const struct bw2_mac_ev *)m {
    NSSize s = self.bounds.size;
    struct bw2_geom g = { s.width, s.height, self.bbW, self.bbH };
    struct bw2_in_event out[4];
    int n = bw2_translate(&tr, &g, m, out, 4);
    for (int i = 0; i < n; i++) bw2_in_push(&out[i]);
}
- (void)mouse:(NSEvent *)e type:(int)type {
    NSPoint p = [self pointOf:e];
    struct bw2_mac_ev m = { .type = type, .x = p.x, .y = p.y, .dx = e.deltaX, .dy = e.deltaY,
                            .button = (int)e.buttonNumber, .mods = e.modifierFlags };
    [self feed:&m];
}
- (void)mouseDown:(NSEvent *)e { [self mouse:e type:BW2_MAC_DOWN]; }
- (void)mouseUp:(NSEvent *)e { [self mouse:e type:BW2_MAC_UP]; }
- (void)rightMouseDown:(NSEvent *)e { [self mouse:e type:BW2_MAC_DOWN]; }
- (void)rightMouseUp:(NSEvent *)e { [self mouse:e type:BW2_MAC_UP]; }
- (void)otherMouseDown:(NSEvent *)e { [self mouse:e type:BW2_MAC_DOWN]; }
- (void)otherMouseUp:(NSEvent *)e { [self mouse:e type:BW2_MAC_UP]; }
- (void)mouseMoved:(NSEvent *)e { [self mouse:e type:BW2_MAC_MOVE]; }
- (void)mouseDragged:(NSEvent *)e { [self mouse:e type:BW2_MAC_MOVE]; }
- (void)rightMouseDragged:(NSEvent *)e { [self mouse:e type:BW2_MAC_MOVE]; }
- (void)otherMouseDragged:(NSEvent *)e { [self mouse:e type:BW2_MAC_MOVE]; }
- (void)scrollWheel:(NSEvent *)e {
    NSPoint p = [self pointOf:e];
    struct bw2_mac_ev m = { .type = BW2_MAC_SCROLL, .x = p.x, .y = p.y, .mods = e.modifierFlags,
                            .precise = e.hasPreciseScrollingDeltas, .momentum = e.momentumPhase != NSEventPhaseNone,
                            .scroll_dy = e.scrollingDeltaY };
    [self feed:&m];
}
- (void)keyDown:(NSEvent *)e {                   /* no interpretKeyEvents: no accent popup, no beep */
    struct bw2_mac_ev m = { .type = BW2_MAC_KEYDOWN, .keycode = e.keyCode, .mods = e.modifierFlags,
                            .repeat = e.isARepeat };
    [self feed:&m];
}
- (void)keyUp:(NSEvent *)e {
    struct bw2_mac_ev m = { .type = BW2_MAC_KEYUP, .keycode = e.keyCode, .mods = e.modifierFlags };
    [self feed:&m];
}
- (void)flagsChanged:(NSEvent *)e {              /* the full flags: the device bits tell left from right */
    struct bw2_mac_ev m = { .type = BW2_MAC_FLAGS, .keycode = e.keyCode, .mods = e.modifierFlags };
    [self feed:&m];
}
@end

/* ---- the window delegate: one per game window ---- */
@interface BW2Game : NSObject <NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) BW2View *view;
@property int fullscreen;                /* what the device last asked for */
@end

static NSMutableArray<BW2Game *> *g_games;   /* the live game windows, newest last (main thread) */

@implementation BW2Game
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w; push_kind(BW2_IN_CLOSE, 0); return NO; }
- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; push_kind(BW2_IN_ACTIVATE, 1); }
- (void)windowDidResignKey:(NSNotification *)n {
    (void)n;
    push_kind(BW2_IN_ACTIVATE, 0);
    bw2_translate_reset(&self.view->tr);         /* input.c gives the game the ups */
}
@end

static int in_full_screen(NSWindow *w) { return (w.styleMask & NSWindowStyleMaskFullScreen) != 0; }

/* The windowed content size for a back buffer, on the window's screen. */
static NSSize fit_size(NSWindow *w, uint32_t bb_w, uint32_t bb_h) {
    NSScreen *s = (w ? w.screen : nil) ?: NSScreen.mainScreen;
    NSRect vis = s ? s.visibleFrame : NSZeroRect;
    double cw, ch;
    bw2_window_fit(vis.size.width, vis.size.height, bb_w, bb_h, &cw, &ch);
    return NSMakeSize(fmax(1, round(cw)), fmax(1, round(ch)));
}

void bw2_mac_window_open(struct mtl_ctx *cc, uint32_t hwnd, uint32_t bb_w, uint32_t bb_h, int fullscreen) {
    if (hwnd) bw2_in_set_window(hwnd);           /* guest thread: the window the input's messages go to */
    MtlCtx *c = ctx_of(cc);
    bw2_main_sync(^{
        if (!g_games) g_games = [NSMutableArray array];
        NSSize size = fit_size(nil, bb_w, bb_h);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskMiniaturizable |
                                                              NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @"Black & White 2";
        win.releasedWhenClosed = NO;
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        win.acceptsMouseMovedEvents = YES;
        win.backgroundColor = NSColor.blackColor;
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = c.dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.displaySyncEnabled = c.vsync ? YES : NO;
        layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        BW2View *view = [[BW2View alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
        view.bbW = bb_w, view.bbH = bb_h;
        view.metal = layer;
        view.layer = layer;                      /* layer-hosting: the layer first, then wantsLayer */
        view.wantsLayer = YES;
        win.contentView = view;
        [view syncDrawable];
        BW2Game *g = [BW2Game new];
        g.window = win, g.view = view, g.fullscreen = fullscreen != 0;
        win.delegate = g;
        [g_games addObject:g];
        [win center];
        [win makeFirstResponder:view];
        [win makeKeyAndOrderFront:nil];
        if (!win.isKeyWindow) push_kind(BW2_IN_ACTIVATE, 0);   /* opened behind another app: not active */
        if (fullscreen) [win toggleFullScreen:nil];
        c.window = win;
        c.layer = layer;
    });
}

static BW2Game *game_of(NSWindow *w) {
    for (BW2Game *g in g_games) if (g.window == w) return g;
    return nil;
}

/* A Reset: the new back buffer; a change of the device's windowed flag switches the window to match.
 * The same flag again leaves the user's Cmd+Ctrl+F choice alone. */
void bw2_mac_window_mode(struct mtl_ctx *cc, uint32_t bb_w, uint32_t bb_h, int fullscreen) {
    NSWindow *win = ctx_of(cc).window;
    if (!win) return;
    bw2_main_sync(^{
        BW2Game *g = game_of(win);
        if (!g) return;
        int resized = g.view.bbW != bb_w || g.view.bbH != bb_h, fs = fullscreen != 0;
        g.view.bbW = bb_w, g.view.bbH = bb_h;
        if (fs != g.fullscreen) {
            g.fullscreen = fs;
            if (fs != in_full_screen(win)) [win toggleFullScreen:nil];
        }
        if (resized && !fs && !in_full_screen(win)) [win setContentSize:fit_size(win, bb_w, bb_h)];
        [g.view syncDrawable];
    });
}

void bw2_mac_window_close(struct mtl_ctx *cc) {
    NSWindow *win = ctx_of(cc).window;
    if (!win) return;
    bw2_main_sync(^{
        BW2Game *g = game_of(win);
        win.delegate = nil;
        [win orderOut:nil];
        [win close];
        if (g) [g_games removeObject:g];
    });
}

/* ---- the app delegate ---- */
@interface BW2AppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation BW2AppDelegate
/* Cmd-Q (and logout) while a game window exists: the game gets WM_CLOSE and decides (spec 7.5). */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app;
    if (g_games.count) { push_kind(BW2_IN_CLOSE, 0); return NSTerminateCancel; }
    return NSTerminateNow;
}
@end

void bw2_mac_install_app_delegate(void) {     /* mac_main.m, on the main thread */
    static BW2AppDelegate *d;
    d = [BW2AppDelegate new];
    NSApp.delegate = d;
}

/* ---- --selftest-input-window (main.c): synthetic events on the newest game window ---- */
static NSEvent *mouse_event(NSWindow *w, NSEventType t, NSPoint p) {
    return [NSEvent mouseEventWithType:t location:p modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                          windowNumber:w.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1];
}
static NSEvent *key_event(NSWindow *w, NSEventType t, unsigned short code, NSString *chars) {
    return [NSEvent keyEventWithType:t location:NSZeroPoint modifierFlags:0
                           timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:w.windowNumber context:nil
                          characters:chars charactersIgnoringModifiers:chars isARepeat:NO keyCode:code];
}

/* Left down and up at the view's centre, the A key, a scroll of 3 lines; 0 if there is no game window. */
int bw2_mac_selftest_input(void) {
    __block int ok = 0;
    bw2_main_sync(^{
        BW2Game *g = g_games.lastObject;
        if (!g) return;
        NSWindow *w = g.window;
        NSRect b = g.view.bounds;
        NSPoint mid = [g.view convertPoint:NSMakePoint(NSMidX(b), NSMidY(b)) toView:nil];
        [w sendEvent:mouse_event(w, NSEventTypeLeftMouseDown, mid)];
        [w sendEvent:mouse_event(w, NSEventTypeLeftMouseUp, mid)];
        [w sendEvent:key_event(w, NSEventTypeKeyDown, 0, @"a")];
        [w sendEvent:key_event(w, NSEventTypeKeyUp, 0, @"a")];
        /* a line scroll: a CGEvent placed over the view's centre, in global (top-left) coordinates */
        NSPoint scr = [w convertPointToScreen:mid];
        CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
        CGEventRef cg = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, 3);
        CGEventSetLocation(cg, CGPointMake(scr.x, top - scr.y));
        NSEvent *s = [NSEvent eventWithCGEvent:cg];
        CFRelease(cg);
        [g.view scrollWheel:s];                  /* made from a CGEvent, it has no window: straight to the view */
        ok = 1;
    });
    return ok;
}
/* The close button, as a click on it does it. */
void bw2_mac_selftest_close(void) {
    bw2_main_sync(^{ [g_games.lastObject.window performClose:nil]; });
}
