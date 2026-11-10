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
    assert c.startswith('#include "recomp_types.h"\n#include "recomp_funcs.h"\n')
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
