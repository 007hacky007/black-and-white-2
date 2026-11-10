# Official patches 1.1 and 1.2 (phase 0 item 2)

Date: 2026-11-10.

## Sources

| Patch | Page | Direct download? |
|---|---|---|
| 1.1 | https://community.pcgamingwiki.com/files/file/2151-black-white-2-v11-patch | No. Three steps: `?do=download` shows terms; `&confirm=1&csrfKey=...` (session cookie) shows a "download begins in seconds" page; `&r=10345&confirm=1&t=1&csrfKey=...` redirects to a signed DigitalOcean Spaces URL valid 20 minutes. Works with plain HTTP and a cookie jar, no JavaScript, no login. |
| 1.2 | https://community.pcgamingwiki.com/files/file/2152-black-white-2-v12-patch | Same flow, `r=10346`. |

GamePressure and moddb also host them; not tried. The Builder needs either
the three-step PCGamingWiki flow (it clicks "Agree" to their terms on the
user's behalf, which the user should know about) or the user's local copy.

## Fingerprints

| Kind | File | Bytes | SHA-256 |
|---|---|---|---|
| patch-1.1 | `BW2Patch1_1.exe` | 111,641,849 | `dd90b3aa303f8dd7fff24e6d6afd77252a05f643f3a880770d37c62f628d3863` |
| patch-1.2 | `BW2Patch1_2.exe` | 33,336,733 | `d2d907cd742289b722e4920563a7e37b2c1f3fefdf719b1c07c48a19dd6cd9fe` |
| inner | `USA_V1_0_TO_V1_1_INSTALL.RTP` | 20,305,842 | `3d39b2e4bbfbf5a6ff0ae36374a0be0e2068c053f71f27a8d81a4bd7979351fb` |
| inner | `ALL_V1_1_TO_V1_2_INSTALL.RTP` | 21,909,803 | `77068393173d5379e181895e446666bc996c32aca38059c2aa30aebfc8299e83` |
| inner | `patchw32.dll` (same in both) | 205,824 | `b843d4f933f2b4221c8c46c2269c69feba1da2ac51738a233e646ff4168253ee` |

## Format

Both are InstallShield 11 setup launchers (`setup.exe` 11.0.0.28844, built
with an evaluation copy of InstallShield 11). After the PE image (overlay at
`0x1c200`) comes a plain list of embedded files, each as four NUL-terminated
strings (name, path such as `Disk1\data1.cab`, version, decimal size) then the
bytes. The embedded set is an ordinary InstallShield cabinet set
(`data1.hdr`, `data1.cab`, `data2.cab`, `engine32.cab`, `setup.inx`, ...).

## Unpacking on macOS

1. Split the overlay (a 20-line script; no tool needed).
2. `unshield -D 2 x` on the embedded `data1.hdr`: works, 81 files (1.1) and
   89 files (1.2), no errors.

No Windows code runs.

## What each patch changes

Two mechanisms: whole files in file groups, and an **RTPatch** binary diff
(`*.RTP`, Pocket Soft's proprietary format, applied on Windows by
`patchw32.dll` via `isrtp32.dll`).

### 1.1 (needs 1.0)

| Group (English/USA) | Destination (from setup strings) | Action |
|---|---|---|
| `USA_V1_0_TO_V1_1_INSTALL.RTP` | install root | RTPatch diff. Patches `white.exe`, `Version.txt`, `Data/VisualEffects/VisualEffectBinary.bin`, `Data/Balance/GameBalanceArmy.txt`, about 55 shaders in `Data/effects/*.sdv` (and `Data/EncryptedShaders`), and `Scripts/BW2/Land1/3/6/7/9.chl`. Directory table covers most of `Data` and `Audio`. Does **not** touch `Data/Everything.stuff` or `.lug` banks. |
| Ape_Texture_Fix | `Data/CTR/bape` | new: `t_ape_evil_scalebias.dds`, `t_ape_good_scalebias.dds` |
| Credits_English | `Data` | replace `credits.txt` |
| Text_English | `Data/Text` | replace `bw2text.bin` (2,061,373 bytes) |
| Readme_English | root | new `readme_v1_1.txt` |
| other languages | - | not used (USA English only) |

### 1.2 (needs 1.1)

| Group | Destination | Action |
|---|---|---|
| `ALL_V1_1_TO_V1_2_INSTALL.RTP` | root | RTPatch diff: `white.exe`, `Version.txt`, `country.txt`, `Data/graphics.config`, `Data/Text/VillagerNames.txt`, `Data/VisualEffects/VisualEffectBinary.bin` |
| Ape_Texture_Fix | `Data/Art/Textures` | new `battery.dds` |
| ATI_Fix | `Data/Effects` | replace `Skinned.sdv` |
| GraphicConfigUpdate | `Data` | replace `graphics.config` |
| ScriptDLL | `Plugins` | replace `ScriptLibraryR.dll` (655,360 bytes; it is a game DLL, see below) |
| Scripts | `Scripts/BW2` | replace `ChallengeBatch.chl`, `Land1..4,6..9,11.chl` |
| Credits_English, Text_English | `Data`, `Data/Text` | replace |
| Readme_English | root | new `readme_v1_2.txt` |

Group-to-folder mapping is read from the `<TARGETDIR>\...` strings in each
setup; the exact group-per-folder pairing is inferred from names and must be
checked against the RTPatch directory table when the applier exists.

`white.exe` changes in both, but we take `white.exe` from fan patch 1.42, so
only the data changes matter.

## Applying without Windows

- Whole-file groups: trivial copy.
- **RTPatch diffs: no open-source macOS applier is known.** Candidates:
  1. **RTPatchX v1.6** (CTPAX-X Team, Apache-2.0, C source, 9 KB, 2013):
     https://www.ctpax-x.org/index.php?goto=cxsoft&show=128&lang=en. Whether it
     decodes RTPatch itself or calls `patchw32.dll` is not stated; read its
     source first.
  2. Write our own RTPatch applier from the format (magic `4B 2A 20 03`,
     directory table, then per-file records with long and 8.3 names).
  3. Lift `patchw32.dll` (205 KB, 32-bit x86) with the same pipeline as the
     game and run it on macOS. Heavier, but reuses tools we build anyway.

## Open questions

- Which RTPatch route works (decide in the Builder plan; try RTPatchX first).
- `Plugins/ScriptLibraryR.dll` is a 32-bit DLL the game loads (the installed
  1.0 copy is 679,936 bytes). It must be lifted too, as a second module.
  This is new scope for the lifter plan.
- Whether the fan patch 1.42 exe actually needs every 1.1/1.2 data change
  (its page says to install both first; assume yes).
