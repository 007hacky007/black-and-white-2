"""Function starts the catalog missed, merged into the lift.

Discovery seeds from calls, RTTI, vtables, relocations and data pointers. A
function the game only ever stores as a callback in code, `mov [reg+0x34],
offset f`, is named by a 32-bit immediate and nothing else, and the first boot
found one (white.exe 0x414A80, reached through an object's callback field).

`immediate_starts` finds those: 16-byte aligned addresses right after int3
padding, outside every catalogued function, that some 32-bit value in the code
section names, and whose first instruction is not more padding. Boot runs add
the starts they reach (reason "boot"). The list is committed as addresses only:
    bw2/catalog/white-1.42.extra-starts.json   {"starts": [[va, reason], ...]}

    python -m bw2.extrastarts [--write]
"""
import argparse
import bisect
import json
import sys
from pathlib import Path

import pefile

from .discover import load_functions
from .paths import GAMEFILES, REPO

EXTRA = REPO / "bw2" / "catalog" / "white-1.42.extra-starts.json"


def immediate_starts(exe: Path, catalog: Path, section: str = ".text") -> list[int]:
    pe = pefile.PE(str(exe), fast_load=True)
    base, img = pe.OPTIONAL_HEADER.ImageBase, bytes(pe.get_memory_mapped_image())
    sec = next(s for s in pe.sections if s.Name.rstrip(b"\0").startswith(section.encode()))
    lo, hi = base + sec.VirtualAddress, base + sec.VirtualAddress + sec.Misc_VirtualSize
    funcs = sorted((f["address"], f["end"]) for f in load_functions(catalog))
    starts = [a for a, _ in funcs]
    known = set(starts)

    def covered(va):
        i = bisect.bisect_right(starts, va) - 1
        return i >= 0 and funcs[i][0] <= va < funcs[i][1]

    code = img[sec.VirtualAddress:sec.VirtualAddress + sec.Misc_VirtualSize]
    named = {int.from_bytes(code[o:o + 4], "little") for o in range(len(code) - 3)}
    out = []
    for va in sorted(v for v in named if lo < v < hi and v % 16 == 0 and v not in known):
        o = va - base
        if img[o - 1] == 0xCC and img[o] != 0xCC and not covered(va):
            out.append(va)
    return out


def load_extra_starts(path: Path = EXTRA) -> list[int]:
    path = Path(path)
    if not path.exists():
        return []
    return sorted({int(va) for va, _why in json.loads(path.read_text())["starts"]})


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.extrastarts")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args(argv)
    found = immediate_starts(GAMEFILES / "exe" / "white.exe", REPO / "bw2" / "catalog" / "white-1.42.functions.json")
    print(f"[extra] {len(found)} immediate-referenced starts")
    if a.write:
        old = json.loads(EXTRA.read_text())["starts"] if EXTRA.exists() else []
        keep = {va: why for va, why in old if why != "immediate"}
        rows = sorted({**{va: "immediate" for va in found}, **keep}.items())
        EXTRA.write_text(json.dumps({"starts": [[va, why] for va, why in rows]}, indent=0) + "\n")
        print(f"[extra] wrote {EXTRA.name}: {len(rows)} starts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
