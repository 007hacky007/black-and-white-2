"""Run tools from the vendored pcrecomp, from its own root (its imports expect that)."""
import subprocess
import sys
from pathlib import Path

from .paths import REPO

PCR = REPO / "third_party" / "pcrecomp"


def run(tool: str, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(PCR / tool), *args], cwd=PCR,
                          capture_output=True, text=True, check=check)
