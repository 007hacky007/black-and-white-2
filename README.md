# Black & White 2 on Apple Silicon

A native ARM64 macOS port of Black & White 2 (Lionhead, 2005) by static
recompilation: the game's x86 executable is lifted to C ahead of time and
compiled for Apple Silicon, with our own Mac code answering its Windows,
Direct3D 9, D3DX, DirectInput, OpenAL and Bink calls. No Wine, no emulator.

Bring your own discs. This repository contains no game code, no game assets,
no patches and no C generated from the game. Built on the MIT-licensed
[pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain, vendored under
`third_party/pcrecomp/`.

Tests: `python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`,
then `.venv/bin/python -m pytest`. Tests that need the real discs and patches
run only with `BW2_GAMEFILES=1`.
