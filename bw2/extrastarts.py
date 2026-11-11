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
# Native code in the protection VM's sections that only its (encrypted) bytecode
# reaches: each start a boot run reached (reason "boot").
VM_EXTRA = REPO / "bw2" / "catalog" / "white-1.42.vm-extra-starts.json"


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


def vm_entries(exe: Path) -> list[int]:
    """The VM's re-entry stubs (`push imm32; jmp <dispatcher>` in .name0). The VM
    rewrites their push operands as it runs (white.exe 0x242A7D4 writes 0x24507E2)."""
    return [va for va in vm_pattern_starts(exe) if _byte(exe, va) == 0x68]


def _byte(exe: Path, va: int) -> int:
    pe = pefile.PE(str(exe), fast_load=True)
    return pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, 1)[0]


def vm_pattern_starts(exe: Path) -> list[int]:
    """Native code of the protection VM that only its bytecode reaches: every entry
    `push imm32; jmp <dispatcher>` in .name0 (the dispatcher is .name0's first byte;
    phase 0 catalogued the 18 behind stxt774's thunks, there are more), and in
    stxt774 every `mov eax, imm32; ret` and every thunk `jmp <entry>` (the VM leaves
    through them)."""
    pe = pefile.PE(str(exe), fast_load=True)
    base, img = pe.OPTIONAL_HEADER.ImageBase, bytes(pe.get_memory_mapped_image())
    secs = {s.Name.rstrip(b"\0").decode(): s for s in pe.sections}
    out = []
    if ".name0" in secs:
        n0 = secs[".name0"]
        lo, size = n0.VirtualAddress, n0.Misc_VirtualSize
        disp = base + lo
        for o in range(lo, lo + size - 9):
            if img[o] == 0x68 and img[o + 5] == 0xE9:
                rel = int.from_bytes(img[o + 6:o + 10], "little", signed=True)
                if base + o + 10 + rel == disp:
                    out.append(base + o)
    if "stxt774" in secs:
        st = secs["stxt774"]
        lo, size = st.VirtualAddress, st.Misc_VirtualSize
        entries = set(out)
        out += [base + o for o in range(lo, lo + size - 5) if img[o] == 0xB8 and img[o + 5] == 0xC3]
        # thunks `jmp <entry>`: the game calls 18 of them, the VM returns through the rest
        out += [base + o for o in range(lo, lo + size - 4) if img[o] == 0xE9 and
                base + o + 5 + int.from_bytes(img[o + 1:o + 5], "little", signed=True) in entries]
    return sorted(set(out))


def load_extra_starts(path: Path | None = EXTRA) -> list[int]:
    if path is None or not Path(path).is_file():
        return []
    path = Path(path)
    return sorted({int(va) for va, _why in json.loads(path.read_text())["starts"]})


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bw2.extrastarts")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args(argv)
    found = immediate_starts(GAMEFILES / "exe" / "white.exe", REPO / "bw2" / "catalog" / "white-1.42.functions.json")
    print(f"[extra] {len(found)} immediate-referenced starts")
    vm = vm_pattern_starts(GAMEFILES / "exe" / "white.exe")
    print(f"[extra] {len(vm)} VM entries and exits")
    if a.write:
        old = json.loads(VM_EXTRA.read_text())["starts"] if VM_EXTRA.exists() else []
        keep = {va: why for va, why in old if why != "pattern"}
        rows = sorted({**{va: "pattern" for va in vm}, **keep}.items())
        VM_EXTRA.write_text(json.dumps({"starts": [[va, why] for va, why in rows]}, indent=0) + "\n")
        print(f"[extra] wrote {VM_EXTRA.name}: {len(rows)} starts")
        old = json.loads(EXTRA.read_text())["starts"] if EXTRA.exists() else []
        keep = {va: why for va, why in old if why != "immediate"}
        rows = sorted({**{va: "immediate" for va in found}, **keep}.items())
        EXTRA.write_text(json.dumps({"starts": [[va, why] for va, why in rows]}, indent=0) + "\n")
        print(f"[extra] wrote {EXTRA.name}: {len(rows)} starts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
