#!/bin/sh
# Builds out/winprobe/saveprobe.exe (a 32-bit Windows console program) with
# mingw-w64 (Homebrew's mingw-w64). The .exe is never committed.
set -eu
cd "$(dirname "$0")/../.."
mkdir -p out/winprobe
i686-w64-mingw32-gcc -O1 -Wall -static -o out/winprobe/saveprobe.exe tools/winprobe/saveprobe.c -lole32 -lshell32
echo "built out/winprobe/saveprobe.exe"
