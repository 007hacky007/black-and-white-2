"""A test-side writer for D3DX fx_2_0 effect binaries (plan 5 Task 5), so every effect a test uses is
written by the test.

The layout, as the D3DX effect loader reads it:
- dword 0xFEFF0901, then the offset of the body, both counted from byte 8 (the "data" base);
- the data: names (a dword size, the bytes with their NUL, padded to 4), typedefs and values, each
  found by its offset from the data base;
- the body: parameter, technique, (unused), object counts; each parameter as (typedef, value, flags,
  annotation count) and its annotations as (typedef, value); each technique as (name, annotation
  count, pass count), its annotations, then each pass as (name, annotation count, state count), its
  annotations and its states as (operation, index, typedef, value); then the object data entries
  (id, size, bytes) and the resources (technique or -1, pass or parameter, element or -1, state,
  usage, size, bytes).

A typedef is (type, class, name, semantic, elements) then, by class: a vector's columns and rows, a
scalar's or matrix's rows and columns, a structure's member count and its members' typedefs. An array
of structures lists the member typedefs once. A value is the numbers as dwords, an object id for a
string, texture or shader, and for a sampler its state count and states.
"""
import struct

SCALAR, VECTOR, MATRIX_ROWS, MATRIX_COLUMNS, OBJECT, STRUCT = range(6)
(VOID, BOOL, INT, FLOAT, STRING, TEXTURE, TEXTURE1D, TEXTURE2D, TEXTURE3D, TEXTURECUBE, SAMPLER, SAMPLER1D,
 SAMPLER2D, SAMPLER3D, SAMPLERCUBE, PIXELSHADER, VERTEXSHADER) = range(17)
SHARED = 1
TEXTURES = (TEXTURE, TEXTURE1D, TEXTURE2D, TEXTURE3D, TEXTURECUBE)
SAMPLERS = (SAMPLER, SAMPLER1D, SAMPLER2D, SAMPLER3D, SAMPLERCUBE)


class P:
    """A parameter, annotation or state value: its type and its default value.

    value: numbers (one per component, row by row) for scalars, vectors and matrices; a list of
    element values for an array; a list of member values for a structure; a str for a string; bytes
    (shader tokens) for a shader; None for a texture; a list of State for a sampler."""

    def __init__(self, name, type, cls, rows=1, cols=1, elements=0, members=(), semantic=None, value=None,
                 flags=0, annotations=()):
        self.name, self.type, self.cls, self.rows, self.cols = name, type, cls, rows, cols
        self.elements, self.members, self.semantic, self.value = elements, list(members), semantic, value
        self.flags, self.annotations = flags, list(annotations)


class State:
    """A pass or sampler state. ref names the parameter a usage-1 resource refers to; blob is a usage-0
    resource (shader tokens or an expression); selector is a usage-2 resource, (array name, expression):
    the expression's value picks the array's element."""

    def __init__(self, op, value, index=0, ref=None, blob=None, selector=None):
        self.op, self.value, self.index, self.ref, self.blob = op, value, index, ref, blob
        self.selector = selector


class Pass:
    def __init__(self, name, states=(), annotations=()):
        self.name, self.states, self.annotations = name, list(states), list(annotations)


class Technique:
    def __init__(self, name, passes=(), annotations=()):
        self.name, self.passes, self.annotations = name, list(passes), list(annotations)


def scalar(name, type, v=0, **kw):
    return P(name, type, SCALAR, 1, 1, value=[v], **kw)


def vector(name, v, type=FLOAT, **kw):
    return P(name, type, VECTOR, 1, len(v), value=list(v), **kw)


def string(name, s, **kw):
    return P(name, STRING, OBJECT, value=s, **kw)


class _Writer:
    def __init__(self):
        self.data = bytearray(struct.pack("<I", 0))     # offset 0: the empty name
        self.objects = {}                                # id -> bytes (the object data section)
        self.nobj = 1                                    # id 0 is never used
        self.resources = []

    def u32(self, *v):
        off = len(self.data)
        self.data += struct.pack(f"<{len(v)}I", *v)
        return off

    def name(self, s):
        if s is None:
            return 0
        b = s.encode() + b"\0"
        off = self.u32(len(b))
        self.data += b + b"\0" * (-len(b) % 4)
        return off

    def obj(self, data=None):
        i = self.nobj
        self.nobj += 1
        if data is not None:
            self.objects[i] = data
        return i

    def typedef_words(self, p, top=True):
        w = [p.type, p.cls, self.name(p.name), self.name(p.semantic), p.elements] if top else []
        if p.cls == VECTOR:
            w += [p.cols, p.rows]
        elif p.cls in (SCALAR, MATRIX_ROWS, MATRIX_COLUMNS):
            w += [p.rows, p.cols]
        elif p.cls == STRUCT:
            w += [len(p.members)]
            for m in p.members:
                w += self.typedef_words(m)
        return w

    def typedef(self, p):
        return self.u32(*self.typedef_words(p))

    def value_words(self, p, v, elem=False):
        if p.elements and not elem:
            v = v if v is not None else [None] * p.elements
            assert len(v) == p.elements, p.name
            return [x for e in v for x in self.value_words(p, e, True)]
        if p.cls == STRUCT:
            v = v if v is not None else [None] * len(p.members)
            return [x for m, mv in zip(p.members, v) for x in self.value_words(m, mv if mv is not None else m.value)]
        if p.cls in (SCALAR, VECTOR, MATRIX_ROWS, MATRIX_COLUMNS):
            v = v if v is not None else [0] * (p.rows * p.cols)
            assert len(v) == p.rows * p.cols, p.name
            fmt = "<f" if p.type == FLOAT else "<i"
            return [struct.unpack("<I", struct.pack(fmt, x))[0] for x in v]
        if p.type in SAMPLERS:
            states = v or []
            return [len(states)] + [x for s in states for x in self.state_words(s, ("sampler", p))]
        if p.type == STRING:
            return [self.obj((v or "").encode() + b"\0")]
        if p.type in (VERTEXSHADER, PIXELSHADER):
            return [self.obj(v if v is not None else b"")]
        return [self.obj(b"")]                           # a texture: no data

    def value(self, p):
        return self.u32(*self.value_words(p, p.value))

    def state_words(self, s, where):
        toff = self.typedef(s.value)
        if s.value.type in (VERTEXSHADER, PIXELSHADER) and s.blob is not None:
            sid = self.nobj
            self.nobj += 1
            voff = self.u32(sid)
        else:
            voff = self.value(s.value)
        if s.ref is not None or s.blob is not None or s.selector is not None:
            self.resources.append((where, s))
        return [s.op, s.index, toff, voff]

    def annos(self, annos):
        return [x for a in annos for x in (self.typedef(a), self.value(a))]


def build(params, techniques=()):
    """The fx_2_0 binary of the effect."""
    w = _Writer()
    body = []
    for pi, p in enumerate(params):
        toff, voff = w.typedef(p), w.value(p)
        body += [toff, voff, p.flags, len(p.annotations)] + w.annos(p.annotations)
    for ti, t in enumerate(techniques):
        body += [w.name(t.name), len(t.annotations), len(t.passes)] + w.annos(t.annotations)
        for pi, ps in enumerate(t.passes):
            body += [w.name(ps.name), len(ps.annotations), len(ps.states)] + w.annos(ps.annotations)
            for si, s in enumerate(ps.states):
                body += w.state_words(s, ("pass", ti, pi, si))
    # where each resource's state is: a sampler state by (parameter index, element 0, state index)
    res = bytearray()
    for where, s in w.resources:
        if where[0] == "pass":
            loc = [where[1], where[2], 0xFFFFFFFF, where[3]]
        else:
            sp = where[1]
            loc = [0xFFFFFFFF, params.index(sp), 0, next(i for i, x in enumerate(sp.value) if x is s)]
        if s.selector is not None:                      # the name (its size padded to dwords), the expression
            name = s.selector[0].encode() + b"\0"
            name += b"\0" * (-len(name) % 4)
            usage, blob = 2, struct.pack("<I", len(name)) + name + s.selector[1]
        else:
            usage, blob = (1, s.ref.encode() + b"\0") if s.ref is not None else (0, s.blob)
        res += struct.pack("<6I", *loc, usage, len(blob)) + blob + b"\0" * (-len(blob) % 4)
    objs = bytearray()
    for i, d in sorted(w.objects.items()):
        objs += struct.pack("<2I", i, len(d)) + d + b"\0" * (-len(d) % 4)
    head = struct.pack("<4I", len(params), len(techniques), 0, w.nobj)
    tail = struct.pack("<2I", len(w.objects), len(w.resources))
    data = bytes(w.data)
    return struct.pack("<2I", 0xFEFF0901, len(data)) + data + head + struct.pack(f"<{len(body)}I", *body) \
        + tail + bytes(objs) + bytes(res)


# ---- Task 6: constant tables, expressions and preshaders, as fxc writes them into effects ----

RS_BOOL, RS_INT4, RS_FLOAT4, RS_SAMPLER = range(4)
# expression register tables
LIT, IN, OUT, TEMP = 1, 2, 4, 7
# FXLC opcodes (inputs)
OPS = {"mov": (0x100, 1), "neg": (0x101, 1), "rcp": (0x103, 1), "frc": (0x104, 1), "exp": (0x105, 1),
       "log": (0x106, 1), "rsq": (0x107, 1), "sin": (0x108, 1), "cos": (0x109, 1), "min": (0x200, 2),
       "max": (0x201, 2), "lt": (0x202, 2), "ge": (0x203, 2), "add": (0x204, 2), "mul": (0x205, 2),
       "cmp": (0x300, 3), "dot": (0x500, 2)}


def fourcc(s):
    return struct.unpack("<I", s.encode())[0]


def comment(cc, data):
    """A comment token holding fourcc cc and data (padded to dwords)."""
    data = bytes(data) + b"\0" * (-len(data) % 4)
    return struct.pack("<2I", 0xFFFE | (len(data) // 4 + 1) << 16, fourcc(cc)) + data


class Const:
    """A constant table entry: name, register set, first register, register count and its type, as
    (class, type, rows, columns, elements, members) with members a list of (name, type)."""

    def __init__(self, name, set, index, count, type):
        self.name, self.set, self.index, self.count, self.type = name, set, index, count, type


def ctab(consts, target="vs_2_0"):
    """D3DXSHADER_CONSTANTTABLE and what it points at (offsets from its start)."""
    head = 28
    info = head
    data = bytearray(b"\0" * (head + 20 * len(consts)))
    def put(b):
        off = len(data)
        data.extend(b + b"\0" * (-len(b) % 4))
        return off

    def typeinfo(t):
        cls, typ, rows, cols, elements, members = t
        moff = 0
        if members:
            subs = [(put(n.encode() + b"\0"), typeinfo(mt)) for n, mt in members]
            moff = put(b"".join(struct.pack("<2I", a, b) for a, b in subs))
        return put(struct.pack("<6HI", cls, typ, rows, cols, elements, len(members), moff))
    creator, tgt = put(b"test\0"), put(target.encode() + b"\0")
    for i, c in enumerate(consts):
        name, ti = put(c.name.encode() + b"\0"), typeinfo(c.type)
        struct.pack_into("<I4HII", data, info + 20 * i, name, c.set, c.index, c.count, 0, ti, 0)
    struct.pack_into("<7I", data, 0, head, creator, 0, len(consts), info, 0, tgt)
    return bytes(data)


FLOAT4_T = (VECTOR, FLOAT, 1, 4, 0, [])
FLOAT_T = (SCALAR, FLOAT, 1, 1, 0, [])
MATRIX_COLUMNS_T = (MATRIX_COLUMNS, FLOAT, 4, 4, 0, [])


def ins(op, ncomp, operands, scalar=False):
    """One FXLC instruction: operands are (table, component offset), the inputs then the output."""
    code, nin = OPS[op]
    assert len(operands) == nin + 1, op
    w = [(1 << 31 if scalar else 0) | code << 20 | ncomp, nin]
    for table, off in operands:
        w += [0, table, off]
    return w


def expr(instructions, consts=(), literals=(), prsi=None, version=0x46580200):
    """An expression's tokens: the version, CTAB, PRSI (a preshader's output ranges), CLIT, FXLC, end."""
    out = struct.pack("<I", version)
    out += comment("CTAB", ctab(list(consts), "fx_2_0"))
    if prsi is not None:
        start = min(r for r, _ in prsi)
        count = max(r + n for r, n in prsi) - start
        out += comment("PRSI", struct.pack(f"<{9 + 2 * len(prsi)}I", start, 0, 0, count, 0, 0, len(prsi),
                                           *[x for p in prsi for x in p], 0, 0))
    out += comment("CLIT", struct.pack(f"<I{len(literals)}d", len(literals), *literals))
    words = [len(instructions)] + [x for i in instructions for x in i] + [0xF0F0F0F0, 0x0F0F0F0F]
    out += comment("FXLC", struct.pack(f"<{len(words)}I", *words))
    return out + struct.pack("<I", 0xFFFF)


def shader(tokens, consts=None, preshader=None):
    """Shader tokens (from the assembler) with a CTAB and a PRES comment after the version, as fxc writes."""
    t = bytes(tokens)
    extra = b""
    if consts is not None:
        extra += comment("CTAB", ctab(list(consts)))
    if preshader is not None:
        extra += comment("PRES", preshader)
    return t[:4] + extra + t[4:]
