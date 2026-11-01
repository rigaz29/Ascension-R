#!/usr/bin/env python3
"""gowa.py - one entry point for the God of War: Ascension (NPUA80918) port pipeline.

    python scripts/gowa.py doctor                  check tools, submodule and game files
    python scripts/gowa.py setup --game-dir DIR    link your installed game into vfs/ and gamedata/
    python scripts/gowa.py analyze                 loader, SPU images, HLE coverage -> analysis/
    python scripts/gowa.py lift                    PPU -> C++ -> recompiled/
    python scripts/gowa.py lle                     lift firmware/libsre.prx (SPURS queues)
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402  (scripts/bench.py)
import frames  # noqa: E402  (scripts/frames.py)

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


# The Wws_Job SPURS workload's policy module: a raw SPU image (no ELF header)
# in the game ELF's read-only data, which the title hands to
# cellSpursAddWorkloadWithAttribute for each of its job workloads (collision,
# rendering, mesh processing and more; 13 at boot). The runtime finds a
# workload's lifted PM by an FNV-1a-64 of exactly these bytes, so it is
# registered under that fingerprint rather than the wrapped ELF's. A newer
# build than the God of War HD one (12288 bytes there 11520).
WWS_PM_VADDR = 0x7A7B00
WWS_PM_SIZE = 12288
WWS_PM_FNV = 0xC8D774963A96DCF2
WWS_PM_BASE = 0xA00          # policy modules load and enter at LS 0xA00
WWS_PM_IMAGE_ID = 0x57575350
WWS_PM_DIR = os.path.join(ROOT, "spu", "wwspm")
# spu_lifter.py --float-spec for every SPU image: a function with no store, channel op, call
# or internal branch and at least this many single-precision float ops gets a speculative body
# (registers in locals, native float ops checked once at each exit, the exact body as the
# fallback). General6's hot leaf (jobmod a67e549fa0ab, 0x4F00, ~1,200 calls a job) is one: its
# jobs -12% on the replay, -10% in the game. Needs SPU_FLOAT_RZ_ONLY, which the port builds with.
SPU_FLOAT_SPEC = 4
# Entries the function finder doesn't reach: indirect branch targets that
# missed the lifted set in the first boots. 0xA2C is the interrupt vector.
WWS_PM_EXTRA_FUNCS = [0xA2C, 0x2D98, 0x3368]


def fnv1a64(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def lift_wws_pm(elf):
    pm = open(elf, "rb").read()[WWS_PM_VADDR - 0x10000:WWS_PM_VADDR - 0x10000 + WWS_PM_SIZE]
    fp = fnv1a64(pm)
    if fp != WWS_PM_FNV:
        die("the Wws_Job policy module at 0x%X doesn't match (fnv 0x%016X)" % (WWS_PM_VADDR, fp))
    raw = os.path.join(ANALYSIS, "spu", "wwsjob_pm.bin")
    wrapped = os.path.join(ANALYSIS, "spu", "wwsjob_pm.elf")
    os.makedirs(os.path.dirname(raw), exist_ok=True)
    open(raw, "wb").write(pm)
    if run([sys.executable, os.path.join(TOOLS, "wrap_spu_elf.py"), raw, "--out", wrapped,
            "--base", "0x%X" % WWS_PM_BASE, "--entry", "0x%X" % WWS_PM_BASE]):
        die("wrap_spu_elf failed")
    shutil.rmtree(WWS_PM_DIR, ignore_errors=True)
    os.makedirs(WWS_PM_DIR)
    if run([sys.executable, os.path.join(TOOLS, "spu_lifter.py"), wrapped,
            "--auto-functions", wrapped, "--symbol-prefix", "wwspm_", "-o", WWS_PM_DIR,
            "--extra-funcs", ",".join("0x%X" % a for a in WWS_PM_EXTRA_FUNCS),
            "--float-spec", str(SPU_FLOAT_SPEC)]):
        die("spu_lifter failed on the Wws_Job policy module")
    sys.path.insert(0, TOOLS)
    from build_spu_workloads import debase_includes
    for f in ("spu_recomp.h", "spu_recomp.c"):
        debase_includes(os.path.join(WWS_PM_DIR, f))
    entry = "wwspm_spu_func_%08X" % WWS_PM_BASE
    jobs = wws_job_modules(elf)
    regions = "".join("    spu_overlay_register_region(0x%08XU, 0x%X, %d);  /* %s */\n"
                      % (j["ea"], j["span"], j["image_id"], j["name"]) for j in jobs)
    with open(os.path.join(WWS_PM_DIR, "wwspm_register.c"), "w", newline="\n") as f:
        f.write("/* Generated by scripts/gowa.py lift. Registers the lifted Wws_Job policy\n"
                " * module under the fingerprint of its raw bytes, which is what\n"
                " * cellSpursAddWorkload looks it up by.\n"
                " *\n"
                " * The module streams job code into LS 0x%X with plain GETs from the\n"
                " * game ELF's copy of each job module's code segment. Those modules are lifted\n"
                " * by the runtime's SPU pipeline under their own image ids; registering\n"
                " * each segment as a code region makes whichever one is resident dispatch\n"
                " * to its lifted code instead of the SPU interpreter. */\n"
                "#include \"spu_workload.h\"\n\n"
                "extern void spu_begin_image(int image_id);\n"
                "extern void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id);\n"
                "extern void %s(spu_context*);\n"
                "extern void wwspm_spu_recomp_register(void);\n\n"
                "__attribute__((constructor)) static void gowa_register_wws_pm(void)\n"
                "{\n"
                "    spu_begin_image(%d);\n"
                "    wwspm_spu_recomp_register();\n"
                "    spu_workload_register_img(0x%016XULL, %s, %d, \"wwsjob_pm\");\n"
                "    spu_begin_image(0);\n"
                "%s"
                "}\n" % (WWS_JOB_CODE_LS, entry, WWS_PM_IMAGE_ID, WWS_PM_FNV, entry,
                         WWS_PM_IMAGE_ID, regions))
    say("Wws_Job policy module lifted to %s (%d job modules registered as code regions)"
        % (os.path.relpath(WWS_PM_DIR, ROOT), len(jobs)))


# Where the Wws_Job policy module streams job code. Job modules embedded in the
# game ELF are linked there (none in this title's four embedded SPU ELFs, which
# are linked at 0x3000, so the list below may well be empty).
WWS_JOB_CODE_LS = 0x4000


def wws_job_modules(elf):
    """The embedded SPU ELFs linked at the Wws_Job code buffer, with the guest
    address of their code segment and the image id the runtime's SPU pipeline
    lifts them under (same derivation as tools/spu_program_pipeline.py)."""
    sys.path.insert(0, TOOLS)
    from spu_elf import scan_spu_candidates
    buf = open(elf, "rb").read()
    phoff, = struct.unpack_from(">Q", buf, 0x20)
    phentsize, phnum = struct.unpack_from(">HH", buf, 0x36)
    loads = []
    for i in range(phnum):
        p_type, _, p_off, p_vaddr, _, p_filesz, _, _ = struct.unpack_from(">IIQQQQQQ", buf, phoff + i * phentsize)
        if p_type == 1:
            loads.append((p_off, p_vaddr, p_filesz))

    def ea_of(off):
        for p_off, p_vaddr, p_filesz in loads:
            if p_off <= off < p_off + p_filesz:
                return p_vaddr + off - p_off
        return None

    jobs = []
    for off, info, err in scan_spu_candidates(buf):
        if err or len(info["executable"]) != 1:
            continue
        seg = info["executable"][0]
        if seg["addr"] != WWS_JOB_CODE_LS:
            continue
        # Span the code sections only: the job DMAs its own data into the
        # tail of the segment, and a GET there from another address would
        # mark the code as overwritten.
        code_end = max(s["addr"] + s["size"] for s in info["sections"] if s["flags"] & 4)
        digest = hashlib.sha256(buf[off:off + info["size"]]).hexdigest()
        ea = ea_of(off + seg["off"])
        if ea is None:
            die("SPU job module at 0x%X is outside the game ELF's load segments" % off)
        jobs.append(dict(name="spu_" + digest[:16], ea=ea, span=code_end - seg["addr"],
                         image_id=0x10000000 | int(digest[:7], 16)))
    return jobs


# The WWS job modules linked into the game ELF (header magic 0xC0DEC0DE, link
# base 0x4000). The Wws_Job policy module streams each from its address into
# local store; lifted and registered as verified regions, they run as native
# code instead of in the SPU interpreter.
JOBMODS_DIR = os.path.join(ROOT, "spu", "jobmods")
# Job entries the function finder doesn't reach, per module: indirect branch
# targets that missed the lifted set in the first boots (callbacks the job API
# calls through a register, the return points of indirect calls). Modules
# overlap in local store, so each list goes to its own module only. Kept in
# scripts/jobmods_extra.json; `jobmods-entries` adds what a run's
# SPU_INTERP_ENTRIES log shows. A missing one is not only slow: the
# interpreter that runs it goes on into the lifted caller's frame and can end
# the policy module's run there.
JOBMOD_EXTRA_FILE = os.path.join(ROOT, "scripts", "jobmods_extra.json")


def jobmod_extra_funcs():
    """{module name: [LS address, ...]} from scripts/jobmods_extra.json."""
    if not os.path.exists(JOBMOD_EXTRA_FILE):
        return {}
    return {k: [int(a, 16) for a in v] for k, v in json.load(open(JOBMOD_EXTRA_FILE)).items()}


# Modules left in the interpreter because their lift misbehaves, by header EA.
JOBMOD_SKIP = {}


def lift_jobmods(elf):
    shutil.rmtree(JOBMODS_DIR, ignore_errors=True)
    extra = ["--skip-ea", ",".join("%X" % ea for ea in sorted(JOBMOD_SKIP))] if JOBMOD_SKIP else []
    for name, addrs in sorted(jobmod_extra_funcs().items()):
        extra += ["--extra-funcs", name + "=" + ",".join("0x%X" % a for a in addrs)]
    if run([sys.executable, os.path.join(TOOLS, "lift_jobmods.py"), "--engine-elf", elf,
            "--out", JOBMODS_DIR, "--register", os.path.join(JOBMODS_DIR, "jobmods_register.c"),
            "--float-spec", str(SPU_FLOAT_SPEC)] + extra):
        die("lift_jobmods failed")
    n = len(json.load(open(os.path.join(JOBMODS_DIR, "jobmods_manifest.json"))))
    say("%d WWS job modules lifted to %s" % (n, os.path.relpath(JOBMODS_DIR, ROOT)))
    native_hooks(JOBMODS_DIR)


# Native code for chosen lifted SPU functions (native/, built with the lifted SPU
# code's options): scripts/spu_native_hooks.json names, per lifted module, the
# function and the native routine called at its entry. A routine does what work
# it can and leaves the registers as the lifted code expects them, which then
# goes on (native/spu_edge_skin.c). A module is a job module (spu/jobmods) or,
# named pic_<EA>, a position-independent piece (spu/picfrags).
NATIVE_HOOKS_FILE = os.path.join(ROOT, "scripts", "spu_native_hooks.json")


def native_hooks(lift_dir, pic=False):
    if not os.path.exists(NATIVE_HOOKS_FILE):
        return
    for module, funcs in sorted(json.load(open(NATIVE_HOOKS_FILE)).items()):
        if module.startswith("pic_") != pic:
            continue
        path = os.path.join(lift_dir, module, "spu_recomp.c")
        if not os.path.exists(path):
            die("native hook: no lifted module %s" % module)
        src = open(path, encoding="utf-8").read()
        for func, native in sorted(funcs.items()):
            head = "void %s(spu_context* SPU_RESTRICT ctx) {\n" % func
            if src.count(head) != 1:
                head = "void %s(spu_context* ctx) {\n" % func   # lifted before SPU_RESTRICT
            if src.count(head) != 1:
                die("native hook: %s not found once in %s" % (func, module))
            src = src.replace(head, "void %s(spu_context* ctx);\n%s    %s(ctx);\n" % (native, head, native))
            say("native hook: %s -> %s" % (func, native))
        open(path, "w", encoding="utf-8", newline="\n").write(src)


# The position-independent code pieces the EDGE geometry jobs stream out of the
# executable to wherever they have room (LS 0x11000-0x12400): lifted once each
# and dispatched wherever they land, instead of interpreted. The list -- EA,
# size, entry offset -- is in scripts/spu_pic_pieces.json, found by running the
# title (docs/notes/2026-10-27-l2-analysis.md).
PIC_DIR = os.path.join(ROOT, "spu", "picfrags")
PIC_LIST = os.path.join(ROOT, "scripts", "spu_pic_pieces.json")


def lift_pic(elf):
    shutil.rmtree(PIC_DIR, ignore_errors=True)
    if run([sys.executable, os.path.join(TOOLS, "lift_spu_pic.py"), "--elf", elf,
            "--list", PIC_LIST, "--out", PIC_DIR, "--float-spec", str(SPU_FLOAT_SPEC)]):
        die("lift_spu_pic failed")
    n = len(json.load(open(os.path.join(PIC_DIR, "pic_manifest.json"))))
    say("%d position-independent SPU pieces lifted to %s" % (n, os.path.relpath(PIC_DIR, ROOT)))
    native_hooks(PIC_DIR, pic=True)


def cmd_jobmods_entries(args):
    """Merge the [interp-entry] lines of runs made with
    SPU_INTERP_ENTRIES=4000-40000 into scripts/jobmods_extra.json: each entry
    inside a lifted job module (image id 200 + its place in the manifest)
    becomes an extra function of that module. Lift and build again after."""
    man = json.load(open(os.path.join(JOBMODS_DIR, "jobmods_manifest.json")))
    by_img = {200 + i: m["name"] for i, m in enumerate(man)}
    extra = jobmod_extra_funcs()
    added = {}
    pat = re.compile(r"\[interp-entry\] pc=0x([0-9A-F]+) owner=(\d+)")
    for log in args.logs:
        for line in open(log, encoding="utf-8", errors="replace"):
            mm = pat.search(line)
            if not mm:
                continue
            pc, img = int(mm.group(1), 16), int(mm.group(2))
            name = by_img.get(img)
            if not name:
                continue
            code_end = next(m["code_end"] for m in man if m["name"] == name)
            if pc < 0x4030 or pc >= code_end or pc in extra.get(name, []):
                continue
            extra.setdefault(name, []).append(pc)
            added.setdefault(name, []).append(pc)
    data = {k: ["0x%X" % a for a in sorted(set(v))] for k, v in sorted(extra.items())}
    open(JOBMOD_EXTRA_FILE, "w", encoding="utf-8", newline="\n").write(json.dumps(data, indent=1) + "\n")
    for name, pcs in sorted(added.items()):
        say("%s: +%d (%s)" % (name, len(pcs), " ".join("0x%X" % a for a in sorted(pcs))))
    say("%d new entries in %d modules -> %s" % (sum(map(len, added.values())), len(added),
                                                os.path.relpath(JOBMOD_EXTRA_FILE, ROOT)))
    return 0


def cmd_lift(args):
    elf = need_elf()
    if args.jobmods_only:
        lift_wws_pm(elf)
        lift_jobmods(elf)
        lift_pic(elf)
        say("re-run `build` so CMake picks up the new file list")
        return 0
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
    # The main thread's polling loops call gowa_poll_wait (poll_wait.cpp).
    hooks = os.path.join(ROOT, "scripts", "ppu_hooks.json")
    if os.path.exists(hooks):
        cmd += ["--insn-hooks", hooks]
    rc = run(cmd)
    if rc:
        die("ppu_lifter failed")
    files = [f for f in os.listdir(RECOMPILED) if f.endswith((".c", ".cpp"))]
    size = sum(os.path.getsize(os.path.join(RECOMPILED, f)) for f in files) / 2**20
    say("lifted: %d source files, %.0f MB in recompiled/" % (len(files), size))
    lift_wws_pm(elf)
    lift_jobmods(elf)
    lift_pic(elf)
    say("re-run `build` so CMake picks up the new file list")
    return 0


# libsre (the firmware's SPURS/sync library) is relocated here. 0x28000000 is
# outside main memory, where the title's own heap lives, and clear of the GCM
# window at 0x20000000 and the raw-SPU windows at 0x30000000.
LIBSRE_BASE = 0x28000000
LIBSRE = os.path.join(ROOT, "firmware", "libsre")

# The libsre exports that run as the real firmware code. The ANY2ANY lock-free
# queue protocol (GetPushPointer2/CompletePushPointer2 behind PushBody) isn't
# documented anywhere and the SPU side of it is Sony's own code, so the PPU side
# has to be Sony's too, and so does the initializer that lays the queue out.
# Everything else in cellSpurs stays HLE.
LIBSRE_LLE_EXPORTS = {
    0x011EE38B: "_cellSpursLFQueueInitialize",
    0x8A85674D: "_cellSpursLFQueuePushBody",
}
# _cellSpursSendSignal is what CompletePushPointer2 calls (through a function
# pointer) to wake a task parked on the queue. It is bridged to the HLE handler,
# because the HLE task model is what parks and wakes the SPU task threads.
LIBSRE_SEND_SIGNAL_NID = 0xE0A6DBE4


def cmd_lle(args):
    prx = os.path.join(ROOT, "firmware", "libsre.prx")
    if not os.path.exists(prx):
        die("firmware/libsre.prx not found. Copy dev_flash/sys/external/libsre.sprx "
            "from RPCS3 to firmware/ and decrypt it there (Utilities > Decrypt PS3 Binaries)")
    py = sys.executable
    os.makedirs(LIBSRE, exist_ok=True)
    if run([py, os.path.join(TOOLS, "lift_prx.py"), prx, "--base", "0x%X" % LIBSRE_BASE,
            "--output", LIBSRE]):
        die("lift_prx failed")
    exports = json.load(open(os.path.join(LIBSRE, "libsre_exports.json")))
    toc = exports["toc"]

    # lift_prx writes {library: {nid: stub}}; the lifter wants [{library, nid, stub}].
    stubs = [{"library": lib, "nid": nid, "stub": stub}
             for lib, table in json.load(open(os.path.join(LIBSRE, "libsre_imports.json"))).items()
             for nid, stub in table.items()]
    stubs_path = os.path.join(LIBSRE, "libsre_hle_stubs.json")
    json.dump(stubs, open(stubs_path, "w"), indent=1)

    lifted = os.path.join(LIBSRE, "lifted")
    shutil.rmtree(lifted, ignore_errors=True)
    if run([py, os.path.join(TOOLS, "ppu_lifter.py"), os.path.join(LIBSRE, "libsre_image.bin"),
            "--raw", "--base", "0x%X" % LIBSRE_BASE, "--toc", toc,
            "--functions", os.path.join(LIBSRE, "libsre_functions.json"),
            "--hle-stubs", stubs_path, "--symbol-prefix", "libsre_",
            "--header-name", "libsre_recomp.h", "--source-name", "libsre_recomp",
            "--output", lifted]):
        die("ppu_lifter failed on libsre")

    nid_opd = {}
    for table in exports["exports"].values():
        for nid, opd in table.items():
            nid_opd[int(nid, 16)] = int(opd, 16)
    missing = [n for n in list(LIBSRE_LLE_EXPORTS) + [LIBSRE_SEND_SIGNAL_NID] if n not in nid_opd]
    if missing:
        die("libsre doesn't export " + ", ".join("0x%08X" % n for n in missing))
    send_signal_code = int(exports["opd_to_code"]["0x%08X" % nid_opd[LIBSRE_SEND_SIGNAL_NID]], 16)

    # PushBody passes CompletePushPointer[2] a function pointer it loads from
    # its TOC; that's the hook that wakes the queue's waiter. Read the load out
    # of PushBody itself ("lwz rN,-off(r2)" right before the call) rather than
    # trusting a fixed offset, and check the target lands in libsre's code.
    image = open(os.path.join(LIBSRE, "libsre_image.bin"), "rb").read()
    rd32 = lambda a: struct.unpack(">I", image[a - LIBSRE_BASE:a - LIBSRE_BASE + 4])[0]
    toc_v = int(toc, 16)
    push_code = int(exports["opd_to_code"]["0x%08X" % nid_opd[0x8A85674D]], 16)
    toc_loads = []
    for a in range(push_code, push_code + 0x200, 4):
        insn = rd32(a)
        if insn >> 26 == 32 and (insn >> 16) & 0x1F == 2:        # lwz rD, d(r2)
            toc_loads.append(toc_v + struct.unpack(">h", struct.pack(">H", insn & 0xFFFF))[0])
    hooks = set()
    for slot in toc_loads:
        opd = rd32(slot)
        if LIBSRE_BASE <= opd < LIBSRE_BASE + len(image) - 8:
            code = rd32(opd)
            if LIBSRE_BASE <= code < LIBSRE_BASE + len(image) and code != push_code:
                hooks.add(code)
    if len(hooks) != 1:
        die("can't identify the LFQueue signal hook in PushBody (candidates: %s)"
            % ", ".join("0x%08X" % h for h in sorted(hooks)))
    lfqueue_signal_code = hooks.pop()

    rows = "\n".join('    { 0x%08Xu, 0x%08Xu, "%s" },' % (nid, nid_opd[nid], name)
                     for nid, name in LIBSRE_LLE_EXPORTS.items())
    with open(os.path.join(lifted, "gowa_libsre_exports.c"), "w", newline="\n") as f:
        f.write("/* Generated by scripts/gowa.py lle. Do not edit. */\n"
                "#include \"gowa_lle.h\"\n\n"
                "const gowa_lle_export gowa_libsre_lle[] = {\n%s\n};\n"
                "const uint32_t gowa_libsre_lle_count = %d;\n"
                "const uint32_t gowa_libsre_base = 0x%08Xu;\n"
                "const uint32_t gowa_libsre_send_signal_code = 0x%08Xu;\n"
                "const uint32_t gowa_libsre_lfqueue_signal_code = 0x%08Xu;\n"
                % (rows, len(LIBSRE_LLE_EXPORTS), LIBSRE_BASE, send_signal_code,
                   lfqueue_signal_code))
    say("libsre lifted to %s (TOC %s); LLE exports: %s" % (
        os.path.relpath(lifted, ROOT), toc, ", ".join(LIBSRE_LLE_EXPORTS.values())))
    say("re-run `build` to link it in")
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
              "-DPORT_SPU_LINES=" + ("ON" if args.spu_lines else "OFF"),
              "-DPORT_SPU_JOBCAP=" + ("ON" if args.jobcap else "OFF"),
              "-DPORT_SPU_FLOAT_APPROX=" + ("ON" if args.spu_approx else "OFF"),
              "-DPython3_EXECUTABLE=" + sys.executable] + ff_opts
             + ["-D" + d for d in args.define], env=env)
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
    ini_set(lines, "paths", "libsre", os.path.join(LIBSRE, "libsre_image.bin"))
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
    env.setdefault("GOWA_LIBSRE_IMAGE", os.path.join(LIBSRE, "libsre_image.bin"))
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


def cmd_replay(args):
    """SPU_JOB_REPLAY: run the jobs an SPU_JOB_CAPTURE run recorded, alone, and report the
    time per job and whether each one's output came out the same (runtime/spu/spu_jobcap.c)."""
    exe = os.path.abspath(args.exe or os.path.join(BUILD, "GowARecomp" + (".exe" if os.name == "nt" else "")))
    if not os.path.isfile(exe):
        die("no executable: " + exe)
    env = dict(os.environ)
    env["SPU_JOB_REPLAY"] = os.path.abspath(args.dir)
    env["SPU_JOB_REPLAY_REPS"] = str(args.reps)
    if args.only:
        env["SPU_JOB_REPLAY_ONLY"] = args.only
    p = subprocess.run([exe, "--no-launcher"], cwd=os.path.dirname(exe), env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, errors="replace")
    for line in p.stderr.splitlines():
        if line.startswith("[replay"):
            print(line)
    return p.returncode


def llvm_tool(name):
    """An LLVM tool from the same places toolchain_env looks for clang-cl."""
    exe = name + (".exe" if os.name == "nt" else "")
    for cand in (load_local().get("llvm_bin"), os.environ.get("GOWA_LLVM_BIN"),
                 os.path.join(os.path.dirname(ROOT), "llvm", "bin")):
        if cand and os.path.isfile(os.path.join(cand, exe)):
            return os.path.join(cand, exe)
    return shutil.which(name)


def spu_line_frames(rvas):
    """RVA -> the frames llvm-symbolizer gives for it (innermost first) as
    (function, file, line), from the PDB a PORT_SPU_LINES build writes."""
    exe = os.path.join(BUILD, "GowARecomp.exe")
    if not os.path.exists(os.path.join(BUILD, "GowARecomp.pdb")):
        die("no build/GowARecomp.pdb: build with --spu-lines")
    sym = llvm_tool("llvm-symbolizer")
    if not sym:
        die("llvm-symbolizer not found")
    base = 0x140000000
    rvas = sorted(rvas)
    p = subprocess.run([sym, "--obj=" + exe, "--output-style=JSON", "--relativenames"],
                       input="\n".join("0x%X" % (base + r) for r in rvas),
                       capture_output=True, text=True)
    out = {}
    for r, line in zip(rvas, p.stdout.splitlines()):
        frames = []
        for s in json.loads(line).get("Symbol", []):
            frames.append((s.get("FunctionName", "?"), s.get("FileName", ""), s.get("Line", 0)))
        out[r] = frames
    return out


def spu_line_report(samples, syms, starts, top):
    """--lines: the lifted SPU samples by lifted C line (one SPU instruction
    each) and by the kind of operation on it: the helper it is inside, else
    the first helper the line calls, else the shape of the line."""
    def obj(r):
        i = bisect.bisect_right(starts, r) - 1
        return syms[i][2] if i >= 0 else ""
    spu = [r for r in samples if r and not r & 0x80000000 and "spu_recomp" in obj(r)]
    if not spu:
        print("no samples in lifted SPU code")
        return
    frames = spu_line_frames(set(spu))
    src_cache = {}

    def text(path, line):
        full = path if os.path.isabs(path) else os.path.join(BUILD, path)
        if full not in src_cache:
            try:
                src_cache[full] = open(full, encoding="utf-8", errors="replace").read().split("\n")
            except OSError:
                src_cache[full] = []
        src = src_cache[full]
        return src[line - 1].strip() if 0 < line <= len(src) else ""

    by_line, by_kind = {}, {}
    for r in spu:
        fr = frames.get(r) or []
        outer = next((f for f in reversed(fr) if f[1].endswith("spu_recomp.c")), None)
        inner = fr[0] if fr else ("?", "", 0)
        if outer is None:
            key, kind = ("(no line)", "", 0), "(no line)"
        else:
            key = (outer[0], outer[1], outer[2])
            if inner is not outer and not inner[1].endswith("spu_recomp.c"):
                kind = inner[0]
            else:
                t = text(outer[1], outer[2])
                m = re.search(r"\b(spu_\w+|SPU_\w+)\s*\(", t)
                kind = (m.group(1) if m else
                        "register copy" if re.match(r"ctx->gpr\[\d+\] = ctx->gpr\[\d+\];", t) else
                        "branch" if t.startswith(("if", "goto", "}", "return")) else
                        "line 0" if outer[2] == 0 else "other: " + t[:40])
        by_line[key] = by_line.get(key, 0) + 1
        by_kind[kind] = by_kind.get(kind, 0) + 1
    tot = len(spu)
    print("lifted SPU samples: %d" % tot)
    print("by operation:")
    for k, c in sorted(by_kind.items(), key=lambda kv: -kv[1])[:top]:
        print("  %5.1f%%  %s" % (100.0 * c / tot, k))
    print("by lifted line:")
    for (fn, path, line), c in sorted(by_line.items(), key=lambda kv: -kv[1])[:top]:
        print("  %5.1f%%  %s:%d  %s" % (100.0 * c / tot, fn[:44], line, text(path, line)[:70]))


def symbolize_rvas(args, syms, starts):
    """--rvas: a PS3_SAMPLE_RVAS file (one RVA per sample, "# <ms>" after each
    report's block), counted by function and by object file over the last
    --last seconds. Lifted PPU functions show as func_XXXXXXXX. A file from
    PS3_SAMPLE_THREAD tags each RVA with the thread name it matched; each name
    is then counted on its own (--thread picks one), and a SPURS worker's
    sample with the workload it was running (--wid picks one)."""
    blocks, cur, names = [], [], []
    for line in open(args.rvas):
        if line.startswith("#!"):
            names = line.split(None, 2)[2].rstrip("\n").split("|")
        elif line.startswith("#"):
            blocks.append((int(line.split()[1]), cur))
            cur = []
        elif line.strip():
            f = line.split()
            r = int(f[0], 16)
            if r == 0 and args.callers and len(f) > 3 and int(f[3], 16):
                r = int(f[3], 16) | 0x80000000          # a wait, named by its caller
            cur.append((r, int(f[1]) if len(f) > 1 else 0, int(f[2]) if len(f) > 2 else -1))
    if not blocks:
        die("no sample blocks in %s" % args.rvas)
    end = blocks[-1][0]
    picked = [b for t, b in blocks if t >= end - args.last * 1000]
    tagged = [s for b in picked for s in b if args.wid is None or s[2] == args.wid]
    say("%d samples in the last %.0f s (%d reports)" % (len(tagged), args.last, len(picked)))
    names = names or ["all threads"]

    def name(r):
        if r == 0:
            return "(outside the executable)", ""
        if r & 0x80000000:
            n, o = name(r & 0x7FFFFFFF)
            return "(wait) " + n, o
        i = bisect.bisect_right(starts, r) - 1
        if i < 0:
            return "?", ""
        m = re.match(r"\?(func_[0-9A-F]{8})@@", syms[i][1])
        return (m.group(1) if m else syms[i][1]), syms[i][2]

    for cls, cname in enumerate(names):
        if args.thread and not cname.startswith(args.thread):
            continue
        samples = [r for r, c, w in tagged if c == cls]
        if not samples:
            continue
        print("=== %s: %d samples ===" % (cname, len(samples)))
        wids = {}
        for r, c, w in tagged:
            if c == cls and w >= 0:
                wids[w] = wids.get(w, 0) + 1
        if wids:
            print("by workload: " + ", ".join("w%d %.1f%%" % (w, 100.0 * n / len(samples))
                                              for w, n in sorted(wids.items(), key=lambda kv: -kv[1])))
        by_fn, by_obj = {}, {}
        for r in samples:
            n, o = name(r)
            by_fn[(n, o)] = by_fn.get((n, o), 0) + 1
            kind = ("(wait) " + (o or n) if n.startswith("(wait)") else
                    "lifted PPU" if n.startswith("func_") else
                    "lifted SPU" if "spu_recomp" in o else o or n)
            by_obj[kind] = by_obj.get(kind, 0) + 1
        tot = len(samples)
        print("by object file:")
        for k, c in sorted(by_obj.items(), key=lambda kv: -kv[1])[:15]:
            print("  %5.1f%%  %s" % (100.0 * c / tot, k))
        print("by function:")
        for (n, o), c in sorted(by_fn.items(), key=lambda kv: -kv[1])[:args.top]:
            print("  %5.1f%%  %-48s %s" % (100.0 * c / tot, n[:48], o))
        if args.lines:
            spu_line_report(samples, syms, starts, args.top)
    return 0


def cmd_symbolize(args):
    """Name the runtime-code RVAs in a PS3_SAMPLE report ("[samp] rva 0x...")
    from the linker map the build writes next to the executable. Only valid
    for the build that produced the log."""
    mp = args.map or os.path.join(BUILD, "GowARecomp.map")
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
    if args.rvas:
        return symbolize_rvas(args, syms, starts)
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
    p.add_argument("--jobmods-only", action="store_true",
                   help="lift only the SPU code this port lifts itself: the Wws_Job policy module, "
                        "the job modules and the position-independent pieces "
                        "(spu/wwspm/, spu/jobmods/, spu/picfrags/; minutes, not the PPU)")
    p = sub.add_parser("jobmods-entries",
                       help="add the job-module interpreter entries of runs (SPU_INTERP_ENTRIES) as extra functions")
    p.add_argument("logs", nargs="+", help="run logs")
    sub.add_parser("lle", help="relocate and lift firmware/libsre.prx for its SPURS queue functions")
    p = sub.add_parser("build", help="configure + build with clang-cl")
    p.add_argument("--jobs", default="3", help="parallel compile jobs (default 3 for 16 GB RAM)")
    p.add_argument("--config", default="Release", choices=["Release", "RelWithDebInfo", "Debug"])
    p.add_argument("--fresh", action="store_true", help="delete build/ and configure again")
    p.add_argument("--no-spu", action="store_true",
                   help="don't lift and register the game ELF's embedded SPU programs")
    p.add_argument("--spu-lines", action="store_true",
                   help="line tables for the lifted SPU code and a PDB, for "
                        "`symbolize --rvas --lines` (PORT_SPU_LINES; same code)")
    p.add_argument("--jobcap", action="store_true",
                   help="SPU job capture and replay hooks (PORT_SPU_JOBCAP), for SPU_JOB_CAPTURE and "
                        "`replay`; a measurement build, not for play")
    p.add_argument("--spu-approx", action="store_true",
                   help="SPU float results of zero may be -0 (PORT_SPU_FLOAT_APPROX); default bit-exact")
    p.add_argument("-D", dest="define", action="append", default=[], metavar="VAR=VALUE",
                   help="another CMake cache setting, kept for later builds (e.g. -D PORT_PPU_FAST_FP=ON)")
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
    p.add_argument("--rvas", help="a PS3_SAMPLE_RVAS file instead of a log: count every sample")
    p.add_argument("--last", type=float, default=1e9, help="with --rvas: only the last N seconds")
    p.add_argument("--top", type=int, default=30, help="with --rvas: functions to list")
    p.add_argument("--thread", help="with --rvas from PS3_SAMPLE_THREAD: only this thread name")
    p.add_argument("--wid", type=int, help="with --rvas from PS3_SAMPLE_THREAD: only samples of "
                                         "SPURS workers running this workload")
    p.add_argument("--map", help="the linker map of the build that recorded the samples (default "
                                 "build/GowARecomp.map; a later build moves the runtime's functions)")
    p.add_argument("--callers", action="store_true",
                   help="with --rvas: name a sample outside the executable by the caller in it "
                        "found on the stack (PS3_SAMPLE_THREAD files)")
    p.add_argument("--lines", action="store_true",
                   help="with --rvas: lifted SPU samples by lifted C line and operation "
                        "(needs a build with --spu-lines)")
    p = sub.add_parser("bench", help="drive the port to the first level and measure flips/s "
                                     "(A/B variants, a baseline, screenshots, GPU use)")
    bench.add_args(p)
    p = sub.add_parser("replay", help="run captured SPU jobs (SPU_JOB_CAPTURE) alone: time and output check")
    p.add_argument("dir", help="the folder the capture wrote")
    p.add_argument("--reps", type=int, default=10, help="runs per job (default 10)")
    p.add_argument("--only", help="only files whose name contains this, e.g. _w15_")
    p.add_argument("--exe", help="the executable (default build/GowARecomp.exe; built with --jobcap, "
                                  "the same runtime as the capture)")
    p = sub.add_parser("frames", help="frame report of a PS3_FTRACE trace (workloads, waits, the RSX, a grid)")
    frames.add_args(p)
    args = ap.parse_args()
    return {"doctor": cmd_doctor, "setup": cmd_setup, "analyze": cmd_analyze,
            "lift": cmd_lift, "lle": cmd_lle, "build": cmd_build,
            "run": cmd_run, "jobmods-entries": cmd_jobmods_entries, "symbolize": cmd_symbolize,
            "bench": bench.main, "frames": frames.main, "replay": cmd_replay}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
