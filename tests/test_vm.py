import struct

import pytest

from bw2 import paths
from bw2.vm import read_jump_table, vm_seeds
from fixtures import CODE, make_pe


def test_read_jump_table(tmp_path):
    table = struct.pack("<4I", 0x401010, 0x401020, 0x401030, 0x401040)
    p = tmp_path / "t.exe"
    p.write_bytes(make_pe([(".text", b"\xc3" * 16 + table, CODE)], {}, 0x1000))
    assert read_jump_table(p, 0x401010, 4) == [0x401010, 0x401020, 0x401030, 0x401040]


def test_vm_seeds_drop_non_thunk_targets(tmp_path, monkeypatch):
    from bw2 import vm
    # .text: call T (a jmp-rel32 thunk in stxt774) ; call J (junk in stxt774) ; ret
    # stxt774 at 0x402000: T = jmp 0x402100 ; J = 0x402010 (not a jmp)
    t_va, j_va, e_va = 0x402000, 0x402010, 0x402100
    c1 = b"\xe8" + struct.pack("<i", t_va - 0x401005)
    c2 = b"\xe8" + struct.pack("<i", j_va - 0x40100a)
    stub = (b"\xe9" + struct.pack("<i", e_va - (t_va + 5))).ljust(0x10, b"\xcc") + b"\x12\x34" * 8
    stub = stub.ljust(0x100, b"\xcc") + b"\x68\x00\x00\x00\x00\xc3"
    p = tmp_path / "v.exe"
    p.write_bytes(make_pe([(".text", c1 + c2 + b"\xc3", CODE), ("stxt774", stub, CODE)], {}, 0x1000))
    monkeypatch.setattr(vm, "VM_DISPATCH", 0x402100)
    monkeypatch.setattr(vm, "VM_HANDLER_TABLE", None)
    seeds = vm_seeds(p)
    assert t_va in seeds and e_va in seeds
    assert j_va not in seeds


@pytest.mark.gamefiles
def test_real_vm_seeds():
    seeds = vm_seeds(paths.GAMEFILES / "exe" / "white.exe")
    assert len(seeds) == 228
    assert 0x242A000 in seeds and 0x2428F9C in seeds
