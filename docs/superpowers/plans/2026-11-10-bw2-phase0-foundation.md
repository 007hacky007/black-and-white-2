# BW2 Phase 0 (Foundation and Investigations) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the repo's Python tooling on a pinned pcrecomp, get the game's install tree and fan-patch `white.exe` out of the user's discs and patches on macOS, and answer the eight phase 0 questions of the design with a go/no-go report.

**Architecture:** A small Python package `bw2/` wraps pcrecomp's MIT tools (vendored at a pinned commit under `third_party/pcrecomp/`, run as subprocesses from that directory) and adds what they lack: BIN/CUE/zip disc reading, release fingerprints, InstallShield 9 install-tree extraction, fan-patch extraction, PE survey and cross-section analysis. Investigations produce committed Markdown reports under `docs/research/` that contain facts about the game (addresses, sizes, hashes, counts, file paths) and never its content.

**Tech Stack:** Python 3.14 (Homebrew), venv at `.venv`, pytest, capstone, unicorn, pefile; Homebrew `innoextract` (zlib licence) and `unshield` (MIT); macOS `hdiutil` for test fixtures.

**Spec:** `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (sections 3, 4, 6, 9, 10 drive this plan). This plan covers only phase 0. Later plans (lifter, runtime, D3D9/Metal, audio, Builder) are written after the phase 0 summary, because its answers change them.

## Global Constraints

- Never commit game material: discs, patches, any `.exe`, extracted files, shader text, strings or bytes copied from the game, or C generated from the exe. Only facts: addresses, sizes, SHA-256s, counts, file paths, our own code.
- Run `third_party/pcrecomp/tools/audit_repo.py` on the repo before every commit that adds data files; it must report nothing.
- Git identity is already set in the repo: `007hacky007` / `007hacky007@users.noreply.github.com`, `commit.gpgsign=false`. No remote; never push.
- Commit messages: one short imperative subject line, no body, no trailers, no AI attribution.
- Documents use only ASCII hyphen-minus, never Unicode dashes.
- No code from NFSU2Recomp (exception: the user's own `builder/`, not used in this plan).
- pcrecomp is pinned at commit `2adc02fec0702933325801aa7b29a7646321857f` (2026-11-06) with its MIT `LICENSE` kept.
- Real game files live outside git: disc zips in the repo root (gitignored), working output under `gamefiles/` (gitignored). Tests that need them carry `@pytest.mark.gamefiles` and run only when `BW2_GAMEFILES=1`.
- HTTP requests use the User-Agent `bw2-builder/0` and nothing identifying the user.
- **Stop gate (decided by the user):** if Task 8 concludes the SafeDisc leftovers are live protection code, stop all work and report to the user.

## Review Focus

- Disc images given out of order, one missing, or the same disc twice: expect an error naming the disc number that is missing or duplicated, never a partial extraction. (Test in Task 3.)
- Real file names with spaces, `&` and parentheses (`Black & White 2 (USA) (Disc 1).bin`): every tool must work on them. (Task 3 fixtures use such names.)
- Case differences between disc listings and names we look up (`DATA1.HDR` vs `data1.hdr`): lookups are case-insensitive. (Test in Task 3.)
- Interrupted download or extraction: no partially written file may later pass as valid; write to `<name>.part` and rename only after the hash matches. (Test in Task 5.)
- Not enough disk space to unpack the 3 GB of zipped discs plus a 3 GB install tree: fail before writing, naming the bytes needed. (Test in Task 3.)

---

### Task 1: Repo scaffold, vendored pcrecomp, hygiene tests

**Files:**
- Create: `third_party/pcrecomp/` (tree at the pinned commit), `third_party/pcrecomp.VENDORED`
- Create: `requirements.txt`, `pytest.ini`, `README.md`
- Create: `bw2/__init__.py`, `bw2/paths.py`, `bw2/pcr.py`
- Modify: `.gitignore` (add `.venv/`, `__pycache__/`, `*.part`)
- Test: `tests/test_hygiene.py`

**Interfaces:**
- Produces:
  - `bw2.paths.REPO: Path`, `bw2.paths.GAMEFILES: Path` (`REPO/"gamefiles"`, created on demand), `bw2.paths.discs_dir() -> Path` (env `BW2_DISCS`, default `REPO`)
  - `bw2.pcr.PCR: Path` (`REPO/"third_party/pcrecomp"`), `bw2.pcr.run(tool: str, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]`: runs `sys.executable <PCR/tool> *args` with `cwd=PCR`, text output captured.

- [ ] **Step 1: Vendor pcrecomp.** Download `https://codeload.github.com/sp00nznet/pcrecomp/tar.gz/2adc02fec0702933325801aa7b29a7646321857f` into the scratchpad, unpack into `third_party/pcrecomp/`. Write `third_party/pcrecomp.VENDORED` with three lines: `url https://github.com/sp00nznet/pcrecomp`, `commit 2adc02fec0702933325801aa7b29a7646321857f`, `date 2026-11-06`. Check `third_party/pcrecomp/LICENSE` starts with `MIT License`.
- [ ] **Step 2: Create the venv and pin dependencies.** `python3 -m venv .venv && .venv/bin/pip install pytest capstone unicorn pefile`, then write the exact installed versions of those four (`pip freeze | grep -iE '^(pytest|capstone|unicorn|pefile)=='`) to `requirements.txt`. `brew install innoextract unshield`.
- [ ] **Step 3: Write the failing tests** in `tests/test_hygiene.py`:

```python
import subprocess
from bw2 import pcr, paths

def test_game_material_is_ignored():
    for name in ["Black & White 2 (USA) (Disc 1).zip", "x.bin", "x.cue", "x.iso",
                 "white.exe", "gamefiles/install/a.txt", "out/a.c", "gen/a.c", "a.part"]:
        r = subprocess.run(["git", "check-ignore", "-q", name], cwd=paths.REPO)
        assert r.returncode == 0, name

def test_pcrecomp_runs_and_audit_is_clean():
    assert pcr.run("tools/audit_repo.py", "--selftest").returncode == 0
    r = pcr.run("tools/audit_repo.py", str(paths.REPO), check=False)
    assert r.returncode == 0, r.stdout + r.stderr
```

`pytest.ini` registers marker `gamefiles` and sets `testpaths = tests`; `tests/conftest.py` skips `gamefiles` tests unless `BW2_GAMEFILES=1`.
- [ ] **Step 4: Run to see them fail.** `.venv/bin/python -m pytest tests/test_hygiene.py -v` - expected: FAIL (`ModuleNotFoundError: bw2`, `a.part` not ignored).
- [ ] **Step 5: Implement `bw2/paths.py`, `bw2/pcr.py`, `.gitignore` additions.** `README.md`: three short paragraphs (what this is, "bring your own discs", how to run the tests).
- [ ] **Step 6: Run tests.** Same command - expected: 2 passed. If `audit_repo.py` flags anything inside `third_party/pcrecomp`, read why; vendored MIT source is our dependency, not game material, so resolve by fixing the flagged path, not by weakening the test.
- [ ] **Step 7: Commit.** `git add -A && git commit -m "Vendor pcrecomp and add test scaffold"`

### Task 2: Release fingerprints

**Files:**
- Create: `bw2/releases.py`, `bw2/known_releases.tsv`
- Test: `tests/test_releases.py`

**Interfaces:**
- Produces:
  - `@dataclass(frozen=True) class Release: kind: str; name: str; size: int; sha256: str; note: str`
  - `load_releases(tsv: Path = <default bundled TSV>) -> list[Release]` (TSV columns `kind name size sha256 note`, `#` comments)
  - `sha256_file(path: Path) -> str` (lowercase hex, 1 MiB reads)
  - `identify(path: Path, kind: str | None = None, releases: list[Release] | None = None) -> Release | None`: compares size first and only hashes on a size match.

- [ ] **Step 1: Failing tests:** `test_identify_matches_size_and_hash`, `test_identify_rejects_same_size_other_hash`, `test_identify_filters_by_kind`, `test_bundled_tsv_has_fan_patch` (asserts rows `fanpatch-installer` size `20266576` sha `2bb875a10b8e10eaca2fcaee8be0e91bf037cda3af161e2700834e023a237bec` and `white-exe` name `white.exe 1.42` size `20828160` sha `ca745211ac9c63c439493ff14fa27507c4b99d199af4fd2b48577bccfd851db2`). Use temp files and an in-test TSV for the first three.
- [ ] **Step 2: Run, expect FAIL** (`ModuleNotFoundError`).
- [ ] **Step 3: Implement `bw2/releases.py` and `bw2/known_releases.tsv`** with the two fan-patch rows. Disc rows come in Task 3.
- [ ] **Step 4: Run, expect 4 passed.**
- [ ] **Step 5: Commit.** `git commit -m "Add release fingerprints"`

### Task 3: Disc images (zip, BIN/CUE, ISO) and ISO 9660 reading

**Files:**
- Create: `bw2/disc.py`, `bw2/diskspace.py`
- Modify: `bw2/known_releases.tsv` (four disc rows)
- Test: `tests/test_disc.py`, `tests/fixtures.py` (fixture builders)

**Interfaces:**
- Consumes: `bw2.releases.identify`, `bw2.releases.sha256_file`, `bw2.paths.GAMEFILES`
- Produces:
  - `class DiscFormatError(Exception)`, `class DiscSetError(Exception)`, `class NotEnoughSpace(Exception)` (message includes needed and free bytes)
  - `bw2.diskspace.ensure_free(path: Path, needed: int) -> None`
  - `class DiscImage` (context manager): `.sectors: int`, `.read_sector(lba: int) -> bytes` (2048 bytes of user data)
  - `open_disc(path: Path) -> DiscImage`: `.iso` (2048-byte sectors), `.cue` (exactly one `TRACK 01 MODE1/2352`, user data at byte 16 of each 2352-byte sector; anything else raises `DiscFormatError`), `.bin` (looks for a sibling `.cue`; with none, assumes MODE1/2352 if size % 2352 == 0), `.zip` (one `.bin` + one `.cue`: unpacked once to `GAMEFILES/discs/` via `.part`+rename after `ensure_free`, then opened as `.cue`)
  - `@dataclass(frozen=True) class IsoEntry: path: str; lba: int; size: int; is_dir: bool` (path uses `/`, version suffix `;1` and trailing `.` stripped)
  - `list_files(disc: DiscImage) -> list[IsoEntry]` (primary volume descriptor at sector 16; Joliet ignored)
  - `find(entries: list[IsoEntry], path: str) -> IsoEntry | None` (case-insensitive)
  - `extract(disc: DiscImage, entry: IsoEntry, dest: Path) -> None`
  - `identify_discs(paths: list[Path]) -> dict[int, Path]`: uses rows of kind `disc` whose `name` ends in `Disc N`; raises `DiscSetError` naming missing numbers (1-4) and duplicates, and naming any path that matches no known disc.
  - CLI: `python -m bw2.disc list <image>`, `python -m bw2.disc extract <image> <iso-path> <dest>`

Implement ISO 9660 parsing ourselves (about 100 lines). `third_party/pcrecomp/tools/assets/iso_peek.py` is the reference and is MIT if anything is copied.

- [ ] **Step 1: Fixture builders in `tests/fixtures.py`.** `make_iso(tmp: Path, files: dict[str, bytes]) -> Path` builds a directory and runs `hdiutil makehybrid -iso -o <iso> <dir>`. `iso_to_bincue(iso: Path, stem: str) -> Path` writes `<stem>.bin` (each 2048-byte sector framed as 16 header bytes + data + 288 zero bytes) and `<stem>.cue` (`FILE "<stem>.bin" BINARY` / `TRACK 01 MODE1/2352` / `INDEX 01 00:00:00`) and returns the cue. `zip_bincue(cue: Path) -> Path`. Use the stem `Black & White 2 (USA) (Disc 1)` so the real naming is exercised.
- [ ] **Step 2: Failing tests:**
  - `test_iso_lists_and_extracts`: files `{"DATA1.HDR": b"h"*5000, "SUB/DATA1.CAB": b"c"*70000}`; `find(entries, "data1.hdr")` is found, `find(entries, "sub/data1.cab").size == 70000`, and extracted bytes are equal.
  - `test_bincue_and_bin_and_zip_read_same_files`: the same listing and bytes from `.iso`, `.cue`, `.bin` and `.zip`.
  - `test_cue_with_two_tracks_is_rejected` and `test_mode2_is_rejected` (`DiscFormatError`).
  - `test_zip_unpack_checks_space` (monkeypatch `shutil.disk_usage` to report 10 bytes free, expect `NotEnoughSpace` and no file left behind in the output dir).
  - `test_identify_discs_reports_missing_and_duplicates`: an in-test TSV with four fake discs; give discs 1, 2, 2 and expect a `DiscSetError` whose message contains `missing: 3, 4` and `duplicate: 2`.
  - `@pytest.mark.gamefiles test_real_disc1_has_installshield`: `find(list_files(open_disc(<disc 1 zip>)), "data1.hdr")` is not None.
- [ ] **Step 3: Run, expect FAIL.**
- [ ] **Step 4: Implement `bw2/disc.py`, `bw2/diskspace.py`.**
- [ ] **Step 5: Run unit tests (expect pass), then `BW2_GAMEFILES=1 .venv/bin/python -m pytest tests/test_disc.py -v` (expect pass).**
- [ ] **Step 6: Record the four real discs.** Compute the SHA-256 of each unpacked `.bin` and add rows `disc  Black & White 2 (USA) (Disc N)  <size>  <sha>  redump bin` with the sizes from spec 3.1 (788886672, 782552736, 782552736, 652846992). Add `test_real_discs_identify` (gamefiles): `identify_discs` on the four zips' unpacked bins returns `{1,2,3,4}`.
- [ ] **Step 7: Commit.** `git commit -m "Read BIN/CUE, ISO and zipped disc images"`

### Task 4: InstallShield 9 install tree (phase 0 items 1 and 8)

**Files:**
- Create: `bw2/install.py`, `bw2/manifests/install-usa.tsv`, `docs/research/install-tree.md`
- Test: `tests/test_install.py`

**Interfaces:**
- Consumes: `bw2.disc.open_disc`, `list_files`, `find`, `extract`, `identify_discs`; `bw2.pcr.run`
- Produces:
  - `extract_install(discs: dict[int, Path], out: Path) -> Path`: copies every disc's InstallShield files into `GAMEFILES/is/` (one flat directory, the way unshield and isextract expect a multi-volume set), extracts to `out` (default `GAMEFILES/install`) through `.part` directories, returns `out`
  - `write_manifest(root: Path, tsv: Path) -> int` (rows `relpath size sha256`, sorted, `/` separators; returns row count)
  - `read_manifest(tsv: Path) -> dict[str, tuple[int, str]]`

- [ ] **Step 1: Look at the real set.** List every disc with `python -m bw2.disc list`, and note the InstallShield files on each (`data1.hdr`, `dataN.cab`, loose files). Write the per-disc file list (names and sizes only) into `docs/research/install-tree.md`.
- [ ] **Step 2: Try both extractors on the gathered set.** Run `pcr.run("tools/assets/isextract.py", "<is>/data1.hdr", "<is>/data1.cab", "--list")` and `unshield -d <is> l <is>/data1.hdr` (and `unshield -V` for its version). Pick the extractor that (a) lists file index 893 `white.exe` at 21,739,061 bytes, (b) extracts every listed file, and (c) reports no checksum or MD5 failure. If both pass, prefer `isextract.py` (pure Python, already vendored). Record the choice and the evidence in the report.
- [ ] **Step 3: Failing tests:**
  - `test_manifest_roundtrip` (temp tree with a nested file and a name with `&` and spaces).
  - `@pytest.mark.gamefiles test_install_tree_complete`: after `extract_install`, every row of `bw2/manifests/install-usa.tsv` exists with matching size and hash, and the tree contains a `white.exe` of 21,739,061 bytes.
- [ ] **Step 4: Run, expect FAIL.**
- [ ] **Step 5: Implement `bw2/install.py`** with the chosen extractor. InstallShield groups map to install sub-directories; reproduce the directory layout the Windows installer creates, as far as the header's group/directory tables say, and list any file whose destination cannot be determined in the report.
- [ ] **Step 6: Generate and commit the manifest.** Run `extract_install`, then `write_manifest`. Run the tests and expect them to pass. Finish the report: file count, total bytes, top-level layout, extractor used, unresolved destinations.
- [ ] **Step 7: Audit and commit.** Run `pcr.run("tools/audit_repo.py", str(REPO))` and expect a clean result, then `git commit -m "Extract the install tree from the discs"`.

### Task 5: Downloads and fan patch extraction

**Files:**
- Create: `bw2/download.py`, `bw2/fanpatch.py`
- Test: `tests/test_download.py`, `tests/test_fanpatch.py`

**Interfaces:**
- Consumes: `bw2.releases.identify`, `Release`
- Produces:
  - `class FingerprintMismatch(Exception)`
  - `fetch(url: str, dest: Path, expected: Release) -> Path`: streams to `dest.with_suffix(dest.suffix + ".part")` with User-Agent `bw2-builder/0`, checks size and SHA-256, renames on match, deletes `.part` and raises `FingerprintMismatch` otherwise; if `dest` already exists and matches, returns it without fetching
  - `FAN_PATCH_URL = "https://www.bwgame.net/downloads/black-white-2-unofficial-patch-v1-42.1421/download"`
  - `extract_fan_exe(installer: Path, out_dir: Path) -> Path`: checks the installer fingerprint (`fanpatch-installer`), runs `innoextract -s -d <tmp> <installer>`, takes `code$GetDestDir/white.exe`, checks its fingerprint (`white-exe`), moves it to `out_dir/white.exe`, returns that path

- [ ] **Step 1: Failing tests:**
  - `test_fetch_ok`, `test_fetch_bad_hash_leaves_nothing` (no `dest`, no `.part` afterwards), `test_fetch_reuses_existing`, using a local `http.server` thread serving a temp file.
  - `test_fetch_sends_generic_user_agent` (the handler records the header, which must equal `bw2-builder/0`).
  - `@pytest.mark.gamefiles test_fan_exe_extracted`: `extract_fan_exe(fetch(FAN_PATCH_URL, GAMEFILES/"patches"/"B&W2 Fan Patch v1.42 Installer.exe", <release>), GAMEFILES/"exe")` returns a file of 20,828,160 bytes.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement `bw2/download.py`, `bw2/fanpatch.py`.**
- [ ] **Step 4: Run all tests, including gamefiles, and expect them to pass.**
- [ ] **Step 5: Commit.** `git commit -m "Download and unpack the fan patch"`

### Task 6: Official patches 1.1 and 1.2 (phase 0 item 2)

This is an investigation. The deliverable is a report plus fingerprints, not an applier; the applier belongs to the Builder plan.

**Files:**
- Create: `docs/research/official-patches.md`
- Modify: `bw2/known_releases.tsv` (rows `patch-1.1`, `patch-1.2`)

**Interfaces:**
- Consumes: `bw2.download.fetch`, `bw2.releases.sha256_file`

- [ ] **Step 1: Find direct, stable download URLs.** Candidates: PCGamingWiki files 2151 (v1.1) and 2152 (v1.2), GamePressure, moddb. Prefer a URL that needs no login and no JavaScript. If none works without a browser, say so in the report and record that the Builder must take a local copy.
- [ ] **Step 2: Download both** to `gamefiles/patches/` (first with no fingerprint check, via a plain urllib call using the same User-Agent). Record size and SHA-256 in `known_releases.tsv`. Expected: 1.1 about 106.5 MB, 1.2 about 31.8 MB.
- [ ] **Step 3: Identify each format without running it.** Use `file`, `strings -n 8 | grep -iE "installshield|inno|nullsoft|rtpatch|wise|7-zip|cab"`, `7zz l`, `innoextract -l`, and `unshield l` on any embedded cab. Unpack each with the first tool that works.
- [ ] **Step 4: Work out what each patch changes.** Compare the unpacked file list against `bw2/manifests/install-usa.tsv`, and say for each file whether it is new, a full replacement, or a binary diff (and in which diff format). State explicitly whether `white.exe` is touched, and whether the data changes can be applied on macOS with an existing open tool or need our own applier.
- [ ] **Step 5: Write the report** with the sections: Sources, Fingerprints, Format, Unpacking on macOS, Files changed (table: path, action, size), Applying without Windows, Open questions.
- [ ] **Step 6: Audit, then commit.** `git commit -m "Research the official 1.1 and 1.2 patches"`

### Task 7: PE survey and exception check (phase 0 items 5 and 6, header half)

**Files:**
- Create: `bw2/pe_survey.py`, `docs/research/white-exe-survey.md`
- Test: `tests/test_pe_survey.py`, extend `tests/fixtures.py`

**Interfaces:**
- Consumes: pefile, capstone
- Produces:
  - `@dataclass class Section: name: str; va: int; vsize: int; raw_size: int; flags: int; entropy: float`
  - `@dataclass class Survey: image_base: int; entry: int; sections: list[Section]; imports: dict[str, list[str]]; tls_callbacks: list[int]; reloc_entries: int; reloc_pages: list[int]; rtti_type_descriptors: int; seh_prologues: int; raise_exception_imported: bool`
  - `survey(exe: Path) -> Survey`
  - `iat_slot(exe: Path, dll: str, func: str) -> int | None` (VA of the IAT slot, dll name case-insensitive)
  - `iat_callsites(exe: Path, dll: str, func: str) -> list[int]` (VAs of `call dword ptr [slot]` and `jmp dword ptr [slot]` in executable sections, found by byte pattern `FF 15 <slot>` / `FF 25 <slot>`)
  - CLI: `python -m bw2.pe_survey <exe> [--json out.json]`

`seh_prologues` counts the MSVC SEH frame setup `64 A1 00 00 00 00` (`mov eax, fs:[0]`) in executable sections. `rtti_type_descriptors` counts `.?AV` and `.?AU` strings in data sections.

- [ ] **Step 1: Fixture `make_pe(sections: list[tuple[str, bytes, int]], imports: dict[str, list[str]], entry_rva: int) -> bytes`** in `tests/fixtures.py`: a minimal valid PE32 (DOS stub, `PE\0\0`, optional header, section table, import directory with IAT), file alignment 0x200, section alignment 0x1000, image base 0x400000.
- [ ] **Step 2: Failing tests:**
  - `test_survey_synthetic`: two sections `.text` and `stxt774`, one import `d3d9.dll!Direct3DCreate9`, entry at `.text`. Assert the section names, `entry == 0x401000`, `imports == {"d3d9.dll": ["Direct3DCreate9"]}`, `reloc_entries == 0`.
  - `test_iat_callsites_synthetic`: `.text` holds `FF 15 <slot>` twice and `FF 25 <slot>` once, and `iat_callsites` returns three VAs in order.
  - `test_seh_and_rtti_counts`.
  - `@pytest.mark.gamefiles test_real_white_exe_matches_spec`: `entry == 0xad8fa3`, `.text` vsize `8454603`, 9 sections, `reloc_entries == 260`, `reloc_pages == [0x202a000]` as RVA, `d3dx9_25.dll` has 60 imports including `D3DXCompileShader`.
- [ ] **Step 3: Run, expect FAIL.**
- [ ] **Step 4: Implement `bw2/pe_survey.py`.**
- [ ] **Step 5: Run, expect pass (unit and gamefiles).**
- [ ] **Step 6: Write `docs/research/white-exe-survey.md`:** the survey as tables; TLS callbacks; RTTI count; SEH prologue count; `RaiseException` import; and a verdict paragraph for item 6: do C++ exceptions or SEH show up on normal paths, judged by `_CxxThrowException`/`__CxxFrameHandler` signatures (search for the MSVC 7.1 `__CxxFrameHandler` stub `B8 xx xx xx xx E9`, i.e. `mov eax, FuncInfo; jmp handler`, and count the distinct handlers) and by where `RaiseException` call sites sit (`iat_callsites`). Report the counts and the call-site VAs; say "needs guest SEH early" or "does not" and why.
- [ ] **Step 7: Commit.** `git commit -m "Survey white.exe headers and exception use"`

### Task 8: SafeDisc leftovers (phase 0 item 3, the stop gate)

**Files:**
- Create: `bw2/xsect.py`, `docs/research/safedisc-leftovers.md`
- Test: `tests/test_xsect.py`

**Interfaces:**
- Consumes: `bw2.pe_survey.survey`, capstone; `make_pe` fixture
- Produces:
  - `@dataclass(frozen=True) class Ref: site: int; kind: str; target: int` (`kind` in `"call"`, `"jmp"`, `"ptr"`)
  - `cross_section_refs(exe: Path, from_section: str, to_sections: list[str]) -> list[Ref]`: E8/E9 rel32 targets plus aligned 4-byte values landing in `to_sections`, sorted by site
  - `disasm_at(exe: Path, va: int, max_insns: int = 40) -> list[str]` (`"0xADDR: mnemonic op_str"`, stops after `ret` or `jmp`)

- [ ] **Step 1: Failing tests:** `test_cross_section_call_found` (synthetic PE where `.text` has `E8` into `stxt774` and a 4-byte pointer into it, expecting one `call` and one `ptr` Ref) and `test_disasm_stops_at_ret`. Then `@pytest.mark.gamefiles test_real_counts`: 47 call/jmp refs with 19 distinct targets from `.text` into `stxt774`/`.name0`/`.name1`.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement `bw2/xsect.py`.**
- [ ] **Step 4: Run, expect pass.**
- [ ] **Step 5: Investigate.** For each of the 19 targets: disassemble it, follow where it goes (an IAT slot? a decryption loop? back into `.text`?), and note which game functions contain the call sites. Also find what references `.name0` (the only relocations are for it), and check `third_party/pcrecomp/tools/drm/` and its docs on SafeDisc for known stub shapes. Typical SafeDisc 2+ shapes to check against: import-redirection stubs that resolve an API and jump to it, and "nanomite"/decryption stubs that rewrite `.text`. Check whether `.text` bytes at the call sites differ from what a plain MSVC call would be.
- [ ] **Step 6: Write the report** ending with exactly one verdict line: `VERDICT: DEAD` (never reached, or reached but harmless), `VERDICT: PATCH-CODE` (the fan patch's own code caves; must be lifted like game code), or `VERDICT: LIVE` (protection logic that runs and matters). Give the evidence per target in a table: target, shape, call-site count, verdict.
- [ ] **Step 7: Commit.** `git commit -m "Analyse the SafeDisc leftover sections"`
- [ ] **Step 8: Gate.** If the verdict is `LIVE`, stop here and report to the user. Run no further tasks.

### Task 9: Function discovery without relocations (phase 0 items 4 and 5)

**Files:**
- Create: `bw2/discover.py`, `bw2/catalog/white-1.42.functions.json` (addresses and extents only), `docs/research/function-discovery.md`
- Test: `tests/test_discover.py`

**Interfaces:**
- Consumes: `bw2.pcr.run`, `bw2.pe_survey.survey`, Task 8's verdict (if `PATCH-CODE`, `stxt774` targets are added as seeds)
- Produces:
  - `discover(exe: Path, out: Path, extra_seeds: list[int] = []) -> Path`: runs `tools/cpp/rtti.py <exe> --seeds <s1>`, `tools/cpp/vtable_scan.py <exe> --seeds <s2>`, merges both with `extra_seeds` into one seeds file in the format `disasm32.py --seed-functions` reads (check its loader), then runs `tools/disasm/disasm32.py <exe> --seed-functions <seeds> -o <out>`
  - `@dataclass class Coverage: text_bytes: int; covered_bytes: int; functions: int; seeds_rtti: int; seeds_vtable: int`
  - `coverage(catalog: Path, exe: Path) -> Coverage` (union of function extents clipped to `.text`)
  - `opcode_mix(catalog: Path, exe: Path) -> dict[str, int]` (counts by capstone group: `x87`, `sse`, `sse2`, `mmx`, `3dnow`, `other`, plus the 20 most common mnemonics)

- [ ] **Step 1: Failing tests:** `test_coverage_unions_overlaps` (a hand-written catalog JSON in disasm32's output shape, built by reading `disasm32.py`'s `_func_to_dict`, with two overlapping functions) and `test_opcode_mix_counts_x87` (synthetic PE with `D9 E8` (`fld1`) and `0F 58 C1` (`addps`)).
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement `bw2/discover.py`.**
- [ ] **Step 4: Run, expect pass.**
- [ ] **Step 5: Run on the real exe.** `discover` on `gamefiles/exe/white.exe`, writing to `bw2/catalog/white-1.42.functions.json`. Record wall time. Check that the JSON holds only addresses, sizes and names we or pcrecomp generated (no instruction bytes); if disasm32 includes bytes or disassembly text, strip them in `discover` before writing.
- [ ] **Step 6: Measure what the lifter lacks.** Run pcrecomp's 32-bit lifter over the catalog into `gen/` (gitignored), using `third_party/pcrecomp/tools/lift/translator.py` the way `docs/PIPELINE.md` describes, and count unsupported-instruction traps by mnemonic.
- [ ] **Step 7: Write the report:** coverage percentage of `.text`, function count, RTTI classes and vtables found, the opcode mix, unsupported mnemonics with counts, the lift wall time, and the largest uncovered gaps (VA ranges). Apply this gate, which is my call: covered at least 90% means GO; 80-90% means GO with the gaps listed as the first work of the lifter plan; under 80% means stop and report to the user.
- [ ] **Step 8: Audit, then commit.** `git commit -m "Discover white.exe functions without relocations"`

### Task 10: D3DX and HLSL use (phase 0 item 7)

**Files:**
- Create: `docs/research/d3dx-usage.md`

**Interfaces:**
- Consumes: `bw2.pe_survey.iat_callsites`, `bw2.manifests/install-usa.tsv`, the extracted install tree, `bw2.discover` catalog (to name the function around each call site)

- [ ] **Step 1: Call sites.** For each of the 60 `d3dx9_25.dll` imports, count its call sites with `iat_callsites`, and for `D3DXCompileShader`, `D3DXCompileShaderFromFileA`, `D3DXAssembleShader` list each site's containing function.
- [ ] **Step 2: Where shader source comes from.** Search the install tree (paths and file types only) for likely shader sources: extensions `.fx .hlsl .psh .vsh .sha` and files containing `technique`, `ps_2_0`, `vs_2_0`, `float4`. Also search the exe's `.rdata` for HLSL-looking strings (count only). Inside STUFF archives, use openblack's `bw2-unstuff` (MIT) only if the loose files explain nothing.
- [ ] **Step 3: Write the report:** a table of imports with call counts; the shader-compile call sites; shader source locations (paths), count and target profiles (`vs_1_1` / `vs_2_0` / `ps_2_0` and so on); whether compilation happens at load or per frame (from where the call sites sit); and a recommendation between "native HLSL compiler (vkd3d-shader or other)" and "capture and translate on the user's Mac", with the licence of the proposed library. No shader text in the report.
- [ ] **Step 4: Commit.** `git commit -m "Research D3DX and HLSL use"`

### Task 11: Phase 0 summary

**Files:**
- Create: `docs/research/phase0-summary.md`
- Modify: `docs/superpowers/specs/2026-11-10-bw2-macos-port-design.md` (section 10: mark each item answered, with a link to its report; correct any design statement a finding contradicts)

- [ ] **Step 1: Write the summary:** one paragraph per phase 0 item (1-8) with the answer, a link to its report and GO / GO-WITH-RISK / STOP. Then the proposed order of the next plans (lifter, Win32 host on pcrecomp, D3D9/Metal, D3DX, audio, input/shell, Builder) with any item findings that change them.
- [ ] **Step 2: Update the spec section 10** and any section a finding contradicts (for example, the extractor choice in 5.1, the patch formats in 3.2 and 8).
- [ ] **Step 3: Run the full test suite** (`BW2_GAMEFILES=1 .venv/bin/python -m pytest -v`), expect all pass; run `audit_repo.py`, expect clean.
- [ ] **Step 4: Commit.** `git commit -m "Summarise phase 0"`
- [ ] **Step 5: Hand the summary to the user** for the go/no-go decision and the next plan.
