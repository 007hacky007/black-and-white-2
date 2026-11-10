# Discovery gaps in white.exe .text (plan 1, task 5)

Date: 2026-11-10. Code: `bw2/gaps.py` (classification) and the reseed loop in
`bw2/discover.py` (`max_rounds=4`). Catalog regenerated:
`bw2/catalog/white-1.42.functions.json`.

## How gaps are classified

| Kind | Rule |
|---|---|
| `int3` | 16 bytes or more of `0xCC` |
| `align` | under 16 bytes, only `0xCC`/`0x90`/`0x00` (and leading padding split off a longer gap) |
| `data` | starts with 3 dwords pointing into `.text` (pointer or jump table), or more than 30% of its first 64 bytes do not decode |
| `code` | after leading padding: a rel32 `call`/`jmp` from covered code lands on it, an aligned dword in `.rdata`/`.data` points at it, or it opens with `55 8B EC` / `83 EC` / `81 EC` |
| `unknown` | none of the above |

`code` gap starts are fed back into disasm32 as seeds until a round adds none
or 4 rounds have run.

## Result

| | Before (phase 0) | After reseeding |
|---|---|---|
| Functions | 65,586 | 65,747 |
| Covered bytes of `.text` | 7,638,381 (90.35%) | 7,719,165 (**91.30%**) |
| Gap seeds added | - | 479 over 4 rounds (the cap; the last round still added some) |
| `code` gaps | 433 (89,955 bytes) | 2 (525 bytes) |
| `unknown` gaps | 4,625 (378,399 bytes) | 4,664 (384,938 bytes) |
| `data` gaps | 249 (11,514 bytes) | 250 (11,516 bytes) |
| `align` + `int3` | 336,354 bytes | 338,459 bytes |
| Wall time / peak memory | 455 s / 3.8 GB | 2,421 s / 4.0 GB |

Covered plus padding: about 95.3% of `.text`. The rest (about 385 KB,
4.6%) is `unknown`.

## Gaps of 1 KB or more after reseeding

| Range | Bytes | Kind | What it looks like |
|---|---|---|---|
| `0xbd4a24`-`0xbdb536` | 27,410 | unknown | entropy 4.63, 26% zero bytes; decodes, but as noise (`lea ebp, [edi+ebx-1]` ...): most likely a table or data block inside `.text` |
| `0x653270`-`0x6537a0` | 1,328 | unknown | clean function code (`mov eax, [0x19a3244]; sub esp, 0x48; push edi ...`) with no reference found |
| `0xa17120`-`0xa17620` | 1,280 | unknown | clean SSE function code (`movss`, `comiss`, `jae`) with no reference found |
| `0x7eefa0`-`0x7ef3f0` | 1,104 | unknown | to check |
| `0xab9030`-`0xab946d` | 1,085 | unknown | to check |
| `0x95784e`-`0x957c60` | 1,042 | unknown | to check |
| `0xa4d9c0`-`0xa4ddd0` | 1,040 | unknown | to check |
| `0xacc700`-`0xaccb10` | 1,040 | unknown | to check |

Several `unknown` gaps are well-formed functions on 16-byte boundaries that
nothing in the image references by address. They are either dead code or
reached through pointers computed at run time (vtables built in code, `lea`
of a function address). The spec's answer for those is the trace loop: an
unresolved call traps with its address (plan 1 task 2), and that address is
seeded on the next lift. Seeding every clean-decoding 16-byte-aligned gap
would also work, at the price of lifting dead code; not done yet.

## The five shader call sites phase 0 found in gaps

| Site | Import | Now |
|---|---|---|
| `0xa573fd` | `D3DXCompileShaderFromFileA` | covered by `sub_A573C0` |
| `0xa8f19d` | `D3DXCompileShaderFromFileA` | covered by `sub_A8F160` |
| `0xa8f22d` | `D3DXCompileShaderFromFileA` | covered by `sub_A8F1F0` |
| `0xa9f93d` | `D3DXCompileShaderFromFileA` | covered by `sub_A9F900` |
| `0x97cdb9` | `D3DXCreateEffect` | still in an `unknown` gap `0x97cd30`-`0x97cdf0` (192 bytes of small methods, no address reference): a virtual method whose vtable was not found; will surface through the trace loop |

So the `D3DXCompileShaderFromFileA` callers are real functions, not dead
code as phase 0 guessed; which files they compile is a question for the
D3DX plan (trap and log at first boot).
