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
import time
from pathlib import Path

import pefile

from . import disc
from .paths import GAMEFILES, REPO

CAB_PATH = "DIRECTX9/APR200~2.CAB"
CAB_SIZE = 1079850
CAB_SHA256 = "1fc43223b767ec0121989e2c07b56b48ccdd2895c3f638068d24d7869049214d"
DLL_NAME = "d3dx9_25.dll"
DLL_SIZE = 2337488
DLL_SHA256 = "4c54df27ce84d21b2924e64ff79b13e7876ce85d8e0c9c1d0abd8da73888187a"
LIFT_BASE = 0x08000000
REBASED_SHA256 = "505a12cf1fa5a7cf1162a27085f7e30036d7bae4f1b666b999777b60a55c2428"

CATALOG = REPO / "bw2" / "catalog" / "d3dx9_25-apr2005.functions.json"
# Function starts discovery misses, at the rebased base. Each entry names the address
# that calls or reads it (an unresolved direct target in the link, or an unresolved
# indirect call into 0x08000000-0x08253000 at run time). Plan 9 Task 4: the compiler's
# first run called 0x0813986E (from sub_0813879A, read from the table at 0x0801B4A8);
# the rest of the batch are the other relocated pointers in .text to code that sits
# after int3 padding and is not in the catalog (each reads from the address named).
EXTRA_SEEDS: list[int] = [
    0x0813986E,  # sub_0813879A calls it, from the table at 0x0801B4A8
    0x080F8C13,  # 0x0800B438
    0x081395D7,  # 0x0801B454
    0x0813967A,  # 0x0801B46C
    0x081396DA,  # 0x0801B478
    0x0813970F,  # 0x0801B484
    0x08139766,  # 0x0801B490
    0x081397E1,  # 0x0801B49C
    0x081398CE,  # 0x0801B4B4
    0x0813995F,  # 0x0801B4CC
    0x08139B8A,  # 0x0801B4E4
    0x08139D30,  # 0x0801B508
    0x08139D9F,  # 0x0801B514
    0x08139DE2,  # 0x0801B520
    0x0814CCCF,  # 0x0801DEEC
    0x0814D4AD,  # 0x0801DF40, 0x08020038
    0x0814E61C,  # 0x0801DF04
    0x0814F27E,  # 0x0801EFBC
    0x08158083,  # 0x0801EFE8
    0x08158B17,  # 0x0801EFC0
    0x0815A46B,  # 0x0801EFD4
    0x0815B4E4,  # 0x0801FFE4
    0x0815D4EA,  # 0x0801FFF0
    0x0816A927,  # 0x0801D8A4, 0x08020B7C
    0x0816A9E5,  # 0x08020B94, 0x08020B9C
    0x081B82BE,  # 0x080230A4, 0x080230C8
    0x081B8511,  # 0x08022EEC, 0x08022EF0
    0x081B85CC,  # 0x08022EDC, 0x08023184
    0x081B900F,  # 0x0802313C
    0x081B91CB,  # 0x08023134, 0x08023190
    0x081B91E9,  # 0x08023148
    0x081B91F8,  # 0x0802314C
    0x081B9D00,  # 0x080232AC
    0x081B9D0C,  # 0x080232C0
    0x081BAAF9,  # 0x080232B8
]

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


def export_seeds(dll: Path) -> list[int]:
    """Every exported function's address: GetProcAddress is the only thing that names one."""
    pe = pefile.PE(str(dll))
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    lo, hi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
    return sorted({base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols
                   if not e.forwarder and lo <= base + e.address < hi})


def catalog(out: Path = CATALOG) -> Path:
    """Discover the rebased DLL's functions into the committed catalog (addresses only)."""
    from .discover import coverage, discover
    dll = d3dx_dll()
    exports = export_seeds(dll)
    t0 = time.time()
    cat = discover(dll, out, extra_seeds=tuple(exports) + tuple(EXTRA_SEEDS),
                   code_sections=(".text",), code_reloc_seeds=True,
                   cfg_aliases=True)
    c = coverage(cat, dll)
    import json
    seeds = json.loads(Path(cat).read_text())["bw2_seeds"]
    print(f"[catalog] {cat.name}: {c.functions} functions, coverage "
          f"{100.0 * c.covered_bytes / c.text_bytes:.2f}% of {c.text_bytes} bytes, "
          f"{time.time() - t0:.0f} s", flush=True)
    print(f"[catalog] seeds: {len(exports)} export, {len(EXTRA_SEEDS)} extra, {seeds}", flush=True)
    return cat


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.d3dx",
                                 description="Make the rebased d3dx9_25.dll from disc 1.")
    ap.add_argument("--catalog", action="store_true",
                    help=f"also discover its functions into {CATALOG.relative_to(REPO)}")
    a = ap.parse_args(argv)
    out = d3dx_dll()
    print(out)
    print(_sha256(out))
    if a.catalog:
        catalog()
    return 0


if __name__ == "__main__":
    sys.exit(main())
