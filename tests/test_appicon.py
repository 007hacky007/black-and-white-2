"""Icon decoding uses synthetic pixels; game artwork stays outside the repository."""
import struct
import zlib
from types import SimpleNamespace

import pytest

from bw2 import appicon


def dib(bpp, pixels, mask=b"\x00\x00\x00\x00", palette=b""):
    return struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, bpp, 0, len(pixels), 0, 0,
                       len(palette) // 4, 0) + palette + pixels + mask


@pytest.mark.parametrize("bpp,pixels,palette", [
    (4, b"\x01\x00\x00\x00", b"\x00\x00\xff\x00\x00\xff\x00\x00"),
    (8, b"\x00\x01\x00\x00", b"\x00\x00\xff\x00\x00\xff\x00\x00"),
    (24, b"\x00\x00\xff\x00\xff\x00\x00\x00", b""),
    (32, b"\x00\x00\xff\x00\x00\xff\x00\x00", b""),
])
def test_decode_dib_palette_color_and_mask(bpp, pixels, palette):
    image = appicon._decode_dib(dib(bpp, pixels, b"\x40\x00\x00\x00", palette))
    assert (image.width, image.height) == (2, 1)
    assert image.pixels == [(255, 0, 0, 255), (0, 255, 0, 0)]


def test_bottom_up_rows_and_nonzero_alpha():
    raw = struct.pack("<IiiHHIIiiII", 40, 1, 4, 1, 32, 0, 8, 0, 0, 0, 0)
    image = appicon._decode_dib(raw + b"\x00\x00\xff\x80\xff\x00\x00\xff" + bytes(8))
    assert image.pixels == [(0, 0, 255, 255), (255, 0, 0, 128)]


@pytest.mark.parametrize("data", [b"", bytes(40), dib(24, b""), dib(16, bytes(8))])
def test_bad_dib_is_unusable(data):
    with pytest.raises(ValueError):
        appicon._decode_dib(data)


def test_scale2x_softens_diagonal_without_mixing_colors():
    a, b = (0, 0, 0, 255), (255, 255, 255, 255)
    source = appicon.Image(3, 3, [a, a, b, a, b, b, b, b, b])
    scaled = appicon._scale2x(source)
    assert (scaled.width, scaled.height) == (6, 6)
    assert set(scaled.pixels) == {a, b}
    assert scaled.pixels[2 * 6 + 2] == a


def test_png_canvas_has_grid_and_transparent_corners():
    source = appicon.Image(2, 2, [(255, 0, 0, 255)] * 4)
    png = appicon._render_icon(source)
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack_from(">II", png, 16) == (1024, 1024)
    offset, compressed = 8, b""
    while offset < len(png):
        size = struct.unpack_from(">I", png, offset)[0]
        if png[offset + 4:offset + 8] == b"IDAT":
            compressed += png[offset + 8:offset + 8 + size]
        offset += 12 + size
    pixels = zlib.decompress(compressed)
    stride = 1 + 1024 * 4
    assert pixels[1:5] == bytes(4)
    assert pixels[512 * stride + 1 + 512 * 4:512 * stride + 5 + 512 * 4] == b"\xff\x00\x00\xff"
    assert pixels[512 * stride + 1 + 99 * 4 + 3] == 0
    assert pixels[512 * stride + 1 + 100 * 4 + 3] == 255


def test_non_pe_is_optional(tmp_path):
    exe = tmp_path / "fake.exe"
    exe.write_bytes(b"MZ fake game")
    destination = tmp_path / "AppIcon.icns"
    assert appicon.make_game_icon(exe, destination) is False
    assert not destination.exists()


def test_extract_largest_usable_group_member(monkeypatch, tmp_path):
    small = dib(24, b"\x00\x00\xff\x00\xff\x00\x00\x00")
    large = struct.pack("<IiiHHIIiiII", 40, 3, 4, 1, 24, 0, 24, 0, 0, 0, 0) + bytes(32)
    group = struct.pack("<HHH", 0, 1, 3)
    for resource_id in (10, 11, 12):
        group += struct.pack("<BBBBHHIH", 0, 0, 0, 0, 1, 24, 0, resource_id)
    blobs = {10: small, 11: large, 12: b"bad DIB", 20: group}

    def resource(resource_id):
        value = SimpleNamespace(OffsetToData=resource_id, Size=len(blobs[resource_id]))
        language = SimpleNamespace(data=SimpleNamespace(struct=value))
        return SimpleNamespace(id=resource_id, directory=SimpleNamespace(entries=[language]))

    class PE:
        DIRECTORY_ENTRY_RESOURCE = SimpleNamespace(entries=[
            SimpleNamespace(id=3, directory=SimpleNamespace(entries=[resource(i) for i in (10, 11, 12)])),
            SimpleNamespace(id=14, directory=SimpleNamespace(entries=[resource(20)])),
        ])

        def __enter__(self):
            return self

        def __exit__(self, *args):
            pass

        def parse_data_directories(self, **kwargs):
            pass

        def get_data(self, offset, size):
            return blobs[offset]

    monkeypatch.setattr(appicon.pefile, "PE", lambda *args, **kwargs: PE())
    image = appicon._extract_icon(tmp_path / "synthetic.exe")
    assert (image.width, image.height) == (3, 2)


@pytest.mark.gamefiles
def test_real_game_icns(tmp_path):
    from bw2 import paths
    exe = paths.GAMEFILES / "game" / "white.exe"
    destination = tmp_path / "AppIcon.icns"
    assert appicon.make_game_icon(exe, destination)
    data = destination.read_bytes()
    assert data[:4] == b"icns"
    assert struct.unpack_from(">I", data, 4)[0] == len(data)
    chunks = {}
    offset = 8
    while offset < len(data):
        kind, size = struct.unpack_from(">4sI", data, offset)
        chunks[kind] = data[offset + 8:offset + size]
        offset += size
    assert struct.unpack_from(">II", chunks[b"ic10"], 16) == (1024, 1024)
    assert struct.unpack_from(">II", chunks[b"ic08"], 16) == (256, 256)
