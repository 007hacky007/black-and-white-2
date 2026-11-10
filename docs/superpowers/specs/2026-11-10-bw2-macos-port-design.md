# Black & White 2 on Apple Silicon - design

Date: 2026-11-10
Status: draft for review

## 1. Intent

Run Black & White 2 (Lionhead / EA, 2005), the original game as it shipped,
natively on Apple Silicon Macs. No Wine, no emulator, no virtual machine, no
Rosetta. The method is the one NFSU2Recomp proved: lift the game's 32-bit x86
executable to C ahead of time, compile it for ARM64 with clang, and answer
every Windows, Direct3D 9, D3DX, DirectInput, OpenAL and Bink call with our
own Mac code.

The user makes the app on their own Mac from their own discs with a small
Builder app. The repo never contains game code, game assets, the patches, or
C generated from the game.

Why: the 2009 Mac version is 32-bit and dead since Catalina, Rosetta 2 is
rated unplayable, and the remaining options (CrossOver, Parallels) are not
native. The user did the same for NFSU2 and wants BW2 next.

## 2. Goals, non-goals, anti-goals

### Goals

- **Done means:** the user can play the whole campaign start to finish, change
  settings, and save and load, all behaving as on Windows.
- Windowed mode and full screen, both available, with a way to switch.
- Mouse with a wheel works as on Windows. The trackpad works too (section 7.4).
- The Builder takes the user's own disc images and the patches and produces
  `Black & White 2.app` with no Terminal use.

### Non-goals (for now)

- Battle of the Gods (the expansion).
- Releases other than the USA four-disc set. Other languages.
- Multiplayer or anything online.
- Enhancements of any kind beyond the shipped game: no widescreen hacks, no
  higher frame rates, no mods, no fork-style extras. (The fan patch's own
  fixes come along because its exe is the one we lift; see 4.2.)
- Publishing. The project is for the user now. It must stay publishable
  later (section 9), but nothing is pushed or released.

### Anti-goals (failure even if it technically runs)

- Game files, patch files, or generated C committed to the repo, in any
  commit.
- NFSU2Recomp code copied in. NFSU2Recomp is proprietary ("All rights
  reserved", Talha Muderrisoglu). It is a reference for ideas and mistakes
  only. Reading it is fine; pasting from it is not. **One exception, decided
  by the user:** the NFSU2 Builder (`NFSU2Recomp/builder/`, every commit
  authored by 007hacky007) is the user's own code and may be copied and
  adapted. Nothing else from that repo, including files with mixed
  authorship such as `runtime/metal.m` and `runtime/com.c`.
- The 007hacky007 identity linked to the user's real identity anywhere.
- Silent wrongness: a lift that "runs" but quietly corrupts state (wrong
  flags, wrong x87 results, unresolved calls returning 0). Every unknown must
  fail loudly with an address.

## 3. Inputs

### 3.1 Disc images (user supplies)

Redump-style images of the USA release, four discs. The user's copies:

| File (inside the zip) | Size (bytes) |
|---|---|
| `Black & White 2 (USA) (Disc 1).bin` | 788,886,672 |
| `Black & White 2 (USA) (Disc 2).bin` | 782,552,736 |
| `Black & White 2 (USA) (Disc 3).bin` | 782,552,736 |
| `Black & White 2 (USA) (Disc 4).bin` | 652,846,992 |

Each zip holds one `.bin` and one `.cue`. Known from sp00nznet/bw2's
reconnaissance (not yet verified by us): each disc carries
`data1.hdr` + `data1.cab` + `data2.cab`, an **InstallShield 9** multi-volume
set spanning all four discs. The retail `white.exe` is file index 893
(21,739,061 bytes) and is not on disc 1. Disc 1 also carries SafeDisc markers
(`DrvMgt.dll`, `00000001.TMP`, `00000002.TMP`). We do not need the retail exe
(see 4.2), but we need every data file the installer would lay down.

The Builder accepts: the zips as-is, `.bin`/`.cue`, or `.iso`. It records a
SHA-256 for each disc in a known-releases table so wrong or damaged images get
a clear red row.

### 3.2 Patches (Builder downloads, or user supplies a local copy)

Applied in this order. We never ship them.

| Patch | Source | What we know |
|---|---|---|
| Official 1.1 | fan mirrors (PCGamingWiki files, GamePressure) | ~106.5 MB, `Black & White(R) 2 Patch 1-1.exe`. Format not yet identified. Requires full install. |
| Official 1.2 | fan mirrors (PCGamingWiki files, GamePressure) | ~31.8 MB. Requires 1.1. Format not yet identified. |
| Unofficial (fan) patch 1.42 | https://www.bwgame.net/downloads/black-white-2-unofficial-patch-v1-42.1421/ (direct `/download`, no login) | `B&W2 Fan Patch v1.42 Installer.exe`, 20,266,576 bytes, SHA-256 `2bb875a10b8e10eaca2fcaee8be0e91bf037cda3af161e2700834e023a237bec`. Inno Setup 6.0.0. Contains only `white.exe` and a readme. Page says 1.1 and 1.2 must be installed first. |

The fan patch's `white.exe`: 20,828,160 bytes (19.9 MiB), SHA-256
`ca745211ac9c63c439493ff14fa27507c4b99d199af4fd2b48577bccfd851db2`.

Every download is pinned by SHA-256. A mismatch is a red row, never a
warning.

## 4. The target binary

### 4.1 What we measured (fan patch 1.42 `white.exe`)

- PE32, i386, image base `0x400000`, entry `0xad8fa3`.
- `.text` 8,454,603 bytes (about 2.3x NFSU2's `SPEED2.EXE`). Plain code,
  entropy 6.49, not encrypted.
- `.rdata` 1.4 MB, `.data` 23.7 MB virtual, `_PDATA` 115 KB, `.tls`, `.rsrc`.
- **Three extra sections:** `stxt774` (5 KB, executable, entropy 7.54),
  `.name0` (158 KB, executable, entropy 7.96, i.e. packed or encrypted),
  `.name1` (holds the only relocation block: 260 entries, all for the
  `.name0` page). `stxt774` is a SafeDisc section name. 47 call/jmp sites in
  `.text` (19 distinct targets) go into `stxt774`; 488 4-byte values in
  `.text` point into the extra sections.
- **No relocation table for the game code.** NFSU2's whole function and
  pointer recovery rested on its intact relocation data; that crutch is gone.
- Imports (all readable, by name): KERNEL32, USER32, GDI32, ADVAPI32
  (registry), SHELL32, SHLWAPI, ole32, COMCTL32, comdlg32, IMM32, WINMM
  (incl. waveIn), WS2_32 (by ordinal), NETAPI32 (Netbios), dbghelp (crash
  dumps), DDRAW (enumeration only), **d3d9** (`Direct3DCreate9`),
  **d3dx9_25** (60 functions incl. `D3DXCompileShader`,
  `D3DXAssembleShader`, texture load/save, math), **DINPUT8**,
  **binkw32** (6 functions).
- Loaded at run time (strings): `OpenAL32.dll`, `wrap_oal.dll`, EAX 3.0
  (`EAXGet`/`EAXSet`), `dsound.dll`, `ksuser.dll`, `PSApi.dll`, `mscoree.dll`.
- Readme of the fan patch lists its changes: launches on Windows 7+, no
  graphics cap on modern GPUs, crash fixes, fewer logos, piracy punishment
  (non-aging villagers) disabled, Tiger creature unlocked, two extra creature
  types, crash minidumps.

### 4.2 Why this exe

The retail exe is SafeDisc-wrapped; NFSU2 refused SafeDisc builds because
there is nothing to lift. The fan patch exe is unwrapped and is what the
community plays. Its fixes (Windows 7+ launch, no graphics cap) are
compatibility fixes, so lifting it is still "the game as shipped" for the
user's purposes. Decided by the user.

## 5. Architecture

```
discs + patches --(Builder)--> game data tree + white.exe
white.exe --(analysis, function discovery)--> function catalog
catalog --(lifter)--> generated C (sharded)  [never committed]
generated C + runtime --(clang -O3, arm64)--> engine binary
engine + game data --(Builder)--> Black & White 2.app (ad-hoc signed)
```

### 5.1 Built on pcrecomp (MIT)

Decided by the user: build on sp00nznet/pcrecomp's MIT pieces where they
fit; write the rest ourselves. pcrecomp is MIT (Copyright (c) 2026 sp00nz),
active, and has a 32-bit PE path and a Win32 host (`runtime/win32hle`) whose
README says it runs on Apple Silicon Macs in "based" memory mode
(`RECOMP_MEM_BASE=0x100000000000ull`), on SDL2.

What we expect to take from it (each to be confirmed by trying it):

| Need | pcrecomp piece |
|---|---|
| Read BIN/CUE, ISO | `tools/assets/bin2iso.js`, `iso_peek.py` |
| InstallShield 9 multi-volume | **unshield** (MIT, Homebrew) instead: `isextract.py` fails on files spanning cabinets (phase 0) |
| PE analysis, imports, resources | `tools/pe/*` |
| Function discovery | `tools/disasm/disasm32.py`, `cpp/rtti.py`, `cpp/vtable_scan.py`, `lift/recover`, scoring tools |
| SafeDisc leftovers | `tools/drm/*` (for understanding `stxt774`/`.name0`) |
| Lifter | `tools/lift/lift32.py` or `lift32_cpu.py` |
| Lift correctness | `tools/lift/difftest.py` (lifted C vs Unicorn, field by field) |
| Win32 host | `runtime/win32hle` (PE loader, kernel32, user32, gdi, registry, files with case-insensitive paths, threads under a machine lock, winmm, wsock, ole32 COM, Bink on ffmpeg) |
| Repo hygiene | `tools/audit_repo.py` |

What pcrecomp does not have, so we write it:

- Direct3D 9 on Metal (pcrecomp has only DirectX 6 Direct3D, in software).
- D3DX 9 (`d3dx9_25`), including an HLSL compiler for `D3DXCompileShader`.
- OpenAL + EAX on macOS.
- DirectInput 8 on macOS (keyboard, mouse; gamepad not needed).
- The Mac shell: Metal window, AppKit event loop, full screen/windowed,
  Retina, trackpad mapping, save locations.
- The Builder app.
- Anything the BW2 lift needs that pcrecomp's lifter lacks (likely: SSE
  coverage, performance work such as O(1) dispatch, guest SEH; see 6).

How it is vendored (my call): pcrecomp is pinned to a specific commit under
`third_party/pcrecomp/` with its LICENSE kept. Our changes to it are kept as
small, separate commits so they could be offered upstream later. Where SDL2 is
only a convenience in win32hle, the Mac-specific layers talk to AppKit and
Metal directly; whether SDL2 stays for anything is the builder's call.

### 5.2 Runtime layers (ours)

- **Memory:** one 4 GB guest mapping, based addressing. NFSU2 lesson: mapping
  the guest at its real address on macOS ARM64 gets the process SIGKILLed;
  based addressing costs nothing (one `uxtw` load).
- **D3D9 -> Metal:** device, swap chain, render states, textures (incl. cube,
  DXT), vertex/index buffers, fixed function, and VS/PS bytecode translated to
  MSL on the GPU. Shader cache on disk, compiled asynchronously.
- **D3DX 9:** the 60 imported functions, natively. **Phase 0:** most rendering goes through
  `ID3DXEffect` on 59 precompiled fx_2_0 effects (`Data/effects/*.sdv`), so the
  effect framework is the largest D3DX item; HLSL is needed only for 7 `.fx`
  sources, and 29 shaders are text assembly (`docs/research/d3dx-usage.md`). `D3DXCompileShader` and
  `D3DXAssembleShader` produce D3D9 bytecode the rest of the D3D9 layer
  already understands. The HLSL compiler is a library choice left to the
  builder (vkd3d-shader is the obvious candidate: it compiles SM1-3 HLSL;
  check its license fits section 9). Alternative if that fails: capture each
  HLSL source the game compiles and translate it ahead of time, keyed by hash.
- **Audio:** the game loads OpenAL through `wrap_oal.dll`. We answer the
  OpenAL + EAX API ourselves on top of an open OpenAL implementation or
  CoreAudio (builder's call). EAX effects can be approximated or dropped in a
  first pass but must not crash.
- **Input:** DirectInput 8 keyboard and mouse from AppKit events, plus the
  Win32 mouse messages the game reads.
- **Video:** Bink through pcrecomp's ffmpeg-backed `bink.c`, adapted to D3D9
  surfaces (`_BinkDX9SurfaceType@4`, `_BinkCopyToBuffer@28`).
- **Files and saves:** the game sees a Windows tree; writes are redirected to
  `~/Library/Application Support/Black & White 2/` because writing inside the
  signed bundle breaks the signature. The registry is persisted there too.

## 6. Hard problems and how we approach them

Ordered by risk. The first phase of work is to retire 6.1 and 6.2 before any
runtime is built.

### 6.1 No relocation table

The lifter must tell pointers from numbers without relocation data. Approach:
recursive descent from the entry point, TLS callbacks and exports; vtables
from `.rdata` (every vtable slot is a function entry, including MSVC adjustor
thunks; pcrecomp's lesson); RTTI if present; jump tables; then a
data-pointer sweep. sp00nznet/bw (the first game's recompilation, 569 types
recovered) gives class-name hypotheses to test against this engine, four
years on. sp00nznet/bw2 itself is reconnaissance only (nothing lifted, nothing builds,
last change 2026-09-23; its README states MIT): its notes feed phase 0 but
there is no code to build on. The runtime treats any call to an address not in the lifted table
as a loud failure naming the address and the target bytes, never as a silent
return of 0. A trace-and-relift loop (pcrecomp's `seed_from_log`) closes the
gap from real play sessions.

### 6.2 SafeDisc leftovers

Find out what `stxt774` and `.name0` do and whether the 47 call sites are
live on the paths we play. Possible answers: harmless stubs, the fan patch's
own code caves, or live protection. If they are dead or the patch's own code,
lift or trap them and document which. **If they turn out to be live
protection code, stop and report the findings to the user before doing
anything else** (decided by the user). This decides whether the fan exe is
liftable at all, so it comes first.

**Phase 0 answer** (`docs/research/safedisc-leftovers.md`): LIVE. `.name0`
holds a code-virtualization VM (about 2.2 KB of interpreter, 191 handlers)
running 18 game functions from encrypted bytecode, called from 46 sites.
**Decided by the user: run the VM as shipped**, i.e. lift its interpreter
like game code and let it interpret its bytecode; nothing is decrypted or
removed. A trial lift of the interpreter needs only `pushf`. If the
virtualized functions turn out to ask for disc or driver checks, stop and ask
the user again before answering them.

### 6.3 Size

8.4 MB of code is 2.3x NFSU2. Budget for build time (NFSU2: about 2 minutes
for lift + compile on an M1 Pro), shard the generated C, and keep generation
byte-reproducible so rebuilds and diffs are cheap. **Phase 0:** a trial lift
of all 49,323 function starts produces 458 MB of C (NFSU2: 137 MB); compile
time on the user's Mac is the first thing the lifter plan measures. The game
also loads a second module, `PlugIns/ScriptLibraryR.dll` (32-bit, replaced by
patch 1.2), which must be lifted too. **Plan 1, measured:** after the
catalog cleanup (`bw2/catclean.py`), the lift is 47,179 `white` functions
(253 MB of C), 228 VM functions (19 MB) and 1,876 DLL functions (15 MB). The
link set has 49,283 functions and 5 wild unresolved targets. The lift takes
about 88 s; the -O3 build takes 79 s of wall time (506 s CPU) on an M1 Pro,
with a 1.60 GB peak, 181 MB of objects and a 92 MB link-check binary. The one
152k-line function (`0xB84D70`) is compiled at -O0 in its own cold shard. See
`docs/research/build-pipeline.md`.

### 6.4 Runtime HLSL compile

New work NFSU2 never needed (it linked D3DX statically and lifted it).
See 5.2.

### 6.5 C++ exceptions / SEH

Neither NFSU2 nor pcrecomp's win32hle implements guest SEH; both stop on
`RaiseException`. BW2 is C++-heavy (MSVC 7.1). Measure early whether it uses
exceptions on normal paths; if it does, implement guest SEH and
`__CxxFrameHandler` early, not after weeks of play-testing. **Phase 0:** 271
`_CxxThrowException` call sites and 1,892 functions with unwind tables, 7
`__try` blocks: guest C++ EH is required. pcrecomp has a 32-bit scheme for it
(`tools/lift/eh32.py` with `runtime/native32/eh32.c`) to port onto the
win32hle host.

## 7. Lessons carried in from NFSU2

These are traps NFSU2 hit; the design takes them as requirements.

### 7.1 Lifter and CPU

- Compile generated C with `-fno-strict-aliasing` from day one (GVN
  miscompiled mixed-type guest accesses). Pass the memory base as a separate
  `restrict` value.
- Lazy flags are worth it, but Capstone's reported flag effects lie in known
  places (x87 reuses eflags; shifts by 0; `rep cmps/scas` with ECX=0;
  `pushfd`/`lahf`/`rcl` reads). Build a flag audit before first boot.
- Capstone swaps `fxch` operand order and misnames `fcomip`; check x87
  decoding against a hand-written ISA suite.
- x87 as doubles, plus emulated precision control: D3D9 `CreateDevice`
  without `FPU_PRESERVE` sets 24-bit precision, and the game's math expects
  it.
- `pushfd`/`popfd` must keep the ID bit (21) or the CPUID check fails and the
  game saves minimum graphics settings permanently. RDTSC must track real
  time or CPU-speed calibration breaks.
- `push` reads its operand before moving `esp`; `call [esp+x]` reads its
  target before pushing.
- Entry block first; a single `int3` is not a block end.
- Indirect dispatch must be O(1) (a table over `.text`), not a binary search
  or string lookup.
- stdcall argument counts for imports come from an authoritative table, not
  inference from push sequences (NFSU2's inference was only 74-77% right).
- Test with a hand-encoded ISA suite and a differential fuzzer (pcrecomp's
  `difftest.py` vs Unicorn; NFSU2 also used Rosetta 2 as an oracle).

### 7.2 Runtime

- Guest threads are real threads under one lock; release the lock fairly,
  not only in blocking calls. TLS through the TEB (`fs:[0x2c]`),
  per-thread thread IDs, `CREATE_SUSPENDED` honoured, `ExitThread` exits,
  `ExitProcess` from any thread works.
- Large host stacks for guest call chains (NFSU2: 1 GB main, 256 MB workers,
  plus `sigaltstack`).
- A real heap with a quarantine for freed blocks (the game touched memory it
  had just freed; a bump allocator ran out after 8 minutes).
- No fixed-size tables for D3D objects or handles ("time bombs").

### 7.3 Direct3D 9

- Translate shader bytecode to MSL on the GPU; keep a CPU path only as a
  debugging fallback. Honour dst shift modifiers, fog output, sampler types
  from DCL tokens, 2D/cube variants for `ps_1_x`.
- Match D3D9 semantics: render targets persist across frames; half-pixel
  offset; each depth surface its own object; `Reset` restores defaults; new
  textures are zeroed; never sample from the drawable.
- Skip redundant state sets; dispatch COM by method index; compile shaders
  asynchronously with a warm disk cache (a cold cache cost NFSU2 8 s of black
  screen).
- NFSU2 found that a Windows reference saves weeks: many of its "bugs" were
  measurement artifacts, and real ones went unseen without one. **There is no
  Windows reference for BW2 now** (decided by the user: nothing available).
  So correctness rests on CPU-level checks (ISA suite, differential testing
  against Unicorn with pcrecomp's `difftest.py`), D3D9 semantics checked
  against the documentation, and the user's play-testing. The runtime should
  make frame capture easy so a reference can be added later if one appears.

### 7.4 Input and display (decided with the user; mapping is my call)

- Mouse: as on Windows (left, right, middle, wheel).
- Trackpad: two-finger click = right click; two-finger scroll = wheel;
  Option + drag = middle button (camera rotate). Pinch or rotate gestures are
  not mapped in the first pass.
- Cursor coordinates from the real client size (NFSU2 had a hard-coded
  height bug at 1080p).
- Windowed and full screen both supported, with a switch (key binding and/or
  menu item, my call: Cmd+Ctrl+F like other Mac apps). Resolutions come from
  the game's own settings menu, and the display's native resolution is
  offered in that list. Retina aware.

### 7.5 Build and packaging

- Compile with `-mcpu=apple-m1` (not `native`) and a fixed minimum macOS
  version (my call: macOS 13); otherwise the binary may refuse to run on
  other Macs.
- Guard build scripts against silent failures NFSU2 hit: empty optimisation
  flags building at `-O0`, clang killed by SIGPIPE while the build returns 0,
  over-long `xargs` command lines, zsh not word-splitting variables.
- Builder detects Xcode Command Line Tools with `xcode-select -p` plus file
  checks, never by running the `clang`/`xcrun` shims (they pop Apple's
  installer).
- Builder pins its Python (if it needs one) by SHA-256 and runs every step
  with a fixed PATH in its own process group; cancel sends SIGTERM then
  SIGKILL.
- App is ad-hoc signed and verified with `codesign --verify --deep --strict`.
  Game data is APFS-cloned into the bundle with a manifest. Quarantine
  removed from built files.
- If a launcher process exists, it runs the engine as a child, not via
  `exec`, so folder permissions it was granted are inherited.
- AppKit event loop basics: `finishLaunching`, pump the run loop, a menu,
  `NSHighResolutionCapable`, `NSTerminateCancel` (not `NSTerminateLater`).

## 8. The Builder

A small native Mac app in Swift, built by copying and adapting the user's
own NFSU2 Builder (see section 2): a checklist where each missing item is a
red row with a button that fixes it. Its Xcode CLT detection, pinned Python,
process-group runner, quarantine and signing steps carry over; the inputs
(discs, three patches) and build steps are new.

Rows:

1. Xcode Command Line Tools installed.
2. Discs 1-4 found and fingerprints match (accepts zip, bin/cue, iso; one
   folder or picked one by one).
3. Patch 1.1, 1.2, fan 1.42: each "Download" or "I have it" (file picker);
   fingerprint checked.
4. Free disk space enough for extraction + build (number to be measured).
5. Build: extract installer data from the discs (no Windows installer run),
   lay out the install tree as the Windows installer would, apply 1.1, 1.2,
   then take `white.exe` from the fan patch; lift; compile; assemble and sign
   the app; show Play.

**Phase 0** (`docs/research/official-patches.md`): both are InstallShield 11
setup launchers whose appended files are an ordinary InstallShield cabinet
set (unshield reads it). Their changes are partly whole files and partly an
**RTPatch** binary diff (Pocket Soft's proprietary format; no open macOS
applier known). The Builder needs an RTPatch applier: first candidate
RTPatchX (Apache-2.0, C source); otherwise our own decoder or lifting
`patchw32.dll`. PCGamingWiki serves both patches through a three-step
terms/confirm flow with no login.

## 9. Repo and identity

- Local git repo at `~/src/black-and-white-2`, branch
  `main`. Identity: `007hacky007` /
  `007hacky007@users.noreply.github.com`, `commit.gpgsign=false`. No remote,
  nothing pushed.
- `.gitignore` keeps out every disc, patch, exe and all generated output.
  pcrecomp's `audit_repo.py` runs before any commit that touches tooling
  output, and before any future publish.
- Licenses: our code MIT (my call, matches pcrecomp). Every third-party piece
  must allow redistribution in a public repo (MIT/BSD/zlib, or LGPL used as a
  separate library). No NFSU2Recomp code.
- Commit messages: short, imperative, no AI attribution.
- What may be committed (my call): facts *about* the exe that contain none of
  its content: addresses, function catalogs and seed lists, import argument
  tables, per-address overrides written by us, SHA-256s and sizes. What may
  not: anything copied out of the game or patches (bytes, strings, shaders,
  textures, sounds) and C generated from the exe. So the Builder applies a
  checked-in, hash-pinned catalog to the user's exe and lifts and compiles on
  their Mac, as NFSU2 does; it does not rerun discovery. The HLSL
  capture-and-precompile fallback (5.2) would therefore have to run on the
  user's Mac too, never ship captured shaders.

## 10. Investigations to do first (phase 0)

Each answers a question that could change the plan. All are read-only on
the game files.

1. Extract the discs (InstallShield 9, four volumes) with pcrecomp's tools or
   another open extractor. Record the install tree and file list.
2. Identify the formats of official patches 1.1 and 1.2, and whether they
   change `white.exe` only, data only, or both. Find stable download sources
   and record SHA-256s.
3. Analyse `stxt774` / `.name0` and the 47 call sites (6.2). Go/no-go for
   the fan exe.
4. Run pcrecomp's 32-bit function discovery on the fan exe; score coverage of
   `.text` without relocations (6.1). Check what RTTI and vtables yield.
5. Measure SSE/x87 instruction mix and any opcodes pcrecomp's lifter lacks.
6. Check for C++ exceptions / SEH on normal paths (6.5).
7. Check how the game uses D3DX HLSL compile (which shaders, how many, from
   which files), and whether a capture-and-precompile route is viable.
8. Check `isextract.py` handles InstallShield 9 multi-volume; if not, pick an
   open alternative (e.g. unshield) that fits section 9.

**Status 2026-11-10: all eight answered**, see
`docs/research/phase0-summary.md` and the reports beside it:
1 `install-tree.md`, 2 `official-patches.md`, 3 `safedisc-leftovers.md`,
4-5 `function-discovery.md`, 6 `white-exe-survey.md`, 7 `d3dx-usage.md`,
8 `install-tree.md`.

## 11. Decided vs left to the builder

Decided by the user:

- Original game as shipped; no extras.
- Executable: fan patch 1.42 `white.exe`, after official 1.1 and 1.2.
- Patches are never shipped; Builder downloads from the source or takes a
  local copy.
- Personal for now; local git repo with the 007hacky007 identity.
- No NFSU2Recomp code; NFSU2 is a reference for ideas and mistakes. The
  user's own NFSU2 Builder code is the exception and may be reused.
- If the SafeDisc leftovers are live protection: stop and ask.
- No Windows reference is available.
- Build on pcrecomp's MIT pieces where they fit.
- Done = campaign playable start to finish, settings, save, load.
- Mouse and trackpad both supported. Windowed and full screen both supported.

My calls (the user may override):

- USA release only; English only; Battle of the Gods out.
- Trackpad mapping in 7.4; Cmd+Ctrl+F for the full screen switch.
- Minimum macOS 13; `-mcpu=apple-m1`.
- pcrecomp vendored at a pinned commit under `third_party/pcrecomp/`.
- Our code MIT.
- Builder accepts zip, bin/cue, iso.
- Saves and registry under `~/Library/Application Support/Black & White 2/`.
- Performance target: smooth at native resolution on an M1-class Mac.

Left to the builder:

- Choice of HLSL compiler library, OpenAL implementation, and whether SDL2
  stays for anything.
- `lift32.py` (global registers) vs `lift32_cpu.py` (CPU struct), or a
  hybrid, based on threading and performance.
- Sharding, caching and build orchestration details.
- Exact phasing after phase 0.
