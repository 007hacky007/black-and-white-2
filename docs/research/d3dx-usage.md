# D3DX and HLSL use (phase 0 item 7)

**Status (plan 9, 2026-11-13):** all 60 imports are answered; HLSL goes to Microsoft's own
compiler, lifted from disc 1's `d3dx9_25.dll`. See `hlsl.md`.

Date: 2026-11-10. Sources: `bw2.pe_survey.iat_callsites`, direct-call
counts to each import thunk, and the install tree. No shader text here.

## How the game reaches D3DX

Each of the 60 `d3dx9_25.dll` imports is reached through one
`jmp dword ptr [IAT]` thunk (at `0x91dd64`-`0x91dda6` and
`0xb522d6`-`0xb523f0`). Real use is the number of `call` instructions that
target the thunk:

| Area | Imports (direct callers) |
|---|---|
| Math (CPU only) | `D3DXMatrixMultiply` (54), `D3DXVec3Normalize` (21), `D3DXMatrixInverse` (15), `D3DXMatrixTranspose` (14), `D3DXQuaternionSlerp` (14), `D3DXMatrixLookAtLH` (11), `D3DXVec3TransformCoord` (11), plus 22 more quaternion/matrix/vector helpers with 1-9 callers each |
| Textures and surfaces | `D3DXCreateTexture` (8), `D3DXFillCubeTexture` (5), `D3DXCreateTextureFromFileInMemory` (5), `D3DXFilterTexture` (5), `D3DXCreateCubeTexture` (4), `D3DXLoadSurfaceFromMemory` (3), `D3DXSaveSurfaceToFileA` (3), `...FromFileInMemoryEx` for 2D, cube and volume (1 each), `D3DXGetImageInfoFromFileInMemory` (1), `D3DXLoadSurfaceFromSurface` (2), `D3DXLoadSurfaceFromFileInMemory` (1), `D3DXCreateTextureFromFileA` (1), `D3DXFillTexture` (2), save-to-file/memory (3) |
| **Effects** | `D3DXCreateEffect` (3: `0x96f4bb`, `0x97cdb9`, `0x97e1ea`), `D3DXCreateEffectPool` (1), `D3DXCreateFragmentLinker` (2) |
| **Shader compile** | `D3DXCompileShaderFromFileA` (4: `0xa573fd`, `0xa8f19d`, `0xa8f22d`, `0xa9f93d`), `D3DXCompileShader` (2: `0xab318b`, `0xab324b`), `D3DXAssembleShader` (2: `0xab3020`, `0xab30d0`), `D3DXGetVertexShaderProfile`/`PixelShaderProfile` (1 each), `D3DXGetShaderVersion` (1) |
| Other | `D3DXCreateSprite` (2), `D3DXCreateFontA` (1), `D3DXCreateSphere` (1), `D3DXCreateBuffer` (1), `D3DXFloat16To32Array` (4) |

Every D3DX import has at least one caller, so all 60 have to be implemented.

Shader and effect call sites, with the catalogued function before each site
(`bw2/catalog/white-1.42.functions.json`):

| Import | Call sites |
|---|---|
| `D3DXCreateEffect` | `0x96f4bb` in `sub_96F450`, `0x97cdb9` after `sub_97CCC0` (in an uncovered gap), `0x97e1ea` in `sub_97E080` |
| `D3DXCreateEffectPool` | `0x97af09` in `sub_97AF00` |
| `D3DXCreateFragmentLinker` | `0x96efd5` in `sub_96EF80`, `0x96f564` in `sub_96F450` |
| `D3DXCompileShaderFromFileA` | `0xa573fd`, `0xa8f19d`, `0xa8f22d`, `0xa9f93d`, **all four in uncovered gaps** (after `sub_A57340`, `sub_A8F120`, `sub_A8F1A3`, `sub_A9F890`) |
| `D3DXCompileShader` | `0xab318b` in `sub_AB3130`, `0xab324b` in `sub_AB31F0` |
| `D3DXAssembleShader` | `0xab3020` in `sub_AB2FD0`, `0xab30d0` in `sub_AB3080` |

Sites in uncovered gaps are either code discovery missed (to be fixed in the
lifter plan) or unreachable development code; the four
`D3DXCompileShaderFromFileA` sites being all unreached hints at the latter.

## Where shaders come from

| Location | Count | Format | Reaches D3DX through |
|---|---|---|---|
| `Data/effects/*.sdv` | 59 files, 1.45 MB | Lionhead container naming an `.fx`, holding a **compiled D3DX effect** (fx_2_0 binary, magic `01 09 FF FE` once per file) with embedded shader bytecode (about 325 vertex and 320 pixel shader token streams counted by version tokens) | `D3DXCreateEffect` on precompiled data: **no HLSL compile**, but the whole `ID3DXEffect` framework (parameters, techniques, passes, state assignments, effect pools) |
| `Data/EncryptedShaders/*.vsh.caesar3` | 17 | text shader **assembly** (`vs.2.0` ...), each byte shifted by +3 | `D3DXAssembleShader` |
| `Data/EncryptedShaders/*.psh.caesar3` | 12 | text pixel shader assembly, +3 shift | `D3DXAssembleShader` |
| `Data/EncryptedShaders/*.fx.caesar3` | 7 | text **HLSL effect source**, +3 shift | `D3DXCreateEffect` from source, or `D3DXCompileShader` (needs HLSL) |
| from file | ? | `D3DXCompileShaderFromFileA`, 4 callers | unknown file names; possibly a development path. Trap and log at first boot |

The +3 shift is trivial obfuscation the game itself undoes before passing the
text to D3DX; our D3DX sees plain text.

Official patch 1.1 rewrites about 55 of the `.sdv` files and patch 1.2
replaces `Skinned.sdv` (see `official-patches.md`).

## When compilation happens

All compile and effect-creation call sites sit in a handful of functions in
the `0x96e000`-`0x97e000` and `0xa57000`-`0xab3000` ranges, away from the
per-frame math callers. Inference: shaders are compiled at load time, not
per frame. Confirm at first boot.

## Recommendation

1. **Native ID3DXEffect for fx_2_0 binaries** is the big item: 59 effects
   carry most of the rendering. The embedded bytecode goes through the same
   D3D9-bytecode-to-MSL translator as everything else.
2. **Native shader assembler** (`vs/ps` 1.x-3.0 text to bytecode) for 29
   files: a small, well-documented job.
3. **HLSL** only for 7 `.fx` sources and whatever `D3DXCompileShader(FromFile)`
   is fed. Candidate: **vkd3d-shader** (part of vkd3d, LGPL-2.1, usable as a
   separate library under spec section 9): compiles SM1-3 HLSL to D3D9
   bytecode; its fx_2_0 effect support must be checked against these 7
   files. Fallback if it falls short: compile those 7 effects once on the
   user's Mac at build time and cache the result keyed by source hash (never
   shipped).
4. D3DX math: implement natively; bit-exact results matter less than matching
   D3DX's float behaviour closely (the game keeps x87 and SSE paths).
