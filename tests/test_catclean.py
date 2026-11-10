from bw2.catclean import clean_functions

BASE = 0x1000
PUSH = b"\x68\xbc\x09\x00\x00"          # push 0x9bc
CALL = b"\xe8\x00\x00\x00\x00"          # call next


def _reader(blob):
    def read(va, k):
        off = va - BASE
        return blob[off:off + k] if 0 <= off < len(blob) else None
    return read


def _fn(addr, end, calls=()):
    return {"address": addr, "end": end, "size": end - addr, "entry_kind": "start",
            "calls_to": list(calls)}


def _run(blob, fns, seeds=(), weak=()):
    return clean_functions(fns, _reader(blob), BASE, BASE + len(blob), seeds=set(seeds),
                           weak_seeds=set(weak))


def _by(fns):
    return {f["address"]: f for f in fns}


def test_mid_instruction_entry_dropped_and_extent_restored():
    blob = PUSH * 3 + b"\xc3"
    fns = [_fn(BASE, BASE + 2), _fn(BASE + 2, BASE + len(blob))]
    out = _run(blob, fns)
    assert list(_by(out)) == [BASE]
    assert _by(out)[BASE]["end"] == BASE + len(blob)


def test_fallthrough_label_past_long_straight_line_becomes_alias():
    # white.exe 0xB84D70: ~500 KB of push/call with no branch, and .data dwords
    # that happen to equal addresses inside it. Each one lifted as its own copy.
    n = 20000
    blob = PUSH * n + b"\xc3"
    label = BASE + 5 * 15000
    fns = [_fn(BASE, label), _fn(label, BASE + len(blob))]
    out = _by(_run(blob, fns))
    assert out[label]["entry_kind"] == "alias"
    assert out[BASE]["entry_kind"] == "start"
    assert out[BASE]["end"] == BASE + len(blob)


def test_called_label_stays_start():
    blob = PUSH * 4 + b"\xc3"
    label = BASE + 10
    fns = [_fn(BASE, label, calls=[label]), _fn(label, BASE + len(blob))]
    assert _by(_run(blob, fns))[label]["entry_kind"] == "start"


def test_seeded_label_stays_start():
    blob = PUSH * 4 + b"\xc3"
    label = BASE + 10
    fns = [_fn(BASE, label), _fn(label, BASE + len(blob))]
    assert _by(_run(blob, fns, seeds=[label]))[label]["entry_kind"] == "start"


def test_entry_after_ret_call_or_padding_stays_start():
    a = PUSH + b"\xc3"                   # ends in ret
    b = PUSH + CALL                      # ends in a call that may not return
    c = PUSH + b"\x90" * 3 + b"\xc3"     # nop padding before d
    blob = a + b + c + PUSH + b"\xc3"
    s_b, s_c = BASE + len(a), BASE + len(a) + len(b)
    s_d = s_c + len(PUSH) + 3
    fns = [_fn(BASE, s_b), _fn(s_b, s_c), _fn(s_c, s_d), _fn(s_d, BASE + len(blob))]
    out = _by(_run(blob, fns))
    assert all(out[x]["entry_kind"] == "start" for x in (BASE, s_b, s_c, s_d)), out


def test_existing_alias_kept_and_not_lifted_twice():
    blob = PUSH * 4 + b"\xc3"
    fns = [_fn(BASE, BASE + 10), dict(_fn(BASE + 10, BASE + len(blob)), entry_kind="alias")]
    out = _by(_run(blob, fns))
    assert out[BASE + 10]["entry_kind"] == "alias"
    assert out[BASE]["end"] == BASE + len(blob)


def test_unaligned_label_after_call_becomes_alias():
    # Most of 0xB84D70 is `push ...; call 0x6A3490`, so most false labels follow a call.
    blob = PUSH + CALL + PUSH + b"\xc3"
    label = BASE + 10
    assert label % 16
    fns = [_fn(BASE, label), _fn(label, BASE + len(blob))]
    assert _by(_run(blob, fns))[label]["entry_kind"] == "alias"


def test_aligned_entry_after_call_stays_start():
    blob = PUSH * 2 + b"\x90" + CALL + PUSH + b"\xc3"
    label = BASE + 16
    fns = [_fn(BASE, label), _fn(label, BASE + len(blob))]
    assert _by(_run(blob, fns))[label]["entry_kind"] == "start"


def test_walk_continues_through_existing_alias_after_call():
    blob = PUSH + CALL + PUSH * 3 + b"\xc3"
    alias, label = BASE + 10, BASE + 20
    fns = [_fn(BASE, alias), dict(_fn(alias, label), entry_kind="alias"),
           _fn(label, BASE + len(blob))]
    out = _by(_run(blob, fns))
    assert out[label]["entry_kind"] == "alias"
    assert out[BASE]["end"] == BASE + len(blob)


def test_protected_seeds_trust_vtable_scan_only_when_aligned():
    # white.exe: RTTI seeds are 99.8% 16-byte aligned, vtable-scan-only seeds 73%;
    # the scan reads runs of .data numbers inside 0xB84D70 as vtables.
    from bw2.catclean import seed_classes
    strong, weak = seed_classes(rtti=[0x1004], vtable=[0x2000, 0x2004], extra=[0x3001],
                                gap=[0x4000])
    assert strong == {0x1004, 0x3001}
    assert weak == {0x2000, 0x4000}


def test_mid_instruction_entry_deep_in_aligned_body_dropped():
    # disasm32's own drop stops walking a body after ~8k instructions.
    n = 30000
    blob = PUSH * n + b"\xc3"
    false = BASE + 5 * 25000 + 2
    fns = [_fn(BASE, false), _fn(false, BASE + len(blob))]
    out = _by(_run(blob, fns))
    assert list(out) == [BASE]
    assert out[BASE]["end"] == BASE + len(blob)


def test_weak_seed_fallen_into_becomes_alias():
    push0 = b"\x6a\x00"
    blob = push0 * 8 + PUSH + b"\xc3"
    label = BASE + 16
    fns = [_fn(BASE, label), _fn(label, BASE + len(blob))]
    out = _by(_run(blob, fns, weak=[label]))
    assert out[label]["entry_kind"] == "alias"


def test_clean_full_updates_function_count(tmp_path):
    import json
    from bw2.catclean import clean_full
    from fixtures import CODE, make_pe
    blob = PUSH * 3 + b"\xc3"
    exe = tmp_path / "x.exe"
    exe.write_bytes(make_pe([(".text", blob, CODE)], {}, 0x1000))
    va = 0x401000
    full = tmp_path / "f.json"
    full.write_text(json.dumps({"code_start": va, "code_end": va + len(blob),
                                "stats": {"functions": 2},
                                "functions": [_fn(va, va + 2), _fn(va + 2, va + len(blob))]}))
    clean_full(full, exe)
    doc = json.loads(full.read_text())
    assert doc["stats"]["functions"] == len(doc["functions"]) == 1


def test_untrusted_walk_does_not_alias():
    # Review: an unaligned, uncalled, unseeded start is the garbage class itself;
    # its walk must not turn the entries it runs into (363 on white.exe) into aliases.
    blob = b"\x90" + PUSH * 2 + b"\x6a\x00" * 3 + PUSH + b"\xc3"
    junk = BASE + 1
    label_a = BASE + 11                     # unaligned, after push
    label_b = BASE + 16                     # aligned, weak seed, after push 0
    fns = [_fn(BASE, junk), _fn(junk, label_a), _fn(label_a, label_b),
           _fn(label_b, BASE + len(blob))]
    fns[0]["entry_kind"] = "alias"          # keep the aligned BASE out of the walk
    out = _by(_run(blob, fns, weak=[label_b]))
    assert out[label_a]["entry_kind"] == "start"
    assert out[label_b]["entry_kind"] == "start"


def test_reclean_refuses_cleaned_catalog_without_uncleaned_copy(tmp_path):
    import json
    import pytest
    from bw2.catclean import main
    from fixtures import CODE, make_pe
    exe = tmp_path / "x.exe"
    exe.write_bytes(make_pe([(".text", PUSH + b"\xc3", CODE)], {}, 0x1000))
    full = tmp_path / "f.full.json"
    full.write_text(json.dumps({"code_start": 0x401000, "code_end": 0x401006, "bw2_clean": {},
                                "functions": [_fn(0x401000, 0x401006)]}))
    with pytest.raises(SystemExit, match="uncleaned"):
        main([str(full), str(exe), str(tmp_path / "out.json")])
