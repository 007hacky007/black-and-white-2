"""Plan 3's end point: the lifted game boots on the patched tree to its graphics setup."""
import os
import subprocess

import pytest

from bw2 import paths

ENGINE = paths.REPO / "out" / "bw2"
# the engine's loud stops (the loader's "0 unresolved imports" is not one)
LOUD = ("TRIPWIRE", "trap:", "unresolved indirect call", "unresolved direct call", "unimplemented import",
        "uncaught C++", "FATAL")


@pytest.mark.gamefiles
def test_boot_reaches_graphics(tmp_path):
    assert ENGINE.exists(), "build it first: python -m bw2.pipeline"
    env = dict(os.environ, BW2_SUPPORT=str(tmp_path / "support"))
    r = subprocess.run([str(ENGINE), "--game", str(paths.GAMEFILES / "game")], capture_output=True, text=True,
                       timeout=120, env=env)
    (tmp_path / "stderr.txt").write_text(r.stderr)
    tail = r.stderr[-4000:]
    assert r.returncode == 4, f"exit {r.returncode}\n{tail}"
    assert "owned by a later plan" in r.stderr, tail
    for word in LOUD:
        assert word not in r.stderr, f"{word!r} in stderr\n{tail}"


@pytest.mark.gamefiles
def test_vm_call_matches_unicorn(tmp_path):
    # The protection VM runs as shipped: the lifted interpreter must give what the original
    # x86 gives. The game's first VM call (from 0x5EFA20) with its arguments, both ways.
    from bw2.pe32emu import Pe32Host
    exe = paths.GAMEFILES / "game" / "white.exe"
    h = Pe32Host(exe.read_bytes())
    h.api["GetCurrentThreadId"] = (0, lambda: 0x1234)
    native = h.call(0x242917C, 0x637AB774, 0x637AB774)
    env = dict(os.environ, BW2_SUPPORT=str(tmp_path / "support"))
    r = subprocess.run([str(ENGINE), "--game", str(paths.GAMEFILES / "game"), "--call", "0x242917C",
                        "0x637AB774", "0x637AB774"], capture_output=True, text=True, timeout=60, env=env)
    assert f"eax 0x{native:08X}" in r.stdout, (native, r.stdout, r.stderr[-2000:])
