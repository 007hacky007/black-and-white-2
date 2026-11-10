import pytest

from bw2.is11 import split_overlay
from fixtures import CODE, make_pe


def _entry(name, path, ver, data):
    return b"\0".join(s.encode() for s in (name, path, ver, str(len(data)))) + b"\0" + data


def _setup(tmp_path, entries):
    p = tmp_path / "setup.exe"
    p.write_bytes(make_pe([(".text", b"\xc3", CODE)], {}, 0x1000) + b"".join(entries))
    return p


def test_split_overlay_roundtrip(tmp_path):
    exe = _setup(tmp_path, [_entry("a.cab", "Disk1\\a.cab", "0.0.0.0", b"x" * 5000),
                            _entry("setup.ini", "Disk1\\setup.ini", "0.0.0.0", b"[x]")])
    out = tmp_path / "out"
    got = split_overlay(exe, out)
    assert got == [out / "Disk1" / "a.cab", out / "Disk1" / "setup.ini"]
    assert got[0].read_bytes() == b"x" * 5000
    assert got[1].read_bytes() == b"[x]"


def test_split_overlay_truncated_raises(tmp_path):
    bad = _entry("b.cab", "Disk1\\b.cab", "0.0.0.0", b"y" * 100)[:-10]
    exe = _setup(tmp_path, [_entry("a.cab", "Disk1\\a.cab", "0.0.0.0", b"x" * 50), bad])
    out = tmp_path / "out"
    with pytest.raises(ValueError, match="b.cab"):
        split_overlay(exe, out)
    assert not list(out.rglob("*.part"))
