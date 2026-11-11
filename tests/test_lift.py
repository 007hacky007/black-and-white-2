import json
import struct

from bw2.lift import lift_module, lift_one
from fixtures import CODE, make_pe


def _module(tmp_path, n_extra=0):
    # f1 @0x401000: call f2; ret.  f2 @0x401006: call 0x401100 (not catalogued); ret.
    f1 = b"\xe8" + struct.pack("<i", 0x401006 - 0x401005) + b"\xc3"
    f2 = b"\xe8" + struct.pack("<i", 0x401100 - 0x40100b) + b"\xc3"
    extra = b"".join(b"\x33\xc0\xc3" for _ in range(n_extra))
    text = (f1 + f2 + extra).ljust(0x100, b"\xcc") + b"\xc3"
    exe = tmp_path / "m.exe"
    exe.write_bytes(make_pe([(".text", text, CODE)], {}, 0x1000))
    funcs = [[0x401000, 0x401006, "start", 0], [0x401006, 0x40100c, "start", 0]]
    funcs += [[0x40100c + 3 * i, 0x40100f + 3 * i, "start", 0] for i in range(n_extra)]
    funcs.append([0x401003, 0x401004, "alias", 0])
    cat = tmp_path / "cat.json"
    cat.write_text(json.dumps({"columns": ["address", "end", "entry_kind", "is_thunk"], "functions": funcs}))
    return exe, cat


def test_lift_module_writes_shards_and_json(tmp_path):
    exe, cat = _module(tmp_path)
    out = tmp_path / "gen"
    m = lift_module("game", exe, cat, out, (".text",))
    c = (out / "game_0000.c").read_text()
    assert "void sub_00401000(void)" in c and "void sub_00401006(void)" in c
    assert c.startswith('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n#include "recomp_funcs.h"\n')
    doc = json.loads((out / "game.json").read_text())
    assert doc["defined"] == ["sub_00401000", "sub_00401006"]
    assert "sub_00401100" in doc["referenced"]
    assert m.functions == 2 and m.shards == 1


def test_lift_is_reproducible(tmp_path):
    exe, cat = _module(tmp_path, n_extra=3)
    a, b = tmp_path / "a", tmp_path / "b"
    lift_module("game", exe, cat, a, (".text",), shard_size=2)
    lift_module("game", exe, cat, b, (".text",), shard_size=2)
    names = sorted(p.name for p in a.iterdir() if p.is_file() and p.suffix in (".c", ".json"))
    assert names == sorted(p.name for p in b.iterdir() if p.is_file() and p.suffix in (".c", ".json"))
    for n in names:
        assert (a / n).read_bytes() == (b / n).read_bytes(), n


def test_shard_size_respected(tmp_path):
    exe, cat = _module(tmp_path, n_extra=1)
    m = lift_module("game", exe, cat, tmp_path / "gen", (".text",), shard_size=2)
    assert m.functions == 3 and m.shards == 2


def test_interrupted_shard_not_reused(tmp_path):
    exe, cat = _module(tmp_path)
    out = tmp_path / "gen"
    out.mkdir()
    (out / "game_0000.c.part").write_text("garbage")
    (out / "game_0007.c").write_text("stale shard from a bigger earlier run")
    lift_module("game", exe, cat, out, (".text",))
    assert not list(out.glob("*.part"))
    assert not (out / "game_0007.c").exists()
    assert "garbage" not in (out / "game_0000.c").read_text()


def test_lift_one(tmp_path):
    exe, _ = _module(tmp_path)
    assert "RECOMP_CALL(sub_00401006)" in lift_one(exe, 0x401000)


def _ml(name, defined, referenced):
    from bw2.lift import ModuleLift
    return ModuleLift(name, len(defined), 1, 0, {}, defined, referenced, 0.0)


def test_cross_module_call_resolves(tmp_path):
    from bw2.lift import link_tables
    a = _ml("a", ["sub_00401000"], ["sub_00402000", "sub_00401100"])
    b = _ml("b", ["sub_00402000"], [])
    r = link_tables(tmp_path, [a, b], entry=0x401000)
    stubs = (tmp_path / "recomp_unresolved.c").read_text()
    assert "sub_00402000" not in stubs
    assert r["unresolved"] == ["sub_00401100"]


def test_unresolved_gets_loud_stub(tmp_path):
    from bw2.lift import link_tables
    link_tables(tmp_path, [_ml("a", ["sub_00401000"], ["sub_00401100"])], entry=0x401000)
    stubs = (tmp_path / "recomp_unresolved.c").read_text()
    assert "void sub_00401100(void) { recomp_unresolved_call(0x00401100u); }" in stubs
    assert "void recomp_unresolved_call(uint32_t va);" in (tmp_path / "recomp_funcs.h").read_text()


def test_dispatch_sorted_and_reproducible(tmp_path):
    from bw2.lift import link_tables
    mods = [_ml("b", ["sub_00402000"], []), _ml("a", ["sub_00401000", "sub_00400500"], [])]
    link_tables(tmp_path / "x", mods, entry=0x401000)
    link_tables(tmp_path / "y", list(reversed(mods)), entry=0x401000)
    d = (tmp_path / "x" / "recomp_dispatch.c").read_text()
    assert d.index("0x00400500u") < d.index("0x00401000u") < d.index("0x00402000u")
    assert "const uint32_t bw2_entry_va = 0x00401000u;" in d
    for f in ("recomp_dispatch.c", "recomp_funcs.h", "recomp_unresolved.c", "link.json"):
        assert (tmp_path / "x" / f).read_bytes() == (tmp_path / "y" / f).read_bytes(), f


def test_huge_function_gets_own_cold_shard(tmp_path):
    # white.exe 0xB84D70 lifts to ~152k lines; clang -O1 ran over 28 minutes on it.
    exe, cat = _module(tmp_path)
    out = tmp_path / "gen"
    m = lift_module("game", exe, cat, out, (".text",), cold_lines=0)
    assert sorted(p.name for p in out.glob("game_*.c")) == [
        "game_cold_00401000.c", "game_cold_00401006.c"]
    assert m.cold == ["game_cold_00401000.c", "game_cold_00401006.c"]
    assert "void sub_00401000(void)" in (out / "game_cold_00401000.c").read_text()
    assert m.functions == 2


def test_normal_functions_stay_in_shards(tmp_path):
    exe, cat = _module(tmp_path)
    m = lift_module("game", exe, cat, tmp_path / "gen", (".text",))
    assert m.cold == [] and m.shards == 1


# ---- plan 3 Task 6: C++ EH landing pads -----------------------------------------
# An MSVC 7.1 C++ EH function at 0x401000: its prologue installs a frame whose handler
# stub (0x401180) names FuncInfo (0x401200) with one try block; the catch funclet
# (0x401100) returns the continuation `cont`.
EH_FUNC_ASM = r"""
func:
    push ebp
    mov ebp, esp
    push -1
    push offset handler
    mov eax, dword ptr fs:[0]
    push eax
    mov dword ptr fs:[0], esp
    sub esp, 8
    mov dword ptr [ebp - 4], 0
    call thrower
    jmp cont
cont:
    mov dword ptr [ebp - 4], -1
    mov ecx, dword ptr [ebp - 0x0C]
    mov dword ptr fs:[0], ecx
    mov esp, ebp
    pop ebp
    ret
.org 0x100
catch_funclet:
    mov eax, offset cont
    ret
.org 0x180
handler:
    mov eax, offset funcinfo
    .byte 0xE9
    .long cxx_frame_handler - . - 4
.org 0x1C0
cxx_frame_handler:
    ret
.org 0x200
funcinfo: .long 0x19930520, 1, unwindmap, 1, tryblocks
unwindmap: .long -1, 0
tryblocks: .long 0, 0, 1, 1, handlers
handlers: .long 0, 0, 0, catch_funclet
.org 0x300
thrower:
    ret
"""
EH_FUNCS = [0x401000, 0x401100, 0x401180, 0x4011C0, 0x401300]


def _eh_module(tmp_path, asm=EH_FUNC_ASM, funcs=EH_FUNCS):
    from fixtures import asm32
    code = asm32(asm, base=0x401000)
    exe = tmp_path / "eh.exe"
    exe.write_bytes(make_pe([(".text", code.ljust(0x400, b"\xcc"), CODE)], {}, 0x1000))
    cat = tmp_path / "eh.json"
    cat.write_text(json.dumps({"columns": ["address", "end", "entry_kind", "is_thunk"],
                               "functions": [[f, f + 1, "start", 0] for f in funcs]}))
    return exe, cat


def test_eh_function_gets_landing_pad(tmp_path):
    exe, cat = _eh_module(tmp_path)
    out = tmp_path / "gen"
    m = lift_module("eh", exe, cat, out, (".text",))
    c = (out / "eh_0000.c").read_text()
    body = c[c.index("void sub_00401000(void)"):c.index("void sub_00401100(void)")]
    import re as _re
    cont = int(_re.search(r"case 0x([0-9A-F]{8})u: goto L_\1;", body).group(1), 16)
    assert "BW2_EH_ENTER();" in body
    assert f"L_{cont:08X}:" in body
    assert _re.search(r"(?<!BW2_EH_LEAVE\(\); )\breturn;", body.replace("{ BW2_EH_LEAVE(); return; }", "").replace(
        "default: bw2_eh_bad_resume", "")) is None, body
    assert m.eh_functions == 1 and m.eh_resume_points == 1


def test_plain_function_unchanged(tmp_path):
    exe, cat = _module(tmp_path)
    out = tmp_path / "gen"
    m = lift_module("game", exe, cat, out, (".text",))
    assert "BW2_EH" not in (out / "game_0000.c").read_text()
    assert m.eh_functions == 0


def test_catch_only_continuation_is_lifted(tmp_path):
    # As white.exe 0x86C180: the try path returns by itself, so only the catch funclet
    # reaches the continuation; it must still be lifted into the function.
    asm = EH_FUNC_ASM.replace("""    call thrower
    jmp cont
cont:""", """    call thrower
    mov esp, ebp
    pop ebp
    ret
cont:""")
    exe, cat = _eh_module(tmp_path, asm)
    out = tmp_path / "gen"
    m = lift_module("eh", exe, cat, out, (".text",))
    c = (out / "eh_0000.c").read_text()
    body = c[c.index("void sub_00401000(void)"):c.index("void sub_00401100(void)")]
    import re as _re
    cont = int(_re.search(r"case 0x([0-9A-F]{8})u: goto L_\1;", body).group(1), 16)
    assert f"L_{cont:08X}:" in body and m.eh_resume_points == 1


def test_continuation_inside_a_block_splits_it(tmp_path):
    # As white.exe 0x9726C0: the continuation is reached by falling through, so it is
    # in the middle of a block; it must become a label.
    asm = EH_FUNC_ASM.replace("""    call thrower
    jmp cont
cont:
    mov dword ptr [ebp - 4], -1""", """    call thrower
    nop
cont:
    mov dword ptr [ebp - 4], -1""")
    exe, cat = _eh_module(tmp_path, asm)
    out = tmp_path / "gen"
    lift_module("eh", exe, cat, out, (".text",))
    c = (out / "eh_0000.c").read_text()
    body = c[c.index("void sub_00401000(void)"):c.index("void sub_00401100(void)")]
    import re as _re
    cont = int(_re.search(r"case 0x([0-9A-F]{8})u: goto L_\1;", body).group(1), 16)
    assert f"L_{cont:08X}:" in body


def test_continuation_falling_into_the_body_is_cut(tmp_path):
    # As white.exe 0x972BF0: only the catch reaches the continuation, and it falls
    # into a block the entry walk already has.
    asm = EH_FUNC_ASM.replace("""    call thrower
    jmp cont
cont:
    mov dword ptr [ebp - 4], -1
    mov ecx, dword ptr [ebp - 0x0C]""", """    call thrower
    jmp tail
cont:
    mov dword ptr [ebp - 4], -1
tail:
    mov ecx, dword ptr [ebp - 0x0C]""")
    exe, cat = _eh_module(tmp_path, asm)
    out = tmp_path / "gen"
    lift_module("eh", exe, cat, out, (".text",))
    c = (out / "eh_0000.c").read_text()
    body = c[c.index("void sub_00401000(void)"):c.index("void sub_00401100(void)")]
    import re as _re
    cont = int(_re.search(r"case 0x([0-9A-F]{8})u: goto L_\1;", body).group(1), 16)
    assert f"L_{cont:08X}:" in body
    assert len(_re.findall(r"mov ecx, dword ptr \[ebp - 0xc\]", body)) == 1, body   # the tail once


def test_computed_ret_option(tmp_path):
    # the protection VM leaves through `popal; popfd; ret` to whatever it pushed
    exe, cat = _module(tmp_path)
    out_on, out_off = tmp_path / "on", tmp_path / "off"
    lift_module("game", exe, cat, out_on, (".text",), computed_ret=True)
    lift_module("game", exe, cat, out_off, (".text",))
    assert "RECOMP_RET_JUMP(_rt)" in (out_on / "game_0000.c").read_text()
    assert "RECOMP_RET_JUMP(_rt)" not in (out_off / "game_0000.c").read_text()


def test_runtime_push_reads_its_operand(tmp_path):
    # the protection VM rewrites the `push imm32` of its re-entry stubs; such a push must read
    # its operand from memory when it runs
    exe, cat = _module(tmp_path)
    out = tmp_path / "gen"
    text = (b"\x68" + struct.pack("<I", 0) + b"\x58\xc3").ljust(0x100, b"\xcc")   # push 0; pop eax; ret
    exe.write_bytes(make_pe([(".text", text, CODE)], {}, 0x1000))
    lift_module("game", exe, cat, out, (".text",), runtime_push={0x401000})
    c = (out / "game_0000.c").read_text()
    assert "PUSH32(esp, MEM32(0x00401001u));" in c, c
