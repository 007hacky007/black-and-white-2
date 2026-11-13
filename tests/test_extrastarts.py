import json
import struct

import pytest

from bw2 import paths
from bw2.extrastarts import data_starts, immediate_starts, load_extra_starts
from fixtures import CODE, DATA, make_pe


def _cat(path, funcs):
    path.write_text(json.dumps({"columns": ["address", "end", "entry_kind", "is_thunk"],
                                "functions": [[a, e, "start", 0] for a, e in funcs]}))
    return path


def test_immediate_referenced_start_after_padding_found(tmp_path):
    # 0x401000: mov dword ptr [eax+0x34], 0x401020; ret   (stores a callback, never calls it)
    # 0x401020 (after int3 padding, 16-aligned): the callback, only named by that immediate
    f0 = b"\xc7\x40\x34" + struct.pack("<I", 0x401020) + b"\xc3"
    text = f0.ljust(0x20, b"\xcc") + b"\x33\xc0\xc3"
    text = text.ljust(0x40, b"\xcc") + b"\xcc" * 16           # an int3 run at 0x401040: not a function
    exe = tmp_path / "m.exe"
    exe.write_bytes(make_pe([(".text", text.ljust(0x100, b"\xcc"), CODE)], {}, 0x1000))
    cat = _cat(tmp_path / "c.json", [(0x401000, 0x401008)])
    assert immediate_starts(exe, cat) == [0x401020]
    cat2 = _cat(tmp_path / "c2.json", [(0x401000, 0x401008), (0x401020, 0x401023)])
    assert immediate_starts(exe, cat2) == []


def test_extra_starts_file(tmp_path):
    p = tmp_path / "x.json"
    p.write_text(json.dumps({"starts": [[0x414A80, "boot"], [0x4042A0, "immediate"]]}))
    assert load_extra_starts(p) == [0x4042A0, 0x414A80]
    assert load_extra_starts(tmp_path / "missing.json") == []
    assert load_extra_starts(None) == []


def test_vm_pattern_starts(tmp_path):
    # .name0 at 0x402000: the dispatcher, then two entries `push imm; jmp dispatcher`;
    # stxt774 at 0x401000: a `mov eax, imm; ret` gadget at +0x10
    from bw2.extrastarts import vm_pattern_starts
    name0 = bytearray(b"\x90\xc3".ljust(0x20, b"\xcc"))
    for off in (0x20, 0x2A):
        name0[off:off + 10] = b"\x68" + struct.pack("<I", 0x1234) + b"\xe9" + struct.pack("<i", 0x402000 - (0x402000 + off + 10))
    # stxt774 also starts with a thunk `jmp <entry 0x402020>` at 0x401000
    stxt = (b"\xe9" + struct.pack("<i", 0x402020 - 0x401005)).ljust(0x10, b"\xcc") + b"\xb8" + struct.pack("<I", 0x401020) + b"\xc3"
    exe = tmp_path / "vm.exe"
    exe.write_bytes(make_pe([("stxt774", stxt.ljust(0x40, b"\xcc"), CODE), (".name0", bytes(name0).ljust(0x40, b"\xcc"), CODE)],
                            {}, 0x1000))
    assert vm_pattern_starts(exe) == [0x401000, 0x401010, 0x402020, 0x40202A]


def test_vm_entries_are_the_push_stubs(tmp_path):
    from bw2.extrastarts import vm_entries
    test_vm_pattern_starts(tmp_path)
    assert vm_entries(tmp_path / "vm.exe") == [0x402020, 0x40202A]


def test_boot_start_8b00b0():
    # the vector-constructor iterator at 0xAD9918 calls 0x8B00B0, which follows a `ret 8` inside the
    # catalogued sub_008B0080 (plan 8, "Measured" item 7): a boot start, kept by --write
    from bw2.extrastarts import EXTRA
    rows = json.loads(EXTRA.read_text())["starts"]
    assert [0x8B00B0, "boot"] in rows
    assert 0x8B00B0 in load_extra_starts()


def _data_named_exe(tmp_path, name, where, offset):
    """.text: a function at 0x401000 whose catalogued extent (to 0x401040) runs across the
    int3 padding at 0x401006-0x40101F into a 16-aligned body at 0x401020, which only a
    dword in a data section names (section `where`, byte `offset` into it)."""
    text = (b"\x33\xc0\xc3".ljust(0x20, b"\xcc") + b"\xb8\x01\x00\x00\x00\xc3").ljust(0x100, b"\xcc")
    secs = {".rdata": bytearray(16), ".data": bytearray(16), "_PDATA": bytearray(16)}
    secs[where][offset:offset + 4] = struct.pack("<I", 0x401020)
    exe = tmp_path / name
    exe.write_bytes(make_pe([(".text", text, CODE)] + [(n, bytes(b), DATA) for n, b in secs.items()],
                            {}, 0x1000))
    return exe


def test_data_named_start_inside_extent(tmp_path):
    cat = _cat(tmp_path / "c.json", [(0x401000, 0x401040)])
    for where in (".data", ".rdata"):
        exe = _data_named_exe(tmp_path, f"d{where}.exe", where, 4)
        assert data_starts(exe, cat) == [0x401020], where
        assert immediate_starts(exe, cat) == []          # covered by the extent: hidden there
    # catalogued already: not an extra start
    exe = _data_named_exe(tmp_path, "d.exe", ".data", 4)
    assert data_starts(exe, _cat(tmp_path / "c2.json", [(0x401000, 0x401040), (0x401020, 0x401026)])) == []
    # an unaligned .data dword, or one in a third non-executable section, names nothing
    assert data_starts(_data_named_exe(tmp_path, "u.exe", ".data", 5), cat) == []
    assert data_starts(_data_named_exe(tmp_path, "p.exe", "_PDATA", 4), cat) == []


WHITE_DATA_STARTS = [0x42FFD0, 0x724310, 0x724870, 0x72B5B0, 0x746700, 0x746C00, 0x7A49E0]


@pytest.mark.gamefiles
def test_white_data_starts():
    import capstone
    import pefile
    from bw2 import d3dx
    cats = paths.REPO / "bw2" / "catalog"
    white = paths.GAMEFILES / "exe" / "white.exe"
    assert data_starts(white, cats / "white-1.42.functions.json") == WHITE_DATA_STARTS
    assert data_starts(paths.GAMEFILES / "dll" / "ScriptLibraryR.dll",
                       cats / "scriptlibraryr-1.2.functions.json") == []
    assert data_starts(d3dx.d3dx_dll(), cats / "d3dx9_25-apr2005.functions.json") == []
    pe = pefile.PE(str(white), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for va in WHITE_DATA_STARTS:
        first = next(md.disasm(pe.get_data(va - base, 16), va), None)
        assert first is not None and first.mnemonic != "int3", hex(va)
    from bw2.extrastarts import EXTRA
    rows = {va: why for va, why in json.loads(EXTRA.read_text())["starts"]}
    assert all(rows.get(va) == "data" for va in WHITE_DATA_STARTS)
    assert set(WHITE_DATA_STARTS) <= set(load_extra_starts())
