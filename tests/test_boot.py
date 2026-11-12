"""Where the boot gets to today: the lifted game boots on the patched tree through Direct3D, its DirectInput
mouse setup, its shaders and effects (plan 5), its sprite and fonts and its loading screen (plan 7) and its
DirectSound audio (plan 8) to the front end, where it first reads input (docs/research/frontend.md). The
run ends at a fixed read count (BW2_QUIT_AFTER_READS), since the front end waits for input."""
import os
import re
import subprocess

import pytest

from bw2 import paths

ENGINE = paths.REPO / "out" / "bw2"
# the engine's loud stops (the loader's "0 unresolved imports" is not one)
LOUD = ("TRIPWIRE", "trap:", "unresolved indirect call", "unresolved direct call", "unimplemented import",
        "uncaught C++", "FATAL", "not implemented")

# the Arial Narrow heights the game asks for, in call order: four rounds (docs/research/startup.md)
ARIAL_NARROW_HEIGHTS = [-10, -8, -13, -10, -15, -12, -20, -16, -31, -24, -46, -36] * 4
FONT = re.compile(r'^\[bw2\] font "([^"]*)" (-?\d+) weight (\d+): (.*)$')
FRAME = re.compile(r"^\[bw2\] frame (\d+): (\d+)x(\d+), mean (\d+),(\d+),(\d+), nonblack (\d+) of (\d+)$")
AUDIO_SECOND = re.compile(r"^\[bw2\] audio second (\d+): (\d+) playing, peak (\d+), nonsilent (\d+) of (\d+)$")
FIRST_READ = "[bw2] DirectInput mouse: first read"
READS = 300


def engine_lines(stderr):
    """stderr as lines, with each engine line split off a game debug line that ran into it. The game's
    OutputDebugStringA text is printed as given, and one of its lines has no newline, so the next engine
    line can follow it on the same line. The engine prints whole lines, each starting "[bw2] "."""
    return [part for ln in stderr.splitlines() for part in re.split(r"(?=\[bw2\] )", ln) if part]


@pytest.mark.gamefiles
def test_boot_to_front_end(tmp_path):
    # Plan 8: the game makes its sprite and its fonts (the 48 Arial Narrow ones from the face it adds with
    # AddFontMemResourceEx), loads its script plugin, shows its loading screen, starts its audio (one
    # streaming DirectSound buffer, played by the mixer), and reaches its front end, where it reads the
    # mouse once per frame. BW2_QUIT_AFTER_READS ends the run at the 300th read (docs/research/frontend.md).
    assert ENGINE.exists(), "build it first: python -m bw2.pipeline"
    support = tmp_path / "support"
    env = dict(os.environ, BW2_SUPPORT=str(support), BW2_HEADLESS="1", BW2_FRAME_LOG="1", BW2_FONT_LOG="1",
               BW2_AUDIO_LOG="1", BW2_QUIT_AFTER_READS=str(READS))
    r = subprocess.run([str(ENGINE), "--game", str(paths.GAMEFILES / "game")], capture_output=True, text=True,
                       timeout=180, env=env)
    (tmp_path / "stderr.txt").write_text(r.stderr)
    tail = r.stderr[-4000:]
    lines = engine_lines(r.stderr)
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert f"[bw2] quit after {READS} input reads (BW2_QUIT_AFTER_READS)" in lines, tail
    # the loader's own counts ("bound 412 imports, 0 unresolved", "0 unresolved imports") are not stops
    err = "\n".join(ln for ln in lines if not (ln.startswith(("[pe] ", "[host] ")) and "0 unresolved" in ln))
    for word in LOUD + ("unresolved", "unimplemented", "uncaught", "trap"):
        assert word not in err, f"{word!r} in stderr\n{tail}"
    assert "owned by a later plan" not in r.stderr, tail
    # the fonts: Courier New from the Mac's file, then all 48 Arial Narrow ones from the game's own face;
    # the front end makes no more by its 300th read
    fonts = [m.groups() for m in map(FONT.match, lines) if m]
    assert len(fonts) == 49, "\n".join(map(str, fonts))
    face, height, weight, source = fonts[0]
    assert (face, height, weight) == ("Courier New", "-16", "400") and source.startswith("/"), fonts[0]
    assert [f[:3] for f in fonts[1:]] == [("Arial Narrow", str(h), "400") for h in ARIAL_NARROW_HEIGHTS], fonts
    assert all(f[3] == "memory" for f in fonts[1:]), fonts
    # the script plugin loads as a lifted DLL with every import answered
    assert "scriptlibraryr.dll: lifted DLL at 0x10000000, 0 imports unanswered" in r.stderr, tail
    # audio: one streaming buffer, and the mixer plays non-silent seconds of it
    bufs = [ln for ln in lines if ln.startswith("[bw2] audio buffer ")]
    assert bufs == ["[bw2] audio buffer 1: 2 ch, 44100 Hz, 16 bit, 21504 bytes, flags 0x18000"], bufs
    seconds = [tuple(map(int, m.groups())) for m in map(AUDIO_SECOND.match, lines) if m]
    assert any(s[3] > 0 for s in seconds), seconds
    # the front end reads the mouse: the first-read marker comes once, at the first read
    assert lines.count(FIRST_READ) == 1, tail
    first_read = lines.index(FIRST_READ)
    # the frames: numbered from 1, at least one per read, and the front end (to the last frame) is mostly
    # not black
    frames = [tuple(map(int, m.groups())) for m in map(FRAME.match, lines) if m]
    assert len(frames) >= READS and [f[0] for f in frames] == list(range(1, len(frames) + 1)), frames[-5:]
    assert all(f[7] == f[1] * f[2] for f in frames), frames
    after = [tuple(map(int, m.groups())) for m in map(FRAME.match, lines[first_read:]) if m]
    assert any(2 * f[6] >= f[7] for f in after), after
    assert 2 * frames[-1][6] >= frames[-1][7], frames[-1]
    # the sound banks are read in place: nothing is written under the support folder's game overlay
    made = sorted(p for p in (support / "Game").rglob("*") if p.is_file()) if (support / "Game").exists() else []
    assert not made, made


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
