"""Frame-by-frame scan of a screen recording for flashes and holes.

    py scripts/flash_scan.py <video.mp4> [--out <dir>] [--extract N]

The recording is decoded small (160x90) through ffmpeg. The game's window is the region that
changes over the recording (the desktop around it does not). For every frame, within it: the
mean R, G, B, the blue and yellow tint ((R+G)/2 - B), and the share of near-black pixels.
A frame is marked when it stands out from the median of the 30 frames around it:
  flash: luma +12, or blue +10, or yellow +10 (a tinted overlay over the whole picture)
  hole:  near-black share +5 points (parts of the scene not drawn)
With --extract N, the first N marked frames (and the frame before each) are written as PNGs at
the recording's size into --out.
"""
import os
import statistics
import subprocess
import sys

FFMPEG = r"C:/ffmpeg/bin/ffmpeg.exe"
W, H = 160, 90


def frames(path, w=W, h=H):
    p = subprocess.Popen([FFMPEG, "-hide_banner", "-loglevel", "error", "-i", path, "-fps_mode", "passthrough",
                          "-vf", "scale=%d:%d" % (w, h), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         stdout=subprocess.PIPE)
    size = w * h * 3
    while True:
        b = p.stdout.read(size)
        if len(b) < size:
            break
        yield b
    p.wait()


def video_fps(path):
    out = subprocess.run([FFMPEG, "-hide_banner", "-i", path], capture_output=True, text=True).stderr
    for tok in out.split(","):
        if " fps" in tok:
            try:
                return float(tok.strip().split()[0])
            except ValueError:
                pass
    return 60.0


def window_box(path):
    """Pixels whose value changes across a sample of frames: the game's window."""
    lo = bytearray([255]) * (W * H)
    hi = bytearray(W * H)
    for i, f in enumerate(frames(path)):
        if i % 20:
            continue
        g = f[1::3]
        for k in range(W * H):
            v = g[k]
            if v < lo[k]: lo[k] = v
            if v > hi[k]: hi[k] = v
    xs, ys = [], []
    for k in range(W * H):
        if hi[k] - lo[k] > 24:
            xs.append(k % W); ys.append(k // W)
    if not xs:
        return 0, 0, W, H
    xs.sort(); ys.sort()
    q = lambda a, f: a[int(f * (len(a) - 1))]
    return q(xs, 0.01), q(ys, 0.01), q(xs, 0.99) + 1, q(ys, 0.99) + 1


DARK = bytes(1 if v < 8 else 0 for v in range(256))


def main():
    args = sys.argv[1:]
    path = args[0]
    out = args[args.index("--out") + 1] if "--out" in args else os.path.splitext(path)[0] + "_marks"
    extract = int(args[args.index("--extract") + 1]) if "--extract" in args else 0
    fps = video_fps(path)
    x0, y0, x1, y1 = window_box(path)
    rows = []
    for f in frames(path):
        n = 0; r = g = b = dark = 0
        for y in range(y0, y1):
            seg = f[(y * W + x0) * 3:(y * W + x1) * 3]
            r += sum(seg[0::3]); gg = seg[1::3]; g += sum(gg); b += sum(seg[2::3])
            dark += sum(gg.translate(DARK)); n += x1 - x0
        rows.append((r / n, g / n, b / n, 100.0 * dark / n))
    marks = []
    for i, (r, g, b, d) in enumerate(rows):
        lo, hi = max(0, i - 15), min(len(rows), i + 16)
        win = rows[lo:hi]
        ml = statistics.median(0.2126 * x[0] + 0.7152 * x[1] + 0.0722 * x[2] for x in win)
        mb = statistics.median(x[2] for x in win)
        my = statistics.median((x[0] + x[1]) / 2 - x[2] for x in win)
        md = statistics.median(x[3] for x in win)
        lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
        kinds = []
        if lum - ml > 12: kinds.append("bright %+.0f" % (lum - ml))
        if b - mb > 10: kinds.append("blue %+.0f" % (b - mb))
        if (r + g) / 2 - b - my > 10: kinds.append("yellow %+.0f" % ((r + g) / 2 - b - my))
        if d - md > 5: kinds.append("hole %+.0f%%" % (d - md))
        if kinds:
            marks.append((i, kinds))
    print("%s: %d frames at %.0f fps (%.1f s), window box x%d-%d y%d-%d of %dx%d" % (
        path, len(rows), fps, len(rows) / fps, x0, x1, y0, y1, W, H))
    flashes = [m for m in marks if any(k.split()[0] in ("bright", "blue", "yellow") for k in m[1])]
    holes = [m for m in marks if any(k.startswith("hole") for k in m[1])]
    print("  flash frames: %d, hole frames: %d" % (len(flashes), len(holes)))
    for i, kinds in marks[:60]:
        print("  %7.2f s  frame %5d  %s" % (i / fps, i, ", ".join(kinds)))
    if extract and marks:
        os.makedirs(out, exist_ok=True)
        want = set()
        for i, _ in marks[:extract]:
            want.update((max(0, i - 1), i))
        for k in sorted(want):
            subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-i", path,
                            "-vf", "select=eq(n\\,%d)" % k, "-vsync", "0", "-frames:v", "1",
                            os.path.join(out, "f%05d.png" % k)])
        print("  extracted %d frames to %s" % (len(want), out))


if __name__ == "__main__":
    main()
