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

## Known items for later plans

The cold shard's host function has a 557 KB stack frame at -O0. That is
above macOS's 512 KB default for secondary threads, so guest threads need
large stacks (plan 3).

pcrecomp's `MEM8`/`MEM16`/`MEM32` cast through `volatile`. That forces every
guest memory access to be a real load or store, so clang cannot keep values
in registers or merge accesses. NFSU2 used `restrict` instead. The change
belongs in the runtime plan, once a running game can measure frame time.
