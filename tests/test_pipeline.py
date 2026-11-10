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
