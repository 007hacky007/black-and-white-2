import struct

import pytest

from bw2.pe32emu import Pe32Host
from bw2.w32files import install_files
from fixtures import CODE, make_pe

INVALID = 0xFFFFFFFF


@pytest.fixture
def env(tmp_path):
    tree, patch = tmp_path / "tree", tmp_path / "patch"
    tree.mkdir()
    patch.mkdir()
    h = Pe32Host(make_pe([(".text", b"\xc3", CODE)], {}, 0x1000))
    fs = install_files(h, {"T": tree, "P": patch}, writable={"T"})
    scratch = h.virtual_alloc(0x10000)

    class E:
        pass
    e = E()
    e.h, e.fs, e.tree, e.patch, e.tmp = h, fs, tree, patch, tmp_path
    e.next = scratch

    def s(text, wide=True):
        a = e.next
        e.next += 1024
        h.put_str(a, text, wide)
        return a

    def buf(n=1024):
        a = e.next
        e.next += (n + 15) & ~15
        return a

    def api(name, *args):
        return h.api[name][1](*args)
    e.s, e.buf, e.api = s, buf, api
    return e


def test_create_read_write_roundtrip(env):
    h = env.api("CreateFileW", env.s("T:\\a.bin"), 0xC0000000, 0, 0, 2, 0x80, 0)
    assert h != INVALID
    data = env.buf()
    env.h.write(data, b"hello world")
    n = env.buf(4)
    assert env.api("WriteFile", h, data, 11, n, 0) == 1 and env.h.u32(n) == 11
    assert env.api("SetFilePointer", h, 6, 0, 0) == 6
    out = env.buf()
    assert env.api("ReadFile", h, out, 100, n, 0) == 1 and env.h.u32(n) == 5
    assert env.h.read(out, 5) == b"world"
    assert env.api("CloseHandle", h) == 1
    assert (env.tree / "a.bin").read_bytes() == b"hello world"


def test_case_insensitive_resolve(env):
    (env.tree / "Data").mkdir()
    (env.tree / "Data" / "x.txt").write_bytes(b"abc")
    h = env.api("CreateFileW", env.s("T:\\DATA\\X.TXT"), 0x80000000, 1, 0, 3, 0, 0)
    assert h != INVALID
    assert env.api("GetFileSize", h, 0) == 3
    assert env.api("GetFileAttributesA", env.s("t:\\data", wide=False)) == 0x10


def test_path_outside_roots_refused(env):
    for path, access, disp in (("T:\\..\\escape.txt", 0x40000000, 2),
                               ("C:\\etc\\passwd", 0x80000000, 3),
                               ("P:\\new.txt", 0x40000000, 2)):
        assert env.api("CreateFileW", env.s(path), access, 0, 0, disp, 0, 0) == INVALID, path
        assert env.h.last_error in (3, 5), path
    assert not (env.tmp / "escape.txt").exists()
    assert not (env.patch / "new.txt").exists()
    assert len(env.fs.refused) == 3


def test_set_file_pointer_64bit(env):
    (env.tree / "big").write_bytes(b"\0" * 16)
    h = env.api("CreateFileW", env.s("T:\\big"), 0x80000000, 1, 0, 3, 0, 0)
    hi = env.buf(4)
    env.h.w32(hi, 1)
    assert env.api("SetFilePointer", h, 0, hi, 0) == 0
    assert env.h.u32(hi) == 1                     # position 4 GB, high half written back
    env.h.w32(hi, 0)
    assert env.api("SetFilePointer", h, 8, hi, 0) == 8 and env.h.u32(hi) == 0


def test_writable_mapping_flushed_on_close_without_unmap(env):
    f = env.api("CreateFileW", env.s("T:\\new.sdv"), 0xC0000000, 0, 0, 4, 0, 0)
    m = env.api("CreateFileMappingA", f, 0, 4, 0, 0x9B4C, 0)
    view = env.api("MapViewOfFile", m, 2, 0, 0, 0)
    env.h.write(view, bytes(range(256)) * (0x9B4C // 256) + b"\xAA" * (0x9B4C % 256))
    assert env.api("CloseHandle", m) == 1          # patchw32 never unmaps
    assert env.api("SetFilePointer", f, 0x9B4C, 0, 0) == 0x9B4C
    assert env.api("SetEndOfFile", f) == 1
    assert env.api("CloseHandle", f) == 1
    data = (env.tree / "new.sdv").read_bytes()
    assert len(data) == 0x9B4C and data[:256] == bytes(range(256)) and data[-1] == 0xAA


def test_find_first_next_lists_with_sizes(env):
    (env.tree / "a.sdv").write_bytes(b"1")
    (env.tree / "B.SDV").write_bytes(b"22")
    (env.tree / "c.txt").write_bytes(b"333")
    rec = env.buf(600)
    h = env.api("FindFirstFileW", env.s("T:\\*.sdv"), rec)
    seen = []
    while True:
        size = struct.unpack("<I", env.h.read(rec + 32, 4))[0]
        seen.append((env.h.cstr(rec + 44, wide=True), size))
        if not env.api("FindNextFileW", h, rec):
            break
    assert env.h.last_error == 18
    assert sorted(seen) == [("B.SDV", 2), ("a.sdv", 1)]
    assert env.api("FindClose", h) == 1


def test_move_replace_flag(env):
    (env.tree / "a").write_bytes(b"A")
    (env.tree / "b").write_bytes(b"B")
    assert env.api("MoveFileExW", env.s("T:\\a"), env.s("T:\\b"), 0) == 0
    assert env.h.last_error == 183
    assert env.api("MoveFileExW", env.s("T:\\a"), env.s("T:\\b"), 1) == 1
    assert (env.tree / "b").read_bytes() == b"A" and not (env.tree / "a").exists()


def test_create_new_and_open_existing_errors(env):
    (env.tree / "x").write_bytes(b"x")
    assert env.api("CreateFileW", env.s("T:\\x"), 0x40000000, 0, 0, 1, 0, 0) == INVALID
    assert env.h.last_error == 80
    assert env.api("CreateFileW", env.s("T:\\nope"), 0x80000000, 0, 0, 3, 0, 0) == INVALID
    assert env.h.last_error == 2
