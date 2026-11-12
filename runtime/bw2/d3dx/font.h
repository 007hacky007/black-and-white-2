/*
 * font.h - fonts for D3DX's ID3DXFont (plan 7): memory fonts (gdi32's
 * AddFontMemResourceEx, private to the process), faces (memory first, then
 * the system's), GDI-style metrics from the font's own tables (VDMX, then
 * OS/2, hhea, head, hdmx) and glyph coverage from CoreText (font_ct.m). The
 * two gdi32 exports are in font_gdi.c.
 */
#ifndef BW2_FONT_H
#define BW2_FONT_H
#include <stddef.h>
#include <stdint.h>

/* ---- memory fonts ---- */
/* Copies data and gives a nonzero handle with *count set to its number of faces, or 0 (with *count 0)
 * if it is not a TrueType/OpenType file or collection; a rejection prints its reason to stderr. */
uint32_t bw2_font_add_memory(const uint8_t *data, uint32_t len, uint32_t *count);
/* 1, or 0 for an unknown handle. The font is hidden from later lookups; a face already opened from it
 * keeps its data until that face is closed. */
int bw2_font_remove_memory(uint32_t handle);

/* ---- faces ---- */
struct bw2_font;
/* face is UTF-8 (read as Windows-1252 if it is not valid UTF-8). Looked up as:
 *   1. memory faces by family (name ID 1) or full name (ID 4), case-insensitively, newest first;
 *   2. a system face whose CoreText family name equals face, case-insensitively.
 * weight >= 600 asks for the bold face and italic for the italic face; a step without that style goes
 * on to the next, and no bold or oblique is synthesized. NULL with a message in err naming the face,
 * height or style on failure. */
struct bw2_font *bw2_font_open(const char *face, int32_t height, uint32_t weight, int italic, char *err,
                               size_t errlen);
void bw2_font_close(struct bw2_font *f);
uint32_t bw2_font_glyph_count(const struct bw2_font *f);      /* maxp.numGlyphs */
const char *bw2_font_source(const struct bw2_font *f);        /* "memory", or the system file's path */

/* ---- metrics (GDI's TEXTMETRIC, in pixels) ---- */
struct bw2_tm {
    int32_t height, ascent, descent, internal_leading, external_leading, ave_char_width, max_char_width,
        weight, overhang, aspect_x, aspect_y;
    uint8_t first, last, deflt, brk, italic, underlined, struck_out, pitch_family, charset;
    uint32_t ppem;
};
const struct bw2_tm *bw2_font_metrics(const struct bw2_font *f);

/* ---- glyphs ---- */
uint32_t bw2_font_glyph(const struct bw2_font *f, uint32_t codepoint);  /* from the cmap; 0 if missing */
int32_t bw2_font_advance(const struct bw2_font *f, uint32_t glyph);     /* pixels: hdmx, else scaled */
/* The black box: left from the pen, top above the baseline, w*h coverage bytes (row 0 at the top,
 * malloc'd; NULL when w = h = 0, as for a blank glyph). */
struct bw2_glyph { int32_t left, top; uint32_t w, h; uint8_t *alpha; };
int bw2_font_raster(const struct bw2_font *f, uint32_t glyph, struct bw2_glyph *out);   /* 1 */

/* The Windows-1252 code point of a byte; the five undefined bytes map to themselves. */
uint32_t bw2_cp1252(uint8_t byte);

#endif
