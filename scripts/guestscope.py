"""guestscope: watch the title's SPURS instance and its Wws job lists from outside a process --
RPCS3 or the port, the same way for both -- with ReadProcessMemory only.

    py scripts/guestscope.py capture --proc rpcs3.exe --at 367 --seconds 90 --out rp.json
    py scripts/guestscope.py capture --proc Ascension-R.exe --at 220 --seconds 90 --out port.json
    py scripts/guestscope.py report rp.json:24.7 port.json:15.6     # SPUs per workload
    py scripts/guestscope.py jobs rp.json:24.7 port.json:15.6       # jobs per list
    py scripts/guestscope.py edge rp.json port.json                 # EDGE jobs per contention

capture waits until the process is --at seconds old, finds the guest memory base (the ELF's load
segment at guest 0x10000), the SPURS instance (the one whose wklInfo tables hold the most entries
with the Wws policy module 0x007A7B00) and each Wws workload's data block, then reads as fast as
it can, for --seconds: the instance's first 0x80 bytes (ready counts, contention, min/max
contention, flags, signals) and each data block's +0x30..+0x7F (list pointers, ticket rows
published << 16 | consumed). Every change is kept with its time. The reads take one host core;
on the first level the port ran 4-7% slower under them, RPCS3 within its usual spread.

The reports take a capture and the frame rate over the same seconds (for per-frame figures). The
title adds its Wws workloads in one order, but the workload ids differ between processes (RPCS3
puts tasksets among them), so workloads are named by their order among the Wws ones.
"""
import argparse, collections, ctypes, json, statistics, struct, sys, time

WWS = ["CollisL", "HighestPrio6", "General6", "SysMisc", "ModelServer", "Network", "Batch", "audio",
       "RenJobSet0", "RenJobSet1", "RenJobSet2", "RenJobSet3", "RenderJobs", "MeshProc", "EDGE0", "EDGE1",
       "EDGE2", "RsxSync"]
WWS_PM = 0x007A7B00
ELF = r"vfs\PS3_GAME\USRDIR\GOWA.elf"


# ---------------------------------------------------------------------------
# capture
# ---------------------------------------------------------------------------

def _win():
    from ctypes import wintypes as W
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)

    class MBI(ctypes.Structure):
        _fields_ = [("BaseAddress", ctypes.c_ulonglong), ("AllocationBase", ctypes.c_ulonglong),
                    ("AllocationProtect", W.DWORD), ("PartitionId", W.WORD),
                    ("RegionSize", ctypes.c_ulonglong), ("State", W.DWORD), ("Protect", W.DWORD),
                    ("Type", W.DWORD)]

    k32.OpenProcess.restype = W.HANDLE
    k32.VirtualQueryEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
    k32.VirtualQueryEx.restype = ctypes.c_size_t
    k32.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                      ctypes.POINTER(ctypes.c_size_t)]
    k32.GetProcessTimes.argtypes = [W.HANDLE] + [ctypes.POINTER(W.FILETIME)] * 4
    return W, k32, psapi, MBI


class Proc:
    QUERY_READ = 0x0410

    def __init__(self, name):
        self.W, self.k32, self.psapi, self.MBI = _win()
        self.pid = None
        for _ in range(600):
            self.pid = self._find(name)
            if self.pid:
                break
            time.sleep(1)
        if not self.pid:
            raise SystemExit("no process " + name)
        self.h = self.k32.OpenProcess(self.QUERY_READ, False, self.pid)

    def _find(self, name):
        W = self.W
        arr, got = (W.DWORD * 4096)(), W.DWORD()
        self.psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(got))
        for pid in arr[:got.value // 4]:
            h = self.k32.OpenProcess(self.QUERY_READ, False, pid)
            if not h:
                continue
            buf = ctypes.create_unicode_buffer(260)
            self.psapi.GetModuleBaseNameW(h, None, buf, 260)
            self.k32.CloseHandle(h)
            if buf.value.lower() == name.lower():
                return pid
        return None

    def age(self):
        W = self.W
        c, e, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
        self.k32.GetProcessTimes(self.h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
        return time.time() - ((c.dwHighDateTime << 32 | c.dwLowDateTime) / 1e7 - 11644473600.0)

    def regions(self):
        addr, m = 0, self.MBI()
        while self.k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(m), ctypes.sizeof(m)):
            if m.State == 0x1000 and m.Protect and not (m.Protect & 0x101):   # committed, readable
                yield m.BaseAddress, m.RegionSize
            addr = m.BaseAddress + m.RegionSize
            if addr >= 1 << 47:
                break

    def read(self, addr, n):
        buf, got = ctypes.create_string_buffer(n), ctypes.c_size_t()
        ok = self.k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
        return buf.raw if ok and got.value == n else None


def elf_head(path):
    f = open(path, "rb").read()
    phoff = struct.unpack_from(">Q", f, 0x20)[0]
    phent, phnum = struct.unpack_from(">HH", f, 0x36)
    for i in range(phnum):
        typ, _, off, vaddr = struct.unpack_from(">IIQQ", f, phoff + i * phent)
        if typ == 1 and vaddr == 0x10000:
            return f[off:off + 256]
    raise SystemExit("no load segment at 0x10000 in " + path)


def guest_bases(p, head):
    out = []
    for base, size in p.regions():
        if p.read(base, 256) == head:
            out.append(base - 0x10000)
        elif size > 0x10100 and p.read(base + 0x10000, 256) == head:
            out.append(base)
    return out


def find_spurs(p, base):
    pm = struct.pack(">Q", WWS_PM)
    best, best_n, seen = None, 0, set()
    for rb, size in p.regions():
        if rb < base or rb >= base + (1 << 32):
            continue
        for off in range(0, size, 4 << 20):
            b = p.read(rb + off, min(4 << 20, size - off))
            i = b.find(pm) if b else -1
            while i >= 0:
                x = rb + off + i - base
                for tab in (0xB00, 0x1000):            # wklInfo1 (wid 0-15), wklInfo2 (16-31)
                    for k in range(16):
                        ea = x - tab - 0x20 * k
                        if ea % 0x80 or ea in seen:
                            continue
                        seen.add(ea)
                        head = p.read(base + ea, 0x80)
                        if not head or head[0x76] != 6:   # nSpus
                            continue
                        t1, t2 = p.read(base + ea + 0xB00, 0x200), p.read(base + ea + 0x1000, 0x200)
                        n = sum(1 for t in (t1, t2) if t for j in range(16) if t[0x20 * j:0x20 * j + 8] == pm)
                        if n > best_n:
                            best, best_n = ea, n
                i = b.find(pm, i + 1)
    return best


def wkl_table(p, base, ea):
    out = {}
    for tab, w0 in ((0xB00, 0), (0x1000, 16)):
        t = p.read(base + ea + tab, 0x200)
        for j in range(16):
            pm, arg = struct.unpack_from(">QQ", t, 0x20 * j)
            if pm:
                out[w0 + j] = (pm, arg, t[0x20 * j + 0x18:0x20 * j + 0x20].hex())
    return out


def cmd_capture(a):
    p = Proc(a.proc)
    while p.age() < a.at:
        time.sleep(0.5)
    bases = guest_bases(p, elf_head(a.elf))
    base = ea = None
    for b in bases:
        ea = find_spurs(p, b)
        if ea is not None:
            base = b
            break
    if base is None:
        raise SystemExit("no SPURS instance found (guest bases: %s)" % " ".join("%X" % b for b in bases))
    table = wkl_table(p, base, ea)
    rows = [(w, arg & 0xFFFFFFFF) for w, (pm, arg, _) in sorted(table.items()) if pm == WWS_PM]
    print("pid %d, guest base %X, SPURS at %08X, age %.1f s" % (p.pid, base, ea, p.age()))
    print("workloads: " + " ".join("%d:%X/%X/%s" % (w, pm, arg & 0xFFFFFFFF, pr) for w, (pm, arg, pr) in sorted(table.items())))
    rpm, h, got = p.k32.ReadProcessMemory, p.h, ctypes.c_size_t()
    head = ctypes.create_string_buffer(0x80)
    bufs = [ctypes.create_string_buffer(0x50) for _ in rows]
    addrs = [base + d + 0x30 for _, d in rows]
    spurs = base + ea
    clock = time.perf_counter_ns
    ev, last, n = [], None, 0
    t0 = clock()
    end = t0 + int(a.seconds * 1e9)
    age0 = p.age()
    while True:
        rpm(h, spurs, head, 0x80, ctypes.byref(got))
        for ad, bf in zip(addrs, bufs):
            rpm(h, ad, bf, 0x50, ctypes.byref(got))
        t = clock()
        n += 1
        cur = head.raw + b"".join(bf.raw for bf in bufs)
        if cur != last:
            ev.append((t - t0, cur.hex()))
            last = cur
        if t >= end:
            break
    dt = (clock() - t0) / 1e9
    print("%d samples in %.1f s (%.1f us apart), %d changes" % (n, dt, dt * 1e6 / n, len(ev)))
    json.dump({"proc": a.proc, "base": base, "spurs": ea, "age0": age0, "seconds": dt, "samples": n,
               "table": {str(w): list(v) for w, v in table.items()}, "rows": rows, "events": ev},
              open(a.out, "w"))


# ---------------------------------------------------------------------------
# reports
# ---------------------------------------------------------------------------

class Capture:
    def __init__(self, arg, need_fps=True):
        path, _, fps = arg.rpartition(":") if need_fps else (arg, None, "0")
        self.path, self.fps = path, float(fps)
        d = json.load(open(path))
        self.T = d["seconds"]
        self.rows = d["rows"]
        self.ev = [(t, bytes.fromhex(h)) for t, h in d["events"]]
        self.is2 = bool(self.ev[0][1][0x74] & 0x40)
        self.names = {w: WWS[i] for i, (w, _) in enumerate(self.rows)}
        self.frames = self.fps * self.T

    def cont(self, b, w, off=0x20):
        v = b[off + (w & 15)]
        return (v & 15 if w < 16 else v >> 4) if self.is2 else v

    def spans(self):
        """(bytes, seconds) for every event."""
        end = self.T * 1e9
        for k, (t, b) in enumerate(self.ev):
            yield b, ((self.ev[k + 1][0] if k + 1 < len(self.ev) else end) - t) / 1e9

    def row(self, b, i, l):
        o = 0x80 + 0x50 * i
        ptr = int.from_bytes(b[o + 4 * l:o + 4 * l + 4], "big")
        r = int.from_bytes(b[o + 0x10 + 0x10 * l:o + 0x14 + 0x10 * l], "big")
        return ptr, r >> 16, r & 0xFFFF


def cmd_report(a):
    caps = [Capture(x) for x in a.captures]
    out = {}
    for c in caps:
        lane, busy, enter = collections.Counter(), collections.Counter(), collections.Counter()
        prev = None
        for b, dt in c.spans():
            for w in c.names:
                n = c.cont(b, w)
                lane[w] += n * dt
                busy[w] += (n > 0) * dt
                if prev is not None and n > c.cont(prev, w):
                    enter[w] += n - c.cont(prev, w)
            prev = b
        b0 = c.ev[0][1]
        out[c.path] = {c.names[w]: (lane[w] / c.T, lane[w] * 1e3 / c.frames, busy[w] * 1e3 / c.frames,
                                    enter[w] / c.frames, c.cont(b0, w, 0x50)) for w in c.names}
        print("%s: %.1f s, %.2f flips/s, SPUs in workloads %.2f" % (c.path, c.T, c.fps, sum(lane.values()) / c.T))
    print("%-13s" % "" + "".join("| %-40s" % c.path[-40:] for c in caps))
    print("%-13s" % "workload" + "| SPUs  lane-ms/f busy-ms/f entries/f max " * len(caps))
    for n in WWS:
        line = "%-13s" % n
        for c in caps:
            r = out[c.path].get(n)
            line += ("| %4.2f %9.1f %9.1f %9.1f %3d " % r) if r else "| %-40s" % "-"
        print(line)


def cmd_jobs(a):
    for c in (Capture(x) for x in a.captures):
        st = {w: {"busy": 0.0, "lane": 0.0, "lists": {}} for w in c.names}
        edge = [w for w, n in c.names.items() if n.startswith("EDGE")]
        edge_busy, prev = 0.0, None
        t = 0.0
        for b, dt in c.spans():
            if any(c.cont(b, w) for w in edge):
                edge_busy += dt
            for i, (w, _) in enumerate(c.rows):
                n = c.cont(b, w)
                s = st[w]
                s["busy"] += (n > 0) * dt
                s["lane"] += n * dt
                for l in range(4):
                    ptr, pub, con = c.row(b, i, l)
                    if not ptr:
                        continue
                    L = s["lists"].setdefault(l, {"pub": 0, "took": 0, "wait_nospu": 0.0, "wait_spu": 0.0})
                    if prev is not None:
                        _, ppub, pcon = c.row(prev, i, l)
                        dp, dc = (pub - ppub) & 0xFFFF, (con - pcon) & 0xFFFF
                        L["pub"] += dp if dp < 0x8000 else 0
                        L["took"] += dc if dc < 0x8000 else 0
                    if pub != con:
                        L["wait_spu" if n else "wait_nospu"] += dt
            prev = b
        f = c.frames
        print("== %s: %.2f flips/s, %.0f s; the EDGE lists busy %.1f ms a frame" % (c.path, c.fps, c.T, edge_busy * 1e3 / f))
        print("%-13s %-3s %8s %8s %9s %6s %11s %11s" % ("workload", "L", "pub/f", "took/f", "busy ms/f", "SPUs",
                                                      "wait noSPU", "wait w/SPU"))
        for w, s in sorted(st.items()):
            for l, L in sorted(s["lists"].items()):
                if L["pub"] or L["took"]:
                    print("%-13s L%-2d %8.1f %8.1f %9.1f %6.2f %11.2f %11.2f" % (
                        c.names[w], l, L["pub"] / f, L["took"] / f, s["busy"] * 1e3 / f,
                        s["lane"] / s["busy"] if s["busy"] else 0.0, L["wait_nospu"] * 1e3 / f, L["wait_spu"] * 1e3 / f))


def cmd_edge(a):
    for c in (Capture(x, need_fps=False) for x in a.captures):
        tim, takes = collections.Counter(), collections.Counter()
        for i, (w, _) in enumerate(c.rows):
            if not c.names[w].startswith("EDGE"):
                continue
            prev = None
            for b, dt in c.spans():
                n = c.cont(b, w)
                tim[n] += dt
                con = c.row(b, i, 3)[2]
                if prev is not None:
                    d = (con - prev[1]) & 0xFFFF
                    if d < 0x8000:
                        takes[prev[0]] += d
                prev = (n, con)
        print("== %s" % c.path)
        for n in sorted(tim):
            if n and tim[n] > 0.1:
                rate = takes[n] / tim[n]
                print("  %d SPUs in the list: %6.2f s, %6.0f jobs taken/s, %5.1f us apart, %5.1f us a job per SPU" % (
                    n, tim[n], rate, 1e6 / rate, n * 1e6 / rate))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("capture")
    p.add_argument("--proc", required=True)
    p.add_argument("--at", type=float, default=0, help="start when the process is this many seconds old")
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--out", required=True)
    p.add_argument("--elf", default=ELF)
    for name in ("report", "jobs"):
        q = sub.add_parser(name)
        q.add_argument("captures", nargs="+", help="capture.json:flips_per_s")
    q = sub.add_parser("edge")
    q.add_argument("captures", nargs="+", help="capture.json")
    a = ap.parse_args()
    {"capture": cmd_capture, "report": cmd_report, "jobs": cmd_jobs, "edge": cmd_edge}[a.cmd](a)


if __name__ == "__main__":
    main()
