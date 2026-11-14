# The creature choice and the first land (plan 12)

Plan 12. What the port does from the creature choice through the first land's
movement tutorial to the land's end and the second land's load, as the engine
at the end of plan 12 runs it: headless, on a 32 GB Apple Silicon Mac,
2026-11-13 and 14, often loaded by other work (the load averages are given
where a number depends on them). Addresses are `white.exe`'s. Nothing here
quotes the game's strings, its advisors' lines, its subtitles, its UI words,
its debug output or its files: a step is described in neutral words with its
place. Places are client pixels of the game's own cursor at 1024x768, which
moves 3/4 of each DirectInput delta (`LAND` in `tests/test_boot.py`), so every
scripted move first goes to the top left corner. Frames were looked at as PNGs
(`BW2_FRAME_DUMP`) in scratch folders outside the repo and deleted, as were the
scratch profiles and support copies. The Windows VM was not used in this plan:
it cannot run the game (plan 11: no 3D device), and no Win32 answer the land
needs was unknown.

## The file anchor

Plan 11's script blocks anchor on movie events and wall time. The land's steps
show no movie, so plan 12 adds `after file <name>` (`runtime/bw2/input/script.c`,
its header has the grammar). guard.c's `k_CreateFileA` and `k_CreateFileW`, the
shims the game's imports resolve to, pass every name to `bw2_script_file` after
the device-path check and before forwarding the call untouched; no answer
changes, no table entry was added and `g_allowed` is still empty. The name is
one token of 1 to 259 characters with no `\` or `/`, matched against the last
path component, ASCII case-insensitively.

The window: a file block counts the lookups recorded at or after the anchor
read of the block before it (from the first read for the first block), not
from its own arming. So a lookup caused by the previous block's own events (a
click's down, pushed before its up lets the next block arm) counts, and one
from before the previous block fired does not. The game caches its models, so
a name asked for before the window opened never comes again; every anchor the
tests use was seen asked for first at its step. The log holds one entry per
(name, read) pair, only for names of unfired file blocks, and stops the run
loudly with the read number if it would pass 256 entries.

`test_boot_file_anchor` shows the game's own lookups reach the recorder: the
front end's `MENUMUSIC.OGG`, the only `CreateFile` after the first read in a
front-end boot to read 300, fires at about read 6.

## The creature choice

Measured on the port, not verified on Windows (the VM cannot run the game).

- **Where they stand.** Four creatures in a row: ape about (150, 400), lion
  (440, 330), wolf (655, 370), cow (890, 400). Hovering one lights a beam over
  it and shows a small mouse picture whose left button blinks. On the New Game
  path the other three change pose while one is hovered; on a loaded save they
  do not, and the choice works the same. The tests pick the lion.
- **One click never picks.** Each of these, made 2 to 44 s after hovering the
  lion, left the choice waiting for at least 15 s: a left click (down, up 4
  reads later), left holds of 0.3, 0.5, 3 and 7 s (the 7 s with a drag
  upward), a left double click (taps 4 reads apart), a right click, a right
  double click, a right hold of 0.5 s, a middle hold of 0.3 s, Enter and
  Space; also a click 50 px up and left of the lion, and hovering all four
  for 10 s each before clicking.
- **Two clicks pick.** A left click on the lion and a second left click later
  pick it, on the New Game path and on a loaded save. So did a 3 s hold and a
  10 s hold 15 s apart; a single 10 s hold did not. After the second click the
  camera turns to the lion alone, the advisors appear, and the game asks for
  `M_SCROLL_BRONZE.bwm`, `t_scrolls.dds` and `M_SCROLL_SILVER.bwm` (not found,
  as for other art no install holds); no earlier moment of any run asks for
  them.
- **The gap.** Gaps of 1, 2, 5 and 10 s between the two clicks each picked
  the lion in 3 of 3 runs, and 15 s in 3 more. The shortest is 1 s; the tests
  use 5 s (`CHOICE_GAP`, twice the shortest with a 5 s floor). The clicks are
  spaced by wall time so two downs never fall within the 500 ms double-click
  time, and the choice test asserts that no scroll model was asked for before
  the second click.
- **The first click.** With the cursor put on the lion 1 s after the load's
  last lookup, the hover picture showed 2.3, 2.45 and 2.46 s after that lookup
  (the last with the suite running); a click 3 s after it was taken in all
  three runs. The tests click 5 s after it (`CHOICE_FIRST`). The first click
  asks for `M_OFFCENTREPOINT.bwm`, `t_meteor.dds`, `M_CYLINDER.bwm` and
  `mesh.dds`, on the **release**: the `M_OFFCENTREPOINT.bwm` lookup's read was
  the click block's anchor read plus 30 (3 runs), 28 and 23 (other runs),
  against the up at plus 4, about 0.4 s after the up.
- **The binding evidence** (probes printed from generated shards in a
  throwaway run; the shards were restored byte for byte and the rebuilt
  `out/bw2` had its old sha1). Each click's down is consumed by `sub_0064B590`
  (pressed, bit 0x01, cleared at `0x0064B5DA`) and its up by `sub_0064B640`
  (released, bit 0x04, cleared at `0x0064B68A`), on the same binding object
  (button kind 1), in the read each arrived, for both clicks. The double-click
  check was polled but never set, and no event bit was left at the
  `0x0056D439` clear. The first click leaves a visible selected state: a beam
  over the lion that hovering alone does not show, plus the four art lookups.
  So the two clicks are recorded as the game's two steps, select then confirm,
  measured on the port; nothing was fixed. If Windows takes one click, the
  only cost is a test that passes with a 5 s pause.
- **The clocks** (plan 12 Ruling 8). `GetTickCount`, `timeGetTime`,
  DirectInput time stamps and `QueryPerformanceCounter` count from the Mac's
  boot, as Windows' count from its own. The Mac's uptime was 44 days in the
  spike, so these stand above 2^31 ms; a spike that based them (and RDTSC) on
  the process start did not change the one-click result, and Windows itself
  hands out values above 2^31 ms after 24.9 days. Not changed; a land fault
  that traces to a clock value reopens this with its evidence.

## The input path in the exe

The land reads the mouse only through `IDirectInputDevice8A::GetDeviceData`
from `sub_009AFD20` (a traced load run: no `GetDeviceState`,
`GetAsyncKeyState`, `GetKeyState` or `GetCursorPos` in the land). It asks for
up to 0x200 elements of 0x14 bytes and walks them until an element's time
stamp is 0, adding axis deltas times the sensitivity scale (`[0x1779230]`) and
passing button changes to `sub_009AF310`. That sets bits in the input object
at `0x1D73318`: the button state byte at `+0x110` (`0x1D73428`) and the
per-frame event byte at `+0x15C` (`0x1D73474`). A down within `[0x1D7A664]` ms
(the double-click time, `GetDoubleClickTime` = 500) of the button's previous
down also sets the double-click bit. The action bindings poll these bytes
through `sub_0064B4C0` (held), `sub_0064B590` (pressed), `sub_0064B5F0`
(double) and `sub_0064B640` (released). `0x0056D439` clears the event byte once
per frame, after `sub_009AFD20`. In the probed run a left down set event bit 1
at read 5,274 and `sub_0064B590` consumed it at `0x0064B5DA` in that read.

## The offer, the save after the choice and its load

- **The offer.** The scroll lookups come 26.4 s after the second click in
  every run. About 12 to 13 s later the advisors finish (the subtitle band
  stopped changing 12.2, 12.3 and 12.9 s after the lookups). A silver scroll
  near (830, 540), with the blinking mouse picture, offers the tutorial on
  moving and looking around, and a gold scroll near (480, 360) offers to skip
  the tutorials. The lion sits at about (390, 580).
- **Escape at the offer** pauses the game and asks whether to skip the land
  (23 s and 40 s after the scroll lookups alike). Its yes (424, 409) skips to
  the second land in about 1 s, writing the same creature mind files as the
  land's own end; its no (598, 409) leaves the pause bar open with the offer
  intact, and continue shows the scrolls again.
- **The save** (`chosen_game`): 23 s after the scroll lookups (`OFFER_WAIT`),
  a held Escape, no, then plan 11's save steps with the name "ba" (saved_game's
  save is "ab", and the game asks before overwriting a save of the same name),
  then plan 11's quit. A save after the choice holds 7 files, not plan 11's 15,
  because only the chosen creature is kept: `Creature0.sav` and `.meta`,
  `Game.sav` and `.meta`, `SaveGame.inf`, `Script.sav` and `ter.sav`. It goes
  to `Saved Games/3`; the quit's own save replaces `Saved Games/1`; the load
  dialog lists 1, 3, 2 (newest first), so its second row (390, 280) loads it.
- **The load** opens the 6 files other than `SaveGame.inf` after the OK,
  `Creature0.sav` last; the load dialog itself reads only `Game.sav` and
  `SaveGame.inf`. `after file Creature0.sav` fires at the first read after
  that lookup, 0.8 to 1.0 s later, since the game reads no input while it
  loads. From frames, the load returns to the offer: the lion, both advisors
  and both scrolls, and the game asks for both scroll models during the load.
  So the land tests start from this save, about 1 minute each instead of
  three.

## The movement tutorial

A click on the silver scroll (830, 540) 8 s after the load's last lookup
starts it. It has four parts; each opens with a hand demo whose file the game
asks for once, as the demo starts (from `Data/HandDemo`):
`PanLandscapeLeftRight.hnd`, `PanLandscapeForwardBack.hnd`,
`RotateCamera.hnd`, `PitchCamera.hnd`. The game reads no input while a demo
plays (3 to 11 s between the lookup and the next read) and shows the part's
instruction when reads resume, so a part's first step starts 8 s after that
anchor. The other steps have no lookup of their own and wait on wall time from
the step before (12 to 20 s, each at least the measured talk plus 5 s; the game
waits for the input indefinitely, so a longer wait costs only time).

- **The buttons.** The Movement button is button 1 (the right); a left drag
  of the same size did not pan. Turning and pitching use button 2 (the
  middle), held on the highlighted spot; a hold started elsewhere is refused
  with a spoken correction and no progress.
- **Drags follow the cursor**, which stops at the screen edge, so a Movement
  drag moves the land at most one screen width (an 8 s drag of 4000 px panned
  no further than a 1000 px one), and a pan is two screen-wide drags 1 s
  apart. The middle-button holds and the wheel do not move the cursor, so one
  long hold does: a turn needs about 3200 px in one hold (2400 px was about a
  quarter undone, and a second hold after a release was refused).
- **The steps**, each drag spread over wall time (`drag_blocks`): pan left
  (right-button drag from (20, 500) by +1000, 3 s, twice); pan right (from
  (1000, 500) by -980, twice); forward (from (500, 20) down by 740, twice);
  back (from (500, 750) up by 730, twice); turn (middle hold at (510, 398)
  moved left 3600 over 10 s); pitch up (middle hold at (484, 337) moved down
  2000 over 8 s); pitch down (hold at (480, 330) moved up 2000 over 8 s); zoom
  out (10 wheel notches back over 5 s); zoom in (20 notches forward over 6 s).
  Each step that failed in bring-up was retried with a longer or slower input
  before anything else changed.
- **The next demo's lookup** comes 5 to 11 s after a part's last block
  (`PanLandscapeForwardBack.hnd` about 6 s, `PitchCamera.hnd` about 5.4 s,
  `RotateCamera.hnd` about 15 s). A third pan-right drag left 0 to 2 s and once
  missed the window, which is why pans use two drags, not three.
- **The end.** The game asks for `M_SCROLL_GOLD.bwm` once in a run, as the
  scene after the tutorial starts (with `M_SCROLL_SILVER.bwm` again and about
  35 villager and prop art names), about 11 s after the last wheel notch. The
  test ends on `after file M_SCROLL_GOLD.bwm` and asserts no step before the
  last one caused it.
- **Times** (an idle Mac): the load anchor 15.6 s after the start, the click
  23.6 s, the first demo lookup 28.2 s, the end lookup 263.4 s; about 240 s
  from the click. `test_land_move_tutorial` took 505, 504 and 505 s with its
  fixtures (the third beside the full suite), and 8 min 53 s alone at a load
  of 9 to 19.
- **Frame rate.** The drags are spread over wall time, but the game still has
  to see enough reads during each one. With other sessions holding load
  averages of 17 to 200, the game fell to about 18 reads a second during a
  drag and a step was not taken (three suite runs in Task 4, each failing only
  this test with no exit in 900 s); the same test passed alone and in the next
  suite run. It is the default suite's most load-sensitive test; it failed
  the same way twice more in Task 6 (see "Suite times").

## The land's end and the second land's load

The path with the fewest steps that ends the land through its own end (5
steps, `LAND_ONE_STEPS`, behind `BW2_LONG`):

1. Click the offer's skip-tutorials scroll at (480, 360), 8 s after the load
   anchor. The game goes straight to the scene that follows the movement
   tutorial and asks for `M_SCROLL_GOLD.bwm` about 0.6 s later.
2. Wait 40 s after that lookup. The scene's letterbox opened 29.5 to 30.8 s
   after it in six runs and the game then shows the land's objective; no file
   is asked for at the scene's end (the last lookups, 16 to 20 s in, are
   villager props), so this is a wall-time wait, the slowest plus about 8 s.
3. Zoom out 3 wheel notches (1.2 s).
4. Put the cursor on the gold scroll's foot at (771, 259) and zoom in 4
   notches (1.6 s); the zoom goes toward the cursor and leaves the scroll
   about 45 px tall near (540, 330). Zoomed out, its click area is a few
   pixels, irregular and below its glow (4 hits in 34 mapped places); zoomed
   in, clicks at (535, 330), (550, 342), (522, 344), (537, 358) and (562, 336)
   hit and (520, 318) and (510, 340) missed.
5. Click the scroll at (540, 342) 6 s after the zoom: the game asks whether to
   leave the land (shown about 0.5 s after the click, visible about 5 s). A
   second click on the scroll 2.5 s later confirms (8.6 s and about 21 s also
   confirmed; the asked state outlives the visible question).

What the land's end does, in every passing run: the game writes the creature's
mind as `Persistant Data/Creature Mind/<id>/2-<hex>.erc` (411,674 bytes) and
its `.meta` (32,136 bytes), neither of which the chosen save holds; asks for
`load_screen_0.bmp` (no install holds it), the effects, `Land2.bwe` and
`Land2.ter` (opened ok from `Data/landscape/BW2`) 3.4 to 3.5 s after the
confirm (3 timed runs); then `sky_Land2.dds` (missing), `Land2_walk.xml`,
`Land2.wal`, `Land2.pat` and `Land2.chl` about 11 s later; and reads input
again 14.7 to 15.8 s after the `Land2.ter` lookup. The second land opens with
a letterboxed in-engine scene and no movie (no Bink open up to 150 s after
the load), so no Bink audio track was met (Ruling 7 not triggered) and no
later plan's layer either. The path takes about 90 s from the load anchor to
the terrain anchor; `test_land_one_to_two` took 6:01, 6:01 and 6:04 with its
fixtures (one beside the red run, one beside the full suite) and 7:58 beside
the suite in Task 4.

## The subtitle aid

The game shows its advisors' lines as text along the bottom of the screen when
the profile's `ScriptText` key is 1 (0 in a new profile). Bring-up runs set it
in scratch copies of the support folder to learn each step from the frames;
the copies and frames were deleted. The tests run with the profile as the game
makes it, and no line of the game's text is written down anywhere.

## Suite times

The default suite (`BW2_GAMEFILES=1`, long tests not run) grew from about 11
minutes before plan 12 with the module fixtures `saved_game` and `chosen_game`
and the choice, chosen-load and tutorial boots:

| Run | Result | Wall time | Load (1-minute average) |
|---|---|---|---|
| Task 2 | 594 passed, 4 skipped | 26 min 15 s | not recorded |
| Task 3 | 597 passed, 4 skipped | 29 min 41 s | a tutorial run beside it |
| Task 4 (fourth run) | 597 passed, 5 skipped | 31 min 6 s | about 5 to 12 |
| Task 6 | 592 passed, 5 skipped, 5 errors | 33 min 46 s | 3.5 at the start, up to 218 while `saved_game` ran, 88 at the end |

The long tests (`BW2_LONG=1`) were not run in Task 6: agents run no long play
sessions (user, 2026-11-14). `test_land_one_to_two` is the only one; its
Task 4 times are above.

Task 6's five errors share one cause: the `saved_game` fixture (plan 11's New
Game script, whose Escapes skip the intro and scenes on wall time) ran while
another session drove the load average from 6 to 218, so its skips fell
behind the scenes, the pause-bar clicks landed on the creature choice
instead, and the boot had no exit in 900 s. Every test that uses the fixture
(the save, the load, the choice, `chosen_game` and the tutorial) reported it.
Rerun alone once at a load of 3 to 15 (21 min 50 s), the save, the load, the
choice and the chosen load passed and `test_land_move_tutorial` timed out.
Run alone once more at a load of 4 to 6.5, with a dotnet build at 100 to 200%
CPU and a browser and WindowServer each above 50% (19 min 34 s), it timed out
again. Both times it was at the same step as in Task 4: the pan-right drags
were not taken, so the `PanLandscapeForwardBack.hnd` anchor never fired.
Counted from a drag's first line (the cursor's placing, 4 reads before the
press) to its 15th move block, the first pan-right drag spanned 31 and 27
reads in the two runs and the second 43 and 95, against 67 and 73 (rerun) and
118 and 100 (the run alone) for the two pan-left drags, which were taken. So
in the run alone the second pan-right drag got about as many reads as a
pan-left drag and the step was still not taken, and in the rerun pan-left was
taken with fewer reads than that. Load alone does not explain it: the first
pan-right drag, which starts 14 s after the last pan-left block with no file
anchor, got the fewest reads in both runs, so this step looks marginal. The engine binary was the one
that passed in Tasks 3 and 4 (sha1 34591cf7), and Task 6 changed only
documents and a docstring. Under the user's rule (2026-11-14), the test and the
engine were not changed and the suite was not run again. Whether this drag
needs more wall time per step can only be decided on a quiet Mac.

Open risk for the merge: the default suite has not been shown green on this
branch since Task 4's fourth run. `test_land_move_tutorial` should be run
alone once on a quiet Mac (1-minute load below 25% of `hw.ncpu`, no process
above 50% CPU). If it fails there too, the pan-right step needs more time:
more wall time per drag block, or a longer wait than 14 s before the first
pan-right drag (Task 4's note).

Frame rates: not measured in Task 6, because the Mac was under load (load 3.5
to 218 during the suite; a GPU hashcat job, dotnet builds and a browser ran
beside it). The plan's spike saw about 91 Presents a second at the choice on
an idle Mac; no bound is set from any number here.

## What was absent

In every run of plan 12 (the spike's 20 runs, the bring-up runs of Tasks 2 to
4, among them 23 in Task 3 and about 40 in Task 4, the tests and the traced
runs): no disc, volume or driver question, no tripwire line, no loud line, no
`owned by a later plan` line, no movie after the intro, no `[module]` line
beyond those the boot printed before plan 12 and nothing written under
`<support>/Game/`; the tutorial's runs also showed no new import. The guard's
allow list (`g_allowed`) is still empty.

## For the next plan

Where each plan 12 test ends:

| Test | Ends | How |
|---|---|---|
| `test_boot_file_anchor` | the front end, at the `MENUMUSIC.OGG` lookup (about read 6) | the script's `quit` |
| `test_boot_creature_choice` | the choice made, 5 s after the scroll models' lookup | the script's `quit` |
| `chosen_game` (fixture) | the front end after the save at the offer | the game's quit |
| `test_boot_chosen_game_loads` | the offer, 30 s after the load's `Creature0.sav` lookup | the script's `quit` |
| `test_land_move_tutorial` | the scene after the tutorial, 10 s after the `M_SCROLL_GOLD.bwm` lookup | the script's `quit` |
| `test_land_one_to_two` (long) | the second land's opening scene, 30 s after the `Land2.ter` lookup | the script's `quit` |

The second land loads and draws with no stop; it reads input again about 15 s
after its terrain lookup, while its opening scene plays. What it then waits
for from the player is not measured. The cheapest way into it is the offer's
Escape and the skip question's yes (424, 409), about 1 s and one step from a
`chosen_game` load, which writes the same creature mind files as the land's
own end; `LAND_ONE_STEPS` (five steps, about 90 s) is the way through the
land's end.

Order of the work after plan 12 (user, 2026-11-14): the Builder app and
packaging next, then performance. No more scripted campaign play-through is
scheduled; gameplay faults come from the user's own longer sessions. Bink
audio tracks stay a later plan's stop until a scene opens a movie that has
one.
