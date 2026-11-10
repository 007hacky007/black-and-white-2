"""Facts about a 32-bit PE: sections, imports, TLS, relocations, RTTI, SEH."""
import argparse
import json
import math
import struct
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path

import pefile

EXEC = 0x20000000


@dataclass
class Section:
    name: str
    va: int
    vsize: int
    raw_size: int
    flags: int
    entropy: float


@dataclass
class Survey:
    image_base: int
    entry: int
    sections: list[Section]
    imports: dict[str, list[str]]
    tls_callbacks: list[int]
    reloc_entries: int
    reloc_pages: list[int]
    rtti_type_descriptors: int
    seh_prologues: int
    raise_exception_imported: bool


def _entropy(b: bytes) -> float:
    if not b:
        return 0.0
    n = len(b)
    return -sum(c / n * math.log2(c / n) for c in Counter(b).values())


def _name(s) -> str:
    return s.Name.split(b"\0")[0].decode("latin-1")


def _load(exe: Path) -> pefile.PE:
    return pefile.PE(str(exe), fast_load=False)


def _exec_sections(pe: pefile.PE):
    base = pe.OPTIONAL_HEADER.ImageBase
    for s in pe.sections:
        if s.Characteristics & EXEC:
            yield base + s.VirtualAddress, s.get_data()


def survey(exe: Path) -> Survey:
    pe = _load(exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    sections = [Section(_name(s), s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData,
                        s.Characteristics, round(_entropy(s.get_data()), 2))
                for s in pe.sections]
    imports: dict[str, list[str]] = {}
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        imports[d.dll.decode("latin-1")] = [
            i.name.decode("latin-1") if i.name else f"#{i.ordinal}" for i in d.imports]
    tls = []
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        p = pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks
        while p:
            v = pe.get_dword_at_rva(p - base)
            if not v:
                break
            tls.append(v)
            p += 4
    pages, entries = [], 0
    for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
        pages.append(blk.struct.VirtualAddress)
        entries += sum(1 for e in blk.entries if e.type != 0)
    rtti = seh = 0
    for s in pe.sections:
        data = s.get_data()
        if s.Characteristics & EXEC:
            seh += data.count(b"\x64\xa1\x00\x00\x00\x00")
        else:
            rtti += data.count(b".?AV") + data.count(b".?AU")
    raise_imp = any("RaiseException" in fns for fns in imports.values())
    return Survey(base, base + pe.OPTIONAL_HEADER.AddressOfEntryPoint, sections, imports,
                  tls, entries, pages, rtti, seh, raise_imp)


def iat_slot(exe: Path, dll: str, func: str) -> int | None:
    pe = _load(exe)
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        if d.dll.decode("latin-1").lower() != dll.lower():
            continue
        for i in d.imports:
            if i.name and i.name.decode("latin-1") == func:
                return i.address
    return None


def iat_callsites(exe: Path, dll: str, func: str) -> list[int]:
    slot = iat_slot(exe, dll, func)
    if slot is None:
        return []
    pe = _load(exe)
    s = struct.pack("<I", slot)
    out = []
    for va, data in _exec_sections(pe):
        for pat in (b"\xff\x15" + s, b"\xff\x25" + s):
            i = data.find(pat)
            while i >= 0:
                out.append(va + i)
                i = data.find(pat, i + 1)
    return sorted(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.pe_survey")
    ap.add_argument("exe", type=Path)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args(argv)
    s = survey(a.exe)
    d = asdict(s)
    if a.json:
        a.json.write_text(json.dumps(d, indent=1))
    else:
        print(json.dumps({k: v for k, v in d.items() if k != "imports"}, indent=1))
        for dll, fns in s.imports.items():
            print(f"{dll}: {len(fns)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
