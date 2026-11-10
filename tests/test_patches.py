import pytest

from bw2 import paths
from bw2.patches import script_dll
from bw2.releases import identify, load_releases


@pytest.mark.gamefiles
def test_script_dll_from_patch12():
    dll = script_dll(paths.GAMEFILES / "patches" / "BW2Patch1_2.exe", paths.GAMEFILES / "dll")
    assert dll == paths.GAMEFILES / "dll" / "ScriptLibraryR.dll"
    assert dll.stat().st_size == 655360
    assert identify(dll, kind="script-dll") is not None
