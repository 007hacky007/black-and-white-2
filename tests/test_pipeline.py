from bw2.pipeline import parse_args


def test_opt_level_accepted_without_dash():
    # argparse takes "--opt -O3" for two options; "O3" and "--opt=-O3" both work.
    assert parse_args(["--opt", "O3"]).opt == "-O3"
    assert parse_args(["--opt=-O1", "--build-only"]).opt == "-O1"
    assert parse_args([]).opt == "-O2"
