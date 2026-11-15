"""Lift catalogued functions to sharded C with pcrecomp's lift32.Lifter."""
import json
import re
import sys
import time
from collections import Counter
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .discover import code_view, load_functions
from .pcr import PCR

for _sub in ("pe", "disasm", "lift"):
    _p = str(PCR / "tools" / _sub)
    if _p not in sys.path:
        sys.path.insert(0, _p)

SYM = re.compile(r"\bsub_[0-9A-F]{8}\b")
TRAP = re.compile(r'RECOMP_TRAP\(0x[0-9A-F]{8}, "([^"]+)"\)')
# A function this long goes to its own shard, which build() compiles at -O0: clang -O1
# ran over 28 minutes on white.exe 0xB84D70 (~152k lines), -O0 takes 8 s.
COLD_LINES = 50_000
HEADER = ('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n#include "recomp_funcs.h"\n'
          '#include "bw2_lift.h"\n\n')
RETURN = re.compile(r"\breturn;")


class LiftError(Exception):
    pass


@dataclass
class ModuleLift:
    name: str
    functions: int
    shards: int
    c_bytes: int
    traps: dict
    defined: list
    referenced: list
    seconds: float
    cold: list = field(default_factory=list)
    eh_functions: int = 0
    eh_resume_points: int = 0


def _tools(exe: Path, computed_ret: bool = False):
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler
    from lift32 import Lifter
    info = analyze_pe(str(exe))
    iat = build_iat_map(info)
    dis = Disassembler(Path(exe).read_bytes(), info.image_base, info.sections)
    return dis, Lifter(iat_map=iat, func_names={}, computed_ret=computed_ret), iat


def _image_reader(exe: Path):
    """read(va, n) -> bytes of the mapped image ('' outside it)."""
    import pefile
    pe = pefile.PE(str(exe), fast_load=True)
    base, img = pe.OPTIONAL_HEADER.ImageBase, pe.get_memory_mapped_image()
    return lambda va, n: img[va - base:va - base + n] if 0 <= va - base < len(img) else b""


def eh_resume_points(md, read, va: int) -> set:
    """Where the function at va continues after its own catch blocks (eh32.resume_points)."""
    from eh32 import resume_points
    insns = list(md.disasm(read(va, 96), va))[:12]
    return resume_points(md, read, insns) or set()


PUSH_IMM = re.compile(r"PUSH32\(esp, [^;]*\); /\* 0x([0-9A-F]{8}): push ")


def _runtime_pushes(code: str, sites, done: set) -> str:
    """Make each `push imm32` at one of sites read its operand from guest memory."""
    def sub(m):
        va = int(m.group(1), 16)
        if va not in sites:
            return m.group(0)
        done.add(va)
        return f"PUSH32(esp, MEM32(0x{va + 1:08X}u)); /* 0x{va:08X}: push (operand rewritten at run time) "
    return PUSH_IMM.sub(sub, code)


def add_continuations(dis, iat, func, resume: set) -> None:
    """Make each catch continuation a block of func's body. One inside a block (reached
    by falling through, as white.exe 0x9726C0) splits it. One that only a catch funclet
    returns to (the try path returns by itself, as white.exe 0x86C180) was never
    reached by the walk from the entry: walk from it and merge its blocks. One that
    would overlap an existing block any other way stops the lift."""
    import copy
    for r in sorted(resume):
        if r in func.blocks:
            continue
        inside = next((b for b in func.blocks.values()
                       if b.start < r < b.end and any(i.address == r for i in b.instructions)), None)
        if inside is not None:                    # reached by falling through: split the block there
            k = next(n for n, i in enumerate(inside.instructions) if i.address == r)
            tail = copy.copy(inside)
            tail.start, tail.instructions = r, inside.instructions[k:]
            inside.instructions, inside.end, inside.successors, inside.is_exit = inside.instructions[:k], r, [r], False
            func.blocks[r] = tail
            continue
        g = dis.disassemble_function(r, iat)
        if g is None:
            raise LiftError(f"sub_{func.address:08X}: cannot disassemble catch continuation 0x{r:08X}")
        for addr, blk in sorted(g.blocks.items()):
            if addr in func.blocks or any(b.start < addr < b.end for b in func.blocks.values()):
                continue                          # code the body already has
            cut = min((b.start for b in func.blocks.values() if blk.start < b.start < blk.end), default=None)
            if cut is not None:                   # falls into a block the body has (white.exe 0x972BF0)
                keep = [i for i in blk.instructions if i.address < cut]
                if not keep or keep[-1].address + keep[-1].size != cut:
                    raise LiftError(f"sub_{func.address:08X}: catch continuation 0x{r:08X} runs into "
                                    f"block 0x{cut:08X} mid-instruction")
                blk.instructions, blk.end, blk.successors, blk.is_exit = keep, cut, [cut], False
            clash = next((b for b in func.blocks.values() if b.start < blk.end and blk.start < b.end), None)
            if clash is not None:
                raise LiftError(f"sub_{func.address:08X}: catch continuation 0x{r:08X} overlaps block "
                                f"0x{clash.start:08X}")
            func.blocks[addr] = blk
        func.calls_to |= g.calls_to
        func.jump_targets |= g.jump_targets


def add_landing_pad(code: str, va: int, resume: set) -> str:
    """Give a lifted function a setjmp landing pad (runtime/bw2/bw2_lift.h) that the
    guest C++ exception dispatch longjmps to, and a case for each continuation."""
    for r in sorted(resume):
        if f"L_{r:08X}:" not in code:
            raise LiftError(f"sub_{va:08X}: catch continuation 0x{r:08X} is not a block label")
    enter = f"RECOMP_ENTER(0x{va:08X}u);"
    head, tail = code.split(enter, 1)
    body, close = tail.rsplit("}", 1)
    body = RETURN.sub("{ BW2_EH_LEAVE(); return; }", body)
    cases = " ".join(f"case 0x{r:08X}u: goto L_{r:08X};" for r in sorted(resume))
    pad = (f"  _eh_resume: switch (_ehf.resume) {{ {cases} "
           f"default: bw2_eh_bad_resume(0x{va:08X}u, _ehf.resume); }}\n")
    return head + enter + "\n    BW2_EH_ENTER();" + body + pad + "}" + close


def _write(path: Path, text: str) -> None:
    part = path.with_name(path.name + ".part")
    part.write_text(text)
    part.rename(path)


def lift_module(name: str, exe: Path, catalog: Path, out_dir: Path,
                code_sections: tuple = (".text",), shard_size: int = 500,
                cold_lines: int = COLD_LINES, extra_starts=(), computed_ret: bool = False,
                runtime_push=frozenset()) -> ModuleLift:
    """computed_ret: a `ret` to an address the code pushed jumps there (pcrecomp's
    Lifter option), for hand-written code that chains through the stack: the
    protection VM leaves its interpreter through `popal; popfd; ret`.

    runtime_push: addresses of `push imm32` instructions whose operand the program
    rewrites while it runs (the VM's re-entry stubs): each reads its operand from
    guest memory when it runs, instead of the value the file had."""
    t0 = time.time()
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in [*out_dir.glob(f"{name}_*.c"), *out_dir.glob(f"{name}*.part")]:
        stale.unlink()
    view = code_view(exe, list(code_sections), out_dir / f".{name}.view.exe")
    dis, lifter, iat = _tools(view, computed_ret)
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    md, read = Cs(CS_ARCH_X86, CS_MODE_32), _image_reader(exe)
    starts = sorted({f["address"] for f in load_functions(catalog) if f.get("entry_kind") != "alias"}
                    | set(extra_starts))
    defined, referenced, traps = [], set(), Counter()
    eh_funcs = eh_points = 0
    pushes_done = set()
    shards, c_bytes, chunk, cold = 0, 0, [], []

    def flush():
        nonlocal shards, c_bytes, chunk
        text = HEADER + "\n".join(chunk)
        _write(out_dir / f"{name}_{shards:04d}.c", text)
        c_bytes += len(text.encode())
        shards += 1
        chunk = []

    for va in starts:
        f = dis.disassemble_function(va, iat)
        if f is None:
            raise RuntimeError(f"{name}: cannot disassemble catalogued function 0x{va:08X}")
        resume = eh_resume_points(md, read, va)
        if resume:
            add_continuations(dis, iat, f, resume)
        code = lifter.lift_function(f)
        if name == "white" and va == 0x5FDC10:
            # Fan 1.42 skips the retained interactive logo along with the EA movie.
            # Call the game's scene, then keep the movie skip (startup.md).
            site = "    goto L_005FDC51; /* 0x005FDC39: jmp 0x5fdc51 */"
            if code.count(site) != 1:
                raise LiftError("white: unexpected 1.42 logo skip at 0x5FDC39")
            code = code.replace(site, "    RECOMP_CALL(sub_00526770);\n    RECOMP_FLAGS_IN();\n" + site)
        if runtime_push:
            code = _runtime_pushes(code, runtime_push, pushes_done)
        if resume:
            code = add_landing_pad(code, va, resume)
            eh_funcs, eh_points = eh_funcs + 1, eh_points + len(resume)
        sym = f"sub_{va:08X}"
        defined.append(sym)
        referenced.update(s for s in SYM.findall(code) if s != sym)
        traps.update(TRAP.findall(code))
        if code.count("\n") > cold_lines:
            fname = f"{name}_cold_{va:08X}.c"
            text = HEADER + code
            _write(out_dir / fname, text)
            c_bytes += len(text.encode())
            cold.append(fname)
            continue
        chunk.append(code)
        if len(chunk) >= shard_size:
            flush()
    if chunk:
        flush()
    missing = sorted(set(runtime_push) - pushes_done)
    if missing:
        raise LiftError(f"{name}: run-time push sites not lifted: {', '.join(f'0x{v:08X}' for v in missing)}")
    m = ModuleLift(name, len(defined), shards, c_bytes, dict(sorted(traps.items())),
                   defined, sorted(referenced), round(time.time() - t0, 1), cold, eh_funcs, eh_points)
    doc = {k: v for k, v in asdict(m).items() if k != "seconds"}   # reproducible
    _write(out_dir / f"{name}.json", json.dumps(doc, indent=0, sort_keys=True))
    return m


def lift_one(exe: Path, va: int, code_sections: tuple = (".text",)) -> str:
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        view = code_view(exe, list(code_sections), Path(td) / "view.exe")
        dis, lifter, iat = _tools(view)
        return lifter.lift_function(dis.disassemble_function(va, iat))


def link_tables(out_dir: Path, modules: list, entry: int) -> dict:
    """One header, one dispatch table and loud stubs for targets nothing lifted."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    defined = sorted({s for m in modules for s in m.defined})
    dset = set(defined)
    unresolved = sorted({s for m in modules for s in m.referenced} - dset)
    va = lambda s: int(s[4:], 16)
    hdr = ["#pragma once", "#include <stdint.h>", "",
           "void recomp_unresolved_call(uint32_t va);", "extern const uint32_t bw2_entry_va;", ""]
    hdr += [f"void {s}(void);" for s in sorted(defined + unresolved, key=va)]
    _write(out_dir / "recomp_funcs.h", "\n".join(hdr) + "\n")
    disp = ['#include "recomp_types.h"', '#include "recomp_funcs.h"', "",
            "const recomp_dispatch_entry_t recomp_dispatch_table[] = {"]
    disp += [f"    {{ 0x{va(s):08X}u, {s} }}," for s in sorted(defined, key=va)]
    disp += ["};", f"const uint32_t recomp_dispatch_count = {len(defined)};",
             f"const uint32_t bw2_entry_va = 0x{entry:08X}u;", ""]
    _write(out_dir / "recomp_dispatch.c", "\n".join(disp))
    stubs = ['#include <stdint.h>', '#include "recomp_funcs.h"', ""]
    stubs += [f"void {s}(void) {{ recomp_unresolved_call(0x{va(s):08X}u); }}"
              for s in sorted(unresolved, key=va)]
    _write(out_dir / "recomp_unresolved.c", "\n".join(stubs) + "\n")
    result = {"functions": len(defined), "unresolved": unresolved,
              "modules": sorted(m.name for m in modules)}
    _write(out_dir / "link.json", json.dumps(result, indent=0, sort_keys=True))
    return result
