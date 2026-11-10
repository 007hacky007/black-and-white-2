# Phase 0 summary

Date: 2026-11-10. Branch `phase0`. Plan:
`docs/superpowers/plans/2026-11-10-bw2-phase0-foundation.md`.
Everything ran on the user's USA discs and the three patches, on macOS, with
no Windows code executed.

## Answers

1. **Discs to install tree: GO.** One InstallShield 9 set in five cabinets
   over four discs. unshield extracts all files with MD5 checks in about
   18 s; 773 installed files, 3.22 GB, manifest committed. Nine group
   destinations are inferred from paths the exe opens (three unverified).
   `install-tree.md`.
2. **Official patches 1.1 and 1.2: GO WITH RISK.** InstallShield 11 setups
   whose payload unshield reads. Their changes are partly whole files and
   partly **RTPatch** diffs (proprietary, no open macOS applier known). The
   Builder needs an RTPatch applier (try RTPatchX, Apache-2.0, first).
   Downloads work from PCGamingWiki through a terms/confirm flow.
   `official-patches.md`.
3. **SafeDisc leftovers: LIVE, user chose to run them as shipped.** A
   code-virtualization VM in `.name0` runs 18 game functions (46 call
   sites). Its 2.2 KB interpreter lifts as one function needing only `pushf`.
   `safedisc-leftovers.md`.
4. **Function discovery without relocations: GO.** 90.35% of `.text`
   covered (93.8% counting padding), 65,586 functions, RTTI gives 9,327
   virtual methods and vtables 15,481. `function-discovery.md`.
5. **Instruction mix and lifter gaps: GO.** SSE 7.5%, x87 3.4%, MMX 0.1%.
   All 49,323 starts lift; 193 functions hit unimplemented instructions,
   mostly MMX. Output is 458 MB of C. `function-discovery.md`.
6. **Exceptions: needs guest C++ EH.** 271 throw sites, 1,892 functions with
   unwind tables, 7 `__try` blocks. pcrecomp's `eh32` scheme is the base.
   `white-exe-survey.md`.
7. **D3DX: GO, bigger than expected.** All 60 imports are used. Most
   rendering is `ID3DXEffect` over 59 precompiled fx_2_0 effects; 29 text
   assembly shaders; only 7 HLSL `.fx` sources need a compiler
   (vkd3d-shader candidate). `d3dx-usage.md`.
8. **Extractor: unshield**, not pcrecomp's `isextract.py` (fails on files
   that span cabinets). `install-tree.md`.

New scope found on the way:
- A second game module to lift: `PlugIns/ScriptLibraryR.dll` (32-bit DLL,
  replaced by patch 1.2).
- RTPatch applier for the Builder.
- The ID3DXEffect framework.
- Build size: 458 MB of generated C.

## Proposed order of the next plans

1. **Lifter and build pipeline.** Lift `white.exe` (including the VM) and
   `ScriptLibraryR.dll` with pcrecomp, close the discovery gaps, add MMX and
   `pushf`, shard the C, and measure clang compile time and binary size on
   an M1-class Mac. Exit: everything compiles and links against stub imports.
2. **Patch application and install tree** (Builder back end, no UI): RTPatch
   applier, 1.1 then 1.2 applied to the extracted tree, verified against
   fingerprints. Without this there is no correct data to boot.
3. **Win32 host on pcrecomp win32hle, based memory on macOS**, plus guest C++
   EH from `eh32`, OpenAL and DirectInput stubs. Exit: the game reaches its
   first `Direct3DCreate9` call.
4. **D3D9 on Metal and D3DX** (effects, assembler, math, textures; HLSL via
   vkd3d-shader). Exit: front-end menu renders.
5. **Audio (OpenAL + EAX), input (mouse, trackpad mapping), display
   (windowed/full screen)**, Bink video. Exit: a new campaign starts and the
   first land plays.
6. **Builder app** (copied and adapted from the user's NFSU2 Builder).
7. **Campaign play-through and save/load**, against the "done" definition.

Plans 1 and 2 are independent and can run in either order.
