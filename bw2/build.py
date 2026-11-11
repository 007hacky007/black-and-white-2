"""Compile lifted shards with clang in parallel and link them with a host."""
import hashlib
import os
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from .paths import REPO

RECOMP32 = REPO / "third_party" / "pcrecomp" / "runtime" / "recomp32"
WIN32HLE = REPO / "third_party" / "pcrecomp" / "runtime" / "win32hle"
BW2RT = REPO / "runtime" / "bw2"
COLD_OPT = "-O0"          # for <module>_cold_<va>.c, see lift.COLD_LINES
# The guest's 4 GB sit at this host address; ADDR() adds it as a constant (spec 5.2).
MEM_BASE = "0x100000000000ull"
FLAGS = ("-fno-strict-aliasing", "-fwrapv", "-mcpu=apple-m1", "-mmacosx-version-min=13.0", "-w",
         f"-DRECOMP_MEM_BASE={MEM_BASE}")
# win32hle's headless core: no SDL2 (gdidc, dsound, screen, present), no ffmpeg (bink,
# mss) and no DirectX 6 (ddraw, d3d, d3d_raster). runtime/bw2/absent.c answers what
# those would have defined.
WIN32HLE_CORE = ("host.c", "host_lite.c", "guest_mem.c", "pe_loader.c", "module.c", "hle_path.c",
                 "kernel32.c", "kernel32_ext.c", "kernel32_crt.c", "user32.c", "gdi32.c", "winmm.c",
                 "ole32.c", "advapi32.c", "wsock32.c", "lz32.c", "storage.c")


# Plan 4: the graphics layers, C and Objective-C (ARC); plan 5: the DirectInput mouse.
BW2_SUBDIRS = ("d3d9", "d3dx", "input")
FRAMEWORKS = ("Metal", "QuartzCore", "AppKit", "Foundation", "ImageIO", "CoreGraphics")


def engine_sources() -> list[Path]:
    own = sorted(p for p in BW2RT.glob("*.c") if not p.stem.endswith("_selftest") and p.name != "linkcheck.c")
    for d in BW2_SUBDIRS:
        own += sorted(p for p in (BW2RT / d).glob("*.[cm]") if not p.stem.endswith("_selftest"))
    return own + [WIN32HLE / n for n in WIN32HLE_CORE]


def bw2_includes() -> list[str]:
    """-I flags for the BW2 runtime and its graphics subdirectories."""
    out = ["-I", str(BW2RT)]
    for d in BW2_SUBDIRS:
        out += ["-I", str(BW2RT / d)]
    return out


def engine_libs() -> list[str]:
    libs = ["-lpthread", "-lm"]
    for f in FRAMEWORKS:
        libs += ["-framework", f]
    return libs


HOSTS = {"linkcheck": lambda: [BW2RT / "linkcheck.c"], "engine": engine_sources}
HOST_LIBS = {"linkcheck": [], "engine": engine_libs()}
OPT_LEVELS = ("-O0", "-O1", "-O2", "-O3", "-Os")


class BuildError(Exception):
    pass


class ToolsMissing(Exception):
    pass


@dataclass
class BuildStats:
    objects: int
    compiled: int
    seconds: float
    binary: Path
    binary_bytes: int


def check_tools() -> Path:
    """clang from the active developer directory; never runs the /usr/bin shims first."""
    r = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True)
    msg = "Install Xcode Command Line Tools: xcode-select --install"
    if r.returncode != 0 or not r.stdout.strip():
        raise ToolsMissing(msg)
    dev = Path(r.stdout.strip())
    for c in (dev / "usr/bin/clang", dev / "Toolchains/XcodeDefault.xctoolchain/usr/bin/clang"):
        if c.exists():
            return c
    raise ToolsMissing(msg)


def sdk_path() -> Path:
    """The macOS SDK under the active developer directory (needed when calling clang directly)."""
    r = subprocess.run(["xcode-select", "-p"], capture_output=True, text=True)
    dev = Path(r.stdout.strip())
    for c in (dev / "SDKs/MacOSX.sdk",
              dev / "Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"):
        if c.exists():
            return c
    raise ToolsMissing("No macOS SDK found; reinstall Xcode Command Line Tools: xcode-select --install")


def _tail(text: str, n: int = 40) -> str:
    return "\n".join(text.splitlines()[-n:])


def _compile_all(clang: Path, jobs, jobs_list) -> list[str]:
    def compile_one(job):
        src, obj, flags = job
        part = obj.with_name(obj.name + ".part")
        objc = ["-fobjc-arc"] if src.suffix == ".m" else []
        r = subprocess.run([str(clang), "-c", *flags, *objc, str(src), "-o", str(part)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not part.exists() or part.stat().st_size == 0:
            part.unlink(missing_ok=True)
            return f"{src.name}: clang exited {r.returncode}\n{_tail(r.stderr)}"
        part.rename(obj)
        return None

    with ThreadPoolExecutor(max_workers=jobs or os.cpu_count() or 4) as ex:
        return [e for e in ex.map(compile_one, jobs_list) if e]


def _stale(obj_dir: Path, stamp_text: str, pairs: list[tuple[Path, Path]]) -> list[tuple[Path, Path]]:
    """Objects to (re)compile: all of them if the stamp (flags + header digest) changed."""
    obj_dir.mkdir(parents=True, exist_ok=True)
    for stale in obj_dir.glob("*.part"):
        stale.unlink()
    stamp = obj_dir / "flags.txt"
    changed = not stamp.exists() or stamp.read_text() != stamp_text
    if changed and stamp.exists():
        stamp.unlink()
    return [(s, o) for s, o in pairs if changed or not o.exists() or o.stat().st_mtime < s.stat().st_mtime]


def _digest(paths) -> str:
    return hashlib.sha256(b"".join(p.name.encode() + b"\0" + p.read_bytes() for p in sorted(paths))).hexdigest()


def build(gen_dir: Path, out: Path, opt: str = "-O3", jobs: int | None = None,
          host: str = "engine", host_obj_dir: Path | None = None) -> BuildStats:
    """Compile the lifted shards in gen_dir and link them with a host (HOSTS).

    Host objects go to host_obj_dir (default gen_dir/obj/host-<name>); an engine
    host includes nothing generated, so its objects can be shared between builds."""
    if opt not in OPT_LEVELS:
        raise ValueError(f"optimisation level must be one of {OPT_LEVELS}, got {opt!r}")
    if host not in HOSTS:
        raise ValueError(f"host must be one of {sorted(HOSTS)}, got {host!r}")
    t0 = time.time()
    clang = check_tools()
    gen_dir, out = Path(gen_dir), Path(out)
    obj_dir = gen_dir / "obj"
    sdk = sdk_path()
    incs = ["-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), "-I", str(gen_dir)]
    cflags = [*FLAGS, "-isysroot", str(sdk), *incs]
    level = lambda src: COLD_OPT if "_cold_" in src.stem else opt
    headers = [*gen_dir.glob("*.h"), *RECOMP32.glob("*.h"), *WIN32HLE.glob("*.h"), *BW2RT.glob("*.h"),
               *(h for d in BW2_SUBDIRS for h in (BW2RT / d).glob("*.h"))]
    # Headers are not in the mtime check, so their content is in the stamp.
    stamp_text = " ".join([str(clang), opt, COLD_OPT, *cflags, _digest(headers)]) + "\n"
    shards = sorted(gen_dir.glob("*.c"))
    shard_pairs = [(s, obj_dir / (s.stem + ".o")) for s in shards]
    hsrc = HOSTS[host]()
    hdir = Path(host_obj_dir) if host_obj_dir else obj_dir / f"host-{host}"
    host_pairs = [(s, hdir / (s.stem + ".o")) for s in hsrc]
    host_stamp = " ".join([str(clang), opt, *cflags[:-2], _digest(headers if host == "linkcheck" else
                                                                   [h for h in headers if h.parent != gen_dir])]) + "\n"
    todo = [(s, o, [level(s), *cflags]) for s, o in _stale(obj_dir, stamp_text, shard_pairs)]
    todo += [(s, o, [opt, *cflags]) for s, o in _stale(hdir, host_stamp, host_pairs)]
    errors = _compile_all(clang, jobs, todo)
    if errors:
        raise BuildError(f"{len(errors)} of {len(todo)} files failed to compile:\n" + "\n".join(errors[:5]))
    (obj_dir / "flags.txt").write_text(stamp_text)
    (hdir / "flags.txt").write_text(host_stamp)
    objects = [o for _, o in shard_pairs + host_pairs]
    out.parent.mkdir(parents=True, exist_ok=True)
    part = out.with_name(out.name + ".part")
    r = subprocess.run([str(clang), "-mmacosx-version-min=13.0", "-isysroot", str(sdk),
                        *map(str, objects), *HOST_LIBS[host], "-o", str(part)],
                       capture_output=True, text=True)
    if r.returncode != 0 or not part.exists():
        part.unlink(missing_ok=True)
        raise BuildError(f"link failed:\n{_tail(r.stderr)}")
    part.rename(out)
    return BuildStats(len(objects), len(todo), round(time.time() - t0, 1), out, out.stat().st_size)
