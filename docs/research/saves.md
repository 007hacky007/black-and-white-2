# Saves, loading and settings

Plan 11. The save path is the game's own code running on win32hle's file calls
and ours (`runtime/bw2/mount.c`, `runtime/bw2/shell.c`). This file records what
Windows answers for those calls ("Windows"), and what the game's save, load and
settings paths do in the port (the sections after it), as the engine at the end
of plan 11 runs them: headless, on a 32 GB Apple Silicon Mac, 2026-11-13, often
loaded by other work (load averages from 40 to over 200). Addresses are
`white.exe`'s unless a DLL is named. Nothing here quotes the game's strings,
dialogs, debug output or files: a button or a dialog is described in neutral
words with its place. Places are client pixels, at 1024x768 unless 800x600 is
said; they were measured from frames (`BW2_FRAME_DUMP`) written outside the
repo and deleted after.

## Windows

Measured 2026-11-13 with `tools/winprobe/saveprobe.c` (ours), built on the Mac
with Homebrew's mingw-w64 (`sh tools/winprobe/build.sh`, a PE32 console program
linked against the UCRT, ole32 and shell32) and run once on the winbench VM:
Windows 11, `ver` = `Microsoft Windows [Version 10.0.26300.9711]`, work folder
`C:\bench\bw2probe\w` on drive C: (NTFS, case-preserving, case-insensitive).
The VM was snapshotted first and rolled back to that snapshot after; the
snapshot was then deleted and `C:\bench\bw2probe` no longer exists there.
Nothing the probe wrote was kept.

Each line is `<case> ret=<hex> err=<decimal>[ <extra>]`: `ret` is the call's
return (a HANDLE prints as `1` when valid and `ffffffff` for
`INVALID_HANDLE_VALUE`), `err` is `GetLastError()` right after the call, which
the probe sets to 0 just before it. Names below are relative to the work
folder `W`. In the extras, `made`, `src`, `dst`, `inner` and `after` say
whether a path exists after the call (`GetFileAttributesA`).

The probe ran twice (the second run added the four `dosdate_*` rounding
cases after the first showed that the two-second step does not truncate);
every line the two runs share was the same in both. A third run (plan 11
Task 5, the same day, the same VM, under its own snapshot, rolled back and
deleted after in the same way) added the `datefmt_*` and `timefmt_*` cases for
the load dialog's dates; its other lines were the same as before.

| Case | Call | ret | err | Extra |
|---|---|---|---|---|
| `mkdir_new` | `CreateDirectoryA(W\d1)` | 1 | 0 | |
| `mkdir_exists` | the same again | 0 | 183 | |
| `shmkdir_nested_new` | `SHCreateDirectoryExA(NULL, W\s1\s2\s3)`, three new levels | 0 | 0 | made=1 |
| `shmkdir_exists` | the same again | 0xB7 (183) | 183 | |
| `shmkdir_relative` | `SHCreateDirectoryExA(NULL, "rel1\rel2")`, the current folder `W` | 0xA1 (161) | 161 | made=0 (nothing made) |
| `attr_dir` | `GetFileAttributesA(W\d1)` | 0x10 | 0 | |
| `attr_dir_slash` | `GetFileAttributesA(W\d1\)` | 0x10 | 0 | |
| `attr_file` | `GetFileAttributesA` of a file made with `FILE_ATTRIBUTE_NORMAL` | 0x20 | 0 | |
| `attr_missing_file` | a missing file in an existing folder | 0xFFFFFFFF | 2 | |
| `attr_missing_dir` | a file in a folder that does not exist | 0xFFFFFFFF | 3 | |
| `find_order` | `FindFirstFileA(W\f\*)` and `FindNextFileA` | 1 | 0 | names=`.` `..` `a` `A1` `aB` `a_b` `b.txt` `sub` `Zeta` `[q` `^r` `_y` `~x` |
| `find_order_end` | the `FindNextFileA` after the last name | 0 | 18 | |
| `find_dir_data` | `FindFirstFileA(W\f\sub)`, a folder, no wildcard | 1 | 0 | attr=0x10 size=0:0 name=sub |
| `find_missing_dir` | `FindFirstFileA(W\nodir\*)` | 0xFFFFFFFF | 3 | |
| `find_no_match` | `FindFirstFileA(W\empty\*.sav)`, an empty folder | 0xFFFFFFFF | 2 | |
| `move_file_new` | `MoveFileA(W\m\a, W\m\b)` | 1 | 0 | src=0 dst=1 |
| `move_file_onto` | `MoveFileA(W\m\b, W\m\c)`, `c` an existing file | 0 | 183 | src=1 dst=1 |
| `move_dir_new` | `MoveFileA(W\m\d1, W\m\d2)`, a folder holding a file | 1 | 0 | src=0 dst=1 inner=1 |
| `move_dir_onto` | `MoveFileA(W\m\d2, W\m\d3)`, `d3` an existing empty folder | 0 | 183 | src=1 dst=1 |
| `move_missing` | `MoveFileA` of a missing file in an existing folder | 0 | 2 | dst=0 |
| `delete_missing` | `DeleteFileA` of a missing file in an existing folder | 0 | 2 | |
| `delete_dir` | `DeleteFileA` of a folder | 0 | 5 | after=1 |
| `delete_readonly` | `DeleteFileA` of a file with `FILE_ATTRIBUTE_READONLY` | 0 | 5 | after=1 |
| `rmdir_nonempty` | `RemoveDirectoryA` of a folder holding a file | 0 | 145 | after=1 (the file is still there) |
| `rmdir_missing` | `RemoveDirectoryA` of a missing folder in an existing one | 0 | 2 | |
| `rmdir_empty` | `RemoveDirectoryA` of an empty folder | 1 | 0 | after=0 |
| `rmdir_slash` | `RemoveDirectoryA(W\r\e2\)`, empty, with a trailing `\` | 1 | 0 | after=0 |
| `create_always_exists` | `CreateFileA` `GENERIC_WRITE`, `CREATE_ALWAYS`, an existing 5-byte file | 1 | 183 | size=0 |
| `open_always_exists` | `CreateFileA` read and write, `OPEN_ALWAYS`, an existing 5-byte file | 1 | 183 | size=5 |
| `open_always_new` | `CreateFileA` read and write, `OPEN_ALWAYS`, a new name | 1 | 0 | made=1 |
| `create_new_exists` | `CreateFileA` `GENERIC_WRITE`, `CREATE_NEW`, an existing file | 0xFFFFFFFF | 80 | |
| `dosdate` | `CoFileTimeToDosDateTime` of 2026-11-13 12:34:56.500 UTC | 1 | 0 | ft=01DD736B:63DB6340 date=0x5D6D time=0x645D |
| `dosdate_even` | the same at 12:34:56.000 | 1 | 0 | ft=01DD736B:638F1800 date=0x5D6D time=0x645C |
| `dosdate_tick` | 12:34:56.000 plus one 100 ns tick | 1 | 0 | ft=01DD736B:638F1801 date=0x5D6D time=0x645D |
| `dosdate_odd` | 12:34:57.000 | 1 | 0 | ft=01DD736B:6427AE80 date=0x5D6D time=0x645D |
| `dosdate_carry` | 2026-12-31 23:59:59.000 | 1 | 0 | ft=01DD9983:0AAF2980 date=0x5E21 time=0x0000 |
| `datefmt_user_lcid` | `GetUserDefaultLCID()` (printed, not a call result) | | | lcid=0409 |
| `datefmt_am_0409` | `GetDateFormatW(0x409, DATE_SHORTDATE, 2026-11-13 09:05:07, NULL, buf, 64)` | 0xB (11) | 0 | text=`11/13/2026`, NUL-terminated |
| `timefmt_am_0409` | `GetTimeFormatW(0x409, TIME_NOSECONDS, the same, NULL, buf, 64)` | 8 | 0 | text=`9:05 AM` |
| `datefmt_am_0400`, `timefmt_am_0400` | the same two with `LOCALE_USER_DEFAULT` (0x400) | 0xB, 8 | 0 | the same texts |
| `datefmt_pm_0409` | `GetDateFormatW(0x409, DATE_SHORTDATE, 2026-01-02 21:34:56, ...)` | 9 | 0 | text=`1/2/2026` |
| `timefmt_pm_0409` | `GetTimeFormatW(0x409, TIME_NOSECONDS, the same, ...)` | 8 | 0 | text=`9:34 PM` |
| `datefmt_pm_0400`, `timefmt_pm_0400` | the same two with 0x400 | 9, 8 | 0 | the same texts |
| `datefmt_count` | `GetDateFormatW` of the 09:05:07 date with `buf` NULL, `cch` 0 | 0xB | 0 | |
| `timefmt_count` | `GetTimeFormatW` of it with `cch` 0 | 8 | 0 | |
| `datefmt_short` | `GetDateFormatW` of it with `cch` 10 | 0 | 122 | |
| `timefmt_short` | `GetTimeFormatW` of it with `cch` 7 | 0 | 122 | |

What these show:

- **Find order** is NTFS's: `.` and `..` first, then the names compared with
  both upper-cased (`_` 0x5F sorts after `Z` and `[` and `^`, `~` last), where
  a lower-casing compare (`strcasecmp`) puts `_`, `[` and `^` before the
  letters and `a_b` before `aB`.
- **A folder that does not exist** answers `ERROR_PATH_NOT_FOUND` (3) for
  `GetFileAttributesA` and `FindFirstFileA`, and a missing name in a folder
  that exists `ERROR_FILE_NOT_FOUND` (2). A folder's find data has size 0.
- **`MoveFileA` never replaces**: onto an existing file or an existing folder
  it answers `ERROR_ALREADY_EXISTS` (183) and changes nothing.
- **`DeleteFileA`** of a folder or of a read-only file answers
  `ERROR_ACCESS_DENIED` (5) and leaves it. **`RemoveDirectoryA`** of a folder
  that holds anything answers `ERROR_DIR_NOT_EMPTY` (145); a trailing `\` is
  accepted.
- **`SHCreateDirectoryExA`** returns its error code and also sets it as the
  last error (183 for a folder that exists); a relative path is
  `ERROR_BAD_PATHNAME` (161) and makes nothing.
- **`CreateFileA`** leaves `ERROR_ALREADY_EXISTS` (183) as the last error after
  a successful `CREATE_ALWAYS` or `OPEN_ALWAYS` of an existing file, 0 after
  making a new one; `CREATE_NEW` on an existing file is `ERROR_FILE_EXISTS`
  (80).
- **`CoFileTimeToDosDateTime`** rounds up to the next two-second step, not
  down: any time past an even second gives the next even second (56.000 is
  56, 56.000 plus 100 ns and 56.5 and 57 are 58), and the step carries into
  the next day and year (23:59:59 on 31 December is 00:00:00 on 1 January).
  The DOS words carry the FILETIME's own clock time (the FILETIMEs were made
  with `SystemTimeToFileTime`, which converts no time zone); the VM's time
  zone was not recorded, so this run does not show whether a FILETIME is
  ever converted to local time.
- **`GetDateFormatW` and `GetTimeFormatW`** with en-US (and the VM's user
  default, which is en-US) give the short date as `M/d/yyyy` and, with
  `TIME_NOSECONDS`, the time as `h:mm tt` (no leading zero on the hour, a
  plain space before AM or PM, all ASCII). The return counts WCHARs with the
  NUL; `cch` 0 asks for that count; a buffer one WCHAR short answers 0 with
  `ERROR_INSUFFICIENT_BUFFER` (122). These are the two calls the game's load
  dialog makes (`sub_00552370`, after `FileTimeToSystemTime`) to show each
  save's date.

### The differences fixed

Where the port answered otherwise, the probe's answer is now the port's, each
checked by one case in `runtime/bw2/shims_selftest.c`'s save files section
(which mounts its own game and support folders so that deletes and folder
makes run as they do under My Documents). Cases the probe did not measure keep
their earlier answers (plan 11 Ruling 8).

| Call | Before | Now | Where |
|---|---|---|---|
| `GetFileAttributesA` of a name in a missing folder | 2 | 3 | win32hle `kernel32_ext.c` (147596d) |
| `FindFirstFileA` order | lower-cased (`strcasecmp`) | upper-cased, dots first, a tie by the bytes | `kernel32_ext.c` (bf97e67) |
| `FindFirstFileA` of a folder, and a wildcard in a missing folder | the folder's own size; 2 | size 0; 3 | `kernel32_ext.c` (452dce3) |
| `DeleteFileA` of a folder or a read-only file | 2 for a folder; a read-only file removed | 5, left in place | `kernel32_ext.c` (e8e41db) |
| `RemoveDirectoryA` of a folder holding anything | `ERROR_INVALID_FUNCTION` | 145 | `kernel32_ext.c` (ca2c103) |
| `CoFileTimeToDosDateTime` | rounded down | rounded up to two seconds, carrying | win32hle `ole32.c` (2226ec3) |
| `SHCreateDirectoryExA` | its code returned only; a relative path made | its code also the last error; a relative path 161, nothing made | ours, `runtime/bw2/shell.c` (7b55f49) |
| `GetDateFormatW`, `GetTimeFormatW` | not answered (the load dialog stopped, `unimplemented import` at `0x00552370`) | the measured pictures; any other flag stops by name | win32hle `kernel32_crt.c` (f2aa665) |

`MoveFileA` already matched every case (`runtime/bw2/mount.c`'s wrapper
answers 183 onto a file or a folder and 2 for a missing source), and so did
`CreateFileA`'s last error. Each vendored change has one line in
`third_party/pcrecomp.PATCHES`. Read-only on the Mac means no owner write bit;
`SetFileAttributesA` stays a no-op, so only a file the user made read-only is
refused.

## Scripts that follow the game

The boot tests drive the game with `BW2_INPUT_SCRIPT` (`runtime/bw2/input/script.c`,
whose header gives the grammar). Plan 6's absolute read numbers do not mark
the same moment in two runs now that the land runs on: headless rates were
measured from 41 to 343 Presents a second (`campaign.md`), and a frame dump
slows a run several times over. The intro's last frame came at read 26,663,
27,461 and 21,780 in three runs and at about 16,500 in one that dumped every
20th frame, and a line 8,000 reads after it fell inside the creature
presentation in one run and after it in another. The game's scenes run on its
own clock, so plan 11's scripts are blocks anchored on what the engine sees of
that clock:

- `after open <frames>` and `after last <frames>`: the movie events
  `_BinkOpen@8` and the last frame's `_BinkCopyToBuffer@28` already report
  (`runtime/bw2/video/binkw32.c`), whether or not `BW2_BINK_LOG` is set.
- `after seconds <s>`: wall time since the block was armed. Every scene the
  tests wait through ends in a state that waits for input, so a generous wait
  is safe. There is no test-only clock: the game's audio and streaming run on
  real time.
- A block is armed only when the one before it has fired and pushed all its
  events, so an empty `after open 1798` holds back the blocks after it until
  the intro opens (the land load before it takes about 23 s).
- Each firing prints one line naming the anchor and its read, and each boot
  test asserts the whole list in order, so an anchor that never comes fails
  the test by name (`run_boot` also turns a timeout into a failure listing the
  lines that fired and the stderr tail).
- `<read> quit` exits 0 at that read, for runs where the game's own quit is
  not what is tested (the second settings boot and the load).
- A held key is a key down, an `after seconds <hold>` block and its up, so it
  stays down for its wall time whatever the frame rate (Escape 0.5 s, letters
  0.2 s). This is a precaution: 40 ms presses also worked in the land, but a
  press of a fixed read count is about 12 ms at 343 Presents a second.

A check run with `NEW_GAME_SCRIPT`, `after open 1798`, `after seconds 2` and
`0 quit` saw the intro open at read 316, the seconds anchor at read 1,339 and
the quit, exit 0.

**The intro skip.** Escape (vk `0x1B`, scan `0x01`) skips the intro, but not
at once: with presses every 100 reads from the open, the movie stopped at the
first press after movie frame 42, 31, 68, 69 and 101 in five runs (1.3 to
4.2 s after the open), and earlier presses were ignored. A skipped intro is
never closed and never shows its last frame, so `after last 1798` never fires
on that path; the save script anchors on `after open 1798` instead.

**The scene skips.** After the intro the land plays a scene over the island
with both advisors and then a scene that shows the four creatures one by one,
then the creature choice, where the game's hand cursor appears. Unskipped, the
two scenes ran more than 12,000 Presents (about 130 s at 90 a second). Each
further Escape cuts a part of them (presses during a transition are ignored).
The save script holds Escape 18 times (`ESCAPES`): the first 3 s after the
intro's open, then one every 2 s (a 1.5 s gap and the 0.5 s hold). The choice
was live after 13 holds in 8 runs and after 12 in one run on a Mac running 10
extra CPU burners (load 75); both cases are covered, as the next paragraph
says.

**The pause bar's toggling and the continue click.** With the choice live,
each Escape opens or closes the pause bar: the land frozen behind a title and
a bar of six buttons at y = 710: continue (85), load (255, greyed until the
land has been saved), save (425), options (594), restart land (765) and quit
(936). One 0.5 s hold toggles it exactly once (frames: one hold opened it and
it stayed open 3 s, the next closed it for 3 s, the next opened it again), so
a hold counts as one press. Which state the surplus holds
leave depends on how many the scenes took, and the land's text is not drawn
through `ID3DXFont` (over a run to the pause bar `DrawText` drew 15 distinct
strings, all in the profile dialog), so nothing the engine sees tells the two
apart. The script therefore clicks the bar's continue place 5 s after the last
hold, which closes the bar if it is open, and then holds Escape once more,
which opens it. On a closed bar that click lands on the live choice: a control
run (19 holds, the bar closed, imports traced) showed over the click's 5 s no
file activity and no import name or caller absent from the 15 s before it
(the only new names came with the cursor moves: `IsBadReadPtr` from
`sub_00AD5235` and `GetTickCount` from `sub_006230C0` after the corner move,
vertex buffer unlocks from `sub_00A97530` and `sub_00AB3E80` after the move to
the place, callers that also run during the scenes), and frames before and
after it show the same four creatures, none picked, and the hand in place.

**The game's cursor in the land.** Before the land the front end's cursor is
the script's running point. Once the land has loaded, the game's own cursor
moves 3/4 of each DirectInput delta, in the land and in the front end after
it (paused-land frame diffs: moves of 100, 200, 4 x 50 and 300 gave 75, 150,
150 and 226 pixels), and stops at the screen's edges. The profile here holds
a mouse sensitivity of 50; another sensitivity would need another scale. So
every click after New Game first moves the cursor far up and left into the
top left corner, waits 0.5 s, and then moves by the place divided by 0.75,
rounded up (`step_blocks` in `tests/test_boot.py`). Plain running-point moves
missed the new-save row, the naming dialog's OK and the front end's quit.

## Settings

Test: `test_boot_settings_persist` (two boots on one support folder).

**The panel at 1024x768.** After the profile the front end already takes
input 0.25 s after the profile's last event (read 84): an options click that
early opened the panel in every probe run, at loads from 43 to 177 (the panel
showed 0.2 to 0.37 s after the click's read; in one run whose whole process
stalled 12 s it showed when frames resumed), so the script waits 3 s and then
spaces its clicks 2 s apart; frame dumps of two runs show each step done in
the frame before the next click. The front end's options button (766, 711)
opens a panel with tabs at y = 181 (audio 335, video 452, input 570), a
credits tab (687, 174), and restore defaults (335, 573) and exit (688, 573)
along its foot. The video tab lists the resolutions from (448, 242) down in
18-pixel rows (640x480, 800x600 at (448, 260), 1024x768, 1152x864, and more),
with detail boxes, a subtitles box and four sliders. Exit after a change asks
whether to save the changes (yes (424, 409), no (599, 409)).

**The reset.** Yes resets the device to 800x600 full screen (the game's debug
line for the reset names the size), and every later frame is 800x600
(480,000 pixels, all non-black). From the first frame after the reset the game
asks whether to keep the new size, and goes back to the old one by itself
after a delay it states; the script answers yes at (312, 325), 2 s after the
save-changes yes. The plan did not foresee that dialog: the first bring-up run
reached its quit only because its confirm click fell on this dialog's yes.

**At 800x600** the front end's quit is at (731, 555) and its confirm yes at
(312, 325): the confirm dialogs keep their 1024x768 pixel size, centred, so
(424, 409) becomes (424 - 512 + 400, 409 - 384 + 300). The game's cursor is
drawn at the running point at every click after the reset (the running point
is clamped to the new client rect).

**What is written when.** Both `Settings.ini` and `Profiles/~ab/Profile.ini`
under the support folder's `Documents and Settings/Player/My Documents/Black &
White 2` record the new size at once on the save-changes yes, and the game's
own quit rewrites both. After boot 1 the game's My Documents folder holds only
those two files (156 and 9,043 bytes), and nothing is written under
`<support>/Game/`.

**Times** (boot 1, two runs with the full suite as load): the first read 3.0
and 4.2 s after start, the 800x600 reset 18.0 and 19.2 s, the quit click 23.2
and 24.5 s, `ExitProcess(0)` 27.1 and 30.2 s. The whole test (both boots) took
38 to 47 s.

**The second boot** reuses the folder with no input but `after seconds 10`
and `0 quit`. It shows no profile dialog (the one HLSL effect is compiled
before the first read, as with any reused folder), draws all its frames at
800x600 from the first (1,739 in the run measured), and quits at read 418,
exit 0.

## The profile

The game writes its profile on its own quit. The front end's quit (936, 711)
asks for confirmation (yes (424, 409), no (598, 409)); yes runs the game's
close path (its debug output marks the close and the D3D release),
`[win32hle] ExitProcess(0)` and exit 0, and rewrites `Settings.ini` and
`Profile.ini`. A run ended by a switch (`BW2_QUIT_AFTER_MOVIE`,
`BW2_QUIT_AFTER_READS` or the script's `quit`) never reaches that path: a
support folder reused after a New Game run that `BW2_QUIT_AFTER_MOVIE` ended
shows the front end with its continue and load buttons greyed, since that
profile was not rewritten after the autosave. Opening the land's pause bar
also rewrites both files. The save and settings tests therefore end through the game's own
quit and assert that no quit switch fired.

## The save

Test: `test_boot_save_game` (the `saved_game` fixture, one boot shared with
the load test).

**The path.** New Game, the intro's open, the skips above, the continue click
and one more Escape (the bar open), then, each 2 s after the one before: the
bar's save button (425, 710); the save dialog's new-save row (390, 263) and
its OK (409, 547), the dialog also holding a greyed delete (511, 547) and
cancel (610, 547). The OK opens a second, naming dialog: a name field (510,
382) holding a default name, OK (422, 427) and cancel (598, 427). A click in
the field empties it; keys typed before this dialog is up are lost. The script
types A and B (each held 0.2 s, B 0.5 s after A's release) and clicks its OK, which writes
the save; the save's `SaveGame.inf` holds the typed name.

**The quit writes a save too.** 20 s later (`SAVE_WAIT`) the script clicks the
bar's quit (936, 710). It asks a yes/no question (yes (424, 409), no about
(598, 409)); yes makes the game write a save of its own into another new
numbered folder and go to the front end, whose buttons are then continue, new
game, load, change profile, options and quit, and whose quit (936, 711) and
yes (424, 409) end with `ExitProcess(0)`. The test lists `Saved Games` at the
save button's first anchor and at the quit's first anchor: exactly one new
folder appears between them (the save) and exactly one more after (the
quit's).

**Folder naming.** Saves go under `Profiles/~ab/Saved Games/` in numbered
folders. In every run the named save went to `2` and the quit's later save to
`1`. `Saved Games/Pictures` stays empty: the game only probes
`PICTURES\Game.sav` and `AUTOSAVE\Game.sav` when it lists saves (on each
pause bar open, at the load button and once at the first read). After the
save the pause bar's load button is enabled.

**The file set.** Both saves hold the same 15 files (sizes from five runs):

| File | Bytes |
|---|---|
| `Creature0.sav` to `Creature4.sav` | 411,674 each |
| `Creature0.sav.meta` to `Creature4.sav.meta` | 32,136 each |
| `Game.sav` | 4.24 to 4.26 MB |
| `Game.sav.meta` | 1.46 to 1.48 MB |
| `SaveGame.inf` | 284 |
| `Script.sav` | 906,309 |
| `ter.sav` | 15,555,915 |

Fourteen are created in the save folder's `temp` folder and moved into place;
`SaveGame.inf` is created in the folder itself, and the `temp` folder is
removed. For comparison, the New Game autosave
(`Saved Games/Autosave/<a six-character name that differs per run>/1/`) holds
five: `Game.sav` 3,698,360, `Game.sav.meta` 1,114,980, `Script.sav` 903,032,
`SaveGame.inf` 284 and `ter.sav` 15,555,915, and New Game also makes
`Profiles/~ab/helpstats.dat`, empty `Saved Games/Pictures` and `Current`
folders, and at the `Black & White 2` root an empty `Cache` folder and a
`Persistant Data` folder holding the empty folders `Creature Mind/<the
autosave's name>` and `Player Info`.

**The file calls by function** (one run with `BW2_TRACE_IMPORTS=1`, from the
naming dialog's OK to the bar's quit; callers in `white.exe` unless
`ScriptLibraryR.dll`, whose functions are at `0x1000xxxx`):

| Call | Callers (count) |
|---|---|
| `CreateFileA` | `sub_00921C00` 7, `sub_00931130` 6, `sub_009314A0` 6, `sub_00AEF0C8` 1, `sub_100493B7` 1 |
| `WriteFile` | `sub_00924440` 6,580, `sub_00930BF0` 108, `sub_00930B50` 4, `sub_00930D00` 6, `sub_009316D0` 6, `sub_10044EE7` 220 |
| `CloseHandle` | `sub_00922060` 5, `sub_00930D00` 6, `sub_009316D0` 6, `sub_009241E0` 2, `sub_1004355D` 1 |
| `SetFilePointer` | `sub_00924480` 99, `sub_009316D0` 6 |
| `ReadFile` | `sub_00924400` 3 |
| `GetFileSize` | `sub_00921C00` 5 |
| `SHCreateDirectoryExA` | `sub_0060F8E0` 2 |
| `DeleteFileA` | `sub_00AD62F0` 12, `sub_006137B0` 14, `sub_0060F940` 1 |
| `MoveFileA` | `sub_006137B0` 14 |
| `RemoveDirectoryA` | `sub_0060F910` 1 (the `temp` folder) |
| `FindFirstFileA` | `sub_0091F270` 20, `sub_0060F940` 3, `sub_00AD660C` 1 |
| `FindNextFileA` | `sub_0091F270` 23, `sub_0060F940` 7, `sub_00AD66E9` 16 |
| `FindClose` | `sub_0091F270` 19, `sub_0060F940` 3, `sub_00AD6588` 1 |
| `GetFileAttributesA` | `sub_00923DB0` 1 |
| `CoFileTimeToDosDateTime` | `sub_0091DFB0` 46 |

The New Game autosave, for comparison, makes its folders with
`SHCreateDirectoryExA` (`sub_0060FFE0` 7, `sub_0060F8E0` 2) and
`CreateDirectoryA` (`sub_00AD997A` 18), probes with `GetFileAttributesA`
(`sub_00923DB0` 452, `sub_008872F0` 60) and finds (`sub_0091F270`), then
`DeleteFileA` and `MoveFileA` (`sub_006137B0`, 4 each), more `DeleteFileA`
(`sub_00AD62F0` 3, `sub_0060F940` 1, `ScriptLibraryR.dll`'s `sub_1003D734`
1) and one `RemoveDirectoryA` (`sub_0060F910`); `CoFileTimeToDosDateTime`
runs 108 times.

**The time to write.** From the naming OK's click anchor to the save's last
file: 0.48, 0.52 and 0.52 s in three runs, 0.85 s to the last `CloseHandle`
with imports traced, 1.25 s on the Mac with 10 extra CPU burners; the files
themselves are written in about 0.7 s of the traced run. `SAVE_WAIT` (20 s) is
more than 3 times the slowest. The save test takes about 106 s.

## The load

Test: `test_boot_load_game`, on a copy of the save boot's support folder.

**The dialog.** The boot shows no profile dialog, and the front end's cursor
is again the running point, from (0, 0). The front end's load button (425,
711) is enabled after the save boot's clean quit; 3 s after the first read
(`LOAD_FIRST`; the first read came 3.1 to 4.9 s after start) the script clicks
it, and the dialog shows 0.2 s later. Its rows start at (390, 263) in 17-pixel
steps; the quit's save (`Saved Games/1`) is listed first and the named save
(`Saved Games/2`) second, so the test clicks (390, 280). Its OK (409, 547),
delete (511, 547) and cancel (610, 547) sit as in the save dialog. Its info box
shows the profile, the save's name, the land and the elapsed time, and the
save's date and time, which the listing (`sub_00552370`, after
`FileTimeToSystemTime`) formats with `GetDateFormatW(0x400, DATE_SHORTDATE,
...)` and `GetTimeFormatW(0x400, TIME_NOSECONDS, ...)`; those were the load
path's one stop (above). Whether the rows sort by time (folder 1 was the
newer) or by folder name is not measured.

**The files read.** Opening the dialog reads `Game.sav` and `SaveGame.inf` of
both saves and probes `PICTURES\Game.sav` and `AUTOSAVE\Game.sav`. After the
OK the game opens, in this order, `Game.sav.meta`, `Game.sav`, `ter.sav`,
`Script.sav`, then `Creature4` down to `Creature0`, each `.sav.meta` and then
`.sav`: 14 of the 15 files, and no file of the other save. During the load
there is one `FindFirstFileA` of `Saved Games\Autosave\<six-character name>`
that answers not found. Nothing is written: every file of the save keeps its
size and modification time.

**The time to the land.** From the OK's click anchor to the first land frame
5.9 and 6.0 s in two runs, with no Present for 3.8 s in between (the game's own
init lines report about 5 s of init). The land is the creature choice (four
creatures, the hand), the state the save was taken in. No movie opens. The
script quits 60 s after the OK (`LOAD_WAIT`), leaving more than 3,000 land
frames. The load test takes about 180 s with its share of the fixture.

**The continue button.** The front end's continue (85, 711), clicked 3 s
after the first read on the same folder, loads `Saved Games/1`, the quit's
save (the newest), not the named one: the same 14 files in the same order, the
land 6.0 s after the click, and no dialog.

## The Windows VM

The winbench VM (Windows 11 build 10.0.26300.9711, 1280x800, Microsoft Basic
Display Adapter, driver 10.0.26100.1, no GPU) is a reference for Win32
behaviour through probe programs only. The patched game tree (without
`SECDRV.SYS`, `DrvMgt.dll` and `00000000.256`) with disc 1's `d3dx9_25.dll`
beside `white.exe` starts there, opens its window and stops at once with its
fatal-error box saying it could not create its 3D device. It asked for no disc
and no driver, and made only `Documents/Black & White 2/Cache` and `Profiles`.
No SafeDisc driver was installed or started.

Every use followed one rule (plan 11 Ruling 7): a `vm_snapshot` before any
change, a rollback to that snapshot after, and the snapshot deleted, so
`vm_snapshots` lists only `pre-winbench` and `clean` again. This departs on
purpose from rolling back to `clean`, which dates from 2026-11-08 and would
destroy the other work in `C:\bench`; the plan's own snapshot returns the VM
to exactly the state it was found in. The VM was used three times: the game
run above (before the plan), the probe (Task 2, two runs in one snapshot) and
the date probe (Task 5). Nothing from it is committed, only these facts; the
probe's source (`tools/winprobe/saveprobe.c`, MIT) is, and its `.exe` is built
into `out/` and never tracked (`tests/test_hygiene.py`).

## What was absent

In every run of plan 11 (bring-up, tests and the traced runs): no disc,
volume or driver question, no tripwire line, no loud line and no `owned by a
later plan` line. The protection VM's thunks called only `GetCurrentThreadId`
(`sub_024290DC`, `sub_02450769` and others). The guard's allow list is still
empty, and no run wrote under `<support>/Game/`.

## For the next plan

Where each boot test ends now:

| Test | Ends | How |
|---|---|---|
| `test_boot_to_front_end` | the front end, read 300 | `BW2_QUIT_AFTER_READS` |
| `test_boot_profile_to_attract` | the attract video | `BW2_QUIT_AFTER_MOVIE` |
| `test_boot_new_campaign` | the land's opening scene, 600 Presents after the intro's last frame | `BW2_QUIT_AFTER_MOVIE` |
| `test_boot_settings_persist` | boot 1: the front end at 800x600; boot 2: the front end 10 s after the first read | the game's quit; the script's `quit` |
| `test_boot_save_game` | the front end after the pause bar's quit (two saves written) | the game's quit |
| `test_boot_load_game` | the creature choice, 60 s after the load's OK | the script's `quit` |
| `test_boot_close_quits` | the profile dialog, read 21 | a close event, the game's quit path |

The creature choice is still where a New Game waits: the four creatures on
the land, the game's hand following the mouse (at 3/4 of each delta), and no
pick made. Picking one needs its own measured UI, which no run has measured
yet: where each creature stands at 1024x768, what the hand must do to pick
one (a click, a hold or a drag), and what confirms the pick. A save taken
before the choice already runs the whole save path, and a load returns to the
choice in about 6 s, so a later test can start from a loaded save instead of
New Game and the skips.
Bink audio tracks stay a later plan's stop until a scene opens a movie that
has one.

Plan 12 measured the choice (two clicks on a creature, select then confirm,
measured on the port), a save at the tutorial offer after it, the first land's
movement tutorial, the land's end and the second land's load; where its tests
end now is in `land1.md` (its "For the next plan").
