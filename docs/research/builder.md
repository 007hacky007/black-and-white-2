# The Builder and the packaged app (plan 13)

Facts measured while plan 13 was built (2026-11-14 and 2026-11-15) on an Apple
Silicon Mac with 10 cores. The Mac was busy for nearly every timing below
(load averages are given with each), so every time here is indicative only and
none is a performance number; no bound is set from them.

## The fan patch without innoextract (Task 1)

`B&W2 Fan Patch v1.42 Installer.exe`, 20,266,576 bytes, is pinned by SHA-256
(`fanpatch-installer` row) and checked before any offset is used:

- `Inno Setup Setup Data (6.0.0) (u)` at file offset 751,376; one file-data
  chunk, magic `zlb\x1a` at 876,032 (`FAN_CHUNK`).
- After the magic: one LZMA2 dictionary-size byte, then a raw LZMA2 stream
  (Python `lzma`, `FORMAT_RAW`, `FILTER_LZMA2`). It decodes to 38,054,164
  bytes, the stream ends, and 1,109,041 bytes follow it.
- The only `MZ` header is at 17,226,004 (`FAN_EXE_OFFSET`); the exe runs to
  the end, 20,828,160 bytes (`FAN_EXE_SIZE`).
- As stored its SHA-256 is `c73141fd...3144`; after Inno's call-instruction
  transform (per 64 KiB block, `E8`/`E9` operands with a `00`/`FF` high byte)
  is undone it is the `white-exe` release (`ca745211...db2`).
- `test_read_fan_exe`: 0.98 s for the decode, the transform and the hashes.
- Errors name the file and the stage: installer, magic, stream or exe.

## unshield from vendored source (Task 2)

- Source: `https://github.com/twogood/unshield/archive/refs/tags/1.6.2.tar.gz`,
  42,833 bytes, SHA-256 `a937ef59...1a2f`, MIT. 17 files are vendored
  byte-identical under `third_party/unshield/`, each listed with its SHA-256
  in `third_party/unshield.SOURCE`.
- Our build files: `tools/unshield/lib/unshield_config.h` (the `HAVE_*`
  headers, `SIZE_FORMAT "zi"`, `HAVE_FNMATCH`, `HAVE_ICONV`,
  `USE_OUR_OWN_MD5` as `0`, which CMake's `#cmakedefine01` gives with the
  option off) and `tools/unshield/openssl/evp.h` (the six EVP MD5 calls over
  CommonCrypto). With `USE_OUR_OWN_MD5` 0, `lib/file.c:922` skips
  `EVP_MD_CTX_free`: about 100 bytes per file stay allocated, as in a CMake
  build.
- One clang call compiles and links:
  `-O2 -mcpu=apple-m1 -mmacosx-version-min=13.0 -isysroot <SDK> -DHAVE_CONFIG_H`
  with `-I tools/unshield -I third_party/unshield -I third_party/unshield/lib`
  (`lib/converter.c` includes `<converter.h>`), `-lz -liconv`. One upstream
  warning (`helper.c:247`, `-Wpointer-sign`). About 0.9 s.
- The binary links only `/usr/lib/libz.1`, `libiconv.2` and `libSystem.B`.
  Its test mode checked the MD5 of every file of the install set under
  `PATH=/usr/bin:/bin` with no failure.

## The LGPL-only FFmpeg (Task 3)

- `https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz`, 12,040,788 bytes,
  SHA-256 `8c385028...e002e` (`ffmpeg-source` row).
- configure flags: `--install-name-dir=@rpath --enable-shared --disable-static
  --disable-programs --disable-doc --disable-everything --disable-autodetect
  --disable-network --disable-avdevice --disable-avfilter --disable-swscale
  --disable-swresample --enable-demuxer=bink
  --enable-decoder=bink,binkaudio_dct,binkaudio_rdft --arch=aarch64
  --target-os=darwin`, the absolute `--cc`, and `-mcpu=apple-m1
  -mmacosx-version-min=13.0 -isysroot <SDK>` in the extra flags; no GPL,
  nonfree or version3 option. Each library reports `LGPL version 2.1 or
  later`.
- The video decoder registers as `binkvideo` (`bink` is configure's component
  name). configure always links CoreFoundation, CoreVideo and CoreMedia
  (`check_apple_framework`); no `--disable` flag removes them.
- First build 34.1 s (unpack 2.1 s, configure 26.9 s, `make -j10` 4.2 s,
  install 0.4 s), download 1.1 s, load 17.1 of 10 cores at the start. A second
  build under a path with spaces: 25.3 s.
- Sizes: `libavformat.63.1.102.dylib` 260,696 bytes,
  `libavcodec.63.1.102.dylib` 392,392, `libavutil.61.1.102.dylib` 719,352.

## The app bundle (Tasks 4 and 5)

    Black & White 2.app/Contents/
      MacOS/bw2                 the engine, CFBundleExecutable (no launcher process)
      Resources/d3dx9_25.dll    the lifted DLL
      Resources/game/           the game tree, APFS-cloned, checked against its manifest
      Frameworks/<install name> libavformat.63, libavcodec.63, libavutil.61 (regular files)
      Info.plist                local.bw2-port.game, BW2BuilderVersion

- The DLL is in `Resources` because `codesign --force --sign -` refuses it in
  `MacOS` ("code object is not signed at all", "In subcomponent:
  .../Contents/MacOS/d3dx9_25.dll"). In `Resources` signing passed.
- The engine's only `LC_RPATH` is `@executable_path/../Frameworks`; each
  Frameworks file is a copy of the real library under its install name (the
  name `libavformat.63.dylib` is a symlink in FFmpeg's layout).
- `/bin/cp -c -R` cloned the 3.1 GB game tree in 0.13 s. The shell's `cp` is
  GNU's and has no `-c`.
- The log: in a bundle the engine's output goes through a pipe to a watcher
  (the same binary, `bw2 --log-watcher LOG`), which writes
  `<support>/Logs/bw2.log` (the earlier one becomes `bw2.previous.log`). The
  decisive line is the last line starting with `[bw2]`, `[win32hle]`, `bw2:` or
  `TRIPWIRE:` (or a run-in `[bw2] ` / `[win32hle] ` inside a line). The end is
  clean only for `[win32hle] ExitProcess(0)`, `[bw2] quit ...` or
  `[bw2] script: quit ...`; anything else, or no decisive line, shows the alert
  `Black & White 2 stopped` with that line and the log's place.
- A bundle boot of the real game with no arguments from `/` reached the
  300-read quit in 78 log lines.
- Packaging (load 8.3 to 9.7 of 10): `package_app` of the game with the LGPL
  engine 20.3 s; `sign_app` again 3.2 and 3.3 s; `verify_app` 1.3 and 1.1 s;
  `verify_manifest` alone 2.2 s (warm cache). Sizes (`du -sk`): the app
  3,292,684 KB, `MacOS` 139,448 KB (`bw2` 142,791,328 bytes),
  `Resources/game` 3,149,384 KB, `Frameworks` 1,340 KB.

## The build in one command (Task 6)

`python -m bw2.builder_cli build`, on Task 6's sources from the four zips and
the three local patches, with `PATH=/usr/bin:/bin:/usr/sbin:/sbin` and
`DEVELOPER_DIR=/Library/Developer/CommandLineTools` (load 3.8 at the start, 74
to 85 at the end):

| Step | Wall time | Weight |
|---|---|---|
| tools (unshield 0.9 s, FFmpeg download and build) | 32.6 s | 6 |
| discs (four zips unpacked and hashed) | 12.6 s | 2 |
| install | 17.9 s | 3 |
| patches (1.1, 1.2 RTPatch 76.0 s, fan exe, DLL) | 160.1 s | 29 |
| lift (white, vm, slr 4.7 s, d3dx 30.0 s; 55,152 functions, 9 unresolved) | 125.3 s | 23 |
| compile (208 of 208 objects, Command Line Tools clang, binary 143.6 MB) | 185.5 s | 33 |
| package | 21.8 s | 4 |
| all | 555.9 s | 100 |

- Space: the workspace's peak `du -sk` was 16,345,136 KB while the app was
  made (`du` counts the app's clone of the game in full); the volume's free
  space fell by 10,604,712 KB at most, other processes included.
  `REQUIRED_BYTES` is that fall plus 20%, rounded up: 13,100,000,000. (It was
  first the `du` peak plus 20%, 20,100,000,000; the final review found that
  `du` counts the APFS clones, install to game and game to app, in full, and
  the workspace is always on the APFS home volume.)

## The disc fingerprints

Each zip holds a deflated `.bin` and a 96-byte `.cue`; the central directory
gives the bin's size and CRC-32 without unpacking (`bw2/manifests/disc-zips.tsv`).
The ISO form (2048-byte user data of every 2352-byte sector) is computed from
the bins; no ISO from a ripping tool was compared.

| Disc | Bin bytes | CRC-32 | ISO bytes | ISO SHA-256 |
|---|---|---|---|---|
| 1 | 788,886,672 | `3fcb89a3` | 686,921,728 | `47f9bea2...fa17` |
| 2 | 782,552,736 | `f8761e77` | 681,406,464 | `65e33e5e...89ad` |
| 3 | 782,552,736 | `37458542` | 681,406,464 | `b5731d13...30c0` |
| 4 | 652,846,992 | `f13bd73b` | 568,465,408 | `dc165230...979e` |

## BW2 Builder.app (Task 8)

- `python -m bw2.make_builder`: `swift build -c release` (Xcode's Swift
  6.4.2), the sources copied by `copy_sources` (415 files and `SOURCES.txt`, 9,180 KB), ad-hoc
  signed and verified; the whole make took about 15 s with a warm build cache.
  The app is 10,176 KB (`du -sk`), its `BW2Builder` executable 886,992 bytes.
- `BW2Builder --support DIR --print-checks` on a fresh support folder prints
  nine rows: Mac and Command Line Tools ready, build tools, discs and the three
  patches blocked, and writes nothing (the support folder is not created).
- The window's calls end to end (`EndToEndTests.testBuildFromWindowCalls`,
  2026-11-15; load 4.8 to 133.9 of 10, so no time here is a measurement): the
  workspace prepared in 0.1 s; the pinned Python downloaded, checked and
  unpacked in 1.8 s; `setup` (venv and the three wheels) 3.0 s; the rows read
  ready in 0.4 s; the build 538.5 s (tools 27.7, discs 12.0, install 19.2,
  patches 179.2, lift 108.0, compile 170.7, package 21.6); install 0.0 s (a
  rename on the same volume); clean 2.0 s; `codesign --verify --deep --strict`
  of the installed app 1.5 s. 548.7 s in all.
- After `clean`, the support folder holds 169,181,184 bytes (the private
  Python, the venv, the workspace's sources and `out/tools/`). The installed
  app is 3,372,605,440 bytes allocated.
- The first two runs found two faults, both fixed: `builder_cli setup`
  imported the build's packages before installing them (the bare private
  Python stopped with `No module named 'pefile'`), and the free-space row used
  "available for important usage" (220 GB, purgeable space included) where the
  build's own check uses statfs (19.4 GB), so the row was green while the build
  refused at once. The row now uses statfs too.
- This Mac had 19.4 GB free (statfs; the rest was held by local Time Machine
  snapshots), short of the 20.1 GB then required, so the run lowered the need with
  the test switch `BW2_REQUIRED_BYTES=12500000000` (Task 6's largest drop of
  free space, 10.6 GB, plus about 18%). Free space fell from 19,139,856 KB to
  12,735,932 KB at its lowest (sampled every 15 s; snapshot thinning moves it
  too). The window never sets the switch.
- The full suite with `BW2_GAMEFILES=1` after Task 8 (2026-11-15): 715
  passed, 9 skipped (long and `builder_e2e` tests), 30 min 38 s wall; load 21.8
  of 10 at the start (dotnet at 202% CPU) and 42.7 at the end, so the wall time
  is not a measurement.

## For the next plan

Performance is next (the user's order, 2026-11-14), then gameplay faults from
the user's own reports. Frame-rate, frame-time and performance numbers are
taken only on a quiet Mac: before measuring, `sysctl -n hw.ncpu vm.loadavg`
and `ps -Ao pcpu,comm -r | head -6`; measure only when the 1-minute load is
below 25% of the cores and no other process uses more than 50% CPU, else wait
(at most an hour) or record "not measured". None of the times above qualifies,
and none sets a bound. The window of `BW2 Builder.app` was never clicked by an
agent; the user's first use of it is its acceptance check.
