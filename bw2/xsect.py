"""References from one PE section into others, and quick disassembly."""
import struct
from dataclasses import dataclass
from pathlib import Path

import capstone

from .pe_survey import _load, _name


@dataclass(frozen=True)
class Ref:
    site: int
    kind: str      # "call" | "jmp" | "ptr"
    target: int


def _ranges(pe, names):
    base = pe.OPTIONAL_HEADER.ImageBase
    return [(base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize,
             s.SizeOfRawData)) for s in pe.sections if _name(s) in names]


def cross_section_refs(exe: Path, from_section: str, to_sections: list[str]) -> list[Ref]:
    """E8/E9 rel32 at every byte offset, plus aligned dwords, that land in `to_sections`.

    The byte-wise branch scan also matches bytes inside longer instructions; it is a
    superset to be checked by disassembly, not a list of real call sites.
    """
    pe = _load(exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    src = next(s for s in pe.sections if _name(s) == from_section)
    va0, data = base + src.VirtualAddress, src.get_data()
    ranges = _ranges(pe, set(to_sections))
    inside = lambda v: any(lo <= v < hi for lo, hi in ranges)
    out = []
    for i in range(len(data) - 4):
        op = data[i]
        if op in (0xE8, 0xE9):
            tgt = (va0 + i + 5 + struct.unpack_from("<i", data, i + 1)[0]) & 0xFFFFFFFF
            if inside(tgt):
                out.append(Ref(va0 + i, "call" if op == 0xE8 else "jmp", tgt))
        if i % 4 == 0:
            v = struct.unpack_from("<I", data, i)[0]
            if inside(v):
                out.append(Ref(va0 + i, "ptr", v))
    return sorted(out, key=lambda r: (r.site, r.kind))


def disasm_at(exe: Path, va: int, max_insns: int = 40) -> list[str]:
    pe = _load(exe)
    rva = va - pe.OPTIONAL_HEADER.ImageBase
    code = pe.get_data(rva, max_insns * 15)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    out = []
    for ins in md.disasm(code, va):
        out.append(f"0x{ins.address:x}: {ins.mnemonic} {ins.op_str}")
        if len(out) >= max_insns or ins.mnemonic in ("ret", "retf", "jmp"):
            break
    return out
