import subprocess
from bw2 import pcr, paths


def test_game_material_is_ignored():
    for name in ["Black & White 2 (USA) (Disc 1).zip", "x.bin", "x.cue", "x.iso",
                 "white.exe", "gamefiles/install/a.txt", "out/a.c", "gen/a.c", "a.part"]:
        r = subprocess.run(["git", "check-ignore", "-q", name], cwd=paths.REPO)
        assert r.returncode == 0, name


def test_pcrecomp_runs_and_audit_is_clean():
    assert pcr.run("tools/audit_repo.py", "--selftest").returncode == 0
    r = pcr.run("tools/audit_repo.py", str(paths.REPO), check=False)
    assert r.returncode == 0, r.stdout + r.stderr
