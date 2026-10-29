"""Frame report of a PS3_FTRACE trace: where a frame of this title goes.

    gowa.py frames trace.txt [--grid 2] [--skip 20] [--ms 2] [--rows]

Record the trace with PS3_FTRACE=<start>:<seconds>:<file>, and GOWA_WWS_TRACE=1 for the job lists'
ticket rows (PS3_FT_LINE events: who published and who took each job). Sections:

  frames      count, median and mean length (flip request to flip request)
  groups      per frame, each workload group's SPU busy time, the time with at least one of
              its runs open, and how many ran at once
  main        the main thread's time outside its waits, and in each wait by call site
  rsx         the walker's drain time, its waits on SEMAPHORE_ACQUIRE by label
  rows        (--rows, needs GOWA_WWS_TRACE) each job list's publishers and consumers
  grid        a few frames on a fine grid: what runs when (--grid N frames from --skip)
"""
import collections
import statistics

SYSCALL, USLEEP, SPU_BUSY, SEMA, SEMA_GAVEUP, DRAIN, FLIP_FIFO, FLIP_REQ, LINE = 1, 2, 4, 5, 6, 7, 8, 9, 16
GROUPS = [("EDGE w14-16", {14, 15, 16}), ("RenderJobs w12", {12}), ("MeshProc w13", {13}),
          ("General6 w2", {2}), ("audio w7", {7}), ("RenJobSet w8-11", {8, 9, 10, 11}),
          ("Batch w6", {6}), ("RsxSync w17", {17}), ("other wids", None)]
WAIT_RSX, WAIT_EDGE = 0x3A3698, 0x41D5C8      # usleep call sites: the RSX's ref, the EDGE counter


def add_args(p):
    p.add_argument("trace", help="a PS3_FTRACE file")
    p.add_argument("--grid", type=int, default=2, help="frames to draw on the grid (default 2, 0 for none)")
    p.add_argument("--skip", type=int, default=20, help="first frame of the grid (default 20)")
    p.add_argument("--ms", type=float, default=2.0, help="grid column width in ms (default 2)")
    p.add_argument("--rows", action="store_true", help="the job lists' publishers and consumers")


def group(w):
    for i, (_, ws) in enumerate(GROUPS):
        if ws is not None and w in ws:
            return i
    return len(GROUPS) - 1


def load(path):
    t = {"names": {}, "busy": [], "drains": [], "sema": [], "freq": [], "ffifo": [], "main": [],
         "lines": collections.defaultdict(list)}
    for raw in open(path):
        if raw.startswith("#"):
            continue
        if raw.startswith("T "):
            _, tid, nm = raw.rstrip("\n").split(" ", 2)
            t["names"][int(tid)] = nm
            continue
        f = raw.split()
        if len(f) < 6:
            continue
        k, who, a, b = int(f[0]), int(f[1]), int(f[2], 16), int(f[3], 16)
        t0, t1 = float(f[4]) / 1000.0, float(f[5]) / 1000.0          # us -> ms
        if k == SPU_BUSY:
            t["busy"].append((t0, t1, b))
        elif k == DRAIN:
            t["drains"].append((t0, t1))
        elif k in (SEMA, SEMA_GAVEUP):
            t["sema"].append((t0, t1, a, b, k == SEMA_GAVEUP))
        elif k == FLIP_REQ:
            t["freq"].append(t0)
        elif k == FLIP_FIFO:
            t["ffifo"].append(t0)
        elif k in (SYSCALL, USLEEP) and who == 1:
            t["main"].append((t0, t1, b, k))
        elif k == LINE:
            t["lines"][a].append((t0, who, b))
    t["freq"].sort()
    t["ffifo"].sort()
    return t


def clip(spans, f0, f1):
    return [(max(s, f0), min(e, f1)) for s, e in spans if e > f0 and s < f1]


def union(spans):
    if not spans:
        return 0.0
    spans = sorted(spans)
    u, cs, ce = 0.0, spans[0][0], spans[0][1]
    for s, e in spans[1:]:
        if s > ce:
            u += ce - cs
            cs, ce = s, e
        else:
            ce = max(ce, e)
    return u + ce - cs


def summary(t):
    freq = t["freq"]
    lens = [freq[i + 1] - freq[i] for i in range(len(freq) - 1)]
    if not lens:
        print("no frames (fewer than two flip requests in the trace)")
        return
    n = len(lens)
    print("frames: %d, median %.1f ms, mean %.1f ms" % (n, statistics.median(lens), statistics.mean(lens)))
    per = collections.defaultdict(lambda: collections.defaultdict(float))
    width = collections.defaultdict(list)
    for i in range(n):
        f0, f1 = freq[i], freq[i + 1]
        for g, (name, _) in enumerate(GROUPS):
            sp = clip([(s, e) for s, e, w in t["busy"] if group(w) == g], f0, f1)
            if sp:
                b, u = sum(e - s for s, e in sp), union(sp)
                per[name]["busy"] += b
                per[name]["open"] += u
                width[name].append(b / u if u else 0.0)
        waits = collections.Counter()
        for s, e, lr, k in t["main"]:
            if e > f0 and s < f1:
                waits["syscall" if k == SYSCALL else "usleep@%X" % lr] += min(e, f1) - max(s, f0)
        for key, v in waits.items():
            per["main"][key] += v
        per["main"]["outside waits"] += (f1 - f0) - sum(waits.values())
        per["rsx"]["drain"] += sum(e - s for s, e in clip(t["drains"], f0, f1))
    print("\ngroups, per frame:   %10s %16s %8s" % ("SPU busy", "with a run open", "width"))
    tot = 0.0
    for name, _ in GROUPS:
        if name in per:
            tot += per[name]["busy"]
            print("  %-17s %8.1f ms %13.1f ms %8.2f" % (name, per[name]["busy"] / n, per[name]["open"] / n,
                                                     statistics.mean(width[name])))
    print("  %-17s %8.1f ms" % ("all", tot / n))
    print("\nmain thread, ms per frame:")
    for key, v in sorted(per["main"].items(), key=lambda kv: -kv[1]):
        if v / n >= 0.1:
            print("  %-22s %6.1f" % (key, v / n))
    print("\nrsx, ms per frame: drain %.1f" % (per["rsx"]["drain"] / n))
    by = collections.defaultdict(lambda: [0.0, 0, 0])
    for s, e, off, val, gaveup in t["sema"]:
        k = (off, val)
        by[k][0] += e - s
        by[k][1] += 1
        by[k][2] += gaveup
    if not by:
        print("  no SEMAPHORE_ACQUIRE waits")
    for (off, val), (ms, cnt, gu) in sorted(by.items(), key=lambda kv: -kv[1][0])[:8]:
        print("  acquire off 0x%X == 0x%08X: %.2f ms/frame, %.1f waits/frame%s"
              % (off, val, ms / n, cnt / n, ", %d gave up" % gu if gu else ""))


def src(t, who):
    if who >= 0x20000:
        return "spu%d" % (who & 0xFF)
    if who >= 0x10000:
        return "w%d" % (who & 0xFFFF)
    return "ppu:%s" % t["names"].get(who, str(who))


def rows(t):
    if not t["lines"]:
        print("\nrows: no PS3_FT_LINE events (record with GOWA_WWS_TRACE=1)")
        return
    span = max(1.0, (t["freq"][-1] - t["freq"][0]) / 1000.0) if len(t["freq"]) > 1 else 1.0
    print("\njob list rows (published << 16 | taken), %.1f s:" % span)
    out = []
    for ea, ev in t["lines"].items():
        ev.sort()
        pub_by, cons_by, prev = collections.Counter(), collections.Counter(), None
        for _, who, v in ev:
            if prev is not None:
                dp = ((v >> 16) - (prev >> 16)) & 0xFFFF
                dc = ((v & 0xFFFF) - (prev & 0xFFFF)) & 0xFFFF
                if dp and dp < 0x8000:
                    pub_by[src(t, who)] += dp
                if dc and dc < 0x8000:
                    cons_by[src(t, who)] += dc
            prev = v
        owner = cons_by.most_common(1)[0][0] if cons_by else "?"
        out.append((owner, ea, pub_by, cons_by))
    for owner, ea, pub_by, cons_by in sorted(out):
        tp, tc = sum(pub_by.values()), sum(cons_by.values())
        print("  %08X taken by %-5s published %6d (%5.0f/s) by %s; taken %d by %s"
              % (ea, owner, tp, tp / span,
                 ", ".join("%s %.0f%%" % (s, 100.0 * c / tp) for s, c in pub_by.most_common(3)) or "-",
                 tc, ", ".join("%s %.0f%%" % (s, 100.0 * c / tc) for s, c in cons_by.most_common(3)) or "-"))


def grid(t, frames, skip, ms):
    freq = t["freq"]
    print("\ngrid: digits are the runs open in a column; main: '#' running, 'r' waiting for the RSX,"
          " 'e' for the EDGE counter, 'u' another usleep, 's' a syscall; rsx: 'D' drain, 'F' flip, 'A' acquire wait")
    for fi in range(skip, min(skip + frames, len(freq) - 1)):
        f0, f1 = freq[fi], freq[fi + 1]
        n = int((f1 - f0) / ms) + 1

        def cols(s, e):
            return range(max(0, int((s - f0) / ms)), min(n, int((e - f0) / ms) + 1))

        rowsv = [[0.0] * n for _ in GROUPS]
        for s, e, w in t["busy"]:
            if e > f0 and s < f1:
                for c in cols(s, e):
                    lo, hi = max(s, f0 + c * ms), min(e, f0 + (c + 1) * ms)
                    if hi > lo:
                        rowsv[group(w)][c] += (hi - lo) / ms
        m = ["#"] * n
        for s, e, lr, k in t["main"]:
            if e > f0 and s < f1:
                ch = "s" if k == SYSCALL else ("r" if lr == WAIT_RSX else ("e" if lr == WAIT_EDGE else "u"))
                for c in cols(s, e):
                    m[c] = ch
        r = [" "] * n
        for s, e in t["drains"]:
            if e > f0 and s < f1:
                for c in cols(s, e):
                    r[c] = "D"
        for s, e, *_ in t["sema"]:
            if e > f0 and s < f1:
                for c in cols(s, e):
                    r[c] = "A"
        for x in t["ffifo"]:
            if f0 <= x < f1:
                r[int((x - f0) / ms)] = "F"
        print("frame %d: %.1f ms (%g ms a column)" % (fi, f1 - f0, ms))
        print("  %-16s %s" % ("main", "".join(m)))
        for (name, _), row in zip(GROUPS, rowsv):
            print("  %-16s %s" % (name, "".join(" " if v < 0.05 else str(min(9, int(round(v)) or 1)) for v in row)))
        print("  %-16s %s" % ("rsx", "".join(r)))


def main(a):
    t = load(a.trace)
    summary(t)
    if a.rows:
        rows(t)
    if a.grid:
        grid(t, a.grid, a.skip, a.ms)
    return 0
