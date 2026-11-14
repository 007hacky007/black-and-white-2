import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def pytest_collection_modifyitems(config, items):
    gamefiles = os.environ.get("BW2_GAMEFILES") == "1"
    long = os.environ.get("BW2_LONG") == "1"
    e2e = os.environ.get("BW2_BUILDER_E2E") == "1"
    skip_files = pytest.mark.skip(reason="needs real game files: set BW2_GAMEFILES=1")
    skip_long = pytest.mark.skip(reason="a long test: set BW2_LONG=1")
    skip_e2e = pytest.mark.skip(reason="the whole Builder build: set BW2_BUILDER_E2E=1")
    for item in items:
        if "gamefiles" in item.keywords and not gamefiles:
            item.add_marker(skip_files)
        elif "long" in item.keywords and not long:
            item.add_marker(skip_long)
        elif "builder_e2e" in item.keywords and not e2e:
            item.add_marker(skip_e2e)


REPO = Path(__file__).resolve().parents[1]
_PCR_TOOLS = REPO / "third_party" / "pcrecomp" / "tools"


@pytest.fixture
def lifted_c(tmp_path):
    """lifted_c(pe_bytes, va) -> C text of the function at va, lifted by pcrecomp."""
    for sub in ("pe", "disasm", "lift"):
        p = str(_PCR_TOOLS / sub)
        if p not in sys.path:
            sys.path.insert(0, p)
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler
    from lift32 import Lifter

    def lift(pe_bytes: bytes, va: int) -> str:
        exe = tmp_path / f"lift_{va:x}.exe"
        exe.write_bytes(pe_bytes)
        info = analyze_pe(str(exe))
        iat = build_iat_map(info)
        dis = Disassembler(pe_bytes, info.image_base, info.sections)
        return Lifter(iat_map=iat, func_names={}).lift_function(dis.disassemble_function(va, iat))
    return lift


def make_fake_ffmpeg_prefix(root: Path, install_name=lambda name: f"@rpath/{name}") -> Path:
    """A prefix laid out as FFmpeg installs itself: for each of FFMPEG_DYLIBS a tiny real dylib
    <stem>.1.2.dylib, the versioned name as a symlink to it, and lib<x>.dylib as another symlink.
    install_name(name) gives each library's install name. Each library also exports its own
    int bw2_fake_<x>(void) (bw2_fake_avformat, ...), returning 1, so a test engine can call all three."""
    import subprocess
    from bw2.build import check_tools, sdk_path
    from bw2.ffmpeg import FFMPEG_DYLIBS
    lib = root / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    for name in FFMPEG_DYLIBS:
        stem = name[:-len(".dylib")]
        src = root / f"{stem}.c"
        src.write_text(f"int bw2_fake_ffmpeg(void) {{ return 0; }}\n"
                       f"int bw2_fake_{name.split('.')[0][3:]}(void) {{ return 1; }}\n")
        real = lib / f"{stem}.1.2.dylib"
        subprocess.run([str(check_tools()), "-dynamiclib", "-isysroot", str(sdk_path()),
                        "-mmacosx-version-min=13.0", "-install_name", install_name(name),
                        str(src), "-o", str(real)], check=True, capture_output=True)
        (lib / name).symlink_to(real.name)
        (lib / (name.split(".")[0] + ".dylib")).symlink_to(real.name)
    return root


@pytest.fixture
def fake_ffmpeg_prefix(tmp_path):
    """fake_ffmpeg_prefix(install_name=...) -> a fake FFmpeg prefix under tmp_path (see make_fake_ffmpeg_prefix)."""
    n = iter(range(1000))
    return lambda **kw: make_fake_ffmpeg_prefix(tmp_path / f"ffmpeg-{next(n)}", **kw)


@pytest.fixture(scope="session")
def lgpl_engine(tmp_path_factory):
    """The engine linked against the LGPL-only FFmpeg (bw2.ffmpeg), with d3dx9_25.dll beside it.
    Host objects go to their own folder: the FFmpeg flags are in the host stamp, so sharing
    gen/obj/host-engine would rebuild the development engine's host objects each way."""
    import shutil
    from bw2 import build as build_mod
    from bw2.ffmpeg import LGPL_PREFIX, build_ffmpeg
    prefix = build_ffmpeg()
    assert prefix == LGPL_PREFIX
    tmp = tmp_path_factory.mktemp("lgpl-engine")
    old = os.environ.get("BW2_FFMPEG_PREFIX")
    os.environ["BW2_FFMPEG_PREFIX"] = str(prefix)
    try:
        st = build_mod.build(REPO / "gen", tmp / "bw2", host="engine", host_obj_dir=tmp / "host-lgpl")
    finally:
        if old is None:
            os.environ.pop("BW2_FFMPEG_PREFIX", None)
        else:
            os.environ["BW2_FFMPEG_PREFIX"] = old
    shutil.copyfile(REPO / "out" / "d3dx9_25.dll", tmp / "d3dx9_25.dll")
    return st.binary
