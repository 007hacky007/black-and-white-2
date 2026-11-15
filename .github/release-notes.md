Builder for the Black & White 2 macOS port (Apple Silicon, macOS 13 or newer).

**New in 0.0.3:**

- The interactive Lionhead logo is back at startup: drag the falling cubes around with the left mouse button. The scene ends by itself and hands over to the main menu. It is the game's own scene, run from your copy.
- Releases are now published as full releases.

This release contains no game files. `BW2 Builder.app` turns your own copy of the game into `Black & White 2.app`:

- the four disc images of Black & White 2 (zips, `.bin`/`.cue` or `.iso`, or one folder holding them),
- the official patches 1.1 and 1.2, and the fan patch 1.42.

It needs the Xcode Command Line Tools (the Builder offers to install them), an internet connection for its pinned Python and packages, and about 20 GB free on the startup disk while it builds.

**Opening it:** download `BW2-Builder.dmg` and open it. The Builder is ad-hoc signed, not notarized, so macOS blocks it the first time; the disk image's window shows the steps: open BW2 Builder, click Done, then System Settings > Privacy & Security > "Open Anyway". On macOS 14 or older, right-click BW2 Builder and choose Open. Or, before opening the image:

    xattr -d com.apple.quarantine ~/Downloads/BW2-Builder.dmg

This is an early test release: the first land, saves, loads and settings work; the rest of the campaign is still being tested.
