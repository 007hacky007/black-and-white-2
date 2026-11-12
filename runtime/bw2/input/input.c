/*
 * input.c - the host input queue and the Windows side of input (plan 6).
 *
 * The Mac side pushes events (bw2_in_push, any thread, a leaf mutex, a ring
 * that grows). The guest thread drains them:
 *   - fully from win32hle's pump hook, inside PeekMessage/GetMessage, where
 *     Windows delivers sent messages: an activation is sent there;
 *   - partly (bw2_in_drain_input) from the DirectInput reads, GetCursorPos
 *     and GetAsyncKeyState: moves, buttons, wheel, keys and close only post,
 *     so no window procedure runs inside those calls. It stops at the first
 *     activation and leaves it, and what follows, for the pump. An inactive
 *     one still counts as met there: the window is inactive and the
 *     foreground devices are lost at once (bw2_in_mouse_lost, once).
 * An inactive activation first posts an up for every key and button the
 * game was given down, and lets the pump hand those out before it sends
 * WM_ACTIVATE, WM_KILLFOCUS and WM_ACTIVATEAPP.
 * The cursor calls (ShowCursor, SetCursor, SetCursorPos, ClipCursor) answer
 * as win32hle does, then recompute the cursor policy (cursor.c), as each
 * activation the full drain delivers does.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"
#include "bw2.h"

#define WM_ACTIVATE    0x0006u
#define WM_SETFOCUS    0x0007u
#define WM_KILLFOCUS   0x0008u
#define WM_CLOSE       0x0010u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP   0x0208u
#define WA_INACTIVE    0u
#define WA_ACTIVE      1u
/* WM_ACTIVATEAPP's lParam names the other application's thread. Here there
 * is none in the guest (the other app is a Mac one), so it is 0. */
#define OTHER_THREAD   0u

/* ---- the queue (any thread) ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct bw2_in_event *g_ring;
static size_t g_cap, g_head, g_n;

static void push(const struct bw2_in_event *e) {
    pthread_mutex_lock(&g_mu);
    if (g_n == g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 256;
        struct bw2_in_event *r = malloc(cap * sizeof *r);
        if (!r) abort();
        for (size_t i = 0; i < g_n; i++) r[i] = g_ring[(g_head + i) % g_cap];
        free(g_ring);
        g_ring = r, g_cap = cap, g_head = 0;
    }
    g_ring[(g_head + g_n) % g_cap] = *e;
    g_n++;
    pthread_mutex_unlock(&g_mu);
}
void bw2_in_push(const struct bw2_in_event *e) {
    struct bw2_in_event x = *e;
    x.time_ms = hle_ticks_ms();
    push(&x);
}
void bw2_in_push_at(const struct bw2_in_event *e) { push(e); }

static int pop(struct bw2_in_event *out) {
    pthread_mutex_lock(&g_mu);
    int got = g_n != 0;
    if (got) {
        *out = g_ring[g_head];
        g_head = (g_head + 1) % g_cap, g_n--;
    }
    pthread_mutex_unlock(&g_mu);
    return got;
}

/* ---- the guest side (guest thread only) ---- */
static uint32_t g_window;
static int g_active = 1;
static unsigned g_losses;
static void (*g_sink)(const struct bw2_in_event *e);
static int g_clip_on, g_clip[4];
static int g_sx, g_sy;                   /* the last screen point given to win32hle */
static struct { int down; uint32_t scan; int extended; } g_key[256];   /* as given to the game */
static int g_button[3];
/* The activation a drain stopped at: still first in line, taken off the ring. */
static struct bw2_in_event g_held;
static int g_held_set, g_held_lost, g_held_ups;
static int g_draining;

void bw2_in_set_window(uint32_t hwnd) { g_window = hwnd; }
uint32_t bw2_in_window(void) { return g_window ? g_window : hle_first_hwnd(); }
static uint32_t window(void) { return bw2_in_window(); }
int bw2_in_active(void) { return g_active; }
unsigned bw2_in_losses(void) { return g_losses; }
void bw2_in_set_mouse_sink(void (*fn)(const struct bw2_in_event *e)) { g_sink = fn; }

void bw2_in_clip(const int *r) {
    g_clip_on = r && r[2] > r[0] && r[3] > r[1];
    if (g_clip_on) memcpy(g_clip, r, sizeof g_clip);
}

/* A screen point clamped to the clip while active. */
static void clamp(int *x, int *y) {
    if (!g_clip_on || !g_active) return;
    if (*x < g_clip[0]) *x = g_clip[0];
    if (*x > g_clip[2] - 1) *x = g_clip[2] - 1;
    if (*y < g_clip[1]) *y = g_clip[1];
    if (*y > g_clip[3] - 1) *y = g_clip[3] - 1;
}
/* The event's client point on the screen, clamped to the clip while active;
 * 0 when there is no window to place it in. */
static int screen_point(const struct bw2_in_event *e, int *sx, int *sy) {
    int x = e->x, y = e->y;
    if (!hle_client_to_screen(window(), &x, &y)) return 0;
    clamp(&x, &y);
    *sx = x, *sy = y;
    return 1;
}

static const uint32_t g_button_msg[3][2] = { { WM_LBUTTONUP, WM_LBUTTONDOWN }, { WM_RBUTTONUP, WM_RBUTTONDOWN },
                                             { WM_MBUTTONUP, WM_MBUTTONDOWN } };

/* The kinds that only post: no guest code runs. */
static void handle(const struct bw2_in_event *e) {
    int sx, sy;
    switch (e->kind) {
    case BW2_IN_MOVE:
    case BW2_IN_BUTTON:
    case BW2_IN_WHEEL:
        if (g_sink) g_sink(e);
        if (bw2_in_mouse_exclusive() || !screen_point(e, &sx, &sy)) return;
        g_sx = sx, g_sy = sy;
        if (e->kind == BW2_IN_MOVE) hle_input_mouse_ex(WM_MOUSEMOVE, sx, sy, e->time_ms);
        else if (e->kind == BW2_IN_WHEEL) hle_input_wheel(e->wheel, sx, sy);
        else if (e->button >= 0 && e->button < 3) {
            g_button[e->button] = e->down != 0;
            hle_input_mouse_ex(g_button_msg[e->button][e->down != 0], sx, sy, e->time_ms);
        }
        return;
    case BW2_IN_KEY: {
        uint32_t vk = e->vk & 0xFFu;
        g_key[vk].down = e->down != 0, g_key[vk].scan = e->scan, g_key[vk].extended = e->extended;
        hle_input_key_ex(e->down, vk, e->scan, e->extended);
        return;
    }
    case BW2_IN_CLOSE:
        hle_post_message(window(), WM_CLOSE, 0, 0);
        return;
    case BW2_IN_ACTIVATE:
        break;
    }
    hle_fatal("bw2 input: event kind %d not handled", (int)e->kind);
}

/* An up for every button and key the game was given down; 1 if any. */
static int post_ups(void) {
    int any = 0;
    for (int b = 0; b < 3; b++)
        if (g_button[b]) {
            g_button[b] = 0, any = 1;
            hle_input_mouse_ex(g_button_msg[b][0], g_sx, g_sy, hle_ticks_ms());
        }
    for (int vk = 0; vk < 256; vk++)
        if (g_key[vk].down) {
            g_key[vk].down = 0, any = 1;
            hle_input_key_ex(0, (uint32_t)vk, g_key[vk].scan, g_key[vk].extended);
        }
    return any;
}

static void send_activation(int active) {
    uint32_t h = window();
    if (active) {
        hle_send(h, WM_ACTIVATEAPP, 1, OTHER_THREAD);
        hle_send(h, WM_ACTIVATE, WA_ACTIVE, 0);
        hle_send(h, WM_SETFOCUS, 0, 0);
    } else {
        hle_send(h, WM_ACTIVATE, WA_INACTIVE, 0);
        hle_send(h, WM_KILLFOCUS, 0, 0);
        hle_send(h, WM_ACTIVATEAPP, 0, OTHER_THREAD);
    }
}

static void drain(int partial) {
    if (g_draining) return;              /* a window procedure run by a send, reading or pumping */
    g_draining = 1;
    for (;;) {
        if (!g_held_set) {
            struct bw2_in_event e;
            if (!pop(&e)) break;
            if (e.kind != BW2_IN_ACTIVATE) { handle(&e); continue; }
            g_held = e, g_held_set = 1, g_held_lost = 0, g_held_ups = 0;
        }
        /* the activation first in line */
        if (!g_held.down && !g_held_lost) {
            g_held_lost = 1, g_active = 0, g_losses++;
            bw2_in_mouse_lost();
        }
        if (partial) break;
        if (!g_held.down && !g_held_ups) {
            g_held_ups = 1;
            if (post_ups()) break;       /* the pump hands them out first, then comes back */
        }
        struct bw2_in_event e = g_held;
        if (e.down) g_active = 1;
        send_activation(e.down);
        g_held_set = 0;
        if (g_sink) g_sink(&e);
        bw2_cursor_update();             /* shown and reattached while inactive */
    }
    g_draining = 0;
}
void bw2_in_drain(void) { drain(0); }
void bw2_in_drain_input(void) { drain(1); }
void bw2_in_drain_hook(void) { bw2_in_drain(); }

/* ---- shims ---- */
static void u_GetCursorPos(void) {
    bw2_in_drain_input();
    bw2_hle_shim("GetCursorPos")();
}
static void u_GetAsyncKeyState(void) {
    bw2_in_drain_input();
    bw2_hle_shim("GetAsyncKeyState")();
}
/* The cursor calls: win32hle's answer, then the cursor policy (cursor.c). */
static void u_ShowCursor(void) {
    bw2_hle_shim("ShowCursor")();
    bw2_cursor_update();
}
static void u_SetCursor(void) {
    bw2_cursor_note_set();
    bw2_hle_shim("SetCursor")();
    bw2_cursor_update();
}
/* SetCursorPos: kept inside the clip while active, as Windows keeps it; the pointer follows unless
 * detached. */
static void u_SetCursorPos(void) {
    int x = (int)A32(0), y = (int)A32(1);
    bw2_hle_shim("SetCursorPos")();
    clamp(&x, &y);
    hle_input_cursor(x, y);
    bw2_cursor_note_pos(x, y);
    bw2_cursor_update();
}
/* ClipCursor(rect): the reported points are clamped to it while active (the real pointer is never
 * trapped); NULL, an empty or an inverted rect clears it. TRUE always. */
static void u_ClipCursor(void) {
    uint32_t p = A32(0);
    int r[4];
    if (p) for (int i = 0; i < 4; i++) r[i] = (int)MEM32(p + 4u * (uint32_t)i);
    bw2_hle_shim("ClipCursor")();
    bw2_in_clip(p ? r : NULL);
    bw2_cursor_update();
}

/* imm32.c */
void bw2_ImmCreateContext(void), bw2_ImmDestroyContext(void), bw2_ImmGetOpenStatus(void),
     bw2_ImmGetDescriptionA(void), bw2_ImmGetProperty(void), bw2_ImmSetCompositionStringA(void),
     bw2_ImmSetCompositionWindow(void);

const win32hle_shim bw2_input_shims[] = {
    { "GetCursorPos", u_GetCursorPos },
    { "GetAsyncKeyState", u_GetAsyncKeyState },
    { "ShowCursor", u_ShowCursor },
    { "SetCursor", u_SetCursor },
    { "SetCursorPos", u_SetCursorPos },
    { "ClipCursor", u_ClipCursor },
    { "ImmCreateContext", bw2_ImmCreateContext },
    { "ImmDestroyContext", bw2_ImmDestroyContext },
    { "ImmGetOpenStatus", bw2_ImmGetOpenStatus },
    { "ImmGetDescriptionA", bw2_ImmGetDescriptionA },
    { "ImmGetProperty", bw2_ImmGetProperty },
    { "ImmSetCompositionStringA", bw2_ImmSetCompositionStringA },
    { "ImmSetCompositionWindow", bw2_ImmSetCompositionWindow },
    { 0, 0 } };

void bw2_input_register(void) {
    win32hle_register(bw2_input_shims);
    bw2_dinput8_register();
}
