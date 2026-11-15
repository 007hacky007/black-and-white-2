"""Build the game's macOS icon from the user's executable, without retained artwork."""
from dataclasses import dataclass
from pathlib import Path
import math
import struct
import subprocess
import tempfile
import zlib

import pefile


@dataclass
class Image:
    width: int
    height: int
    pixels: list[tuple[int, int, int, int]]


def _decode_dib(data: bytes) -> Image:
    if len(data) < 40:
        raise ValueError("short DIB header")
    header, width, doubled, planes, bpp, compression = struct.unpack_from("<IiiHHI", data)
    if (header < 40 or header > len(data) or not 0 < width <= 1024 or
            not 0 < abs(doubled) <= 2048 or doubled % 2 or planes != 1 or
            bpp not in (4, 8, 24, 32) or compression != 0):
        raise ValueError("unsupported icon DIB")
    height = abs(doubled) // 2
    colors = struct.unpack_from("<I", data, 32)[0] or (1 << bpp if bpp <= 8 else 0)
    if colors > 256:
        raise ValueError("invalid palette")
    start = header + colors * 4
    stride = ((width * bpp + 31) // 32) * 4
    mask_stride = ((width + 31) // 32) * 4
    mask_start = start + height * stride
    if mask_start + height * mask_stride > len(data):
        raise ValueError("truncated icon pixels or mask")
    palette = [tuple(data[i:i + 3][::-1]) for i in range(header, start, 4)]
    pixels = []
    for y in range(height):
        row = height - y - 1 if doubled > 0 else y
        base = start + row * stride
        for x in range(width):
            if bpp <= 8:
                index = data[base + x] if bpp == 8 else (data[base + x // 2] >> (4 if x % 2 == 0 else 0)) & 15
                if index >= len(palette):
                    raise ValueError("palette index outside table")
                r, g, b = palette[index]
                a = 255
            else:
                offset = base + x * (bpp // 8)
                b, g, r = data[offset:offset + 3]
                a = data[offset + 3] if bpp == 32 else 255
            pixels.append((r, g, b, a))
    # Older 32-bit Windows icons leave alpha entirely zero and use the AND mask.
    has_alpha = bpp == 32 and any(p[3] for p in pixels)
    for y in range(height):
        row = height - y - 1 if doubled > 0 else y
        for x in range(width):
            r, g, b, a = pixels[y * width + x]
            transparent = data[mask_start + row * mask_stride + x // 8] & (128 >> (x % 8))
            pixels[y * width + x] = (r, g, b, 0 if transparent else a if has_alpha else 255)
    return Image(width, height, pixels)


def _extract_icon(exe: Path) -> Image | None:
    with pefile.PE(str(exe), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
        root = getattr(pe, "DIRECTORY_ENTRY_RESOURCE", None)
        if root is None:
            return None
        icons, groups = {}, []
        for kind in root.entries:
            if kind.id not in (3, 14):
                continue
            for resource in kind.directory.entries:
                for language in resource.directory.entries:
                    entry = language.data.struct
                    blob = pe.get_data(entry.OffsetToData, entry.Size)
                    if kind.id == 3:
                        icons.setdefault(resource.id, []).append(blob)
                    else:
                        groups.append(blob)
        best = None
        for group in groups:
            if len(group) < 6 or struct.unpack_from("<HH", group) != (0, 1):
                continue
            count = struct.unpack_from("<H", group, 4)[0]
            if len(group) < 6 + count * 14:
                continue
            for index in range(count):
                resource_id = struct.unpack_from("<H", group, 6 + index * 14 + 12)[0]
                for blob in icons.get(resource_id, []):
                    try:
                        candidate = _decode_dib(blob)
                    except ValueError:
                        continue
                    if best is None or candidate.width * candidate.height > best.width * best.height:
                        best = candidate
        return best


def _scale2x(source: Image) -> Image:
    """EPX extends matching neighboring edges without inventing new colors."""
    w, h, p = source.width, source.height, source.pixels
    out = [None] * (w * h * 4)
    for y in range(h):
        for x in range(w):
            e = p[y * w + x]
            b, d = p[max(0, y - 1) * w + x], p[y * w + max(0, x - 1)]
            f, g = p[y * w + min(w - 1, x + 1)], p[min(h - 1, y + 1) * w + x]
            if b != g and d != f:
                values = (d if d == b else e, f if b == f else e,
                          d if d == g else e, f if g == f else e)
            else:
                values = (e, e, e, e)
            top = y * 4 * w + x * 2
            out[top:top + 2] = values[:2]
            out[top + 2 * w:top + 2 * w + 2] = values[2:]
    return Image(w * 2, h * 2, out)


def _png(image: Image) -> bytes:
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = bytearray()
    for y in range(image.height):
        rows.append(0)
        rows.extend(channel for p in image.pixels[y * image.width:(y + 1) * image.width] for channel in p)
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", image.width, image.height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def _render_icon(source: Image) -> bytes:
    # Two EPX passes refine diagonals; more passes distort small antialiased text.
    for _ in range(2):
        if max(source.width, source.height) >= 192:
            break
        source = _scale2x(source)
    canvas = Image(1024, 1024, [(0, 0, 0, 0)] * 1024 * 1024)
    tile, inset, radius = 824, 100, 184
    art = 768
    scale = max(source.width, source.height) / art
    width, height = round(source.width / scale), round(source.height / scale)
    left, top = (1024 - width) // 2, (1024 - height) // 2
    for y in range(inset, inset + tile):
        for x in range(inset, inset + tile):
            dx = max(inset + radius - x - 0.5, 0, x + 0.5 - (inset + tile - radius))
            dy = max(inset + radius - y - 0.5, 0, y + 0.5 - (inset + tile - radius))
            alpha = round(255 * max(0, min(1, radius + 0.5 - math.hypot(dx, dy))))
            if not alpha:
                continue
            r, g, b = 23, 23, 25
            if left <= x < left + width and top <= y < top + height:
                sx = max(0, min(source.width - 1, (x - left + 0.5) * scale - 0.5))
                sy = max(0, min(source.height - 1, (y - top + 0.5) * scale - 0.5))
                ix, iy = int(sx), int(sy)
                fx, fy = sx - ix, sy - iy
                weights = {}
                for yy, wy in ((iy, 1 - fy), (min(source.height - 1, iy + 1), fy)):
                    for xx, wx in ((ix, 1 - fx), (min(source.width - 1, ix + 1), fx)):
                        pixel = source.pixels[yy * source.width + xx]
                        weights[pixel] = weights.get(pixel, 0) + wx * wy
                # Sharpen color membership, keeping contour transitions narrow and smooth.
                channels, total = [0.0] * 4, 0.0
                for pixel, weight in weights.items():
                    weight = weight ** 2
                    total += weight
                    channels[3] += pixel[3] * weight
                    for c in range(3):
                        channels[c] += pixel[c] * pixel[3] / 255 * weight
                coverage = channels[3] / (255 * total)
                r, g, b = [round(channels[c] / total + background * (1 - coverage))
                           for c, background in enumerate((r, g, b))]
            canvas.pixels[y * 1024 + x] = (r, g, b, alpha)
    return _png(canvas)


def make_icns(png: Path, destination: Path) -> None:
    """Convert a 1024px PNG using the macOS system icon tools."""
    with tempfile.TemporaryDirectory(prefix="bw2-icon-") as temporary:
        iconset = Path(temporary) / "AppIcon.iconset"
        iconset.mkdir()
        for points in (16, 32, 128, 256, 512):
            for factor in (1, 2):
                pixels = str(points * factor)
                suffix = "@2x" if factor == 2 else ""
                output = iconset / f"icon_{points}x{points}{suffix}.png"
                subprocess.run(["/usr/bin/sips", "-z", pixels, pixels, str(png), "--out", str(output)],
                               check=True, capture_output=True)
        subprocess.run(["/usr/bin/iconutil", "-c", "icns", str(iconset), "-o", str(destination)],
                       check=True, capture_output=True)


def make_game_icon(exe: Path, destination: Path) -> bool:
    """Return False when no supported icon is present; keep packaging optional."""
    try:
        source = _extract_icon(exe)
    except (pefile.PEFormatError, OSError, ValueError, AttributeError, struct.error):
        return False
    if source is None:
        return False
    with tempfile.TemporaryDirectory(prefix="bw2-artwork-") as temporary:
        png = Path(temporary) / "AppIcon.png"
        png.write_bytes(_render_icon(source))
        make_icns(png, destination)
    return True
