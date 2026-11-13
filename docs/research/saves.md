# Saves, loading and settings

Plan 11. The save path is the game's own code running on win32hle's file calls
and ours (`runtime/bw2/mount.c`, `runtime/bw2/shell.c`). This file records what
Windows answers for those calls ("Windows"), and (Task 6) what the game's save,
load and settings paths do in the port.

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
