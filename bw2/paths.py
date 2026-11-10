"""Where things live: the repo, the git-ignored working area, the discs."""
import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GAMEFILES = REPO / "gamefiles"


def discs_dir() -> Path:
    """Folder holding the user's disc images (env BW2_DISCS, default the repo root)."""
    return Path(os.environ.get("BW2_DISCS", REPO))
