# Patch application (plan 2)

Measured 2026-11-11 on an Apple M1 Pro. `python -m bw2.gametree` turns the
extracted 1.0 install (`gamefiles/install`, 773 files) into the game data tree
(`gamefiles/game`, 778 files). It applies four layers in order:

1. Official 1.1.
2. Official 1.2.
3. The fan patch 1.42 `white.exe`.
4. A check against `bw2/manifests/game-usa-1.2-fan1.42.tsv`.

## Route

RTPatch is Pocket Soft's proprietary binary diff format, and no open decoder
exists:

- RTPatchX ships a `PATCHW32.def` import file and reports patchw32's own
  error codes. It is a wrapper around `patchw32.dll`.
- aluigi's myrtp also loads `patchw32.dll`.

So the Builder runs the `patchw32.dll` that ships inside each patch, as is
(205,824 bytes, sha `b843d4f9...`, identical in both patches), in Unicorn. A
small Python stand-in handles the Win32 calls it makes. This was the user's
decision, made after the spike below.

| Module | Role |
|---|---|
| `bw2/pe32emu.py` | PE32 loader, flat GDT with `fs` pointing at a TEB, stack, a 64 KB-aligned heap, import stubs (stdcall and cdecl), callbacks, and SEH dispatch (`RaiseException`, `RtlUnwind`) as a state machine over a return stub |
| `bw2/w32files.py` | File, directory, search and file-mapping calls. The RTP's folder is `P:` (read-only) and the tree is `T:` (writable). Names resolve case-insensitively, and anything outside the roots is refused and logged |
| `bw2/w32base.py` | Start-up, code pages, `wsprintf`, time, resources, and polite failures for the NT security calls |
| `bw2/rtpatch.py` | Calls `DllMain`, then `RTPatchApply32@12("P:\<rtp>" T:\, callback, 1)`. It reads the DLL's callback stream and fails on any error, a non-zero return, or an old file reported missing or invalid |

An import with no handler stops the run with its name. Of the DLL's 173
imports, 87 are implemented. The other 86 (registry, DDE, INI files,
process creation and more) stay unimplemented on purpose.

## What the spike and the first real runs found

- **Writable file mappings.** The DLL builds each new file in a read-write
  mapping that is larger than the empty file, and it closes the mapping
  without calling `UnmapViewOfFile`. A view sized by the current file length,
  or flushed only on unmap, produced "Verifying - Failed!" on every file.
  Views are now sized by the mapping and written back when the mapping or the
  file handle closes.
- **`VirtualAlloc` alignment.** The DLL's C runtime heap expects 64 KB-aligned
  reservations, and commits that round down to the page.
- **SEH.** `RTPatchApply32` leaves through a non-continuable `RaiseException`
  that its own frame catches.
- **Imports the spike never reached.** Real `P:`/`T:` paths and the full run
  reached `GetTimeZoneInformation` (answered as UTC), `GetDriveTypeW`,
  `GetShortPathNameA/W` and `ReleaseMutex`. All four were added test-first
  after the loud stop.
- **Padded counts.** The DLL pads the numbers in its summary, as in `(  69)`.

## Group destinations

The destinations come from the `<TARGETDIR>\...` strings in each setup's
`data1.hdr`. They are recorded in `bw2/gametree.py` `GROUPS`.

| Patch | Group | Destination |
|---|---|---|
| 1.1 | Credits_English | `Data` |
| 1.1 | Text_English | `Data/Text` |
| 1.1 | Ape_Texture_Fix | `Data/CTR/bape` |
| 1.1 | Readme_English | root |
| 1.2 | Credits_English | `Data` |
| 1.2 | Text_English | `Data/Text` |
| 1.2 | Ape_Texture_Fix | `Data/Art/Textures` |
| 1.2 | Readme_English | root |
| 1.2 | ScriptDLL | `Plugins` |
| 1.2 | Scripts | `Scripts/BW2` |
| 1.2 | ATI_Fix | `Data/Effects` |
| 1.2 | GraphicConfigUpdate | `Data` |

Existing folders and files keep their on-disk spelling (`Data/ctr`,
`Data/Art/textures`).

A group file either replaces a file of the same name or is one of the five
new files phase 0 listed. Anything else stops the build.

## Result

| Step | Result | Time |
|---|---|---|
| 1.1 RTPatch | 69 modified, none missing or invalid, every file verified by the DLL | 103.5 s |
| 1.1 groups | 5 files, 3 new (two ape textures, readme) | |
| 1.2 RTPatch | 8 modified, 1 ADD record (`country.txt`, which already exists, so it is overwritten), none missing or invalid | 77.2 s |
| 1.2 groups | 17 files, 2 new (`battery.dds`, readme) | |
| Fan 1.42 `white.exe` | replaces the patched exe (sha `ca745211...`) | |
| Whole build | 778 files: 83 differ from 1.0, 5 are new | 184.5 s wall, 413 MB peak |

## Verification

Without a Windows reference, correctness rests on a chain of checks. Each
link is anchored either in a public reference or in the patch itself:

1. **The discs** match the redump hashes in `bw2/known_releases.tsv`, which
   are an independent public reference.
2. **The 1.0 install** extracted from them must match
   `bw2/manifests/install-usa.tsv` (773 files) before any patch is applied.
   The build stops on any changed, missing or extra file, which takes about
   2 s. The manifest comes from our own extraction, but that extraction is
   a deterministic unpack of the redump-verified discs.
3. **The patch installers, `patchw32.dll`, both `.RTP` files and the fan
   patch** each match a known size and SHA-256.
4. **RTPatch's own verification of every file it writes.** It reports
   "Verifying - Passed" for all 78 patched files. It also refuses old files
   that are not the expected 1.0 bytes. The applier treats a missing or
   invalid old file as an error.
5. **The committed manifest of the finished tree** (paths, sizes, SHA-256).
   Every later build must match it exactly, or it is not renamed into place.
   It was written from our first build, so on its own it only catches drift.
   What vouches for that first build is links 1-4: the 78 files RTPatch
   wrote were verified against checksums inside the diffs. The 22 group
   files and the fan `white.exe` were copied byte for byte from fingerprinted
   installers. Their destinations come from the setups' own `data1.hdr`
   (table above).
