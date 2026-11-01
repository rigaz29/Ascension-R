"""Checks on rsxcap captures (RSX_CAP / RSX_CAP_SHOT): pictures, flicker, sRGB pairing, textures.

    py scripts/rsxcap_check.py png <in.bmp|in.png> <out.png> [step]
        a target or screenshot as a PNG (step: keep every step-th pixel)
    py scripts/rsxcap_check.py flicker <capture dir> [<capture dir> ...]
        consecutive frames (frame_NN/backbuffer.bmp): the mean luma of each, its spread, and
        the mean change from one frame to the next at lags 1-4 (a slow scene changes little
        from frame to frame; a two-state flicker changes less at lag 2 than at lag 1)
    py scripts/rsxcap_check.py srgb <manifest.txt>
        for each target: do the draws that write colour to it encode to sRGB (srgb=1), and do
        the draws that read it decode (TEXTURE_ADDRESS gamma bits 20-23)?
    py scripts/rsxcap_check.py textures <manifest.txt> [...]
        the texture formats, layouts and component remaps the draws sample

Pure Python (no imaging package): BMP 24/32-bit and 8-bit RGB/RGBA PNG.
"""
import os
import re
import statistics
import struct
import sys
import zlib
from collections import Counter, defaultdict


def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0] // 8
    flip, h = h > 0, abs(h)
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(h):
        base = off + (h - 1 - y if flip else y) * stride
        rows.append([(d[base + x * bpp + 2], d[base + x * bpp + 1], d[base + x * bpp]) for x in range(w)])
    return w, h, rows


def read_png(path):
    d = open(path, "rb").read()
    pos, idat = 8, b""
    while pos < len(d):
        n = struct.unpack_from(">I", d, pos)[0]
        t, c = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype = struct.unpack_from(">IIBB", c, 0)
            if depth != 8 or ctype not in (2, 6):
                raise ValueError("only 8-bit RGB/RGBA PNG")
            ch = 3 if ctype == 2 else 4
        elif t == b"IDAT":
            idat += c
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * ch
    prev, rows, i = bytearray(stride), [], 0
    for _ in range(h):
        f, cur = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = cur[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1: cur[x] = (cur[x] + a) & 255
            elif f == 2: cur[x] = (cur[x] + b) & 255
            elif f == 3: cur[x] = (cur[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[x] = (cur[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(cur[x * ch:x * ch + 3]) for x in range(w)])
        prev = cur
    return w, h, rows


def read_image(path):
    return read_bmp(path) if path.lower().endswith(".bmp") else read_png(path)


def write_png(path, rows):
    raw = bytearray()
    for r in rows:
        raw.append(0)
        for p in r:
            raw.extend(p[:3])
    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)
    head = struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 2, 0, 0, 0)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) +
                           chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def cmd_png(args):
    w, h, rows = read_image(args[0])
    step = int(args[2]) if len(args) > 2 else 1
    write_png(args[1], [r[::step] for r in rows[::step]])
    print("%s: %dx%d -> %s" % (args[0], w, h, args[1]))


def luma_grid(path, step=4):
    _, _, rows = read_image(path)
    return [[0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2] for p in r[::step]] for r in rows[::step]]


def cmd_flicker(args):
    for cap in args:
        frames = sorted(f for f in os.listdir(cap) if f.startswith("frame_"))
        grids = [luma_grid(os.path.join(cap, f, "backbuffer.bmp")) for f in frames
                 if os.path.exists(os.path.join(cap, f, "backbuffer.bmp"))]
        if len(grids) < 2:
            print("%s: fewer than two frames" % cap)
            continue
        cells = len(grids[0]) * len(grids[0][0])
        means = [sum(map(sum, g)) / cells for g in grids]
        def change(a, b):
            return sum(abs(x - y) for ra, rb in zip(a, b) for x, y in zip(ra, rb)) / cells
        lags = []
        for lag in (1, 2, 3, 4):
            v = [change(grids[i], grids[i + lag]) for i in range(len(grids) - lag)]
            if v:
                lags.append("lag %d %.2f" % (lag, statistics.mean(v)))
        print("%s: %d frames, mean luma %.1f (%.1f-%.1f, sd %.2f); change %s" % (
            cap, len(grids), statistics.mean(means), min(means), max(means),
            statistics.pstdev(means), ", ".join(lags)))


DISPLAY = (0x0, 0x384000, 0x708000)   # display buffers: sampled at their offset, keyed 0x4xxxxxxx as targets


def parse_manifest(path):
    targets, ops, cur = {}, [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"offrt\[\d+\] off=0x([0-9A-F]+) (\d+x\d+) dxgi=(\d+)", line)
        if m:
            targets[int(m.group(1), 16)] = (m.group(2), int(m.group(3)))
            continue
        m = re.match(r"op(\d+) DRAW\s+rt=0x([0-9A-F]+) mrt=0x([0-9A-F]+)/0x([0-9A-F]+)/0x([0-9A-F]+)"
                     r".*?cmask=(\w+) blend=\d+(?: srgb=(\d))?", line)
        if m:
            cur = dict(op=int(m.group(1)), rts=[int(m.group(i), 16) for i in range(2, 6) if int(m.group(i), 16)],
                       cmask=int(m.group(6), 16), srgb=None if m.group(7) is None else int(m.group(7)), tex=[])
            ops.append(cur)
            continue
        if re.match(r"op\d+ ", line):
            cur = None
            continue
        m = re.match(r"\s+t(\d+) off=0x([0-9A-F]+) fmt=0x([0-9A-F]+) \(([^)]*)\) (\d+x\d+) "
                     r"remap=0x([0-9A-F]+) addr=0x([0-9A-F]+)", line)
        if m and cur is not None:
            off = int(m.group(2), 16) & 0x0FFFFFFF
            cur["tex"].append(dict(unit=int(m.group(1)), off=off | (0x40000000 if off in DISPLAY else 0),
                                   fmt=int(m.group(3), 16), layout=m.group(4), size=m.group(5),
                                   remap=m.group(6), gamma=(int(m.group(7), 16) >> 20) & 15))
    return targets, ops


def cmd_srgb(args):
    targets, ops = parse_manifest(args[0])
    if ops and ops[0]["srgb"] is None:
        print("this manifest has no srgb= field (a capture from before it was added)")
        return
    writes = defaultdict(Counter)
    reads = defaultdict(Counter)
    for o in ops:
        if o["cmask"]:
            for rt in o["rts"]:
                writes[rt][o["srgb"]] += 1
        for t in o["tex"]:
            if t["off"] in targets:
                reads[t["off"]][t["gamma"]] += 1
    print("%-10s %-9s %-4s %-14s %-22s %s" % ("target", "size", "dxgi", "colour writes", "reads by gamma mask", ""))
    for off in sorted(set(writes) | set(reads)):
        size, dxgi = targets.get(off, ("?", 0))
        w, r = writes.get(off, Counter()), reads.get(off, Counter())
        note = ""
        if w and r:
            enc, lin = w[1] > 0, w[0] > 0
            dec, raw = any(k & 7 for k in r), any(not k & 7 for k in r)
            if lin and not enc and dec and not raw:
                note = "written linear, read decoded: darker"
            elif enc and not lin and raw and not dec:
                note = "written encoded, read raw: lighter"
            elif (enc and lin) or (dec and raw):
                note = "mixed: check each pass"
        print("%-10s %-9s %-4s %-14s %-22s %s" % ("0x%08X" % off, size, dxgi,
              "lin %d enc %d" % (w[0], w[1]), " ".join("0x%X:%d" % kv for kv in sorted(r.items())) or "-", note))


FORMATS = {0x81: "B8", 0x82: "A1R5G5B5", 0x83: "A4R4G4B4", 0x84: "R5G6B5", 0x85: "A8R8G8B8", 0x86: "DXT1",
           0x87: "DXT23", 0x88: "DXT45", 0x8B: "G8B8", 0x8F: "R6G5B5", 0x90: "DEPTH24_D8", 0x91: "DEPTH24_D8_F",
           0x92: "DEPTH16", 0x93: "DEPTH16_F", 0x94: "X16", 0x95: "Y16_X16", 0x97: "R5G5B5A1", 0x98: "HILO8",
           0x99: "HILO_S8", 0x9A: "W16Z16Y16X16_F", 0x9B: "W32Z32Y32X32_F", 0x9C: "X32_F", 0x9D: "D1R5G5B5",
           0x9E: "D8R8G8B8", 0x9F: "Y16_X16_F"}


def cmd_textures(args):
    c = Counter()
    for path in args:
        _, ops = parse_manifest(path)
        for o in ops:
            for t in o["tex"]:
                c[(FORMATS.get(t["fmt"] & 0x9F, "0x%02X" % (t["fmt"] & 0x9F)), t["layout"], t["remap"])] += 1
    print("%-16s %-8s %-7s %s" % ("format", "layout", "remap", "samples"))
    for (f, lay, rem), n in c.most_common():
        print("%-16s %-8s 0x%-5s %d" % (f, lay, rem, n))


if __name__ == "__main__":
    cmds = {"png": cmd_png, "flicker": cmd_flicker, "srgb": cmd_srgb, "textures": cmd_textures}
    if len(sys.argv) < 3 or sys.argv[1] not in cmds:
        print(__doc__)
        sys.exit(1)
    cmds[sys.argv[1]](sys.argv[2:])
