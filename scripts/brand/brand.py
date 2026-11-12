#!/usr/bin/env python3
"""brand.py - Ascension-R's icon and GitHub banner, from the precompile screen's own scene (brand.cpp).

Builds brand.cpp with the toolchain gowa.py uses, into build/brand/, and writes:
    docs/images/banner.png   1280x640, the scene with the title: the README's banner and the repository's social preview
    docs/images/icon.png     512x512, the icon at 64 pixels, each 8x8
    port/app.ico             16, 32 and 48 as drawn, 64, and 256 from 64: Ascension-R.exe's icon (app.rc)
The pictures are pixel art: each size of the icon is drawn, and every larger one is a smaller one's pixels made into
squares. check_repo.py lets the two PNGs in by their SHA-256, which this prints: put them there when they change.

Usage:
    py scripts/brand/brand.py
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORK = os.path.join(ROOT, "build", "brand")
CW, CH = 640, 360
BANNER = dict(t=3000, bolt=6210, shift=-110.0, crop=12)   # the scene's moment, its lightning's, and the rows left off the top
CORNER = {16: 3, 32: 5, 48: 7, 64: 9}                       # each size's corner radius


def build():
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import gowa  # noqa: E402
    env = gowa.toolchain_env()
    clang = shutil.which("clang-cl", path=env.get("PATH"))
    if not clang:
        sys.exit("clang-cl not found; see `gowa.py doctor`")
    os.makedirs(WORK, exist_ok=True)
    exe = os.path.join(WORK, "brand.exe")
    cmd = [clang, "/nologo", "/O2", "/EHsc", "/std:c++17", "/D_CRT_SECURE_NO_WARNINGS", "-Wno-unused-function",
           "/I" + os.path.join(ROOT, "port"), "/I" + os.path.join(ROOT, "ps3recomp", "include"),
           os.path.join(HERE, "brand.cpp"), "/Fo" + WORK + os.sep, "/Fe" + exe, "/link", "user32.lib", "gdi32.lib"]
    r = subprocess.run(cmd, env=env, cwd=WORK, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout[-4000:] + r.stderr[-4000:])
    return exe


def draw(exe, *args):
    """The canvas brand.exe drew, as 0x00RRGGBB."""
    raw = os.path.join(WORK, "canvas.raw")
    subprocess.run([exe, args[0], raw] + [str(a) for a in args[1:]], check=True)
    data = open(raw, "rb").read()
    os.remove(raw)
    return struct.unpack("<%dI" % (CW * CH), data)


def rounded(n, r):
    """Whether a pixel of an n x n square with corners of radius r is inside it."""
    def inside(x, y):
        cx = min(max(x + 0.5, r), n - r)
        cy = min(max(y + 0.5, r), n - r)
        return (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r
    return inside


def rgba(px, x0, y0, w, h, scale, mask=None):
    """Rows of RGBA bytes, top down: the canvas's w x h at (x0, y0), each pixel scale x scale."""
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            c = px[(y0 + y) * CW + x0 + x]
            a = 255 if mask is None or mask(x, y) else 0
            row += bytes(((c >> 16) & 255, (c >> 8) & 255, c & 255, a)) * scale
        rows += [bytes(row)] * scale
    return rows


def png(rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    w, h = len(rows[0]) // 4, len(rows)
    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib(rows):
    """An icon image as Windows has always read them: a 32-bit bitmap bottom up, then the mask of the clear pixels."""
    n = len(rows)
    head = struct.pack("<IiiHHIIiiII", 40, n, 2 * n, 1, 32, 0, 0, 0, 0, 0, 0)
    color = b"".join(bytes(b for i in range(0, len(r), 4) for b in (r[i + 2], r[i + 1], r[i], r[i + 3])) for r in reversed(rows))
    stride = (n + 31) // 32 * 4
    mask = bytearray()
    for r in reversed(rows):
        bits = bytearray(stride)
        for x in range(n):
            if r[4 * x + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return head + color + bytes(mask)


def ico(images):
    """An .ico of (size, image bytes), each a DIB or a PNG."""
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    blobs = b""
    for n, data in images:
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    return out + blobs


def write(rel, data):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print("%-24s %7d bytes  SHA-256 %s" % (rel.replace(os.sep, "/"), len(data), hashlib.sha256(data).hexdigest()))


def main():
    exe = build()
    scene = draw(exe, "banner", BANNER["t"], BANNER["bolt"], BANNER["shift"])
    write(os.path.join("docs", "images", "banner.png"), png(rgba(scene, 0, BANNER["crop"], CW, 320, 2)))
    icons = {n: draw(exe, "icon", n) for n in CORNER}
    mask = {n: rounded(n, r) for n, r in CORNER.items()}
    write(os.path.join("docs", "images", "icon.png"), png(rgba(icons[64], 0, 0, 64, 64, 8, mask[64])))
    images = [(n, dib(rgba(icons[n], 0, 0, n, n, 1, mask[n]))) for n in (16, 32, 48)]
    images.append((64, png(rgba(icons[64], 0, 0, 64, 64, 1, mask[64]))))
    images.append((256, png(rgba(icons[64], 0, 0, 64, 64, 4, mask[64]))))
    write(os.path.join("port", "app.ico"), ico(images))
    return 0


if __name__ == "__main__":
    sys.exit(main())
