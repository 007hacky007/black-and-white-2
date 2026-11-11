import datetime
import struct

from bw2.pe32emu import Pe32Host
from bw2.w32base import install_base
from fixtures import CODE, make_pe


def _host(resources=None):
    h = Pe32Host(make_pe([(".text", b"\xc3", CODE)], {}, 0x1000, resources))
    install_base(h, command_line="patch.exe")
    return h, h.virtual_alloc(0x10000)


def api(h, name, *args):
    return h.api[name][1](*args)


def test_wsprintf_formats():
    h, mem = _host()
    fmt, out, argv, s = mem, mem + 0x100, mem + 0x200, mem + 0x300
    h.put_str(fmt, "%s-%05d-%x-%u%%-%c")
    h.put_str(s, "ab")
    for i, v in enumerate((s, 42, 255, 7, ord("Z"))):
        h.w32(argv + 4 * i, v)
    assert api(h, "wvsprintfA", out, fmt, argv) == len("ab-00042-ff-7%-Z")
    assert h.cstr(out) == "ab-00042-ff-7%-Z"
    h.w32(argv, out + 0x80)                         # cdecl form: buf, fmt, args on the stack
    h.w32(argv + 4, fmt)
    for i, v in enumerate((s, -3 & 0xFFFFFFFF, 0x1A, 0, ord("q"))):
        h.w32(argv + 8 + 4 * i, v)
    assert api(h, "wsprintfA", argv) == len("ab--0003-1a-0%-q")
    assert h.cstr(out + 0x80) == "ab--0003-1a-0%-q"


def test_multibyte_roundtrip_cp1252():
    h, mem = _host()
    src, wide, back = mem, mem + 0x100, mem + 0x200
    h.write(src, "café".encode("cp1252") + b"\0")
    assert api(h, "MultiByteToWideChar", 0, 0, src, 0xFFFFFFFF, 0, 0) == 5
    assert api(h, "MultiByteToWideChar", 0, 0, src, 0xFFFFFFFF, wide, 64) == 5
    assert h.cstr(wide, wide=True) == "café"
    assert api(h, "WideCharToMultiByte", 0, 0, wide, 0xFFFFFFFF, back, 64, 0, 0) == 5
    assert h.read(back, 5) == "café".encode("cp1252") + b"\0"
    assert api(h, "MultiByteToWideChar", 0, 0, src, 0xFFFFFFFF, wide, 2) == 0
    assert h.last_error == 122


def test_filetime_systemtime_roundtrip():
    h, mem = _host()
    ft, st, ft2 = mem, mem + 0x20, mem + 0x40
    t = datetime.datetime(2005, 10, 6, 15, 10, 40, 123000)
    h.write(ft, struct.pack("<Q", (t - datetime.datetime(1601, 1, 1)) // datetime.timedelta(microseconds=1) * 10))
    assert api(h, "FileTimeToSystemTime", ft, st) == 1
    assert struct.unpack("<8H", h.read(st, 16)) == (2005, 10, 4, 6, 15, 10, 40, 123)
    assert api(h, "SystemTimeToFileTime", st, ft2) == 1
    assert h.read(ft2, 8) == h.read(ft, 8)
    assert api(h, "FileTimeToLocalFileTime", ft, ft2) == 1 and h.read(ft2, 8) == h.read(ft, 8)


def test_find_resource_reads_resource_directory():
    h, _ = _host({(10, 1): b"payload", (6, 3): b"xy"})
    r = api(h, "FindResourceA", h.base, 1, 10)
    assert r
    p = api(h, "LockResource", api(h, "LoadResource", h.base, r))
    assert h.read(p, 7) == b"payload"
    assert api(h, "FindResourceA", h.base, 9, 10) == 0 and h.last_error == 1814


def test_security_calls_fail_politely():
    h, mem = _host()
    assert api(h, "ImpersonateSelf", 2) == 0 and h.last_error == 120
    assert api(h, "OpenThreadToken", 0xFFFFFFFE, 8, 1, mem) == 0 and h.last_error == 120


def test_process_basics():
    h, mem = _host()
    assert api(h, "GetVersion") == 0x0A280105
    assert h.cstr(api(h, "GetCommandLineA")) == "patch.exe"
    assert api(h, "GetModuleHandleA", 0) == h.base
    m = api(h, "CreateMutexA", 0, 0, 0)
    assert api(h, "WaitForSingleObject", m, 0xFFFFFFFF) == 0
    h.last_error = 5
    assert api(h, "GetLastError") == 5
