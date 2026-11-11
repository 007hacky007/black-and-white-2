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


def test_patch_kinds_known():
    rows = {r.kind: r for r in load_releases()}
    assert (rows["patchw32"].size, rows["patchw32"].sha256) == (
        205824, "b843d4f933f2b4221c8c46c2269c69feba1da2ac51738a233e646ff4168253ee")
    assert (rows["rtp-1.1"].name, rows["rtp-1.1"].size, rows["rtp-1.1"].sha256) == (
        "USA_V1_0_TO_V1_1_INSTALL.RTP", 20305842,
        "3d39b2e4bbfbf5a6ff0ae36374a0be0e2068c053f71f27a8d81a4bd7979351fb")
    assert (rows["rtp-1.2"].name, rows["rtp-1.2"].size, rows["rtp-1.2"].sha256) == (
        "ALL_V1_1_TO_V1_2_INSTALL.RTP", 21909803,
        "77068393173d5379e181895e446666bc996c32aca38059c2aa30aebfc8299e83")


@pytest.mark.gamefiles
@pytest.mark.parametrize("v", ["1.1", "1.2"])
def test_patch_inputs_real(v, tmp_path):
    from bw2.patches import patch_inputs
    exe = paths.GAMEFILES / "patches" / f"BW2Patch{v.replace('.', '_')}.exe"
    pi = patch_inputs(exe, f"patch-{v}", tmp_path / "x")
    assert identify(pi.dll, kind="patchw32") is not None
    assert identify(pi.rtp, kind=f"rtp-{v}") is not None
    assert pi.groups.is_dir()
