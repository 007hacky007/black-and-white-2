First public build of the Builder for the Black & White 2 macOS port (Apple Silicon, macOS 13 or newer).

This release contains no game files. `BW2 Builder.app` turns your own copy of the game into `Black & White 2.app`:

- the four disc images of Black & White 2 (zips, `.bin`/`.cue` or `.iso`, or one folder holding them),
- the official patches 1.1 and 1.2, and the fan patch 1.42.

It needs the Xcode Command Line Tools (the Builder offers to install them), an internet connection for its pinned Python and packages, and about 20 GB free on the startup disk while it builds.

**Opening it:** the Builder is ad-hoc signed, not notarized, so macOS blocks it the first time. Either open it once, then allow it under System Settings > Privacy & Security > "Open Anyway", or run:

    xattr -dr com.apple.quarantine "/path/to/BW2 Builder.app"

This is an early test release: the first land, saves, loads and settings work; the rest of the campaign is still being tested.
