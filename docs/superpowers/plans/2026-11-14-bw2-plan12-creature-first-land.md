# Plan 12: The creature choice and the first land Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Scripted headless runs pick a creature in the first land, take the land's first tutorial (moving and looking around), play the land on to its end and load the second land, and keep a land session running for 10 minutes, with no stop. That moves the boot frontier from the creature choice into the campaign (spec section 2, "Done means").

**The choice was not a port bug.** Every single click on a creature failed to pick it in the spike behind this plan, but two separate clicks on the same creature, 15 s apart, pick it, on the New Game path and on a loaded save alike ("Measured" items 3 and 4). The click reaches the game's input and its action bindings in the read it arrives ("Measured" item 5). So this plan is scripting and measuring, with fixes only where a run stops.

**Architecture:**
- **A file anchor for scripts.** Plan 11's anchors are movies and wall time. The land's steps show no movie, but the game asks for files at known moments: the first click on a creature asks for four art files that no install holds, and the choice asks for the scroll models ("Measured" item 4). `BW2_INPUT_SCRIPT` gains `after file <name>`, fed by a thin `CreateFileA` wrapper in `runtime/bw2/mount.c` that forwards the call untouched. An anchor on a game event replaces a guessed wait, and a test that waits for it fails by name if the step did not happen.
- **One save after the choice.** The land tests start from saves, as plan 11's load test does. A second module fixture, `chosen_game`, loads plan 11's `saved_game`, picks the lion, waits for the tutorial offer and saves from the pause bar. The tutorial, land-end and soak tests then load that save: about 1 minute each, not 3.
- **Subtitles as a measuring aid only.** The game shows its advisors' lines as text when the profile's subtitle setting is on ("Measured" item 6). Bring-up runs turn it on in a scratch copy of the profile and read the frames to learn each step. Tests never depend on it, and no line of the game's text is written down; steps are described in neutral words with their places.
- **Next layer:** the second land and the rest of the campaign (plan 13 on), Bink audio tracks when a scene opens one of the 22 movies that have one (Ruling 7), and the Builder app and packaging once the campaign plays through.

**Tech Stack:** C (clang, arm64, macOS 13+), pcrecomp win32hle (vendored, MIT), Python 3.14 and pytest, capstone (already in the venv) for reading the exe during bring-up.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 2, 5.2 Input and Files and saves, 7.4, 9, 11). Facts: `docs/research/saves.md` ("For the next plan"), `campaign.md`, `input.md`.

## Measured before writing (2026-11-13 and 14)

Main at `10f3134`, `out/bw2` relinked (143,624,920 bytes), headless, an idle 32 GB Apple Silicon Mac. About 20 spike runs, most from a scratch copy of the support folder that plan 11's `SAVE_SCRIPT` leaves (load in about 6 s), some on the New Game path (plan 11's `SKIP_STEPS` without the last Escape, about 3 minutes). Two throwaway spike branches (process-relative clocks; probes printed from generated shards at the click consumers) were deleted, the shards restored and main's engine rebuilt; nothing was kept. Frames were looked at as PNGs (`BW2_FRAME_DUMP`) in a scratch folder outside the repo and deleted. Places are client pixels at 1024x768; the game's own cursor in the land moves 3/4 of each DirectInput delta, so every move first goes to the top left corner (plan 11's `step_blocks`).

1. **The boot today.** `BW2_HEADLESS=1 BW2_QUIT_AFTER_READS=300 out/bw2 --game gamefiles/game` ends `[bw2] quit after 300 input reads (BW2_QUIT_AFTER_READS)`, exit 0, 79 lines, no loud line. Without the switch the front end waits for input; every New Game or loaded save waits at the creature choice.
2. **The choice.** Four creatures stand in a row: ape about (150, 400), lion (440, 330), wolf (655, 370), cow (890, 400). Hovering one lights a beam over it and shows a small mouse picture whose left button blinks. On the New Game path the other three change pose while one is hovered; on a loaded save they do not, but the choice works the same.
3. **One click never picks.** Each of these, made 2 to 44 s after hovering the lion, left the choice waiting for at least 15 s: a left click (down, up 4 reads later), left holds of 0.3, 0.5, 3 and 7 s (the 7 s with a drag upward), a left double click (taps 4 reads apart), a right click, a right double click, a right hold of 0.5 s, a middle hold of 0.3 s, Enter and Space; also a click 50 pixels up and left of the lion, and hovering all four creatures 10 s each before clicking. A spike that based `GetTickCount`, `timeGetTime`, `QueryPerformanceCounter` and RDTSC on the process start (the Mac's uptime was 44 days, so these clocks stand above 2^31 ms) did not change this, and was dropped (Ruling 8).
4. **Two clicks pick.** A left click on the lion and a second left click 15 s later (each down, up 4 reads later) picked it in both paths (two runs); so did a 3 s hold and a 10 s hold 15 s apart. A single 10 s hold did not. After the second click the camera turns to the lion alone and the advisors appear with it. The game then asks (`CreateFileA`, not found, as with other art files no install holds) for `M_SCROLL_BRONZE.bwm` and `t_scrolls.dds` and `M_SCROLL_SILVER.bwm`; no earlier moment of any run asked for them. The first click on a creature asks for `M_OFFCENTREPOINT.bwm`, `t_meteor.dds`, `M_CYLINDER.bwm` and `mesh.dds` in the same way. The shortest working gap between the two clicks is not measured.
5. **The click's path in the exe.** The land reads the mouse only through `IDirectInputDevice8A::GetDeviceData` from `sub_009AFD20` (a traced load run: no `GetDeviceState`, `GetAsyncKeyState`, `GetKeyState` or `GetCursorPos` in the land). It asks for up to 0x200 elements of 0x14 bytes and walks them until an element's time stamp is 0, adding axis deltas times the sensitivity scale (`[0x1779230]`) and passing button changes to `sub_009AF310`, which sets bits in the input object at `0x1D73318`: the button state byte at `+0x110` (`0x1D73428`) and the per-frame event byte at `+0x15C` (`0x1D73474`). A down within `[0x1D7A664]` ms (the double-click time, `GetDoubleClickTime` = 500) of the button's previous down also sets the double-click bit. The action bindings poll these bytes through `sub_0064B4C0` (held), `sub_0064B590` (pressed), `sub_0064B5F0` (double) and `sub_0064B640` (released); in the probed run the left down set event bit 1 at read 5,274 and `sub_0064B590` consumed it at `0x0064B5DA` in that read. `0x0056D439` clears the event byte once per frame, after `sub_009AFD20`.
6. **After the choice** (a loaded save, subtitles on in a scratch profile, about 91 Presents a second): the advisors talk, and about 30 s after the second click (frames counted in one run) a silver scroll near (830, 540), with the blinking mouse picture, offers the tutorial on moving and looking around, and a gold scroll offers to skip the tutorials (the text warns that skipping loses tribute). The lion sits at about (390, 580). The subtitle setting is the profile key the game writes as `ScriptText` (0 in a new profile); with it at 1 the lines show in a band along the bottom of the screen.
7. **Bink.** `Data/Everything.stuff` holds 122 Bink movies; 22 have one audio track (the 17 at 512x384, 25 fps, and five at 400x300), `campaign.md`'s count. No run of this spike opened a movie after the intro.
8. **Absent** in every run: any disc, volume or driver question, any tripwire line, any loud line and any `owned by a later plan` line.

**The engine today:**
- `runtime/bw2/input/script.c`: absolute lines, then blocks `after open <frames>`, `after last <frames>`, `after seconds <s>`, verbs `move`, `down`, `up`, `wheel`, `key`, `close`, `quit` (its header holds the grammar). `binkw32.c` feeds the movie anchors through `bw2_script_movie` (`input.h`).
- `runtime/bw2/mount.c` wraps `DeleteFileA`, `RemoveDirectoryA`, `SetFileAttributesA` and `MoveFileA` (table `bw2_mount_shims`), forwarding to win32hle's own shim with `bw2_hle_shim(name)()`. `CreateFileA` is win32hle's (`HLE_FILETRACE=1` prints `[file] CreateFileA "<guest>" -> <host> ok|<error>`).
- `tests/test_boot.py`: `run_boot`, `step_blocks` (running-point clicks, land clicks through the corner, held keys), `saved_game` (module fixture; plan 11's save boot), `LOAD_STEPS`, `SAVE_STEPS`, `QUIT_STEPS`.

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings, bytes, frames or pictures from the game, the game's saves, profiles or settings files, shaders or effects, or generated C. Facts only (addresses, sizes, counts, hashes, coordinates, file names). Tests use `tmp_path`; bring-up folders live outside the repo and are deleted. Run `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .` before every commit that adds data; it must say CLEAN.
- Docs and tests never quote the game's own text: its advisors' lines, subtitles, UI words, debug output or file contents. A step is described in neutral words with its place. File and folder names, API names and addresses are facts. A test may check that a file exists, its size, and which files the game asks for; never a file's text.
- **Standing rule (user):** if the protection VM or the code it runs asks for a disc or driver check, stop and return to the user with the evidence. The tripwire stays on and its allow list (`runtime/bw2/guard.c` `g_allowed`) stays empty. Winsock keeps answering "no network". Online play, the expansion, mods and enhancements are non-goals.
- Fail loudly, never return 0 silently. An anchor that never fires is visible: each firing prints one line and every test asserts its full list.
- **Later plans' layers** each exit 4 with `[bw2] reached <name>: owned by a later plan`; a movie with an audio track exits 4 as `reached Bink audio track` (Ruling 7). `dinput.dll`, `openal32.dll` and `wrap_oal.dll` stay in `g_later_dlls`, as do `DirectDrawCreate` and `DirectDrawEnumerateExA`.
- **`bw2.h` and the win32hle headers do not change** (a change rebuilds 55,000 functions). New cross-file functions go in `runtime/bw2/input/input.h` (a 2-minute host rebuild) or are declared `extern` where used.
- Exit codes are unchanged: 0 normal (including the quit switches and the script's `quit`), 2 boot failure, 3 tripwire, 4 later plan's layer, `abort()` otherwise.
- Headless (`BW2_HEADLESS=1`) for every test. Tests that need game files carry `@pytest.mark.gamefiles` (`BW2_GAMEFILES=1`).
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES` (path relative to `third_party/pcrecomp`). No NFSU2Recomp code, no Wine or ReactOS source. No new Homebrew package is expected; one that becomes needed is recorded as a ruling.
- The Windows VM (winbench) is used only if a Win32 answer the land needs is unknown, through a probe program as in plan 11 (`tools/winprobe/`), under plan 11 Ruling 7's snapshot rule; never with SafeDisc drivers; nothing from it is committed.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Never push, never add a remote, never change git config, never write the user's real name or email anywhere. Work on branch `plan12`.

## Review Focus

- **Two clicks read as one double click.** Two downs closer than 500 ms set the game's double-click bit ("Measured" item 5), and a double click did not pick ("Measured" item 3). A script that spaces its clicks by read counts could fall under 500 ms at a high frame rate. The choice clicks are spaced by wall time, and the choice test asserts that the first click alone asked for no scroll model in its window (Task 2).
- **A file anchor on a name the game asks for only once per run.** The game caches its models: a name asked for before the anchor was armed never comes again, and the test only times out. The anchor counts only lookups after arming (Task 1, case `file_stale`), and each test's anchor names are ones the bring-up saw asked for first at that step.
- **A wrapper that changes an answer.** The `CreateFileA` wrapper sits in front of every file the game opens, in every test. It must forward the call untouched: the same handle kind, the same last error (2 for a missing file, 183 after `OPEN_ALWAYS` on an existing one, as plan 11 measured), the same overlay redirection. (Task 1, `shims_selftest.c` checks `createfile_forward`; the whole suite runs after it.)
- **Drags that depend on the frame rate.** The land is moved and turned by holding a button while moving the mouse; a drag pushed as moves at fixed read numbers lasts 12 ms at one rate and 100 ms at another. Drags are moves spread over wall time (Task 3's `drag_blocks`), and the tutorial test runs three times, once with the Mac loaded.
- **A long land session.** Players spend hours in a land; leaks in textures, buffers, effects or audio grow until the process dies. The soak test samples the engine's resident size and asserts a measured bound on its growth (Task 5).
- **A save taken mid-scene.** `chosen_game` saves while the advisors offer the scrolls. If the load returns to another state (the offer gone, or the choice undone), every later test starts wrong. Task 2's bring-up checks the loaded state from frames, and its test asserts the scroll lookups on load; if the offer is gone, the tests replay the choice instead (Ruling 4).

---

### Task 1: The `after file` anchor

**Files:**
- Modify: `runtime/bw2/mount.c` (a `CreateFileA` wrapper), `runtime/bw2/input/script.c` (the anchor and its parse), `runtime/bw2/input/input.h` (the declaration).
- Test: `runtime/bw2/input/input_selftest.c` (group `script_after`, new cases), `tests/test_input.py` (`AFTER_LINES` and the bad-line cases), `runtime/bw2/shims_selftest.c` (check `createfile_forward`), `tests/test_engine.py::test_shims_selftest`.

**Interfaces:**
- Consumes: plan 11's block rules (arming, firing, anchor read `A`), `bw2_dinput_reads()`.
- Produces:
  - `void bw2_script_file(const char *guest_path)` in `input.h`: records a file event (the last path component, after the last `\` or `/`, and the read counter's value); it pushes and prints nothing. `mount.c` declares it `extern` and calls it from its `CreateFileA` wrapper before forwarding, for every call, whatever the result.
  - Script syntax `after file <name>`: `<name>` is the rest of the line after one space (trailing spaces and a `#` comment removed), 1 to 259 characters with no `\` or `/`. The block fires at the first read after a file event recorded after its arming whose last component equals `<name>`, compared ASCII case-insensitively; `A` is the read counter's value at that event. It prints `[bw2] script: after file <name> at read <A>`, with `<name>` as written.
  - The `CreateFileA` entry in `bw2_mount_shims`: `bw2_script_file(ASTR(0))`, then `bw2_hle_shim("CreateFileA")()`; nothing else.

- [ ] **Step 1: Write the failing checks.** In `input_selftest.c`'s `script_after` group, cases driven like plan 11's (real `GetDeviceData` reads, `bw2_script_file` called between reads):
  - `file`: `1 down 0`, `after file M_SCROLL_BRONZE.bwm`, `0 up 0`: `bw2_script_file("data\\art\\models\\M_OTHER.bwm")` between reads 2 and 3 fires nothing; `bw2_script_file("data\\art\\models\\m_scroll_bronze.BWM")` between reads 3 and 4 makes read 4 print `[bw2] script: after file M_SCROLL_BRONZE.bwm at read 3` and push the up;
  - `file_stale`: the same script; `bw2_script_file` with the name before the first read (the block is armed at read 1, when `1 down 0` is pushed) fires nothing; the same call between reads 2 and 3 makes read 3 print `... at read 2` and push the up;
  - `file_slash`: a name given with `/` separators (`a/b/Game.sav`) matches `after file Game.sav`;
  - bad lines (in `test_script_after_bad`, each stopping the run naming the line): `after file` (no name), `after file a\b`, `after file a/b`, and a 260-character name.
  Add `AFTER_LINES` entries for the three new cases in `tests/test_input.py`. In `shims_selftest.c`, `createfile_forward`: through the mount table's `CreateFileA`, a missing file under `C:\` gives `INVALID_HANDLE_VALUE` and last error 2, a new file with `OPEN_ALWAYS` a handle and last error 0, the same again a handle and 183, and a read-only open of a game-tree file a handle with nothing made under `<support>/Game/`.
- [ ] **Step 2: Run them.** `.venv/bin/pytest tests/test_input.py tests/test_engine.py -k "script or shims_selftest" -v`. Expected: the new `script_after` cases and bad lines FAIL (unknown anchor); `createfile_forward` FAILS only if the table has no `CreateFileA` entry the check can reach (write it so it calls through `bw2_mount_shims`).
- [ ] **Step 3: Implement** `bw2_script_file` and the anchor in `script.c` (the movie events' pattern: a growing array of events, each with its read value; a block's `arm_file` index marks events recorded before arming), the wrapper and its table line in `mount.c`. Update `script.c`'s header comment with the anchor. Relink: `.venv/bin/python -m bw2.pipeline --build-only` (about 2 minutes: `input.h` is in a `BW2_SUBDIRS` folder).
- [ ] **Step 4: Run them, then `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_input.py tests/test_engine.py tests/test_boot.py -k "script or shims or front_end or new_campaign" -q`.** Expected: all pass (the boot tests show the wrapper changes nothing).
- [ ] **Step 5: Commit.** `git commit -m "Anchor input script blocks on file lookups"`

### Task 2: Picking the creature, and a save after it

**Files:**
- Modify: `tests/test_boot.py` (`CHOICE_STEPS`, `CHOICE_SCRIPT`, `test_boot_creature_choice`, fixture `chosen_game`, `test_boot_chosen_game_loads`).
- Modify only if the bring-up finds a stop: the file that owns it, with its check first.

**Interfaces:**
- Consumes: Task 1's `after file`; `saved_game`, `run_boot`, `step_blocks`, `LOAD_STEPS`, `SAVE_STEPS`, `QUIT_STEPS`, `LAND`.
- Produces:
  - `LION = (440, 330)`, `CHOICE_FIRST` (s from the load's OK to the hover; 15 until Step 3 sets it), `CHOICE_GAP` (s between the two clicks; 15 until Step 3 sets it).
  - `choice_lines(start)`: script lines from the load's OK: `after seconds <CHOICE_FIRST - 0.5>`, the corner move, `after seconds 0.5`, the move to `LION / LAND`, `after seconds 2`, a click (`0 down 0`, `4 up 0`), `after file M_OFFCENTREPOINT.bwm` (no events), `after seconds <CHOICE_GAP>`, the second click, `after file M_SCROLL_BRONZE.bwm` (no events). Returns `(lines, point)`.
  - `chosen_game` (module fixture, `tmp_path_factory.mktemp("chosen")`): a copy of `saved_game`'s support folder booted with `LOAD_STEPS` lines, `choice_lines`, `after seconds <OFFER_WAIT>` (Step 3; the scroll offer is up), one held Escape (the pause bar), then `SAVE_STEPS` and `QUIT_STEPS` as plan 11's save does; returns `(result, lines, support, new_save)`, `new_save` being the one folder under `Saved Games` that this boot made before its pause-bar quit (found as plan 11's fixture finds it, by listing at two anchors).

- [ ] **Step 1: Write the failing tests.**
  - `test_boot_creature_choice(saved_game, tmp_path)` (gamefiles, timeout 600 s): copies the save's support folder; boots `LOAD_STEPS` lines + `choice_lines` + `after seconds 5`, `0 quit`, with `HLE_FILETRACE=1 BW2_FRAME_LOG=1 BW2_BINK_LOG=1`. Asserts: exit 0 with the script's quit line; every anchor line in order (so the game asked for `M_SCROLL_BRONZE.bwm` after the second click); no `[file] CreateFileA` line naming `M_SCROLL_BRONZE.bwm` or `M_SCROLL_SILVER.bwm` before the second click's anchor (the first click alone picked nothing); no movie opened; no loud line, no `owned by a later plan`, nothing under `<support>/Game/`, `[module]` lines only from `MODULE_LINES_BEFORE`.
  - `test_boot_chosen_game_loads(chosen_game, tmp_path)` (gamefiles, timeout 600 s): asserts the fixture's boot exited 0 through `[win32hle] ExitProcess(0)` with every anchor and one new save folder holding plan 11's `SAVE_FILES`; then boots a copy of its support folder with a load of `new_save` (its row in the load dialog, Step 3) and `after seconds 30`, `0 quit`, and asserts: the save's 14 files opened after the OK; the scroll models asked for after the OK (`M_SCROLL_BRONZE.bwm`); no loud line.
- [ ] **Step 2: Run them.** `BW2_GAMEFILES=1 .venv/bin/pytest tests/test_boot.py -k "creature_choice or chosen_game" -v`. Expected: FAIL (places and waits not measured).
- [ ] **Step 3: Bring-up**, by hand with `BW2_FRAME_DUMP=<scratch>:100` and a scratch profile with the subtitle key set to 1 (Ruling 3), three runs at least:
  - the shortest `CHOICE_GAP` that picks in three runs out of three (try 2, 5 and 10 s; if 2 s works, check 1 s), then set it to twice that, at least 5 s; record each try;
  - `CHOICE_FIRST`: when the choice takes the first click after the load's OK (the hover beam shows), three runs, one with the full suite running; set it to twice the slowest, at least 10 s;
  - `OFFER_WAIT`: from the scroll lookups to the scroll offer (about 30 s, "Measured" item 6); set to the slowest plus 10 s;
  - the save after the choice: the pause bar opens over the offer, the save and the quit run as in plan 11, the new folder's row in the load dialog (rows from (390, 263) in 17-pixel steps), and from frames that the load returns to the offer (the scrolls, the lion, the advisors). If it returns elsewhere, Ruling 4's fallback applies: later tests replay the choice from `saved_game` instead, `chosen_game` is dropped, and Ruling 4 is amended with the measurement;
  - delete the PNGs and the scratch profile.
- [ ] **Step 4: Run them three times.** Expected: PASS each time.
- [ ] **Step 5: Commit.** `git commit -m "Pick the creature by script and save after it"`

### Task 3: The tutorial on moving and looking around

**Files:**
- Modify: `tests/test_boot.py` (`drag_blocks`, `MOVE_TUTORIAL_STEPS`, `test_land_move_tutorial`).
- Modify only if the bring-up finds a stop: the file that owns it, with its check first.

**Interfaces:**
- Consumes: `chosen_game` (or Ruling 4's fallback), Task 1's anchors, `step_blocks`.
- Produces:
  - `drag_blocks(button, start, delta, seconds, steps=20)`: script lines for a drag with the game's own cursor at `start` (a land place): the corner move and the move to `start` as in `step_blocks`, `0 down <button>`, then `steps` blocks `after seconds <seconds / steps>` each holding one move of `delta / steps / LAND` (the remainder carried so the total is exact), then `0 up <button>`. Returns `(lines, end_place)`, the end clamped to the client rect.
  - `wheel_blocks(notches, seconds)`: `notches` blocks of `after seconds <seconds / notches>`, each `0 wheel 120` (or `-120` for a negative count).
  - `MOVE_TUTORIAL_STEPS` (Step 3): the click on the tutorial's scroll and each step's input, in the step forms above.

- [ ] **Step 1: Write the failing test** `test_land_move_tutorial(chosen_game, tmp_path)` (gamefiles, timeout 900 s): loads `chosen_game`'s save in a copy of its folder, runs `MOVE_TUTORIAL_STEPS`, then `after seconds 10`, `0 quit`, with `HLE_FILETRACE=1 BW2_FRAME_LOG=1 BW2_BINK_LOG=1`. Asserts: exit 0 with the quit line; every anchor in order, ending with the tutorial's end anchor (Step 3: an `after file`, `after open` or `after last` the tutorial's end causes and no step before it; if none exists, `after seconds` on the measured end plus a margin, Ruling 5); no loud line, no `owned by a later plan`, nothing under `<support>/Game/`.
- [ ] **Step 2: Run it.** Expected: FAIL.
- [ ] **Step 3: Bring-up loop**, with subtitles on in a scratch profile and frame dumps, until the tutorial ends: click the silver scroll ("Measured" item 6); for each instruction the game gives, find the input it asks for (a left drag to move, a drag with another button or the wheel to turn or zoom, a double click to fly: whatever the frames show) and the place, and add it to `MOVE_TUTORIAL_STEPS`; a step the game does not take is retried with a slower drag (more `steps`, more `seconds`) before anything else is changed. Record for Task 6: each step's input in neutral words, its place, the time each took, every file the game asked for during the tutorial (the end anchor candidates), and whether any `[module]` or new import line appeared. Each stop in an owned layer is fixed in its file with a check first; a later plan's stop is recorded and becomes the test's end (exit 4); a disc, volume or driver question goes to the user. Delete the PNGs.
- [ ] **Step 4: Run it three times, one with the full suite running in parallel.** Expected: PASS each time.
- [ ] **Step 5: Commit.** `git commit -m "Take the first land's movement tutorial by script"`

### Task 4: The rest of the first land, to the second land's load

**Files:**
- Modify: `tests/test_boot.py` (`LAND_ONE_STEPS`, `test_land_one_to_two`).
- Modify only if the bring-up finds a stop: the file that owns it, with its check first.

**Interfaces:**
- Consumes: `chosen_game`, Task 3's `drag_blocks` and `wheel_blocks`, Task 1's anchors.
- Produces: `LAND_ONE_STEPS` (Step 3): from the loaded offer to the land's end; the test's last anchor is an `after file` on the second land's terrain file as the bring-up sees it asked for (expected `Land2.ter`, under `Data/landscape/BW2/`).

- [ ] **Step 1: Write the failing test** `test_land_one_to_two(chosen_game, tmp_path)` (gamefiles, timeout 1,800 s): loads `chosen_game`'s save, runs `LAND_ONE_STEPS`, then `after seconds 30`, `0 quit` after the second land's terrain anchor. Asserts: exit 0 with the quit line (or, if Step 3 meets a later plan's layer first, exit 4 with that `reached ... owned by a later plan` line as the last engine line, Ruling 7); every anchor in order; the file trace shows the second land's terrain file opened `ok` after the first land's end step; the last 100 frames before the quit at least half not black (as plan 11's load test); no loud line, nothing under `<support>/Game/`.
- [ ] **Step 2: Run it.** Expected: FAIL.
- [ ] **Step 3: Bring-up loop** as in Task 3, from the scroll offer: take the path to the land's end that needs the fewest steps (the skip scroll, if the land can be ended after it; otherwise the land's own tasks, such as the scrolls that teach moving objects and giving villagers work, as the frames show), then the gold scroll that ends the land and whatever the game asks before the second land loads. Record each step (neutral words, place, input, time), the files and movies the land's end and the load ask for, the load's time, any movie with an audio track (Ruling 7) and any stop. If the land cannot be ended within the steps this task measures (a gesture the script grammar cannot express, or more than about 40 steps), stop at the furthest measured point, make that the test's end with its own anchor, and record what remains for plan 13. Delete the PNGs.
- [ ] **Step 4: Run it three times.** Expected: PASS each time.
- [ ] **Step 5: Commit.** `git commit -m "Play the first land to the second by script"`

### Task 5: A 10-minute land session

**Files:**
- Modify: `tests/test_boot.py` (`SOAK_STEPS`, `test_land_soak`, a `resident_kib(pid)` helper using `ps -o rss= -p <pid>`).

**Interfaces:**
- Consumes: `chosen_game`, `drag_blocks`, `wheel_blocks`, `run_boot(..., on_line=...)` (it runs the engine under `Popen`; the helper needs its pid, so `run_boot` gains an `on_start(pid)` callback, called once after the process starts).
- Produces: `SOAK_STEPS`: from the loaded offer, a cycle repeated for 600 s of wall time: drag the land with the move button (Task 3's measured input) 200 pixels right and back, turn it with the turn input both ways, wheel 3 notches in and out, and one click on open ground at a place Step 3 checks is inert (no scroll, creature or advisor there).

- [ ] **Step 1: Write the failing test** `test_land_soak(chosen_game, tmp_path)` (gamefiles, timeout 1,200 s): loads the save, runs `SOAK_STEPS`, then `0 quit`. A thread samples `resident_kib` every 10 s. Asserts: exit 0 with the quit line; every anchor; at least one frame line in every 10 s of the session (no stall); the growth of the resident size from the 2-minute sample to the last at most `SOAK_GROWTH_KIB` (Step 3); no loud line, no `owned by a later plan`, nothing under `<support>/Game/`.
- [ ] **Step 2: Run it.** Expected: FAIL (`SOAK_GROWTH_KIB` unset).
- [ ] **Step 3: Measure.** Two runs: the resident size at each sample, its growth from minute 2 to minute 10, and the frame rate. Set `SOAK_GROWTH_KIB` to twice the larger measured growth, at least 64 MiB. If either run grows by more than 256 MiB over those 8 minutes, find the growing resource (Metal buffers and textures, effect objects, audio voices, guest heap: the engine's own counters, or `heap`/`vmmap` on the process) and fix it with a check first, before setting the bound. Record the numbers for Task 6.
- [ ] **Step 4: Run it twice.** Expected: PASS each time.
- [ ] **Step 5: Commit.** `git commit -m "Keep a land session running for ten minutes"`

### Task 6: The first land, measured

**Files:**
- Create: `docs/research/land1.md`.
- Modify: `docs/research/saves.md` (a pointer in "For the next plan"), the spec (5.2 Input, 11), `tests/test_boot.py` (module docstring).

**Interfaces:**
- Consumes: everything above.
- Produces: the measured record plan 13 starts from.

- [ ] **Step 1: Write `docs/research/land1.md`** (facts only; the game's text never quoted): the choice ("Measured" items 2 to 5 and Task 2's gap and timings), the input path in the exe (item 5), the save after the choice and what its load returns to, the movement tutorial's steps and places (Task 3), the path to the land's end and the second land's load (Task 4), the soak's numbers (Task 5), the subtitle aid (Ruling 3), the clock fact (Ruling 8), what was absent (no disc, volume or driver question, no tripwire), and "For the next plan": where each test ends and what the second land waits for.
- [ ] **Step 2: Update the spec.** 5.2 Input: a "Plan 12" note (the `after file` anchor; the creature choice is two clicks, measured; drags spread over wall time). 11 "Left to the builder": nothing new unless Task 3 to 5 added a ruling that belongs there.
- [ ] **Step 3: Run the full suite.** `BW2_GAMEFILES=1 .venv/bin/pytest -q > .superpowers/sdd/plan12-suite.txt 2>&1; tail -3 .superpowers/sdd/plan12-suite.txt`. Expected: all pass.
- [ ] **Step 4: Audit and commit.** `.venv/bin/python third_party/pcrecomp/tools/audit_repo.py .` reports CLEAN. `git commit -m "Record the first land's paths"`

## Rulings (my calls; the spec leaves them to the builder)

1. **Scope and number.** Plan 6 (input) is done and merged, so this is plan 12. The boot has no stop of its own: every New Game and loaded save waits at the creature choice, which the spike showed is two clicks, not a fault ("Measured" items 3 and 4). So the next layer is the game itself: the choice, the first land's movement tutorial and the land's end, with a soak for long sessions. The Builder app and packaging come once the campaign plays through, as plan 11 Ruling 1 set; the second land onward is plan 13.
2. **File anchors.** The land's steps show no movie, and wall-time waits are guesses that fail on a busy Mac. The game's file lookups are events the engine already sees at exact moments, so scripts anchor on them. The wrapper only records and forwards: no answer changes.
3. **Subtitles only for measuring.** Bring-up runs set the profile's subtitle key in a scratch copy of the support folder so the frames show the advisors' instructions; the copy and the frames are deleted. Tests run with the profile as the game makes it, and no subtitle text is written down anywhere.
4. **Tests start from saves.** One `chosen_game` save (made once per test session from plan 11's `saved_game`) feeds Tasks 3 to 5, so each costs about a minute plus its own steps instead of three minutes of New Game. If a load does not return to the scroll offer, the tests replay the choice from `saved_game` instead (about 1 minute more each).
5. **End markers in order of preference:** a file the step's end asks for, a movie open or last frame, then (only if none exists) a wall-time anchor sized from three measured runs, at least twice the slowest. Each test records which kind it uses.
6. **The lion.** Tests pick the lion (the second creature from the left). The four are alike in play (the game says so in its own way, and guides agree); one is enough for the scripts.
7. **Bink audio stays a later plan's layer** until a run opens one of the 22 movies with an audio track. If Task 4's path opens one, the test ends there (exit 4, `reached Bink audio track`) and plan 13 answers it first; this plan does not build it ahead of need.
8. **Clocks keep the Mac's uptime.** `GetTickCount`, `timeGetTime`, DirectInput time stamps and `QueryPerformanceCounter` count from the Mac's boot, as Windows' count from its own. A process-relative clock did not change the choice ("Measured" item 3), and Windows itself hands out values above 2^31 ms after 24.9 days. Not changed; a land fault that traces to a clock value reopens this with its evidence.
9. **Coordinates and file names are facts.** Places on the screen and the names of files the game asks for are committed in tests and docs, like addresses; the frames, the game's words and its files are not.
</content>
</invoke>
