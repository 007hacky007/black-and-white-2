import struct

import pytest

from bw2 import paths
from bw2.xsect import Ref, cross_section_refs, disasm_at
from fixtures import CODE, make_pe


def _exe(tmp_path):
    # .text at 0x401000 (0x1000 bytes reserved), stxt774 at 0x402000
    call = b"\xe8" + struct.pack("<i", 0x402000 - (0x401000 + 5))
    ptr = struct.pack("<I", 0x402004)
    text = call + b"\x90\x90\x90" + ptr + b"\xc3"
    stub = b"\x90" * 4 + b"\x55\x8b\xec\x5d\xc3" + b"\xcc" * 7
    p = tmp_path / "x.exe"
    p.write_bytes(make_pe([(".text", text, CODE), ("stxt774", stub, CODE)], {}, 0x1000))
    return p


def test_cross_section_call_found(tmp_path):
    refs = cross_section_refs(_exe(tmp_path), ".text", ["stxt774"])
    assert refs == [Ref(0x401000, "call", 0x402000), Ref(0x401008, "ptr", 0x402004)]


def test_disasm_stops_at_ret(tmp_path):
    lines = disasm_at(_exe(tmp_path), 0x402004)
    assert lines == ["0x402004: push ebp", "0x402005: mov ebp, esp",
                     "0x402007: pop ebp", "0x402008: ret "]


@pytest.mark.gamefiles
def test_real_counts():
    refs = cross_section_refs(paths.GAMEFILES / "exe" / "white.exe", ".text",
                              ["stxt774", ".name0", ".name1"])
    branches = [r for r in refs if r.kind in ("call", "jmp")]
    assert len(branches) == 47
    assert len({r.target for r in branches}) == 19
