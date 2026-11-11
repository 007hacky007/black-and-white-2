# Plan 2: Official Patch Application Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the extracted 1.0 install tree into the game data tree for 1.2 plus fan patch 1.42 (`gamefiles/game/`), on macOS. This includes applying patches 1.1 and 1.2's RTPatch diffs, and the result is checked against a committed fingerprint manifest.

**Architecture:** RTPatch is proprietary and no open applier exists. RTPatchX and myrtp both call Pocket Soft's `patchw32.dll`. Each patch ships that DLL (205,824 bytes, sha `b843d4f9...`), so we run it as shipped in the Unicorn CPU emulator. A small Python stand-in for the Win32 calls it uses runs alongside it (user decision, 2026-11-11).

A spike in plan 2's research step got this working end to end:
- **1.1:** 69 files, all "Verifying - Passed", 84 s.
- **1.2:** 9 files, 54 s.
- **Imports:** 58 used, out of 173.
- **What the spike lacked:** SEH dispatch. The DLL leaves `RTPatchApply32` through `RaiseException`.

The whole-file groups are plain copies. Their destinations are read from the `<TARGETDIR>\...` strings in each setup's `data1.hdr`.

**Tech Stack:** Python 3.14, unicorn 2.1.4, pefile, pytest. Test x86 snippets are assembled with Xcode clang (`-target i386-unknown-linux-elf`).

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 3.2, 8, 9). Phase 0 facts: `docs/research/official-patches.md`.

## Global Constraints

- Never commit game material: discs, patches, `patchw32.dll`, `.RTP` files, exes, extracted files, or strings copied from them. Commit only facts (paths, sizes, SHA-256, addresses). `third_party/pcrecomp/tools/audit_repo.py .` must report CLEAN before any commit that adds data.
- `gamefiles/`, `gen/` and `out/` stay gitignored. The built tree goes to `gamefiles/game/`.
- USA English only: use the `USA_V1_0_TO_V1_1_INSTALL.RTP` and `ALL_V1_1_TO_V1_2_INSTALL.RTP` diffs and the `*_English` groups, and nothing for other languages.
- Executable: fan patch 1.42 `white.exe` (from `bw2/fanpatch.py`), after official 1.1 and 1.2.
- Fail loudly. An unimplemented Win32 call, an RTPatch error, a skipped "old file invalid" or "missing", or a manifest mismatch must each stop the build and name what failed.
- Paths the emulated DLL touches are confined to the patch file's folder (read-only) and the target tree.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short, imperative, with no body or trailers. Identity is 007hacky007. Never push.

## Review Focus

- **A patch path that escapes the tree** (`..\`, an absolute path, another drive letter): refused with Win32 error 3, nothing written on the host. (Task 4, `test_path_outside_roots_refused`.)
- **An interrupted build** (Ctrl-C or a crash mid-patch): the half-patched tree is never taken for a finished one. Build into `game.part`, rename only after the manifest verifies, and start a rerun fresh. (Task 7, `test_interrupted_build_not_reused`.)
- **A 1.0 file that differs from what the patch expects** (a different disc set, corruption): RTPatch counts it as "Old Files Invalid" or "Missing" and carries on. We must fail and name the file. (Task 6, `test_summary_with_ignored_files_fails`.)
- **A code path the spike never reached that calls an import we have not written** (an error path, say): stop with the API's name. Never return garbage. (Task 2, `test_unimplemented_import_raises_with_name`.)
- **A volume without APFS clones and too little space for a plain 3.2 GB copy:** fail before copying, with the bytes needed. (Task 7, `test_copy_fallback_checks_space`.)

---

### Task 1: Fingerprints and patch inputs

**Files:**
- Modify: `bw2/known_releases.tsv`, `bw2/patches.py`
- Test: `tests/test_patches.py`

**Interfaces:**
- Consumes: `extract_patch_groups(patch_exe, kind, out) -> Path` (existing), `identify()`.
- Produces: `PatchInputs(dll: Path, rtp: Path, groups: Path)` and `patch_inputs(patch_exe: Path, kind: str, out: Path) -> PatchInputs`. The function fingerprint-checks the DLL and the RTP, and reuses an existing extraction whose files still match.

- [ ] **Step 1: Write the failing test.** `test_patch_kinds_known` asserts that `load_releases()` has one row of each kind `patchw32` (205,824 bytes, sha `b843d4f933f2b4221c8c46c2269c69feba1da2ac51738a233e646ff4168253ee`), `rtp-1.1` (`USA_V1_0_TO_V1_1_INSTALL.RTP`, 20,305,842, `3d39b2e4bbfbf5a6ff0ae36374a0be0e2068c053f71f27a8d81a4bd7979351fb`) and `rtp-1.2` (`ALL_V1_1_TO_V1_2_INSTALL.RTP`, 21,909,803, `77068393173d5379e181895e446666bc996c32aca38059c2aa30aebfc8299e83`). Also add `test_patch_inputs_real` (skipped unless `BW2_GAMEFILES=1`): for both patches, `patch_inputs()` returns existing files whose `identify()` kinds are `patchw32` and `rtp-1.x`.
- [ ] **Step 2: Run** `.venv/bin/python -m pytest -q tests/test_patches.py`. Expected: FAIL (no such rows or function).
- [ ] **Step 3: Implement.** Add the three TSV rows. `patch_inputs` takes the DLL and the RTP from the `_Support_Language_Independent_OS_Independent_Files` group. A file that fails its fingerprint raises `FingerprintMismatch`.
- [ ] **Step 4: Run** the same command with and without `BW2_GAMEFILES=1`. Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Fingerprint the RTPatch DLL and diffs"`.

### Task 2: Unicorn PE32 host

**Files:**
- Create: `bw2/pe32emu.py`
- Modify: `tests/fixtures.py` (add `asm32`)
- Test: `tests/test_pe32emu.py`

**Interfaces:**
- Produces:
  - `asm32(src: str) -> bytes` in `tests/fixtures.py`. It assembles Intel-syntax x86 with Xcode clang (`-target i386-unknown-linux-elf -c`) and returns `.text`. Tests that need it skip when `bw2.build.check_tools()` raises `ToolsMissing`.
  - `Win32Unimplemented(Exception)`, whose message carries the import's name.
  - `class Pe32Host`:
    - `__init__(self, dll: Path | bytes)` maps `SizeOfImage` at the preferred base. It applies relocations if that base is taken (not expected), sets up a flat GDT with `fs` pointing at a TEB (`fs:[0]` = `0xFFFFFFFF`, `fs:[0x18]` = TEB), a 4 MB stack and a heap region, and points every IAT slot at a stub.
    - `api: dict[str, tuple[int | None, Callable]]`: the argument count (`None` means cdecl, the caller pops) and a handler taking the stack arguments as ints and returning `eax`.
    - `call(self, va: int, *args: int) -> int` calls a guest function with stdcall/cdecl arguments.
    - `export(self, name: str) -> int`.
    - `callback(self, fn, nargs: int) -> int` returns a guest address that runs `fn`.
    - Memory helpers `read`, `write`, `u32`, `w32`, `cstr(va, wide=False)`, `put_str(va, s, wide=False)`.
    - `virtual_alloc(size) -> int`, which returns 64 KB-aligned zeroed blocks. `VirtualAlloc(addr, ...)` with a non-zero address returns `addr & ~0xFFF`. The spike's heap corruption came from getting this wrong.
    - `last_error: int`.

- [ ] **Step 1: Write the failing tests.**
  - `test_stdcall_import_called_and_popped`: a PE with one import `KERNEL32.dll!GetVersion` and code `call [iat]; ret`. Register `api["GetVersion"] = (0, lambda: 0x0A280105)`; `call(entry)` returns `0x0A280105` and `esp` is balanced.
  - `test_args_reach_handler_in_order`: `push 3; push 2; push 1; call [iat]; ret` against `Add3` with 3 args returns 6.
  - `test_cdecl_import_not_popped`: a `wsprintfA`-style cdecl handler, followed by `add esp, 8`, keeps `esp` right.
  - `test_unimplemented_import_raises_with_name`: calling an unregistered import raises `Win32Unimplemented` with `"GetTickCount"` in the message.
  - `test_fs_points_at_teb`: `mov eax, fs:[0x18]; ret` returns the TEB address, and `mov eax, fs:[0]; ret` returns `0xFFFFFFFF`.
  - `test_virtual_alloc_is_64k_aligned`.
  - `test_callback_runs_python`: guest code calls a `callback()` address with 2 stdcall args and gets the Python result.
- [ ] **Step 2: Run** `.venv/bin/python -m pytest -q tests/test_pe32emu.py`. Expected: FAIL (no module).
- [ ] **Step 3: Implement `bw2/pe32emu.py`.**
  - One `UC_HOOK_CODE` hook over the stub region only. Hooking all code costs 10x or more.
  - `call()` pushes a return address at a stop stub and runs `emu_start(va, stop)`.
  - Errors raised inside the hook are stored and re-raised after `emu_start` returns. Unicorn swallows exceptions raised inside hooks.
- [ ] **Step 4: Run** the tests. Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Add a Unicorn host for 32-bit Windows DLLs"`.

### Task 3: Structured exception dispatch

**Files:**
- Modify: `bw2/pe32emu.py`
- Test: `tests/test_pe32emu.py`

**Interfaces:**
- Produces: `RaiseException` (4 args) and `RtlUnwind` (4 args) in `Pe32Host.api` by default, implemented as a state machine over stub return addresses (Unicorn cannot nest `emu_start` inside a hook).
  - **RaiseException:**
    1. Build an `EXCEPTION_RECORD` and an x86 `CONTEXT` (offsets: Edi 0x9C, Esi 0xA0, Ebx 0xA4, Edx 0xA8, Ecx 0xAC, Eax 0xB0, Ebp 0xB4, Eip 0xB8, EFlags 0xC0, Esp 0xC4) on the guest stack.
    2. Call each `fs:[0]` handler as `handler(rec, frame, ctx, dispatcher)` (cdecl).
    3. On return 0 (continue execution), load the CONTEXT into the registers.
    4. On return 1 (continue search), try the next frame.
    5. At the end of the chain, raise `GuestException(code)`.
  - **RtlUnwind(target, ip, rec, value):**
    1. Call each handler above `target` with `ExceptionFlags |= 2` (unwinding).
    2. Set `fs:[0] = target`.
    3. Return to the caller with `ebx`, `esi`, `edi` and `ebp` preserved and 4 arguments popped. If `ip` is non-zero, continue at `ip` instead.

- [ ] **Step 1: Write the failing tests** (snippets via `asm32`).
  - `test_seh_continue_execution`: the function installs a frame whose handler sets `ctx.Eip` to a resume label and `ctx.Eax = 7`, then returns 0. The function raises and returns `eax`. Expected: 7, with `fs:[0]` restored by the function's own epilogue.
  - `test_seh_unwind_to_except_block`: the handler calls `RtlUnwind(frame, 0, rec, 0)`, then restores `esp`/`ebp` from the frame and jumps to an except label that returns 42. An inner frame's handler records that it saw the unwinding flag. Expected: 42, the inner handler ran once with the flag, and `fs:[0]` equals the outer frame's `next`.
  - `test_unhandled_exception_raises_with_code`: no frame; raising `0xE06D7363` raises `GuestException` naming `0xE06D7363`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Dispatch guest SEH in the DLL host"`.

### Task 4: Sandboxed Win32 files

**Files:**
- Create: `bw2/w32files.py`
- Test: `tests/test_w32files.py`

**Interfaces:**
- Consumes: `Pe32Host`.
- Produces: `install_files(host: Pe32Host, roots: dict[str, Path], writable: set[str]) -> FileSystem`.
  - `roots` maps a drive letter to a host folder: `{"P": patch_dir, "T": tree}`. Only letters in `writable` may be written.
  - Paths resolve case-insensitively, as on Windows. Anything outside a root, including after `..`, fails with error 3 and is logged in `FileSystem.refused`.
  - It registers the A and W forms the spike needed:
    - Open/close and I/O: `CreateFileA/W`, `CloseHandle`, `ReadFile`, `WriteFile`, `SetFilePointer` (64-bit through `lpDistanceToMoveHigh`), `SetEndOfFile`, `FlushFileBuffers`, `GetFileSize`, `GetFileType` (1 for files).
    - Directory listing: `FindFirstFileA/W`, `FindNextFileA/W`, `FindClose`, with real sizes and mtimes as FILETIME.
    - Attributes and times: `GetFileAttributesA/W`, `SetFileAttributesW`, `SetFileTime`.
    - File operations: `DeleteFileA/W`, `MoveFileW`, `MoveFileExW`, `CreateDirectoryA/W`, `RemoveDirectoryA/W`.
    - Paths: `GetFullPathNameA/W`, `GetCurrentDirectoryA/W`, `SetCurrentDirectoryA/W`.
    - Volumes: `GetDiskFreeSpaceA` (the real free space, capped at 2^31-1 clusters), `GetVolumeInformationA`, `GetLogicalDrives` (the root letters), `GetDriveTypeA` (3).
    - File mapping: `CreateFileMappingA` and `MapViewOfFile` (the view is sized by the mapping, not by the current file length). A writable view is written back on `UnmapViewOfFile` and when its mapping handle or file handle is closed. patchw32 closes the mapping without unmapping; the spike's "Verifying - Failed!" came from that.
- [ ] **Step 1: Write the failing tests.** Drive the registered handlers directly, with strings placed in guest memory.
  - `test_create_read_write_roundtrip`.
  - `test_case_insensitive_resolve` (`T:\DATA\x.TXT` opens `Data/x.txt`).
  - `test_path_outside_roots_refused` (`T:\..\escape`, `C:\etc\passwd` and `P:\new` opened for write: each returns `INVALID_HANDLE_VALUE`, error 3 or 5, and nothing is created on the host).
  - `test_set_file_pointer_64bit`.
  - `test_writable_mapping_flushed_on_close_without_unmap` (create an empty file, map it at 0x9B4C bytes, write through the view, close the mapping, then the file. The file holds the bytes).
  - `test_find_first_next_lists_with_sizes`.
  - `test_move_replace_flag` (`MoveFileExW` without `MOVEFILE_REPLACE_EXISTING` fails on an existing target).
- [ ] **Step 2: Run** `.venv/bin/python -m pytest -q tests/test_w32files.py`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Add sandboxed Win32 file calls for the DLL host"`.

### Task 5: Remaining Win32 stand-ins

**Files:**
- Create: `bw2/w32base.py`
- Test: `tests/test_w32base.py`

**Interfaces:**
- Produces: `install_base(host: Pe32Host, command_line: str = "patch.exe") -> None`, which registers the rest of the spike's set.
  - Process and C runtime start-up: `GetVersion` (5.1.2600, so `0x0A280105`), `GetCommandLineA`, `GetEnvironmentStrings`, `GetStartupInfoA`, `GetStdHandle`, `GetModuleFileNameA`, `GetModuleHandleA` (the DLL base), `GetProcAddress` (returns 0), `GetCurrentProcessId`, `GetCurrentThread`, `GetSystemInfo`, `SetErrorMode`, `CreateMutexA`, `WaitForSingleObject`.
  - Text: `GetACP` (1252), `GetCPInfo`, `MultiByteToWideChar` and `WideCharToMultiByte` (cp1252), `AreFileApisANSI`, `SetFileApisToOEM`, `wsprintfA` (cdecl) and `wvsprintfA` (`%s %c %d %i %u %x %X` with width, zero-pad and `l`).
  - Arithmetic: `MulDiv` (`VirtualAlloc` is the host's, Task 2).
  - Resources: `FindResourceA`, `LoadResource`, `LockResource`, read from the PE resource directory.
  - Time: `FileTimeToLocalFileTime`, `LocalFileTimeToFileTime` (UTC is local), `FileTimeToSystemTime`, `SystemTimeToFileTime`.
  - Polite failures that a Windows 9x box would give, `last_error = 120`: `ImpersonateSelf` and `OpenThreadToken`. Each is listed explicitly; there is no catch-all.
- [ ] **Step 1: Write the failing tests.**
  - `test_wsprintf_formats` (`"%s-%05d-%x"` gives `"ab-00042-ff"`).
  - `test_multibyte_roundtrip_cp1252` (`"é"` round-trips).
  - `test_filetime_systemtime_roundtrip`.
  - `test_find_resource_reads_resource_directory` (a synthetic PE with one `RT_RCDATA` resource; extend `make_pe` with an optional `resources` argument).
  - `test_security_calls_fail_politely`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Add the remaining Win32 stand-ins for patchw32"`.

### Task 6: RTPatch applier

**Files:**
- Create: `bw2/rtpatch.py`
- Test: `tests/test_rtpatch.py`

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces:
  - `RtpError(Exception)`.
  - `RtpResult(modified: int, added: int, deleted: int, renamed: int, skipped: int, missing: int, invalid: int, files: list[str], seconds: float)`.
  - `parse_summary(lines: list[str]) -> RtpResult`, which reads the DLL's end-of-run count lines by their keyword and the number in parentheses.
  - `apply_rtp(dll: Path, rtp: Path, tree: Path, log: Path | None = None) -> RtpResult`:
    1. Load the DLL and call `DllMain(base, 1, 0)`.
    2. Call `RTPatchApply32@12('"P:\<rtp name>" "T:\"', callback, 1)`.
    3. Return the result.

    The callback returns a non-NULL pointer to an empty string for every id. It collects id 7 (current file) and id 9 (message) lines, and treats ids 3 and 4 as an error message. The full message log goes to `log`.

    It raises `RtpError` on any of these:
    - a non-zero return value;
    - an error message;
    - `missing + invalid > 0`;
    - a refused path;
    - a `Win32Unimplemented`.

    The error names the files involved.
- [ ] **Step 1: Write the failing tests.**
  - `test_parse_summary_counts`: synthetic lines in the DLL's layout (keyword, dots, `(n)`), written fresh, not copied.
  - `test_summary_with_ignored_files_fails` (`invalid=1` raises `RtpError` naming the file seen just before).
  - `test_apply_rtp_real_1_1` (skipped unless `BW2_GAMEFILES=1`): on an APFS clone (`/bin/cp -cR`) of `gamefiles/install` in `tmp_path`, `apply_rtp` returns `modified == 69`, `missing == invalid == 0`, and the return value is 0.
  - `test_apply_rtp_real_1_2` (skipped unless `BW2_GAMEFILES=1`): chained on the same clone, `modified == 8`, `added == 1`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** with `BW2_GAMEFILES=1`. Expected: PASS in about 2.5 minutes for both real tests. Record the times.
- [ ] **Step 5: Commit** `git commit -m "Apply RTPatch diffs with the shipped DLL under emulation"`.

### Task 7: Game tree build and manifest

**Files:**
- Create: `bw2/gametree.py`, `bw2/manifests/game-usa-1.2-fan1.42.tsv` (generated, facts only), `docs/research/patch-application.md`
- Modify: `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (3.2 and 9: RTPatch route decided), `docs/research/official-patches.md` (point to the new doc)
- Test: `tests/test_gametree.py`

**Interfaces:**
- Consumes: `patch_inputs`, `apply_rtp`, `extract_fan_exe(installer, out_dir) -> Path`, `write_manifest`/`read_manifest`, `ensure_free(path, needed)`.
- Produces:
  - `GROUPS` maps each patch kind to a list of `(group, destination)`, from the `data1.hdr` `<TARGETDIR>` strings:
    - 1.1: `Credits_English` to `Data`, `Text_English` to `Data/Text`, `Ape_Texture_Fix` to `Data/CTR/bape`, `Readme_English` to the root.
    - 1.2: `Credits_English` to `Data`, `Text_English` to `Data/Text`, `Ape_Texture_Fix` to `Data/Art/Textures`, `Readme_English` to the root, `ScriptDLL` to `Plugins`, `Scripts` to `Scripts/BW2`, `ATI_Fix` to `Data/Effects`, `GraphicConfigUpdate` to `Data`.
  - `copy_groups(groups_dir: Path, table, tree: Path) -> list[str]` copies files and matches existing names case-insensitively.
  - `build_game_tree(install: Path, patch11: Path, patch12: Path, fan_installer: Path, out: Path = GAMEFILES / "game") -> Path`. The steps:
    1. Clone `install` to `<out>.part` (APFS `clonefile` through `/bin/cp -c`; if that fails, check space and copy).
    2. Apply the 1.1 RTP, then copy the 1.1 groups.
    3. Apply the 1.2 RTP, then copy the 1.2 groups.
    4. Replace `white.exe` with the fan 1.42 exe.
    5. Check against `MANIFEST`, then rename to `out`.
  - CLI: `python -m bw2.gametree [--write-manifest]`. The flag writes the manifest instead of checking it, and is used once in this task.
- [ ] **Step 1: Write the failing tests.**
  - `test_copy_groups_case_insensitive_destinations` (a synthetic tree with `data/effects/skinned.sdv` gets `ATI_Fix/Skinned.sdv` in place, keeping the existing spelling).
  - `test_copy_fallback_checks_space` (monkeypatch the clone to fail and `shutil.disk_usage` to report too little space: `NotEnoughSpace` (from `bw2.diskspace`) names the bytes needed; nothing is copied).
  - `test_interrupted_build_not_reused` (a stale `game.part` from an earlier run is removed before the clone, and a failure after the clone leaves no `game` folder).
  - `test_manifest_mismatch_fails` (a one-byte change in the built tree raises naming the path).
  - `test_build_game_tree_real` (skipped unless `BW2_GAMEFILES=1`): a full build matches the committed manifest, and `white.exe` matches the `white-exe` row of `known_releases.tsv`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Then run `python -m bw2.gametree --write-manifest` once. Before writing it, check:
  - Every RTP and group file that landed is listed.
  - Each group file either replaced a same-named file or is one of the phase 0 "new" files: `t_ape_evil_scalebias.dds`, `t_ape_good_scalebias.dds`, `battery.dds`, `readme_v1_1.txt`, `readme_v1_2.txt`.
  - Record any surprise in the research doc.
- [ ] **Step 4: Run** the full suite with `BW2_GAMEFILES=1` and the audit. Expected: all pass, CLEAN.
- [ ] **Step 5: Write `docs/research/patch-application.md`:** the route, the spike's two bugs, the 58-API set, the timings, the group table and its source, the counts per patch, and that verification rests on the DLL's own per-file check plus the manifest. Update spec 3.2 and 9: RTPatch is applied by running `patchw32.dll` under Unicorn, so Unicorn joins the Builder's pinned Python.
- [ ] **Step 6: Commit** `git commit -m "Build the patched game tree"`.
