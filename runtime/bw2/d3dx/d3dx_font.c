/*
 * d3dx_font.c - ID3DXFont and D3DXCreateFontA (plan 7 Task 4), after the
 * ID3DXFont and DrawText documentation. The face comes from font.h (memory
 * fonts first, then the system's) and is opened once, at D3DXCreateFontA,
 * as D3DX realizes its HFONT at create time; a memory font added or removed
 * later does not change a font already made.
 *
 * Glyphs are cached in 256x256 A8R8G8B8 D3DPOOL_MANAGED textures, one level,
 * each glyph as (255, 255, 255, coverage), packed in rows (shelves) with a
 * one-pixel empty border so linear filtering reads no neighbour. Every
 * device, texture and sprite call goes through the object's guest vtable
 * (d3d_call). DrawText lays text out by GDI's rules (tm height per line, the
 * pen moved by the face's advances) and draws each glyph as one sprite Draw:
 * into the caller's sprite, or into the font's own between
 * Begin(ALPHABLEND | SORT_TEXTURE) and End.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d_com.h"
#include "bw2.h"
#include "ui.h"
#include "font.h"

#define CACHE_SIZE 256u
#define D3DFMT_A8R8G8B8 21u
#define D3DPOOL_MANAGED 1u

#define DT_CENTER 0x1u
#define DT_RIGHT 0x2u
#define DT_VCENTER 0x4u
#define DT_BOTTOM 0x8u
#define DT_WORDBREAK 0x10u
#define DT_SINGLELINE 0x20u
#define DT_EXPANDTABS 0x40u
#define DT_NOCLIP 0x100u
#define DT_CALCRECT 0x400u
#define DT_KNOWN (DT_CENTER | DT_RIGHT | DT_VCENTER | DT_BOTTOM | DT_WORDBREAK | DT_SINGLELINE | DT_EXPANDTABS | \
                  DT_NOCLIP | DT_CALCRECT)

/* the scratch block: arguments the font passes to guest methods */
#define SC_RECT 0u                            /* RECT */
#define SC_VEC 16u                            /* D3DXVECTOR3 */
#define SC_LOCKED 32u                         /* D3DLOCKED_RECT */
#define SC_OUT 40u                            /* an out pointer */
#define SC_SIZE 48u

struct glyph {
    uint8_t state;                            /* 0 unknown, 1 black box known, 2 in the cache */
    int32_t left, top;                        /* the black box from the pen on the baseline */
    uint32_t w, h;
    uint8_t *alpha;                           /* until it is in the cache */
    uint32_t tex, cx, cy;                     /* its cell: texture index and top-left */
};
struct font {
    struct d3d_obj base;
    uint32_t device, sprite, scratch;
    struct bw2_font *f;
    int32_t height;
    uint32_t width, weight, mips, italic;
    uint8_t charset, out_precision, quality, pitch_family;
    char face[32];
    struct glyph *g;
    uint32_t ng;
    uint32_t *tex, ntex, captex;
    uint32_t sx, sy, row;                     /* the shelf cursor in the newest texture */
};

static struct font *font_of(uint32_t o) { return d3d_host(o, D3DK_FONT); }

static uint32_t gcall(uint32_t obj, const char *iface, const char *m, uint32_t n, const uint32_t *a) {
    return d3d_call(obj, iface, m, n, a);
}
static void release(uint32_t obj, const char *iface) { gcall(obj, iface, "Release", 0, NULL); }

/* ---- the glyph cache ---- */

static struct glyph *glyph_box(struct font *ft, uint32_t g) {
    if (g >= ft->ng) d3d_fail("ID3DXFont: glyph %u of \"%s\", which has %u", g, ft->face, ft->ng);
    struct glyph *e = &ft->g[g];
    if (!e->state) {
        struct bw2_glyph b;
        bw2_font_raster(ft->f, g, &b);
        if (b.w > CACHE_SIZE - 2 || b.h > CACHE_SIZE - 2)
            d3d_fail("ID3DXFont: glyph %u of \"%s\" %d is %ux%u pixels, larger than a cache cell (254x254)", g,
                     ft->face, ft->height, b.w, b.h);
        e->left = b.left, e->top = b.top, e->w = b.w, e->h = b.h, e->alpha = b.alpha;
        e->state = 1;
    }
    return e;
}

static void new_texture(struct font *ft) {
    uint32_t a[8] = { CACHE_SIZE, CACHE_SIZE, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, ft->scratch + SC_OUT, 0 };
    uint32_t hr = gcall(ft->device, "IDirect3DDevice9", "CreateTexture", 8, a);
    if (hr) d3d_fail("ID3DXFont: CreateTexture of a 256x256 glyph cache gave 0x%08X", hr);
    uint32_t t = MEM32(ft->scratch + SC_OUT);
    uint32_t l[4] = { 0, ft->scratch + SC_LOCKED, 0, 0 };
    hr = gcall(t, "IDirect3DTexture9", "LockRect", 4, l);
    if (hr) d3d_fail("ID3DXFont: LockRect of a glyph cache gave 0x%08X", hr);
    uint32_t pitch = MEM32(ft->scratch + SC_LOCKED), bits = MEM32(ft->scratch + SC_LOCKED + 4);
    for (uint32_t y = 0; y < CACHE_SIZE; y++)
        for (uint32_t x = 0; x < CACHE_SIZE; x++) MEM32(bits + y * pitch + 4 * x) = 0x00FFFFFFu;  /* white, clear */
    uint32_t u[1] = { 0 };
    hr = gcall(t, "IDirect3DTexture9", "UnlockRect", 1, u);
    if (hr) d3d_fail("ID3DXFont: UnlockRect of a glyph cache gave 0x%08X", hr);
    if (ft->ntex == ft->captex) {
        ft->captex = ft->captex ? 2 * ft->captex : 4;
        ft->tex = realloc(ft->tex, sizeof *ft->tex * ft->captex);
        if (!ft->tex) d3d_fail("ID3DXFont: no memory for %u cache textures", ft->captex);
    }
    ft->tex[ft->ntex++] = t;
    ft->sx = 1, ft->sy = 1, ft->row = 0;
}

/* The glyph's black box, and its cell in a cache texture (none for a blank glyph). */
static struct glyph *glyph_cached(struct font *ft, uint32_t g) {
    struct glyph *e = glyph_box(ft, g);
    if (e->state == 2 || !e->w || !e->h) return e;
    if (ft->ntex && ft->sx + e->w > CACHE_SIZE - 1) ft->sy += ft->row + 1, ft->sx = 1, ft->row = 0;  /* next shelf */
    if (!ft->ntex || ft->sy + e->h > CACHE_SIZE - 1) new_texture(ft);
    e->tex = ft->ntex - 1, e->cx = ft->sx, e->cy = ft->sy;
    ft->sx += e->w + 1;
    if (e->h > ft->row) ft->row = e->h;
    uint32_t t = ft->tex[e->tex], rc = ft->scratch + SC_RECT;
    MEM32(rc) = e->cx, MEM32(rc + 4) = e->cy, MEM32(rc + 8) = e->cx + e->w, MEM32(rc + 12) = e->cy + e->h;
    uint32_t l[4] = { 0, ft->scratch + SC_LOCKED, rc, 0 };
    uint32_t hr = gcall(t, "IDirect3DTexture9", "LockRect", 4, l);
    if (hr) d3d_fail("ID3DXFont: LockRect of a glyph cell gave 0x%08X", hr);
    uint32_t pitch = MEM32(ft->scratch + SC_LOCKED), bits = MEM32(ft->scratch + SC_LOCKED + 4);
    for (uint32_t y = 0; y < e->h; y++)
        for (uint32_t x = 0; x < e->w; x++)
            MEM32(bits + y * pitch + 4 * x) = (uint32_t)e->alpha[y * e->w + x] << 24 | 0x00FFFFFFu;
    uint32_t u[1] = { 0 };
    hr = gcall(t, "IDirect3DTexture9", "UnlockRect", 1, u);
    if (hr) d3d_fail("ID3DXFont: UnlockRect of a glyph cell gave 0x%08X", hr);
    free(e->alpha);
    e->alpha = NULL, e->state = 2;
    return e;
}

/* ---- text ---- */

/* The code points of a DrawText/PreloadText string: count -1 is NUL-terminated. A bytes map through
 * Windows-1252, W is UTF-16 (a lone surrogate stays itself, so it draws glyph 0). malloc'd. */
static uint32_t *decode(const char *method, uint32_t str, int32_t count, int wide, uint32_t *n) {
    if (count < -1) d3d_fail("ID3DXFont::%s: count %d is not handled (only -1 or a length)", method, count);
    if (!str) d3d_fail("ID3DXFont::%s: no text (string NULL, count %d)", method, count);
    uint32_t len = 0;
    if (count >= 0) len = (uint32_t)count;
    else if (wide) while (MEM16(str + 2 * len)) len++;
    else while (MEM8(str + len)) len++;
    uint32_t *cp = malloc(sizeof *cp * (len ? len : 1)), k = 0;
    if (!cp) d3d_fail("ID3DXFont::%s: no memory for %u characters", method, len);
    for (uint32_t i = 0; i < len; i++) {
        if (!wide) { cp[k++] = bw2_cp1252(MEM8(str + i)); continue; }
        uint32_t c = MEM16(str + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len) {
            uint32_t d = MEM16(str + 2 * i + 2);
            if (d >= 0xDC00 && d < 0xE000) c = 0x10000u + ((c - 0xD800) << 10) + (d - 0xDC00), i++;
        }
        cp[k++] = c;
    }
    *n = k;
    return cp;
}

static struct d3dx_glyph_pos *g_layout;
static uint32_t g_nlayout, g_caplayout;
uint32_t d3dx_font_last_layout(struct d3dx_glyph_pos *out, uint32_t max) {
    for (uint32_t i = 0; i < g_nlayout && i < max; i++) out[i] = g_layout[i];
    return g_nlayout;
}
static void place(uint32_t glyph, int32_t x, int32_t y) {
    if (g_nlayout == g_caplayout) {
        g_caplayout = g_caplayout ? 2 * g_caplayout : 256;
        g_layout = realloc(g_layout, sizeof *g_layout * g_caplayout);
        if (!g_layout) d3d_fail("ID3DXFont: no memory for %u placed glyphs", g_caplayout);
    }
    g_layout[g_nlayout++] = (struct d3dx_glyph_pos){ glyph, x, y };
}

struct item { uint32_t glyph; int32_t pen; };        /* pen: from the line's start */
struct line { uint32_t first, n; int32_t width; };

static const struct { uint32_t bit; const char *name; } g_dt_names[] = {
    { 0x80u, "DT_TABSTOP" }, { 0x200u, "DT_EXTERNALLEADING" }, { 0x800u, "DT_NOPREFIX" }, { 0x1000u, "DT_INTERNAL" },
    { 0x2000u, "DT_EDITCONTROL" }, { 0x4000u, "DT_PATH_ELLIPSIS" }, { 0x8000u, "DT_END_ELLIPSIS" },
    { 0x10000u, "DT_MODIFYSTRING" }, { 0x20000u, "DT_RTLREADING" }, { 0x40000u, "DT_WORD_ELLIPSIS" },
    { 0x80000u, "DT_NOFULLWIDTHCHARBREAK" }, { 0x100000u, "DT_HIDEPREFIX" }, { 0x200000u, "DT_PREFIXONLY" },
};
static void check_format(const char *method, uint32_t fmt) {
    uint32_t bad = fmt & ~DT_KNOWN;
    if (bad) {
        uint32_t bit = bad & -bad;
        for (size_t i = 0; i < sizeof g_dt_names / sizeof *g_dt_names; i++)
            if (g_dt_names[i].bit == bit)
                d3d_fail("ID3DXFont::%s: format 0x%X has %s (0x%X), which is not handled", method, fmt,
                         g_dt_names[i].name, bit);
        d3d_fail("ID3DXFont::%s: format 0x%X has bit 0x%X, which is not handled", method, fmt, bit);
    }
    if ((fmt & DT_CENTER) && (fmt & DT_RIGHT))
        d3d_fail("ID3DXFont::%s: format 0x%X has DT_CENTER and DT_RIGHT together", method, fmt);
    if ((fmt & DT_VCENTER) && (fmt & DT_BOTTOM))
        d3d_fail("ID3DXFont::%s: format 0x%X has DT_VCENTER and DT_BOTTOM together", method, fmt);
}
static int32_t floor_half(int32_t v) { return v >= 0 ? v / 2 : -((1 - v) / 2); }

/* Break cp[0..n) into lines (font.h metrics, GDI's DrawText rules); items and lines are malloc'd. */
static uint32_t break_lines(struct font *ft, const uint32_t *cp, uint32_t n, uint32_t fmt, int32_t width,
                            struct item **pitems, struct line **plines) {
    const struct bw2_tm *tm = bw2_font_metrics(ft->f);
    int single = (fmt & DT_SINGLELINE) != 0, wrap = (fmt & DT_WORDBREAK) && !single;
    int expand = (fmt & DT_EXPANDTABS) != 0;
    int32_t tab = 8 * tm->ave_char_width;
    if (expand && tab <= 0) d3d_fail("ID3DXFont: \"%s\" has ave_char_width %d, no tab stops", ft->face, tm->ave_char_width);
    struct item *it = malloc(sizeof *it * (n ? n : 1));
    struct line *ln = malloc(sizeof *ln * (n ? n : 1));
    if (!it || !ln) d3d_fail("ID3DXFont: no memory to lay out %u characters", n);
    uint32_t ni = 0, nl = 0, i = 0;
    while (i < n) {
        struct line *l = &ln[nl++];
        l->first = ni;
        int32_t pen = 0, brk_width = 0;
        int64_t brk_item = -1;                       /* where the last space run after a word starts */
        uint32_t brk_next = 0;                       /* the character after that run */
        int word = 0, in_spaces = 0;
        while (i < n) {
            uint32_t c = cp[i];
            if (c == '\r' || c == '\n') {
                i += c == '\r' && i + 1 < n && cp[i + 1] == '\n' ? 2 : 1;
                if (single) continue;                /* no width, nothing drawn */
                break;
            }
            if (c == '\t' && expand) {
                pen = (pen / tab + 1) * tab;
                word = 1, in_spaces = 0, i++;
                continue;
            }
            uint32_t g = bw2_font_glyph(ft->f, c);
            int32_t adv = bw2_font_advance(ft->f, g);
            if (c == ' ') {
                if (word && !in_spaces) brk_item = ni, brk_width = pen;
                if (word) brk_next = i + 1;
                in_spaces = 1;
            } else {
                if (wrap && pen + adv > width && brk_item >= 0) {   /* break at the last space run */
                    ni = (uint32_t)brk_item, pen = brk_width, i = brk_next;
                    break;
                }
                word = 1, in_spaces = 0;
            }
            it[ni++] = (struct item){ g, pen };
            pen += adv;
            i++;
        }
        l->n = ni - l->first, l->width = pen;
    }
    *pitems = it, *plines = ln;
    return nl;
}

/* DrawTextA and DrawTextW: (this, ID3DXSprite*, string, count, RECT*, format, D3DCOLOR) -> INT */
static void draw_text(int wide) {
    const char *method = wide ? "DrawTextW" : "DrawTextA";
    struct font *ft = font_of(A32(0));
    uint32_t sprite = A32(1), str = A32(2), rcp = A32(4), fmt = A32(5), color = A32(6);
    int32_t count = (int32_t)A32(3);
    check_format(method, fmt);
    g_nlayout = 0;
    if (count == 0) RET(0, 7);
    uint32_t n, *cp = decode(method, str, count, wide, &n);
    if (!n) { free(cp); RET(0, 7); }
    int32_t l = 0, t = 0, r = 0, b = 0;
    if (rcp) l = (int32_t)MEM32(rcp), t = (int32_t)MEM32(rcp + 4), r = (int32_t)MEM32(rcp + 8), b = (int32_t)MEM32(rcp + 12);
    else fmt |= DT_NOCLIP;
    const struct bw2_tm *tm = bw2_font_metrics(ft->f);
    struct item *it;
    struct line *ln;
    uint32_t nl = break_lines(ft, cp, n, fmt, r - l, &it, &ln);
    free(cp);
    int32_t lh = tm->height, text_h = (int32_t)nl * lh, top = t, ret = text_h;
    int calc = (fmt & DT_CALCRECT) != 0;
    if (!calc && (fmt & DT_SINGLELINE) && (fmt & (DT_VCENTER | DT_BOTTOM))) {
        top = fmt & DT_VCENTER ? t + floor_half((b - t) - lh) : b - lh;
        ret = top - t + text_h;
    }
    int32_t widest = 0;
    for (uint32_t k = 0; k < nl; k++) {
        int32_t x0 = l;
        if (fmt & DT_CENTER) x0 = l + floor_half((r - l) - ln[k].width);
        if (fmt & DT_RIGHT) x0 = r - ln[k].width;
        if (ln[k].width > widest) widest = ln[k].width;
        int32_t base = top + (int32_t)k * lh + tm->ascent;
        for (uint32_t j = ln[k].first; j < ln[k].first + ln[k].n; j++) {
            struct glyph *e = glyph_box(ft, it[j].glyph);
            place(it[j].glyph, x0 + it[j].pen + e->left, base - e->top);
        }
    }
    free(it), free(ln);
    if (calc) {
        if (rcp) MEM32(rcp + 8) = (uint32_t)(l + widest), MEM32(rcp + 12) = (uint32_t)(t + text_h);
        RET((uint32_t)ret, 7);
    }
    /* draw: one sprite Draw per glyph, cut to the rect unless DT_NOCLIP */
    uint32_t sp = sprite ? sprite : ft->sprite, hr;
    if (!sprite) {
        uint32_t a[1] = { D3DXSPRITE_ALPHABLEND | D3DXSPRITE_SORT_TEXTURE };
        hr = gcall(sp, "ID3DXSprite", "Begin", 1, a);
        if (hr) d3d_fail("ID3DXFont::%s: its sprite's Begin gave 0x%08X", method, hr);
    }
    int clip = !(fmt & DT_NOCLIP);
    for (uint32_t k = 0; k < g_nlayout; k++) {
        struct glyph *e = glyph_cached(ft, g_layout[k].glyph);
        if (!e->w || !e->h) continue;
        int32_t x = g_layout[k].x, y = g_layout[k].y, w = (int32_t)e->w, h = (int32_t)e->h;
        int32_t sx = (int32_t)e->cx, sy = (int32_t)e->cy;
        if (clip) {
            if (x < l) sx += l - x, w -= l - x, x = l;
            if (y < t) sy += t - y, h -= t - y, y = t;
            if (x + w > r) w = r - x;
            if (y + h > b) h = b - y;
            if (w <= 0 || h <= 0) continue;
        }
        uint32_t rc = ft->scratch + SC_RECT, v = ft->scratch + SC_VEC;
        MEM32(rc) = (uint32_t)sx, MEM32(rc + 4) = (uint32_t)sy, MEM32(rc + 8) = (uint32_t)(sx + w),
        MEM32(rc + 12) = (uint32_t)(sy + h);
        float pos[3] = { (float)x, (float)y, 0.0f };
        memcpy(hle_ptr(v), pos, 12);
        uint32_t a[5] = { ft->tex[e->tex], rc, 0, v, color };
        hr = gcall(sp, "ID3DXSprite", "Draw", 5, a);
        if (hr) d3d_fail("ID3DXFont::%s: ID3DXSprite::Draw gave 0x%08X", method, hr);
    }
    if (!sprite) {
        hr = gcall(sp, "ID3DXSprite", "End", 0, NULL);
        if (hr) d3d_fail("ID3DXFont::%s: its sprite's End gave 0x%08X", method, hr);
    }
    RET((uint32_t)ret, 7);
}
static void ft_DrawTextA(void) { draw_text(0); }
static void ft_DrawTextW(void) { draw_text(1); }

/* ---- the other methods ---- */

static void ft_GetDevice(void) {                 /* (this, IDirect3DDevice9**) */
    struct font *ft = font_of(A32(0));
    uint32_t out = A32(1);
    if (!out) RET(D3DERR_INVALIDCALL, 2);
    gcall(ft->device, "IDirect3DDevice9", "AddRef", 0, NULL);
    MEM32(out) = ft->device;
    RET(D3D_OK, 2);
}
/* D3DXFONT_DESCA (56 bytes) and D3DXFONT_DESCW (88): five 32-bit fields, four bytes, the face */
static void get_desc(int wide) {
    struct font *ft = font_of(A32(0));
    uint32_t d = A32(1);
    if (!d) RET(D3DERR_INVALIDCALL, 2);
    MEM32(d) = (uint32_t)ft->height, MEM32(d + 4) = ft->width, MEM32(d + 8) = ft->weight;
    MEM32(d + 12) = ft->mips, MEM32(d + 16) = ft->italic;
    MEM8(d + 20) = ft->charset, MEM8(d + 21) = ft->out_precision, MEM8(d + 22) = ft->quality;
    MEM8(d + 23) = ft->pitch_family;
    for (uint32_t i = 0; i < 32; i++) {
        uint8_t c = (uint8_t)ft->face[i];
        if (wide) MEM16(d + 24 + 2 * i) = (uint16_t)bw2_cp1252(c);
        else MEM8(d + 24 + i) = c;
    }
    RET(D3D_OK, 2);
}
static void ft_GetDescA(void) { get_desc(0); }
static void ft_GetDescW(void) { get_desc(1); }
/* TEXTMETRICA (56 bytes) and TEXTMETRICW (60): eleven LONGs, four chars, five BYTEs; a BOOL */
static void get_tm(int wide) {
    struct font *ft = font_of(A32(0));
    uint32_t p = A32(1);
    if (!p) RET(0, 2);
    const struct bw2_tm *m = bw2_font_metrics(ft->f);
    const int32_t l[11] = { m->height, m->ascent, m->descent, m->internal_leading, m->external_leading,
                            m->ave_char_width, m->max_char_width, m->weight, m->overhang, m->aspect_x, m->aspect_y };
    for (uint32_t i = 0; i < 11; i++) MEM32(p + 4 * i) = (uint32_t)l[i];
    const uint8_t c[4] = { m->first, m->last, m->deflt, m->brk }, f[5] = { m->italic, m->underlined, m->struck_out,
                                                                            m->pitch_family, m->charset };
    uint32_t o = 44;
    for (int i = 0; i < 4; i++)
        if (wide) MEM16(p + o) = c[i], o += 2;
        else MEM8(p + o) = c[i], o += 1;
    for (int i = 0; i < 5; i++) MEM8(p + o + (uint32_t)i) = f[i];
    o += 5;
    while (o < (wide ? 60u : 56u)) MEM8(p + o++) = 0;
    RET(1, 2);
}
static void ft_GetTextMetricsA(void) { get_tm(0); }
static void ft_GetTextMetricsW(void) { get_tm(1); }
static void ft_PreloadCharacters(void) {         /* (this, first, last): code points */
    struct font *ft = font_of(A32(0));
    uint32_t first = A32(1), last = A32(2);
    if (last > 0x10FFFF && last >= first)
        d3d_fail("ID3DXFont::PreloadCharacters: 0x%X to 0x%X goes past U+10FFFF", first, last);
    for (uint32_t c = first; c <= last; c++) glyph_cached(ft, bw2_font_glyph(ft->f, c));
    RET(D3D_OK, 3);
}
static void preload_text(int wide) {             /* (this, string, count) */
    const char *method = wide ? "PreloadTextW" : "PreloadTextA";
    struct font *ft = font_of(A32(0));
    uint32_t str = A32(1);
    int32_t count = (int32_t)A32(2);
    if (count == 0) RET(D3D_OK, 3);
    uint32_t n, *cp = decode(method, str, count, wide, &n);
    for (uint32_t i = 0; i < n; i++)
        if (cp[i] != '\r' && cp[i] != '\n') glyph_cached(ft, bw2_font_glyph(ft->f, cp[i]));
    free(cp);
    RET(D3D_OK, 3);
}
static void ft_PreloadTextA(void) { preload_text(0); }
static void ft_PreloadTextW(void) { preload_text(1); }
static void ft_OnLostDevice(void) {              /* the cache is managed and survives; the sprite's state block goes */
    struct font *ft = font_of(A32(0));
    uint32_t hr = gcall(ft->sprite, "ID3DXSprite", "OnLostDevice", 0, NULL);
    RET(hr, 1);
}
static void ft_OnResetDevice(void) {
    struct font *ft = font_of(A32(0));
    uint32_t hr = gcall(ft->sprite, "ID3DXSprite", "OnResetDevice", 0, NULL);
    RET(hr, 1);
}

static void font_destroy(struct d3d_obj *o) {
    struct font *ft = (struct font *)o;
    for (uint32_t i = 0; i < ft->ntex; i++) release(ft->tex[i], "IDirect3DTexture9");
    free(ft->tex);
    for (uint32_t i = 0; i < ft->ng; i++) free(ft->g[i].alpha);
    free(ft->g);
    bw2_font_close(ft->f);
    release(ft->sprite, "ID3DXSprite");
    hle_free(hle_ptr(ft->scratch));
    uint32_t device = ft->device;
    free(ft);
    release(device, "IDirect3DDevice9");
}

/* ---- D3DXCreateFontA ---- */

static int font_log(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW2_FONT_LOG");
        on = e && !strcmp(e, "1");
    }
    return on;
}

/* (device, Height, Width, Weight, MipLevels, Italic, CharSet, OutputPrecision, Quality, PitchAndFamily,
 *  pFaceName, ID3DXFont**) */
static void x_D3DXCreateFontA(void) {
    uint32_t device = A32(0), width = A32(2), weight = A32(3), mips = A32(4), italic = A32(5), charset = A32(6);
    uint32_t outp = A32(7), quality = A32(8), pitch = A32(9), facep = A32(10), out = A32(11);
    int32_t height = (int32_t)A32(1);
    if (!device || !facep || !out) RET(D3DERR_INVALIDCALL, 12);
    d3d_host(device, D3DK_DEVICE);
    if (width) d3d_fail("D3DXCreateFontA: Width %u is not handled (only 0)", width);
    if (mips != 1) d3d_fail("D3DXCreateFontA: MipLevels %u is not handled (only 1)", mips);
    if (charset > 1)
        d3d_fail("D3DXCreateFontA: CharSet %u is not handled (only ANSI_CHARSET 0 and DEFAULT_CHARSET 1)", charset);
    if (outp > 0xFF || quality > 0xFF || pitch > 0xFF)
        d3d_fail("D3DXCreateFontA: OutputPrecision %u, Quality %u, PitchAndFamily %u: not bytes", outp, quality, pitch);
    const char *face = (const char *)hle_ptr(facep);
    size_t flen = strnlen(face, 32);
    if (flen == 32) d3d_fail("D3DXCreateFontA: face name \"%.32s...\" is longer than 31 characters", face);
    char err[256];
    struct bw2_font *f = bw2_font_open(face, height, weight, italic != 0, err, sizeof err);
    if (!f) d3d_fail("D3DXCreateFontA: %s", err);
    if (font_log()) {
        fprintf(stderr, "[bw2] font \"%s\" %d weight %u: %s\n", face, height, weight, bw2_font_source(f));
        fflush(stderr);
    }
    struct font *ft = calloc(1, sizeof *ft);
    if (!ft) d3d_fail("D3DXCreateFontA: no memory");
    ft->base.destroy = font_destroy;
    ft->f = f;
    ft->ng = bw2_font_glyph_count(f);
    ft->g = calloc(ft->ng ? ft->ng : 1, sizeof *ft->g);
    if (!ft->g) d3d_fail("D3DXCreateFontA: no memory for %u glyphs", ft->ng);
    ft->device = device;
    ft->height = height, ft->width = width, ft->weight = weight, ft->mips = mips, ft->italic = italic;
    ft->charset = (uint8_t)charset, ft->out_precision = (uint8_t)outp, ft->quality = (uint8_t)quality;
    ft->pitch_family = (uint8_t)pitch;
    memcpy(ft->face, face, flen);
    ft->scratch = hle_va(hle_alloc(SC_SIZE));
    memset(hle_ptr(ft->scratch), 0, SC_SIZE);
    gcall(device, "IDirect3DDevice9", "AddRef", 0, NULL);
    ft->sprite = d3dx_sprite_create(device);
    MEM32(out) = d3d_new(D3DK_FONT, "ID3DXFont", &ft->base);
    RET(D3D_OK, 12);
}

static const win32hle_shim g_impls[] = {
    { "ID3DXFont::GetDevice", ft_GetDevice }, { "ID3DXFont::GetDescA", ft_GetDescA },
    { "ID3DXFont::GetDescW", ft_GetDescW }, { "ID3DXFont::GetTextMetricsA", ft_GetTextMetricsA },
    { "ID3DXFont::GetTextMetricsW", ft_GetTextMetricsW }, { "ID3DXFont::PreloadCharacters", ft_PreloadCharacters },
    { "ID3DXFont::PreloadTextA", ft_PreloadTextA }, { "ID3DXFont::PreloadTextW", ft_PreloadTextW },
    { "ID3DXFont::DrawTextA", ft_DrawTextA }, { "ID3DXFont::DrawTextW", ft_DrawTextW },
    { "ID3DXFont::OnLostDevice", ft_OnLostDevice }, { "ID3DXFont::OnResetDevice", ft_OnResetDevice },
    { 0, 0 } };
static const win32hle_shim g_exports[] = { { "D3DXCreateFontA", x_D3DXCreateFontA }, { 0, 0 } };
void d3dx_font_register(void) { d3d_impls(g_impls); win32hle_register(g_exports); }
