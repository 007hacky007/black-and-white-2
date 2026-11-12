/*
 * font_ct.m - faces, GDI-style metrics and glyph coverage for ID3DXFont
 * (plan 7). A pure host module: CoreText and CoreGraphics for the system's
 * faces and for drawing glyphs, and the font's own tables (read here, from
 * the TrueType specification) for everything GDI would compute from them.
 *
 * A memory font (AddFontMemResourceEx) is private to the process, as on
 * Windows: its faces are CGFonts made from a copy of the data and are never
 * registered with the system, and a memory face wins over a system face of
 * the same name. Each face keeps its own CFData; an open font holds a
 * reference to it, so removing the memory font frees the data only when the
 * last font opened from it is closed.
 *
 * Metrics follow GDI's rules (font.h, plan 7 Task 3): the VDMX entry for the
 * 1:1 ratio when there is one, else the OS/2 win ascent and descent; then
 * hhea, OS/2 and post for the rest. Advances come from hdmx at the font's
 * ppem, else hmtx scaled. Coverage is CoreText's, antialiased, with no font
 * smoothing, at ppem points into a 1:1 8-bit alpha bitmap.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"

static _Noreturn void font_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[bw2] font: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
    abort();
}

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* A table: big-endian reads, the caller checks the length first. */
struct tbl { const uint8_t *p; uint32_t n; };
static uint32_t u16(struct tbl t, uint32_t o) { return (uint32_t)t.p[o] << 8 | t.p[o + 1]; }
static int32_t s16(struct tbl t, uint32_t o) { return (int16_t)u16(t, o); }
static uint32_t u32(struct tbl t, uint32_t o) { return u16(t, o) << 16 | u16(t, o + 2); }
static int fits(struct tbl t, uint64_t off, uint64_t n) { return off <= t.n && n <= t.n - off; }
static int32_t rnd(double x) { return (int32_t)floor(x + 0.5); }      /* half up, as GDI */

/* ---- the sfnt table directory ---- */

static int sfnt_version_ok(uint32_t v) { return v == 0x00010000u || v == TAG('O', 'T', 'T', 'O') || v == TAG('t', 'r', 'u', 'e'); }

/* the directory at dir and every table it names lie inside data */
static int sfnt_dir_ok(struct tbl d, uint32_t dir) {
    if (!fits(d, dir, 12) || !sfnt_version_ok(u32(d, dir))) return 0;
    uint32_t n = u16(d, dir + 4);
    if (n == 0 || !fits(d, dir + 12, 16ull * n)) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (!fits(d, u32(d, dir + 12 + 16 * i + 8), u32(d, dir + 12 + 16 * i + 12))) return 0;
    return 1;
}
static struct tbl sfnt_table(struct tbl d, uint32_t dir, uint32_t tag) {
    uint32_t n = u16(d, dir + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = dir + 12 + 16 * i;
        if (u32(d, r) == tag) return (struct tbl){ d.p + u32(d, r + 8), u32(d, r + 12) };
    }
    return (struct tbl){ NULL, 0 };
}
/* A collection's face as a standalone sfnt (its tables copied after a directory of their own). */
static CFDataRef sfnt_extract(struct tbl d, uint32_t dir) {
    uint32_t n = u16(d, dir + 4);
    size_t total = 12 + 16 * (size_t)n;
    for (uint32_t i = 0; i < n; i++) total += (u32(d, dir + 12 + 16 * i + 12) + 3u) & ~3u;
    uint8_t *out = calloc(1, total);
    if (!out) font_fatal("out of memory copying a collection's face (%zu bytes)", total);
    memcpy(out, d.p + dir, 12);
    size_t at = 12 + 16 * (size_t)n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = d.p + dir + 12 + 16 * i;
        uint32_t off = u32(d, dir + 12 + 16 * i + 8), len = u32(d, dir + 12 + 16 * i + 12);
        memcpy(out + 12 + 16 * i, r, 8);                               /* tag, checksum */
        out[12 + 16 * i + 8] = (uint8_t)(at >> 24), out[12 + 16 * i + 9] = (uint8_t)(at >> 16);
        out[12 + 16 * i + 10] = (uint8_t)(at >> 8), out[12 + 16 * i + 11] = (uint8_t)at;
        memcpy(out + 12 + 16 * i + 12, r + 12, 4);                     /* length */
        memcpy(out + at, d.p + off, len);
        at += (len + 3u) & ~3u;
    }
    return CFDataCreateWithBytesNoCopy(NULL, out, (CFIndex)total, kCFAllocatorMalloc);
}

/* ---- memory fonts ---- */

#define MAX_NAMES 32
struct memface {
    CFDataRef data;                       /* a standalone sfnt */
    CGFontRef cg;
    CFStringRef names[MAX_NAMES];         /* family (name ID 1) and full (ID 4) names, every platform */
    int nnames, bold, italic;
};
struct memfont {
    uint32_t handle, nfaces;
    struct memface *faces;
    struct memfont *next;
};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct memfont *g_mem;             /* newest first */
static uint32_t g_handles;

static void add_name(struct memface *mf, CFStringRef s) {
    for (int i = 0; i < mf->nnames; i++)
        if (CFStringCompare(mf->names[i], s, kCFCompareCaseInsensitive) == kCFCompareEqualTo) { CFRelease(s); return; }
    if (mf->nnames < MAX_NAMES) mf->names[mf->nnames++] = s;
    else CFRelease(s);
}
static void read_names(struct tbl name, struct memface *mf) {
    if (name.n < 6) return;
    uint32_t count = u16(name, 2), soff = u16(name, 4);
    if (!fits(name, 6, 12ull * count)) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t r = 6 + 12 * i, pid = u16(name, r), eid = u16(name, r + 2), nid = u16(name, r + 6);
        uint32_t len = u16(name, r + 8), off = soff + u16(name, r + 10);
        if ((nid != 1 && nid != 4) || !len || !fits(name, off, len)) continue;
        CFStringEncoding enc;
        if (pid == 0 || pid == 3) enc = kCFStringEncodingUTF16BE;
        else if (pid == 1 && eid == 0) enc = kCFStringEncodingMacRoman;
        else continue;
        CFStringRef s = CFStringCreateWithBytes(NULL, name.p + off, (CFIndex)len, enc, false);
        if (s) add_name(mf, s);
    }
}
static void free_faces(struct memface *faces, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k < faces[i].nnames; k++) CFRelease(faces[i].names[k]);
        if (faces[i].cg) CGFontRelease(faces[i].cg);
        if (faces[i].data) CFRelease(faces[i].data);
    }
    free(faces);
}

uint32_t bw2_font_add_memory(const uint8_t *data, uint32_t len, uint32_t *count) {
    *count = 0;
    struct tbl d = { data, len };
    const char *why = NULL;
    uint32_t nf = 1;
    if (!data || len < 12) why = "shorter than an sfnt header";
    else if (u32(d, 0) == TAG('t', 't', 'c', 'f')) {
        nf = u32(d, 8);
        if (nf == 0 || nf > 1024 || !fits(d, 12, 4ull * nf)) why = "a collection header with no faces";
    } else if (!sfnt_version_ok(u32(d, 0))) why = "no sfnt version";
    struct memface *faces = why ? NULL : calloc(nf, sizeof *faces);
    for (uint32_t i = 0; faces && i < nf && !why; i++) {
        struct memface *mf = &faces[i];
        int ttc = u32(d, 0) == TAG('t', 't', 'c', 'f');
        uint32_t dir = ttc ? u32(d, 12 + 4 * i) : 0;
        if (!sfnt_dir_ok(d, dir)) { why = "a table directory outside the data"; break; }
        mf->data = ttc ? sfnt_extract(d, dir) : CFDataCreate(NULL, data, (CFIndex)len);
        struct tbl fd = { CFDataGetBytePtr(mf->data), (uint32_t)CFDataGetLength(mf->data) };
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(mf->data);
        mf->cg = prov ? CGFontCreateWithDataProvider(prov) : NULL;
        CGDataProviderRelease(prov);
        if (!mf->cg) { why = "CoreGraphics cannot read it"; break; }
        read_names(sfnt_table(fd, 0, TAG('n', 'a', 'm', 'e')), mf);
        if (!mf->nnames) { why = "no family or full name"; break; }
        struct tbl os2 = sfnt_table(fd, 0, TAG('O', 'S', '/', '2')), head = sfnt_table(fd, 0, TAG('h', 'e', 'a', 'd'));
        uint32_t fs = os2.n >= 64 ? u16(os2, 62) : 0, mac = head.n >= 46 ? u16(head, 44) : 0;
        mf->bold = (fs & 0x20) || (mac & 1);
        mf->italic = (fs & 0x01) || (mac & 2);
    }
    if (why) {
        fprintf(stderr, "[bw2] font: %u bytes in memory are not a TrueType/OpenType font (%s)\n", len, why);
        if (faces) free_faces(faces, nf);
        return 0;
    }
    if (!faces) font_fatal("out of memory adding a memory font");
    struct memfont *m = calloc(1, sizeof *m);
    if (!m) font_fatal("out of memory adding a memory font");
    m->nfaces = nf, m->faces = faces;
    pthread_mutex_lock(&g_lock);
    m->handle = 0x1F000000u | ++g_handles;
    m->next = g_mem, g_mem = m;
    pthread_mutex_unlock(&g_lock);
    *count = nf;
    return m->handle;
}

int bw2_font_remove_memory(uint32_t handle) {
    pthread_mutex_lock(&g_lock);
    struct memfont **pp = &g_mem;
    while (*pp && (*pp)->handle != handle) pp = &(*pp)->next;
    struct memfont *m = *pp;
    if (m) *pp = m->next;
    pthread_mutex_unlock(&g_lock);
    if (!m) return 0;
    free_faces(m->faces, m->nfaces);                    /* open fonts hold their own references */
    free(m);
    return 1;
}

/* ---- faces ---- */

enum { T_HEAD, T_HHEA, T_OS2, T_MAXP, T_POST, T_HMTX, T_CMAP, T_HDMX, T_VDMX, NTAB };
static const uint32_t g_tags[NTAB] = {
    TAG('h', 'e', 'a', 'd'), TAG('h', 'h', 'e', 'a'), TAG('O', 'S', '/', '2'), TAG('m', 'a', 'x', 'p'),
    TAG('p', 'o', 's', 't'), TAG('h', 'm', 't', 'x'), TAG('c', 'm', 'a', 'p'), TAG('h', 'd', 'm', 'x'),
    TAG('V', 'D', 'M', 'X'),
};
static const char *const g_tag_names[NTAB] = { "head", "hhea", "OS/2", "maxp", "post", "hmtx", "cmap", "hdmx", "VDMX" };

struct bw2_font {
    CTFontRef ct;
    CFDataRef data;                       /* a memory face's sfnt, or NULL */
    CFDataRef copied[NTAB];               /* a system face's tables, from CoreText */
    struct tbl t[NTAB];
    struct tbl sub;                       /* the cmap subtable used */
    uint32_t sub_fmt;
    int symbol;                           /* a (3,0) cmap: code points also at 0xF000 + byte */
    const uint8_t *widths;                /* hdmx at ppem, or NULL */
    uint32_t nglyphs, nhm, upem;
    double s;                             /* ppem / unitsPerEm */
    struct bw2_tm tm;
    char face[128], source[1024];
};

static const char *style_name(int bold, int italic) {
    return bold ? (italic ? "bold italic" : "bold") : (italic ? "italic" : "regular");
}
static CFStringRef cfstr(const char *s) {
    CFStringRef r = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
    return r ? r : CFStringCreateWithCString(NULL, s, kCFStringEncodingWindowsLatin1);
}
static int same_name(CFStringRef a, CFStringRef b) {
    return a && b && CFStringCompare(a, b, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

/* Step 1: a memory face by name and style, newest first. *named is set if any memory face has the name.
 * On a match, *data and *cg are retained for the caller. */
static int find_memory(CFStringRef want, int bold, int italic, int *named, CFDataRef *data, CGFontRef *cg) {
    int found = 0;
    pthread_mutex_lock(&g_lock);
    for (struct memfont *m = g_mem; m && !found; m = m->next)
        for (uint32_t i = 0; i < m->nfaces && !found; i++) {
            struct memface *mf = &m->faces[i];
            int hit = 0;
            for (int k = 0; k < mf->nnames && !hit; k++) hit = same_name(mf->names[k], want);
            if (!hit) continue;
            *named = 1;
            if (!mf->bold != !bold || !mf->italic != !italic) continue;
            *data = (CFDataRef)CFRetain(mf->data);
            *cg = CGFontRetain(mf->cg);
            found = 1;
        }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static CFArrayRef g_families;             /* the system's family names, read once */
static double trait(CFDictionaryRef traits, CFStringRef key) {
    CFNumberRef n = traits ? CFDictionaryGetValue(traits, key) : NULL;
    double v = 0;
    if (n) CFNumberGetValue(n, kCFNumberDoubleType, &v);
    return v;
}
/* Step 2: a system face of the family (CoreText's family name, case-insensitively) with the style:
 * among several, the weight nearest regular (or bold) and the width nearest normal, then the
 * PostScript name, so the choice is stable. A retained descriptor, or NULL. */
static CTFontDescriptorRef find_system(CFStringRef want, int bold, int italic, int *named) {
    CFStringRef fam = NULL;
    pthread_mutex_lock(&g_lock);
    if (!g_families) g_families = CTFontManagerCopyAvailableFontFamilyNames();
    for (CFIndex i = 0; g_families && i < CFArrayGetCount(g_families) && !fam; i++) {
        CFStringRef s = CFArrayGetValueAtIndex(g_families, i);
        if (same_name(s, want)) fam = (CFStringRef)CFRetain(s);
    }
    pthread_mutex_unlock(&g_lock);
    if (!fam) return NULL;
    const void *keys[] = { kCTFontFamilyNameAttribute }, *vals[] = { fam };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFSetRef mandatory = CFSetCreate(NULL, keys, 1, &kCFTypeSetCallBacks);
    CTFontDescriptorRef q = CTFontDescriptorCreateWithAttributes(attrs);
    CFArrayRef all = CTFontDescriptorCreateMatchingFontDescriptors(q, mandatory);
    CTFontDescriptorRef best = NULL;
    double best_score = 0;
    CFStringRef best_ps = NULL;
    for (CFIndex i = 0; all && i < CFArrayGetCount(all); i++) {
        CTFontDescriptorRef dsc = CFArrayGetValueAtIndex(all, i);
        CFStringRef f2 = CTFontDescriptorCopyAttribute(dsc, kCTFontFamilyNameAttribute);
        int same = f2 && CFStringCompare(f2, fam, 0) == kCFCompareEqualTo;
        if (f2) CFRelease(f2);
        if (!same) continue;
        *named = 1;
        CFDictionaryRef tr = CTFontDescriptorCopyAttribute(dsc, kCTFontTraitsAttribute);
        uint32_t sym = (uint32_t)trait(tr, kCTFontSymbolicTrait);
        double score = fabs(trait(tr, kCTFontWeightTrait) - (bold ? 0.4 : 0.0)) + fabs(trait(tr, kCTFontWidthTrait));
        if (tr) CFRelease(tr);
        if (!(sym & kCTFontTraitBold) != !bold || !(sym & kCTFontTraitItalic) != !italic) continue;
        CFStringRef ps = CTFontDescriptorCopyAttribute(dsc, kCTFontNameAttribute);
        if (!best || score < best_score ||
            (score == best_score && ps && best_ps && CFStringCompare(ps, best_ps, 0) == kCFCompareLessThan)) {
            if (best) CFRelease(best);
            if (best_ps) CFRelease(best_ps);
            best = (CTFontDescriptorRef)CFRetain(dsc), best_score = score, best_ps = ps;
        } else if (ps) {
            CFRelease(ps);
        }
    }
    if (best_ps) CFRelease(best_ps);
    if (all) CFRelease(all);
    CFRelease(q), CFRelease(mandatory), CFRelease(attrs), CFRelease(fam);
    return best;
}

/* ---- metrics ---- */

/* The VDMX entry for height (plan 7 Task 3): 1 with ppem, ascent and descent; 0 to use OS/2; -1 if the
 * table is malformed. */
static int vdmx_lookup(struct tbl v, int32_t height, uint32_t *ppem, int32_t *asc, int32_t *desc) {
    if (!v.p) return 0;
    if (v.n < 6) return -1;
    uint32_t nr = u16(v, 4), off = 0;
    if (!fits(v, 6, 6ull * nr)) return -1;
    int found = 0;
    for (uint32_t i = 0; i < nr && !found; i++) {
        uint32_t x = v.p[6 + 4 * i + 1], ys = v.p[6 + 4 * i + 2], ye = v.p[6 + 4 * i + 3];
        if ((x == 0 && ys == 0 && ye == 0) || (ys <= x && x <= ye)) off = u16(v, 6 + 4 * nr + 2 * i), found = 1;
    }
    if (!found) return 0;
    if (!fits(v, off, 4)) return -1;
    uint32_t recs = u16(v, off), start = v.p[off + 2], end = v.p[off + 3];
    if (!fits(v, off + 4, 6ull * recs)) return -1;
    if (height < 0) {
        uint32_t want = (uint32_t)-height;
        if (want < start || want > end) return 0;
        for (uint32_t k = 0; k < recs; k++) {
            uint32_t e = off + 4 + 6 * k;
            if (u16(v, e) == want) { *ppem = want, *asc = s16(v, e + 2), *desc = -s16(v, e + 4); return 1; }
        }
        return 0;
    }
    int got = 0;
    for (uint32_t k = 0; k < recs; k++) {
        uint32_t e = off + 4 + 6 * k, y = u16(v, e);
        int32_t ymax = s16(v, e + 2), ymin = s16(v, e + 4);
        if (y >= start && y <= end && ymax - ymin <= height && (!got || y > *ppem))
            *ppem = y, *asc = ymax, *desc = -ymin, got = 1;
    }
    return got;
}

/* The cmap subtable: (3,10), (0,4) or (0,6) for the full range, then (3,1) or another Unicode one, then
 * (3,0) symbol; formats 0, 4, 6 and 12. */
static int pick_cmap(struct bw2_font *f, char *err, size_t errlen) {
    struct tbl c = f->t[T_CMAP];
    if (c.n < 4 || !fits(c, 4, 8ull * u16(c, 2))) { snprintf(err, errlen, "\"%s\": its cmap is malformed", f->face); return 0; }
    int best = 0;
    for (uint32_t i = 0; i < u16(c, 2); i++) {
        uint32_t pid = u16(c, 4 + 8 * i), eid = u16(c, 4 + 8 * i + 2), off = u32(c, 4 + 8 * i + 4);
        int score = pid == 3 && eid == 10 ? 5 : pid == 0 && (eid == 4 || eid == 6) ? 4 : pid == 3 && eid == 1 ? 3
                  : pid == 0 ? 2 : pid == 3 && eid == 0 ? 1 : 0;
        if (score <= best || !fits(c, off, 8)) continue;
        uint32_t fmt = u16(c, off);
        uint64_t len = fmt == 12 ? u32(c, off + 4) : u16(c, off + 2);
        if ((fmt != 0 && fmt != 4 && fmt != 6 && fmt != 12) || !fits(c, off, len)) continue;
        struct tbl s = { c.p + off, (uint32_t)len };
        if (fmt == 0 && s.n < 262) continue;
        if (fmt == 4 && (s.n < 14 || !fits(s, 14, 4ull * u16(s, 6) + 2))) continue;
        if (fmt == 6 && (s.n < 10 || !fits(s, 10, 2ull * u16(s, 8)))) continue;
        if (fmt == 12 && (s.n < 16 || !fits(s, 16, 12ull * u32(s, 12)))) continue;
        f->sub = s, f->sub_fmt = fmt, f->symbol = score == 1, best = score;
    }
    if (!best) snprintf(err, errlen, "\"%s\": its cmap has no Unicode subtable this layer reads", f->face);
    return best != 0;
}

static int fill_metrics(struct bw2_font *f, int32_t height, uint32_t weight, int italic, char *err, size_t errlen) {
    static const uint32_t min_len[NTAB] = { 54, 36, 78, 6, 16, 4, 4, 0, 0 };
    for (int i = 0; i < NTAB; i++)
        if (min_len[i] && f->t[i].n < min_len[i]) {
            snprintf(err, errlen, "\"%s\": %s table %s", f->face, f->t[i].p ? "a short" : "no", g_tag_names[i]);
            return 0;
        }
    struct tbl head = f->t[T_HEAD], hhea = f->t[T_HHEA], os2 = f->t[T_OS2], post = f->t[T_POST];
    f->upem = u16(head, 18), f->nglyphs = u16(f->t[T_MAXP], 4), f->nhm = u16(hhea, 34);
    uint32_t win_a = u16(os2, 74), win_d = u16(os2, 76);
    if (!f->upem || !f->nglyphs || !f->nhm || f->nhm > f->nglyphs || f->t[T_HMTX].n < 4u * f->nhm || !(win_a + win_d)) {
        snprintf(err, errlen, "\"%s\": head, maxp, hhea, hmtx or OS/2 is malformed", f->face);
        return 0;
    }
    uint32_t ppem = 0;
    int32_t asc = 0, desc = 0;
    int v = vdmx_lookup(f->t[T_VDMX], height, &ppem, &asc, &desc);
    if (v < 0) { snprintf(err, errlen, "\"%s\": its VDMX table is malformed", f->face); return 0; }
    if (!v && height < 0) {
        ppem = (uint32_t)-height;
        double s = (double)ppem / f->upem;
        asc = rnd(win_a * s), desc = rnd(win_d * s);
    } else if (!v) {
        double s = (double)height / (win_a + win_d);
        int32_t p = rnd(f->upem * s);
        asc = rnd(win_a * s), desc = height - asc;
        if (p <= 0) { snprintf(err, errlen, "\"%s\": height %d gives ppem 0", f->face, height); return 0; }
        ppem = (uint32_t)p;
    }
    double s = f->s = (double)ppem / f->upem;
    struct bw2_tm *t = &f->tm;
    t->height = asc + desc, t->ascent = asc, t->descent = desc, t->internal_leading = t->height - (int32_t)ppem;
    int32_t gap = s16(hhea, 8), hasc = s16(hhea, 4), hdesc = s16(hhea, 6);
    int32_t ext = rnd((double)(gap - ((int32_t)(win_a + win_d) - (hasc - hdesc))) * s);
    t->external_leading = ext > 0 ? ext : 0;
    t->ave_char_width = rnd(s16(os2, 2) * s), t->max_char_width = rnd(u16(hhea, 10) * s);
    t->weight = weight ? (int32_t)weight : 400, t->overhang = 0, t->aspect_x = 96, t->aspect_y = 96;
    int32_t fc = (int32_t)u16(os2, 64), lc = (int32_t)u16(os2, 66);
    t->first = (uint8_t)(fc - 2 < 0 ? 0 : fc - 2 > 255 ? 255 : fc - 2);
    t->deflt = (uint8_t)(fc - 1 < 0 ? 0 : fc - 1 > 255 ? 255 : fc - 1);
    t->last = (uint8_t)(lc > 255 ? 255 : lc), t->brk = 0x20;
    t->italic = italic ? 255 : 0, t->underlined = 0, t->struck_out = 0;
    int fixed = u32(post, 12) != 0;                                  /* GDI's flag is the inverse */
    t->pitch_family = (uint8_t)(6 + (fixed ? 0 : 1) + (fixed ? 0x30 : 0x20)), t->charset = 0;
    t->ppem = ppem;
    struct tbl h = f->t[T_HDMX];                                     /* hdmx widths at ppem */
    f->widths = NULL;
    if (h.p) {
        int32_t n = h.n >= 8 ? s16(h, 2) : -1;
        int32_t size = h.n >= 8 ? (int32_t)u32(h, 4) : -1;
        if (n < 0 || size < (int32_t)(2 + f->nglyphs) || !fits(h, 8, (uint64_t)n * (uint64_t)size)) {
            snprintf(err, errlen, "\"%s\": its hdmx table is malformed", f->face);
            return 0;
        }
        for (int32_t i = 0; i < n && !f->widths; i++)
            if (h.p[8 + (uint32_t)i * (uint32_t)size] == ppem) f->widths = h.p + 8 + (uint32_t)i * (uint32_t)size + 2;
    }
    return pick_cmap(f, err, errlen);
}

struct bw2_font *bw2_font_open(const char *face, int32_t height, uint32_t weight, int italic, char *err,
                               size_t errlen) {
    if (errlen) err[0] = 0;
    if (height == 0) { snprintf(err, errlen, "\"%s\": height 0", face); return NULL; }
    if (height < -2048 || height > 2048) { snprintf(err, errlen, "\"%s\": height %d is out of range", face, height); return NULL; }
    int bold = weight >= 600;
    italic = !!italic;
    CFStringRef want = cfstr(face);
    if (!want) { snprintf(err, errlen, "no font named \"%s\"", face); return NULL; }
    int named = 0;
    CFDataRef data = NULL;
    CGFontRef cg = NULL;
    CTFontDescriptorRef desc = NULL;
    if (!find_memory(want, bold, italic, &named, &data, &cg)) desc = find_system(want, bold, italic, &named);
    CFRelease(want);
    if (!data && !desc) {
        if (named)
            snprintf(err, errlen, "\"%s\" has no %s face (GDI's synthetic bold and oblique are not done)", face,
                     style_name(bold, italic));
        else
            snprintf(err, errlen, "no font named \"%s\"", face);
        return NULL;
    }
    struct bw2_font *f = calloc(1, sizeof *f);
    if (!f) font_fatal("out of memory opening \"%s\"", face);
    snprintf(f->face, sizeof f->face, "%s", face);
    CTFontRef probe = NULL;
    if (data) {
        f->data = data;
        struct tbl d = { CFDataGetBytePtr(data), (uint32_t)CFDataGetLength(data) };
        for (int i = 0; i < NTAB; i++) f->t[i] = sfnt_table(d, 0, g_tags[i]);
        snprintf(f->source, sizeof f->source, "memory");
    } else {
        probe = CTFontCreateWithFontDescriptor(desc, 12.0, NULL);
        for (int i = 0; i < NTAB && probe; i++) {
            f->copied[i] = CTFontCopyTable(probe, g_tags[i], kCTFontTableOptionNoOptions);
            if (f->copied[i])
                f->t[i] = (struct tbl){ CFDataGetBytePtr(f->copied[i]), (uint32_t)CFDataGetLength(f->copied[i]) };
        }
        CFURLRef url = CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
        if (!url || !CFURLGetFileSystemRepresentation(url, true, (UInt8 *)f->source, sizeof f->source))
            snprintf(f->source, sizeof f->source, "?");
        if (url) CFRelease(url);
    }
    int ok = (data || probe) && fill_metrics(f, height, weight, italic, err, errlen);
    if (!data && !probe) snprintf(err, errlen, "\"%s\": CoreText cannot open the %s face", face, style_name(bold, italic));
    if (ok) {
        f->ct = data ? CTFontCreateWithGraphicsFont(cg, f->tm.ppem, NULL, NULL)
                     : CTFontCreateCopyWithAttributes(probe, f->tm.ppem, NULL, NULL);
        /* CoreText draws from this face's own data: a memory face that shares a system face's PostScript
         * name (the game's Arial Narrow does) must not be swapped for the system's */
        if (!f->ct || (uint32_t)CTFontGetGlyphCount(f->ct) != f->nglyphs) {
            snprintf(err, errlen, "\"%s\": CoreText gives %ld glyphs, the face's maxp %u", face,
                     f->ct ? (long)CTFontGetGlyphCount(f->ct) : -1L, f->nglyphs);
            ok = 0;
        }
    }
    if (probe) CFRelease(probe);
    if (cg) CGFontRelease(cg);
    if (desc) CFRelease(desc);
    if (!ok) { bw2_font_close(f); return NULL; }
    return f;
}

void bw2_font_close(struct bw2_font *f) {
    if (!f) return;
    if (f->ct) CFRelease(f->ct);
    if (f->data) CFRelease(f->data);
    for (int i = 0; i < NTAB; i++)
        if (f->copied[i]) CFRelease(f->copied[i]);
    free(f);
}

uint32_t bw2_font_glyph_count(const struct bw2_font *f) { return f->nglyphs; }
const char *bw2_font_source(const struct bw2_font *f) { return f->source; }
const struct bw2_tm *bw2_font_metrics(const struct bw2_font *f) { return &f->tm; }

/* ---- glyphs ---- */

static uint32_t cmap_lookup(const struct bw2_font *f, uint32_t cp) {
    struct tbl s = f->sub;
    switch (f->sub_fmt) {
    case 0:
        return cp < 256 ? s.p[6 + cp] : 0;
    case 6: {
        uint32_t first = u16(s, 6), n = u16(s, 8);
        return cp >= first && cp - first < n ? u16(s, 10 + 2 * (cp - first)) : 0;
    }
    case 4: {
        if (cp > 0xFFFF) return 0;
        uint32_t n = u16(s, 6) / 2, ends = 14, starts = 16 + 2 * n, deltas = starts + 2 * n, ranges = deltas + 2 * n;
        for (uint32_t k = 0; k < n; k++) {
            if (cp > u16(s, ends + 2 * k)) continue;
            uint32_t start = u16(s, starts + 2 * k), delta = u16(s, deltas + 2 * k), ro = u16(s, ranges + 2 * k);
            if (cp < start) return 0;
            if (!ro) return (cp + delta) & 0xFFFF;
            uint64_t at = (uint64_t)ranges + 2 * k + ro + 2 * (cp - start);
            if (!fits(s, at, 2)) font_fatal("\"%s\": cmap format 4 reads outside its subtable for U+%04X", f->face, cp);
            uint32_t g = u16(s, (uint32_t)at);
            return g ? (g + delta) & 0xFFFF : 0;
        }
        return 0;
    }
    case 12:
        for (uint32_t k = 0, n = u32(s, 12); k < n; k++) {
            uint32_t g = 16 + 12 * k, a = u32(s, g), b = u32(s, g + 4);
            if (cp >= a && cp <= b) return u32(s, g + 8) + (cp - a);
        }
        return 0;
    }
    return 0;
}

uint32_t bw2_font_glyph(const struct bw2_font *f, uint32_t cp) {
    uint32_t g = cmap_lookup(f, cp);
    if (!g && f->symbol && cp < 0x100) g = cmap_lookup(f, 0xF000u + cp);
    if (g >= f->nglyphs) font_fatal("\"%s\": the cmap maps U+%04X to glyph %u of %u", f->face, cp, g, f->nglyphs);
    return g;
}

int32_t bw2_font_advance(const struct bw2_font *f, uint32_t glyph) {
    if (glyph >= f->nglyphs) font_fatal("\"%s\": advance of glyph %u of %u", f->face, glyph, f->nglyphs);
    if (f->widths) return f->widths[glyph];
    uint32_t i = glyph < f->nhm ? glyph : f->nhm - 1;
    return rnd(u16(f->t[T_HMTX], 4 * i) * f->s);
}

int bw2_font_raster(const struct bw2_font *f, uint32_t glyph, struct bw2_glyph *out) {
    if (glyph >= f->nglyphs) font_fatal("\"%s\": raster of glyph %u of %u", f->face, glyph, f->nglyphs);
    memset(out, 0, sizeof *out);
    CGGlyph g = (CGGlyph)glyph;
    CGRect r;
    CTFontGetBoundingRectsForGlyphs(f->ct, kCTFontOrientationHorizontal, &g, &r, 1);
    if (CGRectIsNull(r) || CGRectIsEmpty(r)) return 1;                 /* blank: the space */
    int32_t x0 = (int32_t)floor(CGRectGetMinX(r)), y0 = (int32_t)floor(CGRectGetMinY(r));
    int32_t x1 = (int32_t)ceil(CGRectGetMaxX(r)), y1 = (int32_t)ceil(CGRectGetMaxY(r));
    if (x1 <= x0 || y1 <= y0) return 1;
    uint32_t w = (uint32_t)(x1 - x0), h = (uint32_t)(y1 - y0);
    if (w > 4096 || h > 4096) font_fatal("\"%s\": glyph %u is %ux%u pixels", f->face, glyph, w, h);
    size_t stride = (w + 15u) & ~15u;
    uint8_t *buf = calloc(stride * h, 1), *alpha = malloc((size_t)w * h);
    if (!buf || !alpha) font_fatal("out of memory drawing glyph %u", glyph);
    CGContextRef cx = CGBitmapContextCreate(buf, w, h, 8, stride, NULL, (CGBitmapInfo)kCGImageAlphaOnly);
    if (!cx) font_fatal("\"%s\": no %ux%u alpha bitmap for glyph %u", f->face, w, h, glyph);
    CGContextSetAllowsAntialiasing(cx, true);
    CGContextSetShouldAntialias(cx, true);
    CGContextSetAllowsFontSmoothing(cx, false);
    CGContextSetShouldSmoothFonts(cx, false);
    CGContextSetAllowsFontSubpixelPositioning(cx, false);
    CGContextSetShouldSubpixelPositionFonts(cx, false);
    CGContextSetAllowsFontSubpixelQuantization(cx, false);
    CGContextSetShouldSubpixelQuantizeFonts(cx, false);
    CGContextSetTextMatrix(cx, CGAffineTransformIdentity);
    CGContextSetGrayFillColor(cx, 0, 1);
    CGPoint pos = { -(CGFloat)x0, -(CGFloat)y0 };
    CTFontDrawGlyphs(f->ct, &g, &pos, 1, cx);
    CGContextFlush(cx);
    CGContextRelease(cx);
    for (uint32_t y = 0; y < h; y++) memcpy(alpha + (size_t)y * w, buf + (size_t)y * stride, w);   /* row 0: top */
    free(buf);
    out->left = x0, out->top = y1, out->w = w, out->h = h, out->alpha = alpha;
    return 1;
}

/* ---- Windows-1252 ---- */

static const uint16_t g_cp1252_80[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};
uint32_t bw2_cp1252(uint8_t byte) { return byte >= 0x80 && byte < 0xA0 ? g_cp1252_80[byte - 0x80] : byte; }
