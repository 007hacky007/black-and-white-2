# Build pipeline: whole-game lift and link check

Measured 2026-11-11 on an Apple M1 Pro (10 cores, 32 GB), Xcode clang.
`python -m bw2.pipeline` lifts three modules to C under `gen/` and builds
`out/bw2-linkcheck`. That binary links every lifted function against stub
host functions, looks up the entry point in the dispatch table and exits.
No game code runs.

## Modules

| module | source | functions lifted | shards | C | traps | lift |
|---|---|---|---|---|---|---|
| `white` | `white.exe` `.text` | 47,179 | 95 + 1 cold | 252.9 MB | 100 | 77.8 s |
| `vm` | `white.exe` `stxt774` + `.name0` | 228 | 1 | 18.8 MB | 0 | 5.1 s |
| `slr` | `ScriptLibraryR.dll` 1.2 `.text` | 1,876 | 4 | 15.2 MB | 39 | 4.6 s |
| `d3dx` | disc 1's `d3dx9_25.dll`, rebased to `0x08000000`, `.text` (plan 9) | 5,681 | 11 + 1 cold | 101.3 MB | 13,294 | 29.5 s |

Link set: 49,283 functions, 5 unresolved direct targets. All five are wild
addresses outside any image (`0x347DB478`, `0x47239E12`, `0x83B279A0`,
`0xC76E37E3`, `0xDDBD7AA5`), so they come from data decoded as code. Each one
gets a stub that aborts loudly.

Function counts are catalog starts minus aliases, which the lifter skips. The
DLL catalog covers 78.19% of its `.text` (phase 0 / Task 6). It has not been
re-cleaned; that happens on its next discovery run.

### Traps by mnemonic

`white`: int 17, iretd 14, insd 13, insb 11, int1 10, aad 8, cli 4, lcall 4,
salc 4, aam 3, fldenv 2, fnstenv 2, hlt 2, fbld 1, fbstp 1, into 1, invlpg 1,
pinsrw 1, sti 1.

`slr`: int 32, fldenv 2, fnstenv 2, ficom 1, hlt 1, iretd 1.

`d3dx`: insb 2867, arpl 2335, insd 2172, pfmul 1273, bound 873, pfadd 719,
daa 691, invd 405, das 358, outsw 196, pfsub 148, femms 131, popaw 105,
aas 104, pfacc 99, aaa 73, sldt 68, int 51, iretd 50, into 47, pfrcpit2 43,
aam 42, int1 42, salc 42, sti 36, pfsubr 27, pfrcp 26, pfrcpit1 24, ljmp 22,
fcmovne 21, pfcmpge 21, lcall 20, ficom 19, pfrsqit1 19, pfrsqrt 19,
pswapd 19, aad 16, hlt 13, pfcmpgt 13, pfpnacc 9, pi2fd 9, pfnacc 7, cli 4,
fisttp 3, pfcmpeq 3, packssdw 2, pf2id 2, pmaxsw 2, ficomp 1, pfmax 1,
pfmin 1, pmovmskb 1. The DLL keeps its read-only data in `.text`, so far
more data decodes as code than in `white`; the privileged, BCD and I/O
mnemonics come from there. The 3DNow! ones (`pf*`, `femms`, `pswapd`,
`pi2fd`, `pf2id`; about 2,600) are D3DX's AMD code paths, which it takes
only on a CPU that reports 3DNow!. `pmaxsw`, `pmovmskb` and `packssdw` are
integer SSE/MMX sites to check when the run first reaches them.

Most of these are privileged, BCD or I/O instructions that only show up when
data is decoded as code. Three kinds may be real code and need checking when
the runtime first reaches them:

- `fldenv`/`fnstenv`: probably CRT floating-point environment helpers.
- `ficom`: an x87 integer compare.
- `pinsrw`: one SSE site in `white`.

A trap stops the program and prints its address, so none of them can fail
silently.

## Catalog cleanup

The first full lift produced 461 MB of C for `white`. One shard was 187 MB,
and clang -O1 had not finished it after 2.5 hours.

The cause was the function at `0xB84D70`: about 500 KB of straight-line
`push`/`call` with no branches, which looks like a generated table setup.
White.exe's 22 MB `.data` holds dwords that happen to equal addresses inside
it. disasm32 keeps such "weak" entries as function starts. pcrecomp's
vtable scan also reads runs of those numbers as vtables (38 seeds inside this
one function). The catalog therefore cut the function into about 48 starts,
some of them mid-instruction, and each one lifted as a near-full copy.

`bw2/catclean.py` now runs after discovery:

1. It runs disasm32's mid-instruction drop to a fixpoint. A second pass over
   disasm32's own output drops more.
2. Seeds split into two classes:
   - Strong: RTTI, VM and relocation seeds.
   - Weak: 16-byte-aligned vtable-scan seeds, and gap seeds.

   RTTI seeds are 99.8% 16-byte aligned. Vtable-scan-only seeds are 73%.
3. It walks each trusted body straight on, with no instruction cap. A body
   is trusted if it is called, strongly seeded, or aligned. Untrusted bodies
   are not walked at all. disasm32's own walk stops after about 8k
   instructions.
   - An uncalled entry the walk falls into becomes an alias. After a `call`,
     this applies only if the entry is unaligned.
   - An entry inside one of the walk's instructions is dropped.

Effect on `white`: 65,747 catalog entries became 64,473, and aliases went
from 16,247 to 17,294. The lift went from 461 MB to 253 MB, traps from 212
to 100, and unresolved targets from 10 to 5.

If one of these rules is wrong, a real function that is only reached through
a data pointer and sits inside another body becomes an alias or is dropped.
Calling it then fails loudly as unresolved.

## Optimisation level

Sample: every 5th `white` shard (19 shards), compiled in parallel. The
oversized function was excluded (see below).

| level | wall | objects |
|---|---|---|
| -O1 | 11.4 s | 22.3 MB |
| -O2 | 12.4 s | 22.1 MB |
| -O3 | 13.6 s | 22.3 MB |

-O3 costs 1.10x -O2, which is under the plan's 1.5x threshold, so the build
uses **-O3**.

`0xB84D70` lifts to a single function of 152,156 lines. clang -O1 ran for
more than 28 minutes on it without finishing. At -O0 it compiles in 7.8 s
and peaks at 1.8 GB. The next largest function is 8,621 lines. Any function
over 50,000 lifted lines now goes to its own `<module>_cold_<va>.c`, which
`build()` always compiles at -O0. Only this one function qualifies. It looks
like a one-shot setup routine, so running it unoptimised should cost
nothing noticeable.

## Full build

`/usr/bin/time -l python -m bw2.pipeline --build-only --opt O3`, from a clean
`gen/obj`:

- 104 objects compiled; build step 79.2 s.
- 79.38 s wall, 506.2 s user, 12.3 s sys.
- Maximum resident set 1.60 GB. This is `time`'s figure for the largest
  child process, the cold shard's clang.
- Objects 181 MB; `out/bw2-linkcheck` is 91.8 MB.
- Output: `linkcheck: 49283 functions, entry 0x00AD8FA3 -> found`.

`FLAGS` include `-fwrapv`. Lifted `imul` multiplies as `int32_t`, and x86
code overflows on purpose, so signed overflow must wrap rather than be
undefined behaviour at -O3. The build stamp hashes the headers and the host
source, so editing `recomp_types.h` recompiles everything.

A full lift of all three modules takes about 88 s. Lift plus build from
scratch takes about 2.8 minutes. That is in line with the spec's budget,
NFSU2's roughly 2 minutes for 2.3x less code.

## Plan 3 additions

- The default host is the engine (`out/bw2`): win32hle's headless core plus
  `runtime/bw2/`, with based memory at `RECOMP_MEM_BASE=0x100000000000`.
  `linkcheck` stays as `build(host="linkcheck")`.
- The lift merges extra function starts: 48 that are only named by code
  immediates (`white-1.42.extra-starts.json`), and 138 of the protection VM's
  native entries, exits and thunks (`white-1.42.vm-extra-starts.json`). The
  link set is 49,423 functions, and the VM module is 330 functions.
- Every module is lifted with computed returns, and the VM module also with
  run-time `push` operands (`docs/research/boot.md`). The engine is 114 MB.
- EH landing pads: 9 white functions (9 continuations) and 1 in ScriptLibraryR.
- Lifted back-edges hand the machine to other guest threads (`RECOMP_BACKEDGE`).
- `python -m bw2.pipeline --only vm` relifts one module and reuses the others.
  Adding any function changes `recomp_funcs.h`, so every shard recompiles
  (about 80 s).

## Known items for later plans

The cold shard's host function has a 557 KB stack frame at -O0. That is
above macOS's 512 KB default for secondary threads, so guest threads need
large stacks (plan 3).

pcrecomp's `MEM8`/`MEM16`/`MEM32` cast through `volatile`. That forces every
guest memory access to be a real load or store, so clang cannot keep values
in registers or merge accesses. NFSU2 used `restrict` instead. The change
belongs in the runtime plan, once a running game can measure frame time.

## Plan 9: the disc's `d3dx9_25.dll` as a fourth module

Measured 2026-11-12 on the same machine.

- **Catalog.** `python -m bw2.d3dx --catalog` writes
  `bw2/catalog/d3dx9_25-apr2005.functions.json` (addresses only; the
  `sha256` is the rebased image's, `image_base` `0x08000000`). Discovery
  took 3,670 s wall (2,753 s user, 6.5 GB peak), two `disasm32` rounds of
  about 28 minutes each. Seeds: 323 exports, 254 relocation targets in
  `.data`, 149 vtable-scan methods (from `.data`; the scan skips `.text`),
  0 RTTI, 0 gap seeds, 0 `EXTRA_SEEDS`, and 1,076 code-section relocation
  targets (402 new after round 1). 14,394 entries, 5,681 starts and 8,713
  aliases; 92.20% of the 2,162,371 bytes of `.text` covered.
- **Code-section relocation seeds** (`discover(.., code_reloc_seeds=True)`,
  `reloc_code_targets`): HIGHLOW sites in `.text` outside every instruction
  and jump table the first round decoded, whose target is a vtable-scan
  method or is not inside a decoded instruction and decodes as a function
  body (`probes_as_function_body`). By their first instructions (157 of the
  402 new ones load or address `ecx` from an `ebp` frame slot) most look like
  C++ unwind funclets and SEH blocks that the DLL's own tables name.
- **Labels folded into their bodies** (`discover(.., cfg_aliases=True)`,
  `catclean.alias_cfg_labels`). The first lift of the catalog was 1.3 GB of
  C with 304 cold shards: a single 176 KB function at `0x0808E0C4` (unrolled
  float code, 38,521 decoded instructions) had 304 of its loop heads in the
  catalog as starts, and each start lifted as a full copy of the function.
  In an image that keeps its relocations every address the code can take is
  a relocation target, a call target or an export, so a start that is none
  of these and that a trusted body's own control flow reaches is a label of
  that body; it becomes an alias. 530 starts folded. Off for `white`, `vm`
  and `slr`, whose catalogs do not change.
- **Lift:** 5,681 functions, 11 shards and 1 cold shard (`0x0808E0C4`),
  101.3 MB of C, 29.5 s. A lifter patch (`third_party/pcrecomp.PATCHES`)
  makes a `loop`/`jecxz` to a target with no label dispatch like a `jcc`,
  for data decoded as a `loope` at `0x08005C6D`.
- **Link set:** 55,115 functions, 9 unresolved direct targets. The four new
  ones (`0x0D088B12`, `0xD80A380D`, `0xE0990182`, `0xE0A90162`) lie outside
  every image, like `white`'s five, so they come from data decoded as code
  and stay loud stubs. None is inside `0x08000000`-`0x08253000`.
- **Build:** lifting the four modules takes about 181 s (`white` 138 s,
  `vm` 8 s, `slr` 6 s, `d3dx` 30 s). The build after relifting only `d3dx`
  compiled 116 of 200 objects in 194 s (235 s wall with the lift).
  `out/bw2` is 143.4 MB (114.8 MB before). Every build copies the rebased
  DLL to `out/d3dx9_25.dll`, beside the engine.
