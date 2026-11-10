# BW2 Plan 1 (Lifter and Build Pipeline) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift all game code (`white.exe` `.text`, the protection VM interpreter, and patch 1.2's `ScriptLibraryR.dll`) to C with committed code, compile it with clang for Apple Silicon, and link it against a minimal link-check host. Measure lift time, C size, compile time and binary size.

**Architecture:** `bw2/lift.py` drives pcrecomp's `lift32.Lifter` over our committed catalogs. Each module goes to its own sharded directory under `gen/` (gitignored). A link step merges the modules into one header, one dispatch table and one set of loud stubs for call targets nothing lifted. `bw2/build.py` compiles the shards in parallel with fixed flags and links them with `runtime/bw2/linkcheck.c`. That host only defines what generated code needs and aborts on anything unresolved; plan 3 replaces it with pcrecomp's win32hle. Lifter gaps (loud traps, `pushf`/`pushfd`, MMX) are fixed in the vendored pcrecomp as separate commits.

**Tech Stack:** Python 3.14 venv (capstone, unicorn, pefile, pytest), Apple clang from Xcode Command Line Tools, unshield, pcrecomp at `2adc02f` (vendored, with our patches on top).

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 5, 6.1, 6.2, 6.3, 7.1, 7.5) and `docs/research/phase0-summary.md` (next-plan item 1).

## Global Constraints

- Never commit game material or anything generated from it: no C under `gen/`, no objects, no binaries (`out/`), no exe or DLL bytes, no shader text. Catalogs (addresses, extents, kinds) and fingerprints are fine. Run `third_party/pcrecomp/tools/audit_repo.py .` before each commit that adds data; it must say CLEAN.
- Changes to vendored pcrecomp go in their own commits, each touching only `third_party/pcrecomp/`. Record them in `third_party/pcrecomp.PATCHES` (one line each: file, what, why).
- Loud failure: an unimplemented instruction, an unresolved direct call target, or an unresolved indirect call must stop the run and name the guest address. No silent `eax = 0`.
- Compiler flags (spec 7.1, 7.5): always `-fno-strict-aliasing -mcpu=apple-m1 -mmacosx-version-min=13.0` and an explicit optimisation level. The build refuses an empty level.
- Generated output is byte-reproducible: the same inputs give identical files.
- Git identity is already set (`007hacky007`), no remote, never push. Commit messages are one short imperative line, with no body and no AI attribution. Docs use ASCII hyphen-minus only.
- Tests that need real game files carry `@pytest.mark.gamefiles` and run only with `BW2_GAMEFILES=1`. Full lifts and builds are run as steps, not as tests.
- The SafeDisc VM runs as shipped (user decision). If any step finds the VM functions call disc or driver checks, stop and ask the user.

## Review Focus

- Interrupted lift or build (Ctrl-C, ENOSPC, clang crash): no half-written shard, object or binary may later be reused as complete. Write to `.part` and rename; a rebuild must recompile any shard whose object is missing or older than its source. (Tests in Tasks 7 and 9.)
- Clang failing on one shard among hundreds: the build must fail, name the shard and show the end of clang's stderr. It must not link a binary that is missing that shard's functions. (Test in Task 9.)
- A call from one module into another (game to VM thunk, game to DLL export): it must resolve through the merged tables, not become a stub. (Test in Task 8.)
- Running the lift twice: identical bytes, so rebuilds and diffs stay cheap. (Tests in Tasks 7 and 8.)
- Xcode Command Line Tools missing: fail with a message saying to install them. Detect with `xcode-select -p` plus a file check, never by running `clang` blindly. (Test in Task 9.)

---

### Task 1: Patch 1.2's ScriptLibraryR.dll from committed code

The DLL comes whole in patch 1.2's `ScriptDLL` group; phase 0 got it with a scratch script. Here that path becomes committed code. The overlay splitter is also what plan 2 needs.

**Files:**
- Create: `bw2/is11.py`, `bw2/patches.py`
- Modify: `bw2/known_releases.tsv` (row `script-dll`)
- Test: `tests/test_is11.py`, `tests/test_patches.py`

**Interfaces:**
- Consumes: `bw2.releases.identify`, `bw2.download.FingerprintMismatch`
- Produces:
  - `bw2.is11.split_overlay(setup_exe: Path, out: Path) -> list[Path]`: walks the entries after the PE image. Each entry is four NUL-terminated strings (name, path such as `Disk1\data1.cab`, version, decimal size) followed by the bytes. Writes each file to `out/<path with / separators>` via `.part` plus rename, and returns the written paths in order. A size that runs past EOF raises `ValueError` naming the entry.
  - `bw2.patches.extract_patch_groups(patch_exe: Path, kind: str, out: Path) -> Path`: checks the fingerprint (`kind` is `patch-1.1` or `patch-1.2`), runs `split_overlay` into `out/setup`, then `unshield -d out/setup/Disk1 x -d out/groups out/setup/Disk1/data1.hdr`, and returns `out/groups`.
  - `bw2.patches.script_dll(patch12_exe: Path, out_dir: Path) -> Path`: returns `out_dir/ScriptLibraryR.dll`, fingerprint-checked as `script-dll`.

- [ ] **Step 1: Failing tests.** `tests/test_is11.py`:
  - `test_split_overlay_roundtrip`: build a `make_pe` image, then append entries `("a.cab", "Disk1\\a.cab", "0.0.0.0", b"x"*5000)` and `("setup.ini", "Disk1\\setup.ini", "0.0.0.0", b"[x]")`. Expect two files with those bytes at `Disk1/a.cab` and `Disk1/setup.ini`.
  - `test_split_overlay_truncated_raises` (the last entry claims more bytes than remain): expect `ValueError` and no `.part` left.

  `tests/test_patches.py`, `@pytest.mark.gamefiles` `test_script_dll_from_patch12`: the DLL from `gamefiles/patches/BW2Patch1_2.exe` is 655,360 bytes and matches the new row.
- [ ] **Step 2: Run, expect FAIL** (`ModuleNotFoundError`).
- [ ] **Step 3: Implement** `bw2/is11.py` and `bw2/patches.py`. Compute the SHA-256 of the real DLL and add the row `script-dll  ScriptLibraryR.dll 1.2  655360  <sha>  from patch 1.2 ScriptDLL group`.
- [ ] **Step 4: Run** `BW2_GAMEFILES=1 .venv/bin/python -m pytest tests/test_is11.py tests/test_patches.py -v`. Expected: all pass.
- [ ] **Step 5: Commit.** `git commit -m "Extract patch files and ScriptLibraryR.dll"`

### Task 2: Loud failures in the vendored lifter and runtime header

**Files:**
- Modify: `third_party/pcrecomp/tools/lift/lift32.py` (the `UNIMPLEMENTED` emission, around line 2176)
- Modify: `third_party/pcrecomp/runtime/recomp32/recomp_types.h` (the unresolved branch of `RECOMP_ICALL` and `RECOMP_ITAIL`)
- Create: `third_party/pcrecomp.PATCHES`
- Test: `tests/test_lifter_patches.py`

**Interfaces:**
- Produces, for generated C and the host:
  - `RECOMP_TRAP(va, "mnemonic")`, a macro emitted instead of the `/* UNIMPLEMENTED */` comment. It expands to `recomp_trap((uint32_t)(va), (mnem))`.
  - `void recomp_trap(uint32_t va, const char *mnem);` declared in `recomp_types.h`. The host defines it.
  - `void recomp_unresolved_icall(uint32_t va, uint32_t from);` declared in `recomp_types.h` and called on the unresolved path of `RECOMP_ICALL`/`RECOMP_ITAIL` instead of `fprintf` + `eax = 0`. The host defines it (abort by default).
- Test helper: a fixture in `tests/conftest.py`, `lifted_c(pe_bytes: bytes, va: int) -> str`, that lifts one function with pcrecomp directly (Task 7's `lift_one` comes later).

- [ ] **Step 1: Failing tests.**
  - `test_unimplemented_becomes_trap`: a synthetic PE whose `.text` is `F4 C3` (`hlt; ret`). Its lifted C contains `RECOMP_TRAP(0x00401000, "hlt")` and not `UNIMPLEMENTED`.
  - `test_unresolved_icall_calls_hook`: the text of `recomp_types.h` has `recomp_unresolved_icall(_va, g_cur_func)` inside both `RECOMP_ICALL` and `RECOMP_ITAIL`, and has no `eax = 0` in those macros.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** both edits. Keep the pcrecomp comment explaining why nothing is popped on the unresolved path. Write `third_party/pcrecomp.PATCHES` with two lines.
- [ ] **Step 4: Run** `.venv/bin/python -m pytest tests/test_lifter_patches.py -v`, expect pass. Also run `pcr.run("tools/lift/difftest.py")` and expect the same pass count as before the change. Record the count in the commit's PATCHES line.
- [ ] **Step 5: Commit** (only `third_party/` and the test): `git commit -m "Make lifter gaps and unresolved calls trap"`

### Task 3: pushf/pushfd and popf/popfd with the ID bit

The VM interpreter starts with `pushfd; pushad` and has 20 `pushf` sites. NFSU2 lesson (spec 7.1): EFLAGS bit 21 (ID) must survive a `pushfd`/`popfd` round trip, or a CPUID check fails.

**Files:**
- Modify: `third_party/pcrecomp/tools/lift/lift32.py`, `third_party/pcrecomp/runtime/recomp32/recomp_types.h` (state for the non-arithmetic EFLAGS bits)
- Modify: `third_party/pcrecomp/tools/lift/difftest.py` (cases)
- Modify: `third_party/pcrecomp.PATCHES`
- Test: `tests/test_lifter_patches.py`

**Interfaces:**
- Produces: `extern uint32_t g_eflags_hi;`, which holds every EFLAGS bit the lazy model does not compute (DF stays where pcrecomp already keeps it; ID bit 21; AC bit 18; IF bit 9 always reads 1; bit 1 always reads 1). `pushfd` pushes `recomp_flags_pack() | g_eflags_hi | 0x202`. `popfd` restores the arithmetic flags through pcrecomp's existing flag-setting path and stores bits 18 and 21 into `g_eflags_hi`. `pushf`/`popf` (16-bit) do the same on the low word.

- [ ] **Step 1: Failing difftest cases** added to `difftest.py`, compared against Unicorn: `pushfd_after_cmp` (`cmp eax, ebx; pushfd; pop ecx`), `popfd_roundtrip_id` (`pushfd; or dword [esp], 0x200000; popfd; pushfd; pop ecx`, so ECX has bit 21 set), `pushf16` (`cmp al, bl; pushf; pop cx`). Plus a pytest `test_pushf_lifts_without_trap`: a synthetic `9C 66 9C 9D C3` lifts with no `RECOMP_TRAP`.
- [ ] **Step 2: Run** `pcr.run("tools/lift/difftest.py", "-k", "pushf")` and the pytest. Expect: the new cases FAIL or trap.
- [ ] **Step 3: Implement** in `lift32.py` and `recomp_types.h`.
- [ ] **Step 4: Run** the three difftest cases and the pytest, expect pass. Run the whole difftest suite and expect no regressions.
- [ ] **Step 5: Commit** `git commit -m "Lift pushf and popf with the ID bit"`

### Task 4: MMX instructions the game uses

Phase 0 counted these in reachable-looking code: `pmaddwd` 1,652, `punpckhdq` 294, `punpckldq` 294, `paddsw` 52, `pmulhw` 4, `punpcklwd` 4, `punpckhwd` 4, `pinsrw` 1. pcrecomp already models `g_mm[8]` (`recomp_types.h`) and has `runtime/recomp32/mmx_selftest.c`; read it to see which MMX forms exist.

**Files:**
- Modify: `third_party/pcrecomp/tools/lift/lift32.py` (or its MMX helper module, if one exists; follow where the existing MMX forms live)
- Modify: `third_party/pcrecomp/tools/lift/difftest.py`, `third_party/pcrecomp.PATCHES`

**Interfaces:**
- Produces: lifted C for each listed mnemonic, register and memory source forms, writing `g_mm[n]` with the exact integer semantics (`pmaddwd`: four signed 16x16 products, adjacent pairs summed into two int32; `paddsw`: signed saturating add of words; `pmulhw`: high 16 bits of signed products; unpacks interleave as the Intel SDM says; `pinsrw mm, r32/m16, imm8`: replace word `imm8 & 3`).

- [ ] **Step 1: Failing difftest cases**, one per mnemonic, each with a register source and a memory source. Use operands that hit saturation and sign boundaries: `0x7FFF + 0x0001` for `paddsw`, `0x8000 * 0x8000` for `pmaddwd`.
- [ ] **Step 2: Run** `pcr.run("tools/lift/difftest.py", "-k", "mmx")`. Expect: the new cases FAIL or trap.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the MMX cases and then the full difftest suite. Expect all pass and no regressions.
- [ ] **Step 5: Commit** `git commit -m "Lift the MMX instructions BW2 uses"`

### Task 5: Close discovery gaps in .text

**Files:**
- Create: `bw2/gaps.py`, `docs/research/discovery-gaps.md`
- Modify: `bw2/discover.py` (loop: discover, classify gaps, reseed, until no new seeds)
- Modify: `bw2/catalog/white-1.42.functions.json` (regenerated)
- Test: `tests/test_gaps.py`

**Interfaces:**
- Consumes: `bw2.discover.load_functions`, `discover`, `_union`; `bw2.pe_survey`
- Produces:
  - `@dataclass(frozen=True) class Gap: lo: int; hi: int; kind: str`. `kind` is one of `"int3"` (all `0xCC`), `"align"` (under 16 bytes, all `0xCC`/`0x90`/`0x00`), `"code"`, `"data"`, `"unknown"`.
  - `gaps(catalog: Path, exe: Path) -> list[Gap]`. Code evidence for a gap start: an `E8`/`E9` rel32 from covered code lands on it, an aligned dword in `.rdata`/`.data` points at it, or it opens with `55 8B EC` / `83 EC xx` / `81 EC`. Data evidence: more than 30% of the bytes in its first 64 decode to no valid instruction, or it is a run of dwords pointing into `.text` (a jump or pointer table).
  - `code_seeds(gaps: list[Gap]) -> list[int]`: starts of `"code"` gaps.
  - `discover(..., max_rounds: int = 4)`: after the first pass, reseeds with `code_seeds` until a round adds none or `max_rounds` is reached. Records the rounds in `bw2_seeds["rounds"]`.

- [ ] **Step 1: Failing tests** on a synthetic PE (`make_pe`):
  - `test_int3_and_align_gaps_classified`.
  - `test_gap_reached_by_call_is_code`: covered code has `E8` into an uncovered `55 8B EC ... C3`.
  - `test_pointer_table_gap_is_data`: three dwords pointing into `.text`.
  - `test_reseed_adds_function`: `discover` on a PE whose second function is reachable only from a pointer in `.data` ends with it catalogued.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `bw2/gaps.py` and the reseed loop.
- [ ] **Step 4: Run the unit tests and expect pass.** Then regenerate the real catalog: `discover(gamefiles/exe/white.exe, bw2/catalog/white-1.42.functions.json)`. Expect coverage not lower than 90.35%.
- [ ] **Step 5: Write `docs/research/discovery-gaps.md`:** coverage before and after; every remaining gap of 1 KB or more listed with kind and evidence; and the five shader call sites that phase 0 found in gaps (`0x97cdb9`, `0xa573fd`, `0xa8f19d`, `0xa8f22d`, `0xa9f93d`), each now covered or explained.
- [ ] **Step 6: Audit, commit.** `git commit -m "Reseed discovery from classified gaps"`

### Task 6: VM and DLL catalogs from committed code

**Files:**
- Create: `bw2/vm.py`, `bw2/catalog/white-1.42.vm.json`, `bw2/catalog/scriptlibraryr-1.2.functions.json`
- Test: `tests/test_vm.py`

**Interfaces:**
- Consumes: `bw2.xsect.cross_section_refs`, `bw2.discover.discover`, `compact_catalog`, `load_functions`
- Produces:
  - `read_jump_table(exe: Path, va: int, n: int) -> list[int]`
  - `VM_DISPATCH = 0x242A000`, `VM_HANDLER_TABLE = 0x242A885`, `VM_CODE_END = 0x242A885`. These are facts from `docs/research/safedisc-leftovers.md`.
  - `vm_seeds(exe: Path) -> list[int]`: the dispatcher, the 18 thunks (real `call` sites only, so the byte-scan false positive at `0x4142c9` is dropped because its target is not a `jmp rel32`), their 18 entries, and the in-range handler-table targets. Sorted and unique.
  - `vm_catalog(exe: Path, out: Path) -> Path`: discovery over `stxt774` + `.name0` with `vm_seeds`, then filtered to functions whose start is a seed or lies in `[VM_DISPATCH, VM_CODE_END)`. That drops the bytecode noise. Written in compact form.
  - DLL catalog: `discover(dll, out, code_sections=(".text",))`. No view is needed, but the call is the same.

- [ ] **Step 1: Failing tests:**
  - `test_read_jump_table` (synthetic PE with a 4-entry table).
  - `test_vm_seeds_drop_non_thunk_targets` (synthetic: one `call` to a `jmp rel32` thunk, one `call` into non-thunk bytes; only the first is a seed).
  - `@pytest.mark.gamefiles test_real_vm_seeds`: 228 seeds containing `0x242a000` and `0x2428f9c`.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `bw2/vm.py`.
- [ ] **Step 4: Run tests, expect pass.** Generate both catalogs. Check the VM catalog has 18 thunks, 18 entries and covers `[0x242a000, 0x242a885)`.
- [ ] **Step 5: Audit, commit.** `git commit -m "Catalog the VM interpreter and ScriptLibraryR.dll"`

### Task 7: Lift driver, per module

**Files:**
- Create: `bw2/lift.py`
- Test: `tests/test_lift.py`

**Interfaces:**
- Consumes: pcrecomp `pe_analyze.analyze_pe`, `build_iat_map`, `disasm32.Disassembler.disassemble_function`, `lift32.Lifter.lift_function` (all imported by path, as in `translator.py`); `bw2.discover.load_functions`, `code_view`
- Produces:
  - `@dataclass class ModuleLift: name: str; functions: int; shards: int; c_bytes: int; traps: dict[str, int]; defined: list[str]; referenced: list[str]; seconds: float`
  - `lift_module(name: str, exe: Path, catalog: Path, out_dir: Path, code_sections: tuple[str, ...], shard_size: int = 500) -> ModuleLift`. It lifts every non-alias function, in address order, from a `code_view` of `exe` with `code_sections` executable. It writes `out_dir/<name>_NNNN.c`, each starting `#include "recomp_types.h"` and `#include "recomp_funcs.h"`. It writes `out_dir/<name>.json` holding the `ModuleLift` fields. Shard files go through `.part` and rename. `referenced` holds every `sub_XXXXXXXX` named in `RECOMP_CALL`/tail-call positions; `traps` counts `RECOMP_TRAP` by mnemonic.
  - `lift_one(exe: Path, va: int, code_sections=(".text",)) -> str` (for tests and debugging).

- [ ] **Step 1: Failing tests** on a synthetic PE: `.text` holds `f1: call f2; ret` at `0x401000` and `f2: call 0x401100 (not catalogued); ret`.
  - `test_lift_module_writes_shards_and_json`: the shard has `void sub_00401000(void)` and `void sub_00401006(void)`. The json shows `defined` contains both and `referenced` contains `sub_00401100`.
  - `test_lift_is_reproducible`: two runs into two dirs give identical bytes in every file.
  - `test_shard_size_respected`: 3 functions with `shard_size=2` give 2 shards.
  - `test_interrupted_shard_not_reused`: a stale `x_0000.c.part` in the dir is overwritten, and no `.part` remains after the run.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `bw2/lift.py`.
- [ ] **Step 4: Run, expect pass.**
- [ ] **Step 5: Commit** `git commit -m "Add the per-module lift driver"`

### Task 8: Merge modules into one link set

**Files:**
- Modify: `bw2/lift.py`
- Test: `tests/test_lift.py`

**Interfaces:**
- Produces: `link_tables(out_dir: Path, modules: list[ModuleLift], entry: int) -> dict`. It writes:
  - `recomp_funcs.h`: every defined function, `void sub_X(void);`, sorted.
  - `recomp_dispatch.c`: `recomp_dispatch_table[]` with every defined function sorted by VA, `recomp_dispatch_count`, and `const uint32_t bw2_entry_va = <entry>;`.
  - `recomp_unresolved.c`: for each referenced but undefined symbol, `void sub_X(void) { recomp_unresolved_call(0xXu); }`.
  - It returns `{"functions": n, "unresolved": [...]}`, and the same goes to `link.json`.
  - `void recomp_unresolved_call(uint32_t va);` is declared in `recomp_funcs.h`; the host defines it.

- [ ] **Step 1: Failing tests:**
  - `test_cross_module_call_resolves`: module A references `sub_00402000`, which module B defines, so it is absent from `recomp_unresolved.c`.
  - `test_unresolved_gets_loud_stub`: `sub_00401100` gets a stub calling `recomp_unresolved_call(0x00401100u)`.
  - `test_dispatch_sorted_and_reproducible`.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run, expect pass.**
- [ ] **Step 5: Commit** `git commit -m "Merge lifted modules into one link set"`

### Task 9: Build: parallel clang and the link-check host

**Files:**
- Create: `bw2/build.py`, `runtime/bw2/linkcheck.c`
- Test: `tests/test_build.py`

**Interfaces:**
- Consumes: `link_tables` output in `gen/`; `third_party/pcrecomp/runtime/recomp32/recomp_types.h`
- Produces:
  - `class BuildError(Exception)`, raised with the shard name and the last 40 lines of clang's stderr.
  - `class ToolsMissing(Exception)`, with the message `Install Xcode Command Line Tools: xcode-select --install`.
  - `check_tools() -> Path`: path of `clang` under `xcode-select -p` (`usr/bin/clang`, or `Toolchains/XcodeDefault.xctoolchain/usr/bin/clang`). It never runs `/usr/bin/clang` before `xcode-select -p` succeeds.
  - `FLAGS = ("-fno-strict-aliasing", "-mcpu=apple-m1", "-mmacosx-version-min=13.0", "-w")`
  - `@dataclass class BuildStats: objects: int; compiled: int; seconds: float; binary: Path; binary_bytes: int`
  - `build(gen_dir: Path, out: Path, opt: str = "-O2", jobs: int | None = None, host: Path = REPO/"runtime/bw2/linkcheck.c") -> BuildStats`. It refuses `opt` not in `("-O0", "-O1", "-O2", "-O3", "-Os")` with `ValueError`. Each `.c` compiles to `gen_dir/obj/<stem>.o.part`, renamed when clang exits 0 and the file is non-empty. Shards whose `.o` is newer than the `.c` and the flags stamp `obj/flags.txt` are skipped. Linking goes to `out.part`, then rename.
  - `runtime/bw2/linkcheck.c` defines everything the generated code links against. Get the list from the `extern` declarations in `recomp_types.h` and from link errors.
    - It provides `recomp_lookup` (binary search over `recomp_dispatch_table`), `recomp_lookup_manual` and `recomp_lookup_import` (both return NULL), and `recomp_trap`, `recomp_unresolved_icall` and `recomp_unresolved_call`. Those three print the guest address and call `abort()`.
    - Its `main(argc, argv)` prints `linkcheck: <count> functions, entry 0x<entry> -> <found|missing>`, then exits 0 if the entry resolves and 1 if not.
    - It never calls any lifted function.

- [ ] **Step 1: Failing tests** on a tiny hand-written gen dir: two shards with one function each, plus `recomp_funcs.h`, `recomp_dispatch.c` and `recomp_unresolved.c` from `link_tables`.
  - `test_build_links_and_runs`: the binary prints `2 functions` and exits 0.
  - `test_broken_shard_fails_with_name`: a syntax error in shard 1 raises `BuildError` containing its file name, and no binary or `.o.part` is left.
  - `test_rebuild_skips_up_to_date` (second run compiles 0).
  - `test_rebuild_after_flag_change` (changing `opt` recompiles all).
  - `test_bad_opt_rejected` (`""` and `"-O9"`).
  - `test_tools_missing` (monkeypatch `subprocess.run` for `xcode-select -p` to fail; expect `ToolsMissing`).
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `bw2/build.py` (a `concurrent.futures.ThreadPoolExecutor` running `subprocess.run` per shard, never through a shell or a pipe) and `runtime/bw2/linkcheck.c`.
- [ ] **Step 4: Run, expect pass.**
- [ ] **Step 5: Commit** `git commit -m "Build lifted code with parallel clang"`

### Task 10: Full lift and build, measured

**Files:**
- Create: `bw2/pipeline.py`, `docs/research/build-pipeline.md`
- Modify: `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (section 6.3: the measured numbers)

**Interfaces:**
- Consumes: Tasks 1 and 5-9
- Produces: `run_pipeline(gen: Path = REPO/"gen", out: Path = REPO/"out/bw2-linkcheck", opt: str = "-O2") -> dict`. It lifts three modules: `white` (`.text`, the Task 5 catalog), `vm` (`stxt774`+`.name0`, the Task 6 VM catalog) and `slr` (the DLL `.text`, the Task 6 DLL catalog). It then calls `link_tables(..., entry=0xAD8FA3)` and `build`, and returns all stats. CLI: `python -m bw2.pipeline [--opt -O2]`.

- [ ] **Step 1: Pick the optimisation level.** Build the same 20 shards (every 25th `white` shard) at `-O1`, `-O2` and `-O3` and record wall time and object size. Use `-O2` unless `-O3` costs under 1.5x the time. Record the choice and the numbers.
- [ ] **Step 2: Run the full pipeline.** `python -m bw2.pipeline --opt <chosen>`. Expected: `linkcheck: <n> functions, entry 0x00AD8FA3 -> found` and exit 0. If clang fails on a shard, that is a lifter bug: fix it (in pcrecomp, as a patch commit with a difftest case) and rerun. Do not exclude the shard.
- [ ] **Step 3: Write `docs/research/build-pipeline.md`:**
  - per-module function counts, shards, C bytes, trap counts by mnemonic, and lift seconds;
  - unresolved direct targets (count and the first 20);
  - compile wall time, CPU time and peak memory (`/usr/bin/time -l`);
  - object and binary sizes;
  - the optimisation level choice;
  - the `volatile` in pcrecomp's `MEM32` as a known performance item for later (NFSU2 used `restrict` instead).
- [ ] **Step 4: Update spec 6.3** with the measured numbers. Run the full test suite with `BW2_GAMEFILES=1` and expect all pass. Run the audit and expect CLEAN (nothing under `gen/` or `out/` is tracked).
- [ ] **Step 5: Commit** `git commit -m "Lift and build the whole game"`
