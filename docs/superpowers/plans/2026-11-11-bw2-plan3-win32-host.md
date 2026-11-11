# Plan 3: Win32 Host and Guest C++ Exceptions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Link the lifted game with pcrecomp's win32hle host into an engine binary, `out/bw2`. On the patched tree it runs the game's own startup headless: the CRT, `WinMain`, the protection VM as shipped, window creation and the game's threads. Guest C++ exceptions are dispatched. The run stops loudly at the first call into a layer a later plan owns (Direct3D 9, D3DX, DirectInput, DirectDraw, OpenAL/DirectSound, Bink).

**Architecture:** win32hle answers imports as "shims" that run inside the lifted model (`g_esp` points at `[ret][args]`). We link its headless core (no SDL2, no ffmpeg) and add a BW2 layer under `runtime/bw2/`:
- process entry, mounts, registry and support folder;
- O(1) dispatch, CPUID, TLS, big stacks, lifted-DLL loading;
- guest C++ exception dispatch, ported from pcrecomp's `native32/eh32.c` to based memory and POSIX;
- the disc/driver tripwire the user's standing rule needs;
- BW2's own answers for imports win32hle lacks.

The lifter gains landing pads (setjmp) in every function whose prologue installs an MSVC C++ EH frame. The runtime catches a throw at `RaiseException(0xE06D7363)`, finds the catch from the guest's own FuncInfo data, runs unwind and catch funclets as guest code, and longjmps into the owning lifted body.

**Tech Stack:** C (clang, arm64, macOS 13+), pcrecomp win32hle and recomp32 (vendored, MIT), Python 3.14 (lift driver, build, pytest), capstone, pefile. Test guests are assembled with Xcode clang (`asm32`) and lifted with our real lifter.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 5.1, 5.2 Memory and Files, 6.2, 6.5, 7.1, 7.2). Facts: `docs/research/white-exe-survey.md` (imports, EH counts), `docs/research/safedisc-leftovers.md` (the VM), `docs/research/build-pipeline.md` (lift and build).

## Measured before writing (2026-11-11)

- **Imports:** win32hle already answers 256 of `white.exe`'s 412 imports and 132 of `ScriptLibraryR.dll`'s 144.
  - Missing, owned by later plans: all of `d3d9`, `d3dx9_25` (60), `dinput8`, `DirectDrawCreateEx`, and `_BinkDX9SurfaceType@4`.
  - Missing, for this plan: 25 kernel32, 6 user32, 5 advapi32, 7 imm32, 3 winmm, 2 ole32, 2 shell32, plus comctl32/comdlg32/shlwapi/netapi32 (one or two each), 28 ws2_32 (wsock32 is answered by ordinal; ws2_32 is not registered), and 9 dbghelp.
- **SDL2:** only `gdidc.c`, `dsound.c`, `screen.c` and `present.c` include it. `bink.c` needs ffmpeg and `dsound.c`. `ddraw.c`, `d3d.c` and `d3d_raster.c` are DirectX 6 and unused.
- **Exceptions:** BW2's CRT is static. `_CxxThrowException` (`0xAD9B13`) builds the `0xE06D7363` record and calls the `RaiseException` import. `__CxxFrameHandler` is at `0xAD9240`, 1,892 functions have FuncInfo stubs, and there are 7 `_except_handler3` frames.
- **Lifter gaps:**
  - `bw2/lift.py` calls `Lifter.lift_function`, which has no EH landing pads; only `generate.py`'s linear path does.
  - `lift32.py` emits no `RECOMP_BACKEDGE`, so a lifted spin-wait never hands the machine to another thread.
  - On arm64, `CPUID_impl` does nothing, so the guest reads back its own registers.
- **win32hle gaps:**
  - No PE TLS directory support. White has a `.tls` section, and `fs:[0x2c]` is never set up.
  - `CreateThread` uses pthread's default stack. The cold shard's function alone needs a 557 KB host frame.
  - `LoadLibraryA` maps a code DLL anywhere and never runs it. `ScriptLibraryR.dll` is lifted at its base `0x10000000` and must run there.
  - `recomp_lookup` is a binary search.
  - `MAX_HANDLES` is 4096 and fixed.
  - The heap reuses freed blocks at once.
- **Imports that can probe a disc or driver:** `white.exe` imports `DeviceIoControl`, `Netbios`, `GetDriveTypeA`, `GetDiskFreeSpaceA`, `CreateFileA/W`, `LoadLibraryA` and `GetProcAddress`. VM ranges: `stxt774` `0x2428000`-`0x24293B4`, `.name0` `0x242A000`-`0x24508E5`.

## Global Constraints

- Never commit game material: exes, DLLs, extracted files, strings or bytes copied from them, or generated C. Facts only (addresses, sizes, hashes, paths, registry key paths). Run `third_party/pcrecomp/tools/audit_repo.py .`, which must report CLEAN, before any commit that adds data.
- **Standing rule (user):** the protection VM runs as shipped. If the VM, or the code it runs, asks for a disc or driver check, **stop and ask the user** before answering it. The tripwire in Task 9 enforces this mechanically. A tripped run is reported, never worked around.
- Fail loudly, never return 0 silently. Each of these stops the run and names the import, address or type:
  - an unimplemented import;
  - a lifter trap;
  - an unresolved call;
  - an uncaught guest C++ exception;
  - a non-C++ `RaiseException`;
  - a full table.
- Exit codes:
  - 0: the guest exited normally.
  - 2: boot failure.
  - 3: tripwire (the user must decide).
  - 4: reached a layer owned by a later plan; the message names the import.
  - Anything else ends through `abort()` after the message.
- Memory is based: `-DRECOMP_MEM_BASE=0x100000000000ull` for every generated shard and every runtime file.
- No SDL2 and no ffmpeg in this plan's link (my call: the Mac layers in plans 4 and 5 decide).
- Changes inside `third_party/pcrecomp/` are small, separate commits, each with one line in `third_party/pcrecomp.PATCHES` (file - what - why). No NFSU2Recomp code.
- Writes never touch the game tree. The support folder is `~/Library/Application Support/Black & White 2/`, or `$BW2_SUPPORT` if set (tests always set it). The guest sees:
  - the game at `C:\Program Files\EA GAMES\Black & White 2\` (my call);
  - its writes under that folder in `<support>/Game/`;
  - the rest of `C:` in `<support>/C/`;
  - user name `Player`;
  - My Documents at `C:\Documents and Settings\Player\My Documents`.
- The registry persists to `<support>/registry.txt`.
- Generated documents use only the ASCII hyphen-minus. Commit subjects are short and imperative, with no body or trailers. Identity is 007hacky007. Never push. Work on branch `plan3`.

## Review Focus

- **A function with an EH frame that returns normally, again and again, then a throw:** the throw must be caught by a live frame, never by a stale host frame from an earlier activation. (Task 6, `test_no_stale_eh_frames_after_return`.)
- **A recursive function that catches:** the throw resumes the newest activation that covers it, with that activation's `ebp` locals. (Task 7, `test_recursive_catch_resumes_right_activation`.)
- **A worker thread with a deep call chain or a huge frame:** it completes, with no host stack overflow. (Task 3, `test_worker_thread_deep_stack`.)
- **The game writing into its own install folder** (options, profiles): the write lands in `<support>/Game/`, the game tree is byte-for-byte unchanged, and a reread sees the new bytes. (Task 2, `test_writes_land_in_support_dir`.)
- **`ExitProcess` from a worker thread while the main thread waits:** the process exits with that code and the registry file is flushed. (Task 3, `test_exit_from_worker_thread`.)

---

### Task 1: Engine build on win32hle

**Files:**
- Create: `runtime/bw2/main.c` (minimal for now), `runtime/bw2/dispatch.c`, `runtime/bw2/absent.c`, `runtime/bw2/bw2.h`, `tests/test_engine.py`
- Modify: `bw2/build.py`, `bw2/pipeline.py`, `tests/fixtures.py`, `third_party/pcrecomp/runtime/win32hle/host_lite.c` (make `recomp_lookup` `RECOMP_WEAK`), `third_party/pcrecomp.PATCHES`

**Interfaces:**
- Consumes: `bw2.lift.lift_module`, `link_tables`; `tests/fixtures.asm32`, `make_pe`.
- Produces:
  - `bw2/build.py`:
    - `HOSTS = {"linkcheck": [...], "engine": ENGINE_SOURCES}`.
    - `ENGINE_SOURCES` lists `runtime/bw2/*.c` (no `*_selftest.c`) plus these win32hle files: `host.c host_lite.c guest_mem.c pe_loader.c module.c hle_path.c kernel32.c kernel32_ext.c kernel32_crt.c user32.c gdi32.c winmm.c ole32.c advapi32.c wsock32.c lz32.c storage.c`.
    - `build(gen_dir, out, opt="-O3", jobs=None, host="engine") -> BuildStats`. Host objects are compiled into `gen_dir/obj/host-<name>/` with `-I` for recomp32, win32hle and `runtime/bw2`.
    - `MEM_BASE = "0x100000000000ull"`, added to `FLAGS` as `-DRECOMP_MEM_BASE=...`.
  - `pipeline` builds `out/bw2`.
  - `tests/fixtures.py`:
    - `asm32(src, base=None)`: with `base`, it applies the object's `R_386_32` and `R_386_PC32` relocations against its own `.text` symbols, as if `.text` sat at `base`. Labels can then hold data such as FuncInfo.
    - `guest_engine(tmp_path, pe: bytes, functions: list[int], exe_name="game.exe", dlls: dict[str, tuple[bytes, list[int]]] | None = None) -> Path`: writes the PE (and each DLL under its relative path), lifts the listed function starts with `bw2.lift`, links with the engine host, and returns the binary. Engine host objects are cached in one session-scoped directory.
    - `run_engine(binary, game_dir, support_dir, *args, timeout=60) -> CompletedProcess`.
- [ ] **Step 1: Write the failing tests** in `tests/test_engine.py`.
  - `test_engine_runs_lifted_entry`: a PE whose entry pushes 7 and calls `ExitProcess`. `run_engine` returns code 7.
  - `test_unimplemented_import_named`: a PE importing `KERNEL32.dll!BogusCall` boots (the bind is permissive by default), and the entry calls it. Expect a non-zero exit and `BogusCall` in stderr.
  - `test_strict_lists_unanswered_imports`: the same PE with `--strict` exits 2 before running, and lists `BogusCall`.
  - `test_dispatch_is_hashed`: an engine with 3 lifted functions answers `recomp_lookup` for each and NULL for `entry+1`. Check this through `--selftest-dispatch`, which prints `dispatch ok <n>`.
  - `test_guest_memory_is_based`: `--selftest-mem` prints `mem base 0x100000000000`.
- [ ] **Step 2: Run** `.venv/bin/python -m pytest -q tests/test_engine.py`. Expected: FAIL (no `guest_engine`, no engine host).
- [ ] **Step 3: Implement.**
  - `dispatch.c`: an open-addressing hash, size the next power of two at least 2x `recomp_dispatch_count`, built once at boot from `recomp_dispatch_table`.
  - `absent.c`: one aborting definition, naming itself, for each symbol the linker reports missing from the excluded win32hle files.
  - `main.c`: argument parsing, `recomp_host_init`, boot, run, exit code. Mounts come in Task 2. The bind is permissive by default, so an unanswered import stops the run, naming itself, when it is first called. `--strict` lists every unanswered import and exits 2 instead (my call: the game has 156 unanswered imports, and most of them never run).
- [ ] **Step 4: Run** the tests, then `.venv/bin/python -m pytest -q tests/test_build.py tests/test_pipeline.py`, and `BW2_GAMEFILES=1 .venv/bin/python -m bw2.pipeline --build-only`. Expected: PASS, and `out/bw2` links with all 49,283 functions.
- [ ] **Step 5: Commit** the vendored change first (`git commit -m "Make win32hle's dispatch lookup overridable"`), then the rest (`git commit -m "Link the lifted game with the win32hle host"`).

### Task 2: Mounts, support folder, registry

**Files:**
- Create: `runtime/bw2/mount.c`, `runtime/bw2/shell.c` (shell32, shlwapi, user name)
- Modify: `runtime/bw2/main.c`, `runtime/bw2/bw2.h`
- Test: `tests/test_engine.py`

**Interfaces:**
- Produces:
  - `int bw2_mount(const char *game_dir, const char *support_dir)` sets up the guest layout in Global Constraints. The suggested mechanism is `<support>/C/Program Files/EA GAMES/Black & White 2` as a symlink to the game tree, plus `hle_set_overlay(that path, <support>/Game)`.
  - `GetModuleFileNameA(NULL)` answers `C:\Program Files\EA GAMES\Black & White 2\white.exe`, and the current directory starts at the install folder.
  - `SHGetFolderPathA` answers CSIDL_PERSONAL, CSIDL_APPDATA and CSIDL_LOCAL_APPDATA under `C:\Documents and Settings\Player\`. `SHCreateDirectoryExA` creates the folder chain. `GetUserNameA` answers `Player`.
  - The registry is loaded from, and saved to, `<support>/registry.txt` with `hle_reg_load` and win32hle's save. Nothing is seeded in this task.
  - CLI: `bw2 --game DIR [--exe white.exe] [--strict] [--trace-eh] [--lifted-dll NAME=0xBASE]...`.
- [ ] **Step 1: Write the failing tests.**
  - `test_writes_land_in_support_dir`: the guest does `CreateFileA("data.txt", GENERIC_WRITE)` in the install folder, writes `xyz`, closes, and reads it back. The read returns `xyz`. `<support>/Game/data.txt` holds `xyz`. The game dir's file list and hashes are unchanged.
  - `test_game_files_read_case_insensitively`: reading `DATA\A.TXT` opens `game/Data/a.txt`.
  - `test_module_path_and_cwd`: `GetModuleFileNameA` and `GetCurrentDirectoryA` return the pinned paths.
  - `test_my_documents_path`: `SHGetFolderPathA(CSIDL_PERSONAL)` returns the pinned path, and writing a file there creates `<support>/C/Documents and Settings/Player/My Documents/<name>`.
  - `test_registry_persists`: run 1 does `RegCreateKeyExA` + `RegSetValueExA`; run 2 reads the value.
- [ ] **Step 2: Run** the tests. Expected: FAIL.
- [ ] **Step 3: Implement.** If `hle_path.c` cannot express the overlay, patch it with the smallest change and add a `PATCHES` line.
- [ ] **Step 4: Run** `.venv/bin/python -m pytest -q tests/test_engine.py`. Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Mount the game tree read-only with writes in the support folder"`.

### Task 3: Threads: big stacks, TLS, fair hand-over, exit

**Files:**
- Create: `runtime/bw2/threads.c`
- Modify: `third_party/pcrecomp/runtime/win32hle/kernel32_crt.c` (stack size from a host-set `hle_thread_stack_size`), `third_party/pcrecomp/tools/lift/lift32.py` (emit `RECOMP_BACKEDGE();` before every jump whose target is at or below the jump's own address), `third_party/pcrecomp.PATCHES`, `runtime/bw2/main.c`
- Test: `tests/test_engine.py`, `tests/test_lifter_patches.py`

**Interfaces:**
- Produces:
  - The guest main thread runs on a host pthread with a 1 GB stack; workers get 256 MB. Both have a `sigaltstack` so a stack overflow still prints its crash report.
  - `recomp_yield_hook = hle_yield`.
  - `void bw2_tls_thread_init(void)` runs on every guest thread before its first guest code. If the main image or a lifted DLL has a TLS directory, each thread gets a copy of the template (zero-filled to `SizeOfZeroFill`). The image's `AddressOfIndex` slot holds the module's index, and `TEB+0x2C` points at the thread's slot array. Allocations come from guest memory.
- [ ] **Step 1: Write the failing tests.**
  - `test_backedge_emitted`: lifting `L: dec ecx; jnz L` yields `RECOMP_BACKEDGE()` before the branch; a forward `jnz` does not.
  - `test_spin_wait_hands_over`: thread A spins on a guest dword until thread B (made with `CreateThread`) sets it. The run exits 0 within 10 s.
  - `test_worker_thread_deep_stack`: a worker calls a lifted function that recurses 20,000 times with a 64-byte guest frame. It also calls a host C helper `bw2_test_big_frame` (compiled only with `-DBW2_SELFTEST`) that touches a 600 KB local array. Exit 0.
  - `test_tls_per_thread`: a PE with a TLS directory (template dword `0x1234`) reads `fs:[0x2c]` -> `[idx*4]` -> dword. The main thread and a worker each see `0x1234`; the worker writes `5`, and main still reads `0x1234`.
  - `test_exit_from_worker_thread`: a worker calls `ExitProcess(9)` while main is in `WaitForSingleObject(INFINITE)`. Exit code 9, and a registry value set before exiting is in `registry.txt`.
- [ ] **Step 2: Run** the tests. Expected: FAIL.
- [ ] **Step 3: Implement.** Make the vendored changes (each its own commit) and `threads.c`. Run the pcrecomp difftest after the `lift32.py` change: `.venv/bin/python third_party/pcrecomp/tools/lift/difftest.py`. Expected: the same pass count as before, with the one known `sqrtps` failure.
- [ ] **Step 4: Run** `.venv/bin/python -m pytest -q tests/test_engine.py tests/test_lifter_patches.py`. Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Give guest threads big stacks, TLS and fair hand-over"`.

### Task 4: CPUID and RDTSC on arm64

**Files:**
- Create: `runtime/bw2/cpu.c`
- Modify: `third_party/pcrecomp/runtime/recomp32/recomp_types.h` (on non-x86 hosts, `CPUID_impl` calls `void recomp_cpuid(uint32_t leaf, uint32_t sub)` and `__rdtsc` calls `uint64_t recomp_rdtsc(void)`, both defined by the host), `third_party/pcrecomp.PATCHES`
- Test: `tests/test_engine.py`

**Interfaces:**
- Produces: a fixed virtual CPU (my call; it matches what the lifter implements):

| Leaf | eax | ebx | ecx | edx |
|---|---|---|---|---|
| 0 | `1` | `GenuineIntel`: `0x756E6547` | `0x6C65746E` | `0x49656E69` |
| 1 | `0x000006F6` | `0x00010800` | `0` | `0x0789FBFF` |
| `0x80000000` | `0x80000004` | `0` | `0` | `0` |
| `0x80000001` | `0` | `0` | `0` | `0` |
| `0x80000002`-`4` | brand string `BW2 virtual CPU @ 3.00GHz`, zero-padded | | | |
| anything else | `0` | `0` | `0` | `0` |

  Leaf 1 edx `0x0789FBFF` means FPU through CLFSH, MMX, FXSR, SSE and SSE2, with no HTT, no SSE3 and no 3DNow!. RDTSC is a monotonic count at a fixed 3 GHz (`BW2_TSC_HZ = 3000000000`): nanoseconds since boot times 3, so it tracks real time. pcrecomp's plain nanosecond count would read as a 1 GHz CPU, below BW2's minimum of about 1.6 GHz. A game that sets its detail level from measured clock speed would then pick low settings and save them for good, as NFSU2 did (spec 7.1).
- [ ] **Step 1: Write the failing tests.**
  - `test_cpuid_answers`: a guest runs `cpuid` for leaves 0, 1 and `0x80000002` and stores the registers. Assert the table values.
  - `test_rdtsc_tracks_time`: a guest reads `rdtsc`, calls `Sleep(200)`, and reads it again. The delta is between 0.45e9 and 1.2e9 (3 GHz).
- [ ] **Step 2: Run.** Expected: FAIL (CPUID leaves the registers unchanged, and the RDTSC delta is about 0.2e9).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** the vendored change, then `git commit -m "Answer CPUID with a fixed virtual CPU"`.

### Task 5: Lifted DLLs

**Files:**
- Create: `runtime/bw2/dll.c`
- Modify: `third_party/pcrecomp/runtime/win32hle/module.c` (a hook `int (*hle_lifted_dll_hook)(const char *base_name, const char *host_path, uint32_t *hmod)` consulted first by `LoadLibraryA`), `third_party/pcrecomp.PATCHES`, `tests/fixtures.py` (`make_pe(..., exports=dict[str, int], dll=True, tls=...)`)
- Test: `tests/test_engine.py`

**Interfaces:**
- Produces: a lifted-DLL table holding `scriptlibraryr.dll` at `0x10000000` by default, extended by each `--lifted-dll NAME=0xBASE` (the tests use this). A listed DLL:
  - maps at its preferred base without relocation, or stops loudly if any page is taken;
  - has its imports bound by the same resolver as the main image;
  - gets its TLS set up;
  - has its lifted entry called once, as `DllMain(hmod, DLL_PROCESS_ATTACH, 0)`. A FALSE result fails `LoadLibraryA` with error 1114.
  - `GetProcAddress` answers its exports by name and ordinal. A second `LoadLibraryA` only counts a reference.
- [ ] **Step 1: Write the failing tests.**
  - `test_lifted_dll_loads_at_base`: a main PE calls `LoadLibraryA("plugins\\Lib.dll")` and `GetProcAddress(h, "Add2")`, then calls it with 3 and 4. The result is 7, and `DllMain` ran exactly once (it increments a DLL global the main image reads through a second export).
  - `test_lifted_dll_base_taken_fails_loudly`: the DLL's base overlaps the main image. Expect a non-zero exit naming the DLL and the base.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** the vendored change, then `git commit -m "Load lifted DLLs at their base and run DllMain"`.

### Task 6: EH landing pads in the lift

**Files:**
- Create: `runtime/bw2/bw2_lift.h` (included by generated shards), `runtime/bw2/eh.c` (the frame list only: `recomp_eh_push/pop/owner/resume`, `bw2_eh_bad_resume`, `bw2_eh_depth`, thread-local; derived from pcrecomp `runtime/native32/eh32.c`, MIT, attribution in its header comment)
- Modify: `bw2/lift.py` (`HEADER` includes `bw2_lift.h`; EH pass), `bw2/build.py` (include path)
- Test: `tests/test_lift.py`, `tests/test_engine.py`

**Interfaces:**
- Consumes: `third_party/pcrecomp/tools/lift/eh32.py` `resume_points(md, read, insns)`.
- Produces:
  - For a function whose `resume_points` set is non-empty, `bw2/lift.py` rewrites the lifted text:
    - After the `RECOMP_ENTER` line it inserts `BW2_EH_ENTER();`.
    - Every `return;` becomes `{ BW2_EH_LEAVE(); return; }`.
    - Before the closing brace it adds `_eh_resume: switch (_ehf.resume) { case 0x<va>u: goto L_<va>; ... default: bw2_eh_bad_resume(0x<func>u, _ehf.resume); return; }`.
  - A resume point that is not a block label in the lifted body raises `LiftError` naming the function and the address.
  - `BW2_EH_ENTER` pushes a `recomp_ehframe_t` (`entry_esp = g_esp`). On `setjmp` returning nonzero it runs `RECOMP_REGS_IN()`, `_flag_k = FK_NONE`, `_df = 1`, then `goto _eh_resume`.
  - `ModuleLift` gains `eh_functions: int` and `eh_resume_points: int`.
- [ ] **Step 1: Write the failing tests.**
  - `test_eh_function_gets_landing_pad` (`tests/test_lift.py`): an asm32 function with the MSVC prologue (`push ebp; mov ebp, esp; push -1; push handler; mov eax, fs:[0]; push eax; mov fs:[0], esp`). Its handler stub is `mov eax, FuncInfo; jmp X`, with one try block whose catch funclet returns `L_cont`. The lifted text contains `BW2_EH_ENTER();` and `case 0x<L_cont>u: goto L_<L_cont>;`, and every `return;` is wrapped.
  - `test_plain_function_unchanged`: no prologue, so no `BW2_EH_ENTER`.
  - `test_no_stale_eh_frames_after_return` (`tests/test_engine.py`): an EH function is called and returns 1,000 times. Then the selftest export `bw2_eh_depth()`, printed through `--selftest-eh-depth` at exit, reports 0.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Re-lift white and slr with `python -m bw2.pipeline --lift-only`. Record the `eh_functions` and `eh_resume_points` counts and the time.
- [ ] **Step 4: Run** the tests, then `BW2_GAMEFILES=1 .venv/bin/python -m bw2.pipeline`. Expected: PASS, no `LiftError`, and `out/bw2` links.
- [ ] **Step 5: Commit** `git commit -m "Give lifted EH functions a landing pad"`.

### Task 7: Guest C++ exception dispatch

**Files:**
- Modify: `runtime/bw2/eh.c` (add the dispatch), `runtime/bw2/main.c` (register BW2's `RaiseException` ahead of win32hle's)
- Test: `tests/test_engine.py`

**Interfaces:**
- Produces:
  - `RaiseException(code, flags, n, args)`:
    - Code `0xE06D7363` with 3 arguments (magic `0x19930520`-`0x19930522`, object, ThrowInfo) dispatches as eh32 does, walking `fs:[0]` from the TIB.
    - Object and ThrowInfo 0 is a rethrow of the thread's current exception.
    - Any other code stops loudly with the code, `g_cur_func` and the return address.
  - The port changes from eh32:
    - guest memory through `hle_ptr` (based);
    - `_Thread_local` for thread state;
    - `IsBadReadPtr` replaced by a check that the address lies inside a mapped image;
    - an uncaught throw prints `uncaught C++ exception <decorated type> thrown from 0x<site>` and aborts;
    - a C++ throw whose catch lies above an `_except_handler3` frame stops loudly, naming that frame (its `__finally` would not run).
  - `--trace-eh` prints one line per throw and catch.
- [ ] **Step 1: Write the failing tests.** Each guest is asm32 with hand-built MSVC 7.1 EH data (FuncInfo, TryBlockMapEntry, HandlerType, ThrowInfo, CatchableTypeArray, CatchableType, TypeDescriptor), laid out as eh32.c reads it. The thrower calls an asm `_CxxThrowException` that calls `RaiseException(0xE06D7363, 1, 3, [0x19930520, obj, info])`.
  - `test_throw_caught_one_frame_up`: catch(int) stores the value, and the continuation returns `[ebp-8]` set before the try. Exit code is the thrown value plus the local.
  - `test_unwind_runs_destructors`: an intermediate frame's unwind-map action increments a counter. Counter 1, caught above.
  - `test_catch_all`: catch(...) catches a struct type.
  - `test_type_mismatch_goes_outward`: an inner catch(float) is skipped and the outer catch(int) catches.
  - `test_rethrow_caught_outside`: `throw;` inside a catch reaches the outer catch with the same object.
  - `test_recursive_catch_resumes_right_activation`: depth 3 recursion; the throw at depth 3 is caught by the depth-3 activation and returns its depth.
  - `test_uncaught_throw_named`: non-zero exit, with `uncaught C++ exception` and the type name in stderr.
  - `test_other_raise_is_loud`: `RaiseException(0x12345678, ...)` gives a non-zero exit naming `0x12345678`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `.venv/bin/python -m pytest -q tests/test_engine.py`. Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Dispatch guest C++ exceptions from guest EH data"`.

### Task 8: Heap quarantine and growable handle table

**Files:**
- Modify: `third_party/pcrecomp/runtime/win32hle/guest_mem.c`, `third_party/pcrecomp/runtime/win32hle/host_lite.c`, `third_party/pcrecomp.PATCHES`
- Test: `tests/test_engine.py`

**Interfaces:**
- Produces:
  - Freed heap blocks join a FIFO quarantine of 64 MB (`hle_heap_quarantine_bytes`, host-settable) before they can be reused. Their contents are left as they were, not poisoned.
  - The handle table grows by doubling, with no fixed cap below 2^20 handles; at the cap it stops loudly.
- [ ] **Step 1: Write the failing tests.**
  - `test_freed_block_not_reused_at_once`: `HeapAlloc` 64 bytes, write a pattern, `HeapFree`, then `HeapAlloc` 64 bytes 100 times. No allocation returns the freed address, and the freed bytes still hold the pattern.
  - `test_many_handles`: 10,000 open events, then all closed. Exit 0.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS. Rerun win32hle's own selftests (`third_party/pcrecomp/runtime/win32hle/build_selftests.sh` needs SDL2_ttf; if it is not installed, build only `host_selftest.c` and `kernel32_ext_selftest.c` with the core, as the script does).
- [ ] **Step 5: Commit** each vendored change, `git commit -m "Quarantine freed guest heap blocks"` and `git commit -m "Let the win32hle handle table grow"`.

### Task 9: Disc and driver tripwire

**Files:**
- Create: `runtime/bw2/guard.c`
- Test: `tests/test_engine.py`

**Interfaces:**
- Produces: BW2's own `DeviceIoControl`, `Netbios`, `GetDriveTypeA/W`, `GetVolumeInformationA/W`, `GetLogicalDrives`, `GetLogicalDriveStringsA`, `QueryDosDeviceA`, `mciSendCommandA` and `mciSendStringA` (registered ahead of win32hle), and a wrapped `CreateFileA/W`.
  - `DeviceIoControl`, `Netbios`, any `CreateFile` of a `\\.\` or `\\?\` device path, and any `mci*` call naming `cdaudio` always trip.
  - The others trip when the caller's return address or `g_cur_func` is inside the main image's `stxt774` or `.name0` section. The ranges are read from its section table at boot, not hard-coded. Otherwise they forward to win32hle's answer, or for the ones win32hle lacks, answer a fixed hard disk C: only.
  - A trip prints `TRIPWIRE: <call>(<args>) from 0x<ret> in sub_<func> (VM: yes|no). Per the user's rule this needs their decision.` plus the last 64 function entries, then exits 3.
  - `GetProcAddress` for any guarded name returns the guarded shim.
- [ ] **Step 1: Write the failing tests.**
  - `test_deviceiocontrol_trips`: exit 3, with `TRIPWIRE: DeviceIoControl` in stderr.
  - `test_drive_type_from_game_answers`: a guest outside the VM ranges calls `GetDriveTypeA("C:\\")` and gets 3 (DRIVE_FIXED); exit 0.
  - `test_drive_type_from_vm_range_trips`: the test PE puts its caller in a section named `stxt774`. Exit 3, with `VM: yes`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Stop on disc and driver checks for the user"`.

### Task 10: Boot the game to the graphics layer

**Files:**
- Create: `runtime/bw2/later.c` (exit 4 for later plans' imports), `runtime/bw2/shims_*.c` (grouped by DLL), `runtime/bw2/shims_selftest.c`, `docs/research/boot.md`
- Modify: `runtime/bw2/main.c`, `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (5.2 and 6.5: what plan 3 measured), `docs/research/build-pipeline.md` (EH counts)
- Test: `tests/test_engine.py`, `tests/test_boot.py`

**Interfaces:**
- Produces:
  - `later.c` registers every `d3d9`, `d3dx9_25` and `dinput8` import, `DirectDrawCreateEx`, `DirectDrawEnumerateExA`, `DirectSoundCreate*`, every `binkw32` import, and `LoadLibraryA` of `OpenAL32.dll`, `wrap_oal.dll` or `dsound.dll`. Each prints `[bw2] reached <name>: owned by a later plan` and exits 4.
  - BW2's own shims answer every other import the boot reaches. Each new shim gets a check in `shims_selftest.c` first. The selftest is run by `test_shims_selftest`, which builds it with the engine core.
- [ ] **Step 1: Write the failing test.** In `tests/test_boot.py`, `test_boot_reaches_graphics` (gamefiles-only): `out/bw2 --game gamefiles/game` with a fresh `BW2_SUPPORT` exits 4 within 120 s. stderr has no `TRIPWIRE`, `trap`, `unresolved`, `unimplemented` or `uncaught`.
- [ ] **Step 2: Run** `BW2_GAMEFILES=1 .venv/bin/python -m pytest -q tests/test_boot.py`. Expected: FAIL at the first missing piece.
- [ ] **Step 3: Bring-up loop.** Run the engine, read the loud stop, and act on it:
  - **Missing import:** add its check to `shims_selftest.c`, watch it fail, implement it, and rerun.
  - **Trap or unresolved address:** find the cause in the catalog or lifter first (systematic debugging). Record each fix in `boot.md`.
  - **Tripwire:** **stop the plan and ask the user**, with the trip message, the call's arguments and the entry trace.
  - **Registry value the game expects from its installer:** seed it in `main.c` on first run, only under `Software\Lionhead Studios Ltd\Black & White 2` and `Software\Electronic Arts\Black and White 2`. Record each key, value name and our chosen value in `boot.md`.
- [ ] **Step 4: Run** `BW2_GAMEFILES=1 .venv/bin/python -m pytest -q` (the whole suite). Expected: PASS. Write `docs/research/boot.md`:
  - imports reached, in first-call order, with counts;
  - threads created;
  - exceptions thrown and caught (`--trace-eh` summary);
  - VM functions entered;
  - the CPU speed the game measures, and the detail settings it chooses and saves on first run (registry or files). If it picks below its defaults because of CPU speed, that is a finding to fix in this task;
  - registry values seeded;
  - files opened, by folder;
  - wall time to exit 4;
  - which later-plan import stopped it.
  Update the spec and `build-pipeline.md`.
- [ ] **Step 5: Commit** `git commit -m "Boot the lifted game to its graphics setup"`.
