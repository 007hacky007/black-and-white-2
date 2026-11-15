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

## The Builder

`BW2 Builder.app` turns your own four disc images and the three patches
(official 1.1 and 1.2, fan patch 1.42) into `Black & White 2.app`. It needs an
Apple Silicon Mac with macOS 13 or newer, the Xcode Command Line Tools (the
Builder offers to install them), an internet connection for its pinned Python
and packages, and about 20 GB free on the startup disk while it builds.

Making it (needs Xcode or the Command Line Tools for `swift`):

    .venv/bin/python -m bw2.make_builder          # out/BW2 Builder.app

Using it: open `BW2 Builder.app` and work down the rows until each is green.
Set Up fetches the private Python; Choose... takes the disc zips, `.bin` and
`.cue` files or `.iso` files, or one folder holding them; each patch row
downloads its file (for patches 1.1 and 1.2 that accepts PCGamingWiki's
download terms for you) or takes the one you have with I have it.... Build runs
for about 10 to 15 minutes, puts `Black & White 2.app` into `/Applications`
(or the folder you chose), removes its build files and offers Play. Its work
folder is `~/Library/Application Support/BW2 Builder`; saves, settings and the
game's log live in `~/Library/Application Support/Black & White 2`, which a
rebuild never touches.

Without the window, the same build is
`python -m bw2.builder_cli build --input <discs and patches> --builder-version <v>`
in a copy of the sources, and `BW2Builder --print-checks` prints the rows.
