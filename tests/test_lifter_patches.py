import re

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
