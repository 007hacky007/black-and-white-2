import re

import pytest

from bw2 import paths
from fixtures import CODE, make_pe

HDR = paths.REPO / "third_party" / "pcrecomp" / "runtime" / "recomp32" / "recomp_types.h"


def _macro(text: str, name: str) -> str:
    m = re.search(rf"#define {name}\(target_va\) do \{{(.*?)\}} while\(0\)", text, re.S)
    assert m, name
    return m.group(1)


def test_unimplemented_becomes_trap(lifted_c):
    c = lifted_c(make_pe([(".text", b"\xf4\xc3", CODE)], {}, 0x1000), 0x401000)
    assert 'RECOMP_TRAP(0x00401000, "hlt")' in c
    assert "UNIMPLEMENTED" not in c


def test_unresolved_icall_calls_hook():
    text = HDR.read_text()
    for name in ("RECOMP_ICALL", "RECOMP_ITAIL"):
        body = _macro(text, name)
        assert "recomp_unresolved_icall(_va, g_cur_func)" in body, name
        assert "eax = 0" not in body, name
    assert "void recomp_trap(uint32_t va, const char *mnem);" in text
    assert "#define RECOMP_TRAP(va, mnem)" in text


def test_pushf_lifts_without_trap(lifted_c):
    c = lifted_c(make_pe([(".text", b"\x9c\x66\x9c\x66\x9d\x9d\xc3", CODE)], {}, 0x1000), 0x401000)
    assert "RECOMP_TRAP" not in c, c


def test_backedge_emitted(lifted_c):
    # L: dec ecx; jnz L; ret  -- a loop gives other guest threads a turn (RECOMP_BACKEDGE)
    c = lifted_c(make_pe([(".text", b"\x49\x75\xfd\xc3", CODE)], {}, 0x1000), 0x401000)
    assert re.search(r"\{ RECOMP_BACKEDGE\(\); goto L_00401000; \}", c), c
    # test eax, eax; jnz +1; nop; ret  -- a forward branch does not
    c = lifted_c(make_pe([(".text", b"\x85\xc0\x75\x01\x90\xc3", CODE)], {}, 0x1000), 0x401000)
    assert "RECOMP_BACKEDGE" not in c, c
    # L: loop L; ret  and  L: jmp L
    c = lifted_c(make_pe([(".text", b"\xe2\xfe\xc3", CODE)], {}, 0x1000), 0x401000)
    assert "RECOMP_BACKEDGE" in c, c
    c = lifted_c(make_pe([(".text", b"\x90\xeb\xfd", CODE)], {}, 0x1000), 0x401000)
    assert "RECOMP_BACKEDGE" in c, c


def test_loop_and_jecxz_to_a_target_with_no_block_dispatch(lifted_c):
    # Data in .text decoded as code: a loop/jecxz whose target does not decode has no
    # label, so the branch dispatches (loudly, through RECOMP_ITAIL) like a jcc does,
    # instead of an undeclared goto (d3dx9_25.dll 0x08005C6D: loope).
    bad = b"\xff\xff"                                  # not an instruction
    for op in (b"\xe2", b"\xe1", b"\xe0", b"\xe3"):    # loop, loope, loopne, jecxz
        code = op + b"\x02" + b"\xc3\xcc" + bad
        c = lifted_c(make_pe([(".text", code, CODE)], {}, 0x1000), 0x401000)
        assert "goto L_00401004" not in c, c
        assert "RECOMP_ITAIL(0x00401004u); return;" in c, c
    # a loop to a real label still jumps there
    c = lifted_c(make_pe([(".text", b"\x40\xe2\xfd\xc3", CODE)], {}, 0x1000), 0x401000)
    assert "goto L_00401000;" in c, c


def _negated_table_pe(neg_reg: bytes = b"\xd9") -> bytes:
    """0x401000: neg ecx (or another register); jmp dword ptr [ecx*4 + 0x401060]. Three
    slots before the displacement (0x401054, 0x401058, 0x40105C) name the arms 0x401020,
    0x401030 and 0x401040, each `mov eax, imm32; ret`. Slot 0 (0x401060) holds no code
    address. MSVC's memmove dispatches its backward tail so (CopyUnwindDown)."""
    import struct
    text = bytearray(b"\xcc" * 0x100)
    text[0:9] = b"\xf7" + neg_reg + b"\xff\x24\x8d" + struct.pack("<I", 0x401060)
    for k, arm in enumerate((0x20, 0x30, 0x40)):
        text[arm:arm + 6] = b"\xb8" + struct.pack("<I", k + 1) + b"\xc3"
    text[0x54:0x60] = struct.pack("<III", 0x401020, 0x401030, 0x401040)
    text[0x60:0x64] = bytes(4)
    return make_pe([(".text", bytes(text), CODE)], {}, 0x1000)


ARMS = [0x401020, 0x401030, 0x401040]


def _disasm32(pe_bytes, tmp_path):
    from bw2.discover import _pcr_disassembler
    exe = tmp_path / "neg.exe"
    exe.write_bytes(pe_bytes)
    dis, iat = _pcr_disassembler(exe)
    import disasm32
    return disasm32, dis, iat


def test_negated_index_table_arms(tmp_path, lifted_c):
    pe = _negated_table_pe()
    d32, dis, iat = _disasm32(pe, tmp_path)
    neg, jmp = list(dis.disassemble_at(0x401000, max_bytes=9))
    assert (neg.mnemonic, jmp.mnemonic) == ("neg", "jmp")
    assert sorted(dis.jump_table_targets(jmp, negated=True)) == ARMS
    assert not set(dis.jump_table_targets(jmp, negated=False)) & set(ARMS)
    assert d32.negated_index(neg, jmp)
    assert d32.table_entries_before(dis.read_bytes, 0x401060, dis.is_code_address) == \
        [0x401040, 0x401030, 0x401020]
    # neg of another register does not index the table backwards
    d32b, disb, _ = _disasm32(_negated_table_pe(b"\xda"), tmp_path)   # neg edx
    negb, jmpb = list(disb.disassemble_at(0x401000, max_bytes=9))
    assert negb.op_str == "edx" and not d32b.negated_index(negb, jmpb)
    assert d32.negated_index(None, jmp) is False

    func = dis.disassemble_function(0x401000, iat)
    assert set(ARMS) <= set(func.jump_targets) and set(ARMS) <= set(func.blocks)

    # body_instructions (drop_mid_instruction_entries' interior walk) reaches the arms:
    # false entries inside each arm's mov are dropped
    lo, hi = 0x401000, 0x401100
    fns = {0x401000: 0x20, 0x401021: 5, 0x401031: 5, 0x401041: 5}
    d32.drop_mid_instruction_entries(dis.read_bytes, fns, lo, hi, verbose=False)
    assert sorted(fns) == [0x401000]

    # close_dispatch_targets makes each arm past the extent dispatchable
    fns = {0x401000: 9}
    d32.close_dispatch_targets(dis.read_bytes, fns, lo, hi, verbose=False)
    assert set(ARMS) <= set(fns)

    c = lifted_c(pe, 0x401000)
    for arm in ARMS:
        assert f"L_{arm:08X}:" in c, c
        assert f"case 0x{arm:08X}u: goto L_{arm:08X};" in c, c
        assert f"RECOMP_ITAIL(0x{arm:08X}u)" not in c, c


MEMMOVE_ARMS = [0xAD466C + 8 * k for k in range(7)]       # slots 0xAD464C-0xAD4664


@pytest.mark.gamefiles
def test_white_memmove_arms():
    # white.exe's CRT memmove and memcpy: `neg ecx; jmp [ecx*4 + T]` at 0xAD4596 (T = 0xAD4668)
    # and 0xAD88C6 (T = 0xAD8998). Seven code slots lie before T and slot 0 is code too. The
    # eighth slot before T reads 0x00498D00, the alignment bytes before the table, which is a
    # .text address but no arm: the backward read stops at MSVC's 7 (Task 1 ruling).
    from bw2.discover import _pcr_disassembler, load_functions
    dis, iat = _pcr_disassembler(paths.GAMEFILES / "exe" / "white.exe")
    f = dis.disassemble_function(0xAD43D0, iat)
    assert 0xAD468C in f.jump_targets
    assert set(MEMMOVE_ARMS) | {0xAD46AF} <= set(f.jump_targets)
    assert 0x498D00 not in f.jump_targets
    cat = paths.REPO / "bw2" / "catalog" / "white-1.42.functions.json"
    starts = sorted(fn["address"] for fn in load_functions(cat) if fn["address"] <= 0xAD88C6)
    g = dis.disassemble_function(starts[-1], iat)
    assert {0xAD899C + 8 * k for k in range(7)} | {0xAD89DF} <= set(g.jump_targets)
    assert 0x498D00 not in g.jump_targets

    dis, iat = _pcr_disassembler(paths.GAMEFILES / "dll" / "ScriptLibraryR.dll")
    cat = paths.REPO / "bw2" / "catalog" / "scriptlibraryr-1.2.functions.json"
    fs = sorted(fn["address"] for fn in load_functions(cat))
    for site, table in ((0x1003B226, 0x1003B2F8), (0x1003EE66, 0x1003EF38)):
        start = max(a for a in fs if a <= site)
        g = dis.disassemble_function(start, iat)
        want = {table + 4 + 8 * k for k in range(7)} | {table + 0x47}
        assert want <= set(g.jump_targets), hex(site)
