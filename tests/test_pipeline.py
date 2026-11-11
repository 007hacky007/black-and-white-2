from bw2.pipeline import parse_args


def test_opt_level_accepted_without_dash():
    # argparse takes "--opt -O3" for two options; "O3" and "--opt=-O3" both work.
    assert parse_args(["--opt", "O3"]).opt == "-O3"
    assert parse_args(["--opt=-O1", "--build-only"]).opt == "-O1"
    assert parse_args([]).opt == "-O3"


def test_chosen_opt_level_is_the_default():
    # Task 10 measured -O3 at 1.10x -O2 and chose it (docs/research/build-pipeline.md).
    import inspect
    from bw2.build import build
    from bw2.pipeline import run_pipeline
    assert inspect.signature(build).parameters["opt"].default == "-O3"
    assert inspect.signature(run_pipeline).parameters["opt"].default == "-O3"


def test_every_module_lifted_with_computed_returns(tmp_path, monkeypatch):
    # A ret that finds an address the code pushed goes there, as on the CPU: the VM calls into
    # game code that returns into a VM re-entry stub (final review, plan 3).
    from bw2 import pipeline
    seen = {}
    monkeypatch.setattr(pipeline, "modules", lambda: [(n, tmp_path / "x.exe", tmp_path / "c.json", (".text",))
                                                       for n in ("white", "vm", "slr")])
    monkeypatch.setattr(pipeline, "vm_entries", lambda exe: [])
    monkeypatch.setattr(pipeline, "lift_module", lambda name, *a, **k: seen.setdefault(name, k) and None
                        or pipeline.ModuleLift(name, 0, 0, 0, {}, [], [], 0.0))
    pipeline.lift_all(tmp_path)
    assert {n: k.get("computed_ret") for n, k in seen.items()} == {"white": True, "vm": True, "slr": True}
