import json
import struct

from bw2 import discover as disc_mod
from bw2.gaps import Gap, code_seeds, gaps
from fixtures import CODE, DATA, make_pe


def _cat(tmp_path, funcs):
    p = tmp_path / "cat.json"
    p.write_text(json.dumps({"functions": [
        {"address": a, "end": e, "entry_kind": "start", "is_thunk": False} for a, e in funcs]}))
    return p


def _exe(tmp_path, text, data=b"\0" * 16):
    p = tmp_path / "x.exe"
    p.write_bytes(make_pe([(".text", text, CODE), (".data", data, DATA)], {}, 0x1000))
    return p


def test_int3_and_align_gaps_classified(tmp_path):
    text = b"\xc3" + b"\xcc" * 7 + b"\xc3" + b"\xcc" * 40 + b"\xc3"
    exe = _exe(tmp_path, text)
    g = gaps(_cat(tmp_path, [(0x401000, 0x401001), (0x401008, 0x401009), (0x401031, 0x401032)]), exe)
    assert Gap(0x401001, 0x401008, "align") in g
    assert Gap(0x401009, 0x401031, "int3") in g


def test_gap_reached_by_call_is_code(tmp_path):
    # f1 at 0x401000: call 0x401010; ret.  Uncovered at 0x401010: xor eax,eax; inc eax; ret
    call = b"\xe8" + struct.pack("<i", 0x401010 - 0x401005)
    text = call + b"\xc3" + b"\xcc" * 10 + b"\x33\xc0\x40\xc3" + b"\xcc" * 12
    g = gaps(_cat(tmp_path, [(0x401000, 0x401006)]), _exe(tmp_path, text))
    code = [x for x in g if x.kind == "code"]
    assert [x.lo for x in code] == [0x401010]
    assert code_seeds(g) == [0x401010]


def test_pointer_table_gap_is_data(tmp_path):
    table = struct.pack("<III", 0x401000, 0x401000, 0x401000)
    text = b"\xc3" + b"\xcc" * 3 + table + b"\xc3"
    g = gaps(_cat(tmp_path, [(0x401000, 0x401001), (0x401010, 0x401011)]), _exe(tmp_path, text))
    assert Gap(0x401004, 0x401010, "data") in g


def test_reseed_adds_function(tmp_path, monkeypatch):
    monkeypatch.setattr(disc_mod, "GAMEFILES", tmp_path / "gf")
    # entry at 0x401000 just returns. 0x401020 (no prologue, no reference) is found
    # only if the reseed loop feeds code_seeds back into discovery.
    text = b"\xc3".ljust(0x20, b"\xcc") + b"\x33\xc0\x40\x40\xc3" + b"\xcc" * 11
    exe = _exe(tmp_path, text)
    monkeypatch.setattr(disc_mod, "code_seeds", lambda g: [0x401020])
    out = disc_mod.discover(exe, tmp_path / "cat.json")
    starts = {f["address"] for f in disc_mod.load_functions(out)}
    assert 0x401020 in starts
    assert json.loads(out.read_text())["bw2_seeds"]["rounds"] >= 2
