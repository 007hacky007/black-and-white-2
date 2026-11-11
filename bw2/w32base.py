"""The rest of the Win32 calls patchw32.dll makes: start-up, text, time, resources.

Each call is registered by name; anything not here or in w32files stays
unimplemented and stops the run (Pe32Host raises Win32Unimplemented).
"""
import datetime
import re
import struct

from .pe32emu import Pe32Host

ERROR_INSUFFICIENT_BUFFER, ERROR_CALL_NOT_IMPLEMENTED = 122, 120
ERROR_RESOURCE_TYPE_NOT_FOUND, ERROR_RESOURCE_NAME_NOT_FOUND = 1813, 1814
_EPOCH = datetime.datetime(1601, 1, 1)
_CODEPAGES = {0: "cp1252", 1: "cp437", 3: "cp1252", 437: "cp437", 1252: "cp1252", 65001: "utf-8"}
_FMT = re.compile(r"%([-0#+ ]*)(\d*)(?:\.(\d+))?([hlL]?)([sScdiuxXp%])")


def _signed(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


def wsprintf(h: Pe32Host, fmt: str, argp: int) -> str:
    out, pos = [], 0
    for m in _FMT.finditer(fmt):
        out.append(fmt[pos:m.start()])
        pos = m.end()
        flags, width, _prec, _size, conv = m.groups()
        if conv == "%":
            out.append("%")
            continue
        v = h.u32(argp)
        argp += 4
        if conv in "sS":
            text = h.cstr(v, wide=conv == "S") or ""
        elif conv == "c":
            text = chr(v & 0xFF)
        elif conv in "di":
            text = str(_signed(v))
        elif conv == "u":
            text = str(v)
        else:
            text = format(v, "X" if conv in "Xp" else "x")
            if conv == "p":
                text = text.rjust(8, "0")
        w = int(width or 0)
        if "-" in flags:
            text = text.ljust(w)
        elif "0" in flags and conv not in "sSc":
            neg = text.startswith("-")
            text = ("-" if neg else "") + text.lstrip("-").rjust(w - neg, "0")
        else:
            text = text.rjust(w)
        out.append(text)
    out.append(fmt[pos:])
    return "".join(out)


def install_base(h: Pe32Host, command_line: str = "patch.exe",
                 module_path: str = "P:\\patchw32.dll") -> None:
    strings = h.virtual_alloc(0x1000)
    cmdline, env = strings, strings + 0x800
    h.put_str(cmdline, command_line)
    h.write(env, b"\0\0")

    def put_out(buf: int, n: int, data: bytes, units: int) -> int:
        if n == 0:
            return units
        if units > n:
            h.last_error = ERROR_INSUFFICIENT_BUFFER
            return 0
        h.write(buf, data)
        return units

    def mb2wc(cp, flags, src, n, dst, dn):
        enc = _CODEPAGES.get(cp, "cp1252")
        raw = h.read(src, n) if n != 0xFFFFFFFF else (h.cstr(src) or "").encode("cp1252", "replace") + b"\0"
        if n == 0xFFFFFFFF:
            raw = h.read(src, len(raw))
        text = raw.decode(enc, "replace")
        return put_out(dst, dn, text.encode("utf-16-le"), len(text))

    def wc2mb(cp, flags, src, n, dst, dn, default, used):
        enc = _CODEPAGES.get(cp, "cp1252")
        text = h.read(src, 2 * n).decode("utf-16-le") if n != 0xFFFFFFFF \
            else (h.cstr(src, wide=True) or "") + "\0"
        data = text.encode(enc, "replace")
        if used:
            h.w32(used, 0)
        return put_out(dst, dn, data, len(data))

    def systime(ft_va, st_va):
        ft = struct.unpack("<Q", h.read(ft_va, 8))[0]
        t = _EPOCH + datetime.timedelta(microseconds=ft // 10)
        h.write(st_va, struct.pack("<8H", t.year, t.month, t.isoweekday() % 7, t.day,
                                   t.hour, t.minute, t.second, t.microsecond // 1000))
        return 1

    def filetime(st_va, ft_va):
        y, mo, _wd, d, hh, mi, s, ms = struct.unpack("<8H", h.read(st_va, 16))
        t = datetime.datetime(y, mo, d, hh, mi, s, ms * 1000)
        h.write(ft_va, struct.pack("<Q", (t - _EPOCH) // datetime.timedelta(microseconds=1) * 10))
        return 1

    def copy_filetime(src, dst):
        h.write(dst, h.read(src, 8))
        return 1

    def res_key(v):
        if v <= 0xFFFF:
            return v
        s = (h.cstr(v) or "").upper()
        return int(s[1:]) if s.startswith("#") and s[1:].isdigit() else s

    def find_resource(module, name, typ):
        t, n = res_key(typ), res_key(name)
        for te in getattr(h.pe, "DIRECTORY_ENTRY_RESOURCE", None).entries \
                if hasattr(h.pe, "DIRECTORY_ENTRY_RESOURCE") else []:
            if (te.id if te.name is None else str(te.name).upper()) != t:
                continue
            for ne in te.directory.entries:
                if (ne.id if ne.name is None else str(ne.name).upper()) == n:
                    return h.base + ne.directory.entries[0].data.struct.OffsetToData
            h.last_error = ERROR_RESOURCE_NAME_NOT_FOUND
            return 0
        h.last_error = ERROR_RESOURCE_TYPE_NOT_FOUND
        return 0

    def system_info(va):
        h.write(va, struct.pack("<HHIIIIIIIHH", 0, 0, 4096, 0x10000, 0x7FFEFFFF, 1, 1, 586,
                                0x10000, 6, 0x0F01))

    def module_file(module, buf, n):
        return put_out(buf, n, module_path.encode("cp1252") + b"\0", len(module_path) + 1) and len(module_path)

    def not_implemented(*_):
        h.last_error = ERROR_CALL_NOT_IMPLEMENTED
        return 0

    def last_error():
        return h.last_error

    h.api.update({
        "GetVersion": (0, lambda: 0x0A280105),              # Windows XP, 5.1.2600
        "GetCommandLineA": (0, lambda: cmdline),
        "GetEnvironmentStrings": (0, lambda: env),
        "GetStartupInfoA": (1, lambda va: h.write(va, struct.pack("<I", 68) + b"\0" * 64)),
        "GetStdHandle": (1, lambda n: 0),
        "GetModuleFileNameA": (3, module_file),
        "GetModuleHandleA": (1, lambda name: h.base),
        "GetProcAddress": (2, lambda module, name: 0),
        "GetCurrentProcessId": (0, lambda: 0x1234),
        "GetCurrentThread": (0, lambda: 0xFFFFFFFE),
        "GetSystemInfo": (1, system_info),
        "SetErrorMode": (1, lambda mode: 0),
        "CreateMutexA": (3, lambda sa, owner, name: h.new_handle("mutex")),
        "WaitForSingleObject": (2, lambda handle, ms: 0),
        "GetLastError": (0, last_error),
        "GetACP": (0, lambda: 1252),
        "GetOEMCP": (0, lambda: 437),
        "GetCPInfo": (2, lambda cp, va: (h.write(va, struct.pack("<I", 1) + b"?\0" + b"\0" * 14), 1)[1]),
        "MultiByteToWideChar": (6, mb2wc),
        "WideCharToMultiByte": (8, wc2mb),
        "AreFileApisANSI": (0, lambda: 1),
        "SetFileApisToOEM": (0, lambda: 0),
        "SetFileApisToANSI": (0, lambda: 0),
        "wvsprintfA": (3, lambda buf, fmt, args: h.put_str(buf, wsprintf(h, h.cstr(fmt) or "", args))),
        "wsprintfA": (None, lambda argp: h.put_str(h.u32(argp),
                                                   wsprintf(h, h.cstr(h.u32(argp + 4)) or "", argp + 8))),
        "MulDiv": (3, lambda a, b, c: (_signed(a) * _signed(b) // _signed(c)) if c else 0xFFFFFFFF),
        "FindResourceA": (3, find_resource),
        "LoadResource": (2, lambda module, res: res),
        "LockResource": (1, lambda res: res),
        "FileTimeToLocalFileTime": (2, copy_filetime),
        "LocalFileTimeToFileTime": (2, copy_filetime),
        "FileTimeToSystemTime": (2, systime),
        "SystemTimeToFileTime": (2, filetime),
        "ImpersonateSelf": (1, not_implemented),
        "OpenThreadToken": (4, not_implemented),
    })
