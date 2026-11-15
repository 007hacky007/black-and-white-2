#!/bin/sh
# Rebuild the committed PNG with librsvg (brew install librsvg).
set -eu
cd "$(dirname "$0")"
rsvg-convert --width 1024 --height 1024 --output AppIcon-1024.png AppIcon.svg
