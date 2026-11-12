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
    resource (shader tokens or an expression)."""

    def __init__(self, op, value, index=0, ref=None, blob=None):
        self.op, self.value, self.index, self.ref, self.blob = op, value, index, ref, blob


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
        if s.ref is not None or s.blob is not None:
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
