"""A small TrueType reader and writer for the font tests (plan 7 Task 3).

It reads an sfnt's tables, works out GDI's text metrics from them (the VDMX rules first, then the OS/2
rules, as runtime/bw2/d3dx/font_ct.m does), and rebuilds a font renamed or without one table. The tests
use it on the Mac's own fonts and write the results only to their tmp_path."""
import math
import struct


def _u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def _i16(b, o):
    return struct.unpack_from(">h", b, o)[0]


def _u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def rnd(x):
    """GDI-style rounding (half up), as the C side's floor(x + 0.5); Python's round() is half-even."""
    return math.floor(x + 0.5)


SFNT_VERSIONS = (b"\0\1\0\0", b"OTTO", b"true")


def tables(data: bytes) -> dict:
    """{tag: bytes} of a single-face sfnt."""
    if data[:4] not in SFNT_VERSIONS:
        raise ValueError(f"not a TrueType/OpenType file: {data[:4]!r}")
    out = {}
    for i in range(_u16(data, 4)):
        tag, _sum, off, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        out[tag.decode("latin-1")] = data[off:off + length]
    return out


def _vdmx(v, height):
    """(ppem, ascent, descent) from a VDMX table, or None: the first ratio record holding 1:1, its group
    when ppem is within startsz..endsz."""
    if v is None:
        return None
    nratios = _u16(v, 4)
    for i in range(nratios):
        _charset, x, ys, ye = v[6 + 4 * i:10 + 4 * i]
        if (x == 0 and ys == 0 and ye == 0) or ys <= x <= ye:
            off = _u16(v, 6 + 4 * nratios + 2 * i)
            break
    else:
        return None
    recs, start, end = _u16(v, off), v[off + 2], v[off + 3]
    entries = [struct.unpack_from(">Hhh", v, off + 4 + 6 * k) for k in range(recs)]
    if height < 0:
        ppem = -height
        if not start <= ppem <= end:
            return None
        for y, ymax, ymin in entries:
            if y == ppem:
                return ppem, ymax, -ymin
        return None
    best = None
    for y, ymax, ymin in entries:
        if start <= y <= end and ymax - ymin <= height and (best is None or y > best[0]):
            best = (y, ymax, -ymin)
    return best


def metrics(data: bytes, height: int, weight: int = 400) -> dict:
    """GDI's TEXTMETRIC for a face at a LOGFONT height, by the rules in plan 7 Task 3."""
    if height == 0:
        raise ValueError("height 0")
    t = tables(data)
    head, hhea, os2, post = t["head"], t["hhea"], t["OS/2"], t["post"]
    upem = _u16(head, 18)
    win_a, win_d = _u16(os2, 74), _u16(os2, 76)
    v = _vdmx(t.get("VDMX"), height)
    if v:
        ppem, ascent, descent = v
    elif height < 0:
        ppem = -height
        s = ppem / upem
        ascent, descent = rnd(win_a * s), rnd(win_d * s)
    else:
        s = height / (win_a + win_d)
        ppem = rnd(upem * s)
        ascent = rnd(win_a * s)
        descent = height - ascent
    h = ascent + descent
    s = ppem / upem
    gap, asc, desc = _i16(hhea, 8), _i16(hhea, 4), _i16(hhea, 6)
    fixed = _u32(post, 12) != 0
    fc, lc = _u16(os2, 64), _u16(os2, 66)
    return dict(height=h, ascent=ascent, descent=descent, internal_leading=h - ppem,
                external_leading=max(0, rnd((gap - ((win_a + win_d) - (asc - desc))) * s)),
                ave_char_width=rnd(_i16(os2, 2) * s), max_char_width=rnd(_u16(hhea, 10) * s),
                weight=weight or 400, overhang=0, aspect_x=96, aspect_y=96,
                first=min(max(fc - 2, 0), 0xFF), last=min(lc, 0xFF), deflt=min(max(fc - 1, 0), 0xFF), brk=0x20,
                italic=0, underlined=0, struck_out=0,
                pitch_family=6 + (0 if fixed else 1) + (0x30 if fixed else 0x20), charset=0, ppem=ppem)


def num_glyphs(data: bytes) -> int:
    return _u16(tables(data)["maxp"], 4)


def units_per_em(data: bytes) -> int:
    return _u16(tables(data)["head"], 18)


def cap_height(data: bytes) -> int:
    """OS/2 sCapHeight (version 2 and later); an older OS/2 (the Mac's Arial Narrow has version 1) has
    none, and then the top of 'H' in glyf, which is what sCapHeight records."""
    os2 = tables(data)["OS/2"]
    if _u16(os2, 0) >= 2:
        return _i16(os2, 88)
    return bbox(data, glyph(data, ord("H")))[3]


def glyph(data: bytes, cp: int) -> int:
    """The glyph of a BMP code point from the (3,1) or (0,3) format 4 cmap, 0 when missing."""
    cmap = tables(data)["cmap"]
    sub = None
    for i in range(_u16(cmap, 2)):
        pid, eid, off = struct.unpack_from(">HHI", cmap, 4 + 8 * i)
        if (pid, eid) in ((3, 1), (0, 3)) and _u16(cmap, off) == 4:
            sub = cmap[off:]
            break
    assert sub is not None, "no format 4 Unicode cmap"
    n = _u16(sub, 6) // 2
    ends, starts = 14, 16 + 2 * n
    deltas, ranges = starts + 2 * n, starts + 4 * n
    for k in range(n):
        if cp <= _u16(sub, ends + 2 * k):
            if cp < _u16(sub, starts + 2 * k):
                return 0
            ro = _u16(sub, ranges + 2 * k)
            if ro == 0:
                return (cp + _u16(sub, deltas + 2 * k)) & 0xFFFF
            g = _u16(sub, ranges + 2 * k + ro + 2 * (cp - _u16(sub, starts + 2 * k)))
            return (g + _u16(sub, deltas + 2 * k)) & 0xFFFF if g else 0
    return 0


def advance(data: bytes, g: int) -> int:
    """hmtx advanceWidth in font units."""
    t = tables(data)
    nh = _u16(t["hhea"], 34)
    return _u16(t["hmtx"], 4 * min(g, nh - 1))


def hdmx(data: bytes, ppem: int):
    """The hdmx widths for ppem (a bytes of one width per glyph), or None."""
    h = tables(data).get("hdmx")
    if h is None:
        return None
    n, size = _i16(h, 2), struct.unpack_from(">i", h, 4)[0]
    for i in range(n):
        rec = h[8 + i * size:8 + (i + 1) * size]
        if rec[0] == ppem:
            return rec[2:2 + num_glyphs(data)]
    return None


def bbox(data: bytes, g: int):
    """(xMin, yMin, xMax, yMax) of a glyph from its glyf header, in font units."""
    t = tables(data)
    long_loca = _i16(t["head"], 50) == 1
    off = _u32(t["loca"], 4 * g) if long_loca else 2 * _u16(t["loca"], 2 * g)
    return struct.unpack_from(">hhhh", t["glyf"], off + 2)


def _checksum(b: bytes) -> int:
    b = b + b"\0" * (-len(b) % 4)
    return sum(struct.unpack(f">{len(b) // 4}I", b)) & 0xFFFFFFFF


def build(tabs: dict, version: bytes = b"\0\1\0\0") -> bytes:
    """An sfnt of these tables, with table checksums and head.checkSumAdjustment."""
    tabs = dict(tabs)
    if "head" in tabs:
        tabs["head"] = tabs["head"][:8] + b"\0\0\0\0" + tabs["head"][12:]
    tags = sorted(tabs)
    n = len(tags)
    es = int(math.log2(n))
    hdr = version + struct.pack(">HHHH", n, 16 << es, es, 16 * n - (16 << es))
    off, dir_, body, head_at = 12 + 16 * n, b"", b"", None
    for t in tags:
        d = tabs[t]
        if t == "head":
            head_at = off + len(body)
        dir_ += struct.pack(">4sIII", t.encode("latin-1"), _checksum(d), off + len(body), len(d))
        body += d + b"\0" * (-len(d) % 4)
    out = bytearray(hdr + dir_ + body)
    if head_at is not None:
        struct.pack_into(">I", out, head_at + 8, (0xB1B0AFBA - _checksum(bytes(out))) & 0xFFFFFFFF)
    return bytes(out)


def _names(name: bytes, changes: dict) -> bytes:
    """A format 0 name table with the strings of the IDs in changes replaced, on every platform."""
    fmt, count, soff = struct.unpack_from(">HHH", name, 0)
    assert fmt == 0, "only format 0 name tables are rewritten"
    recs, store = b"", b""
    for i in range(count):
        pid, eid, lid, nid, ln, off = struct.unpack_from(">6H", name, 6 + 12 * i)
        s = name[soff + off:soff + off + ln]
        if nid in changes:
            s = changes[nid].encode("utf-16-be" if pid in (0, 3) else "mac-roman")
        recs += struct.pack(">6H", pid, eid, lid, nid, len(s), len(store))
        store += s
    return struct.pack(">HHH", 0, count, 6 + len(recs)) + recs + store


def name(data: bytes, nid: int) -> str:
    """Name ID nid from the (3,1) record (English, US), else the first record with that ID."""
    n = tables(data)["name"]
    count, soff = _u16(n, 2), _u16(n, 4)
    found = None
    for i in range(count):
        pid, eid, lid, rid, ln, off = struct.unpack_from(">6H", n, 6 + 12 * i)
        if rid != nid:
            continue
        s = n[soff + off:soff + off + ln].decode("utf-16-be" if pid in (0, 3) else "mac-roman")
        if (pid, eid, lid) == (3, 1, 0x409):
            return s
        found = found or s
    return found


def renamed(data: bytes, family: str, ps: str | None = None) -> bytes:
    """The font with family (IDs 1 and 16), full name (4) and PostScript name (6) changed to family."""
    t = tables(data)
    sub = name(data, 2) or "Regular"
    full = family if sub == "Regular" else f"{family} {sub}"
    if ps is None:
        ps = family.replace(" ", "") + ("" if sub == "Regular" else "-" + sub.replace(" ", ""))
    t["name"] = _names(t["name"], {1: family, 4: full, 6: ps, 16: family})
    return build(t, data[:4])


def dropped(data: bytes, tag: str) -> bytes:
    """The font without one table."""
    t = tables(data)
    del t[tag]
    return build(t, data[:4])


def collection(*fonts: bytes) -> bytes:
    """A TrueType collection ('ttcf' 1.0) of these single-face fonts, each with its own tables."""
    base = 12 + 4 * len(fonts)
    offsets, parts = [], []
    for f in fonts:
        f = bytearray(build(tables(f), f[:4]))
        for i in range(_u16(f, 4)):
            struct.pack_into(">I", f, 12 + 16 * i + 8, _u32(f, 12 + 16 * i + 8) + base)
        offsets.append(base)
        parts.append(bytes(f))
        base += len(f)
    return b"ttcf" + struct.pack(">II", 0x00010000, len(fonts)) + \
        b"".join(struct.pack(">I", o) for o in offsets) + b"".join(parts)
