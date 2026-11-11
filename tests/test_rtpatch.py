import subprocess

import pytest

from bw2 import paths
from bw2.rtpatch import RtpError, check_run, parse_summary

# Synthetic end-of-run lines in the DLL's layout: a keyword, dots, a count in parentheses.
SUMMARY = ["   Files Modified ........ (   3)\n", "   Files Renamed. ........ (0)\n",
           "   Files Added ........... (1)\n", "   Files Deleted ......... (2)\n",
           "Skipped: New Files Up-To-Date .... (4)\n",
           "Ignored: Old Files Missing ....... (0)\n", "         Old Files Invalid ....... (0)\n"]


def test_parse_summary_counts():
    r = parse_summary(SUMMARY)
    assert (r.modified, r.renamed, r.added, r.deleted, r.skipped, r.missing, r.invalid) == \
        (3, 0, 1, 2, 4, 0, 0)


def test_summary_with_ignored_files_fails():
    lines = [l.replace("Invalid ....... (0)", "Invalid ....... (1)") for l in SUMMARY]
    events = [(7, "Data\\good.sdv"), (9, "Applying the patch\n"),
              (7, "Data\\bad.sdv"), (9, "Old file is invalid, skipped\n")] + [(9, l) for l in lines]
    with pytest.raises(RtpError, match=r"bad\.sdv"):
        check_run(events, 0)


def test_error_message_fails():
    events = [(3, "\nerror ept4.4d: "), (4, "Something went wrong\n")] + [(9, l) for l in SUMMARY]
    with pytest.raises(RtpError, match="Something went wrong"):
        check_run(events, 0)


def test_nonzero_return_fails():
    with pytest.raises(RtpError, match="returned 5"):
        check_run([(9, l) for l in SUMMARY], 5)


def test_clean_run_passes():
    r = check_run([(7, "a"), (7, "b")] + [(9, l) for l in SUMMARY], 0)
    assert r.files == ["a", "b"] and r.modified == 3


@pytest.fixture(scope="module")
def patched_11(tmp_path_factory):
    from bw2.patches import patch_inputs
    from bw2.rtpatch import apply_rtp
    work = tmp_path_factory.mktemp("rtp")
    tree = work / "tree"
    subprocess.run(["/bin/cp", "-cR", str(paths.GAMEFILES / "install"), str(tree)], check=True)
    pi = patch_inputs(paths.GAMEFILES / "patches" / "BW2Patch1_1.exe", "patch-1.1",
                      paths.GAMEFILES / "patches" / "inputs-1_1")
    return tree, apply_rtp(pi.dll, pi.rtp, tree, log=work / "rtp-1.1.log")


@pytest.mark.gamefiles
def test_apply_rtp_real_1_1(patched_11):
    _, r = patched_11
    assert r.modified == 69 and r.missing == r.invalid == 0
    print(f"rtp 1.1: {r.seconds} s")


@pytest.mark.gamefiles
def test_apply_rtp_real_1_2(patched_11, tmp_path):
    from bw2.patches import patch_inputs
    from bw2.rtpatch import apply_rtp
    tree, _ = patched_11
    pi = patch_inputs(paths.GAMEFILES / "patches" / "BW2Patch1_2.exe", "patch-1.2",
                      paths.GAMEFILES / "patches" / "inputs-1_2")
    r = apply_rtp(pi.dll, pi.rtp, tree, log=tmp_path / "rtp-1.2.log")
    assert r.modified == 8 and r.added == 1 and r.missing == r.invalid == 0
    print(f"rtp 1.2: {r.seconds} s")
