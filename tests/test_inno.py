import hashlib
import lzma
import random

import pytest

from bw2 import paths
from bw2.download import FingerprintMismatch
from bw2.inno import call_decode, read_fan_exe
from bw2.releases import load_releases


def _release(kind):
    return next(r for r in load_releases() if r.kind == kind)


def _encode(data: bytes, block: int = 0x10000) -> bytes:
    """Inno Setup's encode direction: the decode loop, but rel += addr after the flip check."""
    b = bytearray(data)
    for base in range(0, len(b), block):
        size = min(block, len(b) - base)
        i = 0
        while i < size - 4:
            p = base + i
            if b[p] in (0xE8, 0xE9):
                i += 1
                p += 1
                if b[p + 3] in (0x00, 0xFF):
                    addr = (base + i + 4) & 0xFFFFFF
                    rel = b[p] | b[p + 1] << 8 | b[p + 2] << 16
                    if rel & 0x800000:
                        b[p + 3] ^= 0xFF
                    rel = (rel + addr) & 0xFFFFFF
                    b[p:p + 3] = rel.to_bytes(3, "little")
                i += 4
            else:
                i += 1
    return bytes(b)


def test_call_decode_vectors():
    assert call_decode(b"\xE8\x05\x00\x00\x00") == b"\xE8\x00\x00\x00\x00"
    assert call_decode(b"\xE8\x00\x00\x00\x00") == b"\xE8\xFB\xFF\xFF\xFF"
    two = b"\xE8\x12\x34\x56\x7F" + b"\xE8\x05\x00\x00\x00"
    assert call_decode(two) == b"\xE8\x12\x34\x56\x7F" + b"\xE8\xFB\xFF\xFF\xFF"
    # An instruction whose operand crosses the block's end is left alone; the next
    # block's addresses count from its own offset in the file.
    blk = bytearray(0x10000 + 5)
    blk[0xFFFC:0x10000] = b"\xE8\x05\x00\x00"
    blk[0x10000:0x10005] = b"\xE8\x05\x00\x00\x00"
    out = call_decode(bytes(blk))
    assert out[0xFFFC:0x10000] == b"\xE8\x05\x00\x00"
    rel = (5 - (0x10000 + 5)) & 0xFFFFFF
    assert out[0x10000:0x10005] == b"\xE8" + rel.to_bytes(3, "little") + b"\xFF"
    assert out[:0xFFFC] == bytes(0xFFFC)


def test_call_decode_roundtrip():
    rnd = random.Random(1342)
    x = bytearray(rnd.randbytes(300_000))
    for _ in range(3000):
        p = rnd.randrange(len(x) - 5)
        x[p] = rnd.choice((0xE8, 0xE9))
        x[p + 4] = rnd.choice((0x00, 0xFF))
    for p in (0xFFFB, 0xFFFC, 0xFFFD, 0x1FFFF, 0x20000, len(x) - 5, len(x) - 4):
        x[p] = 0xE8
    x = bytes(x)
    enc = _encode(x)
    assert enc != x
    assert call_decode(enc) == x


@pytest.mark.gamefiles
def test_read_fan_exe():
    inst = paths.GAMEFILES / "patches" / _release("fanpatch-installer").name
    exe = read_fan_exe(inst)
    want = _release("white-exe")
    assert len(exe) == want.size
    assert hashlib.sha256(exe).hexdigest() == want.sha256


def test_read_fan_exe_refuses(tmp_path):
    small = tmp_path / "small.exe"
    small.write_bytes(bytes(1024))
    with pytest.raises(FingerprintMismatch, match="installer") as e:
        read_fan_exe(small)
    assert "small.exe" in str(e.value)


@pytest.mark.gamefiles
def test_read_fan_exe_refuses_the_exe():
    with pytest.raises(FingerprintMismatch, match="installer"):
        read_fan_exe(paths.GAMEFILES / "exe" / "white.exe")


def test_read_fan_exe_names_each_stage(tmp_path, monkeypatch):
    import bw2.inno as inno
    monkeypatch.setattr(inno, "identify", lambda path, releases: releases[0])
    f = tmp_path / "fake.exe"
    f.write_bytes(bytes(inno.FAN_CHUNK + 64))
    with pytest.raises(FingerprintMismatch, match="fake.exe: magic"):
        read_fan_exe(f)
    f.write_bytes(bytes(inno.FAN_CHUNK) + inno.CHUNK_MAGIC + b"\x10" + b"\xff" * 64)
    with pytest.raises(FingerprintMismatch, match="fake.exe: stream"):
        read_fan_exe(f)
    small = lzma.compress(b"MZ" + bytes(100), format=lzma.FORMAT_RAW,
                          filters=[{"id": lzma.FILTER_LZMA2, "dict_size": 1 << 16}])
    f.write_bytes(bytes(inno.FAN_CHUNK) + inno.CHUNK_MAGIC + b"\x0a" + small)
    with pytest.raises(FingerprintMismatch, match="fake.exe: stream"):
        read_fan_exe(f)
    monkeypatch.setattr(inno, "FAN_EXE_OFFSET", 0)
    monkeypatch.setattr(inno, "FAN_EXE_SIZE", 102)
    with pytest.raises(FingerprintMismatch, match="fake.exe: exe"):
        read_fan_exe(f)
