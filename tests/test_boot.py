"""Where the boot stops today: the lifted game boots on the patched tree through Direct3D, its DirectInput
mouse setup and its effects (techniques and passes, plan 5 Task 6) to a later plan's layer (Task 7 replaces
this)."""
import os
import subprocess

import pytest

from bw2 import paths

ENGINE = paths.REPO / "out" / "bw2"
# the engine's loud stops (the loader's "0 unresolved imports" is not one)
LOUD = ("TRIPWIRE", "trap:", "unresolved indirect call", "unresolved direct call", "unimplemented import",
        "uncaught C++", "FATAL", "not implemented")


# Where the boot stops with plan 5's effect techniques and passes (Task 6): the game creates its effects,
# validates and sets their techniques, runs Begin/BeginPass/EndPass/End on them, and then calls
# D3DXCreateSprite, which plan 7 owns: the run exits 4 naming it.
FIRST_STOP = "reached D3DXCreateSprite: owned by a later plan"


@pytest.mark.gamefiles
def test_boot_reaches_effects(tmp_path):
    assert ENGINE.exists(), "build it first: python -m bw2.pipeline"
    env = dict(os.environ, BW2_SUPPORT=str(tmp_path / "support"), BW2_HEADLESS="1")
    r = subprocess.run([str(ENGINE), "--game", str(paths.GAMEFILES / "game")], capture_output=True, text=True,
                       timeout=120, env=env)
    (tmp_path / "stderr.txt").write_text(r.stderr)
    tail = r.stderr[-4000:]
    assert r.returncode == 4, f"exit {r.returncode}\n{tail}"
    assert "reached D3DXCreateEffect" not in r.stderr, tail
    assert FIRST_STOP in r.stderr, tail
    for word in LOUD:
        assert word not in r.stderr, f"{word!r} in stderr\n{tail}"


def _vm_thunks_called_by_the_game():
    """stxt774's thunks into the VM that .text calls directly (the 18 virtualized functions)."""
    import struct
    from bw2.extrastarts import vm_entries
    from bw2.lift import _image_reader
    exe = paths.GAMEFILES / "game" / "white.exe"
    read, ents = _image_reader(exe), set(vm_entries(exe))
    lo, hi = 0x2428000, 0x24293B4
    d = read(lo, hi - lo)
    thunks = {lo + o for o in range(len(d) - 4)
              if d[o] == 0xE9 and lo + o + 5 + struct.unpack_from("<i", d, o + 1)[0] in ents}
    text = read(0x401000, 0x8101CB)
    return sorted({t for o in range(len(text) - 5) if text[o] == 0xE8
                   for t in [0x401000 + o + 5 + struct.unpack_from("<i", text, o + 1)[0]] if t in thunks})


@pytest.mark.gamefiles
def test_vm_calls_match_unicorn(tmp_path):
    # The protection VM runs as shipped: each virtualized function, lifted, must return what the
    # original x86 returns under Unicorn, from the same fresh state and arguments.
    from bw2.pe32emu import Pe32Host
    exe = paths.GAMEFILES / "game" / "white.exe"
    thunks = _vm_thunks_called_by_the_game()
    assert len(thunks) == 18
    env = dict(os.environ, BW2_SUPPORT=str(tmp_path / "support"))
    bad = []
    for va in thunks:
        for args in ((0x637AB774, 0x637AB774), (1, 2)):
            h = Pe32Host(exe.read_bytes())
            h.api["GetCurrentThreadId"] = (0, lambda: 0x1234)
            native = h.call(va, *args)
            r = subprocess.run([str(ENGINE), "--game", str(paths.GAMEFILES / "game"), "--call", hex(va),
                                *map(hex, args)], capture_output=True, text=True, timeout=60, env=env)
            if f"eax 0x{native:08X}" not in r.stdout:
                bad.append(f"0x{va:08X}{args}: native 0x{native:08X}, lifted {r.stdout.strip() or r.stderr[-300:]}")
    assert not bad, "\n".join(bad)
