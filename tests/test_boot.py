"""Where the boot gets to today: the lifted game boots on the patched tree through Direct3D, its DirectInput
mouse setup, its shaders and effects (plan 5), its sprite and fonts and its loading screen (plan 7) and its
DirectSound audio (plan 8) to the front end, where it first reads input (docs/research/frontend.md). With no
input the front end waits, so that run ends at a fixed read count (BW2_QUIT_AFTER_READS). With scripted input
(plan 6, BW2_INPUT_SCRIPT) the game takes a new profile in its first dialog, compiles its first HLSL effect
(its tone-mapping filter) through Microsoft's own compiler, lifted from disc 1's d3dx9_25.dll (plan 9), and
draws its front end until its idle timer opens the attract video, which plan 10 plays (Bink through FFmpeg).
A click on New Game instead loads the first land, plays the intro movie to its last frame and draws the
land's opening scene in 3D. Those runs end at a movie frame (BW2_QUIT_AFTER_MOVIE), a moment the movie's clock
sets whatever the frame rate (docs/research/input.md, docs/research/hlsl.md, docs/research/campaign.md)."""
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
GPU_ABORT = "[bw2] metal: command buffer failed: "
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
    # the script plugin loads as a lifted DLL. 12 of its imports have no answer, each bound to a stub that
    # stops by name if called: its crash reporter's dbghelp calls (SymInitialize, StackWalk and 6 more),
    # DebugBreak, IsDebuggerPresent, PeekNamedPipe and GDI's DeleteObject; the boot calls none of them
    assert "scriptlibraryr.dll: lifted DLL at 0x10000000, 12 imports unanswered" in r.stderr, tail
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


# Plan 6: the front end's first dialog, driven through the host input queue at DirectInput read counts
# (docs/research/input.md): click the name field, six Backspaces clear the default name, type "ab" (A, B),
# then move to the confirm button and click it. Coordinates and key codes only.
PROFILE_SCRIPT = "\n".join(
    ["10 move 510 382", "14 down 0", "16 up 0"]
    + [f"{22 + 2 * i + j} key 8 0x0E {'up' if j else 'down'}" for i in range(6) for j in (0, 1)]
    + ["40 key 0x41 0x1E down", "41 key 0x41 0x1E up", "42 key 0x42 0x30 down", "43 key 0x42 0x30 up",
       "50 move -88 45", "80 down 0", "84 up 0"]) + "\n"
# The [module] lines the boot printed before plan 9 (measured with the engine built at 41a8e9f, which stopped at
# the HLSL effect): the game's and the script plugin's CRT asking kernel32 for the fiber-local storage calls,
# the game's PSApi.dll and its two ntdll lookups. Any other [module] line is new and fails the boot test, except
# the lifted D3DX asking for the debug runtime d3d9d.dll, which is absent as on a retail machine (plan 9 Ruling 12).
MODULE_LINES_BEFORE = {f"[module] GetProcAddress(kernel32.dll, {n}): not answered"
                       for n in ("FlsAlloc", "FlsGetValue", "FlsSetValue", "FlsFree")} | {
    '[module] LoadLibraryA("PSApi.dll"): not found',
    "[module] GetProcAddress(ntdll.dll, ZwLockVirtualMemory): not answered",
    "[module] GetProcAddress(ntdll.dll, ZwUnlockVirtualMemory): not answered",
    '[module] LoadLibraryA("d3d9d.dll"): not found'}
D3DX_LOADED = "[bw2] d3dx9_25.dll: lifted DLL at 0x08000000, 52 imports unanswered"


# Plan 10: New Game. After the profile, the running point moves from (422, 427) to (252, 711), the front end's
# New Game button at 1024x768, and clicks it, well before the attract's timer (docs/research/campaign.md).
NEW_GAME_SCRIPT = PROFILE_SCRIPT + "300 move -170 284\n310 down 0\n314 up 0\n"
BINK_OPEN = "[bw2] bink open: "
ATTRACT_OPEN = "[bw2] bink open: 720x576, 2033 frames at 25/1 fps, 0 audio tracks, 32540316 bytes from memory"
INTRO_OPEN = "[bw2] bink open: 640x360, 1798 frames at 5000000/208333 fps, 0 audio tracks, 51782036 bytes from memory"
ATTRACT_QUIT = "[bw2] quit at movie frame 2033:100 + 0 presents (BW2_QUIT_AFTER_MOVIE)"
# Bounds from docs/research/campaign.md, each at most half the smallest value measured over three runs: the
# non-black share of the brightest attract frame, and of the brightest intro frame
ATTRACT_NONBLACK = 0.5
MOVIE_NONBLACK = 0.375
# the last intro frame the game shows (Goto clamps at 1798; every run reached it), and 600 Presents after it:
# the first land's opening scene
INTRO_LAST = 1798
CAMPAIGN_QUIT = f"[bw2] quit at movie frame 1798:{INTRO_LAST} + 600 presents (BW2_QUIT_AFTER_MOVIE)"
INTRO_LAST_SHOWN = "[bw2] bink last frame: 1798"


def run_boot(tmp_path, script, timeout, **env_vars):
    assert ENGINE.exists(), "build it first: python -m bw2.pipeline"
    support = tmp_path / "support"
    path = tmp_path / "input.script"
    path.write_text(script)
    env = dict(os.environ, BW2_SUPPORT=str(support), BW2_HEADLESS="1", BW2_INPUT_SCRIPT=str(path), **env_vars)
    env.pop("BW2_QUIT_AFTER_READS", None)
    r = subprocess.run([str(ENGINE), "--game", str(paths.GAMEFILES / "game")], capture_output=True, text=True,
                       timeout=timeout, env=env)
    (tmp_path / "stderr.txt").write_text(r.stderr)
    return r, engine_lines(r.stderr), support


def frames_of(lines):
    return [tuple(map(int, m.groups())) for m in map(FRAME.match, lines) if m]


def aborted_frames(lines):
    """Frames whose GPU work macOS aborted (the engine's "metal: command buffer failed" line for Metal's
    "Impacting Interactivity" error, which another program's GPU load brings): they show only what was drawn
    before the abort, so the picture checks leave them out (one run under another program's load lost 9 of 79)."""
    return {int(FRAME.match(ln).group(1)) for prev, ln in zip(lines, lines[1:])
            if FRAME.match(ln) and prev.startswith(GPU_ABORT) and "Impacting Interactivity" in prev}


def loud_lines(lines):
    # the loader's own counts ("bound 412 imports, 0 unresolved", "0 unresolved imports") are not stops
    return [ln for ln in lines if any(w in ln for w in LOUD)
            and not (ln.startswith(("[pe] ", "[host] ")) and "0 unresolved" in ln)]


@pytest.mark.gamefiles
def test_boot_profile_to_attract(tmp_path):
    # Plan 6: scripted input names a new profile "ab" in the front end's first dialog and confirms it. The
    # game writes the profile and applies its display settings (a Reset); plan 9 compiles its first effect given
    # as HLSL text; the front end's idle timer then opens its attract video, which plan 10 plays from the game's
    # view of Data/Everything.stuff. BW2_QUIT_AFTER_MOVIE ends the run at the movie's frame 100, a moment its
    # clock sets, whatever the frame rate.
    r, lines, support = run_boot(tmp_path, PROFILE_SCRIPT, 300, BW2_FRAME_LOG="1", BW2_HLSL_LOG="1",
                                 BW2_BINK_LOG="1", BW2_QUIT_AFTER_MOVIE="2033:100")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert lines.count(ATTRACT_QUIT) == 1, tail
    quit_at = lines.index(ATTRACT_QUIT)
    opens = [i for i, ln in enumerate(lines) if ln.startswith(BINK_OPEN)]
    assert [lines[i] for i in opens] == [ATTRACT_OPEN], [lines[i] for i in opens]
    opened = opens[0]
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    # the profile and the settings are written in My Documents, the profile under the typed name
    docs = support / "C" / "Documents and Settings" / "Player" / "My Documents" / "Black & White 2"
    for f in (docs / "Profiles" / "~ab" / "Profile.ini", docs / "Settings.ini"):
        assert f.is_file() and f.stat().st_size > 0, f
    assert not (docs / "Profiles" / "~Player").exists()
    made = sorted(p for p in (support / "Game").rglob("*") if p.is_file()) if (support / "Game").exists() else []
    assert not made, made
    # no lookup that nobody answered beyond those the boot already made before plan 9
    modules = {ln for ln in lines if ln.startswith("[module] ")}
    assert modules <= MODULE_LINES_BEFORE, sorted(modules - MODULE_LINES_BEFORE)
    # the lifted D3DX loads once, at the first HLSL call, with the import count plan 9 Task 4 pinned
    assert lines.count(D3DX_LOADED) == 1, [ln for ln in lines if "d3dx9_25" in ln]
    # one effect, the game's 11199 bytes of HLSL text: S_OK and 2 techniques (the generated one and the filter
    # source's own; docs/research/hlsl.md)
    effects = [i for i, ln in enumerate(lines) if ln.startswith("[bw2] hlsl effect: ")]
    assert len(effects) == 1, effects
    effect = effects[0]
    assert lines[effect].startswith("[bw2] hlsl effect: 11199 bytes -> 0x00000000, "), lines[effect]
    assert ", 2 techniques, " in lines[effect], lines[effect]
    assert lines.index(D3DX_LOADED) < effect
    # the order: the first read, the effect, frames, the movie's open (about 30 s after the effect, on the
    # front end's timer), frames, the quit
    assert lines.count(FIRST_READ) == 1, tail
    first_read = lines.index(FIRST_READ)
    assert first_read < effect < opened < quit_at, (first_read, effect, opened, quit_at)
    assert any(FRAME.match(ln) for ln in lines[effect:opened]), tail
    # the front end's picture, from the first read to the open, is mostly not black in every frame
    window = lines[first_read:opened]
    front = frames_of(window)
    aborted = aborted_frames(window)
    assert front and len(aborted) * 2 < len(front), (len(aborted), len(front))
    dark = [f for f in front if 2 * f[6] < f[7] and f[0] not in aborted]
    assert not dark, dark
    # the movie shows: some frame after the open is lit at least as much as the bound
    movie = [f for f in frames_of(lines[opened:quit_at]) if f[0] not in aborted_frames(lines[opened:quit_at])]
    assert any(f[6] >= ATTRACT_NONBLACK * f[7] for f in movie), max((f[6] / f[7], f[0]) for f in movie)


@pytest.mark.gamefiles
def test_boot_new_campaign(tmp_path):
    # Plan 10: after the profile, a click on New Game loads the first land (about 23 s of the game's own
    # loading, with no Present), plays the intro movie from the game's view of Data/Everything.stuff, and then
    # draws the land's opening scene in 3D. BW2_QUIT_AFTER_MOVIE ends the run 600 Presents after the intro's
    # last frame the game shows (docs/research/campaign.md).
    r, lines, support = run_boot(tmp_path, NEW_GAME_SCRIPT, 900, BW2_FRAME_LOG="1", BW2_BINK_LOG="1",
                                 BW2_QUIT_AFTER_MOVIE=f"1798:{INTRO_LAST}+600")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert lines.count(CAMPAIGN_QUIT) == 1, tail
    quit_at = lines.index(CAMPAIGN_QUIT)
    opens = [i for i, ln in enumerate(lines) if ln.startswith(BINK_OPEN)]
    assert [lines[i] for i in opens] == [INTRO_OPEN], [lines[i] for i in opens]
    opened = opens[0]
    # the game plays the intro to its last frame (Goto clamps every later frame there) and never closes it
    assert lines.count(INTRO_LAST_SHOWN) == 1, tail
    shown = lines.index(INTRO_LAST_SHOWN)
    assert opened < shown < quit_at, (opened, shown, quit_at)
    assert not any(ln.startswith("[bw2] bink close: ") for ln in lines), tail
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    # the land load's uploads never run the GPU out of memory (plan 10 Task 7); macOS's "Impacting
    # Interactivity" aborts are allowed, as in the profile test, and their frames left out below
    oom = [ln for ln in lines if ln.startswith(GPU_ABORT) and "Insufficient Memory" in ln]
    assert not oom, oom
    made = sorted(p for p in (support / "Game").rglob("*") if p.is_file()) if (support / "Game").exists() else []
    assert not made, made
    modules = {ln for ln in lines if ln.startswith("[module] ")}
    # the New Game path makes no lookup that nobody answered beyond those the boot made before plan 9
    assert modules <= MODULE_LINES_BEFORE, sorted(modules - MODULE_LINES_BEFORE)
    # the movie shows: some frame while it plays is lit at least as much as the bound
    window = lines[opened:shown]
    movie = [f for f in frames_of(window) if f[0] not in aborted_frames(window)]
    assert any(f[6] >= MOVIE_NONBLACK * f[7] for f in movie), max(((f[6] / f[7], f[0]) for f in movie), default=None)
    # the land's opening scene: the last 100 frames before the quit are each at least half not black
    window = lines[shown:quit_at]
    aborted = aborted_frames(window)
    land = frames_of(window)
    assert len(land) >= 600, len(land)
    last = [f for f in land[-100:] if f[0] not in aborted]
    assert len(last) * 2 > 100, len(last)
    dark = [f for f in last if 2 * f[6] < f[7]]
    assert not dark, dark


@pytest.mark.gamefiles
def test_boot_close_quits(tmp_path):
    # Plan 6: Cmd-Q and the close button push CLOSE, which reaches the game as Windows delivers its close
    # button and Alt+F4 (WM_SYSCOMMAND SC_CLOSE). The game's own quit path takes it and the game exits 0 by
    # itself, well before the safety stop at read 600 (BW2_QUIT_AFTER_READS).
    assert ENGINE.exists(), "build it first: python -m bw2.pipeline"
    support = tmp_path / "support"
    script = tmp_path / "close.script"
    script.write_text("20 close\n")
    env = dict(os.environ, BW2_SUPPORT=str(support), BW2_HEADLESS="1", BW2_INPUT_SCRIPT=str(script),
               BW2_QUIT_AFTER_READS="600")
    r = subprocess.run([str(ENGINE), "--game", str(paths.GAMEFILES / "game")], capture_output=True, text=True,
                       timeout=180, env=env)
    (tmp_path / "stderr.txt").write_text(r.stderr)
    tail = r.stderr[-4000:]
    lines = engine_lines(r.stderr)
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert not any("BW2_QUIT_AFTER_READS" in ln for ln in lines), tail
    assert lines.count(FIRST_READ) == 1, tail
    err = "\n".join(ln for ln in lines if not (ln.startswith(("[pe] ", "[host] ")) and "0 unresolved" in ln))
    for word in LOUD:
        assert word not in err, f"{word!r} in stderr\n{tail}"
    assert "owned by a later plan" not in r.stderr, tail


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
