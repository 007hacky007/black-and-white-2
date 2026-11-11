"""Win32 file, directory and file-mapping calls for Pe32Host, confined to given roots.

Guest paths use drive letters mapped to host folders (`{"P": patch_dir, "T": tree}`).
Names resolve case-insensitively, as on Windows. A path outside every root, or a
write to a root that is not writable, fails with a Win32 error and is logged in
`FileSystem.refused`; nothing outside the roots is ever touched.
"""
import fnmatch
import os
import shutil
import struct
from dataclasses import dataclass, field
from pathlib import Path

from .pe32emu import Pe32Host

INVALID = 0xFFFFFFFF
GENERIC_WRITE, FILE_WRITE_DATA = 0x40000000, 0x2
ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_ACCESS_DENIED = 2, 3, 5
ERROR_INVALID_HANDLE, ERROR_NO_MORE_FILES, ERROR_FILE_EXISTS = 6, 18, 80
ERROR_INVALID_PARAMETER, ERROR_NEGATIVE_SEEK, ERROR_DIR_NOT_EMPTY = 87, 131, 145
ERROR_ALREADY_EXISTS = 183
ATTR_DIR, ATTR_NORMAL = 0x10, 0x80
EPOCH_DELTA = 11644473600          # seconds from 1601-01-01 to 1970-01-01


def to_filetime(t: float) -> int:
    return int((t + EPOCH_DELTA) * 10_000_000)


def from_filetime(ft: int) -> float:
    return ft / 10_000_000 - EPOCH_DELTA


@dataclass
class _File:
    f: object
    path: Path
    writable: bool
    closed: bool = False


@dataclass
class _Mapping:
    file: int
    size: int
    writable: bool


@dataclass
class _View:
    mapping: int
    file: int
    offset: int
    size: int
    writable: bool


@dataclass
class FileSystem:
    host: Pe32Host
    roots: dict
    writable: set
    refused: list = field(default_factory=list)
    cwd: str = ""
    views: dict = field(default_factory=dict)

    # ---- paths ----
    def guest_full(self, path: str) -> str:
        p = path.replace("/", "\\")
        if len(p) >= 2 and p[1] == ":":
            drive, rest = p[0].upper(), p[2:]
        else:
            drive, rest = self.cwd[0], (self.cwd[2:] if not p.startswith("\\") else "") + "\\" + p
        parts = []
        for part in rest.split("\\"):
            if part in ("", "."):
                continue
            if part == "..":
                if not parts:
                    return f"{drive}:\\..\\"          # above the root: refused by resolve
                parts.pop()
            else:
                parts.append(part)
        return f"{drive}:\\" + "\\".join(parts)

    def resolve(self, path: str, write: bool = False) -> Path | None:
        full = self.guest_full(path)
        drive = full[0]
        if drive not in self.roots or "\\..\\" in full + "\\":
            return self._refuse(path, ERROR_PATH_NOT_FOUND)
        if write and drive not in self.writable:
            return self._refuse(path, ERROR_ACCESS_DENIED)
        cur = Path(self.roots[drive])
        for part in [x for x in full[3:].split("\\") if x]:
            cand = cur / part
            if not cand.exists() and cur.is_dir():
                low = part.lower()
                match = next((n for n in sorted(os.listdir(cur)) if n.lower() == low), None)
                if match:
                    cand = cur / match
            cur = cand
        return cur

    def _refuse(self, path: str, err: int) -> None:
        self.refused.append(path)
        self.host.last_error = err
        return None

    def _str(self, va: int, wide: bool) -> str:
        return self.host.cstr(va, wide) or ""

    def _fail(self, err: int, ret: int = 0) -> int:
        self.host.last_error = err
        return ret

    # ---- handles ----
    def _file(self, h: int) -> _File | None:
        o = self.host.handles.get(h)
        return o if isinstance(o, _File) and not o.closed else None

    def _flush_view(self, va: int) -> None:
        v = self.views[va]
        fo = self.host.handles.get(v.file)
        if v.writable and isinstance(fo, _File):
            pos = fo.f.tell()
            fo.f.seek(v.offset)
            fo.f.write(self.host.read(va, v.size))
            fo.f.seek(pos)

    def _release_file(self, h: int) -> None:
        fo = self.host.handles.get(h)
        in_use = any(isinstance(o, _Mapping) and o.file == h for o in self.host.handles.values()) \
            or any(v.file == h for v in self.views.values())
        if isinstance(fo, _File) and fo.closed and not in_use:
            fo.f.close()
            del self.host.handles[h]

    def close_handle(self, h: int) -> int:
        o = self.host.handles.get(h)
        if o is None:
            return self._fail(ERROR_INVALID_HANDLE)
        if isinstance(o, _Mapping):
            for va in [va for va, v in self.views.items() if v.mapping == h]:
                self._flush_view(va)
            del self.host.handles[h]
            self._release_file(o.file)
        elif isinstance(o, _File):
            for va in [va for va, v in self.views.items() if v.file == h]:
                self._flush_view(va)
            o.closed = True
            self._release_file(h)
        else:
            del self.host.handles[h]
        return 1

    # ---- files ----
    def create_file(self, path: str, access: int, disp: int) -> int:
        wants_write = bool(access & (GENERIC_WRITE | FILE_WRITE_DATA)) or disp in (1, 2, 5)
        hp = self.resolve(path, write=wants_write or disp == 4)
        if hp is None:
            return INVALID
        exists = hp.exists()
        if exists and hp.is_dir():
            return self._fail(ERROR_ACCESS_DENIED, INVALID)
        if not exists and not hp.parent.is_dir():
            return self._fail(ERROR_PATH_NOT_FOUND, INVALID)
        if disp == 1 and exists:
            return self._fail(ERROR_FILE_EXISTS, INVALID)
        if disp in (3, 5) and not exists:
            return self._fail(ERROR_FILE_NOT_FOUND, INVALID)
        if disp not in (1, 2, 3, 4, 5):
            return self._fail(ERROR_INVALID_PARAMETER, INVALID)
        if disp in (1, 2) or (disp == 4 and not exists):
            f = open(hp, "w+b")
        elif disp == 5:
            f = open(hp, "r+b")
            f.truncate(0)
        else:
            f = open(hp, "r+b" if wants_write else "rb")
        self.host.last_error = ERROR_ALREADY_EXISTS if exists and disp in (2, 4) else 0
        return self.host.new_handle(_File(f, hp, wants_write))

    def read_file(self, h, buf, n, pread, ov):
        fo = self._file(h)
        if fo is None:
            return self._fail(ERROR_INVALID_HANDLE)
        data = fo.f.read(n)
        self.host.write(buf, data)
        if pread:
            self.host.w32(pread, len(data))
        return 1

    def write_file(self, h, buf, n, pwritten, ov):
        fo = self._file(h)
        if fo is None:
            return self._fail(ERROR_INVALID_HANDLE)
        if not fo.writable:
            return self._fail(ERROR_ACCESS_DENIED)
        fo.f.write(self.host.read(buf, n))
        if pwritten:
            self.host.w32(pwritten, n)
        return 1

    def set_file_pointer(self, h, lo, phi, method):
        fo = self._file(h)
        if fo is None:
            return self._fail(ERROR_INVALID_HANDLE, INVALID)
        if phi:
            dist = (struct.unpack("<i", self.host.read(phi, 4))[0] << 32) | lo
        else:
            dist = struct.unpack("<i", struct.pack("<I", lo))[0]
        base = {0: 0, 1: fo.f.tell(), 2: os.fstat(fo.f.fileno()).st_size}.get(method)
        if base is None:
            return self._fail(ERROR_INVALID_PARAMETER, INVALID)
        pos = base + dist
        if pos < 0:
            return self._fail(ERROR_NEGATIVE_SEEK, INVALID)
        fo.f.seek(pos)
        if phi:
            self.host.w32(phi, pos >> 32)
        self.host.last_error = 0
        return pos & 0xFFFFFFFF

    def set_end_of_file(self, h):
        fo = self._file(h)
        if fo is None or not fo.writable:
            return self._fail(ERROR_ACCESS_DENIED)
        fo.f.truncate(fo.f.tell())
        return 1

    def get_file_size(self, h, phi):
        fo = self._file(h)
        if fo is None:
            return self._fail(ERROR_INVALID_HANDLE, INVALID)
        fo.f.flush()
        size = os.fstat(fo.f.fileno()).st_size
        if phi:
            self.host.w32(phi, size >> 32)
        return size & 0xFFFFFFFF

    def set_file_time(self, h, pc, pa, pm):
        fo = self._file(h)
        if fo is None:
            return self._fail(ERROR_INVALID_HANDLE)
        if pm:
            fo.f.flush()
            t = from_filetime(struct.unpack("<Q", self.host.read(pm, 8))[0])
            os.utime(fo.path, (t, t))
        return 1

    # ---- names ----
    def attributes(self, path: str) -> int:
        hp = self.resolve(path)
        if hp is None:
            return INVALID
        if not hp.exists():
            return self._fail(ERROR_FILE_NOT_FOUND, INVALID)
        return ATTR_DIR if hp.is_dir() else ATTR_NORMAL

    def set_attributes(self, path: str) -> int:
        hp = self.resolve(path, write=True)
        if hp is None:
            return 0
        return 1 if hp.exists() else self._fail(ERROR_FILE_NOT_FOUND)

    def delete(self, path: str) -> int:
        hp = self.resolve(path, write=True)
        if hp is None:
            return 0
        if not hp.is_file():
            return self._fail(ERROR_FILE_NOT_FOUND)
        hp.unlink()
        return 1

    def move(self, src: str, dst: str, flags: int) -> int:
        a, b = self.resolve(src, write=True), self.resolve(dst, write=True)
        if a is None or b is None:
            return 0
        if not a.exists():
            return self._fail(ERROR_FILE_NOT_FOUND)
        if b.exists() and not flags & 1:
            return self._fail(ERROR_ALREADY_EXISTS)
        os.replace(a, b)
        return 1

    def mkdir(self, path: str) -> int:
        hp = self.resolve(path, write=True)
        if hp is None:
            return 0
        if hp.exists():
            return self._fail(ERROR_ALREADY_EXISTS)
        if not hp.parent.is_dir():
            return self._fail(ERROR_PATH_NOT_FOUND)
        hp.mkdir()
        return 1

    def rmdir(self, path: str) -> int:
        hp = self.resolve(path, write=True)
        if hp is None:
            return 0
        if not hp.is_dir():
            return self._fail(ERROR_FILE_NOT_FOUND)
        if any(hp.iterdir()):
            return self._fail(ERROR_DIR_NOT_EMPTY)
        hp.rmdir()
        return 1

    # ---- search ----
    def _find_record(self, va: int, path: Path, wide: bool) -> None:
        st = path.stat()
        is_dir = path.is_dir()
        ft = struct.pack("<Q", to_filetime(st.st_mtime))
        rec = struct.pack("<I", ATTR_DIR if is_dir else ATTR_NORMAL) + ft * 3 \
            + struct.pack("<II", 0, 0 if is_dir else st.st_size & 0xFFFFFFFF) + b"\0" * 8
        if is_dir:
            rec = rec[:28] + struct.pack("<II", 0, 0) + rec[36:]
        else:
            rec = rec[:28] + struct.pack("<II", st.st_size >> 32, st.st_size & 0xFFFFFFFF) + rec[36:]
        if wide:
            name = path.name.encode("utf-16-le")[:518].ljust(520, b"\0") + b"\0" * 28
        else:
            name = path.name.encode("cp1252", "replace")[:259].ljust(260, b"\0") + b"\0" * 14
        self.host.write(va, rec + name)

    def find_first(self, pattern: str, rec: int, wide: bool) -> int:
        full = self.guest_full(pattern)
        head, _, pat = full.rpartition("\\")
        d = self.resolve(head + "\\")
        if d is None:
            return INVALID
        if not d.is_dir():
            return self._fail(ERROR_PATH_NOT_FOUND, INVALID)
        pat_l = "*" if pat in ("*.*", "*") else pat.lower()
        names = sorted(n for n in os.listdir(d) if fnmatch.fnmatchcase(n.lower(), pat_l))
        if not names:
            return self._fail(ERROR_FILE_NOT_FOUND, INVALID)
        items = [d / n for n in names]
        self._find_record(rec, items.pop(0), wide)
        return self.host.new_handle(["find", items])

    def find_next(self, h: int, rec: int, wide: bool) -> int:
        o = self.host.handles.get(h)
        if not (isinstance(o, list) and o[0] == "find"):
            return self._fail(ERROR_INVALID_HANDLE)
        if not o[1]:
            return self._fail(ERROR_NO_MORE_FILES)
        self._find_record(rec, o[1].pop(0), wide)
        return 1

    # ---- mapping ----
    def create_mapping(self, h, sa, prot, hi, lo, name) -> int:
        fo = self._file(h)
        if fo is None:
            return self._fail(ERROR_INVALID_HANDLE)
        fo.f.flush()
        size = (hi << 32) | lo or os.fstat(fo.f.fileno()).st_size
        return self.host.new_handle(_Mapping(h, size, prot in (0x04, 0x08, 0x40, 0x80)))

    def map_view(self, hm, access, hi, lo, n) -> int:
        m = self.host.handles.get(hm)
        if not isinstance(m, _Mapping):
            return self._fail(ERROR_INVALID_HANDLE)
        off = (hi << 32) | lo
        n = n or (m.size - off)
        fo = self.host.handles[m.file]
        pos = fo.f.tell()
        fo.f.seek(off)
        data = fo.f.read(n)
        fo.f.seek(pos)
        va = self.host.virtual_alloc(n)
        self.host.write(va, data)
        self.views[va] = _View(hm, m.file, off, n, m.writable and bool(access & 0x2))
        return va

    def unmap_view(self, va) -> int:
        if va not in self.views:
            return self._fail(ERROR_INVALID_PARAMETER)
        self._flush_view(va)
        v = self.views.pop(va)
        self._release_file(v.file)
        return 1

    # ---- directories and volumes ----
    def full_path(self, p, n, buf, pfile, wide) -> int:
        full = self.guest_full(self._str(p, wide))
        if len(full) + 1 > n:
            return len(full) + 1
        self.host.put_str(buf, full, wide)
        if pfile:
            self.host.w32(pfile, buf + (full.rfind("\\") + 1) * (2 if wide else 1))
        return len(full)

    def get_cwd(self, n, buf, wide) -> int:
        if len(self.cwd) + 1 > n:
            return len(self.cwd) + 1
        self.host.put_str(buf, self.cwd, wide)
        return len(self.cwd)

    def set_cwd(self, p, wide) -> int:
        path = self._str(p, wide)
        hp = self.resolve(path)
        if hp is None:
            return 0
        if not hp.is_dir():
            return self._fail(ERROR_PATH_NOT_FOUND)
        self.cwd = self.guest_full(path)
        return 1

    def disk_free(self, root, spc, bps, free, total) -> int:
        letter = (self._str(root, False) or self.cwd)[0].upper()
        if letter not in self.roots:
            return self._fail(ERROR_PATH_NOT_FOUND)
        du = shutil.disk_usage(self.roots[letter])
        for va, v in ((spc, 8), (bps, 512), (free, min(du.free // 4096, 0x7FFFFFFF)),
                      (total, min(du.total // 4096, 0x7FFFFFFF))):
            if va:
                self.host.w32(va, v)
        return 1

    def volume_info(self, root, name, nn, serial, maxlen, flags, fsname, fsn) -> int:
        if name:
            self.host.put_str(name, "BW2")
        if serial:
            self.host.w32(serial, 0x0B2B2B2B)
        if maxlen:
            self.host.w32(maxlen, 255)
        if flags:
            self.host.w32(flags, 0x3)
        if fsname:
            self.host.put_str(fsname, "NTFS")
        return 1


def install_files(host: Pe32Host, roots: dict, writable: set) -> FileSystem:
    roots = {k.upper(): Path(v) for k, v in roots.items()}
    fs = FileSystem(host, roots, {w.upper() for w in writable})
    fs.cwd = f"{sorted(fs.writable or roots)[0]}:\\"
    S = fs._str
    for w, sfx in ((False, "A"), (True, "W")):
        host.api["CreateFile" + sfx] = (7, lambda p, a, sh, sa, d, at, t, w=w: fs.create_file(S(p, w), a, d))
        host.api["GetFileAttributes" + sfx] = (1, lambda p, w=w: fs.attributes(S(p, w)))
        host.api["SetFileAttributes" + sfx] = (2, lambda p, a, w=w: fs.set_attributes(S(p, w)))
        host.api["DeleteFile" + sfx] = (1, lambda p, w=w: fs.delete(S(p, w)))
        host.api["MoveFile" + sfx] = (2, lambda a, b, w=w: fs.move(S(a, w), S(b, w), 0))
        host.api["MoveFileEx" + sfx] = (3, lambda a, b, f, w=w: fs.move(S(a, w), S(b, w), f)
                                        if b else fs._fail(ERROR_INVALID_PARAMETER))
        host.api["CreateDirectory" + sfx] = (2, lambda p, sa, w=w: fs.mkdir(S(p, w)))
        host.api["RemoveDirectory" + sfx] = (1, lambda p, w=w: fs.rmdir(S(p, w)))
        host.api["FindFirstFile" + sfx] = (2, lambda p, r, w=w: fs.find_first(S(p, w), r, w))
        host.api["FindNextFile" + sfx] = (2, lambda h, r, w=w: fs.find_next(h, r, w))
        host.api["GetFullPathName" + sfx] = (4, lambda p, n, b, f, w=w: fs.full_path(p, n, b, f, w))
        host.api["GetCurrentDirectory" + sfx] = (2, lambda n, b, w=w: fs.get_cwd(n, b, w))
        host.api["SetCurrentDirectory" + sfx] = (1, lambda p, w=w: fs.set_cwd(p, w))
    host.api.update({
        "CloseHandle": (1, fs.close_handle),
        "ReadFile": (5, fs.read_file),
        "WriteFile": (5, fs.write_file),
        "SetFilePointer": (4, fs.set_file_pointer),
        "SetEndOfFile": (1, fs.set_end_of_file),
        "FlushFileBuffers": (1, lambda h: 1 if fs._file(h) else fs._fail(ERROR_INVALID_HANDLE)),
        "GetFileSize": (2, fs.get_file_size),
        "GetFileType": (1, lambda h: 1 if fs._file(h) else 0),
        "SetFileTime": (4, fs.set_file_time),
        "FindClose": (1, fs.close_handle),
        "CreateFileMappingA": (6, fs.create_mapping),
        "MapViewOfFile": (5, fs.map_view),
        "UnmapViewOfFile": (1, fs.unmap_view),
        "GetDiskFreeSpaceA": (5, fs.disk_free),
        "GetVolumeInformationA": (8, fs.volume_info),
        "GetLogicalDrives": (0, lambda: sum(1 << (ord(k) - 65) for k in fs.roots)),
        "GetDriveTypeA": (1, lambda p: 3 if p and (S(p, False) or " ")[0].upper() in fs.roots else 1),
    })
    return fs
