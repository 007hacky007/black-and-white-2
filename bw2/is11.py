"""InstallShield 10/11 setup launchers: the files appended after the PE image.

Each entry is four NUL-terminated strings (name, path such as `Disk1\\data1.cab`,
version, decimal size) followed by that many bytes.
"""
import struct
from pathlib import Path, PureWindowsPath


def _image_end(d: bytes) -> int:
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    sh = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0]
    return max(sum(struct.unpack_from("<II", d, sh + i * 40 + 16)) for i in range(nsec))


def split_overlay(setup_exe: Path, out: Path) -> list[Path]:
    d = Path(setup_exe).read_bytes()
    p = _image_end(d)
    out = Path(out)
    written = []
    while p < len(d):
        fields = []
        for _ in range(4):
            e = d.find(b"\0", p)
            if e < 0:
                raise ValueError(f"{setup_exe}: entry header cut off at offset {p}")
            fields.append(d[p:e].decode("latin-1"))
            p = e + 1
        name, path, _ver, size = fields
        size = int(size)
        if p + size > len(d):
            raise ValueError(f"{setup_exe}: entry {name} ({path}) needs {size} bytes, "
                             f"{len(d) - p} remain")
        target = out.joinpath(*PureWindowsPath(path).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        part = target.with_name(target.name + ".part")
        part.write_bytes(d[p:p + size])
        part.rename(target)
        written.append(target)
        p += size
    return written
