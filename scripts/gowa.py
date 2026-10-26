#!/usr/bin/env python3
"""gowa.py - one entry point for the God of War: Ascension (NPUA80918) port pipeline.

    python scripts/gowa.py doctor                  check tools, submodule and game files
    python scripts/gowa.py setup --game-dir DIR    link your installed game into vfs/ and gamedata/
    python scripts/gowa.py analyze                 loader, SPU images, HLE coverage -> analysis/
    python scripts/gowa.py lift                    PPU -> C++ -> recompiled/
    python scripts/gowa.py build                   configure + build with clang-cl -> build/
    python scripts/gowa.py run                     start the port, log to logs/
    python scripts/gowa.py run --trace             the same, with an ftrace of the session (stutter)

The program that is ported is USRDIR/GOWA.SELF. The title's EBOOT.BIN is a
launcher of a few KB that starts GOWA.SELF; the port starts GOWA.SELF itself.

Every file made from the game (vfs/, gamedata/, analysis/, recompiled/, spu/,
build/, logs/) is git-ignored and must stay on your machine.

Needs Python 3.10+ with ps3recomp/requirements.txt installed (the ps3recomp
tools use 3.10 syntax).
"""
import argparse
import bisect
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PS3RECOMP = os.path.join(ROOT, "ps3recomp")
TOOLS = os.path.join(PS3RECOMP, "tools")
LOCAL = os.path.join(ROOT, "local.json")
ANALYSIS = os.path.join(ROOT, "analysis")
RECOMPILED = os.path.join(ROOT, "recompiled")
BUILD = os.path.join(ROOT, "build")
VFS_GAME = os.path.join(ROOT, "vfs", "PS3_GAME")
HDD0 = os.path.join(ROOT, "gamedata", "dev_hdd0")

# The release this port is written against. A different GOWA.SELF lifts to
# different addresses, so every curated function bound, hook and patch would
# be wrong for it.
TITLE_ID = "NPUA80918"
APP_VER = "01.12"
GAME_ELF = "GOWA.elf"
GAME_ELF_SHA256 = "4bf75c897b82d1beb25514cbadaff3d03e1b0c7bf849fa5d289be4f9183462b5"


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def say(msg):
    print(msg, flush=True)


def die(msg):
    say("error: " + msg)
    sys.exit(1)


def run(cmd, **kw):
    say("> " + " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], **kw).returncode


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def parse_sfo(path):
    d = open(path, "rb").read()
    _, _, key_tab, data_tab, count = struct.unpack("<4sIIII", d[:20])
    out = {}
    for i in range(count):
        ko, fmt, ln, _, do = struct.unpack("<HHIII", d[20 + i * 16:36 + i * 16])
        key = d[key_tab + ko:d.index(b"\0", key_tab + ko)].decode()
        raw = d[data_tab + do:data_tab + do + ln]
        out[key] = (struct.unpack("<I", raw)[0] if fmt == 0x0404
                    else raw.rstrip(b"\0").decode("utf-8", "replace"))
    return out


def code_end(elf):
    """End of the last executable section (SHF_EXECINSTR), or None.

    The R-X segment also holds .rodata and the embedded SPU images. Without a
    bound the lifter treats the whole segment as code, and branch-looking words
    in the data become bogus functions. Section flags give the bound even where
    section names are stripped."""
    with open(elf, "rb") as f:
        hdr = f.read(64)
        shoff, = struct.unpack(">Q", hdr[0x28:0x30])
        shentsize, shnum = struct.unpack(">HH", hdr[0x3A:0x3E])
        if not shoff or not shnum:
            return None
        f.seek(shoff)
        table = f.read(shentsize * shnum)
    end = 0
    for i in range(shnum):
        sh = table[i * shentsize:(i + 1) * shentsize]
        flags, addr = struct.unpack(">QQ", sh[8:24])
        size, = struct.unpack(">Q", sh[32:40])
        if flags & 0x4:
            end = max(end, addr + size)
    return end or None


def load_local():
    if os.path.exists(LOCAL):
        return json.load(open(LOCAL))
    return {}


def game_elf():
    return os.path.join(VFS_GAME, "USRDIR", GAME_ELF)


def need_elf():
    elf = game_elf()
    if not os.path.exists(elf):
        die("%s not found. Run `setup --game-dir ...` first, and decrypt "
            "USRDIR/GOWA.SELF to GOWA.elf with RPCS3 (Utilities > Decrypt PS3 "
            "Binaries)." % os.path.relpath(elf, ROOT))
    return elf


def link_dir(link, target):
    """Directory junction on Windows (no admin needed), symlink elsewhere."""
    if os.path.lexists(link):
        if os.path.realpath(link) == os.path.realpath(target):
            return
        die("%s already exists and points somewhere else; remove it first" % link)
    os.makedirs(os.path.dirname(link), exist_ok=True)
    if os.name == "nt":
        rc = subprocess.run(["cmd", "/c", "mklink", "/J", link, target],
                            capture_output=True, text=True)
        if rc.returncode:
            die("mklink /J failed: " + (rc.stderr or rc.stdout).strip())
    else:
        os.symlink(target, link, target_is_directory=True)
    say("linked %s -> %s" % (os.path.relpath(link, ROOT), target))


def find_ffmpeg():
    """A shared FFmpeg build (headers + DLLs) for the runtime's video and audio
    decoding: local.json's "ffmpeg_dir", or the one folder under ../ffmpeg.
    The runtime loads the DLLs at run time, so the headers only have to match
    the DLLs found on PATH then. Returns the build's root, or None."""
    cands = [load_local().get("ffmpeg_dir")]
    base = os.path.join(os.path.dirname(ROOT), "ffmpeg")
    if os.path.isdir(base):
        cands += [os.path.join(base, d) for d in sorted(os.listdir(base))]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "include", "libavcodec", "avcodec.h")):
            return c
    return None


def toolchain_env():
    """MSVC environment (vcvars) plus a clang-cl that may live outside it.

    Besides the copy Visual Studio installs, a portable LLVM is picked up from
    local.json's "llvm_bin", $GOWA_LLVM_BIN, or ../llvm/bin next to the repo,
    so no admin install is needed."""
    sys.path.insert(0, TOOLS)
    from msvc_env import environ
    env = environ()
    for cand in (load_local().get("llvm_bin"), os.environ.get("GOWA_LLVM_BIN"),
                 os.path.join(os.path.dirname(ROOT), "llvm", "bin")):
        if cand and os.path.isfile(os.path.join(cand, "clang-cl.exe" if os.name == "nt" else "clang")):
            env["PATH"] = cand + os.pathsep + env.get("PATH", "")
            break
    return env


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_doctor(args):
    ok = True

    def check(label, good, detail=""):
        nonlocal ok
        ok &= bool(good)
        say("  [%s] %-22s %s" % ("ok" if good else "!!", label, detail))

    say("tools")
    check("python >= 3.10", sys.version_info >= (3, 10), sys.version.split()[0])
    try:
        import capstone  # noqa: F401
        check("ps3recomp requirements", True, "capstone " + capstone.__version__)
    except ImportError:
        check("ps3recomp requirements", False,
              "pip install -r ps3recomp/requirements.txt")
    for exe in ("git", "cmake", "ninja"):
        path = shutil.which(exe)
        check(exe, path, path or "not on PATH")
    env = toolchain_env()
    clang = shutil.which("clang-cl", path=env.get("PATH")) if os.name == "nt" else shutil.which("clang")
    check("clang-cl" if os.name == "nt" else "clang", clang,
          clang or "install 'C++ Clang Compiler for Windows' in the VS Installer, "
                   "or extract a portable LLVM to ../llvm (see PLAN.md)")
    if os.name == "nt":
        check("MSVC environment", env.get("INCLUDE"), "vcvars64.bat found"
              if env.get("INCLUDE") else "install VS Build Tools with C++ workload")

    say("repository")
    check("ps3recomp submodule", os.path.exists(os.path.join(TOOLS, "ppu_lifter.py")),
          "git submodule update --init" if not os.path.exists(TOOLS) else
          subprocess.run(["git", "-C", PS3RECOMP, "rev-parse", "--abbrev-ref", "HEAD"],
                         capture_output=True, text=True).stdout.strip())

    say("game")
    sfo = os.path.join(VFS_GAME, "PARAM.SFO")
    if os.path.exists(sfo):
        s = parse_sfo(sfo)
        check("title", s.get("TITLE_ID") == TITLE_ID, "%s %s" % (s.get("TITLE_ID"), s.get("TITLE")))
        check("version", s.get("APP_VER") == APP_VER, "APP_VER %s (port targets %s)"
              % (s.get("APP_VER"), APP_VER))
    else:
        check("vfs/PS3_GAME", False, "run `setup --game-dir ...`")
    elf = game_elf()
    if os.path.exists(elf):
        h = sha256(elf)
        check(GAME_ELF, h == GAME_ELF_SHA256,
              "sha256 matches" if h == GAME_ELF_SHA256 else "sha256 %s differs" % h[:16])
    else:
        check(GAME_ELF, False, "decrypt USRDIR/GOWA.SELF with RPCS3")

    say("machine")
    say("  %s, %d logical CPUs" % (platform.processor() or platform.machine(), os.cpu_count()))
    free = shutil.disk_usage(ROOT).free / 2**30
    check("free disk", free > 20, "%.0f GB free on the repo drive (lift + build want ~20 GB)" % free)
    say("\n" + ("all good" if ok else "fix the [!!] lines above"))
    return 0 if ok else 1


def cmd_setup(args):
    game = os.path.abspath(args.game_dir or load_local().get("game_dir", ""))
    if not os.path.isfile(os.path.join(game, "PARAM.SFO")):
        die("no PARAM.SFO in %r; pass the installed game folder, e.g. "
            "<RPCS3>/dev_hdd0/game/%s" % (game, TITLE_ID))
    s = parse_sfo(os.path.join(game, "PARAM.SFO"))
    if s.get("TITLE_ID") != TITLE_ID:
        die("that folder is %s, not %s" % (s.get("TITLE_ID"), TITLE_ID))
    local = load_local()
    local["game_dir"] = game
    json.dump(local, open(LOCAL, "w"), indent=1)
    link_dir(VFS_GAME, game)
    link_dir(os.path.join(HDD0, "game", TITLE_ID), game)
    os.makedirs(os.path.join(HDD0, "home", "00000001", "savedata"), exist_ok=True)
    if not os.path.exists(game_elf()):
        say("note: USRDIR/GOWA.elf is missing; decrypt GOWA.SELF with RPCS3 next")
    say("setup done: saves go to gamedata/, the game folder is only read")
    return 0


def cmd_analyze(args):
    elf = need_elf()
    py = sys.executable
    os.makedirs(ANALYSIS, exist_ok=True)
    if run([py, os.path.join(TOOLS, "ppu_loader.py"), elf, "-o", ANALYSIS]):
        die("ppu_loader failed")
    spu = os.path.join(ANALYSIS, "spu")
    shutil.rmtree(spu, ignore_errors=True)
    if run([py, os.path.join(TOOLS, "extract_spu_images.py"), elf, "--output", spu]):
        die("extract_spu_images failed")
    with open(os.path.join(ANALYSIS, "hle_coverage.txt"), "w") as out:
        rc = run([py, os.path.join(ROOT, "tools", "hle_coverage.py")],
                 stdout=out, stderr=subprocess.STDOUT)
    say(open(os.path.join(ANALYSIS, "hle_coverage.txt")).read())
    return rc


def cmd_lift(args):
    elf = need_elf()
    funcs = os.path.join(ANALYSIS, "GOWA.functions.json")
    imports = os.path.join(ANALYSIS, "GOWA.imports.json")
    if not (os.path.exists(funcs) and os.path.exists(imports)):
        die("run `analyze` first")
    shutil.rmtree(RECOMPILED, ignore_errors=True)
    cmd = [sys.executable, os.path.join(TOOLS, "ppu_lifter.py"), elf,
           "--functions", funcs, "--hle-stubs", imports,
           "--output", RECOMPILED, "--jobs", args.jobs, "--chunks", args.chunks]
    end = code_end(elf)
    if end:
        say("code ends at 0x%X (last executable section)" % end)
        cmd += ["--code-end", "0x%X" % end]
    rc = run(cmd)
    if rc:
        die("ppu_lifter failed")
    files = [f for f in os.listdir(RECOMPILED) if f.endswith((".c", ".cpp"))]
    size = sum(os.path.getsize(os.path.join(RECOMPILED, f)) for f in files) / 2**20
    say("lifted: %d source files, %.0f MB in recompiled/" % (len(files), size))
    say("re-run `build` so CMake picks up the new file list")
    return 0


def cmd_build(args):
    if not os.path.exists(os.path.join(RECOMPILED, "ppu_recomp.h")):
        die("no lifted code; run `lift` first")
    env = toolchain_env()
    compiler = "clang-cl" if os.name == "nt" else "clang"
    if not shutil.which(compiler, path=env.get("PATH")):
        die("%s not found; see `doctor`" % compiler)
    if args.fresh:
        shutil.rmtree(BUILD, ignore_errors=True)
    cxx = compiler if os.name == "nt" else "clang++"
    # Configure every time: it's quick once cached, and it keeps the SPU
    # setting in step with the command line. PORT_SPU_INPUTS makes the
    # template scan the game ELF for embedded SPU ELFs, lift them into
    # build/generated_spu/ and register them.
    spu_inputs = need_elf() if not args.no_spu else ""
    ffmpeg = find_ffmpeg()
    if ffmpeg:
        ff_opts = ["-DPS3RECOMP_FFMPEG_RUNTIME=ON",
                   "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=" + os.path.join(ffmpeg, "include")]
    else:
        say("note: no FFmpeg build in ../ffmpeg; movies and compressed audio won't decode")
        ff_opts = ["-DPS3RECOMP_FFMPEG_RUNTIME=OFF"]
    rc = run(["cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja",
              "-DCMAKE_BUILD_TYPE=" + args.config,
              "-DCMAKE_C_COMPILER=" + compiler, "-DCMAKE_CXX_COMPILER=" + cxx,
              "-DPS3RECOMP_DIR=" + PS3RECOMP, "-DRECOMP_DIR=" + RECOMPILED,
              "-DPORT_SPU_INPUTS=" + spu_inputs,
              "-DPython3_EXECUTABLE=" + sys.executable] + ff_opts, env=env)
    if rc:
        die("cmake configure failed")
    # The lifted TUs are huge: each clang-cl job can take several GB, so the
    # default stays well under the CPU count on a 16 GB machine.
    rc = run(["cmake", "--build", BUILD, "-j", args.jobs], env=env)
    if not rc:
        write_settings(ffmpeg)
    return rc


def ini_set(lines, section, key, value, keep=False):
    """Set section.key in an INI kept as its lines, as the runtime's
    ps3_settings does: in place, else at the end of the section, else in a new
    section at the end. keep=True leaves a value that is already there."""
    sec, last, at = None, -1, -1
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("[") and "]" in s:
            sec = s[1:s.index("]")].strip().lower()
            if sec == section:
                last = i
            continue
        if sec != section or not s or s[0] in ";#" or "=" not in s:
            continue
        last = i
        if at < 0 and s.split("=", 1)[0].strip().lower() == key:
            at = i
    quote = value != value.strip() or any(c in value for c in ' ;#"')
    text = ('%s = "%s"' if quote else "%s = %s") % (key, value)
    if at >= 0:
        if not keep:
            lines[at] = text
    elif last >= 0:
        lines.insert(last + 1, text)
    else:
        if lines and lines[-1].strip():
            lines.append("")
        lines += ["[%s]" % section, text]


def write_settings(ffmpeg):
    """build/gowa.ini: this machine's paths, so GowARecomp.exe starts
    without this script. A player's settings in the file are kept."""
    path = os.path.join(BUILD, "gowa.ini")
    lines = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    else:
        lines = ["; GowARecomp settings. [paths] is written by `gowa.py build`;",
                 "; the other sections are the player's.", ""]
    ini_set(lines, "paths", "elf", game_elf())
    ini_set(lines, "paths", "vfs", os.path.join(ROOT, "vfs"))
    ini_set(lines, "paths", "hdd0", HDD0)
    if ffmpeg:
        ini_set(lines, "paths", "ffmpeg", os.path.join(ffmpeg, "bin"))
    ini_set(lines, "graphics", "shader_cache", os.path.join(BUILD, "d3d12_shader_cache.bin"), keep=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    say("settings: " + os.path.relpath(path, ROOT))


def cmd_run(args):
    elf = need_elf()
    exe = os.path.join(BUILD, "GowARecomp" + (".exe" if os.name == "nt" else ""))
    if not os.path.exists(exe):
        die("no build; run `build` first")
    env = dict(os.environ)
    env.setdefault("PS3_VFS_ROOT", os.path.join(ROOT, "vfs"))
    env.setdefault("PS3_HDD0_ROOT", HDD0)
    ffmpeg = find_ffmpeg()
    if ffmpeg:
        env["PATH"] = os.path.join(ffmpeg, "bin") + os.pathsep + env.get("PATH", "")
    # What the title itself needs (the boot path and the runtime switches) is
    # set by main.cpp, so the executable also starts without this script.
    # Keep the compiled guest shaders between runs: a scene's first draws
    # otherwise compile its programs on the drain thread. The file is built
    # from the game's own programs, so it lives under build/, which git ignores.
    env.setdefault("D3D12_SHADER_CACHE", os.path.join(BUILD, "d3d12_shader_cache.bin"))
    # The runtime logs every lv2 wait and SPURS poll whenever stderr is
    # redirected, which it always is here (the log file). That is ~20k lines a
    # second through one CRT FILE lock that every guest thread shares; the
    # diagnostics this port relies on are switched on by their own variables.
    env.setdefault("PS3_VERBOSE", "1" if args.verbose else "0")
    logs = os.path.join(ROOT, "logs")
    os.makedirs(logs, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    log = os.path.join(logs, stamp + ".log")
    say("log: " + os.path.relpath(log, ROOT))
    if getattr(args, "trace", False):
        # For stutter in a hand-played session: an ftrace of the whole run,
        # flips included, written when the window is closed. 12M spans is
        # about half an hour of gameplay.
        trace = os.path.join(logs, stamp + ".ftrace.txt")
        env.setdefault("PS3_FTRACE", "0:86400:" + trace.replace("\\", "/"))
        env.setdefault("PS3_FTRACE_CAP", str(12 << 20))
        say("trace: " + os.path.relpath(trace, ROOT))
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        p = subprocess.Popen([exe, "--no-launcher", elf] + args.extra, cwd=ROOT, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding="utf-8", errors="replace")
        timer = None
        if args.seconds:
            # A boot that hangs never closes its window; stop it so the log
            # can be read.
            timer = threading.Timer(args.seconds, p.kill)
            timer.start()
        for line in p.stdout:
            f.write(line)
            if not args.quiet:
                sys.stdout.write(line)
        rc = p.wait()
        if timer:
            timer.cancel()
    killed = " (stopped after %ds)" % args.seconds if args.seconds and rc in (1, -9) else ""
    say("exit code %d%s, log in %s" % (rc, killed, os.path.relpath(log, ROOT)))
    return rc


def cmd_symbolize(args):
    """Name the runtime-code RVAs in a PS3_SAMPLE report ("[samp] rva 0x...")
    from the linker map the build writes next to the executable. Only valid
    for the build that produced the log."""
    mp = os.path.join(BUILD, "GowARecomp.map")
    if not os.path.exists(mp):
        die("no %s; rebuild (the map is written at link time)" % os.path.relpath(mp, ROOT))
    base, syms = 0x140000000, []
    sym = re.compile(r"^\s*0001:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s+(\S+)")
    with open(mp, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Preferred load address is" in line:
                base = int(line.split()[-1], 16)
            m = sym.match(line)
            if m:
                syms.append((int(m.group(2), 16) - base, m.group(1), m.group(3).split(":")[-1]))
    syms.sort()
    starts = [s[0] for s in syms]
    log = args.log or max((os.path.join(ROOT, "logs", n) for n in os.listdir(os.path.join(ROOT, "logs"))
                           if n.endswith(".log")), key=os.path.getmtime)
    rva = re.compile(r"\[samp\] rva 0x([0-9A-Fa-f]+)\s+([\d.]+)%")
    for line in open(log, encoding="utf-8", errors="replace"):
        if line.startswith("[samp] --- report"):
            print(line.rstrip())
        m = rva.search(line)
        if not m:
            continue
        r = int(m.group(1), 16)
        i = bisect.bisect_right(starts, r) - 1
        name = "%s+0x%X (%s)" % (syms[i][1], r - syms[i][0], syms[i][2]) if i >= 0 else "?"
        print("  %6s%%  0x%08X  %s" % (m.group(2), r, name))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("doctor", help="check tools, submodule and game files")
    p = sub.add_parser("setup", help="link the installed game into vfs/ and gamedata/")
    p.add_argument("--game-dir", help="installed game folder (holds PARAM.SFO and USRDIR/)")
    sub.add_parser("analyze", help="loader, SPU images, HLE coverage -> analysis/")
    p = sub.add_parser("lift", help="PPU -> C++ in recompiled/")
    p.add_argument("--jobs", default="6", help="lifter worker processes (default 6)")
    p.add_argument("--chunks", default="96",
                   help="number of source files to split into (default 96; more "
                        "files = less memory per compile job)")
    p = sub.add_parser("build", help="configure + build with clang-cl")
    p.add_argument("--jobs", default="3", help="parallel compile jobs (default 3 for 16 GB RAM)")
    p.add_argument("--config", default="Release", choices=["Release", "RelWithDebInfo", "Debug"])
    p.add_argument("--fresh", action="store_true", help="delete build/ and configure again")
    p.add_argument("--no-spu", action="store_true",
                   help="don't lift and register the game ELF's embedded SPU programs")
    p = sub.add_parser("run", help="start the port")
    p.add_argument("--quiet", action="store_true", help="write the log only, not the console")
    p.add_argument("--seconds", type=int, default=0, help="stop the port after this many seconds")
    p.add_argument("--verbose", action="store_true",
                   help="log every lv2 wait and SPURS poll (PS3_VERBOSE=1; slow)")
    p.add_argument("--trace", action="store_true",
                   help="ftrace the whole session to logs/<stamp>.ftrace.txt, for stutter")
    p.add_argument("extra", nargs="*", help="extra arguments after the ELF path")
    p = sub.add_parser("symbolize", help="name the runtime addresses in a PS3_SAMPLE report")
    p.add_argument("log", nargs="?", help="run log (default: the newest in logs/)")
    args = ap.parse_args()
    return {"doctor": cmd_doctor, "setup": cmd_setup, "analyze": cmd_analyze,
            "lift": cmd_lift, "build": cmd_build,
            "run": cmd_run, "symbolize": cmd_symbolize}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
