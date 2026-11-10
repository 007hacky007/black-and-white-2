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
