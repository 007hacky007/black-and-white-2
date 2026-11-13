/*
 * input.h - the host input queue (plan 6). The Mac side pushes events in the
 * game's terms from any thread; the guest thread drains them into win32hle's
 * messages and the DirectInput mouse: fully from the pump hook (inside
 * PeekMessage/GetMessage, where Windows delivers sent messages), and partly
 * (only what posts, stopping at an activation) from the DirectInput reads,
 * GetCursorPos and GetAsyncKeyState.
 */
#ifndef BW2_INPUT_H
#define BW2_INPUT_H

#include <stdint.h>
#include "win32hle.h"

enum bw2_in_kind { BW2_IN_MOVE, BW2_IN_BUTTON, BW2_IN_WHEEL, BW2_IN_KEY, BW2_IN_ACTIVATE, BW2_IN_CLOSE };

struct bw2_in_event {
    enum bw2_in_kind kind;
    uint32_t time_ms;                    /* hle_ticks_ms()'s clock, stamped by bw2_in_push */
    int x, y;                            /* client coordinates in back-buffer pixels */
    int dx, dy;                          /* DirectInput deltas */
    int button;                          /* 0 L, 1 R, 2 M, 3, 4 */
    int down;                            /* also active (1) or inactive (0) for ACTIVATE */
    int wheel;                           /* a multiple of 120 */
    uint32_t vk, scan;
    int extended;
    int repeat;
};

/* The queue: any thread, a leaf mutex, no fixed cap. */
void bw2_in_push(const struct bw2_in_event *e);
void bw2_in_push_at(const struct bw2_in_event *e);   /* keeps e->time_ms (selftests only) */

/* Draining, guest thread only. */
void bw2_in_drain(void);             /* the full drain: only from the pump hook */
void bw2_in_drain_hook(void);        /* win32hle's pump hook */
void bw2_in_drain_input(void);       /* the partial drain: the reads, GetCursorPos, GetAsyncKeyState */
unsigned bw2_in_losses(void);        /* bw2_in_mouse_lost() calls made by a drain so far */

void bw2_in_set_window(uint32_t hwnd);   /* the game window; until set, hle_first_hwnd() */
uint32_t bw2_in_window(void);            /* that window */
int  bw2_in_active(void);
void bw2_in_set_mouse_sink(void (*fn)(const struct bw2_in_event *e));
void bw2_in_clip(const int *rect_or_null);   /* screen {l, t, r, b}; empty or inverted clears */

/* dinput8.c: the DirectInput side of the drain. */
int  bw2_in_mouse_exclusive(void);
void bw2_in_mouse_lost(void);
void bw2_dinput8_sink(const struct bw2_in_event *e);   /* the mouse sink dinput8.c installs */
void bw2_dinput8_register(void);

void bw2_input_register(void);
extern const win32hle_shim bw2_input_shims[];

/* ---- script.c: BW2_INPUT_SCRIPT (plan 6 Task 6), a test and measurement switch ---- */
/* From each DirectInput read, after the quit check and before its drain: pushes every script event
 * whose read number is at most n and not yet pushed. The file is read at the first call; a bad line
 * or a file that cannot be opened stops the run (d3d_fail). Unset, nothing happens. */
void bw2_script_at_read(unsigned long long n);
/* Plan 11 Task 1: binkw32.c's movie notices, last = 0 when _BinkOpen@8 returns a handle and 1 at the
 * copy that shows a movie's last frame. Only records the event and bw2_dinput_reads(); pushes and
 * prints nothing. */
void bw2_script_movie(int last, uint32_t frames);
/* The script's wall clock in seconds: NULL (the start) is CLOCK_MONOTONIC; the selftest sets its own. */
void bw2_script_set_clock(double (*now)(void));
/* dinput8.c: the read counter's current value (GetDeviceData and GetDeviceState reads so far). */
unsigned long long bw2_dinput_reads(void);

/* ---- cursor.c: the cursor policy (plan 6 Task 5) ---- */
struct bw2_cursor_mode {
    int hidden;                          /* the Mac pointer hidden over the game */
    int detached;                        /* the pointer detached from the mouse (the exclusive mouse) */
    uint32_t image;                      /* 0: the system arrow; else a win32hle cursor handle with an image */
    int warp;                            /* a pending warp, in client coordinates (back-buffer pixels) */
    int warp_x, warp_y;
};
void bw2_cursor_policy(struct bw2_cursor_mode *out);   /* pure: the state now, and the last published mode */
void bw2_cursor_update(void);                          /* guest thread: the policy, then publish */
void bw2_cursor_publish(const struct bw2_cursor_mode *m);   /* record; then, with a window, the main thread */
void bw2_cursor_last(struct bw2_cursor_mode *out);     /* the last published mode */
void bw2_cursor_rgba(const hle_cursor_image_t *img, uint8_t out[32 * 32 * 4]);   /* pure */
void bw2_cursor_note_set(void);                        /* the SetCursor shim: the game set a cursor */
void bw2_cursor_note_pos(int screen_x, int screen_y);  /* the SetCursorPos shim: the next update may warp */
/* mac_window.m, main thread: applies a published mode (rgba: the image's pixels, or NULL for the arrow). */
void bw2_mac_cursor_apply(const struct bw2_cursor_mode *m, const uint8_t *rgba, int hot_x, int hot_y);

/* ---- translate.c: Mac events in the game's terms (pure C, any one thread) ---- */
enum { BW2_MAC_DOWN, BW2_MAC_UP, BW2_MAC_MOVE, BW2_MAC_SCROLL, BW2_MAC_KEYDOWN, BW2_MAC_KEYUP, BW2_MAC_FLAGS };

/* NSEventModifierFlags bits (device-independent), as the Mac side passes them. */
#define BW2_MOD_CAPS    (1ull << 16)
#define BW2_MOD_SHIFT   (1ull << 17)
#define BW2_MOD_CONTROL (1ull << 18)
#define BW2_MOD_OPTION  (1ull << 19)
#define BW2_MOD_COMMAND (1ull << 20)

#define BW2_SCROLL_POINTS 10.0           /* precise scroll points per wheel notch */

struct bw2_mac_ev {
    int type;                            /* BW2_MAC_* */
    double x, y;                         /* view points, bottom-left origin (AppKit's) */
    double dx, dy;                       /* deltaX/deltaY in points, down positive */
    int button;                          /* NSEvent.buttonNumber */
    unsigned keycode;                    /* kVK_* */
    uint64_t mods;                       /* NSEventModifierFlags */
    int precise;                         /* hasPreciseScrollingDeltas */
    int momentum;                        /* momentumPhase != 0 */
    int repeat;                          /* isARepeat */
    double scroll_dy;                    /* scrollingDeltaY */
};

struct bw2_geom { double view_w, view_h; uint32_t bb_w, bb_h; };   /* view in points, back buffer in pixels */

/* The carried state. All zero is the start. */
struct bw2_translator {
    double scroll_rem;                   /* precise points not yet a notch */
    double frac_x, frac_y;               /* delta fractions in back-buffer pixels */
    int have_point, cx, cy;              /* the last client point given */
    unsigned buttons;                    /* bit b: button b given down */
    int middle;                          /* the left button is down as the middle one */
    int option_held;                     /* Option's events held back until it is released */
    uint8_t keys[128];                   /* by kVK_* code: given down (Alt as the game sees it included) */
    int caps;                            /* Caps Lock as the game was last told */
};

int  bw2_translate(struct bw2_translator *t, const struct bw2_geom *g, const struct bw2_mac_ev *m,
                   struct bw2_in_event *out, int max);   /* events written, 0..4 */
void bw2_translate_reset(struct bw2_translator *t);     /* on deactivation: forget what was given down */
int  bw2_keymap(unsigned mac_keycode, uint32_t *vk, uint32_t *scan, int *extended);   /* 0: not sent */
void bw2_letterbox(const struct bw2_geom *g, double *x0, double *y0, double *scale);  /* the picture's
                                         top-left from the view's top-left, in points, and points per pixel */
/* A client pixel's centre in view points (bottom-left origin), through the letterbox; points off the
 * picture map off it. The inverse of the translator's client point. */
void bw2_client_to_view(const struct bw2_geom *g, int cx, int cy, double *x, double *y);
/* The first windowed size, in points: the back buffer (a pixel as a point) scaled down, never up, to fit
 * 90% of the screen's visible frame, the aspect kept. No visible frame: the back buffer. */
void bw2_window_fit(double vis_w, double vis_h, uint32_t bb_w, uint32_t bb_h, double *w, double *h);

#endif
