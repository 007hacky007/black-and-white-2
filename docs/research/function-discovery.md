# Function discovery without relocations (phase 0 items 4 and 5)

Date: 2026-11-10. Code: `bw2/discover.py` on pcrecomp `2adc02f`
(`tools/cpp/rtti.py`, `tools/cpp/vtable_scan.py`, `tools/disasm/disasm32.py`).
Catalog: `bw2/catalog/white-1.42.functions.json` (address, end, entry kind,
thunk flag; no bytes).

## Method

pcrecomp's analyzer takes the code range from the first to the last
executable section. In `white.exe` that runs from `.text` (`0x401000`) to the
end of `.name0` (`0x2451000`) and takes in `.rdata` and the 23 MB `.data`.
So discovery runs on a *view*: a copy of the exe in which only the chosen
sections keep their executable flag (`bw2.discover.code_view`). The game
(`.text`) and the protection VM (`stxt774` + `.name0`) are discovered
separately.

Seeds: 9,327 virtual methods from RTTI, 15,481 from vtable-shaped pointer
runs; 16,782 distinct seeds inside `.text`, plus the entry point.

## Result: game code (`.text`)

| Measure | Value |
|---|---|
| Functions | 65,586 (49,323 starts + 16,263 aliases: labels inside other bodies and C++ EH funclets) |
| Thunks / leaves | 277 / 20,922 |
| Instructions (per function, overlaps counted) | 8,609,941 |
| **Coverage of `.text`** | **7,638,381 of 8,454,603 bytes = 90.35%** |
| Uncovered, `int3` padding | 294,590 bytes |
| Uncovered, gaps under 16 bytes (alignment) | 298,236 bytes |
| Explained (code + padding) | about 93.8% |
| Largest uncovered run | `0xbd4a24`-`0xbdb536` (27,410 bytes), then 3,921 at `0xb8015f` and 2,692 at `0xa1247c`; likely data in `.text` (tables), to check |
| Wall time / peak memory (M1-class, 10 cores) | 455 s / 3.8 GB |

For scale, NFSU2 with its intact relocation table reached 96.1% code coverage
on a 3.7 MB `.text`.

## Instruction mix (linear over the covered ranges)

| Group | Instructions | Share |
|---|---|---|
| general purpose | 2,054,852 | 88.69% |
| **SSE (single precision)** | 173,393 | **7.48%** |
| x87 | 79,687 | 3.44% |
| SSE2+ | 6,307 | 0.27% |
| MMX | 2,680 | 0.12% |
| 3DNow! | 2 | 0 |

Top mnemonics: `mov` 612,887, `push` 428,561, `call` 187,341, `pop`
102,744, `movss` 101,413, `lea` 98,495, `add` 96,504, `test`, `cmp`, `je`,
`ret` 58,490, `jne`, `jmp`, `xor`, `sub`, `int3` 25,832, `fld` 23,114,
`mulss` 20,691, `fstp` 20,263, `addss` 12,749.

SSE is six times NFSU2's share (1.25%); the SSE lifter (`lift32_sse.py`)
matters more here than x87.

## Trial lift (pcrecomp `lift32.Lifter`, global-register model)

Every catalogued start, disassembled and lifted in process, output counted
and discarded (driver kept outside the repo; it is measurement only).

| Measure | Value |
|---|---|
| Functions lifted | **49,323 of 49,323**, no lifter exceptions |
| Functions with an `UNIMPLEMENTED` instruction | 193 |
| Generated C | 458 MB (NFSU2: about 137 MB) |
| Time / memory | 160 s / 0.27 GB |

Unimplemented, by mnemonic (2,516 sites, 32 mnemonics): MMX `pmaddwd`
1,652, `punpckhdq` 294, `punpckldq` 294, `paddsw` 52, `pmulhw`/`punpcklwd`/
`punpckhwd` 4 each, `pinsrw` 1; x87 `fcmovnb` 4, `fnstenv`/`fldenv` 2 each,
`fisttp`/`fbld`/`fbstp`/`ficom` 1 each; BCD `aam` 6, `das`/`aad` 3 each,
`daa`/`salc` 1 each; and privileged or I/O instructions that no user-mode
game executes (`iretd` 33, `insb` 28, `int1` 27, `int` 25, `insd` 17,
`into` 15, `hlt` 13, `sti` 10, `cli` 7, `lcall` 7, `ljmp` 2, `invlpg` 1),
which mark data decoded as code inside some catalogued functions.

The MMX block is concentrated (probably an audio or video routine) and is
the only real lifter gap; the rest are either rare x87 forms or bogus code.

## Result: protection VM (`stxt774` + `.name0`), user choice 1

Seeds: the VM entry `0x242a000`, the 18 thunks, their 18 `.name0` entries,
and the 256 handler-table slots at `0x242a885` (191 distinct handlers, all
in `0x242a05c`-`0x242a86e`).

- The interpreter is about 2.2 KB of code (`0x242a000`-`0x242a885`), and
  discovery covers it (2,206 bytes). The other ~409 "functions" found in
  `.name0` come from scanning the encrypted bytecode and are noise.
- **Trial lift from `0x242a000`: one function, 84 KB of C, the only gap is
  `pushf` (20 sites).** `pushfd` needs the NFSU2 rule (keep EFLAGS bit 21).
- The handler table must be treated as a jump table of the interpreter
  function; the per-thread context table at `0x242ac85` is data.

## Gate

Coverage 90.35% meets the 90% threshold: **GO**.

First work for the lifter plan:
1. The 27 KB and smaller mixed gaps (data or missed code).
2. MMX in the lifter (`pmaddwd`, unpacks, `paddsw`, `pmulhw`).
3. `pushf`/`pushfd` with bit 21 preserved, for the VM.
4. Trim functions that run into data (the privileged-instruction hits).
5. Build time: 458 MB of C is 3.4x NFSU2; shard and measure compile time
   before anything else.
