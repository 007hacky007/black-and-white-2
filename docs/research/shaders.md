# The boot through shaders and effects (plan 5)

Measured 2026-11-12 on an Apple M1 Pro, with `out/bw2` built by `python -m bw2.pipeline`
(114,710,776 bytes), on the patched tree, headless, with a fresh support folder:

```
BW2_HEADLESS=1 BW2_SUPPORT=/tmp/s out/bw2 --game gamefiles/game
```

The run gets through DirectInput's setup, the device, every effect the game loads
at start-up and the shaders those effects carry. It exits with code 4 at its first
call into a later plan's layer:

```
[bw2] reached D3DXCreateSprite: owned by a later plan (from guest 0xDEAD0000, sub_00B5231E)
```

There is no loud line before it: no `TRIPWIRE`, no trap, no unresolved call, no
unimplemented import or method. The game asks no disc or driver question on the way.

`tests/test_boot.py::test_boot_after_effects` pins this: exit code 4, no
`reached D3DXCreateEffect`, no loud line, and exactly one stop, at one of plan 7's
names.

## Effects created

`BW2_TRACE_IMPORTS=1` gives 1,265,146 import calls to 202 distinct names. The
game makes 32 `D3DXCreateEffect` calls, one per file, in this order. Each passes
the compiled fx_2_0 binary out of its `.sdv` container (first dword `0xFEFF0901`),
with no defines, no include handler and flags 0. The first seven have no pool.
The game then makes one `D3DXCreateEffectPool`, and every later effect shares it.

| # | File (`Data/effects/`) | Bytes | Pool | Techniques | Parameters |
|---|---|---|---|---|---|
| 1 | `DownScaleFilter.sdv` | 1,804 | no | 1 | 3 |
| 2 | `BrightnessCutOffFilter.sdv` | 1,796 | no | 1 | 7 |
| 3 | `Gaussian1DFilter.sdv` | 2,876 | no | 1 | 5 |
| 4 | `BlurFilter.sdv` | 2,712 | no | 1 | 5 |
| 5 | `DistortionMaskFilter.sdv` | 1,716 | no | 1 | 3 |
| 6 | `DistortionFilter.sdv` | 3,684 | no | 1 | 6 |
| 7 | `ToneMapping.sdv` | 4,004 | no | 1 | 21 |
| 8 | `NewTree.sdv` | 36,972 | yes | 5 | 67 |
| 9 | `Tree.sdv` | 18,836 | yes | 2 | 64 |
| 10 | `Decals.sdv` | 17,372 | yes | 2 | 65 |
| 11 | `Ghost.sdv` | 33,376 | yes | 3 | 70 |
| 12 | `Aligned_Backlit_Gloss.sdv` | 27,856 | yes | 4 | 63 |
| 13 | `Aligned_Diffuse.sdv` | 26,508 | yes | 4 | 63 |
| 14 | `Aligned_Walls.sdv` | 27,732 | yes | 4 | 65 |
| 15 | `Unaligned_Walls.sdv` | 18,452 | yes | 2 | 62 |
| 16 | `Aligned_Gloss.sdv` | 27,544 | yes | 4 | 63 |
| 17 | `Unaligned_Gloss.sdv` | 18,296 | yes | 2 | 62 |
| 18 | `Aligned_Vines.sdv` | 22,628 | yes | 3 | 63 |
| 19 | `Aligned_Plants.sdv` | 22,352 | yes | 3 | 63 |
| 20 | `Rock.sdv` | 18,640 | yes | 2 | 63 |
| 21 | `Aligned_Thatch.sdv` | 27,004 | yes | 4 | 63 |
| 22 | `Thatch.sdv` | 18,748 | yes | 2 | 63 |
| 23 | `Pond.sdv` | 18,008 | yes | 2 | 66 |
| 24 | `Invisible.sdv` | 18,460 | yes | 2 | 63 |
| 25 | `yard.sdv` | 18,460 | yes | 2 | 63 |
| 26 | `NormalMap.sdv` | 18,668 | yes | 2 | 62 |
| 27 | `Lightmap.sdv` | 18,664 | yes | 2 | 63 |
| 28 | `CPUSkinningAlpha.sdv` | 18,356 | yes | 2 | 66 |
| 29 | `CPUSkinning.sdv` | 18,964 | yes | 2 | 66 |
| 30 | `Skinned.sdv` | 33,236 | yes | 2 | 70 |
| 31 | `DefaultMaterial.sdv` | 18,428 | yes | 2 | 63 |
| 32 | `ErrorShader.sdv` | 12,812 | yes | 1 | 59 |

That is 574,964 bytes of effect binary, 72 techniques and 1,650 top-level
parameters. 27 of the 59 `.sdv` files are not loaded before the stop.

Each of the seven filter effects is cloned onto the same device right after it is
created (`ID3DXEffect::CloneEffect` from `sub_0096EF80`, 7 calls). So 39 effect
objects exist at the stop.

## Techniques chosen

- **The filters (1-7):** the original is never used again. Six of the seven
  clones get `ValidateTechnique` and then `SetTechnique` on their only technique.
  None of them is begun.
- **The pooled effects (8-32):** the game walks every technique in order. For
  technique i it calls `ValidateTechnique(i)`, `SetTechnique(i)`, `Begin`,
  `BeginPass(0)`, `EndPass`, `End`, and then `SetTechnique(0)` again. All 65
  techniques have one pass. Every `Begin` passes flags 0, so the device state is
  saved and restored. One more `SetTechnique(0)` follows the first technique's
  walk, so an effect with n techniques gets 2n+1 `SetTechnique` calls.
- All 71 `ValidateTechnique` calls succeed.

This walk is a warm-up: each pass is applied once, so its shaders are created,
with no draw between `BeginPass` and `EndPass`.

## ID3DXEffect methods called

| Method | Calls |
|---|---|
| `GetAnnotationByName` | 883 |
| `GetParameter` | 858 |
| `GetParameterDesc` | 858 |
| `SetTechnique` | 161 |
| `GetParameterBySemantic` | 100 |
| `ValidateTechnique`, `GetTechnique`, `GetTechniqueDesc` | 71 each |
| `GetCurrentTechnique`, `Begin`, `BeginPass`, `End` | 65 each |
| `EndPass` | 64 |
| `GetDesc` | 63 |
| `GetParameterByName` | 26 |
| `CloneEffect` | 7 |
| `GetBool`, `AddRef` | 6 each |
| `SetVector`, `SetTexture`, `GetString` | 1 each |

The other D3DX calls before the stop are `D3DXCreateEffect` (32),
`D3DXCreateEffectPool` (1), `D3DXGetShaderVersion` (64),
`D3DXGetImageInfoFromFileInMemory` (1), `D3DXCreateTextureFromFileInMemoryEx` (1),
`D3DXMatrixPerspectiveFovLH` (1), `D3DXMatrixLookAtLH` (1) and the stop,
`D3DXCreateSprite`.

The game never reaches the parameter blocks (`BeginParameterBlock` and the others),
`SetStateManager`, or the base effect's `GetVertexShader`/`GetPixelShader`. All of
them still stop loudly.

The passes reach the device through its own methods. Among them:
`SetSamplerState` 1,392, `SetRenderState` 584, `SetVertexShaderConstantF` 479,
`SetTexture` 263, `SetPixelShaderConstantF` 172, `SetTextureStageState` 142,
`IDirect3DStateBlock9::Apply` 142, and `SetVertexShader`/`SetPixelShader` 71 each.

## Shaders

- **Outside effects:** none. The game makes no `D3DXAssembleShader` call and does
  not read `Data/EncryptedShaders` before the stop. It makes no
  `CreateVertexShader`/`CreatePixelShader` call of its own.
- **Inside effects:** applying the passes creates 64 vertex and 64 pixel shaders.
  They translate to 97 distinct MSL libraries:

  | Version | Libraries |
  |---|---|
  | vs_1_1 | 25 |
  | vs_2_0 | 21 |
  | ps_1_1 | 20 |
  | ps_1_4 | 4 |
  | ps_2_0 | 27 |

- After each `BeginPass`, the game reads back the pixel shader that pass set:
  `GetPixelShader` (65 calls), `IDirect3DPixelShader9::GetFunction` twice
  (128 calls: the size, then the tokens), `D3DXGetShaderVersion` (64), and
  `Release` (64). One pass sets no pixel shader.

## Compiling

- **Pipelines: none.** The game draws nothing before the stop. There is no
  `Draw*` call, so no render pipeline is built and `<support>/ShaderCache/` stays
  empty. A warm pipeline cache cannot change this boot.
- **Libraries:** the 97 MSL libraries are compiled on the background queue as the
  shaders are created. The boot does not wait for them, and only a few finish
  before it exits. To measure the compiles, a throwaway build (not kept) waited
  for every library at the stop:
  - **Cold**, with Metal's own compiler cache bypassed by a unique comment in each
    source: all 97 compile, with no failure, 0.83 s after the first request. The
    first libraries to finish took 75-95 ms each.
  - **Warm**, with the same sources as an earlier run: 0.37 s.

## The first frame

None. There is no `Present` and no `BeginScene` before the stop, so there is
nothing to read back.

## Input

- The game reads no input before the stop: no `GetDeviceData` and no
  `GetDeviceState`.
- It pumps window messages once, right after it creates its window and before
  DirectInput: `PeekMessageA` 3, `GetMessageA` 1, `TranslateMessage` 1 and
  `DispatchMessageA` 1, from `0x9B1100` and `0x9B1A50`.
- Plan 6 starts from the setup recorded in plan 5's Measured section. That setup
  is still the only DirectInput use before the stop.

## Time to the stop

About 0.3 s of wall time, headless and untraced (0.30-0.31 s over four runs, two
with a fresh support folder and two with an old one). The first run after a
relink takes about 1.2 s.

## Fixed during the bring-up

Ghost.sdv names a texture in a string annotation. The game reads it with
`GetString`, turns it into a full path with the CRT (`GetFullPathNameA`), and then
makes that path relative to its current directory. The path is used in two ways:

1. First as a loose file, through the CRT's `CreateFileA` (`0xAEF0C8`). The file
   is missing in the shipped tree, as it is on Windows.
2. Then from `Data/Everything.stuff`, through the game's streaming reader:
   - `CreateFileA` with `FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN`
     (`0x48000000`) at `0xB526E4`;
   - a sector-aligned `VirtualAlloc` buffer and `ReadFileEx` at `0xB525DE`, with
     completion routine `0xB52410`;
   - an alertable `SleepEx` (the wrapper `0x9379E0`, gated by `[0x1D6D874]`).
     `WaitForSingleObjectEx(.., INFINITE, TRUE)` at `0x8809B2` is the other
     alertable wait.

   The texture then goes to `D3DXGetImageInfoFromFileInMemory`,
   `D3DXCreateTextureFromFileInMemoryEx` and `ID3DXEffect::SetTexture`.

Two Win32 answers were wrong or missing:

- **`GetFullPathNameA`** (vendored win32hle) joined the host's working directory.
  The game then built a relative path through the Mac's folders, and the
  texture's path resolved outside the allowed folders. The texture was skipped
  without any message. It now answers in the guest's folders, as
  `GetCurrentDirectoryA` does (`third_party/pcrecomp.PATCHES`).
- **`ReadFileEx`** was unanswered once the path was right. It now reads at once,
  at the OVERLAPPED's offset, and queues the completion routine for the calling
  thread. `SleepEx` and `WaitForSingleObjectEx` with alertable TRUE run the queued
  routines and return `WAIT_IO_COMPLETION` (`runtime/bw2/shims_k32.c`). Both are
  checked in `runtime/bw2/shims_selftest.c`.

## For the next plans

- Plan 7 answered the stop. The game makes its sprite and fonts, shows its loading
  screen and stops at audio. That boot, with its first frames and the audio start for
  plan 8, is in `startup.md`. These notes are left as plan 5 found them.
- The stop is `D3DXCreateSprite`, which plan 7 owns. Plan 7 also owns the font,
  the mesh, HLSL compilation and the fragment linker. None of those has been
  reached yet.
- The first draw, the first `Present` and the first input read all come after
  that stop. The pipeline cache and the per-draw shader variants are first used
  there.
- The streaming reader above is how the game loads its assets from
  `Everything.stuff`. Only one texture went through it before the stop.
