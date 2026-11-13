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
import math
import os
import re
import shutil
import subprocess
import threading

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


def run_boot(tmp_path, script, timeout, support=None, on_line=None, **env_vars):
    """One headless boot with a fresh support folder under tmp_path, or with `support` used as it is. With
    `on_line`, each engine line of stderr is passed to on_line(line, support) as it arrives."""
    assert ENGINE.exists(), "build it first: python -m bw2.pipeline"
    support = tmp_path / "support" if support is None else support
    path = tmp_path / "input.script"
    path.write_text(script)
    env = dict(os.environ, BW2_SUPPORT=str(support), BW2_HEADLESS="1", BW2_INPUT_SCRIPT=str(path), **env_vars)
    env.pop("BW2_QUIT_AFTER_READS", None)
    cmd = [str(ENGINE), "--game", str(paths.GAMEFILES / "game")]

    def timed_out(err):
        # a click that missed leaves the game waiting: fail with the script's lines (each fired anchor) and the tail
        (tmp_path / "stderr.txt").write_text(err)
        fired = [ln for ln in engine_lines(err) if ln.startswith("[bw2] script: ")]
        pytest.fail(f"no exit in {timeout} s; script lines {fired}\n{err[-4000:]}")

    if on_line is None:
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        except subprocess.TimeoutExpired as e:
            timed_out(e.stderr.decode("utf-8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or ""))
    else:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace",
                             env=env)
        out, err, expired = [], [], threading.Event()

        def kill():
            expired.set()
            p.kill()
        timer = threading.Timer(timeout, kill)
        reader = threading.Thread(target=lambda: out.append(p.stdout.read()))
        timer.start()
        reader.start()
        try:
            for raw in p.stderr:
                err.append(raw)
                for part in engine_lines(raw):
                    on_line(part, support)
            p.wait()
        finally:
            timer.cancel()
            if p.poll() is None:
                p.kill()
                p.wait()
            reader.join()
        if expired.is_set():
            timed_out("".join(err))
        r = subprocess.CompletedProcess(cmd, p.returncode, "".join(out), "".join(err))
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


# Plan 11 Task 3: settings. After the profile (the running point at (422, 427)) the script waits SETTINGS_WAIT s
# for the front end to take input, then clicks, each click 2 s after the one before: options (766, 711), the
# video tab (452, 181), the 800x600 row (448, 260), exit (688, 573) and yes to saving the changes (424, 409), all
# 1024x768 places. The game resets its device to 800x600 and asks at once whether to keep the new size (it
# reverts by itself after a while); the script answers yes (312, 325), and 3 s later clicks the front end's quit
# button (731, 555) and its confirm yes (312, 325), all measured at 800x600 (docs/research/saves.md). Each move
# is a delta from the running point; a click is a down 4 reads after the move and an up 4 reads after that.
SETTINGS_WAIT = 3
SETTINGS_CLICKS = [(766, 711), (452, 181), (448, 260), (688, 573), (424, 409)]
SETTINGS_KEEP = (312, 325)
SETTINGS_QUIT = [(731, 555), (312, 325)]
PROFILE_POINT = (422, 427)
SCRIPT_ANCHOR = re.compile(r"^\[bw2\] script: (after .+) at read (\d+)$")
EXIT_LINE = "[win32hle] ExitProcess(0)"


def click_blocks(start, waits_and_places):
    """Script blocks, one per (wait, place): `after seconds <wait>`, a move from the running point to the place,
    a down 4 reads later and an up 4 reads after that."""
    out, (x, y) = [], start
    for wait, (px, py) in waits_and_places:
        out += [f"after seconds {wait}", f"0 move {px - x} {py - y}", "4 down 0", "8 up 0"]
        x, y = px, py
    return out


SETTINGS_STEPS = ([(SETTINGS_WAIT, SETTINGS_CLICKS[0])] + [(2, p) for p in SETTINGS_CLICKS[1:]]
                  + [(2, SETTINGS_KEEP), (3, SETTINGS_QUIT[0]), (2, SETTINGS_QUIT[1])])
SETTINGS_SCRIPT = PROFILE_SCRIPT + "\n".join(click_blocks(PROFILE_POINT, SETTINGS_STEPS)) + "\n"
RESTART_SCRIPT = "after seconds 10\n0 quit\n"


def game_files_made(support):
    return sorted(p for p in (support / "Game").rglob("*") if p.is_file()) if (support / "Game").exists() else []


@pytest.mark.gamefiles
def test_boot_settings_persist(tmp_path):
    # Plan 11 Task 3: the front end's options change the resolution to 800x600, the game's own quit writes the
    # settings, and a second boot on the same support folder starts at 800x600 (docs/research/saves.md).
    r, lines, support = run_boot(tmp_path, SETTINGS_SCRIPT, 300, BW2_FRAME_LOG="1")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert EXIT_LINE in lines, tail
    assert not any("BW2_QUIT_AFTER" in ln or ln.startswith("[bw2] script: quit") for ln in lines), tail
    anchors = [m.group(1) for m in map(SCRIPT_ANCHOR.match, lines) if m]
    assert anchors == [f"after seconds {w}" for w, _ in SETTINGS_STEPS], (anchors, tail)
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    # the device goes from 1024x768 to 800x600 once and stays there to the quit
    sizes = [(f[1], f[2]) for f in frames_of(lines)]
    runs = [s for i, s in enumerate(sizes) if i == 0 or s != sizes[i - 1]]
    assert runs == [(1024, 768), (800, 600)], runs
    # the game's quit writes the settings and the profile in My Documents
    docs = support / "C" / "Documents and Settings" / "Player" / "My Documents" / "Black & White 2"
    for f in (docs / "Settings.ini", docs / "Profiles" / "~ab" / "Profile.ini"):
        assert f.is_file() and f.stat().st_size > 0, f
    assert not game_files_made(support), game_files_made(support)

    # boot 2 on the same folder: no input, a quit 10 s after the first read
    boot2 = tmp_path / "boot2"
    boot2.mkdir()
    r, lines, _ = run_boot(boot2, RESTART_SCRIPT, 300, support=support, BW2_FRAME_LOG="1", BW2_HLSL_LOG="1")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    anchors = [m.group(1) for m in map(SCRIPT_ANCHOR.match, lines) if m]
    assert anchors == ["after seconds 10"], (anchors, tail)
    quits = [ln for ln in lines if re.fullmatch(r"\[bw2\] script: quit at read \d+", ln)]
    assert len(quits) == 1, tail
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    # the front end starts at 800x600: the first frame after the first read and every one after it
    assert lines.count(FIRST_READ) == 1, tail
    first_read = lines.index(FIRST_READ)
    after = frames_of(lines[first_read:])
    assert after and all((f[1], f[2]) == (800, 600) for f in after), sorted({(f[1], f[2]) for f in after})
    # no profile dialog: a reused folder compiles the one HLSL effect before the first read
    effects = [i for i, ln in enumerate(lines) if ln.startswith("[bw2] hlsl effect: ")]
    assert len(effects) == 1 and effects[0] < first_read, (effects, first_read)
    assert not game_files_made(support), game_files_made(support)


# Plan 11 Task 4: a save from the first land. After New Game (the running point at (252, 711)) the script waits for
# the intro to open, then skips it and the land's scenes with ESCAPES held Escapes (each down 0.5 s, 1.5 s apart):
# the first skips the intro, 12 more the land's scenes, and the surplus toggles the pause bar over the live
# creature choice, so a click at the bar's continue place closes it if it is open, and one more Escape opens it
# (plan 11 Rulings 4 and 5). Then: the bar's save button, the dialog's new-save row and OK, which opens a naming
# dialog; a click in its name field (which empties it), the keys A and B (each held 0.2 s) and its OK, which
# writes the save; a wait of SAVE_WAIT s; the bar's quit and yes to its question (the game writes a save of its
# own and goes to the front end); then the front end's quit and yes. From the land on the game's cursor moves 3/4
# of each DirectInput delta, so every click puts it in the top left corner first (step_blocks). Places are
# 1024x768 client pixels (docs/research/saves.md).
ESCAPE = (0x1B, 0x01)
ESCAPES = 18
# the share of each DirectInput delta the game's own cursor moves once the land has loaded, in the land and in
# the front end after it (measured; before the land the front end's cursor is the running point)
LAND = 0.75
CLIENT = (1024, 768)
CONTINUE = (85, 710)
SAVE_BUTTON = (425, 710)
NEW_SAVE_ROW = (390, 263)
SAVE_OK = (409, 547)
NAME_FIELD = (510, 382)
NAME_OK = (422, 427)
SAVE_WAIT = 20
PAUSE_QUIT = (936, 710)
QUIT_YES = (424, 409)
FRONT_QUIT = (936, 711)
NEW_GAME_POINT = (252, 711)
DOCS = ("C", "Documents and Settings", "Player", "My Documents", "Black & White 2")
# the files a save holds, the name OK's and the quit's alike (measured; the New Game autosave holds the last five)
SAVE_FILES = sorted([f"Creature{i}.sav{m}" for i in range(5) for m in ("", ".meta")]
                    + ["Game.sav", "Game.sav.meta", "SaveGame.inf", "Script.sav", "ter.sav"])


def step_blocks(start, steps):
    """Script blocks and the running point after them. A step is one of:
    (wait, (x, y)), a click at a running-point place (the front end before the land): `after seconds <wait>`, a
    move from the running point, a down 4 reads later and an up 4 reads after that;
    (wait, scale, (x, y)), a click at a place of the game's own cursor, which moves `scale` of each DirectInput
    delta (LAND) and stops at the screen's edges: `after seconds <wait - 0.5>`, a move far up and left that puts it
    in the top left corner, `after seconds 0.5`, a move of (x, y) / scale, then the down and the up;
    (wait, (vk, scan), hold), a held key: `after seconds <wait>`, the key down, `after seconds <hold>`, the up."""
    out, (x, y) = [], start
    for step in steps:
        if len(step) == 2:
            wait, (px, py) = step
            out += [f"after seconds {wait}", f"0 move {px - x} {py - y}", "4 down 0", "8 up 0"]
            x, y = px, py
        elif isinstance(step[1], float):
            wait, scale, (px, py) = step
            dx, dy = (math.ceil(v / scale) for v in (px, py))
            out += [f"after seconds {wait - 0.5:g}", "0 move -4000 -4000", "after seconds 0.5", f"0 move {dx} {dy}",
                    "4 down 0", "8 up 0"]
            x, y = min(dx, CLIENT[0] - 1), min(dy, CLIENT[1] - 1)
        else:
            wait, (vk, scan), hold = step
            key = f"key 0x{vk:02X} 0x{scan:02X}"
            out += [f"after seconds {wait}", f"0 {key} down", f"after seconds {hold}", f"0 {key} up"]
    return out, (x, y)


def anchors_in(lines):
    return sum(ln.startswith("after ") for ln in lines)


SKIP_STEPS = ([(3, ESCAPE, 0.5)] + [(1.5, ESCAPE, 0.5)] * (ESCAPES - 1) + [(5, LAND, CONTINUE), (2, ESCAPE, 0.5)])
SAVE_STEPS = [(2, LAND, SAVE_BUTTON), (2, LAND, NEW_SAVE_ROW), (2, LAND, SAVE_OK), (2, LAND, NAME_FIELD),
              (2, (0x41, 0x1E), 0.2), (0.5, (0x42, 0x30), 0.2), (2, LAND, NAME_OK)]
QUIT_STEPS = [(SAVE_WAIT, LAND, PAUSE_QUIT), (2, LAND, QUIT_YES), (5, LAND, FRONT_QUIT), (2, LAND, QUIT_YES)]
_skip, _point = step_blocks(NEW_GAME_POINT, SKIP_STEPS)
SKIP_TO_CHOICE = "\n".join(["after open 1798"] + _skip) + "\n"
_save, _point = step_blocks(_point, SAVE_STEPS)
_quit, _ = step_blocks(_point, QUIT_STEPS)
SAVE_SCRIPT = NEW_GAME_SCRIPT + SKIP_TO_CHOICE + "\n".join(_save + _quit) + "\n"
# the anchors the save script prints, in order, and three of them counted from 1: the save button's first (after
# the intro's open and the skip's), the name OK's click (the save's last block), and the pause bar quit's first
SAVE_ANCHORS = [ln for ln in SAVE_SCRIPT.splitlines() if ln.startswith("after ")]
SAVE_BUTTON_ANCHOR = 2 + anchors_in(_skip)
NAME_OK_ANCHOR = 1 + anchors_in(_skip) + anchors_in(_save)
PAUSE_QUIT_ANCHOR = NAME_OK_ANCHOR + 1


def saved_games(support):
    return support.joinpath(*DOCS, "Profiles", "~ab", "Saved Games")


def folder_names(folder):
    return sorted(p.name for p in folder.iterdir()) if folder.is_dir() else []


@pytest.fixture(scope="module")
def saved_game(tmp_path_factory):
    """One scripted save from the first land (plan 11 Task 4), shared by the save and load tests: (result, lines,
    support, before, at_quit), `before` and `at_quit` being the names under Saved Games when the save button's
    first anchor and the pause bar quit's first anchor fired."""
    tmp = tmp_path_factory.mktemp("save")
    seen, listed = [0], {}

    def on_line(line, support):
        if SCRIPT_ANCHOR.match(line):
            seen[0] += 1
            if seen[0] in (SAVE_BUTTON_ANCHOR, PAUSE_QUIT_ANCHOR):
                listed[seen[0]] = folder_names(saved_games(support))
    r, lines, support = run_boot(tmp, SAVE_SCRIPT, 900, on_line=on_line, HLE_FILETRACE="1", BW2_FRAME_LOG="1",
                                 BW2_BINK_LOG="1")
    return r, lines, support, listed.get(SAVE_BUTTON_ANCHOR), listed.get(PAUSE_QUIT_ANCHOR)


def save_files(save):
    return sorted(str(p.relative_to(save)) for p in save.rglob("*") if p.is_file())


@pytest.mark.gamefiles
def test_boot_save_game(saved_game):
    # Plan 11 Task 4: the script skips the intro and the land's scenes, saves from the pause bar over the live
    # creature choice, and quits by the game's own path (docs/research/saves.md).
    r, lines, support, before, at_quit = saved_game
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert EXIT_LINE in lines, tail
    assert not any("BW2_QUIT_AFTER" in ln or ln.startswith("[bw2] script: quit") for ln in lines), tail
    anchor_at = [i for i, ln in enumerate(lines) if SCRIPT_ANCHOR.match(ln)]
    anchors = [SCRIPT_ANCHOR.match(lines[i]).group(1) for i in anchor_at]
    assert anchors == SAVE_ANCHORS, (anchors, tail)
    # the intro opened once and was skipped: it never showed its last frame
    opens = [ln for ln in lines if ln.startswith(BINK_OPEN)]
    assert opens == [INTRO_OPEN], opens
    assert INTRO_LAST_SHOWN not in lines, tail
    # by the pause bar's quit, one new folder under Saved Games, holding the save's files, each non-empty
    assert before is not None and at_quit is not None, (before, at_quit)
    folder = saved_games(support)
    new = sorted(set(at_quit) - set(before))
    assert len(new) == 1 and set(before) <= set(at_quit), (before, at_quit)
    save = folder / new[0]
    files = save_files(save)
    assert files == SAVE_FILES, files
    empty = [f for f in files if (save / f).stat().st_size == 0]
    assert not empty, empty
    # the file trace shows each of them created for writing (all but SaveGame.inf in the folder's temp folder,
    # which the game then moves into place) after the name OK's click and before the quit
    ok_at, quit_at = anchor_at[NAME_OK_ANCHOR - 1], anchor_at[PAUSE_QUIT_ANCHOR - 1]
    made = [ln for ln in lines[ok_at:quit_at] if ln.startswith("[file] CreateFileA ") and ln.endswith(" ok")]
    for f in files:
        hosts = (f" -> {save / 'temp' / f} ok", f" -> {save / f} ok")
        assert any(ln.endswith(hosts) for ln in made), (f, [ln for ln in made if new[0] in ln][:20])
    # the quit writes one more save folder of its own, with the same file set
    final = folder_names(folder)
    more = sorted(set(final) - set(at_quit))
    assert len(more) == 1 and set(at_quit) <= set(final), (at_quit, final)
    assert save_files(folder / more[0]) == SAVE_FILES, save_files(folder / more[0])
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    assert not game_files_made(support), game_files_made(support)
    modules = {ln for ln in lines if ln.startswith("[module] ")}
    assert modules <= MODULE_LINES_BEFORE, sorted(modules - MODULE_LINES_BEFORE)


# Plan 11 Task 5: the load. A boot on a copy of the save's support folder starts in the front end with the
# profile (no profile dialog); there the game's cursor is still the running point, which starts at (0, 0). The
# script waits LOAD_FIRST s, clicks the front end's load button, the save's row in the load dialog and the dialog's
# OK, each 2 s after the one before, then quits LOAD_WAIT s later, with the land drawn. The dialog lists the
# quit's save first and the test's save (the one Task 4's script named) second. Places are 1024x768 client
# pixels (docs/research/saves.md).
LOAD_FIRST = 3
LOAD_BUTTON = (425, 711)
LOAD_ROW = (390, 280)
LOAD_OK = (409, 547)
LOAD_WAIT = 60
LOAD_STEPS = [(LOAD_FIRST, LOAD_BUTTON), (2, LOAD_ROW), (2, LOAD_OK)]
_load, _ = step_blocks((0, 0), LOAD_STEPS)
LOAD_SCRIPT = "\n".join(_load + [f"after seconds {LOAD_WAIT}", "0 quit"]) + "\n"
LOAD_ANCHORS = [ln for ln in LOAD_SCRIPT.splitlines() if ln.startswith("after ")]
# the anchor of the dialog's OK, counted from 1
LOAD_OK_ANCHOR = anchors_in(_load)
# the save's files the load opens after the OK (measured): all but SaveGame.inf, which the dialog's listing reads
LOAD_READS = [f for f in SAVE_FILES if f != "SaveGame.inf"]


@pytest.mark.gamefiles
def test_boot_load_game(saved_game, tmp_path):
    # Plan 11 Task 5: the front end's load dialog loads the save Task 4's script made back into the first land
    # (docs/research/saves.md).
    _, _, saved, before, at_quit = saved_game
    assert before is not None and at_quit is not None, (before, at_quit)
    new = sorted(set(at_quit) - set(before))
    assert len(new) == 1, (before, at_quit)
    support = tmp_path / "support"
    shutil.copytree(saved, support)
    save = saved_games(support) / new[0]
    stamps = {f: ((save / f).stat().st_size, (save / f).stat().st_mtime_ns) for f in save_files(save)}
    r, lines, _ = run_boot(tmp_path, LOAD_SCRIPT, 600, support=support, HLE_FILETRACE="1", BW2_FRAME_LOG="1",
                           BW2_BINK_LOG="1")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    quits = [i for i, ln in enumerate(lines) if re.fullmatch(r"\[bw2\] script: quit at read \d+", ln)]
    assert len(quits) == 1, tail
    quit_at = quits[0]
    anchor_at = [i for i, ln in enumerate(lines) if SCRIPT_ANCHOR.match(ln)]
    anchors = [SCRIPT_ANCHOR.match(lines[i]).group(1) for i in anchor_at]
    assert anchors == LOAD_ANCHORS, (anchors, tail)
    # after the dialog's OK, the game opens the save's files, and no file of another save
    ok_at = anchor_at[LOAD_OK_ANCHOR - 1]
    opened = [ln for ln in lines[ok_at:quit_at] if ln.startswith("[file] CreateFileA ") and ln.endswith(" ok")]
    for f in LOAD_READS:
        assert any(ln.endswith(f" -> {save / f} ok") for ln in opened), (f, [ln for ln in opened if ".sav" in ln])
    others = [ln for ln in opened if f" -> {saved_games(support)}/" in ln and f" -> {save}/" not in ln]
    assert not others, others
    # ... for reading: every file of the save keeps its size and time
    after = {f: ((save / f).stat().st_size, (save / f).stat().st_mtime_ns) for f in save_files(save)}
    assert after == stamps, sorted(set(after.items()) ^ set(stamps.items()))
    # no New Game: no movie opens (the intro plays only on New Game)
    opens = [ln for ln in lines if ln.startswith(BINK_OPEN)]
    assert opens == [], opens
    # the land is drawn: the last 100 frames before the quit are each at least half not black
    window = lines[ok_at:quit_at]
    aborted = aborted_frames(window)
    land = frames_of(window)
    assert len(land) >= 100, len(land)
    last = [f for f in land[-100:] if f[0] not in aborted]
    assert len(last) * 2 > 100, len(last)
    dark = [f for f in last if 2 * f[6] < f[7]]
    assert not dark, dark
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    assert not game_files_made(support), game_files_made(support)
    modules = {ln for ln in lines if ln.startswith("[module] ")}
    assert modules <= MODULE_LINES_BEFORE, sorted(modules - MODULE_LINES_BEFORE)


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
