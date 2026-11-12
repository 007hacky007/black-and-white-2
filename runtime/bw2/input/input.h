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

#endif
