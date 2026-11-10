# SafeDisc leftovers in the fan patch white.exe (phase 0 item 3)

Date: 2026-11-10. Tool: `bw2/xsect.py` (`cross_section_refs`, `disasm_at`).

## What the extra sections are

| Section | Range | What it holds |
|---|---|---|
| `stxt774` | `0x2428000`-`0x24293b4` | 18 five-byte thunks, each `jmp <.name0 entry>` (SafeDisc's section name) |
| `.name0` | `0x242a000`-`0x2450925` | first ~8 KB: a **bytecode virtual machine** (dispatcher at `0x242a000` and its opcode handlers); then ~140 KB of encrypted bytecode (entropy 7.98); at the end, one entry per thunk: `push <bytecode address>; jmp 0x242a000` |
| `.name1` | `0x2051000` RVA | the 260 relocations for `.name0` |

The dispatcher at `0x242a000` saves all registers and flags, calls
`GetCurrentThreadId` (IAT `0xc121cc`) to find a per-thread context, takes the
bytecode address pushed by the entry, then loops: fetch a byte, decrypt it
with a rolling key (`add bl; ror 5; sub 0x12; xor 0xc7; ror 5`), and jump
through a 256-entry handler table at `0x242a885`. This is code virtualization:
18 of the game's functions were compiled into this VM's bytecode by the
protection, and the original x86 for them is not in the file.

## Who calls it

The byte scan finds 47 rel32 branches into the sections, 19 distinct
targets. One is a false positive (`0x4142c9`, inside the instruction at
`0x4142c7`; its "target" is encrypted bytecode). The real picture:

- **46 call sites in ordinary game code, 18 thunks.** Every one is
  `push a; push b; call <thunk>; add esp, 8`: a cdecl function of two
  arguments that returns a value in `eax`.
- Callers sit in game code around `0x518e00`-`0x60b800` and
  `0x851500`-`0x851d40`. Several compare the result with magic constants
  straight after the call (`cmp eax, 0xb7101d4a` at `0x5fceba`, `cmp eax,
  0x412a3267` at `0x5ef97e`), on paths shared with a non-virtualized call
  through a function pointer (`call edi`) just before. That shape fits hash or
  identifier functions used in normal logic, not one-off disc checks; it is
  not proven.
- 134 aligned dwords in `.text` point into the sections (125 into `.name0`,
  9 into `stxt774`); not yet classified.

| Thunk | Entry in `.name0` | Call sites |
|---|---|---|
| `0x2428efc` | `0x2450601` | 1 |
| `0x2428f1c` | `0x2450615` | 1 |
| `0x2428f3c` | `0x2450629` | 1 |
| `0x2428f7c` | `0x245065b` | 2 |
| `0x2428f9c` | `0x2450679` | 8 |
| `0x2428fdc` | `0x24506ab` | 4 |
| `0x242901c` | `0x24506dd` | 3 |
| `0x242903c` | `0x24506fb` | 2 |
| `0x242908c` | `0x245072d` | 1 |
| `0x24290dc` | `0x245075f` | 5 |
| `0x24290fc` | `0x2450773` | 3 |
| `0x242913c` | `0x24507a5` | 1 |
| `0x242917c` | `0x24507d7` | 5 |
| `0x242919c` | `0x24507eb` | 3 |
| `0x24291dc` | `0x245081d` | 1 |
| `0x242921c` | `0x245084f` | 1 |
| `0x242925c` | `0x2450881` | 1 |
| `0x242929c` | `0x24508b3` | 3 |

The fan patch did not remove this: it removed the outer SafeDisc wrapper
(the code is no longer encrypted on disk) but left the virtualized functions
in place, and the game still calls them.

## Verdict

These are not dead stubs and not the fan patch's own code caves. They are the
protection's virtual machine running 18 real functions that the game calls
from 46 places on what look like normal paths.

VERDICT: LIVE

## What could be done (for the user to decide)

1. **Run the VM as-is.** The interpreter is ordinary x86 code. Lift it like
   the rest of the game and let it interpret its own bytecode at run time.
   Nothing is decrypted, removed or bypassed; the program runs as shipped.
   Risks: the VM may use tricks the lifter dislikes (self-modifying code,
   `pushfd`/`popfd` games, jumps into the middle of instructions), and the
   virtualized functions might call disc or driver checks that our Win32
   layer would then have to answer.
2. **Devirtualize.** Decode the bytecode of the 18 functions back into C.
   Faster at run time, but it is undoing the protection, and a lot of work.
3. **Reimplement the 18 functions** from their observed behaviour (inputs
   and outputs logged while option 1 runs). Removes the VM in the end.

Per the design (section 6.2), work stops here until the user chooses.
