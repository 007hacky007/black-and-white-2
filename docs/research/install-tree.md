# Install tree from the USA discs (phase 0 items 1 and 8)

Date: 2026-11-10. Inputs: the four Redump USA images, fingerprints in
`bw2/known_releases.tsv`. Code: `bw2/disc.py`, `bw2/install.py`.

## What is on each disc

Each disc is one MODE1/2352 data track. Read through the ISO 9660 primary
volume descriptor (8.3 names; Joliet not read).

| Disc | InstallShield files | Other |
|---|---|---|
| 1 | `DATA1.HDR` 106,167; `DATA1.CAB` 23,104,445; `DATA2.CAB` 538,247,168; `LAYOUT.BIN`, `SETUP.INX`, `SETUP.IBT`, `SETUP.INI`, `SETUP.EXE`, `SETUP1.MHS` | `00000001.TMP`, `00000002.TMP` (SafeDisc markers), `ARUN.EXE`, `AUTORUN.INF`, `DIRECTX9/`, `ENGINE32.CAB`, `SETUP/README/`, `SUPPORT/` (EA help, launcher) |
| 2 | `DATA3.CAB` 679,802,880; `SETUP2.MHS` | `AUTORUN.INF`, `LH.ICO`, `SUPPRE~0.EXE` |
| 3 | `DATA4.CAB` 679,802,880; `SETUP3.MHS` | same as disc 2 |
| 4 | `DATA5.CAB` 566,861,579; `SETUP4.MHS` | same as disc 2 |

So it is one InstallShield 9 set (header version `0x01009500`) split into
five cabinets over four discs. Copying `data1.hdr` and `data1..5.cab` into
one directory is enough for an extractor.

## Extractor choice (item 8)

| | unshield 1.6.2 (MIT, Homebrew) | pcrecomp `tools/assets/isextract.py` |
|---|---|---|
| Lists `white.exe` at 21,739,061 bytes | yes (`White\white.exe`) | yes (index 893, no directory) |
| Files extracted | 844 of 844 | 841 + 7 loose, 44 skipped as invalid |
| Errors | none (MD5 checked) | 3: `Data/Everything.stuff`, `Audio/Music/Dynamic Music/project.bin`, `Audio/SFX/game/Ingame.lug` ("Unexpected end of cab reading chunk data", i.e. files spanning two cabinets) |
| Time on M1-class Mac | about 14 s | about 11 s |

**Chosen: unshield.** `isextract.py` cannot read files that cross a cabinet
boundary, and one of them is the 1.4 GB `Everything.stuff`. Fixing that
upstream is possible later; it is not needed now.

## Layout

unshield (`-D 2`) writes one directory per InstallShield file group. The
installer script maps groups to folders under the install directory; we do
not run it, so `bw2/install.py` maps them with a fixed table. Destinations
other than `.` are inferred from paths that `white.exe` opens, and from
sibling files already in `MainApp`:

| Group | Destination | Evidence |
|---|---|---|
| MainApp | `.` | holds `Data/`, `Audio/`, `Scripts/`, `binkw32.dll` |
| White | `.` | `white.exe` beside `binkw32.dll` |
| SplashEnglish | `Data/Splash` | exe opens `data/splash/Splash.tga`, `data/splash/%s.bik` |
| CreditsEnglish | `Data` | exe opens `data\credits.txt` |
| Infocdat | `Scripts` | exe opens `scripts\info.cdat` |
| SpeechEnglish | `Audio/Dialogue` | `Speech.lug` beside `VillagerAnims.lug`; exe scans `%s\*.lug` |
| MusicEnglish | `Audio/Music/Cutscene` | `nutoilsongchorus1..5.ogg` beside `nutoilsong.ogg` |
| TextEnglish | `Data/Text` | `bw2text.bin` beside `VillagerNames.txt`; exe opens `.\Data\Text\BW2Text.lhts` (name differs, **unverified**) |
| ReadmeEnglish | `.` | readme |
| `_Engine_*`, `_Support_*` (12 groups) | not installed | InstallShield engine and EA registration support |

Unknown groups are an error (`UnknownGroup`), so a different release fails
loudly instead of installing half a game.

**To verify when the game first runs:** the TextEnglish, SpeechEnglish and
MusicEnglish destinations, and whether the game expects files the installer
would generate (registry keys, `.ini` files) rather than copy.

## Result

- 773 files, 3,223,196,155 bytes, manifest in `bw2/manifests/install-usa.tsv`
  (path, size, SHA-256; no content).
- Largest parts: `Data/Everything.stuff` (1.43 GB, one file), `Audio/Music`
  (283 files, 646 MB), `Audio/SFX` (376 MB), `Audio/Dialogue` (309 MB),
  `Data/landscape` (216 MB).
- SafeDisc pieces the installer copies: `DrvMgt.dll`, `SECDRV.SYS`,
  `00000000.256`. The port does not need them.
- `Data/EncryptedShaders/` (36 files, 76 KB) is relevant to item 7.
- Extraction from bins to tree takes about 18 s.
