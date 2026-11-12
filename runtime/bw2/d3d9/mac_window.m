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
 *     translator forgets what it gave down: input.c gives the game its ups);
 *   - the cursor follows cursor.c's policy (plan 6 Task 5, bw2_mac_cursor_apply):
 *     warped, detached and hidden only while the game window is key.
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

static NSCursor *cursor_image(void);
static void cursor_refresh(void);
static void cursor_release(void);

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
                                                               NSTrackingMouseEnteredAndExited |
                                                               NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                         owner:self userInfo:nil]];
    [super updateTrackingAreas];
}
- (void)cursorUpdate:(NSEvent *)e { (void)e; [cursor_image() set]; cursor_refresh(); }
- (void)mouseEntered:(NSEvent *)e { (void)e; cursor_refresh(); }
- (void)mouseExited:(NSEvent *)e { (void)e; cursor_refresh(); }

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
- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; push_kind(BW2_IN_ACTIVATE, 1); cursor_refresh(); }
- (void)windowDidResignKey:(NSNotification *)n {
    (void)n;
    push_kind(BW2_IN_ACTIVATE, 0);
    bw2_translate_reset(&self.view->tr);         /* input.c gives the game the ups */
    cursor_release();                            /* shown and reattached at once, whatever the game says */
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
        cursor_refresh();                        /* no game window: the pointer is the user's again */
    });
}

/* ---- the cursor (plan 6 Task 5): cursor.c's policy, applied ----
 * The mode comes from the guest thread (bw2_cursor_publish, dispatch_async). Here:
 *   - while the game window is key: detached as the mode says (warped into the picture first, so the
 *     frozen pointer is inside the view and AppKit's clicks reach it), hidden while detached, and
 *     otherwise hidden only while the pointer is inside the view and the mode hides it;
 *   - while it is not key (resigned, or a mode that raced a Cmd-Tab): shown and attached, no warp;
 *   - [NSCursor hide]/unhide kept balanced with a flag. */
static struct bw2_cursor_mode g_cmode;   /* the last mode received, without its warp */
static NSCursor *g_cimage;               /* the mode's image; nil: the arrow */
static int g_chidden, g_cdetached;      /* what is applied now */

static NSCursor *cursor_image(void) { return g_cimage ?: NSCursor.arrowCursor; }

static void set_hidden(int h) {
    if (h == g_chidden) return;
    g_chidden = h;
    if (h) [NSCursor hide];
    else [NSCursor unhide];
}
static void set_detached(int d) {
    if (d == g_cdetached) return;
    g_cdetached = d;
    CGAssociateMouseAndMouseCursorPosition(d ? false : true);
}
static BW2Game *cursor_game(void) {
    BW2Game *g = g_games.lastObject;
    return g && g.window.isKeyWindow ? g : nil;
}
static int pointer_inside(BW2Game *g) {
    NSPoint p = [g.view convertPoint:g.window.mouseLocationOutsideOfEventStream fromView:nil];
    return NSPointInRect(p, g.view.bounds);
}
/* A client point (back-buffer pixels) to the global display (top-left origin), through the letterbox. */
static void warp_to(BW2Game *g, int cx, int cy) {
    NSSize s = g.view.bounds.size;
    struct bw2_geom geom = { s.width, s.height, g.view.bbW, g.view.bbH };
    double vx, vy;
    bw2_client_to_view(&geom, cx, cy, &vx, &vy);
    NSPoint w = [g.view convertPoint:NSMakePoint(vx, vy) toView:nil];
    NSPoint scr = [g.window convertPointToScreen:w];
    CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
    CGWarpMouseCursorPosition(CGPointMake(scr.x, top - scr.y));
    CGAssociateMouseAndMouseCursorPosition(true);   /* no event suppression after the warp */
    g_cdetached = 0;
}
static void cursor_release(void) { set_hidden(0), set_detached(0); }
/* Apply g_cmode as the window's state allows; warp is the mode's own warp, if any. */
static void cursor_apply_mode(const struct bw2_cursor_mode *m) {
    BW2Game *g = cursor_game();
    if (!g) { cursor_release(); return; }
    if (m && m->warp) warp_to(g, m->warp_x, m->warp_y);
    else if (g_cmode.detached && !g_cdetached)       /* a detach whose warp was not applied: the centre */
        warp_to(g, (int)g.view.bbW / 2, (int)g.view.bbH / 2);
    set_detached(g_cmode.detached);
    set_hidden(g_cmode.detached || (g_cmode.hidden && pointer_inside(g)));
}
static void cursor_refresh(void) { cursor_apply_mode(NULL); }

void bw2_mac_cursor_apply(const struct bw2_cursor_mode *m, const uint8_t *rgba, int hot_x, int hot_y) {
    g_cmode = *m;
    g_cmode.warp = 0;
    if (!m->image) g_cimage = nil;
    else if (rgba) {
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:32 pixelsHigh:32
                                                                     bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                       bytesPerRow:32 * 4 bitsPerPixel:32];
        memcpy(rep.bitmapData, rgba, 32 * 32 * 4);  /* black, white or clear: premultiplied as it is */
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(32, 32)];
        [img addRepresentation:rep];
        g_cimage = [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint(hot_x, hot_y)];
    }
    BW2Game *g = cursor_game();
    if (g && pointer_inside(g)) [cursor_image() set];
    cursor_apply_mode(m);
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

/* ---- --selftest-cursor-window (main.c): the cursor policy on a real window ---- */
static CGPoint g_saved_pointer;
static CGPoint pointer_now(void) {
    CGEventRef e = CGEventCreate(NULL);
    CGPoint p = CGEventGetLocation(e);
    CFRelease(e);
    return p;
}
/* The pointer to the main display's top-left corner, below the menu bar: outside the window to come. */
void bw2_mac_selftest_park_pointer(void) {
    bw2_main_sync(^{
        g_saved_pointer = pointer_now();
        CGRect b = CGDisplayBounds(CGMainDisplayID());
        CGWarpMouseCursorPosition(CGPointMake(b.origin.x + 8, b.origin.y + 60));
        CGAssociateMouseAndMouseCursorPosition(true);
    });
}
void bw2_mac_selftest_restore_pointer(void) {
    bw2_main_sync(^{
        cursor_release();
        CGWarpMouseCursorPosition(g_saved_pointer);
        CGAssociateMouseAndMouseCursorPosition(true);
    });
}
/* Hide the app (as Cmd-H or Cmd-Tab away leaves the game) or bring it back. */
void bw2_mac_selftest_app_active(int active) {
    bw2_main_sync(^{
        if (!active) { [NSApp hide:nil]; return; }
        [NSApp unhide:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [g_games.lastObject.window makeKeyAndOrderFront:nil];
    });
}
/* The newest game window's cursor as applied: key, hidden, detached, the pointer inside the view and
 * its client point. 0 if there is no game window. */
int bw2_mac_selftest_cursor(int *key, int *hidden, int *detached, int *inside, int *cx, int *cy) {
    __block int ok = 0;
    bw2_main_sync(^{
        BW2Game *g = g_games.lastObject;
        if (!g) return;
        CGPoint gp = pointer_now();
        CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
        NSPoint w = [g.window convertPointFromScreen:NSMakePoint(gp.x, top - gp.y)];
        NSPoint v = [g.view convertPoint:w fromView:nil];
        NSSize s = g.view.bounds.size;
        struct bw2_geom geom = { s.width, s.height, g.view.bbW, g.view.bbH };
        struct bw2_translator t;
        memset(&t, 0, sizeof t);
        struct bw2_mac_ev m = { .type = BW2_MAC_MOVE, .x = v.x, .y = v.y };
        struct bw2_in_event out[4];
        int n = bw2_translate(&t, &geom, &m, out, 4);
        *key = g.window.isKeyWindow, *hidden = g_chidden, *detached = g_cdetached;
        *inside = NSPointInRect(v, g.view.bounds);
        *cx = n ? out[0].x : -1, *cy = n ? out[0].y : -1;
        ok = 1;
    });
    return ok;
}
