"""Clean a disasm32 catalog before lifting.

Two kinds of false entry survive disasm32 on white.exe, and both lift as full
copies of someone else's code:

- Entries that start inside an instruction. disasm32 drops these, but stops
  after a fixed number of rounds; another pass over its output drops 1,301 more.
- Instruction boundaries inside a long body that only a pointer-shaped value in
  .data names. disasm32 keeps those as starts on purpose ("weak" entries), and
  white.exe's 22 MB .data names dozens of points inside the ~500 KB straight-line
  function at 0xB84D70. Each one lifted as its own ~130k-instruction copy.

The second kind becomes an alias, which the lifter skips. An entry stays a start
when something calls it, when it is seeded, or when the code before it ends in a
terminator, a call (which may not return) or padding.
"""
import importlib.util
import json
from functools import lru_cache
from pathlib import Path

import capstone
import pefile

from .paths import REPO

_ENDS = {"ret", "retn", "retf", "iret", "iretd", "jmp", "int3", "hlt", "ud2"}
_PAD = {"nop", "int3"}
_CHUNK = 0x2000


@lru_cache(maxsize=1)
def _disasm32():
    p = REPO / "third_party" / "pcrecomp" / "tools" / "disasm" / "disasm32.py"
    spec = importlib.util.spec_from_file_location("bw2_pcr_disasm32", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _drop_mid_instruction(fns, read_va, code_start, code_end, seeds) -> int:
    d = _disasm32()
    callers = {}
    for a, f in fns.items():
        for t in f.get("calls_to", ()):
            callers.setdefault(t, set()).add(a)
    total = 0
    while True:
        sizes = {a: f["size"] for a, f in fns.items()}
        gone = d.drop_mid_instruction_entries(read_va, sizes, code_start, code_end,
                                              verbose=False, keep=set(seeds) & set(sizes),
                                              callers=callers)
        for a in list(fns):
            if a not in sizes:
                del fns[a]
            else:
                fns[a]["size"] = sizes[a]
                fns[a]["end"] = a + sizes[a]
        total += gone
        if not gone:
            return total


def _walk(read_va, start, code_end, md):
    """Instructions from `start`, straight on, until a terminator or bad decode."""
    va = start
    while va < code_end:
        code = read_va(va, min(_CHUNK, code_end - va))
        if not code:
            return
        advanced = False
        for ins in md.disasm(code, va):
            advanced = True
            va = ins.address + ins.size
            yield ins
            if ins.mnemonic.split()[-1] in _ENDS:
                return
        if not advanced:
            return


def _fold_into_bodies(fns, read_va, code_start, code_end, strong) -> None:
    """Walk each trusted body straight on. Entries it falls into become aliases;
    entries inside one of its instructions go. A body is trusted when it is
    called, strongly seeded, or 16-byte aligned, and no trusted walk has
    already passed through it."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    called = {t for f in fns.values() for t in f.get("calls_to", ())}
    protected = called | set(strong)
    interior = bytearray(max(0, code_end - code_start))
    inside = lambda a: code_start <= a < code_end and interior[a - code_start]
    walked = set()
    for start in sorted(fns):
        f = fns[start]
        if start in walked or f["entry_kind"] == "alias" or inside(start):
            continue
        walked.add(start)
        trusted = start in protected or start % 16 == 0
        prev = None
        for ins in _walk(read_va, start, code_end, md):
            a = ins.address
            if a != start and a in fns:
                m = prev.mnemonic.split()[-1] if prev else ""
                if fns[a]["entry_kind"] == "alias":
                    walked.add(a)            # already a label: the body goes on
                elif prev is None or m in _PAD or a in protected:
                    break
                elif m == "call" and a % 16 == 0:
                    break                    # a function after a call that does not return
                else:
                    fns[a]["entry_kind"] = "alias"
                    walked.add(a)
            if trusted:
                lo = max(ins.address + 1, code_start)
                hi = min(ins.address + ins.size, code_end)
                interior[lo - code_start:hi - code_start] = b"\x01" * max(0, hi - lo)
            prev = ins
    for a in list(fns):
        if inside(a) and a not in protected:
            fns[a]["entry_kind"] = "drop"


def _restore_extents(fns) -> None:
    owner = None
    for a in sorted(fns):
        f = fns[a]
        if f["entry_kind"] not in ("alias", "drop"):
            owner = f
        elif owner is not None and owner["end"] >= a and f["end"] > owner["end"]:
            owner["end"] = f["end"]
            owner["size"] = owner["end"] - owner["address"]
    for a in [a for a, f in fns.items() if f["entry_kind"] == "drop"]:
        del fns[a]


def seed_classes(rtti=(), vtable=(), extra=(), gap=()) -> tuple[set[int], set[int]]:
    """(strong, weak) seeds. RTTI and explicit seeds are evidence and are never
    touched. A vtable-scan hit is only a run of code-shaped numbers and a gap
    seed only a guess; aligned vtable hits and gap seeds are weak: they keep
    disasm32's own mid-instruction drop off them, nothing more."""
    strong = set(rtti) | set(extra)
    weak = ({a for a in vtable if a % 16 == 0} | set(gap)) - strong
    return strong, weak


def clean_functions(functions, read_va, code_start, code_end, seeds=frozenset(),
                    weak_seeds=frozenset()) -> list[dict]:
    fns = {f["address"]: dict(f) for f in functions}
    _drop_mid_instruction(fns, read_va, code_start, code_end, set(seeds) | set(weak_seeds))
    _fold_into_bodies(fns, read_va, code_start, code_end, seeds)
    _restore_extents(fns)
    return [fns[a] for a in sorted(fns)]


def clean_full(full: Path, exe: Path, seeds=frozenset(), weak_seeds=frozenset()) -> dict:
    """Clean a full disasm32 catalog in place; returns before/after counts."""
    doc = json.loads(Path(full).read_text())
    pe = pefile.PE(str(exe), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()

    def read_va(va, n):
        off = va - base
        return img[off:off + n] if 0 <= off < len(img) else None

    before = doc["functions"]
    kept = clean_functions(before, read_va, doc["code_start"], doc["code_end"], seeds, weak_seeds)
    stats = {"before": len(before), "after": len(kept),
             "aliases_before": sum(f["entry_kind"] == "alias" for f in before),
             "aliases_after": sum(f["entry_kind"] == "alias" for f in kept)}
    doc["functions"] = kept
    doc["bw2_clean"] = stats
    if isinstance(doc.get("stats"), dict):
        doc["stats"]["functions"] = len(kept)
    Path(full).write_text(json.dumps(doc, indent=0))
    return stats


def main(argv=None) -> int:
    """Re-clean an existing full catalog without rerunning discovery.

    Seeds are seed_classes() of the RTTI and vtable scans plus any --seeds file; the gap seeds of
    the original run are not recorded, so they get no protection here.
    """
    import argparse
    import shutil
    import tempfile

    from . import pcr
    from .discover import _seed_list, compact_catalog
    from .releases import sha256_file

    ap = argparse.ArgumentParser(prog="python -m bw2.catclean")
    ap.add_argument("full", type=Path)
    ap.add_argument("exe", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--seeds", type=Path, action="append", default=[])
    a = ap.parse_args(argv)
    found = {}
    with tempfile.TemporaryDirectory() as td:
        for tool, name in (("tools/cpp/rtti.py", "rtti"), ("tools/cpp/vtable_scan.py", "vt")):
            s = Path(td) / f"{name}.json"
            pcr.run(tool, str(a.exe.resolve()), "--seeds", str(s))
            found[name] = _seed_list(s)
    extra = [x for s in a.seeds for x in _seed_list(s)]
    seeds, weak = seed_classes(found["rtti"], found["vt"], extra)
    backup = a.full.with_suffix(".uncleaned.json")
    if not backup.exists():
        shutil.copyfile(a.full, backup)
    shutil.copyfile(backup, a.full)
    stats = clean_full(a.full, a.exe, seeds, weak)
    compact_catalog(a.full, a.out, sha256_file(a.exe))
    print(f"[catclean] {a.out.name}: {stats}, {len(seeds)} strong, {len(weak)} weak seeds")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
