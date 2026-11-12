"""Lift the game, the VM interpreter, ScriptLibraryR.dll and the disc's d3dx9_25.dll,
then build the engine. Every build puts the rebased d3dx9_25.dll beside the engine
binary, where the engine loads it from.

    python -m bw2.pipeline [--opt O3] [--lift-only] [--build-only]
"""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

from .build import build
from .d3dx import CATALOG as D3DX_CATALOG, DLL_NAME as D3DX_DLL_NAME, d3dx_dll
from .extrastarts import EXTRA, VM_EXTRA, load_extra_starts, vm_entries
from .lift import ModuleLift, lift_module, link_tables
from .paths import GAMEFILES, REPO
from .patches import script_dll
from .vm import VM_SECTIONS

ENTRY = 0xAD8FA3
EXTRA_FOR = {"white": EXTRA, "vm": VM_EXTRA}
CAT = REPO / "bw2" / "catalog"


def modules():
    exe = GAMEFILES / "exe" / "white.exe"
    dll = script_dll(GAMEFILES / "patches" / "BW2Patch1_2.exe", GAMEFILES / "dll")
    return [("white", exe, CAT / "white-1.42.functions.json", (".text",)),
            ("vm", exe, CAT / "white-1.42.vm.json", VM_SECTIONS),
            ("slr", dll, CAT / "scriptlibraryr-1.2.functions.json", (".text",)),
            ("d3dx", d3dx_dll(), D3DX_CATALOG, (".text",))]


def lift_all(gen: Path, only: tuple = ()) -> list[ModuleLift]:
    """Lift every module (or only those named; the others are read back from gen)."""
    out = []
    for name, exe, cat, secs in modules():
        if only and name not in only:
            d = json.loads((gen / f"{name}.json").read_text())
            out.append(ModuleLift(seconds=0.0, **d))
            continue
        m = lift_module(name, exe, cat, gen, secs, extra_starts=load_extra_starts(EXTRA_FOR.get(name)),
                        computed_ret=True, runtime_push=set(vm_entries(exe)) if name == "vm" else ())
        print(f"[lift] {name}: {m.functions} functions, {m.shards} shards, "
              f"{m.c_bytes / 1e6:.1f} MB, {sum(m.traps.values())} traps, {m.seconds} s", flush=True)
        out.append(m)
    return out


def load_lifted(gen: Path) -> list[ModuleLift]:
    out = []
    for name, *_ in modules():
        d = json.loads((gen / f"{name}.json").read_text())
        out.append(ModuleLift(seconds=0.0, **d))
    return out


def run_pipeline(gen: Path = REPO / "gen", out: Path = REPO / "out" / "bw2",
                 opt: str = "-O3", lift: bool = True, do_build: bool = True, only: tuple = ()) -> dict:
    t0 = time.time()
    mods = lift_all(gen, tuple(only)) if lift else load_lifted(gen)
    link = link_tables(gen, mods, entry=ENTRY)
    print(f"[link] {link['functions']} functions, {len(link['unresolved'])} unresolved targets", flush=True)
    res = {"modules": {m.name: {k: getattr(m, k) for k in ("functions", "shards", "c_bytes", "traps", "seconds")}
                       for m in mods},
           "functions": link["functions"], "unresolved": len(link["unresolved"])}
    if do_build:
        st = build(gen, out, opt=opt, host="engine")
        print(f"[build] {st.objects} objects ({st.compiled} compiled) in {st.seconds} s, "
              f"binary {st.binary_bytes / 1e6:.1f} MB", flush=True)
        res["build"] = {"objects": st.objects, "compiled": st.compiled, "seconds": st.seconds,
                        "binary_bytes": st.binary_bytes}
        beside = Path(out).parent / D3DX_DLL_NAME
        part = beside.with_name(beside.name + ".part")
        shutil.copyfile(d3dx_dll(), part)
        part.replace(beside)
        print(f"[build] {beside}", flush=True)
    res["seconds"] = round(time.time() - t0, 1)
    return res


def parse_args(argv=None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="python -m bw2.pipeline")
    ap.add_argument("--opt", default="-O3", type=lambda v: v if v.startswith("-") else "-" + v,
                    help="O1, O2 or O3 (or --opt=-O3)")
    ap.add_argument("--lift-only", action="store_true")
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--only", action="append", default=[], help="lift only this module (white, vm, slr, d3dx)")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    a = parse_args(argv)
    res = run_pipeline(opt=a.opt, lift=not a.build_only, do_build=not a.lift_only, only=tuple(a.only))
    print(json.dumps(res, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
