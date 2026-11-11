import json
import struct

from bw2.extrastarts import immediate_starts, load_extra_starts
from fixtures import CODE, make_pe


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
