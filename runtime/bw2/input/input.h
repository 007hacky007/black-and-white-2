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
/* The first windowed size, in points: the back buffer (a pixel as a point) scaled down, never up, to fit
 * 90% of the screen's visible frame, the aspect kept. No visible frame: the back buffer. */
void bw2_window_fit(double vis_w, double vis_h, uint32_t bb_w, uint32_t bb_h, double *w, double *h);

#endif
