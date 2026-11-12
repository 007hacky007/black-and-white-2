"""Microsoft's d3dx9_25.dll (April 2005) from disc 1, checked and rebased for the lifter.

The disc's DirectX redistributable carries it in DIRECTX9/APR200~2.CAB. The file is
pinned by size and SHA-256 at every step, and rebased to LIFT_BASE so its lifted code
sits beside the game's image. Nothing here is committed: the outputs live in
gamefiles/d3dx/.
"""
import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import pefile

from . import disc
from .paths import GAMEFILES

CAB_PATH = "DIRECTX9/APR200~2.CAB"
CAB_SIZE = 1079850
CAB_SHA256 = "1fc43223b767ec0121989e2c07b56b48ccdd2895c3f638068d24d7869049214d"
DLL_NAME = "d3dx9_25.dll"
DLL_SIZE = 2337488
DLL_SHA256 = "4c54df27ce84d21b2924e64ff79b13e7876ce85d8e0c9c1d0abd8da73888187a"
LIFT_BASE = 0x08000000
REBASED_SHA256 = "505a12cf1fa5a7cf1162a27085f7e30036d7bae4f1b666b999777b60a55c2428"

TAR = "/usr/bin/tar"
TOOL_ENV = {"PATH": "/usr/bin:/bin"}
REL_ABSOLUTE, REL_HIGHLOW = 0, 3


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def _check(path: Path, name: str, size: int, sha256: str) -> None:
    found = path.stat().st_size
    if found != size:
        raise ValueError(f"{name}: expected size {size}, found {found}")
    digest = _sha256(path)
    if digest != sha256:
        raise ValueError(f"{name}: expected SHA-256 {sha256}, found {digest}")


def extract_dll(disc1: Path, dest: Path) -> Path:
    """Take d3dx9_25.dll out of disc 1's DirectX cabinet into dest/DLL_NAME."""
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    cab_name = CAB_PATH.rsplit("/", 1)[1]
    tmp = Path(tempfile.mkdtemp(prefix="extract-", dir=dest))
    try:
        with disc.open_disc(disc1) as d:
            entry = disc.find(disc.list_files(d), CAB_PATH)
            if entry is None:
                raise ValueError(f"{cab_name}: not on {Path(disc1).name} (looked for {CAB_PATH})")
            cab = tmp / cab_name
            disc.extract(d, entry, cab)
        _check(cab, cab_name, CAB_SIZE, CAB_SHA256)
        subprocess.run([TAR, "-xf", str(cab), "-C", str(tmp), DLL_NAME],
                       env=TOOL_ENV, check=True)
        dll = tmp / DLL_NAME
        _check(dll, DLL_NAME, DLL_SIZE, DLL_SHA256)
        out = dest / DLL_NAME
        dll.replace(out)
        return out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def rebase(src: Path, dst: Path, base: int = LIFT_BASE) -> Path:
    """Apply the base relocations for `base` and set ImageBase; every other byte stays."""
    data = bytearray(Path(src).read_bytes())
    pe = pefile.PE(data=bytes(data), fast_load=True)
    opt = pe.DOS_HEADER.e_lfanew + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x10B:
        raise ValueError(f"{Path(src).name}: OptionalHeader magic 0x{magic:x} is not PE32")
    old_base = struct.unpack_from("<I", data, opt + 28)[0]
    rel_rva, rel_size = struct.unpack_from("<II", data, opt + 96 + 5 * 8)
    delta = (base - old_base) & 0xFFFFFFFF

    def offset(rva: int, what: str) -> int:
        off = pe.get_offset_from_rva(rva)
        if off is None or off < 0 or off + 4 > len(data):
            raise ValueError(f"{what} at RVA 0x{rva:x} is outside the file")
        return off

    if rel_size == 0 and delta:
        raise ValueError(f"{Path(src).name}: no base relocation directory")
    pos, end = rel_rva, rel_rva + rel_size
    while pos < end:
        boff = offset(pos, "relocation block")
        page, size = struct.unpack_from("<II", data, boff)
        if size < 8 or pos + size > end:
            raise ValueError(f"relocation block size {size} at RVA 0x{pos:x}")
        for i in range((size - 8) // 2):
            word = struct.unpack_from("<H", data, boff + 8 + 2 * i)[0]
            kind, rva = word >> 12, page + (word & 0xFFF)
            if kind == REL_ABSOLUTE:
                continue
            if kind != REL_HIGHLOW:
                raise ValueError(f"relocation type {kind} at RVA 0x{rva:x}")
            off = offset(rva, "relocation site")
            v = struct.unpack_from("<I", data, off)[0]
            struct.pack_into("<I", data, off, (v + delta) & 0xFFFFFFFF)
        pos += size
    struct.pack_into("<I", data, opt + 28, base)
    dst = Path(dst)
    part = dst.with_name(dst.name + ".part")
    part.write_bytes(bytes(data))
    part.replace(dst)
    return dst


def _disc1(gamefiles: Path) -> Path:
    root = Path(gamefiles) / "discs"
    images = sorted(p for p in root.rglob("*") if p.suffix.lower() in (".bin", ".iso"))
    return disc.identify_discs(images)[1]


def d3dx_dll(gamefiles: Path = GAMEFILES) -> Path:
    """The rebased d3dx9_25.dll in gamefiles/d3dx/, made from disc 1 when missing."""
    folder = Path(gamefiles) / "d3dx"
    out = folder / f"d3dx9_25.0x{LIFT_BASE:08X}.dll"
    if not out.exists():
        dll = extract_dll(_disc1(gamefiles), folder)
        rebase(dll, out)
        digest = _sha256(out)
        if digest != REBASED_SHA256:
            out.unlink()
            raise ValueError(f"{out.name}: expected SHA-256 {REBASED_SHA256}, "
                             f"found {digest}")
        return out
    digest = _sha256(out)
    if digest != REBASED_SHA256:
        raise ValueError(f"{out}: expected SHA-256 {REBASED_SHA256}, found {digest}")
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.d3dx",
                                 description="Make the rebased d3dx9_25.dll from disc 1.")
    ap.parse_args(argv)
    out = d3dx_dll()
    print(out)
    print(_sha256(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
