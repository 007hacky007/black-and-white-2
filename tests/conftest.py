import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def pytest_collection_modifyitems(config, items):
    if os.environ.get("BW2_GAMEFILES") == "1":
        return
    skip = pytest.mark.skip(reason="needs real game files: set BW2_GAMEFILES=1")
    for item in items:
        if "gamefiles" in item.keywords:
            item.add_marker(skip)
