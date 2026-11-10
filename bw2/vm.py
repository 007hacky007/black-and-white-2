"""The SafeDisc code-virtualization VM in white.exe, run as shipped (user decision).

Facts from docs/research/safedisc-leftovers.md: the dispatcher at 0x242A000,
its 256-slot handler table at 0x242A885 (the interpreter's code ends there),
and 18 `jmp rel32` thunks in stxt774 that game code calls.
"""
import json
import struct
from pathlib import Path

import pefile

from .xsect import cross_section_refs

VM_DISPATCH = 0x242A000
VM_HANDLER_TABLE = 0x242A885
VM_CODE_END = 0x242A885
VM_SECTIONS = ("stxt774", ".name0")


def read_jump_table(exe: Path, va: int, n: int) -> list[int]:
    pe = pefile.PE(str(exe), fast_load=True)
    data = pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, 4 * n)
    return list(struct.unpack(f"<{n}I", data))


def vm_seeds(exe: Path) -> list[int]:
    pe = pefile.PE(str(exe), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    seeds = {VM_DISPATCH}
    for r in cross_section_refs(exe, ".text", list(VM_SECTIONS)):
        if r.kind != "call":
            continue
        b = pe.get_data(r.target - base, 5)
        if len(b) == 5 and b[0] == 0xE9:
            seeds.add(r.target)
            seeds.add((r.target + 5 + struct.unpack_from("<i", b, 1)[0]) & 0xFFFFFFFF)
    if VM_HANDLER_TABLE is not None:
        seeds |= {t for t in read_jump_table(exe, VM_HANDLER_TABLE, 256)
                  if VM_DISPATCH <= t < VM_CODE_END}
    return sorted(seeds)


def vm_catalog(exe: Path, out: Path) -> Path:
    """Discovery over the VM sections, keeping only the interpreter and its entry points."""
    from .discover import discover
    seeds = vm_seeds(exe)
    out = discover(exe, out, extra_seeds=tuple(seeds), code_sections=VM_SECTIONS)
    doc = json.loads(Path(out).read_text())
    cols = doc["columns"]
    keep = set(seeds)
    rows = [r for r in doc["functions"]
            if r[cols.index("address")] in keep or VM_DISPATCH <= r[cols.index("address")] < VM_CODE_END]
    doc["functions"] = rows
    doc["stats"] = {"functions": len(rows)}
    Path(out).write_text(json.dumps(doc, separators=(",", ":")))
    return Path(out)
