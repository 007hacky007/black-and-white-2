"""Plan 5 Task 5: D3DX effects - the fx_2_0 binary, parameters and pools (runtime/bw2/d3dx/fx_selftest.c).

Every effect here is written by tests/fxwrite.py; the selftest's expectations follow the builders
below. The game's effects and executable are read only with BW2_GAMEFILES=1, into tmp_path."""
import collections
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

from bw2 import paths
from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, engine_libs,
                       engine_sources, sdk_path)

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxwrite as F  # noqa: E402


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("effects") / "fx_selftest"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [BW2RT / "d3dx" / "fx_selftest.c"]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *map(str, srcs),
                        *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def run_group(exe, group, tmp_path, *args):
    env = dict(os.environ, BW2_HEADLESS="1", BW2_SUPPORT=str(tmp_path / "support"))
    return subprocess.run([str(exe), group, *map(str, args)], capture_output=True, text=True, timeout=300,
                          cwd=tmp_path, env=env)


def check(exe, group, tmp_path, *args):
    r = run_group(exe, group, tmp_path, *args)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-6000:] + r.stderr[-4000:]
    return r


def write(tmp_path, name, data):
    p = tmp_path / name
    p.write_bytes(data)
    return p


# ---- the synthetic effects ----

def params_effect():
    """One of each parameter type, a structure, an array of 4 float4, two annotations and a string."""
    tex = F.P(None, F.TEXTURE, F.OBJECT)
    return F.build([
        F.scalar("b", F.BOOL, 1),
        F.scalar("i", F.INT, 7),
        F.scalar("f", F.FLOAT, 0.5, semantic="FSEM",
                 annotations=[F.string("a1", "hello"), F.scalar("a2", F.FLOAT, 3.0)]),
        F.vector("v", [1, 2, 3, 4]),
        F.vector("v3", [0.25, 0.5, 0.75]),
        F.P("m", F.FLOAT, F.MATRIX_ROWS, 4, 4, value=[float(k) for k in range(16)]),
        F.P("t", F.TEXTURE, F.OBJECT),
        F.P("s", F.VOID, F.STRUCT, members=[F.scalar("a", F.FLOAT, 1.0), F.vector("member", [5, 6, 7, 8])]),
        F.P("arr", F.FLOAT, F.VECTOR, 1, 4, elements=4, value=[[k, k + 0.5, k + 0.25, -k] for k in range(4)]),
        F.string("str", "text"),
        F.P("smp", F.SAMPLER2D, F.OBJECT, value=[F.State(164, tex, ref="t"), F.State(165, F.scalar(None, F.INT, 3))]),
        F.P("fa", F.FLOAT, F.SCALAR, 1, 1, elements=3, value=[[1.5], [2.5], [3.5]]),
    ], [F.Technique("t0", [F.Pass("p0", [F.State(0, F.scalar(None, F.INT, 1))],
                                  annotations=[F.scalar("pa", F.INT, 2)])],
                    annotations=[F.string("tanno", "x")])])


def matrix_effect():
    """Row- and column-major matrices, as stored: 1..16 (or 1..12) in storage order."""
    seq = [float(k) for k in range(1, 17)]
    return F.build([
        F.P("mr", F.FLOAT, F.MATRIX_ROWS, 4, 4, value=seq),
        F.P("mc", F.FLOAT, F.MATRIX_COLUMNS, 4, 4, value=seq),
        F.P("m34", F.FLOAT, F.MATRIX_ROWS, 3, 4, value=seq[:12]),
        F.P("c43", F.FLOAT, F.MATRIX_COLUMNS, 4, 3, value=seq[:12]),
        F.P("ma", F.FLOAT, F.MATRIX_ROWS, 4, 4, elements=2),
        F.P("mca", F.FLOAT, F.MATRIX_COLUMNS, 4, 4, elements=2),
    ])


def pool_effects():
    a = F.build([F.scalar("g", F.FLOAT, 1.0, flags=F.SHARED), F.scalar("h", F.FLOAT, 2.0),
                 F.vector("sv", [1, 2, 3, 4], flags=F.SHARED)])
    b = F.build([F.scalar("h", F.FLOAT, 5.0), F.scalar("g", F.FLOAT, 9.0, flags=F.SHARED)])
    bad = F.build([F.vector("g", [0, 0, 0, 0], flags=F.SHARED)])
    return a, b, bad


def test_fx_params(selftest, tmp_path):
    check(selftest, "fx_params", tmp_path, write(tmp_path, "params.fxo", params_effect()))


def test_fx_matrix(selftest, tmp_path):
    check(selftest, "fx_matrix", tmp_path, write(tmp_path, "matrix.fxo", matrix_effect()))


def test_fx_pool(selftest, tmp_path):
    a, b, bad = pool_effects()
    check(selftest, "fx_pool", tmp_path, write(tmp_path, "a.fxo", a), write(tmp_path, "b.fxo", b),
          write(tmp_path, "bad.fxo", bad))


def test_fx_parse_errors(selftest, tmp_path):
    r = check(selftest, "fx_parse_errors", tmp_path, write(tmp_path, "params.fxo", params_effect()))
    print(r.stdout[-1000:])


def test_fx_text_is_later(selftest, tmp_path):
    r = run_group(selftest, "fx_text", tmp_path)
    assert r.returncode == 4 and "fx_text: returned" not in r.stdout, r.stdout[-2000:] + r.stderr[-2000:]
    assert "reached D3DXCreateEffect (HLSL source): owned by a later plan" in r.stderr, r.stderr[-2000:]


# ---- the game's effects ----

MEASURED_PASS_OPS = {0, 1, 3, 4, 6, 7, 8, 9, 10, 11, 13, 14, 22, 26, 30, 48, 59, 73, 75, 103, 105, 107, 109, 116,
                     119, 123, 124, 125, 146, 147, 164}


@pytest.mark.gamefiles
def test_fx_game_parse(selftest, tmp_path):
    # The 59 Data/effects/*.sdv files: each holds the effect's HLSL text, then the fx_2_0 binary (found
    # by its first dword), which runs to the end of the file.
    d = tmp_path / "fx"
    d.mkdir()
    files = sorted((paths.GAMEFILES / "game" / "Data" / "effects").glob("*.sdv"))
    assert len(files) == 59
    for p in files:
        b = p.read_bytes()
        (d / (p.stem + ".fxo")).write_bytes(b[b.find(b"\x01\x09\xff\xfe"):])
    r = check(selftest, "fx_game_parse", tmp_path, d)
    lines = r.stdout.splitlines()
    print("\n".join(ln for ln in lines if not ln.endswith(".fxo")))
    assert "parsed 59 created 59" in lines, r.stdout[-3000:]
    # Measured: 2,645 parameters (322 flagged shared), 165 techniques and 172 passes.
    assert "totals: params 2645 techniques 165 passes 172 shared 322" in lines, r.stdout[-3000:]
    ops = next(ln for ln in lines if ln.startswith("pass ops:"))
    assert set(map(int, ops.split(":")[1].split())) == MEASURED_PASS_OPS, ops


# ---- the vtable against the game's calls ----

# D3DX9 (d3dx9_25) ID3DXEffect methods: stack arguments including this.
ARITY = {
    "QueryInterface": 3, "AddRef": 1, "Release": 1, "GetDesc": 2, "GetParameterDesc": 3, "GetTechniqueDesc": 3,
    "GetPassDesc": 3, "GetFunctionDesc": 3, "GetParameter": 3, "GetParameterByName": 3,
    "GetParameterBySemantic": 3, "GetParameterElement": 3, "GetTechnique": 2, "GetTechniqueByName": 2,
    "GetPass": 3, "GetPassByName": 3, "GetFunction": 2, "GetFunctionByName": 2, "GetAnnotation": 3,
    "GetAnnotationByName": 3, "SetValue": 4, "GetValue": 4, "SetBool": 3, "GetBool": 3, "SetBoolArray": 4,
    "GetBoolArray": 4, "SetInt": 3, "GetInt": 3, "SetIntArray": 4, "GetIntArray": 4, "SetFloat": 3,
    "GetFloat": 3, "SetFloatArray": 4, "GetFloatArray": 4, "SetVector": 3, "GetVector": 3,
    "SetVectorArray": 4, "GetVectorArray": 4, "SetMatrix": 3, "GetMatrix": 3, "SetMatrixArray": 4,
    "GetMatrixArray": 4, "SetMatrixPointerArray": 4, "GetMatrixPointerArray": 4, "SetMatrixTranspose": 3,
    "GetMatrixTranspose": 3, "SetMatrixTransposeArray": 4, "GetMatrixTransposeArray": 4,
    "SetMatrixTransposePointerArray": 4, "GetMatrixTransposePointerArray": 4, "SetString": 3, "GetString": 3,
    "SetTexture": 3, "GetTexture": 3, "GetPixelShader": 3, "GetVertexShader": 3, "SetArrayRange": 4,
    "GetPool": 2, "SetTechnique": 2, "GetCurrentTechnique": 1, "ValidateTechnique": 2,
    "FindNextValidTechnique": 3, "IsParameterUsed": 3, "Begin": 3, "BeginPass": 2, "CommitChanges": 1,
    "EndPass": 1, "End": 1, "GetDevice": 2, "OnLostDevice": 1, "OnResetDevice": 1, "SetStateManager": 2,
    "GetStateManager": 2, "BeginParameterBlock": 1, "EndParameterBlock": 1, "ApplyParameterBlock": 2,
    "CloneEffect": 3,
}


def effect_methods():
    """ID3DXEffect's methods in vtable order, as runtime/bw2/d3d9/d3d9_methods.c lists them."""
    src = (BW2RT / "d3d9" / "d3d9_methods.c").read_text()
    macros = {}
    for m in re.finditer(r"#define (\w+) ((?:.*\\\n)*.*)", src):
        macros[m.group(1)] = m.group(2).replace("\\\n", " ")
    body = re.search(r"m_effect\[\] = \{(.*?)\};", src, re.S).group(1)

    def expand(text):
        out = []
        for tok in re.findall(r'"(\w+)"|(\b[A-Z_]+\b)|\b0\b', text):
            if tok[0]:
                out.append(tok[0])
            elif tok[1] in macros:
                out += expand(macros[tok[1]])
        return out
    return expand(body)


def effect_calls():
    """Each call the game makes through an ID3DXEffect: (function, address, slot, stack arguments).

    The effect pointers: D3DXCreateEffect at 0x97CDB9 and 0x97E1EA stores the effect at this+0x28 of
    the object whose methods are at 0x97C000-0x97F800; at 0x96F4BB it stores it at offset 0 of a
    12-byte holder (effect, fragment linker, buffer), which the renderers reach as [object+0x74].
    A call through such a pointer's vtable (call [vtable+slot*4], the object pushed last) is counted
    with the pushes since the last call, pop or jump, not counting a saved register's first push."""
    import capstone
    from capstone import x86
    exe = paths.GAMEFILES / "game" / "white.exe"
    import pefile
    pe = pefile.PE(str(exe), fast_load=True)
    img, base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    funcs = json.loads((paths.REPO / "bw2" / "catalog" / "white-1.42.functions.json").read_text())["functions"]
    saved = ("ebx", "esi", "edi", "ebp")

    def show(e):
        return "?" if e is None else e[1] if e[0] == "reg" else f"[{show(e[1])}+{e[2]:#x}]"
    calls, seen = [], set()
    for a, end, *_ in funcs:
        if a in seen:
            continue
        seen.add(a)
        regs = {r: ("reg", r) for r in ("ecx",) + saved}
        written, hist, pushed = set(), [], {}
        for i in md.disasm(img[a - base:end - base], a):
            ops = i.operands
            hist.append((i, set(written)))
            if i.mnemonic == "push" and ops[0].type == x86.X86_OP_REG:
                pushed[i.address] = regs.get(i.reg_name(ops[0].reg))
            if i.mnemonic == "call":
                if ops[0].type == x86.X86_OP_MEM and ops[0].mem.base and not ops[0].mem.index:
                    v = regs.get(i.reg_name(ops[0].mem.base))
                    if v and v[0] == "load" and v[2] == 0:
                        obj = show(v[1])
                        ours = obj.endswith("+0x74]+0x0]") or (obj in ("[ecx+0x28]", "[?+0x28]")
                                                              and 0x97C000 <= a < 0x97F800)
                        last = next((j for j, _ in reversed(hist[:-1]) if j.mnemonic in ("push", "call")), None)
                        com = (last is not None and last.mnemonic == "push" and pushed.get(last.address) == v[1])
                        if ours and com:
                            n = 0
                            for j, w in reversed(hist[:-1]):
                                if j.mnemonic in ("call", "ret", "jmp", "pop"):
                                    break
                                if j.mnemonic == "push":
                                    if (j.operands[0].type == x86.X86_OP_REG and j.reg_name(j.operands[0].reg) in saved
                                            and j.reg_name(j.operands[0].reg) not in w):
                                        break
                                    n += 1
                            calls.append((a, i.address, ops[0].mem.disp // 4, n))
                for r in ("eax", "ecx", "edx"):
                    regs[r] = None
                written |= {"eax", "ecx", "edx"}
            elif i.mnemonic == "mov" and len(ops) == 2 and ops[0].type == x86.X86_OP_REG:
                dst = i.reg_name(ops[0].reg)
                src = ops[1]
                if src.type == x86.X86_OP_MEM and not src.mem.index and src.mem.base \
                        and i.reg_name(src.mem.base) != "esp":
                    regs[dst] = ("load", regs.get(i.reg_name(src.mem.base)), src.mem.disp)
                elif src.type == x86.X86_OP_REG:
                    regs[dst] = regs.get(i.reg_name(src.reg))
                else:
                    regs[dst] = None
                written.add(dst)
            elif ops and ops[0].type == x86.X86_OP_REG and i.mnemonic not in ("push", "cmp", "test"):
                regs[i.reg_name(ops[0].reg)] = None
                written.add(i.reg_name(ops[0].reg))
    return calls


@pytest.mark.gamefiles
def test_fx_vtable_against_game(tmp_path):
    methods = effect_methods()
    assert len(methods) == 77 and set(methods) == set(ARITY), methods
    calls = effect_calls()
    per = collections.Counter((slot, n) for _, _, slot, n in calls)
    for (slot, n), k in sorted(per.items()):
        name = methods[slot] if slot < len(methods) else "?"
        print(f"slot {slot:2d} {name:28s} args {n} (wants {ARITY.get(name)}) x{k}")
    bad = [f"0x{at:08X} in sub_{fn:08X}: slot {slot} ({methods[slot] if slot < len(methods) else '?'}) "
           f"with {n} arguments" for fn, at, slot, n in calls
           if slot >= len(methods) or ARITY[methods[slot]] != n]
    assert not bad, "\n".join(bad)
    # Measured: 307 calls through 35 slots, Begin/BeginPass/EndPass/End among them.
    assert len(calls) >= 300, len(calls)
    assert {"Begin", "BeginPass", "EndPass", "End", "SetTechnique", "SetMatrix", "SetTexture"} <= \
        {methods[s] for _, _, s, _ in calls}


# ---- Task 6: expressions, techniques and passes ----

D3DXFX_DONOTSAVESTATE, D3DXFX_DONOTSAVESHADERSTATE, D3DXFX_DONOTSAVESAMPLERSTATE = 1, 2, 4


def assemble(exe, tmp_path, src):
    """Shader tokens from the engine's assembler (the selftest's fx_asm group)."""
    p = tmp_path / "shader.asm"
    p.write_text(src)
    r = check(exe, "fx_asm", tmp_path, p)
    words = next(ln for ln in r.stdout.splitlines() if ln.startswith("tokens:")).split()[1:]
    return b"".join(int(w, 16).to_bytes(4, "little") for w in words)


VS_WVP = "vs_1_1\ndcl_position v0\nm4x4 oPos, v0, c0\n"
PS_C0 = "ps_2_0\nmov oC0, c0\n"
WVP_CONST = F.Const("WorldViewProj", F.RS_FLOAT4, 0, 4, F.MATRIX_COLUMNS_T)
TINT_CONST = F.Const("Tint", F.RS_FLOAT4, 0, 1, F.FLOAT4_T)
VS_OBJ, PS_OBJ = F.P(None, F.VERTEXSHADER, F.OBJECT), F.P(None, F.PIXELSHADER, F.OBJECT)


def i32(v):
    return F.scalar(None, F.INT, v)


def b32(v):
    return F.scalar(None, F.BOOL, v)


# The expressions fx_expr checks, in order, each a VertexShaderConstantF[k] state of one pass: inputs A, B
# and C (float4) in c0, c1 and c2; the selftest computes the same in C.
EXPR_INPUTS = [F.Const("A", F.RS_FLOAT4, 0, 1, F.FLOAT4_T), F.Const("B", F.RS_FLOAT4, 1, 1, F.FLOAT4_T),
               F.Const("C", F.RS_FLOAT4, 2, 1, F.FLOAT4_T)]
A, B, C, T = (F.IN, 0), (F.IN, 4), (F.IN, 8), (F.TEMP, 0)


def o(k=0):
    return (F.OUT, k)


EXPRS = {
    "mad": [F.ins("mul", 4, [A, B, T]), F.ins("add", 4, [T, C, o()])],
    "dot": [F.ins("dot", 4, [A, B, o()])],
    "rcp": [F.ins("rcp", 1, [(F.IN, k), o(k)]) for k in range(4)],
    "rsq": [F.ins("rsq", 1, [(F.IN, 4 + k), o(k)]) for k in range(4)],
    "min": [F.ins("min", 4, [A, B, o()])],
    "max": [F.ins("max", 4, [A, B, o()])],
    "lt": [F.ins("lt", 4, [A, B, o()])],
    "ge": [F.ins("ge", 4, [A, B, o()])],
    "frc": [F.ins("frc", 4, [C, o()])],
    "sin": [F.ins("sin", 1, [(F.IN, k), o(k)]) for k in range(4)],
    "cos": [F.ins("cos", 1, [(F.IN, k), o(k)]) for k in range(4)],
    "neg": [F.ins("neg", 4, [B, o()])],
    "scalar_mul": [F.ins("mul", 4, [(F.IN, 2), C, o()], scalar=True)],
    "literal": [F.ins("add", 1, [(F.LIT, 1), (F.IN, 3), o(0)]), F.ins("mov", 2, [(F.LIT, 0), o(1)])],
    "cmp": [F.ins("cmp", 4, [B, A, C, o()])],
}


def expr_effect():
    states = [F.State(148, F.vector(None, [0, 0, 0, 0]), index=k,
                      blob=F.expr(ins_, EXPR_INPUTS, literals=[0.75, 2.5]))
              for k, ins_ in enumerate(EXPRS.values())]
    return F.build([F.vector("A", [1.5, -2.25, 0.3, 4.0]), F.vector("B", [0.7, 3.1, -1.2, 0.5]),
                    F.vector("C", [-0.25, 0.125, 9.0, -3.5])],
                   [F.Technique("t", [F.Pass("p", states)])])


def test_fx_expr(selftest, tmp_path):
    r = check(selftest, "fx_expr", tmp_path, write(tmp_path, "expr.fxo", expr_effect()), len(EXPRS))
    print(r.stdout[-2000:])


@pytest.mark.gamefiles
def test_fx_expr_game_ops(selftest, tmp_path):
    d = game_binaries(tmp_path)
    r = check(selftest, "fx_expr_game_ops", tmp_path, d)
    ops = next(ln for ln in r.stdout.splitlines() if ln.startswith("ops:"))
    print(r.stdout[-3000:])
    # Measured: the 117 expressions (104 preshaders, 13 state values) use these opcodes.
    assert set(ops.split()[1:]) >= {"mov", "neg", "rcp", "rsq", "min", "max", "lt", "add", "mul", "cmp", "dot"}, ops
    assert "expressions 117" in r.stdout, r.stdout[-2000:]


def pass_states_effect():
    smp = F.P("smp", F.SAMPLER2D, F.OBJECT, value=[F.State(170, i32(3)), F.State(165, i32(3))])
    # fxc writes "VertexShader = NULL" as the number 0 (p0); an empty shader object is NULL too (p1)
    p0 = F.Pass("p0", [F.State(0, i32(1)), F.State(13, b32(0)), F.State(6, i32(2)), F.State(170, i32(1), index=1),
                       F.State(146, i32(0)), F.State(147, i32(0))])
    p1 = F.Pass("p1", [F.State(0, i32(0)), F.State(13, b32(1)), F.State(6, i32(5)), F.State(7, i32(6)),
                       F.State(170, i32(2), index=1), F.State(178, F.P(None, F.SAMPLER2D, F.OBJECT), ref="smp"),
                       F.State(103, i32(2)), F.State(105, i32(0)), F.State(107, i32(2)), F.State(109, i32(0)),
                       F.State(48, b32(0)), F.State(146, VS_OBJ, blob=b""), F.State(147, PS_OBJ, blob=b"")])
    return F.build([smp], [F.Technique("t", [p0, p1])])


def test_fx_pass_states(selftest, tmp_path):
    check(selftest, "fx_pass_states", tmp_path, write(tmp_path, "states.fxo", pass_states_effect()))


def shaders_effect(exe, tmp_path, preshader=False, tint_flags=0):
    vs = F.shader(assemble(exe, tmp_path, VS_WVP), [WVP_CONST])
    if preshader:
        # c0 = Tint * Scale, computed on the CPU: Tint in c0, Scale in c1 of the preshader's inputs
        pre = F.expr([F.ins("mul", 4, [(F.IN, 4), (F.IN, 0), (F.OUT, 0)], scalar=True)],
                     [F.Const("Tint", F.RS_FLOAT4, 0, 1, F.FLOAT4_T), F.Const("Scale", F.RS_FLOAT4, 1, 1, F.FLOAT_T)],
                     prsi=[(0, 1)], version=0x46580201)
        ps = F.shader(assemble(exe, tmp_path, PS_C0), [], pre)
    else:
        ps = F.shader(assemble(exe, tmp_path, PS_C0), [TINT_CONST])
    params = [F.P("WorldViewProj", F.FLOAT, F.MATRIX_ROWS, 4, 4,
                  value=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
              F.vector("Tint", [1, 1, 1, 1], flags=tint_flags), F.scalar("Scale", F.FLOAT, 1.0),
              F.vector("Unused", [0, 0, 0, 0])]
    p = F.Pass("p", [F.State(0, i32(0)), F.State(8, i32(1)), F.State(146, VS_OBJ, blob=vs),
                     F.State(147, PS_OBJ, blob=ps)])
    return F.build(params, [F.Technique("t", [p])])


def test_fx_shaders(selftest, tmp_path):
    check(selftest, "fx_shaders", tmp_path, write(tmp_path, "shaders.fxo", shaders_effect(selftest, tmp_path)))


def test_fx_preshader_commit(selftest, tmp_path):
    check(selftest, "fx_preshader_commit", tmp_path,
          write(tmp_path, "pre.fxo", shaders_effect(selftest, tmp_path, preshader=True)))


def test_fx_pool_commit(selftest, tmp_path):
    # Review Focus: Tint shared through a pool by two effects, set between passes, reaches the other's
    # shader at its CommitChanges; a third effect's unshared Tint stays its own.
    check(selftest, "fx_pool_commit", tmp_path,
          write(tmp_path, "shared.fxo", shaders_effect(selftest, tmp_path, tint_flags=F.SHARED)),
          write(tmp_path, "own.fxo", shaders_effect(selftest, tmp_path)))


def selector_effect(exe, tmp_path):
    """VertexShader = vsa[which]: an array of two shaders picked by an expression; the second shader's
    constant table also names a bool2 (b1-b2) and an int4 (i0)."""
    vs0 = F.shader(assemble(exe, tmp_path, "vs_1_1\ndcl_position v0\nmov oPos, v0\n"), [])
    vs1 = F.shader(assemble(exe, tmp_path, "vs_2_0\ndcl_position v0\nmov oPos, v0\n"),
                   [F.Const("Flags", F.RS_BOOL, 1, 2, (F.VECTOR, F.BOOL, 1, 2, 0, [])),
                    F.Const("Ints", F.RS_INT4, 0, 1, (F.VECTOR, F.INT, 1, 4, 0, []))])
    which = F.expr([F.ins("mov", 1, [(F.IN, 0), (F.OUT, 0)])], [F.Const("which", F.RS_FLOAT4, 0, 1, F.FLOAT_T)])
    return F.build([F.P("vsa", F.VERTEXSHADER, F.OBJECT, elements=2, value=[vs0, vs1]),
                    F.scalar("which", F.INT, 0), F.P("Flags", F.BOOL, F.VECTOR, 1, 2, value=[0, 1]),
                    F.P("Ints", F.INT, F.VECTOR, 1, 4, value=[3, -4, 5, 6])],
                   [F.Technique("t", [F.Pass("p", [F.State(146, VS_OBJ, selector=("vsa", which))])])])


def test_fx_selector(selftest, tmp_path):
    check(selftest, "fx_selector", tmp_path, write(tmp_path, "selector.fxo", selector_effect(selftest, tmp_path)))


def save_restore_effect(exe, tmp_path):
    vs = F.shader(assemble(exe, tmp_path, VS_WVP), [WVP_CONST])
    ps = F.shader(assemble(exe, tmp_path, PS_C0), [TINT_CONST])
    p = F.Pass("p", [F.State(0, i32(0)), F.State(13, b32(1)), F.State(6, i32(5)), F.State(170, i32(2), index=0),
                     F.State(146, VS_OBJ, blob=vs), F.State(147, PS_OBJ, blob=ps)])
    return F.build([F.P("WorldViewProj", F.FLOAT, F.MATRIX_ROWS, 4, 4, value=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                                                                                  0, 0, 0, 1]),
                    F.vector("Tint", [1, 0, 0, 1])], [F.Technique("t", [p])])


def test_fx_save_restore(selftest, tmp_path):
    check(selftest, "fx_save_restore", tmp_path, write(tmp_path, "save.fxo", save_restore_effect(selftest, tmp_path)))


def validate_effect(exe, tmp_path):
    vs11 = F.shader(assemble(exe, tmp_path, VS_WVP), [WVP_CONST])
    vs30 = bytes.fromhex("0003feff") + bytes.fromhex("ffff0000")     # vs_3_0, then the end token
    wvp = F.P("WorldViewProj", F.FLOAT, F.MATRIX_ROWS, 4, 4, value=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
    return F.build([wvp], [
        F.Technique("t0", [F.Pass("p", [F.State(146, VS_OBJ, blob=vs30)])]),
        F.Technique("t1", [F.Pass("p", [F.State(146, VS_OBJ, blob=vs11)])]),
        F.Technique("t2", [F.Pass("p", [F.State(9, i32(99))])]),                  # ZFUNC 99 is not a D3DCMPFUNC
        F.Technique("t3", [F.Pass("p", [F.State(9, i32(4))]), F.Pass("q", [F.State(146, VS_OBJ, blob=vs11)])]),
    ])


def test_fx_validate(selftest, tmp_path):
    check(selftest, "fx_validate", tmp_path, write(tmp_path, "validate.fxo", validate_effect(selftest, tmp_path)))


def game_binaries(tmp_path):
    d = tmp_path / "fx"
    d.mkdir(exist_ok=True)
    files = sorted((paths.GAMEFILES / "game" / "Data" / "effects").glob("*.sdv"))
    assert len(files) == 59
    for p in files:
        b = p.read_bytes()
        (d / (p.stem + ".fxo")).write_bytes(b[b.find(b"\x01\x09\xff\xfe"):])
    return d


@pytest.mark.gamefiles
def test_fx_game_render(selftest, tmp_path):
    r = check(selftest, "fx_game_render", tmp_path, game_binaries(tmp_path))
    lines = r.stdout.splitlines()
    print("\n".join(ln for ln in lines if not ln.endswith(".fxo")))
    tot = next(ln for ln in lines if ln.startswith("render totals:"))
    print(tot)
    # Measured: 165 techniques and 172 passes; every pass of a valid technique is drawn.
    m = re.match(r"render totals: techniques (\d+) valid (\d+) passes (\d+) drawn (\d+)", tot)
    assert m and int(m.group(1)) == 165 and int(m.group(3)) == int(m.group(4)) and int(m.group(2)) > 0, tot
