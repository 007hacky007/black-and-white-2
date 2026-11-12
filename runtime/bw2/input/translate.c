/*
 * translate.c - Mac events in the game's terms (plan 6). Pure C, no AppKit:
 * the Mac side fills a bw2_mac_ev from each NSEvent, and this turns it into
 * the input events the queue takes (input.h): client coordinates in
 * back-buffer pixels through the letterbox, Windows virtual keys and set-1
 * scan codes (US layout), wheel notches of 120 and DirectInput deltas.
 *
 * Rules (the plan's Global Constraints, "Mapping decisions"):
 *   - buttons 0-4 as they come (left, right, middle, 3, 4); a MOVE goes first
 *     when the point or the delta changed; Control+click is a left click;
 *   - deltas are points divided by the letterbox scale, fractions carried;
 *   - a precise scroll gives a notch per BW2_SCROLL_POINTS, the remainder
 *     carried; a line scroll gives a notch per line, at least one; momentum
 *     is ignored; the sign is scrollingDeltaY's;
 *   - Option + left is the middle button: Alt goes up first if the game has
 *     it down, Option's events are held back until it is released, and that
 *     release is dropped; the middle goes up with that left button;
 *   - Command is never sent, nor a key pressed with it; Fn, keypad Clear,
 *     keypad = and F16-F20 are not sent; Caps Lock is a down and an up on
 *     each change of the lock.
 * Something the Mac side should never pass (an unknown event type, too small
 * an output) stops loudly: this runs on the main thread, with no guest
 * address to name.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"

#define VK_CAPITAL 0x14u

static void fail(const char *what, long v) {
    fprintf(stderr, "[bw2] FATAL: input translate: %s %ld\n", what, v);
    fflush(stderr);
    abort();
}

/* ---- the key map: kVK_* code -> VK, set-1 scan code, extended ---- */
struct key { uint8_t vk, scan, ext; };
static const struct key g_map[128] = {
    /* letters */
    [0x00] = { 'A', 0x1E, 0 }, [0x01] = { 'S', 0x1F, 0 }, [0x02] = { 'D', 0x20, 0 }, [0x03] = { 'F', 0x21, 0 },
    [0x04] = { 'H', 0x23, 0 }, [0x05] = { 'G', 0x22, 0 }, [0x06] = { 'Z', 0x2C, 0 }, [0x07] = { 'X', 0x2D, 0 },
    [0x08] = { 'C', 0x2E, 0 }, [0x09] = { 'V', 0x2F, 0 }, [0x0B] = { 'B', 0x30, 0 }, [0x0C] = { 'Q', 0x10, 0 },
    [0x0D] = { 'W', 0x11, 0 }, [0x0E] = { 'E', 0x12, 0 }, [0x0F] = { 'R', 0x13, 0 }, [0x10] = { 'Y', 0x15, 0 },
    [0x11] = { 'T', 0x14, 0 }, [0x1F] = { 'O', 0x18, 0 }, [0x20] = { 'U', 0x16, 0 }, [0x22] = { 'I', 0x17, 0 },
    [0x23] = { 'P', 0x19, 0 }, [0x25] = { 'L', 0x26, 0 }, [0x26] = { 'J', 0x24, 0 }, [0x28] = { 'K', 0x25, 0 },
    [0x2D] = { 'N', 0x31, 0 }, [0x2E] = { 'M', 0x32, 0 },
    /* digits */
    [0x12] = { '1', 0x02, 0 }, [0x13] = { '2', 0x03, 0 }, [0x14] = { '3', 0x04, 0 }, [0x15] = { '4', 0x05, 0 },
    [0x17] = { '5', 0x06, 0 }, [0x16] = { '6', 0x07, 0 }, [0x1A] = { '7', 0x08, 0 }, [0x1C] = { '8', 0x09, 0 },
    [0x19] = { '9', 0x0A, 0 }, [0x1D] = { '0', 0x0B, 0 },
    /* punctuation: - = [ ] ; ' ` \ , . / and the ISO section key */
    [0x1B] = { 0xBD, 0x0C, 0 }, [0x18] = { 0xBB, 0x0D, 0 }, [0x21] = { 0xDB, 0x1A, 0 }, [0x1E] = { 0xDD, 0x1B, 0 },
    [0x29] = { 0xBA, 0x27, 0 }, [0x27] = { 0xDE, 0x28, 0 }, [0x32] = { 0xC0, 0x29, 0 }, [0x2A] = { 0xDC, 0x2B, 0 },
    [0x2B] = { 0xBC, 0x33, 0 }, [0x2F] = { 0xBE, 0x34, 0 }, [0x2C] = { 0xBF, 0x35, 0 }, [0x0A] = { 0xE2, 0x56, 0 },
    /* Return, Tab, Space, Delete (Backspace), Escape */
    [0x24] = { 0x0D, 0x1C, 0 }, [0x30] = { 0x09, 0x0F, 0 }, [0x31] = { 0x20, 0x39, 0 }, [0x33] = { 0x08, 0x0E, 0 },
    [0x35] = { 0x1B, 0x01, 0 },
    /* modifiers */
    [0x38] = { 0xA0, 0x2A, 0 }, [0x3C] = { 0xA1, 0x36, 0 }, [0x3B] = { 0xA2, 0x1D, 0 }, [0x3E] = { 0xA3, 0x1D, 1 },
    [0x3A] = { 0xA4, 0x38, 0 }, [0x3D] = { 0xA5, 0x38, 1 }, [0x39] = { 0x14, 0x3A, 0 },
    /* F1-F15 */
    [0x7A] = { 0x70, 0x3B, 0 }, [0x78] = { 0x71, 0x3C, 0 }, [0x63] = { 0x72, 0x3D, 0 }, [0x76] = { 0x73, 0x3E, 0 },
    [0x60] = { 0x74, 0x3F, 0 }, [0x61] = { 0x75, 0x40, 0 }, [0x62] = { 0x76, 0x41, 0 }, [0x64] = { 0x77, 0x42, 0 },
    [0x65] = { 0x78, 0x43, 0 }, [0x6D] = { 0x79, 0x44, 0 }, [0x67] = { 0x7A, 0x57, 0 }, [0x6F] = { 0x7B, 0x58, 0 },
    [0x69] = { 0x7C, 0x64, 0 }, [0x6B] = { 0x7D, 0x65, 0 }, [0x71] = { 0x7E, 0x66, 0 },
    /* keypad */
    [0x52] = { 0x60, 0x52, 0 }, [0x53] = { 0x61, 0x4F, 0 }, [0x54] = { 0x62, 0x50, 0 }, [0x55] = { 0x63, 0x51, 0 },
    [0x56] = { 0x64, 0x4B, 0 }, [0x57] = { 0x65, 0x4C, 0 }, [0x58] = { 0x66, 0x4D, 0 }, [0x59] = { 0x67, 0x47, 0 },
    [0x5B] = { 0x68, 0x48, 0 }, [0x5C] = { 0x69, 0x49, 0 },
    [0x41] = { 0x6E, 0x53, 0 }, [0x43] = { 0x6A, 0x37, 0 }, [0x45] = { 0x6B, 0x4E, 0 }, [0x4E] = { 0x6D, 0x4A, 0 },
    [0x4B] = { 0x6F, 0x35, 1 }, [0x4C] = { 0x0D, 0x1C, 1 },
    /* navigation; Help is Insert */
    [0x73] = { 0x24, 0x47, 1 }, [0x77] = { 0x23, 0x4F, 1 }, [0x74] = { 0x21, 0x49, 1 }, [0x79] = { 0x22, 0x51, 1 },
    [0x75] = { 0x2E, 0x53, 1 }, [0x72] = { 0x2D, 0x52, 1 },
    [0x7B] = { 0x25, 0x4B, 1 }, [0x7C] = { 0x27, 0x4D, 1 }, [0x7D] = { 0x28, 0x50, 1 }, [0x7E] = { 0x26, 0x48, 1 },
};

int bw2_keymap(unsigned mac, uint32_t *vk, uint32_t *scan, int *extended) {
    if (mac >= 128 || !g_map[mac].vk) return 0;
    *vk = g_map[mac].vk, *scan = g_map[mac].scan, *extended = g_map[mac].ext;
    return 1;
}

/* ---- modifier families: the left and right key, the NSEvent bit, the device bits ---- */
struct family { unsigned left, right; uint64_t bit, dev_left, dev_right; };
static const struct family g_families[3] = {
    { 0x38, 0x3C, BW2_MOD_SHIFT, 0x02, 0x04 },            /* NX_DEVICELSHIFTKEYMASK, ...RSHIFT... */
    { 0x3B, 0x3E, BW2_MOD_CONTROL, 0x01, 0x2000 },        /* NX_DEVICELCTLKEYMASK, ...RCTL... */
    { 0x3A, 0x3D, BW2_MOD_OPTION, 0x20, 0x40 },           /* NX_DEVICELALTKEYMASK, ...RALT... */
};
static const struct family *family_of(unsigned k) {
    for (int i = 0; i < 3; i++)
        if (g_families[i].left == k || g_families[i].right == k) return &g_families[i];
    return NULL;
}

/* ---- geometry ---- */
static int geom_ok(const struct bw2_geom *g) {
    return g->view_w > 0 && g->view_h > 0 && g->bb_w && g->bb_h && isfinite(g->view_w) && isfinite(g->view_h);
}
/* Which side fills the view: 1 if the width (bars above and below, or none). */
static int width_bound(const struct bw2_geom *g) { return g->view_w * g->bb_h <= g->view_h * g->bb_w; }

void bw2_letterbox(const struct bw2_geom *g, double *x0, double *y0, double *scale) {
    if (!geom_ok(g)) { *x0 = 0, *y0 = 0, *scale = 0; return; }
    double s = width_bound(g) ? g->view_w / g->bb_w : g->view_h / g->bb_h;
    *scale = s;
    *x0 = (g->view_w - g->bb_w * s) / 2;
    *y0 = (g->view_h - g->bb_h * s) / 2;
}

/* View points to back-buffer pixels, with one rounding: d * bb / view on the filling side. */
static double to_pixels(const struct bw2_geom *g, double d) {
    return width_bound(g) ? d * g->bb_w / g->view_w : d * g->bb_h / g->view_h;
}
static int clamp_floor(double v, uint32_t n) {
    if (!(v >= 0)) return 0;                             /* NaN included */
    if (v >= (double)n) return (int)n - 1;
    int i = (int)floor(v);
    return i > (int)n - 1 ? (int)n - 1 : i;
}
static void client(const struct bw2_geom *g, double x, double y, int *cx, int *cy) {
    double x0, y0, s;
    bw2_letterbox(g, &x0, &y0, &s);
    *cx = clamp_floor(to_pixels(g, x - x0), g->bb_w);
    *cy = clamp_floor(to_pixels(g, g->view_h - y - y0), g->bb_h);
}

/* ---- output ---- */
struct outq { struct bw2_in_event ev[4]; int n; };
static struct bw2_in_event *add(struct outq *o, enum bw2_in_kind kind) {
    if (o->n == 4) fail("more than 4 events from one Mac event, kind", kind);
    struct bw2_in_event *e = &o->ev[o->n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    return e;
}
static void key(struct bw2_translator *t, struct outq *o, unsigned mac, int down, int repeat) {
    struct bw2_in_event *e = add(o, BW2_IN_KEY);
    e->down = down;
    e->vk = g_map[mac].vk, e->scan = g_map[mac].scan, e->extended = g_map[mac].ext;
    e->repeat = repeat;
    t->keys[mac] = (uint8_t)(down != 0);
}

/* The MOVE the mouse events share; first=1 for a DOWN or UP, which sends it
 * only if the point or the delta changed. */
static void move(struct bw2_translator *t, const struct bw2_geom *g, const struct bw2_mac_ev *m, struct outq *o,
                 int first, int *cx, int *cy) {
    client(g, m->x, m->y, cx, cy);
    t->frac_x += to_pixels(g, m->dx), t->frac_y += to_pixels(g, m->dy);
    double ix = trunc(t->frac_x), iy = trunc(t->frac_y);
    t->frac_x -= ix, t->frac_y -= iy;
    int moved = !t->have_point || *cx != t->cx || *cy != t->cy || ix != 0 || iy != 0;
    if (first && !moved) return;
    struct bw2_in_event *e = add(o, BW2_IN_MOVE);
    e->x = *cx, e->y = *cy, e->dx = (int)ix, e->dy = (int)iy;
    t->have_point = 1, t->cx = *cx, t->cy = *cy;
}

static void button(struct bw2_translator *t, const struct bw2_geom *g, const struct bw2_mac_ev *m, struct outq *o) {
    int cx, cy, down = m->type == BW2_MAC_DOWN, b = m->button;
    move(t, g, m, o, 1, &cx, &cy);
    if (b < 0 || b > 4) return;
    if (b == 0 && down && (m->mods & BW2_MOD_OPTION)) {
        /* Option + left: the middle. Alt up first if the game has it down. */
        const struct family *f = &g_families[2];
        if (t->keys[f->left]) key(t, o, f->left, 0, 0);
        if (t->keys[f->right]) key(t, o, f->right, 0, 0);
        t->option_held = 1, t->middle = 1;
        b = 2;
    } else if (b == 0 && !down && t->middle) {
        t->middle = 0;
        b = 2;
    } else if (!down && !(t->buttons & 1u << b)) {
        return;                                          /* never given down (or forgotten on deactivation) */
    }
    if (down) t->buttons |= 1u << b;
    else t->buttons &= ~(1u << b);
    struct bw2_in_event *e = add(o, BW2_IN_BUTTON);
    e->button = b, e->down = down, e->x = cx, e->y = cy;
}

static void scroll(struct bw2_translator *t, const struct bw2_geom *g, const struct bw2_mac_ev *m, struct outq *o) {
    if (m->momentum || !isfinite(m->scroll_dy)) return;
    double notches;
    if (m->precise) {
        t->scroll_rem += m->scroll_dy;
        notches = trunc(t->scroll_rem / BW2_SCROLL_POINTS);
        t->scroll_rem -= notches * BW2_SCROLL_POINTS;
    } else {
        /* a wheel event is a detent: at least one notch */
        notches = round(m->scroll_dy);
        if (notches == 0 && m->scroll_dy != 0) notches = m->scroll_dy > 0 ? 1 : -1;
    }
    if (notches == 0) return;
    if (fabs(notches) > 1000) notches = notches > 0 ? 1000 : -1000;
    struct bw2_in_event *e = add(o, BW2_IN_WHEEL);
    client(g, m->x, m->y, &e->x, &e->y);
    e->wheel = (int)notches * 120;
}

static void flags(struct bw2_translator *t, const struct bw2_mac_ev *m, struct outq *o) {
    int caps = (m->mods & BW2_MOD_CAPS) != 0;
    if (caps != t->caps) {
        /* the lock changed: a press of Caps Lock */
        t->caps = caps;
        struct bw2_in_event *e = add(o, BW2_IN_KEY);
        e->down = 1, e->vk = VK_CAPITAL, e->scan = g_map[0x39].scan;
        e = add(o, BW2_IN_KEY);
        e->down = 0, e->vk = VK_CAPITAL, e->scan = g_map[0x39].scan;
    }
    const struct family *f = family_of(m->keycode);
    if (!f) return;                                      /* Command, Fn, Caps Lock, or no key */
    int set = (m->mods & f->bit) != 0;
    if (f->bit == BW2_MOD_OPTION && t->option_held) {
        if (!set) t->option_held = 0;                    /* the release is dropped */
        return;
    }
    uint64_t dev = m->mods & (f->dev_left | f->dev_right);
    const unsigned side[2] = { f->left, f->right };
    const uint64_t side_dev[2] = { f->dev_left, f->dev_right };
    for (int i = 0; i < 2; i++) {
        unsigned k = side[i];
        int want;
        if (!set) want = 0;                              /* the family is up: both sides */
        else if (dev) want = (m->mods & side_dev[i]) != 0;
        else want = k == m->keycode ? 1 : t->keys[k];   /* no device bits: only the key that changed */
        if (want != t->keys[k]) key(t, o, k, want, 0);
    }
}

int bw2_translate(struct bw2_translator *t, const struct bw2_geom *g, const struct bw2_mac_ev *m,
                  struct bw2_in_event *out, int max) {
    struct outq o = { .n = 0 };
    int cx, cy;
    switch (m->type) {
    case BW2_MAC_DOWN:
    case BW2_MAC_UP:
        if (geom_ok(g)) button(t, g, m, &o);
        break;
    case BW2_MAC_MOVE:
        if (geom_ok(g)) move(t, g, m, &o, 0, &cx, &cy);
        break;
    case BW2_MAC_SCROLL:
        if (geom_ok(g)) scroll(t, g, m, &o);
        break;
    case BW2_MAC_KEYDOWN:
        if (m->keycode < 128 && g_map[m->keycode].vk && !(m->mods & BW2_MOD_COMMAND))
            key(t, &o, m->keycode, 1, m->repeat && t->keys[m->keycode]);
        break;
    case BW2_MAC_KEYUP:
        if (m->keycode < 128 && t->keys[m->keycode]) key(t, &o, m->keycode, 0, 0);
        break;
    case BW2_MAC_FLAGS:
        flags(t, m, &o);
        break;
    default:
        fail("unknown Mac event type", m->type);
    }
    if (o.n > max) fail("output too small for the events, max", max);
    memcpy(out, o.ev, (size_t)o.n * sizeof *out);
    return o.n;
}

void bw2_translate_reset(struct bw2_translator *t) {
    memset(t->keys, 0, sizeof t->keys);
    t->buttons = 0, t->middle = 0, t->option_held = 0;
    t->frac_x = 0, t->frac_y = 0, t->scroll_rem = 0;
}
