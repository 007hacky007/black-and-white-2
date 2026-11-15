"""Where the boot gets to today: the lifted game boots on the patched tree through Direct3D, its DirectInput
mouse setup, its shaders and effects (plan 5), its sprite and fonts and its loading screen (plan 7) and its
DirectSound audio (plan 8) through the interactive Lionhead logo to the front end. Scripts wait for the
front-end music before entering the menus (docs/research/startup.md). With scripted input
(plan 6, BW2_INPUT_SCRIPT) the game takes a new profile in its first dialog, compiles its first HLSL effect
(its tone-mapping filter) through Microsoft's own compiler, lifted from disc 1's d3dx9_25.dll (plan 9), and
draws its front end until its idle timer opens the attract video, which plan 10 plays (Bink through FFmpeg).
A click on New Game instead loads the first land, plays the intro movie to its last frame and draws the
land's opening scene in 3D. Those runs end at a movie frame (BW2_QUIT_AFTER_MOVIE), a moment the movie's clock
sets whatever the frame rate (docs/research/input.md, docs/research/hlsl.md, docs/research/campaign.md).
Plan 11's scripts follow the game, not the frame rate: blocks anchored on the movie events and on wall time, held
keys and a quit verb. With them the front end's options change the resolution and a restart keeps it; Escape
skips the intro and the land's scenes to the creature choice, where a save is made from the pause bar; the
game's own quit (which writes the profile) ends those runs; and the front end's load dialog loads the save back
into the land, at the creature choice, where a New Game still waits for the player (docs/research/saves.md).
Plan 12's scripts also anchor on the files the game asks for (after file). From a loaded save two clicks on the
lion pick it (select, then confirm; measured on the port, not verified on Windows), and a save is taken at the
tutorial offer that follows; loaded back, the script takes the first land's tutorial on moving and looking
around with drags spread over wall time (behind BW2_LONG, like the next test), and (behind BW2_LONG) plays the land to its end and the second land's
load (docs/research/land1.md)."""
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
    # mouse once per frame. End 300 reads after the front-end music lookup.
    assert ENGINE.exists(), "build it first: python -m bw2.pipeline"
    r, lines, support = run_boot(tmp_path, f"after file {FRONT_FILE}\n{READS} quit\n", 180,
                                 BW2_FRAME_LOG="1", BW2_FONT_LOG="1", BW2_AUDIO_LOG="1")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert any(ln.startswith("[bw2] script: quit at read ") for ln in lines), tail
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
    # The logo reads the mouse first; the front-end anchor follows its natural completion.
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


# Plan 12 Task 1: a file the front end asks for after its first read, and only once in the run (the front-end
# music; picked from an HLE_FILETRACE=1 boot to the 300th read, where it is the only CreateFile after that read).
FRONT_FILE = "MENUMUSIC.OGG"
FRONT_ANCHOR = f"after file {FRONT_FILE}"


@pytest.mark.gamefiles
def test_boot_lionhead_logo(tmp_path):
    # The game's interactive logo must run before the front-end music starts.
    r, lines, support = run_boot(tmp_path, "1 move 510 382\nafter seconds 2\n0 down 0\n0 move 100 50\n"
                           "after seconds 1\n0 up 0\n"
                           f"after file {FRONT_FILE}\n0 quit\n", 180, HLE_FILETRACE="1")
    assert r.returncode == 0, r.stderr[-4000:]
    logo = [i for i, ln in enumerate(lines) if asks_for(ln, "logo.png") and ln.endswith(" ok")]
    assert logo, r.stderr[-4000:]
    front = [i for i, ln in enumerate(lines) if asks_for(ln, FRONT_FILE)]
    assert front and logo[0] < lines.index(FIRST_READ) < front[0]
    anchors = [m.group(1) for m in map(SCRIPT_ANCHOR.match, lines) if m]
    assert anchors == ["after seconds 2", "after seconds 1", FRONT_ANCHOR], anchors
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr
    assert not game_files_made(support), game_files_made(support)


@pytest.mark.gamefiles
def test_boot_file_anchor(tmp_path):
    # A script block anchored on a file lookup: the game's own CreateFileA, through guard.c's wrapper, reaches
    # the script's recorder, the block fires and its quit ends the run.
    r, lines, _ = run_boot(tmp_path, f"after file {FRONT_FILE}\n0 quit\n", 300)
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    script = [ln for ln in lines if ln.startswith("[bw2] script: ")]
    assert len(script) == 2, script
    m = re.fullmatch(rf"\[bw2\] script: after file {re.escape(FRONT_FILE)} at read (\d+)", script[0])
    assert m and int(m.group(1)) >= 1 and lines.index(FIRST_READ) < lines.index(script[0]), script
    assert re.fullmatch(r"\[bw2\] script: quit at read \d+", script[1]) and lines.index(script[0]) < lines.index(script[1])
    assert int(script[1].rsplit(" ", 1)[1]) >= int(m.group(1)), script
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail


# Plan 6: the front end's first dialog, driven through the host input queue at DirectInput read counts
# (docs/research/input.md): click the name field, six Backspaces clear the default name, type "ab" (A, B),
# then move to the confirm button and click it. Coordinates and key codes only.
PROFILE_SCRIPT = FRONT_ANCHOR + "\n" + "\n".join(
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
    # The front end's picture, after the logo, is mostly not black in every frame.
    front_start = next(i for i, ln in enumerate(lines) if ln.startswith(f"[bw2] script: {FRONT_ANCHOR} "))
    window = lines[front_start:opened]
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
RESTART_SCRIPT = FRONT_ANCHOR + "\nafter seconds 10\n0 quit\n"


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
    assert anchors == [FRONT_ANCHOR] + [f"after seconds {w}" for w, _ in SETTINGS_STEPS], (anchors, tail)
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
    assert anchors == [FRONT_ANCHOR, "after seconds 10"], (anchors, tail)
    quits = [ln for ln in lines if re.fullmatch(r"\[bw2\] script: quit at read \d+", ln)]
    assert len(quits) == 1, tail
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    # the front end starts at 800x600: the first frame after the first read and every one after it
    assert lines.count(FIRST_READ) == 1, tail
    first_read = next(i for i, ln in enumerate(lines) if ln.startswith(f"[bw2] script: {FRONT_ANCHOR} "))
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
SAVE_BUTTON_ANCHOR = 3 + anchors_in(_skip)
NAME_OK_ANCHOR = 2 + anchors_in(_skip) + anchors_in(_save)
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
_load = [FRONT_ANCHOR] + _load
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


# Plan 12 Task 2: the creature choice, measured on the port, not verified on Windows (docs/research/land1.md). After
# the load (LOAD_STEPS) the game opens the save's files, Creature0.sav last (LOAD_ANCHOR); the land takes the mouse
# about 2.5 s after that lookup (three runs, one with the suite running), so the script waits CHOICE_FIRST s, hovers
# the lion for 2 s and clicks it. That click selects it (a beam over it; the game asks for four art files no install
# holds, OFFCENTRE first, about 0.4 s after the up); CHOICE_GAP s later a second click confirms (every gap from 1 to
# 15 s picked), and about 26 s later, as the advisors offer the tutorials on two scrolls, the game asks for the
# scroll models (SCROLL first). Both clicks go through the same action binding (its pressed and released polls).
# Each click is a down and an up 4 reads later; the clicks are spaced by wall time so that two downs never fall
# within the 500 ms double-click time. Places are 1024x768 client pixels of the game's own cursor (LAND), reached
# through the top left corner as in step_blocks.
LION = (440, 330)
LOAD_ANCHOR = "Creature0.sav"
CHOICE_FIRST = 5
CHOICE_GAP = 5
OFFER_WAIT = 23
SKIP_NO = (598, 409)
CHOSEN_ROW = (390, 280)
OFFCENTRE = "M_OFFCENTREPOINT.bwm"
SCROLL = "M_SCROLL_BRONZE.bwm"
SCROLL_MODELS = ("M_SCROLL_BRONZE.bwm", "M_SCROLL_SILVER.bwm")
CHOICE_TEST_WAIT = 5
# a save after the choice holds one creature, the chosen one: 7 files, not plan 11's 15; the load opens all but
# SaveGame.inf after the OK, Creature0.sav last
CHOSEN_SAVE_FILES = sorted(["Creature0.sav", "Creature0.sav.meta", "Game.sav", "Game.sav.meta", "SaveGame.inf",
                            "Script.sav", "ter.sav"])
CHOSEN_READS = [f for f in CHOSEN_SAVE_FILES if f != "SaveGame.inf"]


def choice_lines(start):
    """Script lines from the load's OK to the scroll models' lookup, and the running point after them. `start`,
    the running point before them, does not change them: the first move puts the cursor in the corner."""
    dx, dy = (math.ceil(v / LAND) for v in LION)
    lines = [f"after file {LOAD_ANCHOR}", f"after seconds {CHOICE_FIRST - 0.5:g}", "0 move -4000 -4000",
             "after seconds 0.5", f"0 move {dx} {dy}", "after seconds 2", "0 down 0", "4 up 0",
             f"after file {OFFCENTRE}", f"after seconds {CHOICE_GAP:g}", "0 down 0", "4 up 0",
             f"after file {SCROLL}"]
    return lines, (min(dx, CLIENT[0] - 1), min(dy, CLIENT[1] - 1))


def chosen_load_lines():
    """Script lines that load chosen_game's save from a boot's start, ending with the anchor on the save's last
    file, and the running point after them."""
    lines, point = step_blocks((0, 0), [(LOAD_FIRST, LOAD_BUTTON), (2, CHOSEN_ROW), (2, LOAD_OK)])
    return [FRONT_ANCHOR] + lines + [f"after file {LOAD_ANCHOR}"], point


def asks_for(line, name):
    """Whether a file trace line is a CreateFileA of a guest path whose last component is `name` (any case)."""
    m = re.match(r'^\[file\] CreateFileA "([^"]*)" -> ', line)
    return bool(m) and re.split(r"[\\/]", m.group(1))[-1].lower() == name.lower()


_load_lines, _load_point = step_blocks((0, 0), LOAD_STEPS)
_load_lines = [FRONT_ANCHOR] + _load_lines
_choice, _choice_point = choice_lines(_load_point)
CHOICE_SCRIPT = "\n".join(_load_lines + _choice + [f"after seconds {CHOICE_TEST_WAIT}", "0 quit"]) + "\n"
CHOICE_ANCHORS = [ln for ln in CHOICE_SCRIPT.splitlines() if ln.startswith("after ")]
# the second click's anchor, counted from 1: the sixth of the choice's seven
SECOND_CLICK_ANCHOR = anchors_in(_load_lines) + 6
# chosen_game: the choice, then OFFER_WAIT s after the scroll lookups (the advisors finish about 13 s after them,
# three runs), a held Escape. At the offer Escape pauses the game and asks whether to skip the land (yes
# (424, 409) skips to the second land); its no (SKIP_NO) leaves the pause bar open over the offer. Then plan 11's
# save, and the quit, whose own save replaces the quit save in Saved Games/1. The chosen save is the third
# folder and the load dialog's second row (CHOSEN_ROW): the rows run newest first.
_offer, _point12 = step_blocks(_choice_point, [(OFFER_WAIT, ESCAPE, 0.5), (2, LAND, SKIP_NO)])
# plan 11's save steps with the keys swapped: the name "ab" is saved_game's, and the game asks before it
# overwrites a save of the same name
CHOSEN_SAVE_STEPS = SAVE_STEPS[:4] + [(2, (0x42, 0x30), 0.2), (0.5, (0x41, 0x1E), 0.2)] + SAVE_STEPS[6:]
_csave, _point12 = step_blocks(_point12, CHOSEN_SAVE_STEPS)
_cquit, _ = step_blocks(_point12, QUIT_STEPS)
CHOSEN_SCRIPT = "\n".join(_load_lines + _choice + _offer + _csave + _cquit) + "\n"
CHOSEN_ANCHORS = [ln for ln in CHOSEN_SCRIPT.splitlines() if ln.startswith("after ")]
CHOSEN_SAVE_ANCHOR = 1 + anchors_in(_load_lines + _choice + _offer)
CHOSEN_QUIT_ANCHOR = 1 + anchors_in(_load_lines + _choice + _offer + _csave)
CHOSEN_WAIT = 30


def drag_blocks(button, start, delta, seconds, steps=20):
    """Script lines for a drag of the game's own cursor (LAND) and its place after it. The lines first put the
    cursor in the top left corner and then at `start` (as step_blocks does; the first line is an event of the
    caller's block), press `button` 4 reads later, then spread the move of `delta` land pixels over `steps`
    blocks of `seconds / steps` wall seconds, so a drag lasts the same at any frame rate; the DirectInput deltas
    add up to delta / LAND exactly. The up comes with the last move. The place is start + delta, kept in the
    client rect."""
    sx, sy = (math.ceil(v / LAND) for v in start)
    out = ["0 move -4000 -4000", "after seconds 0.5", f"0 move {sx} {sy}", f"4 down {button}"]
    done = (0, 0)
    for k in range(1, steps + 1):
        at = tuple(math.floor(d / LAND * k / steps + 0.5) for d in delta)
        out += [f"after seconds {seconds / steps:g}", f"0 move {at[0] - done[0]} {at[1] - done[1]}"]
        done = at
    out.append(f"0 up {button}")
    end = tuple(min(max(math.floor(s + d * LAND + 0.5), 0), c - 1) for s, d, c in zip(start, done, CLIENT))
    return out, end


def wheel_blocks(notches, seconds):
    """`abs(notches)` wheel notches (120 each, negative turns the other way) spread over `seconds` wall seconds."""
    step = 120 if notches > 0 else -120
    return [f"after seconds {seconds / abs(notches):g}", f"0 wheel {step}"] * abs(notches)


def copy_support(saved, tmp_path):
    support = tmp_path / "support"
    shutil.copytree(saved, support)
    return support


@pytest.mark.gamefiles
def test_boot_creature_choice(saved_game, tmp_path):
    # Plan 12 Task 2: the loaded save's creature choice takes two clicks on the lion; the first alone picks
    # nothing, the second makes the game ask for the scroll models.
    support = copy_support(saved_game[2], tmp_path)
    r, lines, _ = run_boot(tmp_path, CHOICE_SCRIPT, 600, support=support, HLE_FILETRACE="1", BW2_FRAME_LOG="1",
                           BW2_BINK_LOG="1")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    quits = [ln for ln in lines if re.fullmatch(r"\[bw2\] script: quit at read \d+", ln)]
    assert len(quits) == 1, tail
    anchor_at = [i for i, ln in enumerate(lines) if SCRIPT_ANCHOR.match(ln)]
    anchors = [SCRIPT_ANCHOR.match(lines[i]).group(1) for i in anchor_at]
    assert anchors == CHOICE_ANCHORS, (anchors, tail)
    # the first click alone picked nothing: no scroll model asked for before the second click's anchor
    second = anchor_at[SECOND_CLICK_ANCHOR - 1]
    early = [ln for ln in lines[:second] if any(asks_for(ln, n) for n in SCROLL_MODELS)]
    assert not early, early
    assert not [ln for ln in lines if ln.startswith(BINK_OPEN)], tail
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    assert not game_files_made(support), game_files_made(support)
    modules = {ln for ln in lines if ln.startswith("[module] ")}
    assert modules <= MODULE_LINES_BEFORE, sorted(modules - MODULE_LINES_BEFORE)


@pytest.fixture(scope="module")
def chosen_game(saved_game, tmp_path_factory):
    """One scripted save taken after the creature choice, at the tutorial offer (plan 12 Task 2), from a copy of
    saved_game's support folder: (result, lines, support, new_save), new_save being the one folder under Saved
    Games that appeared between the save button's first anchor and the pause bar quit's first anchor (None if
    not exactly one)."""
    tmp = tmp_path_factory.mktemp("chosen")
    support = copy_support(saved_game[2], tmp)
    seen, listed = [0], {}

    def on_line(line, support):
        if SCRIPT_ANCHOR.match(line):
            seen[0] += 1
            if seen[0] in (CHOSEN_SAVE_ANCHOR, CHOSEN_QUIT_ANCHOR):
                listed[seen[0]] = folder_names(saved_games(support))
    r, lines, support = run_boot(tmp, CHOSEN_SCRIPT, 900, support=support, on_line=on_line, HLE_FILETRACE="1",
                                 BW2_FRAME_LOG="1", BW2_BINK_LOG="1")
    before, at_quit = listed.get(CHOSEN_SAVE_ANCHOR), listed.get(CHOSEN_QUIT_ANCHOR)
    new = sorted(set(at_quit) - set(before)) if before is not None and at_quit is not None else []
    return r, lines, support, new[0] if len(new) == 1 and set(before) <= set(at_quit) else None


@pytest.mark.gamefiles
def test_boot_chosen_game_loads(chosen_game, tmp_path):
    # Plan 12 Task 2: the save taken at the tutorial offer loads back to it: the game asks for the scroll models.
    r, lines, saved, new_save = chosen_game
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert EXIT_LINE in lines, tail
    assert not any("BW2_QUIT_AFTER" in ln or ln.startswith("[bw2] script: quit") for ln in lines), tail
    anchors = [m.group(1) for m in map(SCRIPT_ANCHOR.match, lines) if m]
    assert anchors == CHOSEN_ANCHORS, (anchors, tail)
    assert new_save is not None, folder_names(saved_games(saved))
    assert save_files(saved_games(saved) / new_save) == CHOSEN_SAVE_FILES, save_files(saved_games(saved) / new_save)
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    assert not game_files_made(saved), game_files_made(saved)

    support = copy_support(saved, tmp_path)
    save = saved_games(support) / new_save
    load, _ = chosen_load_lines()
    script = "\n".join(load + [f"after seconds {CHOSEN_WAIT}", "0 quit"]) + "\n"
    r, lines, _ = run_boot(tmp_path, script, 600, support=support, HLE_FILETRACE="1", BW2_FRAME_LOG="1",
                           BW2_BINK_LOG="1")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    quits = [i for i, ln in enumerate(lines) if re.fullmatch(r"\[bw2\] script: quit at read \d+", ln)]
    assert len(quits) == 1, tail
    anchor_at = [i for i, ln in enumerate(lines) if SCRIPT_ANCHOR.match(ln)]
    anchors = [SCRIPT_ANCHOR.match(lines[i]).group(1) for i in anchor_at]
    assert anchors == [ln for ln in load if ln.startswith("after ")] + [f"after seconds {CHOSEN_WAIT}"], anchors
    # after the OK the game opens the chosen save's 6 files, and asks for the scroll models
    ok_at = anchor_at[anchors_in(load) - 2]
    opened = [ln for ln in lines[ok_at:quits[0]] if ln.startswith("[file] CreateFileA ") and ln.endswith(" ok")]
    for f in CHOSEN_READS:
        assert any(ln.endswith(f" -> {save / f} ok") for ln in opened), (f, [ln for ln in opened if ".sav" in ln])
    others = [ln for ln in opened if f" -> {saved_games(support)}/" in ln and f" -> {save}/" not in ln]
    assert not others, others
    assert any(asks_for(ln, SCROLL) for ln in lines[ok_at:quits[0]]), tail
    assert not [ln for ln in lines if ln.startswith(BINK_OPEN)], tail
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    assert not game_files_made(support), game_files_made(support)


def test_drag_blocks_lines():
    # a drag: the corner, the move to the start, the down 4 reads later, then one move per wall-time block whose
    # DirectInput deltas add up to delta / LAND exactly, and the up with the last move
    lines, end = drag_blocks(1, (300, 600), (200, -10), 2, steps=4)
    assert lines == ["0 move -4000 -4000", "after seconds 0.5", "0 move 400 800", "4 down 1",
                     "after seconds 0.5", "0 move 67 -3", "after seconds 0.5", "0 move 66 -4",
                     "after seconds 0.5", "0 move 67 -3", "after seconds 0.5", "0 move 67 -3", "0 up 1"]
    assert end == (500, 590)
    # the end stays in the client rect
    assert drag_blocks(0, (900, 20), (400, -100), 1, steps=1)[1] == (CLIENT[0] - 1, 0)
    # every step's moves add up to the total for any step count
    for steps in (1, 3, 7, 20, 41):
        lines, _ = drag_blocks(0, (500, 400), (-333, 101), 3, steps=steps)
        moves = [tuple(map(int, ln.split()[2:])) for ln in lines[4:] if " move " in ln]
        assert len(moves) == steps
        assert (sum(m[0] for m in moves), sum(m[1] for m in moves)) == (-444, 135)


def test_wheel_blocks_lines():
    assert wheel_blocks(3, 1.5) == ["after seconds 0.5", "0 wheel 120"] * 3
    assert wheel_blocks(-2, 1) == ["after seconds 0.5", "0 wheel -120"] * 2


# Plan 12 Task 3: the first land's tutorial on moving and looking around, measured on the port
# (docs/research/land1.md). TUTORIAL_FIRST s after the load's last lookup a click on the silver scroll starts it.
# It has four parts, each opened by a hand demo whose file the game asks for as the demo starts (HAND_DEMOS);
# the game reads no input while a demo plays and shows the part's instruction when it ends, so a part's first
# step starts HAND_DEMO_WAIT s after the first read that follows the lookup. The parts: pan left and then right,
# forward and then back (each a drag with the Movement button, MOVEMENT, twice); turn the view (a hold of the
# middle button on the highlighted spot, moved left); pitch it up and then down (the same hold moved down, then
# up); then zoom out and in with the wheel. A part's second step waits on wall time from the first step's last
# drag (the advisors talk in between), and the tutorial's end asks for TUTORIAL_END as the next scene starts.
# The Movement drags follow the cursor, which stops at the screen's edge, so a pan is two screen-wide drags; the
# middle-button holds and the wheel do not move the cursor. Places are 1024x768 client pixels of the game's own
# cursor (LAND); every drag spreads its moves over wall time (drag_blocks).
SILVER_SCROLL = (830, 540)
TUTORIAL_FIRST = 8
HAND_DEMO_WAIT = 8
TUTORIAL_END = "M_SCROLL_GOLD.bwm"
HAND_DEMOS = ("PanLandscapeLeftRight.hnd", "PanLandscapeForwardBack.hnd", "RotateCamera.hnd", "PitchCamera.hnd")
TUTORIAL_TAIL = 10
MOVEMENT, MIDDLE = 1, 2
_tut_click, _ = step_blocks((0, 0), [(TUTORIAL_FIRST, LAND, SILVER_SCROLL)])


def _twice(drag):
    return drag + ["after seconds 1"] + drag


MOVE_TUTORIAL_STEPS = (
    _tut_click
    + ["after file PanLandscapeLeftRight.hnd", f"after seconds {HAND_DEMO_WAIT}"]
    + _twice(drag_blocks(MOVEMENT, (20, 500), (1000, 0), 3, steps=15)[0])
    + ["after seconds 14"] + _twice(drag_blocks(MOVEMENT, (1000, 500), (-980, 0), 3, steps=15)[0])
    + ["after file PanLandscapeForwardBack.hnd", f"after seconds {HAND_DEMO_WAIT}"]
    + _twice(drag_blocks(MOVEMENT, (500, 20), (0, 740), 3, steps=15)[0])
    + ["after seconds 14"] + _twice(drag_blocks(MOVEMENT, (500, 750), (0, -730), 3, steps=15)[0])
    + ["after file RotateCamera.hnd", f"after seconds {HAND_DEMO_WAIT}"]
    + drag_blocks(MIDDLE, (510, 398), (-3600, 0), 10, steps=50)[0]
    + ["after file PitchCamera.hnd", f"after seconds {HAND_DEMO_WAIT}"]
    + drag_blocks(MIDDLE, (484, 337), (0, 2000), 8, steps=40)[0]
    + ["after seconds 12"] + drag_blocks(MIDDLE, (480, 330), (0, -2000), 8, steps=40)[0]
    + ["after seconds 20"] + wheel_blocks(-10, 5)
    + ["after seconds 10"] + wheel_blocks(20, 6)
    + [f"after file {TUTORIAL_END}"])


@pytest.mark.gamefiles
@pytest.mark.long
def test_land_move_tutorial(chosen_game, tmp_path):
    # Behind BW2_LONG since 2026-11-14: its drags are counted in input reads, so it times out when the Mac's
    # GPU is busy, and the user runs land play (docs/research/land1.md).
    # Plan 12 Task 3: from the loaded offer, a click on the silver scroll starts the tutorial on moving and
    # looking around; the script takes each of its steps, and its end makes the game ask for TUTORIAL_END.
    _, _, saved, new_save = chosen_game
    assert new_save is not None, folder_names(saved_games(saved))
    support = copy_support(saved, tmp_path)
    load, _ = chosen_load_lines()
    script = "\n".join(load + MOVE_TUTORIAL_STEPS + [f"after seconds {TUTORIAL_TAIL}", "0 quit"]) + "\n"
    r, lines, _ = run_boot(tmp_path, script, 900, support=support, HLE_FILETRACE="1", BW2_FRAME_LOG="1",
                           BW2_BINK_LOG="1")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    quits = [i for i, ln in enumerate(lines) if re.fullmatch(r"\[bw2\] script: quit at read \d+", ln)]
    assert len(quits) == 1, tail
    anchor_at = [i for i, ln in enumerate(lines) if SCRIPT_ANCHOR.match(ln)]
    anchors = [SCRIPT_ANCHOR.match(lines[i]).group(1) for i in anchor_at]
    expected = [ln for ln in load + MOVE_TUTORIAL_STEPS if ln.startswith("after ")]
    assert anchors == expected + [f"after seconds {TUTORIAL_TAIL}"], (anchors, tail)
    assert expected[-1] == f"after file {TUTORIAL_END}", expected[-1]
    # the end's lookup comes from the tutorial's end: no step before the last one made the game ask for it
    last_step = anchor_at[len(expected) - 2]
    early = [ln for ln in lines[:last_step] if asks_for(ln, TUTORIAL_END)]
    assert not early, early
    # the hand demos open in the tutorial's order, each once, after the scroll's click
    click = anchor_at[anchors_in(load)]
    demos = [i for i, ln in enumerate(lines) if any(asks_for(ln, d) for d in HAND_DEMOS)]
    asked = [d for i in demos for d in HAND_DEMOS if asks_for(lines[i], d)]
    assert asked == list(HAND_DEMOS), asked
    assert all(click < i < anchor_at[-2] for i in demos), (click, demos)
    assert not [ln for ln in lines if ln.startswith(BINK_OPEN)], tail
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    assert not game_files_made(support), game_files_made(support)
    modules = {ln for ln in lines if ln.startswith("[module] ")}
    assert modules <= MODULE_LINES_BEFORE, sorted(modules - MODULE_LINES_BEFORE)


# Plan 12 Task 4: from the loaded offer to the first land's end and the second land's load, measured on the port
# (docs/research/land1.md). The path with the fewest steps: SKIP_FIRST s after the load's last lookup a click on
# the offer's other scroll (SKIP_SCROLL) skips the tutorials, and the scene after them starts at once (the game
# asks for TUTORIAL_END, as at the movement tutorial's end). It ends about 31 s later (the letterbox opened 29.5
# to 30.8 s after that lookup in six runs) with the land's objective: the gold scroll on the mountain, out of
# view. So the script waits SCENE_WAIT s, zooms out ZOOM_OUT notches, puts the cursor on the scroll's foot
# (ZOOM_POINT) and zooms in ZOOM_IN notches, which brings the camera to the scroll and shows it large; a click on
# the scroll (GOLD_SCROLL; its click area lies below the glow) makes the game ask whether to leave the land, and a
# second click CONFIRM_GAP s later confirms. The game writes the creature's mind under Persistant Data, opens the
# second land's files (LAND_TWO_TERRAIN about 3.4 s after the confirm) and reads input again about 15 s later.
# Places are 1024x768 client pixels of the game's own cursor (LAND), reached through the top left corner.
SKIP_SCROLL = (480, 360)
SKIP_FIRST = 8
SCENE_WAIT = 40
ZOOM_OUT, ZOOM_IN = -3, 4
ZOOM_POINT = (771, 259)
GOLD_SCROLL = (540, 342)
GOLD_FIRST = 6
CONFIRM_GAP = 2.5
LAND_TWO_TERRAIN = "Land2.ter"
LAND_TWO_DIR = ("Data", "landscape", "BW2")
CREATURE_MIND = ("Persistant Data", "Creature Mind")
LAND_ONE_TAIL = 30
_skip_click, _ = step_blocks((0, 0), [(SKIP_FIRST, LAND, SKIP_SCROLL)])
_gold_click, _ = step_blocks((0, 0), [(GOLD_FIRST, LAND, GOLD_SCROLL)])
_zx, _zy = (math.ceil(v / LAND) for v in ZOOM_POINT)
LAND_ONE_STEPS = (
    _skip_click
    + [f"after file {TUTORIAL_END}", f"after seconds {SCENE_WAIT}"] + wheel_blocks(ZOOM_OUT, 1.2)
    + ["after seconds 4", "0 move -4000 -4000", "after seconds 0.5", f"0 move {_zx} {_zy}", "after seconds 2"]
    + wheel_blocks(ZOOM_IN, 1.6)
    + _gold_click
    + [f"after seconds {CONFIRM_GAP:g}", "0 down 0", "4 up 0"]
    + [f"after file {LAND_TWO_TERRAIN}"])


def creature_minds(support):
    folder = support.joinpath(*DOCS, *CREATURE_MIND)
    return sorted(str(p.relative_to(folder)) for p in folder.rglob("*") if p.is_file()) if folder.is_dir() else []


@pytest.mark.gamefiles
@pytest.mark.long
def test_land_one_to_two(chosen_game, tmp_path):
    # Plan 12 Task 4: from the loaded offer the script plays the first land to its end; the game carries the
    # creature over and loads the second land, whose terrain file it opens.
    _, _, saved, new_save = chosen_game
    assert new_save is not None, folder_names(saved_games(saved))
    support = copy_support(saved, tmp_path)
    assert creature_minds(support) == [], creature_minds(support)
    load, _ = chosen_load_lines()
    script = "\n".join(load + LAND_ONE_STEPS + [f"after seconds {LAND_ONE_TAIL}", "0 quit"]) + "\n"
    r, lines, _ = run_boot(tmp_path, script, 1800, support=support, HLE_FILETRACE="1", BW2_FRAME_LOG="1",
                           BW2_BINK_LOG="1")
    tail = r.stderr[-4000:]
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    quits = [i for i, ln in enumerate(lines) if re.fullmatch(r"\[bw2\] script: quit at read \d+", ln)]
    assert len(quits) == 1, tail
    quit_at = quits[0]
    anchor_at = [i for i, ln in enumerate(lines) if SCRIPT_ANCHOR.match(ln)]
    anchors = [SCRIPT_ANCHOR.match(lines[i]).group(1) for i in anchor_at]
    expected = [ln for ln in load + LAND_ONE_STEPS if ln.startswith("after ")]
    assert anchors == expected + [f"after seconds {LAND_ONE_TAIL}"], (anchors, tail)
    assert expected[-1] == f"after file {LAND_TWO_TERRAIN}", expected[-1]
    # the second land's terrain opens from the install tree, and only after the confirming click: the first click
    # on the gold scroll alone left nothing
    end_step = anchor_at[len(expected) - 2]
    terrain = "/".join(LAND_TWO_DIR + (LAND_TWO_TERRAIN,))
    asked = [i for i, ln in enumerate(lines) if asks_for(ln, LAND_TWO_TERRAIN)]
    assert asked and asked[0] > end_step, (end_step, asked)
    assert lines[asked[0]].endswith(f" -> {terrain} ok"), lines[asked[0]]
    # the land's end carries the creature over: its mind is written under Persistant Data (one file and its .meta)
    minds = creature_minds(support)
    assert len(minds) == 2 and minds[0] + ".meta" == minds[1] and minds[0].endswith(".erc"), minds
    # the second land is drawn: the last 100 frames before the quit are each at least half not black
    window = lines[asked[0]:quit_at]
    aborted = aborted_frames(window)
    land = frames_of(window)
    assert len(land) >= 100, len(land)
    last = [f for f in land[-100:] if f[0] not in aborted]
    assert len(last) * 2 > 100, len(last)
    dark = [f for f in last if 2 * f[6] < f[7]]
    assert not dark, dark
    assert not [ln for ln in lines if ln.startswith(BINK_OPEN)], tail
    assert not loud_lines(lines), loud_lines(lines)
    assert "owned by a later plan" not in r.stderr, tail
    assert not game_files_made(support), game_files_made(support)
    modules = {ln for ln in lines if ln.startswith("[module] ")}
    assert modules <= MODULE_LINES_BEFORE, sorted(modules - MODULE_LINES_BEFORE)


@pytest.mark.gamefiles
def test_boot_close_quits(tmp_path):
    # Plan 6: Cmd-Q and the close button push CLOSE, which reaches the game as Windows delivers its close
    # button and Alt+F4 (WM_SYSCOMMAND SC_CLOSE). The game's own quit path takes it and the game exits 0 by
    # itself, before the scripted safety quit. Wait through the logo first.
    r, lines, _ = run_boot(tmp_path, FRONT_ANCHOR + "\n20 close\nafter seconds 10\n0 quit\n", 180)
    tail = r.stderr[-4000:]
    lines = engine_lines(r.stderr)
    assert r.returncode == 0, f"exit {r.returncode}\n{tail}"
    assert not any("BW2_QUIT_AFTER_READS" in ln for ln in lines), tail
    assert not any(ln.startswith("[bw2] script: quit") for ln in lines), tail
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
