"""Plan 8 Tasks 2 to 4: the host mixer and its output, IDirectSound with its streaming buffers, and
dsound.dll's private device enumeration with the waveOut guard (runtime/bw2/audio/audio_selftest.c).

Each test runs one group of the selftest headless and expects "<group>: 0 failed".
The groups put their sample data in hle_alloc blocks; nothing here is game material."""
import os
import re
import signal
import subprocess

import pytest

from bw2.build import (BW2RT, FLAGS, RECOMP32, WIN32HLE, bw2_includes, check_tools, ffmpeg_cflags, engine_libs,
                       engine_sources, sdk_path)

NOTICE = "[bw2] audio: no output device (OSStatus "
STOPPED = "[bw2] audio: output stopped at exit"


@pytest.fixture(scope="module")
def selftest(tmp_path_factory):
    exe = tmp_path_factory.mktemp("audio") / "audio_selftest"
    src = BW2RT / "audio" / "audio_selftest.c"
    assert src.exists(), f"{src} is missing"
    srcs = [s for s in engine_sources() if s.name != "main.c"] + [src]
    r = subprocess.run([str(check_tools()), "-O1", "-g", *FLAGS, "-fobjc-arc", "-isysroot", str(sdk_path()),
                        "-I", str(RECOMP32), "-I", str(WIN32HLE), *bw2_includes(), *ffmpeg_cflags(), *map(str, srcs),
                        *engine_libs(), "-o", str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[-4000:]
    return exe


def run_group(exe, group, tmp_path, *args, headless=True, **env_extra):
    env = dict(os.environ, BW2_SUPPORT=str(tmp_path / "support"))
    env.pop("BW2_AUDIO_LOG", None)
    env.update(env_extra)
    if headless:
        env["BW2_HEADLESS"] = "1"
    else:
        env.pop("BW2_HEADLESS", None)
    return subprocess.run([str(exe), group, *map(str, args)], capture_output=True, text=True, timeout=120,
                          cwd=tmp_path, env=env)


def check(exe, group, tmp_path, *args, **kw):
    r = run_group(exe, group, tmp_path, *args, **kw)
    assert r.returncode == 0 and f"{group}: 0 failed" in r.stdout, r.stdout[-4000:] + r.stderr[-4000:]
    return r


@pytest.mark.parametrize("group", ["ds_mix_exact", "ds_mix_formats", "ds_mix_sum", "ds_mix_cursor"])
def test_ds_mix(selftest, tmp_path, group):
    r = check(selftest, group, tmp_path)
    assert NOTICE not in r.stderr


def test_ds_mix_free(selftest, tmp_path):
    r = check(selftest, "ds_mix_free", tmp_path)
    m = re.search(r"checked (\d+) samples", r.stdout)
    assert m and int(m.group(1)) > 0, r.stdout[-2000:]


def test_ds_out_null(selftest, tmp_path):
    r = check(selftest, "ds_out_null", tmp_path)
    assert r.stderr.count(NOTICE) == 1, r.stderr[-2000:]
    assert ", mixing to the null sink" in r.stderr, r.stderr[-2000:]


def test_ds_out_stall(selftest, tmp_path):
    r = check(selftest, "ds_out_stall", tmp_path)
    assert NOTICE not in r.stderr, r.stderr[-2000:]


@pytest.mark.parametrize("how, stopped", [("hle", 0), ("exit", 1)])
def test_ds_out_exit(selftest, tmp_path, how, stopped):
    r = run_group(selftest, "ds_out_exit", tmp_path, how, BW2_AUDIO_LOG="1")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    m = re.search(r"ds_out_exit: consumed (\d+) frames, leaving by (\w+)", r.stdout)
    assert m and int(m.group(1)) > 0 and m.group(2) == how, r.stdout[-2000:]
    assert r.stderr.count(STOPPED) == stopped, r.stderr[-2000:]


def test_ds_audio_log(selftest, tmp_path):
    r = check(selftest, "ds_audio_log", tmp_path, BW2_AUDIO_LOG="1")
    lines = [ln for ln in r.stderr.splitlines() if "audio second" in ln]
    assert lines == ["[bw2] audio second 1: 1 playing, peak 16383, nonsilent 44100 of 44100",
                     "[bw2] audio second 2: 1 playing, peak 16383, nonsilent 44100 of 44100"], r.stderr[-2000:]


def test_ds_audio_log_off(selftest, tmp_path):
    r = check(selftest, "ds_audio_log", tmp_path)
    assert "audio second" not in r.stderr, r.stderr[-2000:]


@pytest.mark.skipif(os.environ.get("BW2_AUDIO_DEVICE") != "1", reason="set BW2_AUDIO_DEVICE=1 to play on this Mac")
def test_ds_device(selftest, tmp_path):
    r = check(selftest, "ds_device", tmp_path, headless=False)
    assert NOTICE not in r.stderr, r.stderr[-2000:]


BUFFER_LINE = "[bw2] audio buffer 1: 2 ch, 44100 Hz, 16 bit, 21504 bytes, flags 0x18000"


def test_ds_create(selftest, tmp_path):
    r = check(selftest, "ds_create", tmp_path, BW2_AUDIO_LOG="1")
    buffers = [ln for ln in r.stderr.splitlines() if "audio buffer" in ln]
    assert buffers == [BUFFER_LINE], r.stderr[-2000:]
    assert NOTICE not in r.stderr, r.stderr[-2000:]


def test_ds_create_log_off(selftest, tmp_path):
    r = check(selftest, "ds_create", tmp_path)
    assert "audio buffer" not in r.stderr, r.stderr[-2000:]


@pytest.mark.parametrize("group", ["ds_rules", "ds_buffer_lock"])
def test_ds_buffer_rules(selftest, tmp_path, group):
    check(selftest, group, tmp_path)


def test_ds_buffer_stream(selftest, tmp_path):
    r = check(selftest, "ds_buffer_stream", tmp_path)
    m = re.search(r"ds_buffer_stream: (\d+) chunks written, (\d+) frames checked", r.stdout)
    assert m and int(m.group(2)) == 200 * 441, r.stdout[-2000:]


@pytest.mark.parametrize("case, message", [
    ("flags", "IDirectSound::CreateSoundBuffer flags 0x18014: bits 0x14 not implemented"),
    ("ds8", "CLSID_DirectSound8 {3901CC3F-84B5-4FA4-BA35-AA8172B8A09B}: IDirectSound8 not implemented"),
    ("iid", "IID {C50A7E93-F395-4834-9EF6-7FA99DE50966} not implemented"),
    ("extensible", "format tag 0xFFFE, 2 channels, 16 bits"),
    ("format", "format tag 0x1, 6 channels, 16 bits"),
    ("play", "IDirectSoundBuffer::Play flags 0x0"),
    ("lockflags", "IDirectSoundBuffer::Lock flags 0x4 not implemented"),
    ("coop", "IDirectSound::SetCooperativeLevel DSSCL_WRITEPRIMARY not implemented"),
    ("dscaps", "IDirectSound::GetCaps not implemented"),
    ("caps", "IDirectSoundBuffer::GetCaps not implemented"),
    ("volume", "IDirectSoundBuffer::SetVolume not implemented"),
    ("capture", "dsound: DirectSoundCapture not implemented"),
    ("create", "dsound: DirectSoundCreate not implemented"),
    ("create8", "dsound: DirectSoundCreate8 not implemented"),
    ("enumerate", "dsound: DirectSoundEnumerateA not implemented"),
    ("init", "IDirectSound::Initialize of device {DEF00001-9C6D-47ED-AAF1-4DDA8F2B5C03} not implemented"),
])
def test_ds_stops(selftest, tmp_path, case, message):
    r = run_group(selftest, "ds_stops", tmp_path, case)
    assert r.returncode == -signal.SIGABRT and "ds_stops: returned" not in r.stdout, \
        f"exit {r.returncode}\n" + r.stdout[-2000:] + r.stderr[-2000:]
    line = [ln for ln in r.stderr.splitlines() if "FATAL" in ln]
    assert len(line) == 1 and line[0].startswith("[bw2] FATAL: d3d9: ") and message in line[0], r.stderr[-2000:]
    assert line[0].count("(from guest 0x") == 1, line[0]


def test_ds_stops_openal(selftest, tmp_path):
    r = run_group(selftest, "ds_stops", tmp_path, "openal")
    assert r.returncode == 4 and "ds_stops: returned" not in r.stdout, f"exit {r.returncode}\n" + r.stderr[-2000:]
    assert "[bw2] reached openal32.dll: owned by a later plan" in r.stderr, r.stderr[-2000:]


def test_ds_private(selftest, tmp_path):
    check(selftest, "ds_private", tmp_path)


def test_ds_private_rules(selftest, tmp_path):
    check(selftest, "ds_private_rules", tmp_path)


@pytest.mark.parametrize("case, message", [
    ("class", "dsound: DllGetClassObject of class {47D4D946-62E8-11CF-93BC-444553540000} not answered"),
    ("iid", "dsound: DllGetClassObject of class {11AB3EC0-25EC-11D1-A4D8-00C04FC28ACA} with IID "
            "{279AFA83-4981-11CE-A521-0020AF0BE560} not answered"),
    ("ole32", "dsound: DllGetClassObject of class {47D4D946-62E8-11CF-93BC-444553540000} not answered"),
    ("id", "IKsPropertySet::Get of set {84624F82-25EC-11D1-A4D8-00C04FC28ACA}, id 8 not implemented"),
    ("set", "IKsPropertySet::Get of set {31EFAC30-515C-11D0-A9AA-00AA0061BE93}, id 7 not implemented"),
    ("kset", "IKsPropertySet::Set not implemented"),
    ("waveout", "waveOutOpen: the waveOut output is not answered"),
])
def test_ds_private_stops(selftest, tmp_path, case, message):
    r = run_group(selftest, "ds_private_rules", tmp_path, case)
    assert r.returncode == -signal.SIGABRT and "ds_private_rules: returned" not in r.stdout, \
        f"exit {r.returncode}\n" + r.stdout[-2000:] + r.stderr[-2000:]
    line = [ln for ln in r.stderr.splitlines() if "FATAL" in ln]
    assert len(line) == 1 and line[0].startswith("[bw2] FATAL: d3d9: ") and message in line[0], r.stderr[-2000:]
    assert line[0].count("(from guest 0x") == 1, line[0]
