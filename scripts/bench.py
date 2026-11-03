"""Level benchmark: drive the port to the first level and measure flips/s.

    gowa.py bench                                   # build/GowARecomp.exe, one run
    gowa.py bench --runs 2 --variant "a|build/ab_a.exe|" --variant "b|build/ab_b.exe|RSX_ZPASS=0"
    gowa.py bench --baseline logs/bench-<stamp>/result.json --shots 75,140 --gpu 100
    gowa.py bench --variant "fsr|||--set graphics.scaling=fsr1 --set display.window_size=1600x900"

A run starts the executable with PAD_FILE input: Square every 5 s from 15 s
until the opening movie's video decoder closes, then Cross on the main menu
(Single Player), the profiles (Profile 1) and Profile 1's menu, whose cursor
rests on Continue Game: its checkpoint loads R_MEGAERA03, the opening
cinematic runs to ~45 s after the choice, and the Megaera fight follows.
(--profile N: Down N-1 times on the profiles, for another profile's
checkpoint. Where a checkpoint is depends on the saves, so a run on a copy of
them, --set paths.hdd0=<copy>, keeps the player's own.)
The menu answers at once (screenshots every 4 s: the main menu is up 2 s
after the movie), so the three presses come 6 s apart from 6 s after it, and
the choice is confirmed by the thread it starts
(SaveDataUtil::LoadFileSaveProfile): a press the menu missed is made again,
6 s later, up to three times. Every later time counts from that thread's
start (the load): a Cross 24 s after it, as the old route's fourth press
fell in the cinematic, and flips/s from GOWA_THREADS' 10 s windows from
--from (default 69) to --until (default --from + 80) seconds after it. That
is the scene the old fixed route measured (Cross at 45, 80, 105 and 130 s
after the movie, windows from 175 s), ~85 s sooner: a run takes 3:20 instead
of ~5:40. The level windows begin ~120 s after the process starts (for
guestscope.py capture --at). --route T1,T2,... presses
at fixed times after the movie instead, and counts from the movie.

Why 80 s of level: between two runs of one executable the medians differ by
~1.3% (median of 123 pairs since 2026-10-30), and that comes from the runs,
not from how many windows each has: 8 windows give the same spread as 12
(1v1 1.12% against 1.29%, 2v2 0.92% against 0.95%).

--early-stop: an A/B's first round (a, b) already decides when the two differ
by at least that much (default 6%: 3 of the 123 same-executable pairs did,
none by 7%), and the rest of the rounds are skipped.

A run that never reaches the level (fewer than 3 windows, or a median of 30
flips/s or more: stuck on a menu or loading screen) or freezes in it (median
of 3 or less, or no flips for a minute) is reported as failed and run once
more. Variants are run in ABBA order (a b, then b a, ...), the way A/Bs are
judged: compare medians, two runs a side at least. In plain alternation
(a b a b) the first variant of each round came out ahead 6 times in 6, by
1.7-2.6%, for builds the ABBA order then found within 1% of each other.

Everything goes to logs/bench-<stamp>/: the logs of each run, screenshots,
and result.json (the windows, per-thread CPU, memory, GPU utilisation).
"""
import ctypes
import datetime
import hashlib
import json
import os
import re
import shlex
import statistics
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")

CROSS, SQUARE, DOWN = 0x4000, 0x8000, 0x0040
MENU = (6, 12, 18)                  # Cross: Single Player, Profile 1, Continue Game (s after the movie)
LOADING = b"SaveDataUtil::LoadFileSaveProfile"   # the thread Continue Game starts
CINEMATIC_PRESS = 24                # s after the load: the old route's fourth Cross
WM_CLOSE, WM_KEYDOWN, VK_F12 = 0x0010, 0x0100, 0x7B


def add_args(p):
    p.add_argument("--variant", action="append", default=[],
                   help='"name|exe|ENV=1;ENV2=0|ARGS" (exe relative to the repo; default '
                        '"base|build/GowARecomp.exe|"; ARGS: the executable\'s own, e.g. '
                        '"--set graphics.antialiasing=smaa"; gowa.ini is not changed); repeat for an A/B')
    p.add_argument("--runs", type=int, default=1, help="runs per variant, alternated (default 1)")
    p.add_argument("--env", action="append", default=[], help="K=V for every variant")
    p.add_argument("--profile", type=int, default=1,
                   help="the profile the menu route continues (default 1): Down that many times "
                        "less one on the profiles, between the first two Crosses")
    p.add_argument("--route", default=None,
                   help="Cross at these seconds after the movie (e.g. the old 45,80,105,130), "
                        "counting every later time from the movie; default: the menu route, "
                        "counting from the level load")
    p.add_argument("--from", dest="win_from", type=int, default=None,
                   help="first window, seconds after the load (default 69), or after the movie "
                        "with --route (default the last press + 45)")
    p.add_argument("--until", type=int, default=None,
                   help="stop this many seconds after the load or the movie (default --from + 80)")
    p.add_argument("--early-stop", type=float, default=6.0,
                   help="with two variants, stop after the first round when they differ by at "
                        "least this many percent (default 6; 0 runs every round)")
    p.add_argument("--shots", default="",
                   help="screenshots at these seconds after the load (or the movie with --route), e.g. 75,140")
    p.add_argument("--gpu", type=int, default=0,
                   help="sample the GPU engine counters for 20 s from this many seconds after the load "
                        "(or the movie with --route)")
    p.add_argument("--baseline", help="a result.json to compare the first variant with")
    p.add_argument("--out", help="output folder (default logs/bench-<stamp>)")
    p.add_argument("--no-retry", action="store_true", help="do not run a failed run again")


# -- Windows helpers (ctypes) -------------------------------------------------

user32 = ctypes.windll.user32 if os.name == "nt" else None
kernel32 = ctypes.windll.kernel32 if os.name == "nt" else None
if kernel32:
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]


class _RECT(ctypes.Structure):
    _fields_ = [("l", ctypes.c_long), ("t", ctypes.c_long), ("r", ctypes.c_long), ("b", ctypes.c_long)]


def main_window(pid):
    """The visible top-level window of pid with the largest client area."""
    best = [None, 0]
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(h, _):
        wp = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(h), ctypes.byref(wp))
        if wp.value == pid and user32.IsWindowVisible(ctypes.c_void_p(h)):
            rc = _RECT()
            user32.GetClientRect(ctypes.c_void_p(h), ctypes.byref(rc))
            a = (rc.r - rc.l) * (rc.b - rc.t)
            if a > best[1]:
                best[0], best[1] = h, a
        return True

    user32.EnumWindows(proto(cb), None)
    return best[0]


def post(pid, msg, wparam=0):
    h = main_window(pid)
    if h:
        user32.PostMessageA(ctypes.c_void_p(h), msg, ctypes.c_void_p(wparam), None)
    return bool(h)


class _MEMSTATUS(ctypes.Structure):
    _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                ("avail", ctypes.c_ulonglong), ("pagetotal", ctypes.c_ulonglong),
                ("pageavail", ctypes.c_ulonglong), ("virttotal", ctypes.c_ulonglong),
                ("virtavail", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]


def free_ram_mb():
    m = _MEMSTATUS()
    m.len = ctypes.sizeof(m)
    kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.avail // (1 << 20)


class _PMC(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("faults", ctypes.c_ulong), ("peakws", ctypes.c_size_t),
                ("ws", ctypes.c_size_t), ("a", ctypes.c_size_t), ("b", ctypes.c_size_t),
                ("c", ctypes.c_size_t), ("d", ctypes.c_size_t), ("pagefile", ctypes.c_size_t),
                ("peakpagefile", ctypes.c_size_t), ("private", ctypes.c_size_t)]


def private_mb(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)        # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return 0
    pmc = _PMC()
    pmc.cb = ctypes.sizeof(pmc)
    ok = ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(h), ctypes.byref(pmc), pmc.cb)
    kernel32.CloseHandle(h)
    return pmc.private // (1 << 20) if ok else 0


def running(image):
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", "IMAGENAME eq " + image],
                         capture_output=True, text=True).stdout
    return image.lower() in out.lower()


def gpu_sample(pid, seconds, result):
    """GPU engine utilisation of pid (Windows performance counters), averaged per engine type."""
    ps = ("Get-Counter -Counter '\\GPU Engine(pid_%d_*)\\Utilization Percentage' -SampleInterval 1 "
          "-MaxSamples %d -ErrorAction SilentlyContinue | ForEach-Object { $_.CounterSamples | "
          "ForEach-Object { '{0}|{1}|{2}' -f $_.Timestamp.ToString('HH:mm:ss'), $_.InstanceName, "
          "$_.CookedValue } }") % (pid, seconds)
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                         timeout=seconds + 60).stdout
    per = {}
    for line in out.splitlines():
        f = line.split("|")
        m = re.search(r"luid_(0x[0-9a-f]+_0x[0-9a-f]+).*engtype_(\w+)", f[1]) if len(f) == 3 else None
        if m:
            key = m.group(1) + " " + m.group(2)
            per.setdefault(key, {}).setdefault(f[0], 0.0)
            per[key][f[0]] += float(f[2].replace(",", "."))
    for key, by_t in per.items():
        v = list(by_t.values())
        if max(v) > 0.5:
            result[key] = {"avg": round(sum(v) / len(v), 1), "max": round(max(v), 1)}
    try:
        nv = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu,clocks.sm,clocks.max.sm,pstate",
                             "--format=csv,noheader"], capture_output=True, text=True, timeout=20).stdout
        result["nvidia-smi"] = nv.strip()
    except (OSError, subprocess.SubprocessError):
        pass


# -- one run ------------------------------------------------------------------

THREADS_RE = re.compile(r"^\[threads\] t=(\d+)s flips=([\d.]+)/s")
# The same line's presents, and the flips never presented (runtimes that count them).
PRESENTS_RE = re.compile(r"presents=([\d.]+)/s")
SKIPPED_RE = re.compile(r"skipped=([\d.]+)/s")
TABLE_RE = re.compile(r"^\[threads\]\s+([\d.]+)%\s+(.+?)\s+x(\d+)\s*$")


def run_once(exe, env_extra, a, out, tag):
    """Start exe, drive it to the level, measure, stop. Returns the run's record."""
    rec = {"tag": tag, "exe": os.path.relpath(exe, ROOT), "env": env_extra}
    image = os.path.basename(exe)
    for _ in range(600):
        if not running(image):
            break
        time.sleep(1)
    else:
        rec["failed"] = "another %s is still running" % image
        return rec
    for _ in range(24):
        if free_ram_mb() >= 4096:
            break
        time.sleep(5)
    pad = os.path.join(out, tag + ".pad")
    open(pad, "w").close()
    env = dict(os.environ)
    env.update({"GOWA_THREADS": "10", "D3D12_VSYNC": "0", "PAD_FILE": pad})
    env.update(env_extra)
    log_out, log_err = os.path.join(out, tag + ".log"), os.path.join(out, tag + ".err")
    shots_dir = os.path.join(os.path.dirname(exe), "screenshots")
    with open(log_out, "w") as fo, open(log_err, "w") as fe:
        args = shlex.split(env_extra.get("GOWA_BENCH_ARGS", ""), posix=False)
        p = subprocess.Popen([exe, "--no-launcher"] + args, cwd=os.path.dirname(exe), env=env, stdout=fo, stderr=fe)
    t0 = time.monotonic()

    def t():
        return time.monotonic() - t0

    def log_has(pat):
        with open(log_out, "r", errors="replace") as f:
            return pat in f.read()

    def press(mask, n=10):
        with open(pad, "w") as f:
            f.write("0x%04X %d" % (mask, n))

    # Skip the movie: Square every 5 s from 15 s until the video decoder closes.
    mc = None
    nxt = 15.0
    while p.poll() is None and t() < 240:
        if log_has("cellVdec] Close"):
            mc = t()
            break
        if t() >= 100 and not log_has("cellVdec] Open"):
            break
        if t() >= nxt:
            press(SQUARE)
            nxt += 5
        time.sleep(0.5)
    if mc is None:
        rec["failed"] = "no movie" if p.poll() is None else "exited at %.0f s" % t()
        stop(p)
        return rec
    rec["movie_closed_s"] = round(mc, 1)
    if a.route_s is None:
        # The menu route: Cross until Continue Game starts loading the level.
        loading = seen(log_err, LOADING)
        presses, extra, last, anchor = list(MENU), 0, t(), None
        # --profile: Down a second apart from 3 s after the first Cross, all before the second.
        downs = [MENU[0] + 3 + k for k in range(max(0, min(a.profile, 4) - 1))]
        while p.poll() is None:
            if loading():
                anchor = t()
                break
            if downs and t() >= mc + downs[0]:
                downs.pop(0)
                press(DOWN, 4)
            elif presses and t() >= mc + presses[0]:
                presses.pop(0)
                press(CROSS)
                last = t()
            elif not presses and t() - last >= 6:
                if extra == 3:
                    break
                press(CROSS)
                extra += 1
                last = t()
            time.sleep(0.25)
        if anchor is None:
            if p.poll() is not None:
                rec["failed"] = "exited at %.0f s" % t()
            else:
                last = [f for _, f in windows(log_err)][-2:]
                rec["failed"] = ("frozen in the menus (no flips)" if len(last) == 2 and max(last) < 0.5
                                 else "the menu route did not reach Continue Game")
            stop(p)
            return rec
        rec["load_s"] = round(anchor, 1)
        rec["menu_presses"] = len(MENU) + extra
        plan = [(anchor + CINEMATIC_PRESS, "press")]
        level_from = anchor + 20
    else:
        anchor = mc
        plan = [(mc + s, "press") for s in a.route_s]
        level_from = mc + a.route_s[-1] + 20
    plan += [(anchor + int(s), "shot %s" % s) for s in a.shots.split(",") if s.strip()]
    if a.gpu:
        plan.append((anchor + a.gpu, "gpu"))
    plan.sort()
    gpu, gpu_thread, peak_priv, least_free, frozen = {}, None, 0, 1 << 30, False
    last_mem = 0.0
    while p.poll() is None and t() < anchor + a.until:
        while plan and t() >= plan[0][0]:
            _, what = plan.pop(0)
            if what == "press":
                press(CROSS)
            elif what == "gpu":
                gpu_thread = threading.Thread(target=gpu_sample, args=(p.pid, 20, gpu), daemon=True)
                gpu_thread.start()
            else:
                take_shot(p.pid, shots_dir, os.path.join(out, "%s_%s.png" % (tag, what.split()[1])))
        if t() - last_mem >= 10:
            last_mem = t()
            peak_priv = max(peak_priv, private_mb(p.pid))
            least_free = min(least_free, free_ram_mb())
            # Frozen in the level: no flips for a minute.
            w = windows(log_err)
            late = [f for (s, f) in w if s >= level_from]
            if len(late) >= 6 and max(late[-6:]) < 0.5:
                frozen = True
                break
        time.sleep(0.5)
    exited = p.poll() is not None
    stop(p)
    if gpu_thread:
        gpu_thread.join(timeout=90)
    w = [(round(s - anchor), f) for (s, f) in windows(log_err) if s >= anchor + a.win_from]
    vals = sorted(f for _, f in w)
    shown = [(p, k) for (s, p, k) in shown_windows(log_err) if s >= anchor + a.win_from]
    if shown:
        rec["presents_windows"] = [p for p, _ in shown]
        rec["presents"] = round(statistics.median(p for p, _ in shown), 2)
        if all(k is not None for _, k in shown):
            rec["skipped_windows"] = [k for _, k in shown]
            rec["skipped"] = round(statistics.median(k for _, k in shown), 2)
    rec.update({"windows": w, "peak_private_mb": peak_priv, "least_free_mb": least_free,
                "threads": thread_table(log_err, anchor + a.win_from), "gpu": gpu})
    if vals:
        rec.update({"median": round(statistics.median(vals), 2), "mean": round(statistics.mean(vals), 2),
                    "min": vals[0], "max": vals[-1]})
    if frozen:
        rec["failed"] = "frozen: no flips for a minute"
    elif exited and t() < anchor + a.until:
        rec["failed"] = "exited at %.0f s" % t()
    elif len(vals) < 3 or not 3 < statistics.median(vals) < 30:
        rec["failed"] = "never reached the level" if len(vals) < 3 or statistics.median(vals) >= 30 else "frozen"
    return rec


def seen(path, pat):
    """A test for pat in the file at path, reading only what was added since the last call."""
    state = {"pos": 0, "tail": b""}

    def test():
        with open(path, "rb") as f:
            f.seek(state["pos"])
            new = f.read()
        state["pos"] += len(new)
        hit = pat in state["tail"] + new
        state["tail"] = (state["tail"] + new)[-len(pat):]
        return hit
    return test


def stop(p):
    if p.poll() is None:
        post(p.pid, WM_CLOSE)
        for _ in range(6):
            if p.poll() is not None:
                return
            time.sleep(0.5)
        p.kill()
        p.wait()


def windows(log_err):
    out = []
    with open(log_err, "r", errors="replace") as f:
        for line in f:
            m = THREADS_RE.match(line)
            if m:
                out.append((int(m.group(1)), float(m.group(2))))
    return out


def shown_windows(log_err):
    """(t, presents/s, skipped flips/s or None) for each window of the thread table."""
    out = []
    with open(log_err, "r", errors="replace") as f:
        for line in f:
            m = THREADS_RE.match(line)
            p = m and PRESENTS_RE.search(line)
            if p:
                k = SKIPPED_RE.search(line)
                out.append((int(m.group(1)), float(p.group(1)), float(k.group(1)) if k else None))
    return out


def thread_table(log_err, from_s):
    """Average CPU per named host thread over the windows from from_s (GOWA_THREADS tables)."""
    sums, n, on = {}, 0, False
    with open(log_err, "r", errors="replace") as f:
        for line in f:
            m = THREADS_RE.match(line)
            if m:
                on = int(m.group(1)) >= from_s
                n += on
                continue
            m = TABLE_RE.match(line) if on else None
            if m:
                sums[m.group(2)] = sums.get(m.group(2), 0.0) + float(m.group(1))
    return {k: round(v / n, 1) for k, v in sorted(sums.items(), key=lambda kv: -kv[1]) if n and v / n >= 2}


def take_shot(pid, shots_dir, dest):
    """F12: the port writes the frame into screenshots/ at its next present."""
    before = set(os.listdir(shots_dir)) if os.path.isdir(shots_dir) else set()
    if not post(pid, WM_KEYDOWN, VK_F12):
        return
    for _ in range(60):
        time.sleep(0.5)
        now = set(os.listdir(shots_dir)) if os.path.isdir(shots_dir) else set()
        new = sorted(now - before)
        if new:
            time.sleep(0.5)
            with open(os.path.join(shots_dir, new[-1]), "rb") as s, open(dest, "wb") as d:
                d.write(s.read())
            return


# -- the command ----------------------------------------------------------------

def parse_variant(s):
    f = s.split("|")
    name, exe = f[0], f[1] if len(f) > 1 and f[1] else os.path.join("build", "GowARecomp.exe")
    env = {}
    for kv in (f[2].split(";") if len(f) > 2 else []):
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k.strip()] = v
    # The fourth field: the executable's own arguments, kept with the run's env record.
    if len(f) > 3 and f[3].strip():
        env["GOWA_BENCH_ARGS"] = f[3].strip()
    return name, os.path.join(ROOT, exe) if not os.path.isabs(exe) else exe, env


def sha12(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:12]


def git_head(path):
    r = subprocess.run(["git", "-C", path, "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip()


def line(rec):
    if "windows" not in rec or not rec.get("windows"):
        return "%s FAILED: %s" % (rec["tag"], rec.get("failed", "?"))
    s = "%s movie=%s median %.2f mean %.2f (%d windows %.2f-%.2f) peak private %d MB" % (
        rec["tag"], rec.get("movie_closed_s"), rec.get("median", 0), rec.get("mean", 0), len(rec["windows"]),
        rec.get("min", 0), rec.get("max", 0), rec.get("peak_private_mb", 0))
    if "presents" in rec:
        s += " | presents %.2f" % rec["presents"]
    if "skipped" in rec:
        s += ", skipped %.2f" % rec["skipped"]
    if rec.get("failed"):
        s += " FAILED: " + rec["failed"]
    return s


def main(a):
    a.route_s = [float(x) for x in a.route.split(",") if x.strip()] if a.route else None
    if a.win_from is None:
        a.win_from = int(a.route_s[-1] + 45) if a.route_s else 69
    if a.until is None:
        a.until = a.win_from + 80
    variants = [parse_variant(v) for v in (a.variant or ["base||"])]
    common = dict(kv.split("=", 1) for kv in a.env if "=" in kv)
    for name, exe, _ in variants:
        if not os.path.isfile(exe):
            print("no executable for %s: %s" % (name, exe))
            return 1
    # Absolute: the game runs in its executable's folder and reads PAD_FILE from there.
    out = os.path.abspath(a.out or os.path.join(ROOT, "logs", "bench-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    result = {"started": datetime.datetime.now().isoformat(timespec="seconds"),
              "port": git_head(ROOT), "runtime": git_head(os.path.join(ROOT, "ps3recomp")),
              "route": a.route or "menu", "anchor": "movie" if a.route else "load",
              "from": a.win_from, "until": a.until, "runs": []}
    for name, exe, env in variants:
        result.setdefault("exes", {})[name] = {"path": os.path.relpath(exe, ROOT), "sha256": sha12(exe)}
    for i in range(a.runs):
        # ABBA: every second round in reverse, so neither a variant's place in a
        # round nor a slow drift of the machine (heat, background work) favours one.
        for name, exe, env in (variants if i % 2 == 0 else variants[::-1]):
            e = dict(common)
            e.update(env)
            for attempt in range(1 if a.no_retry else 2):
                tag = "%s%d" % (name, i + 1) + ("_retry" if attempt else "")
                rec = run_once(exe, e, a, out, tag)
                rec["variant"] = name
                result["runs"].append(rec)
                print(line(rec), flush=True)
                with open(os.path.join(out, "result.json"), "w") as f:
                    json.dump(result, f, indent=1)
                if not rec.get("failed"):
                    break
        if i == 0 and a.runs > 1 and len(variants) == 2 and a.early_stop > 0:
            first = [next((r for r in result["runs"] if r["variant"] == n and not r.get("failed")), None)
                     for n, _, _ in variants]
            if all(first):
                m0, m1 = first[0]["median"], first[1]["median"]
                d = 100.0 * (m1 - m0) / ((m0 + m1) / 2)
                if abs(d) >= a.early_stop:
                    print("== the first round differs by %+.1f%% (--early-stop %g): no more rounds"
                          % (d, a.early_stop), flush=True)
                    result["early_stop"] = round(d, 2)
                    break
    print("== by variant (medians of all windows of the good runs)")
    summary = {}
    for name, _, _ in variants:
        good = [r for r in result["runs"] if r["variant"] == name and not r.get("failed")]
        vals = [f for r in good for _, f in r["windows"]]
        if vals:
            summary[name] = {"runs": len(good), "windows": len(vals), "median": round(statistics.median(vals), 2),
                             "run_medians": [r["median"] for r in good]}
            extra = ""
            pv = [p for r in good for p in r.get("presents_windows", [])]
            if pv:
                summary[name]["presents"] = round(statistics.median(pv), 2)
                extra += " | presents %.2f" % summary[name]["presents"]
            kv = [k for r in good for k in r.get("skipped_windows", [])]
            if kv:
                summary[name]["skipped"] = round(statistics.median(kv), 2)
                extra += ", skipped %.2f" % summary[name]["skipped"]
            print("  %-10s %d runs, %3d windows, median %.2f (runs %s)%s" % (
                name, len(good), len(vals), summary[name]["median"],
                ", ".join("%.2f" % m for m in summary[name]["run_medians"]), extra))
        else:
            print("  %-10s no good run" % name)
    result["summary"] = summary
    if a.baseline and variants and variants[0][0] in summary:
        with open(a.baseline) as f:
            base = json.load(f)
        bs = base.get("summary", {})
        b = bs.get(variants[0][0]) or (next(iter(bs.values())) if bs else None)
        if b:
            m0, m1 = b["median"], summary[variants[0][0]]["median"]
            print("== against %s: %.2f -> %.2f flips/s (%+.1f%%)" % (a.baseline, m0, m1, 100.0 * (m1 - m0) / m0))
            result["baseline"] = {"file": a.baseline, "median": m0}
    with open(os.path.join(out, "result.json"), "w") as f:
        json.dump(result, f, indent=1)
    print("results: " + os.path.relpath(out, ROOT))
    return 0
