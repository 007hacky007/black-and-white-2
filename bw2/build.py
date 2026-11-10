"""Compile lifted shards with clang in parallel and link them with a host."""
import os
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from .paths import REPO

RECOMP32 = REPO / "third_party" / "pcrecomp" / "runtime" / "recomp32"
FLAGS = ("-fno-strict-aliasing", "-mcpu=apple-m1", "-mmacosx-version-min=13.0", "-w")
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


def build(gen_dir: Path, out: Path, opt: str = "-O2", jobs: int | None = None,
          host: Path = REPO / "runtime" / "bw2" / "linkcheck.c") -> BuildStats:
    if opt not in OPT_LEVELS:
        raise ValueError(f"optimisation level must be one of {OPT_LEVELS}, got {opt!r}")
    t0 = time.time()
    clang = check_tools()
    gen_dir, out = Path(gen_dir), Path(out)
    obj_dir = gen_dir / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    for stale in obj_dir.glob("*.part"):
        stale.unlink()
    sdk = sdk_path()
    cflags = [opt, *FLAGS, "-isysroot", str(sdk), "-I", str(RECOMP32), "-I", str(gen_dir)]
    stamp = obj_dir / "flags.txt"
    stamp_text = " ".join([str(clang), *cflags]) + "\n"
    flags_changed = not stamp.exists() or stamp.read_text() != stamp_text
    sources = sorted(gen_dir.glob("*.c")) + [Path(host)]
    objects = [obj_dir / (s.stem + ".o") for s in sources]
    todo = [(s, o) for s, o in zip(sources, objects)
            if flags_changed or not o.exists() or o.stat().st_mtime < s.stat().st_mtime]
    if flags_changed and stamp.exists():
        stamp.unlink()

    def compile_one(job):
        src, obj = job
        part = obj.with_name(obj.name + ".part")
        r = subprocess.run([str(clang), "-c", *cflags, str(src), "-o", str(part)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not part.exists() or part.stat().st_size == 0:
            part.unlink(missing_ok=True)
            return f"{src.name}: clang exited {r.returncode}\n{_tail(r.stderr)}"
        part.rename(obj)
        return None

    with ThreadPoolExecutor(max_workers=jobs or os.cpu_count() or 4) as ex:
        errors = [e for e in ex.map(compile_one, todo) if e]
    if errors:
        raise BuildError(f"{len(errors)} of {len(todo)} files failed to compile:\n" + "\n".join(errors[:5]))
    stamp.write_text(stamp_text)
    out.parent.mkdir(parents=True, exist_ok=True)
    part = out.with_name(out.name + ".part")
    r = subprocess.run([str(clang), "-mmacosx-version-min=13.0", "-isysroot", str(sdk),
                        *map(str, objects), "-o", str(part)],
                       capture_output=True, text=True)
    if r.returncode != 0 or not part.exists():
        part.unlink(missing_ok=True)
        raise BuildError(f"link failed:\n{_tail(r.stderr)}")
    part.rename(out)
    return BuildStats(len(objects), len(todo), round(time.time() - t0, 1), out, out.stat().st_size)
