/*
 * mac_main.m - the process main thread (plan 4 Task 1). The guest runs on its
 * own big-stack thread (threads.c); the main thread runs the run loop, so
 * AppKit and window work can be sent to it with bw2_main_sync. Headless
 * (BW2_HEADLESS=1): no NSApplication, only the run loop that drains the
 * main dispatch queue.
 */
#import <AppKit/AppKit.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "bw2.h"

static atomic_int g_guest_done;

int bw2_headless(void) {
    const char *e = getenv("BW2_HEADLESS");
    return e && !strcmp(e, "1");
}

void bw2_main_sync(void (^block)(void)) {
    if (pthread_main_np()) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

/* Called on the guest thread when it ends: wake the main thread's loop. */
void bw2_mac_guest_done(void) {
    atomic_store(&g_guest_done, 1);
    CFRunLoopWakeUp(CFRunLoopGetMain());
    if (!bw2_headless()) {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                                       modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
            [NSApp postEvent:e atStart:YES];
        });
    }
}

void bw2_mac_install_app_delegate(void);   /* mac_window.m: Cmd-Q reaches the game as SC_CLOSE */

static NSMenu *submenu(NSMenu *bar, NSString *title) {
    NSMenuItem *item = [[NSMenuItem alloc] init];
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];
    item.submenu = m;
    [bar addItem:item];
    return m;
}

/* The menus (plan 6): the app menu (Hide Cmd-H, Quit Cmd-Q), View (Enter Full Screen, Cmd+Ctrl+F;
 * AppKit retitles it while in full screen) and Window (Minimize Cmd-M). */
static void menus(void) {
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenu *app = submenu(bar, @"Black & White 2");
    [app addItemWithTitle:@"Hide Black & White 2" action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:@"Quit Black & White 2" action:@selector(terminate:) keyEquivalent:@"q"];
    NSMenu *view = submenu(bar, @"View");
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:)
                              keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    NSMenu *window = submenu(bar, @"Window");
    [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [NSApp setMainMenu:bar];
    NSApp.windowsMenu = window;
}

static void app_setup(void) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    bw2_mac_install_app_delegate();
    menus();
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
}

uint32_t bw2_mac_main_loop(pthread_t guest) {
    if (bw2_headless()) {
        while (!atomic_load(&g_guest_done))
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    } else {
        @autoreleasepool { app_setup(); }
        while (!atomic_load(&g_guest_done)) {
            @autoreleasepool {
                NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]
                                                   inMode:NSDefaultRunLoopMode dequeue:YES];
                if (e && [e type] != NSEventTypeApplicationDefined) [NSApp sendEvent:e];
            }
        }
    }
    void *ret = NULL;
    pthread_join(guest, &ret);
    return (uint32_t)(uintptr_t)ret;
}
