"""guestscope: watch the title's SPURS instance and its Wws job lists from outside a process --
RPCS3 or the port, the same way for both -- with ReadProcessMemory only.

    py scripts/guestscope.py capture --proc rpcs3.exe --at 367 --seconds 90 --out rp.json
    py scripts/guestscope.py capture --proc Ascension-R.exe --at 220 --seconds 90 --out port.json
    py scripts/guestscope.py capture --proc rpcs3.exe --log <rpcs3>/log/TTY.log --delay 66 --seconds 60 --out rp.json
    py scripts/guestscope.py report rp.json:24.7 port.json:15.6     # SPUs per workload
    py scripts/guestscope.py jobs rp.json:24.7 port.json:15.6       # jobs per list
    py scripts/guestscope.py edge rp.json port.json                 # EDGE jobs per contention
    py scripts/guestscope.py states rp.json port.json               # running, ready with no SPU, blocked
    py scripts/guestscope.py frames rp.json port.json               # the median frame's parts

capture waits until the process is --at seconds old, finds the guest memory base (the ELF's load
segment at guest 0x10000), the SPURS instance (the one whose wklInfo tables hold the most entries
with the Wws policy module 0x007A7B00) and each Wws workload's data block, then reads as fast as
it can, for --seconds: the instance's first 0x80 bytes (ready counts, contention, min/max
contention, flags, signals) and each data block's +0x30..+0x7F (list pointers, ticket rows
published << 16 | consumed). Every change is kept with its time. The reads take one host core;
on the first level the port ran 4-7% slower under them, RPCS3 within its usual spread.
With --log, it waits for the level's TTY line (--needle) in that log instead, the same line in
RPCS3's TTY.log and in the port's output, and starts --delay seconds after it. It also keeps the
window titles, where both programs show their FPS, and each thread's CPU time over the capture.

The reports take a capture and the frame rate over the same seconds (for per-frame figures), as
capture.json:fps, or the capture alone when it has the titles. The title adds its Wws workloads in
one order, but the workload ids differ between processes (RPCS3 puts tasksets among them), so
workloads are named by their order among the Wws ones.

The EDGE lists' published and taken counts move together: a list's jobs are taken as they are
published, so "nothing pending" does not mean an EDGE list is idle. frames cuts a capture at each
RenderJobs publish (its ~40 jobs come at once, once a frame).
"""
import argparse, collections, ctypes, json, os, re, statistics, struct, sys, threading, time

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


def threads(pid):
    """{tid: (name, cpu seconds)} for a process's threads."""
    from ctypes import wintypes as W

    class TE32(ctypes.Structure):
        _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD),
                    ("th32OwnerProcessID", W.DWORD), ("tpBasePri", W.LONG), ("tpDeltaPri", W.LONG),
                    ("dwFlags", W.DWORD)]
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateToolhelp32Snapshot.restype = W.HANDLE
    k32.OpenThread.restype = W.HANDLE
    k32.GetThreadDescription.argtypes = [W.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    te = TE32()
    te.dwSize = ctypes.sizeof(te)
    res = {}
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            h = k32.OpenThread(0x0800, False, te.th32ThreadID)
            if h:
                c, e, kt, ut = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
                k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
                cpu = ((kt.dwHighDateTime << 32 | kt.dwLowDateTime) + (ut.dwHighDateTime << 32 | ut.dwLowDateTime)) / 1e7
                name, nm = ctypes.c_wchar_p(), ""
                if k32.GetThreadDescription(h, ctypes.byref(name)) >= 0 and name.value:
                    nm = name.value
                    k32.LocalFree(name)
                res[te.th32ThreadID] = (nm, cpu)
                k32.CloseHandle(h)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return res


def titles(pid):
    """The titles of a process's visible windows."""
    from ctypes import wintypes as W
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    got = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(h, l):
        p = W.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            b = ctypes.create_unicode_buffer(512)
            u32.GetWindowTextW(h, b, 512)
            if b.value:
                got.append(b.value)
        return True
    u32.EnumWindows(cb, 0)
    return got


def read_shared(path, pos):
    """A file's bytes from pos on, opened with read, write and delete sharing: RPCS3 holds TTY.log
    in a way open() cannot share."""
    import msvcrt
    from ctypes import wintypes as W
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = W.HANDLE
    k32.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, ctypes.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
    h = k32.CreateFileW(path, 0x80000000, 7, None, 3, 0, None)
    if h in (None, W.HANDLE(-1).value):
        return None
    with os.fdopen(msvcrt.open_osfhandle(h, os.O_RDONLY | os.O_BINARY), "rb") as f:
        f.seek(pos)
        return f.read()


def wait_line(path, needle):
    """Waits until needle is in the file; returns the time it was seen."""
    pos, n = 0, needle.encode()
    while True:
        try:
            b = read_shared(path, pos)
            if b:
                if n in b:
                    return time.time()
                pos += max(0, len(b) - len(n))
        except OSError:
            pass
        time.sleep(0.2)


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
    anchor = None
    if a.log:
        anchor = wait_line(a.log, a.needle)
        print("the level's line %.1f s after the process started" % (p.age() - (time.time() - anchor)), flush=True)
    else:
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
    if anchor is not None:
        while time.time() < anchor + a.delay:
            time.sleep(0.05)
    stop, tl = threading.Event(), []

    def title_loop():
        while not stop.is_set():
            tl.append((time.time(), titles(p.pid)))
            stop.wait(0.5)
    th0, wall0 = threads(p.pid), time.time()
    tt = threading.Thread(target=title_loop, daemon=True)
    tt.start()
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
    wall1, th1 = time.time(), threads(p.pid)
    stop.set()
    tt.join(2)
    thr = sorted(((tid, nm or th0.get(tid, ("", 0))[0], c1 - th0.get(tid, (nm, 0.0))[1]) for tid, (nm, c1) in th1.items()),
                 key=lambda x: -x[2])
    print("%d samples in %.1f s (%.1f us apart), %d changes; threads, share of a core:" % (n, dt, dt * 1e6 / n, len(ev)))
    for tid, nm, c in thr[:16]:
        print("  %6d %-40s %6.1f%%" % (tid, nm[:40], 100 * c / (wall1 - wall0)))
    print("  all threads %.1f%%" % (100 * sum(c for _, _, c in thr) / (wall1 - wall0)))
    json.dump({"proc": a.proc, "base": base, "spurs": ea, "age0": age0, "seconds": dt, "samples": n,
               "table": {str(w): list(v) for w, v in table.items()}, "rows": rows, "events": ev,
               "anchor": anchor, "wall0": wall0, "wall1": wall1, "threads": thr, "titles": tl},
              open(a.out, "w"))


# ---------------------------------------------------------------------------
# reports
# ---------------------------------------------------------------------------

def title_fps(d):
    """The mean FPS the window titles showed over a capture: RPCS3 writes "FPS: 21.34", the port
    "FPS: 21.3" too. None without titles."""
    vals, last = [], None
    for _, ts in d.get("titles") or []:
        for s in ts:
            m = re.search(r"FPS[:\s]+([0-9]+(?:\.[0-9]+)?)", s) or re.search(r"([0-9]+(?:\.[0-9]+)?)\s*FPS", s)
            if m:
                if s != last:
                    vals.append(float(m.group(1)))
                last = s
                break
    return statistics.mean(vals) if vals else None


class Capture:
    def __init__(self, arg, need_fps=True):
        path, fps = arg, None
        head, _, tail = arg.rpartition(":")
        if head and re.fullmatch(r"[0-9]+(\.[0-9]*)?", tail):
            path, fps = head, float(tail)
        d = json.load(open(path))
        if fps is None:
            fps = title_fps(d)
            if fps is None and need_fps:
                raise SystemExit("%s holds no titles: give its frame rate as %s:<fps>" % (path, path))
        self.path, self.fps = path, fps or 0.0
        self.d = d
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

    def ready(self, b, w):
        """The ready count: 0x00 for workloads 0-15, 0x10 for 16-31 (SPURS2)."""
        return b[(0x00 if w < 16 else 0x10) + (w & 15)]

    def counts(self, b, i):
        """Published and taken, over a workload's lists (16-bit counters, summed)."""
        pub = took = 0
        for l in range(4):
            ptr, p, t = self.row(b, i, l)
            if ptr:
                pub += p
                took += t
        return pub, took

    def pending(self, b, i):
        n = 0
        for l in range(4):
            ptr, p, t = self.row(b, i, l)
            if ptr:
                n += (p - t) & 0xFFFF
        return n


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


def cmd_states(a):
    """Who waits for whom: per workload, ms a frame it runs (an SPU in it), is ready with no SPU (split
    by whether all SPUs were in workloads then), or has jobs published and not taken with neither a
    ready count nor an SPU (blocked on something outside the SPUs)."""
    for c in (Capture(x) for x in a.captures):
        nspu = c.ev[0][1][0x76]
        st = {w: collections.Counter() for w in c.names}
        rdy = {w: collections.Counter() for w in c.names}
        g = collections.Counter()
        for b, dt in c.spans():
            used = sum(c.cont(b, w) for w in c.names)
            idle_ready = False
            for i, (w, _) in enumerate(c.rows):
                n, r = c.cont(b, w), c.ready(b, w)
                if n:
                    st[w]["run"] += dt
                elif r:
                    st[w]["full" if used >= nspu else "free"] += dt
                    idle_ready |= used < nspu
                elif c.pending(b, i):
                    st[w]["blocked"] += dt
                if r:
                    rdy[w][min(r, 9)] += dt
            if used < nspu:
                g["free"] += dt
                g["free_ready"] += dt * idle_ready
        f = c.frames
        print("== %s: %.2f flips/s, %.0f s, %d SPUs; fewer in workloads %.1f ms a frame, %.1f of it with one ready" % (
            c.path, c.fps, c.T, nspu, g["free"] * 1e3 / f, g["free_ready"] * 1e3 / f))
        print("%-13s %-16s %8s %11s %11s %9s   %s" % ("workload", "priority", "run ms/f", "ready, full", "ready, free",
                                                    "blocked", "ready count: share of the time"))
        for w in sorted(c.names):
            s = st[w]
            hist = " ".join("%d:%.0f%%" % (k, 100 * v / c.T) for k, v in sorted(rdy[w].items()) if v / c.T >= 0.01)
            print("%-13s %-16s %8.1f %11.1f %11.1f %9.1f   %s" % (
                c.names[w], c.d["table"][str(w)][2], s["run"] * 1e3 / f, s["full"] * 1e3 / f, s["free"] * 1e3 / f,
                s["blocked"] * 1e3 / f, hist))


def cmd_frames(a):
    """A capture cut into frames at each RenderJobs publish, and the median of each frame's parts."""
    def analyse(c):
        idx = {c.names[w]: (w, i) for i, (w, _) in enumerate(c.rows)}
        nspu = c.ev[0][1][0x76]
        ev = [(t / 1e9, b) for t, b in c.ev]
        rjw, rji = idx["RenderJobs"]
        edges = [idx[n] for n in ("EDGE0", "EDGE1", "EDGE2")]
        mpw, mpi = idx["MeshProc"]
        g6w, g6i = idx["General6"]
        starts, prev = [], None
        for k, (t, b) in enumerate(ev):
            p = c.counts(b, rji)[0]
            if prev is not None and 20 <= ((p - prev) & 0xFFFF) < 0x8000:
                starts.append(k)
            prev = p
        out = []
        for a0, z in zip(starts, starts[1:]):
            t0, t1 = ev[a0][0], ev[z][0]
            rj_pub = c.counts(ev[a0][1], rji)[0]
            f = collections.Counter(frame=t1 - t0)
            rj_done = mp_first = mp_last = g6_first = g6_last = edge_end = None
            e_prev = [c.counts(ev[a0][1], i)[1] for _, i in edges]
            mp_prev, g6_prev = c.counts(ev[a0][1], mpi)[1], c.counts(ev[a0][1], g6i)[1]
            for k in range(a0, z):
                t, b = ev[k]
                dt, rel = ev[k + 1][0] - t, t - t0
                if rj_done is None:
                    f["rj_spu"] += c.cont(b, rjw) * dt
                    left = (rj_pub - c.counts(b, rji)[1]) & 0xFFFF
                    if left == 0 or left >= 0x8000:
                        rj_done = rel
                ec = [c.cont(b, w) for w, _ in edges]
                if any(ec):
                    f["edge_busy"] += dt
                    edge_end = rel + dt
                f["edge_spu"] += sum(ec) * dt
                for j, (_, i) in enumerate(edges):
                    v = c.counts(b, i)[1]
                    d = (v - e_prev[j]) & 0xFFFF
                    f["edge_jobs"] += d if d < 0x8000 else 0
                    e_prev[j] = v
                if c.ready(b, mpw):
                    mp_first = rel if mp_first is None else mp_first
                    f["mp_wait"] += dt * (not c.cont(b, mpw))
                if c.counts(b, mpi)[1] != mp_prev:
                    mp_prev, mp_last = c.counts(b, mpi)[1], rel
                if c.ready(b, g6w):
                    g6_first = rel if g6_first is None else g6_first
                    f["g6_wait"] += dt * (not c.cont(b, g6w))
                if c.counts(b, g6i)[1] != g6_prev:
                    g6_prev, g6_last = c.counts(b, g6i)[1], rel
                f["free"] += max(0, nspu - sum(c.cont(b, w) for w in c.names)) * dt
            f.update(rj_drain=rj_done if rj_done is not None else t1 - t0, edge_end=edge_end or 0,
                     mp_first=mp_first if mp_first is not None else -1, mp_last=mp_last if mp_last is not None else -1,
                     g6_first=g6_first if g6_first is not None else -1, g6_last=g6_last if g6_last is not None else -1)
            f["rj_spus"] = f["rj_spu"] / rj_done if rj_done else 0.0
            out.append(f)
        return out
    caps = [(c.path, analyse(c)) for c in (Capture(x, need_fps=False) for x in a.captures)]
    rows = [("frame", "frame, ms", 1e3), ("rj_drain", "RenderJobs: publish to last job taken, ms", 1e3),
            ("rj_spus", "RenderJobs: SPUs meanwhile", 1), ("edge_busy", "EDGE: some list has an SPU, ms", 1e3),
            ("edge_jobs", "EDGE: jobs taken", 1), ("edge_spu", "EDGE: SPU time, ms", 1e3),
            ("edge_end", "EDGE: its last SPU of the frame ends, ms after the publish", 1e3),
            ("mp_first", "MeshProc: first ready, ms after the publish", 1e3), ("mp_wait", "MeshProc: ready with no SPU, ms", 1e3),
            ("mp_last", "MeshProc: last job taken, ms after the publish", 1e3),
            ("g6_first", "General6: first ready, ms after the publish", 1e3), ("g6_wait", "General6: ready with no SPU, ms", 1e3),
            ("g6_last", "General6: last job taken, ms after the publish", 1e3), ("free", "free SPU time, ms", 1e3)]
    print("%-58s" % "median of the frames" + "".join("%16s" % os.path.basename(p)[:15] for p, _ in caps))
    print("%-58s" % "frames" + "".join("%16d" % len(fr) for _, fr in caps))
    for k, label, s in rows:
        print("%-58s" % label + "".join("%16.1f" % (statistics.median(f[k] for f in fr) * s if fr else 0) for _, fr in caps))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("capture")
    p.add_argument("--proc", required=True)
    p.add_argument("--at", type=float, default=0, help="start when the process is this many seconds old")
    p.add_argument("--log", help="start --delay s after --needle shows in this log (RPCS3's TTY.log, the port's output)")
    p.add_argument("--needle", default="RestoreStreamedGONetworkID", help="the level's TTY line (default: the first level's)")
    p.add_argument("--delay", type=float, default=66)
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--out", required=True)
    p.add_argument("--elf", default=ELF)
    for name in ("report", "jobs", "states"):
        q = sub.add_parser(name)
        q.add_argument("captures", nargs="+", help="capture.json[:flips_per_s]")
    for name in ("edge", "frames"):
        q = sub.add_parser(name)
        q.add_argument("captures", nargs="+", help="capture.json")
    a = ap.parse_args()
    {"capture": cmd_capture, "report": cmd_report, "jobs": cmd_jobs, "edge": cmd_edge,
     "states": cmd_states, "frames": cmd_frames}[a.cmd](a)


if __name__ == "__main__":
    main()
