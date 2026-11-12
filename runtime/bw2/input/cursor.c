/*
 * cursor.c - the cursor policy (plan 6 Task 5): whether the Mac pointer is
 * hidden over the game, detached from the mouse, which image it shows and
 * where it is warped to, computed from the game's cursor calls, the window's
 * activation and the exclusive DirectInput mouse. The Mac side
 * (mac_window.m, bw2_mac_cursor_apply) only applies it.
 *
 * Rules:
 *   - detached = active and an exclusive DirectInput mouse is acquired;
 *   - hidden = inactive ? 0 : (ShowCursor's count < 0, or SetCursor(NULL),
 *     or detached);
 *   - image: the cursor the game set, when win32hle holds its image; else
 *     0, the system arrow (also before any SetCursor);
 *   - every detach (detached now, not in the last published mode) carries a
 *     warp to the centre of the game window's client rect, so the frozen
 *     pointer sits over the picture; SetCursorPos while active and not
 *     detached carries a warp to its client point. A mode with no change
 *     carries no warp.
 * Guest thread only: it reads win32hle state.
 */
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"
#include "bw2.h"

static struct bw2_cursor_mode g_last;    /* the last published mode */
static int g_set;                        /* the game called SetCursor */
static int g_pos_pending, g_pos_x, g_pos_y;   /* a SetCursorPos since the last update, on the screen */

void bw2_cursor_note_set(void) { g_set = 1; }
void bw2_cursor_note_pos(int sx, int sy) { g_pos_pending = 1, g_pos_x = sx, g_pos_y = sy; }

void bw2_cursor_policy(struct bw2_cursor_mode *out) {
    struct bw2_cursor_mode m;
    memset(&m, 0, sizeof m);
    int active = bw2_in_active();
    uint32_t cur = hle_current_cursor();
    m.detached = active && bw2_in_mouse_exclusive();
    m.hidden = active && (!hle_cursor_shown() || (g_set && !cur) || m.detached);
    m.image = cur && hle_cursor_image(cur) ? cur : 0;
    int l, t, r, b;                      /* the client rect on the screen: win32hle windows have no frame */
    int have_rect = hle_window_screen_rect(bw2_in_window(), &l, &t, &r, &b);
    if (m.detached && !g_last.detached) {
        if (!have_rect) hle_fatal("bw2 cursor: the exclusive mouse detached with no game window (0x%X)", bw2_in_window());
        m.warp = 1, m.warp_x = (r - l) / 2, m.warp_y = (b - t) / 2;
    } else if (active && !m.detached && g_pos_pending && have_rect) {
        m.warp = 1, m.warp_x = g_pos_x - l, m.warp_y = g_pos_y - t;
    }
    *out = m;
}

void bw2_cursor_last(struct bw2_cursor_mode *out) { *out = g_last; }

void bw2_cursor_rgba(const hle_cursor_image_t *img, uint8_t out[32 * 32 * 4]) {
    for (int r = 0; r < 32; r++)
        for (int x = 0; x < 32; x++) {
            int bit = 0x80 >> (x % 8), i = r * 4 + x / 8;
            int and_bit = (img->and_mask[i] & bit) != 0, xor_bit = (img->xor_mask[i] & bit) != 0;
            uint8_t *p = out + (r * 32 + x) * 4;
            /* AND 1 XOR 0: transparent; AND 0: black or white by XOR; AND 1 XOR 1 (inverted): black */
            uint8_t v = !and_bit && xor_bit ? 255 : 0, a = and_bit && !xor_bit ? 0 : 255;
            p[0] = p[1] = p[2] = v, p[3] = a;
        }
}

void bw2_cursor_publish(const struct bw2_cursor_mode *m) {
    int same = !m->warp && m->hidden == g_last.hidden && m->detached == g_last.detached && m->image == g_last.image;
    g_last = *m;
    g_pos_pending = 0;
    if (bw2_headless() || same) return;
    struct bw2_cursor_mode copy = *m;
    const hle_cursor_image_t *img = m->image ? hle_cursor_image(m->image) : NULL;
    uint8_t *rgba = NULL;
    int hx = 0, hy = 0;
    if (img) {
        rgba = malloc(32 * 32 * 4);
        if (!rgba) abort();
        bw2_cursor_rgba(img, rgba);
        hx = img->hot_x, hy = img->hot_y;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        bw2_mac_cursor_apply(&copy, rgba, hx, hy);
        free(rgba);
    });
}

void bw2_cursor_update(void) {
    struct bw2_cursor_mode m;
    bw2_cursor_policy(&m);
    bw2_cursor_publish(&m);
}
