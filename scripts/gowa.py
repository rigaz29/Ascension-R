#!/usr/bin/env python3
"""gowa.py - one entry point for the God of War: Ascension (NPUA80918) port pipeline.

    python scripts/gowa.py doctor                  check tools, submodule and game files
    python scripts/gowa.py setup --game-dir DIR    link your installed game into vfs/ and gamedata/
    python scripts/gowa.py analyze                 loader, SPU images, HLE coverage -> analysis/
    python scripts/gowa.py lift                    PPU -> C++ -> recompiled/
    python scripts/gowa.py missed                  what the sessions ran without a lift of it
    python scripts/gowa.py build                   configure + build with clang-cl -> build/
    python scripts/gowa.py ffmpeg                  build the small FFmpeg a release carries
    python scripts/gowa.py package prebuilt|kit    build and zip a release -> dist/<version>/<release>/
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
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import threading
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402  (scripts/bench.py)
import frames  # noqa: E402  (scripts/frames.py)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PS3RECOMP = os.path.join(ROOT, "ps3recomp")
TOOLS = os.path.join(PS3RECOMP, "tools")
LOCAL = os.path.join(ROOT, "local.json")
# GOWA_WORK: where what is made from the game goes (analysis/, recompiled/, spu/), and GOWA_ELF: the decrypted
# GOWA.elf. The Kit's setup sets both to the player's data folder; by default they are this tree's.
WORK = os.path.abspath(os.environ.get("GOWA_WORK") or ROOT)
ANALYSIS = os.path.join(WORK, "analysis")
RECOMPILED = os.path.join(WORK, "recompiled")
# GOWA_BUILD_DIR: another build tree (relative to the repo, or absolute), e.g. a --jobcap one
# beside the normal build, so the two need not rebuild everything each time one is switched.
BUILD = os.path.join(ROOT, os.environ.get("GOWA_BUILD_DIR") or "build")
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
    return os.environ.get("GOWA_ELF") or os.path.join(VFS_GAME, "USRDIR", GAME_ELF)


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


DXC_DLLS = ("dxcompiler.dll", "dxil.dll")
VC_DLLS = ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
FSR_DLLS = ("amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll",
            "amd_fidelityfx_upscaler_dx12.dll")

# A release carries FFmpeg with only the two decoders the runtime asks for (libs/codec/vdec_ffmpeg.c) and no other library.
FFMPEG_VERSION = "8.1.3"
FFMPEG_TARBALL = "ffmpeg-%s.tar.xz" % FFMPEG_VERSION
FFMPEG_URL = "https://ffmpeg.org/releases/" + FFMPEG_TARBALL
FFMPEG_SHA256 = "7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3"
FFMPEG_DLLS = (("libavcodec", "avcodec-62.dll"), ("libavutil", "avutil-60.dll"))
FFMPEG_CONFIGURE = ("--arch=x86_64", "--target-os=mingw32", "--prefix=/ffmpeg", "--cc=gcc",
                    "--disable-everything", "--disable-autodetect", "--disable-programs", "--disable-doc", "--disable-debug",
                    "--disable-network", "--disable-static", "--enable-shared", "--disable-avdevice", "--disable-avformat",
                    "--disable-avfilter", "--disable-swresample", "--disable-swscale",
                    "--enable-decoder=h264", "--enable-decoder=mpeg2video", "--extra-ldflags=-static")
FFMPEG_HOME = os.path.join(os.path.dirname(ROOT), "ffmpeg-release")

DIST = os.path.join(ROOT, "dist")
# The two ways to make a release, both from this tree (`package prebuilt`, `package kit`):
#   Prebuilt  the exe as `build` makes it, with the game's code translated into it. The player unpacks it and plays.
#   Kit       the program without that code, and the tools that build it once on the player's PC from their own copy.
RELEASES = {
    "prebuilt": {"name": "Prebuilt", "license":
                 "The code written for this port is under the MIT License: Ascension-R.txt in this folder. Ascension-R.exe also holds\n"
                 "code translated from the game: that license does not cover it, and it remains its owners'. The other parts of this\n"
                 "download keep their own licenses."},
    "kit": {"name": "Kit", "license":
            "The code written for this port is under the MIT License: Ascension-R.txt in this folder. No code in this download was\n"
            "translated from the game by a tool. The setup builds that part on your PC from your copy of the game: that license\n"
            "does not cover what it builds, which remains its owners' and stays on your PC. The other parts of this download keep\n"
            "their own licenses."},
}
# What a release is built with: `build` keeps a bench's -D in its cache.
RELEASE_CMAKE = {"CMAKE_BUILD_TYPE": "Release", "PS3RECOMP_NO_DECRYPT": "ON", "PS3RECOMP_FFMPEG_RUNTIME": "ON",
                 "PORT_SPU_JOBCAP": "OFF", "PORT_SPU_LINES": "OFF", "PORT_SPU_LS_DIAG": "OFF", "PORT_SPU_FLOAT_APPROX": "OFF",
                 "PORT_PPU_PGO": "", "PORT_SPU_PGO": ""}
# In this PC's System32 perhaps, but not on every PC.
NOT_SYSTEM = re.compile(r"(msvcp|vcruntime|vcomp|concrt|mfc|libgcc|libstdc|libwinpthread|d3dx|dxcompiler|dxil|avcodec|avutil)", re.I)


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


def find_dxc():
    """The folder with dxcompiler.dll and dxil.dll, for guest shaders: local.json's
    "dxc_dir", or the newest Windows SDK that has them. Returns it, or None."""
    cands = [load_local().get("dxc_dir")]
    kits = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "bin")
    if os.path.isdir(kits):
        cands += [os.path.join(kits, d, "x64") for d in sorted(os.listdir(kits), reverse=True)]
    for c in cands:
        if c and all(os.path.isfile(os.path.join(c, f)) for f in DXC_DLLS):
            return c
    return None


def find_fsr():
    """The folder with the FSR API's DLLs: local.json's "fsr_dir", or signedbin in an SDK
    unpacked under ../fsr-sdk. Returns it, or None."""
    cands = [load_local().get("fsr_dir")]
    base = os.path.join(os.path.dirname(ROOT), "fsr-sdk")
    if os.path.isdir(base):
        cands += [os.path.join(base, d, "Kits", "FidelityFX", "signedbin") for d in sorted(os.listdir(base), reverse=True)]
    for c in cands:
        if c and all(os.path.isfile(os.path.join(c, f)) for f in FSR_DLLS):
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
    if os.name == "nt":
        dxc = find_dxc()
        say("  [%s] %-22s %s" % ("ok" if dxc else "--", "DXC", dxc or
                                 "no Windows SDK with dxcompiler.dll: shaders stay on D3DCompile, ~2.4x slower"))
        fsr = find_fsr()
        say("  [%s] %-22s %s" % ("ok" if fsr else "--", "FSR SDK", fsr or "none in ../fsr-sdk: no FSR 3"))

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
    if WORK != ROOT:   # the Kit's setup: the coverage report is for developers
        return 0
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
WWS_PM_DIR = os.path.join(WORK, "spu", "wwspm")
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
            *(["--merge-chunks"] if os.environ.get("GOWA_PM_MERGE", "1") != "0" else []),
            "--extra-funcs", ",".join("0x%X" % a for a in WWS_PM_EXTRA_FUNCS),
            "--float-spec", str(SPU_FLOAT_SPEC)]):
        die("spu_lifter failed on the Wws_Job policy module")
    sys.path.insert(0, TOOLS)
    from build_spu_workloads import debase_includes
    for f in ("spu_recomp.h", "spu_recomp.c"):
        debase_includes(os.path.join(WWS_PM_DIR, f))
    native_hooks(os.path.dirname(WWS_PM_DIR), only="wwspm")
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
JOBMODS_DIR = os.path.join(WORK, "spu", "jobmods")
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
# Job modules are lifted with --merge-chunks (GOWA_JOBMOD_MERGE=0: without): a
# run of chunks is one C function with gotos, not chunks that tail-call each
# other. In replay that was 1-8% faster for every workload but General6, whose
# modules are left out here: their time is in --float-spec bodies, which only a
# function without internal branches gets, so merged they were 12% slower
# (2026-11-01).
JOBMOD_MERGE_SKIP = {"jobmod_a67e549fa0ab_e41E8", "jobmod_1bdd67b54c3b_e7F60"}


def loop_locals_args():
    """The job modules and pieces lifted with --loop-locals (GOWA_LOOP_LOCALS=0: without), but
    not the loops a native routine takes (spu_native_hooks.json): their lifted copy runs one
    iteration. GOWA_LOOP_LOCALS_MAX=N: only loops that name at most N registers."""
    if os.environ.get("GOWA_LOOP_LOCALS", "1") != "1":
        return []
    args = ["--loop-locals"]
    if os.path.exists(NATIVE_HOOKS_FILE):
        heads = sorted({f[-8:] for funcs in json.load(open(NATIVE_HOOKS_FILE)).values() for f in funcs})
        if heads:
            args += ["--loop-locals-skip", ",".join(heads)]
    if os.environ.get("GOWA_LOOP_LOCALS_MAX"):
        args += ["--loop-locals-max", os.environ["GOWA_LOOP_LOCALS_MAX"]]
    return args


def lift_jobmods(elf):
    shutil.rmtree(JOBMODS_DIR, ignore_errors=True)
    extra = ["--skip-ea", ",".join("%X" % ea for ea in sorted(JOBMOD_SKIP))] if JOBMOD_SKIP else []
    for name, addrs in sorted(jobmod_extra_funcs().items()):
        extra += ["--extra-funcs", name + "=" + ",".join("0x%X" % a for a in addrs)]
    spu_patches = title_patches_spu_file(title_patches(elf))
    if spu_patches:
        extra += ["--insn-patches", spu_patches]
        say("title patches: their SPU instruction edits lifted both ways (off unless switched on)")
    if os.environ.get("GOWA_JOBMOD_MERGE") != "0":
        extra += ["--merge-chunks", "--merge-skip", ",".join(sorted(JOBMOD_MERGE_SKIP))]
        say("job modules: --merge-chunks, but not %s" % ", ".join(sorted(JOBMOD_MERGE_SKIP)))
    # --ijmp-local: a computed jump to an entry of its own C function -- the branch-free
    # loops' `selb $rX, $head, $exit, $cond; bi $rX` -- can be a goto, not a pass through
    # the dispatcher (~18% of the audio worker's time). main.cpp turns it on;
    # SPU_IJMP_LOCAL=0 turns it off (plan 2.17, 2.18). --loop-locals: loop_locals_args.
    if run([sys.executable, os.path.join(TOOLS, "lift_jobmods.py"), "--engine-elf", elf,
            "--out", JOBMODS_DIR, "--register", os.path.join(JOBMODS_DIR, "jobmods_register.c"),
            "--float-spec", str(SPU_FLOAT_SPEC), "--ijmp-local"] + extra + loop_locals_args()):
        die("lift_jobmods failed")
    n = len(json.load(open(os.path.join(JOBMODS_DIR, "jobmods_manifest.json"))))
    say("%d WWS job modules lifted to %s" % (n, os.path.relpath(JOBMODS_DIR, ROOT)))
    native_hooks(JOBMODS_DIR)


# Native code for chosen lifted SPU functions (native/, built with the lifted SPU
# code's options): scripts/spu_native_hooks.json names, per lifted module, the
# function and the native routine called at its entry. A routine does what work
# it can and leaves the registers as the lifted code expects them, which then
# goes on (native/spu_edge_skin.c). A module is a job module (spu/jobmods), or,
# named pic_<EA>, a position-independent piece (spu/picfrags), or wwspm, the job
# manager (spu/wwspm).
# A routine named "?name" may take the whole function instead: it returns int, and
# nonzero makes the function return to its link (native/spu_edge_cull.c).
NATIVE_HOOKS_FILE = os.path.join(ROOT, "scripts", "spu_native_hooks.json")


def native_hooks(lift_dir, pic=False, only=None):
    if not os.path.exists(NATIVE_HOOKS_FILE):
        return
    for module, funcs in sorted(json.load(open(NATIVE_HOOKS_FILE)).items()):
        if module.startswith("pic_") != pic or (module == "wwspm") != (only == "wwspm"):
            continue
        path = os.path.join(lift_dir, module, "spu_recomp.c")
        if not os.path.exists(path):
            die("native hook: no lifted module %s" % module)
        src = open(path, encoding="utf-8").read()
        for func, native in sorted(funcs.items()):
            takes = native.startswith("?")
            native = native.lstrip("?")
            call = ("    if (%s(ctx)) SPU_RET(ctx);\n" if takes else "    %s(ctx);\n") % native
            head = "void %s(spu_context* SPU_RESTRICT ctx) {\n" % func
            if src.count(head) != 1:
                head = "void %s(spu_context* ctx) {\n" % func   # lifted before SPU_RESTRICT
            if src.count(head) != 1:
                die("native hook: %s not found once in %s" % (func, module))
            # --ijmp-local: a computed jump back to the function's start goes to its
            # ijmp_ label, so the call goes right after that label, which the way in
            # through the head passes too.
            at = src.find(head)
            own = src.find("ijmp_%s: ;\n" % func[-8:], at, src.find("\n}\n", at))
            if own >= 0:
                own += len("ijmp_%s: ;\n" % func[-8:])
                src = src[:own] + call + src[own:]
            else:
                src = src.replace(head, head + call)
            say("native hook: %s -> %s" % (func, native))
            # --merge-chunks: the loop is also inside the merged run of chunks
            # that leads to it, entered there by a goto rather than through the
            # function above. There the call goes right after the loop's label,
            # so it runs on every way in; a routine takes the loop wherever it
            # finds it and returns at once with one iteration or none left.
            label = "\nloc_%s:\n" % func[-8:]
            pos, extra = 0, 0
            while True:
                pos = src.find(label, pos)
                if pos < 0:
                    break
                fn_start = src.rfind("\nvoid ", 0, pos)
                ins = 0
                if src.find(head, fn_start) != fn_start + 1:     # not the function above
                    at = pos + len(label)
                    # --loop-locals: the loop's registers are loaded into locals on the
                    # line before its label; the routine reads and writes ctx->gpr, so
                    # it runs ahead of that load (once on the way in, as it takes the
                    # loop from there).
                    prev = src.rfind("\n", 0, pos) + 1
                    if src[prev:pos].lstrip().startswith("spu_l") and "= ctx->gpr[" in src[prev:pos]:
                        at = prev
                    src = src[:at] + call + src[at:]
                    ins = len(call)
                    extra += 1
                pos += len(label) + ins
            if extra:
                say("native hook: %s also in %d merged function(s)" % (native, extra))
            # Between a loop's local loads and its label the routine would work on
            # registers the loop no longer reads.
            if re.search(r"\n[ \t]*spu_l\d+ = ctx->gpr\[\d+\];[^\n]*\nloc_%s:\n[ \t]*(?:if \()?%s\(ctx\)"
                         % (func[-8:], native), src):
                die("native hook: %s lands after the loop-locals loads of loc_%s" % (native, func[-8:]))
        # the routines' declarations ahead of every function that calls them
        first = min(i for i in (src.find("\nvoid "), src.find("\nstatic void ")) if i >= 0)
        decls = "".join(("int %s(spu_context* ctx);\n" if n.startswith("?") else "void %s(spu_context* ctx);\n")
                        % n.lstrip("?") for n in sorted(set(funcs.values())))
        src = src[:first + 1] + decls + src[first + 1:]
        open(path, "w", encoding="utf-8", newline="\n").write(src)


# The position-independent code pieces the EDGE geometry jobs stream out of the
# executable to wherever they have room (LS 0x11000-0x12400): lifted once each
# and dispatched wherever they land, instead of interpreted. The list -- EA,
# size, entry offset -- is in scripts/spu_pic_pieces.json, found by running the
# title (docs/notes/2026-10-27-l2-analysis.md).
PIC_DIR = os.path.join(WORK, "spu", "picfrags")
PIC_LIST = os.path.join(ROOT, "scripts", "spu_pic_pieces.json")


def lift_pic(elf):
    shutil.rmtree(PIC_DIR, ignore_errors=True)
    if run([sys.executable, os.path.join(TOOLS, "lift_spu_pic.py"), "--elf", elf,
            "--list", PIC_LIST, "--out", PIC_DIR, "--float-spec", str(SPU_FLOAT_SPEC)] + loop_locals_args()):
        die("lift_spu_pic failed")
    n = len(json.load(open(os.path.join(PIC_DIR, "pic_manifest.json"))))
    say("%d position-independent SPU pieces lifted to %s" % (n, os.path.relpath(PIC_DIR, ROOT)))
    native_hooks(PIC_DIR, pic=True)


# What the runs missed: SPU code that ran in the interpreter and PPU calls that found no lifted
# function (ps3_missed_code_to, runtime/diag/missed_code.c). Every launcher session adds to this
# file; `lift` takes it into the lists above before it lifts, and `missed` shows it.
MISSED_PROFILE = os.path.join(BUILD, "logs", "missed_code.txt")
PPU_EXTRA_FILE = os.path.join(ROOT, "scripts", "ppu_extra_functions.json")


def missed_code(elf, profile, apply):
    cmd = [sys.executable, os.path.join(TOOLS, "missed_code.py"), "--profile", profile, "--elf", elf,
           "--pic-list", PIC_LIST, "--jobmods-manifest", os.path.join(JOBMODS_DIR, "jobmods_manifest.json"),
           "--jobmods-extra", JOBMOD_EXTRA_FILE, "--functions", os.path.join(ANALYSIS, "GOWA.functions.json"),
           "--ppu-extra", PPU_EXTRA_FILE]
    if apply:
        cmd.append("--apply")
    if run(cmd):
        die("missed_code failed")


def cmd_missed(args):
    """What the runs missed, and with --apply into the lists the next lift reads."""
    if not os.path.exists(args.profile):
        say("nothing missed so far: no %s" % os.path.relpath(args.profile, ROOT))
        return 0
    missed_code(need_elf(), args.profile, args.apply)
    return 0


def ppu_functions_with_extras(funcs):
    """The function list for the PPU lift: the analysis's, or a copy with the PPU functions the
    runs missed (scripts/ppu_extra_functions.json) in it."""
    if not os.path.exists(PPU_EXTRA_FILE):
        return funcs
    sys.path.insert(0, TOOLS)
    from missed_code import merge_ppu_functions
    addrs = [int(e["addr"], 16) for e in json.load(open(PPU_EXTRA_FILE))]
    merged = os.path.join(ANALYSIS, "GOWA.functions.extra.json")
    with open(merged, "w", encoding="utf-8", newline="\n") as f:
        json.dump(merge_ppu_functions(json.load(open(funcs)), addrs), f, indent=1)
    say("PPU functions: %d the runs missed, on top of the analysis's" % len(addrs))
    return merged


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


# Switchable patches of the title (scripts/title_patches.json). `lift` lifts each
# patch's instruction edits in both forms behind the patch's guard (ppu_lifter
# --insn-patches) and writes recompiled/gowa_title_patches.h, the guards and the
# data edits patches.cpp applies at boot to the patches switched on in ascension-r.ini
# [patches]. Every patch is off unless switched on.
TITLE_PATCHES_FILE = os.path.join(ROOT, "scripts", "title_patches.json")
TITLE_PATCHES_HEADER = "gowa_title_patches.h"


def elf_bytes(elf, ea, n):
    """n bytes of the ELF's file image at guest address ea, or None where no
    PT_LOAD segment has file bytes for all of them."""
    with open(elf, "rb") as f:
        hdr = f.read(64)
        phoff, = struct.unpack(">Q", hdr[0x20:0x28])
        phentsize, phnum = struct.unpack(">HH", hdr[0x36:0x3A])
        f.seek(phoff)
        table = f.read(phentsize * phnum)
        for i in range(phnum):
            ph = table[i * phentsize:(i + 1) * phentsize]
            p_type, = struct.unpack(">I", ph[0:4])
            p_offset, p_vaddr, _, p_filesz = struct.unpack(">QQQQ", ph[8:40])
            if p_type == 1 and p_vaddr <= ea and ea + n <= p_vaddr + p_filesz:
                f.seek(p_offset + ea - p_vaddr)
                return f.read(n)
    return None


def title_patches(elf):
    """scripts/title_patches.json, checked against the ELF: each orig must be what
    the ELF holds there, or the set is for another build of the title."""
    if not os.path.exists(TITLE_PATCHES_FILE):
        return []
    rel = os.path.relpath(TITLE_PATCHES_FILE, ROOT)
    patches = json.load(open(TITLE_PATCHES_FILE, encoding="utf-8")).get("patches", [])
    ids, guards = set(), set()

    def bad(p, why):
        die("%s: %s: %s" % (rel, p.get("id", "?"), why))
    for p in patches:
        for k in ("id", "name", "guard"):
            if not isinstance(p.get(k), str) or not p[k]:
                bad(p, "no %s" % k)
        if not re.fullmatch(r"[a-z][a-z0-9_]*", p["id"]) or p["id"] in ids:
            bad(p, "the id must be lower case, digits and _, and unique")
        if not re.fullmatch(r"[A-Za-z_]\w*", p["guard"]) or p["guard"] in guards:
            bad(p, "the guard must be a C identifier, and unique")
        if re.search(r'["\\]', p["name"]):
            bad(p, "the name may not hold quotes or backslashes")
        ids.add(p["id"])
        guards.add(p["guard"])
        if not p.get("ppu") and not p.get("data") and not p.get("spu"):
            bad(p, "no ppu, spu or data edit")
        for e in p.get("spu", []):
            mod, ls = e.get("module", ""), int(str(e["ls"]), 0)
            orig, new = int(str(e["orig"]), 0), int(str(e["new"]), 0)
            if not re.fullmatch(r"jobmod_[0-9a-f]{12}_e[0-9A-F]+", mod):
                bad(p, "spu edit: %r is not a job module's name" % mod)
            if ls & 3 or not 0 <= ls < 0x40000 or orig >> 32 or new >> 32:
                bad(p, "spu edit: bad local-store address or word at 0x%X" % ls)
            # Where a lift has placed the module, its bytes in the ELF must be the
            # original; spu_lifter.py checks them again at the lift either way.
            for m in jobmods_manifest():
                if m["name"] != mod:
                    continue
                for ea in m.get("eas", [m.get("ea")]):
                    held = elf_bytes(elf, ea + ls - m["base"], 4) if ea else None
                    if held is None or struct.unpack(">I", held)[0] != orig:
                        bad(p, "%s holds %s at LS 0x%X, the patch expects 0x%08X (another build?)"
                            % (mod, "0x%08X" % struct.unpack(">I", held)[0] if held else "nothing", ls, orig))
        for e in p.get("ppu", []):
            addr, orig, new = (int(str(e[k]), 0) for k in ("addr", "orig", "new"))
            if addr & 3:
                bad(p, "0x%08X is not an instruction address" % addr)
            held = elf_bytes(elf, addr, 4)
            if held is None or struct.unpack(">I", held)[0] != orig:
                bad(p, "the ELF has %s at 0x%08X, the patch expects 0x%08X (another build?)"
                    % ("0x%08X" % struct.unpack(">I", held)[0] if held else "nothing", addr, orig))
            if (new >> 26) in (16, 17, 18, 19):
                bad(p, "0x%08X: a branch, sc or CR op cannot be lifted as a replacement" % addr)
        for e in p.get("data", []):
            addr = int(str(e["addr"]), 0)
            orig, new = bytes.fromhex(e["orig"]), bytes.fromhex(e["new"])
            if not 0 < len(orig) <= 16 or len(new) != len(orig):
                bad(p, "0x%08X: orig and new must be 1-16 bytes, as many of each" % addr)
            if elf_bytes(elf, addr, len(orig)) != orig:
                bad(p, "the ELF does not hold %s at 0x%08X (another build?)" % (e["orig"], addr))
    return patches


def jobmods_manifest():
    """The last lift's job modules (name, base, ea/eas), or [] before one."""
    path = os.path.join(JOBMODS_DIR, "jobmods_manifest.json")
    return json.load(open(path)) if os.path.exists(path) else []


def title_patches_spu_file(patches):
    """lift_jobmods.py --insn-patches from the catalog's spu edits, or None."""
    edits = [{"module": e["module"], "addr": e["ls"], "orig": e["orig"], "new": e["new"],
              "guard": p["guard"], "optional": bool(e.get("optional")),
              "note": "%s: %s" % (p["id"], e.get("note", ""))}
             for p in patches for e in p.get("spu", [])]
    if not edits:
        return None
    path = os.path.join(ANALYSIS, "GOWA.spu_insn_patches.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"patches": edits}, f, indent=1)
    return path


def update_title_patches_header(patches):
    """recompiled/'s header, rewritten only when it changes (a lift of the job
    modules alone still adds their patches' guards)."""
    os.makedirs(RECOMPILED, exist_ok=True)
    old = os.path.join(RECOMPILED, TITLE_PATCHES_HEADER)
    before = open(old, encoding="utf-8").read() if os.path.exists(old) else None
    stage = RECOMPILED + ".hdr"
    os.makedirs(stage, exist_ok=True)
    write_title_patches_header(stage, patches)
    new = os.path.join(stage, TITLE_PATCHES_HEADER)
    if open(new, encoding="utf-8").read() != before:
        os.replace(new, old)
    shutil.rmtree(stage, ignore_errors=True)


def write_title_patches_header(out_dir, patches):
    """The guards and data edits for patches.cpp, from the checked catalog."""
    L = ["/* Generated by scripts/gowa.py lift from scripts/title_patches.json: do not edit. */"]
    L += ["GOWA_TITLE_GUARD(%s)" % p["guard"] for p in patches]
    L.append("static const gowa_title_data k_title_data[] = {")
    for i, p in enumerate(patches):
        for e in p.get("data", []):
            o, n = bytes.fromhex(e["orig"]), bytes.fromhex(e["new"])
            L.append("    { %d, 0x%08Xu, %d, { %s }, { %s } },   /* %s */" % (
                i, int(str(e["addr"]), 0), len(o), ", ".join("0x%02X" % b for b in o),
                ", ".join("0x%02X" % b for b in n), p["id"]))
    L += ["    { -1, 0, 0, { 0 }, { 0 } }", "};", "static const gowa_title_patch k_title_patches[] = {"]
    L += ['    { "%s", "%s", &%s, %d, %d, %d },' % (p["id"], p["name"], p["guard"], len(p.get("ppu", [])),
                                               len(p.get("spu", [])), 1 if p.get("default") else 0)
          for p in patches]
    L += ["    { NULL, NULL, NULL, 0, 0, 0 }", "};", ""]
    open(os.path.join(out_dir, TITLE_PATCHES_HEADER), "w", encoding="utf-8", newline="\n").write("\n".join(L))


def sync_dir(src, dst):
    """Move src's files over dst's where they differ, drop dst's files src lacks,
    then remove src. A file whose bytes are the same keeps its time, so a lift
    that changes three functions rebuilds three files, not all of them. Returns
    (files written, files in dst)."""
    import filecmp
    os.makedirs(dst, exist_ok=True)
    names = set(os.listdir(src))
    written = 0
    for n in names:
        a, b = os.path.join(src, n), os.path.join(dst, n)
        if os.path.isfile(b) and filecmp.cmp(a, b, shallow=False):
            os.remove(a)
        else:
            os.replace(a, b)
            written += 1
    for n in set(os.listdir(dst)) - names:
        if os.path.isfile(os.path.join(dst, n)):
            os.remove(os.path.join(dst, n))
    shutil.rmtree(src, ignore_errors=True)
    return written, len(names)


def cmd_patches(args):
    """List the switchable patches and check each against the ELF."""
    patches = title_patches(need_elf())
    if not patches:
        say("no %s" % os.path.relpath(TITLE_PATCHES_FILE, ROOT))
        return 0
    for p in patches:
        say("%-24s %-24s %d PPU, %d SPU instruction edit(s), %d data edit(s): matches the ELF"
            % (p["id"], p["name"], len(p.get("ppu", [])), len(p.get("spu", [])), len(p.get("data", []))))
    say("switch one on with [patches] <id> = on in ascension-r.ini, --set patches.<id>=on or "
        "GOWA_PATCH_<ID>=1; `lift` and `build` after a change to the file")
    return 0


def cmd_lift(args):
    elf = need_elf()
    if os.path.exists(MISSED_PROFILE) and not args.no_missed:
        missed_code(elf, MISSED_PROFILE, True)
    if args.jobmods_only:
        update_title_patches_header(title_patches(elf))
        lift_wws_pm(elf)
        lift_jobmods(elf)
        lift_pic(elf)
        say("re-run `build` so CMake picks up the new file list")
        return 0
    funcs = os.path.join(ANALYSIS, "GOWA.functions.json")
    imports = os.path.join(ANALYSIS, "GOWA.imports.json")
    if not (os.path.exists(funcs) and os.path.exists(imports)):
        die("run `analyze` first")
    funcs = ppu_functions_with_extras(funcs)
    # Lifted into a staging folder, then only the files that changed replace
    # recompiled/'s (sync_dir), so the build after it is incremental.
    stage = RECOMPILED + ".new"
    shutil.rmtree(stage, ignore_errors=True)
    cmd = [sys.executable, os.path.join(TOOLS, "ppu_lifter.py"), elf,
           "--functions", funcs, "--hle-stubs", imports,
           "--output", stage, "--jobs", args.jobs, "--chunks", args.chunks,
           "--lazy-flags", args.lazy_flags, "--dead-saves", args.dead_saves]
    end = code_end(elf)
    if end:
        say("code ends at 0x%X (last executable section)" % end)
        cmd += ["--code-end", "0x%X" % end]
    # The main thread's polling loops call gowa_poll_wait (poll_wait.cpp).
    hooks = os.path.join(ROOT, "scripts", "ppu_hooks.json")
    if os.path.exists(hooks):
        cmd += ["--insn-hooks", hooks]
    # The switchable title patches: their instruction edits in both forms.
    patches = title_patches(elf)
    edits = [{"addr": e["addr"], "orig": e["orig"], "new": e["new"], "guard": p["guard"],
              "note": "%s: %s" % (p["id"], e.get("note", ""))} for p in patches for e in p.get("ppu", [])]
    if edits:
        insn_patches = os.path.join(ANALYSIS, "GOWA.insn_patches.json")
        with open(insn_patches, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"patches": edits}, f, indent=1)
        cmd += ["--insn-patches", insn_patches]
        say("title patches: %d, %d instruction edit(s) lifted both ways (off unless switched on)"
            % (len(patches), len(edits)))
    rc = run(cmd)
    if rc:
        shutil.rmtree(stage, ignore_errors=True)
        die("ppu_lifter failed")
    write_title_patches_header(stage, patches)
    written, total = sync_dir(stage, RECOMPILED)
    files = [f for f in os.listdir(RECOMPILED) if f.endswith((".c", ".cpp"))]
    size = sum(os.path.getsize(os.path.join(RECOMPILED, f)) for f in files) / 2**20
    say("lifted: %d source files, %.0f MB in recompiled/ (%d of %d files changed)"
        % (len(files), size, written, total))
    lift_wws_pm(elf)
    lift_jobmods(elf)
    lift_pic(elf)
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
        copy_dxc()
        copy_fsr()
        copy_vc_runtime(env)
    return rc


def copy_dxc():
    """The runtime takes the two DLLs from the exe's folder only; without them guest
    shaders are compiled by D3DCompile, ~2.4 times slower."""
    src = find_dxc()
    if not src:
        say("note: no dxcompiler.dll in a Windows SDK; guest shaders stay on D3DCompile")
        return
    for f in DXC_DLLS:
        to = os.path.join(BUILD, f)
        if not os.path.isfile(to) or os.path.getmtime(to) != os.path.getmtime(os.path.join(src, f)):
            shutil.copy2(os.path.join(src, f), to)


def copy_fsr():
    """The FSR API's DLLs beside the exe: frame generation needs them, the rest of the port does not."""
    src = find_fsr()
    if not src:
        say("note: no FSR SDK in ../fsr-sdk; no FSR 3")
        return
    for f in FSR_DLLS:
        to = os.path.join(BUILD, f)
        if not os.path.isfile(to) or os.path.getmtime(to) != os.path.getmtime(os.path.join(src, f)):
            shutil.copy2(os.path.join(src, f), to)


def copy_vc_runtime(env):
    """The C++ runtime beside the exe, for a PC without the Visual C++ Redistributable. The FSR DLLs need it too."""
    redist = os.path.join(env.get("VCToolsRedistDir", ""), "x64")
    crt = [d for d in (os.listdir(redist) if os.path.isdir(redist) else []) if re.match(r"Microsoft\.VC\d+\.CRT$", d)]
    if not crt:
        say("note: no Visual C++ redistributable files; the exe needs the runtime installed")
        return
    src = os.path.join(redist, crt[0])
    for f in VC_DLLS:
        to = os.path.join(BUILD, f)
        if not os.path.isfile(to) or os.path.getmtime(to) != os.path.getmtime(os.path.join(src, f)):
            shutil.copy2(os.path.join(src, f), to)


def git_shell():
    """Git for Windows' bash, for FFmpeg's configure. The bash in System32 is WSL's."""
    d = os.path.dirname(shutil.which("git") or "")
    while d != os.path.dirname(d):
        sh = os.path.join(d, "bin", "bash.exe")
        if os.path.isfile(sh):
            return sh
        d = os.path.dirname(d)
    return None


def cmd_ffmpeg(args):
    """avcodec and avutil for a release, from FFmpeg's source tarball. Needs a MinGW-w64 gcc and nasm on PATH."""
    tarball = os.path.join(FFMPEG_HOME, FFMPEG_TARBALL)
    if not os.path.isfile(tarball):
        die("no %s: download it from %s into %s" % (FFMPEG_TARBALL, FFMPEG_URL, FFMPEG_HOME))
    if sha256(tarball) != FFMPEG_SHA256:
        die("%s is not the expected file" % tarball)
    sh = git_shell()
    if not sh:
        die("no bash from Git for Windows")
    missing = [t for t in ("gcc", "nasm", "mingw32-make") if not shutil.which(t)]
    if missing:
        die("not on PATH: %s" % ", ".join(missing))
    base = os.path.join(FFMPEG_HOME, "src")
    src = os.path.join(base, "ffmpeg-" + FFMPEG_VERSION)
    if not os.path.isfile(os.path.join(src, "configure")):
        with tarfile.open(tarball, "r:xz") as t:
            t.extractall(base, filter="data")
    tmp = os.path.join(FFMPEG_HOME, "tmp")
    os.makedirs(tmp, exist_ok=True)
    # configure takes TMPDIR as a POSIX path: the backslashes of a Windows one are lost.
    env = dict(os.environ, TMPDIR="/%s%s" % (tmp[0].lower(), tmp[2:].replace("\\", "/")))
    if run([sh, "-c", "./configure " + " ".join(FFMPEG_CONFIGURE)], cwd=src, env=env):
        die("configure failed; see %s" % os.path.join(src, "ffbuild", "config.log"))
    if run([sh, "-c", "mingw32-make -j" + args.jobs], cwd=src, env=env):
        die("make failed")
    out = os.path.join(FFMPEG_HOME, FFMPEG_VERSION)
    os.makedirs(out, exist_ok=True)
    for lib, dll in FFMPEG_DLLS:
        shutil.copy2(os.path.join(src, lib, dll), out)
    shutil.copy2(os.path.join(src, "COPYING.LGPLv2.1"), out)
    gcc = subprocess.run(["gcc", "--version"], stdout=subprocess.PIPE, text=True).stdout.splitlines()[0]
    with open(os.path.join(out, "FFmpeg-build.txt"), "w", encoding="utf-8") as f:
        f.write("FFmpeg %s, unchanged, from %s\nSHA-256 of that file: %s\n\nBuilt with %s:\n\n./configure %s\nmake\n"
                % (FFMPEG_VERSION, FFMPEG_URL, FFMPEG_SHA256, gcc, " ".join(FFMPEG_CONFIGURE)))
    say("FFmpeg %s -> %s" % (FFMPEG_VERSION, out))
    return 0


def find_dxc_redist():
    """The Windows SDK's folder of the files that may be passed on. Returns it, or None."""
    d = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "Windows Kits", "10", "Redist", "D3D", "x64")
    return d if all(os.path.isfile(os.path.join(d, f)) for f in DXC_DLLS) else None


def git_text(cwd, *a):
    return subprocess.run(["git", "-C", cwd] + list(a), stdout=subprocess.PIPE, text=True, check=True).stdout.strip()


def release_tree_problems():
    """A release is built from what is committed and pushed. scratch/ in the submodule is not built."""
    out = git_text(ROOT, "status", "--porcelain", "--ignore-submodules=dirty").splitlines()
    out += [l for l in git_text(PS3RECOMP, "status", "--porcelain").splitlines() if not l.split(None, 1)[-1].startswith("scratch/")]
    for repo in (ROOT, PS3RECOMP):
        if not git_text(repo, "branch", "-r", "--contains", "HEAD"):
            out.append("%s: HEAD is not pushed" % os.path.basename(repo))
    return out


def check_release_build():
    """build/'s cache can hold a bench's -D, and the exe must have seen the headers of the FFmpeg that goes with it."""
    cache = {}
    with open(os.path.join(BUILD, "CMakeCache.txt"), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"(\w+):\w+=(.*)$", line.rstrip("\r\n"))
            if m:
                cache[m.group(1)] = m.group(2)
    wrong = ["%s=%s, a release needs %s" % (k, cache.get(k), v) for k, v in RELEASE_CMAKE.items() if cache.get(k) != v]
    inc = os.path.join(find_ffmpeg() or "", "include")
    for (lib, dll), rel, name in zip(FFMPEG_DLLS, ("libavcodec/version_major.h", "libavutil/version.h"),
                                     ("LIBAVCODEC_VERSION_MAJOR", "LIBAVUTIL_VERSION_MAJOR")):
        with open(os.path.join(inc, rel), encoding="utf-8", errors="replace") as f:
            major = re.search(r"#define\s+%s\s+(\d+)" % name, f.read()).group(1)
        if "-%s.dll" % major not in dll:
            wrong.append("the exe saw the headers of %s %s, the release carries %s" % (lib, major, dll))
    if wrong:
        die("not a release build:\n  " + "\n  ".join(wrong))


def release_problems(stage, env):
    """A DLL a player may lack, this machine's paths or the author's name in a binary."""
    dumpbin = shutil.which("dumpbin", path=env.get("PATH"))
    if not dumpbin:
        die("no dumpbin in the toolchain")
    system32 = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    names = [ROOT, os.path.expanduser("~"), os.environ.get("USERNAME", ""),
             git_text(ROOT, "config", "user.email"), git_text(ROOT, "config", "user.name")]
    needles = {}
    for n in filter(None, names):
        for v in (n, n.replace("\\", "/")):
            needles[v.lower().encode()] = n
            needles[v.lower().encode("utf-16-le")] = n
    bad = []
    for name in sorted(os.listdir(stage)):
        path = os.path.join(stage, name)
        if not name.lower().endswith((".exe", ".dll")):
            continue
        text = subprocess.run([dumpbin, "/nologo", "/dependents", path], stdout=subprocess.PIPE, text=True, check=True).stdout
        for dep in sorted(set(re.findall(r"^\s+(\S+\.dll)\s*$", text, re.M | re.I))):
            if os.path.isfile(os.path.join(stage, dep)) or dep.lower().startswith(("api-ms-win-", "ext-ms-")):
                continue
            if NOT_SYSTEM.match(dep) or not os.path.isfile(os.path.join(system32, dep)):
                bad.append("%s needs %s, which is not in the package" % (name, dep))
        with open(path, "rb") as f:
            low = f.read().lower()
        for own in ("github.com/rigaz29".encode(), "github.com/rigaz29".encode("utf-16-le")):  # the project's public address
            low = low.replace(own, b"")
        bad += ["%s holds %r" % (name, n) for n in sorted(set(n for k, n in needles.items() if k in low))]
    return bad


def fsr1_license():
    with open(os.path.join(PS3RECOMP, "third_party", "fidelityfx_fsr1", "ffx_a.h"), encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    a = next(i for i, l in enumerate(lines) if "Copyright (c) 2021 Advanced Micro Devices" in l)
    b = next(i for i in range(a, len(lines)) if lines[i].rstrip().endswith("THE SOFTWARE."))
    return "\n".join(l.lstrip("/ ").rstrip() for l in lines[a:b + 1]) + "\n"


def release_text(src, release, version):
    """A file of packaging/ with what depends on the release and the version filled in."""
    with open(os.path.join(ROOT, "packaging", src), encoding="utf-8") as f:
        text = f.read()
    for key, value in (("@VERSION@", version), ("@FFMPEG_VERSION@", FFMPEG_VERSION), ("@RELEASE@", RELEASES[release]["name"]),
                       ("@LICENSE@", RELEASES[release]["license"])):
        text = text.replace(key, value)
    return text


def cmd_package(args):
    """One zip for players, and beside it what reading their crash reports will take. Nothing is uploaded."""
    import check_repo  # noqa: E402  (scripts/check_repo.py)
    if args.release == "kit":
        die("the Kit cannot be made yet: the game's DLL builds here (PORT_GAME_MODULE), but nothing builds it on a player's PC")
    with open(os.path.join(ROOT, "port", "version.h"), encoding="utf-8") as f:
        version = re.search(r'#define\s+AR_VERSION\s+"([^"]+)"', f.read()).group(1)
    version_dir = os.path.join(DIST, version)
    out = os.path.join(DIST, "test", args.release) if args.test else os.path.join(version_dir, args.release)
    if args.test:
        shutil.rmtree(out, ignore_errors=True)
    else:
        # Before the releases had names, a version's zip sat right in its folder.
        if os.path.exists(out) or (os.path.isdir(version_dir) and any(f.endswith("-win64.zip") for f in os.listdir(version_dir))):
            die("%s exists: a version is packaged once, so change version.h" % out)
        problems = release_tree_problems()
        if problems:
            die("not a release tree:\n  " + "\n  ".join(problems))
    ff = os.path.join(FFMPEG_HOME, FFMPEG_VERSION)
    dxc, fsr = find_dxc_redist(), find_fsr()
    if not all(os.path.isfile(os.path.join(ff, d)) for _, d in FFMPEG_DLLS):
        die("no release FFmpeg; run `gowa.py ffmpeg`")
    if not dxc or not fsr:
        die("this takes the Windows SDK's Redist\\D3D\\x64 and an FSR SDK in ../fsr-sdk")
    if cmd_build(argparse.Namespace(fresh=False, config="Release", no_spu=False, spu_lines=False, jobcap=False,
                                    spu_approx=False, define=[], jobs=args.jobs)):
        die("the build failed")
    check_release_build()

    name = "Ascension-R-%s-%s" % (version, args.release)
    stage = os.path.join(out, name)
    lic = os.path.join(stage, "licenses")
    os.makedirs(lic)
    files = [(BUILD, f) for f in ("Ascension-R.exe",) + VC_DLLS] + [(fsr, f) for f in FSR_DLLS]
    files += [(dxc, f) for f in DXC_DLLS] + [(ff, d) for _, d in FFMPEG_DLLS]
    for folder, f in files:
        shutil.copy2(os.path.join(folder, f), stage)
    sdk = os.path.dirname(os.path.dirname(os.path.dirname(fsr)))
    third = os.path.join(PS3RECOMP, "third_party")
    for src, to in ((os.path.join(ROOT, "LICENSE"), "Ascension-R.txt"),
                    (os.path.join(PS3RECOMP, "LICENSE"), "ps3recomp.txt"),
                    (os.path.join(sdk, "docs", "license.md"), "AMD-FidelityFX-SDK.txt"),
                    (os.path.join(third, "smaa", "LICENSE.txt"), "SMAA.txt"),
                    (os.path.join(third, "zstd", "LICENSE"), "Zstandard.txt"),
                    (os.path.join(ROOT, "third_party", "zlib", "LICENSE"), "zlib.txt"),
                    (os.path.join(ff, "COPYING.LGPLv2.1"), "FFmpeg-LGPL-2.1.txt"),
                    (os.path.join(ff, "FFmpeg-build.txt"), "FFmpeg-build.txt")):
        shutil.copyfile(src, os.path.join(lic, to))
    with open(os.path.join(lic, "AMD-FSR1.txt"), "w", encoding="utf-8") as f:
        f.write(fsr1_license())
    for src, to in (("README.txt", stage), ("THIRD-PARTY.txt", lic)):
        with open(os.path.join(to, src), "w", encoding="utf-8") as f:
            f.write(release_text(src, args.release, version))

    env = toolchain_env()
    problems = release_problems(stage, env)
    say("looking for PS3 keys in the exe")
    with open(os.path.join(stage, "Ascension-R.exe"), "rb") as f:
        if check_repo.has_key(f.read()):
            problems.append("Ascension-R.exe holds a PS3 key")
    if problems:
        die("not packaged:\n  " + "\n  ".join(problems))

    zip_path = os.path.join(out, name + "-win64.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for folder, _, names in os.walk(stage):
            for f in sorted(names):
                p = os.path.join(folder, f)
                z.write(p, os.path.relpath(p, out).replace("\\", "/"))
    shutil.copy2(os.path.join(BUILD, "Ascension-R.map"), out)
    shutil.copy2(os.path.join(FFMPEG_HOME, FFMPEG_TARBALL), out)
    clang = subprocess.run([shutil.which("clang-cl", path=env.get("PATH")), "--version"], stdout=subprocess.PIPE, text=True).stdout
    info = ["Ascension-R %s %s" % (version, RELEASES[args.release]["name"]) + ("  TEST PACKAGE, not for release" if args.test else ""),
            "port      " + git_text(ROOT, "rev-parse", "HEAD"),
            "ps3recomp " + git_text(PS3RECOMP, "rev-parse", "HEAD"),
            "exe SHA-256 " + sha256(os.path.join(stage, "Ascension-R.exe")),
            "zip SHA-256 " + sha256(zip_path),
            "compiler  " + clang.splitlines()[0],
            "FSR SDK   " + os.path.basename(sdk),
            "DXC       " + dxc,
            "FFmpeg    " + FFMPEG_VERSION]
    with open(os.path.join(out, "build-info.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(info) + "\n")
    unpacked = sum(os.path.getsize(os.path.join(d, f)) for d, _, ns in os.walk(stage) for f in ns)
    shutil.rmtree(stage)
    say("%s: %.1f MB, %.1f MB unpacked" % (zip_path, os.path.getsize(zip_path) / 1048576.0, unpacked / 1048576.0))
    return 0


# ---------------------------------------------------------------------------
# The game's DLL the way the Kit builds it on a player's PC
# ---------------------------------------------------------------------------

KIT_DIR = os.path.join(ROOT, "port", "kit")
# The Visual C++ the code is compiled as. clang-cl takes the installed one's version when it finds one, 19.33 when it
# does not; one fixed version gives the same objects on every PC, and the same as CMake's here.
KIT_MSVC = "19.51.36247"
MODULE_DLL = "ascension_game.dll"
MODULE_DEFINES_SPU = ("SPU_USE_DISPATCH_MT", "SPU_INLINE_MFC_PARAMS", "PS3_GAME_MODULE", "SPU_LS_NO_DIAG", "SPU_FLOAT_RZ_ONLY")


def kit_llvm():
    """The folder with clang-cl.exe and lld-link.exe: $GOWA_KIT_LLVM (the Kit's own), local.json's "llvm_bin",
    $GOWA_LLVM_BIN or ../llvm/bin."""
    for c in (os.environ.get("GOWA_KIT_LLVM"), load_local().get("llvm_bin"), os.environ.get("GOWA_LLVM_BIN"),
              os.path.join(os.path.dirname(ROOT), "llvm", "bin")):
        if c and all(os.path.isfile(os.path.join(c, t)) for t in ("clang-cl.exe", "lld-link.exe")):
            return c
    die("no clang-cl.exe and lld-link.exe: set llvm_bin in local.json")


def module_sources(out):
    """(kind, path) of every translation unit of the DLL, in the order CMake has them (PORT_GAME_MODULE)."""
    def listed(*pats):
        names = []
        for p in pats:
            names += glob.glob(p)
        return sorted(os.path.normpath(n) for n in names)

    gen = os.path.join(out, "generated_spu")
    with open(os.path.join(gen, "sources.cmake"), encoding="utf-8") as f:
        generated = [os.path.normpath(p) for p in re.findall(r"\[==\[(.+?)\]==\]", f.read())]
    spu_dir = os.path.join(WORK, "spu")
    native = []
    for n in listed(os.path.join(ROOT, "native", "spu_*.c")):
        native.append(os.path.join(out, "native", "spu_g6_routines.c") if os.path.basename(n) == "spu_g6_routines.c" else n)
    spu = (generated + listed(os.path.join(spu_dir, "wwspm", "*.c"))
           + listed(os.path.join(spu_dir, "jobmods", "*", "spu_recomp.c"), os.path.join(spu_dir, "jobmods", "jobmods_register.c"))
           + listed(os.path.join(spu_dir, "picfrags", "*", "spu_recomp.c"), os.path.join(spu_dir, "picfrags", "pic_register.c"))
           + native)
    return ([("c", os.path.join(ROOT, "port", "game_module.c"))]
            + [("ppu", p) for p in listed(os.path.join(RECOMPILED, "*.c"), os.path.join(RECOMPILED, "*.cpp"))]
            + [("spu", p) for p in spu])


def module_flags(kind, src):
    """clang-cl's options for one translation unit: CMake's for the DLL, and no header but the Kit's and clang's own."""
    cxx = src.endswith(".cpp")
    rt = os.path.join(PS3RECOMP, "runtime")
    inc = [RECOMPILED, os.path.join(PS3RECOMP, "include"), os.path.join(rt, "ppu"), os.path.join(rt, "spu"),
           os.path.join(rt, "memory"), os.path.join(rt, "syscalls"), os.path.join(rt, "platform"),
           os.path.join(PS3RECOMP, "libs", "video"), ROOT]
    f = ["/nologo", "/X", "-fms-compatibility-version=" + KIT_MSVC, "/TP" if cxx else "/TC", "/DWIN32", "/D_WINDOWS"]
    f += (["/EHsc"] if cxx else []) + ["/O2", "/Ob2", "/DNDEBUG", "-std:c++20" if cxx else "/std:c17", "-MD", "-Wall", "-Wextra",
                                       "-Wno-unused-parameter", "-fno-strict-aliasing", "-fwrapv"]
    f += (["/Zc:tlsGuards-"] if cxx else []) + ["/GS-"]
    if kind == "ppu":
        f += ["/w", "/bigobj", "/clang:-ffp-contract=off", "/clang:-mfma", "-DPPU_MM_LEAN", "/FI" + os.path.join(KIT_DIR, "prelude.h")]
    elif kind == "spu":
        f += ["-O2", "-msse4.1", "-mavx2", "-mfma", "-ffp-contract=off", "/GS-"] + ["-D" + d for d in MODULE_DEFINES_SPU]
        inc = [os.path.join(rt, "spu")] + inc
        if os.path.basename(src) == "spu_g6_routines.c":   # written into the build folder, away from the headers it shares
            inc.append(os.path.join(ROOT, "native"))
    return f + ["-I" + d for d in inc] + ["/imsvc" + os.path.join(KIT_DIR, "include")]


def module_headers_digest(out):
    """One digest of every header a translation unit can see, so an object is made again when one changes."""
    h = hashlib.sha256()
    pats = [os.path.join(KIT_DIR, "**", "*.h"), os.path.join(ROOT, "port", "*.h"), os.path.join(ROOT, "native", "*.h"),
            os.path.join(RECOMPILED, "*.h"), os.path.join(WORK, "spu", "**", "*.h"), os.path.join(out, "generated_spu", "**", "*.h"),
            os.path.join(PS3RECOMP, "include", "**", "*.h"), os.path.join(PS3RECOMP, "runtime", "**", "*.h"),
            os.path.join(PS3RECOMP, "libs", "video", "*.h")]
    for p in sorted(set(n for pat in pats for n in glob.glob(pat, recursive=True))):
        h.update(p.encode())
        with open(p, "rb") as f:
            h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()


def peak_mb(proc):
    """A finished process's peak working set and peak commit, in MB."""
    import ctypes
    from ctypes import wintypes

    class PMC(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD), ("peak_ws", ctypes.c_size_t), ("ws", ctypes.c_size_t),
                    ("a", ctypes.c_size_t), ("b", ctypes.c_size_t), ("c", ctypes.c_size_t), ("d", ctypes.c_size_t),
                    ("pagefile", ctypes.c_size_t), ("peak_pagefile", ctypes.c_size_t)]
    pmc = PMC()
    pmc.cb = ctypes.sizeof(pmc)
    k = ctypes.windll.kernel32
    k.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    if not k.K32GetProcessMemoryInfo(int(proc._handle), ctypes.byref(pmc), pmc.cb):
        return 0, 0
    return pmc.peak_ws // (1 << 20), pmc.peak_pagefile // (1 << 20)


def module_jobs(arg):
    """--jobs, or as many compiles as the cores and 1 GB of free memory each allow (the most one took was 325 MB)."""
    if arg != "auto":
        return max(1, int(arg))
    return max(1, min(os.cpu_count() or 1, (bench.free_ram_mb() - 2048) // 1024))


def cmd_module(args):
    """The game's DLL with clang-cl and lld-link alone: port/kit's headers in place of a C runtime's, nothing from Visual
    Studio or the Windows SDK, and the lift's output (`lift`) as it is. Objects made before with the same command,
    source and headers are kept, so a stopped build goes on where it was."""
    import concurrent.futures
    import time
    elf = need_elf()
    if not os.path.isfile(os.path.join(RECOMPILED, "ppu_recomp.h")):
        die("no lifted code in %s; run `lift` first" % RECOMPILED)
    llvm = kit_llvm()
    out = os.path.abspath(args.out or os.path.join(WORK, "build-module"))
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "module.log"), "a", encoding="utf-8")
    t0 = time.monotonic()
    py = [sys.executable, "-E", "-s"]   # not -I: a tool imports its neighbours from its own folder

    def step(what, cmd, env=None):
        log.write("> %s\n" % " ".join(cmd))
        log.flush()
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", env=env)
        log.write(r.stdout)
        if r.returncode:
            say(r.stdout[-3000:])
            die("%s failed (%s)" % (what, os.path.join(out, "module.log")))

    gen = os.path.join(out, "generated_spu")
    os.makedirs(gen, exist_ok=True)
    say("[module] the SPU programs inside the game's ELF")
    step("build_spu_workloads", py + [os.path.join(TOOLS, "build_spu_workloads.py"), "--lifted", gen,
                                      "--out", os.path.join(gen, "spu_workloads.c"), "--constructor", "--merge-chunks", "--input", elf])
    os.makedirs(os.path.join(out, "native"), exist_ok=True)
    env = dict(os.environ, GOWA_WORK=WORK, GOWA_ELF=elf)
    step("spu_to_native", py + [os.path.join(ROOT, "scripts", "spu_to_native.py"), os.path.join(ROOT, "scripts", "spu_g6_routines.json"),
                                os.path.join(out, "native", "spu_g6_routines.c")], env)

    srcs = module_sources(out)
    headers = module_headers_digest(out)
    clean = {"SystemRoot": os.environ.get("SystemRoot", r"C:\Windows"), "windir": os.environ.get("windir", r"C:\Windows"),
             "TEMP": os.environ.get("TEMP", out), "TMP": os.environ.get("TMP", out),
             "PATH": llvm + os.pathsep + os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")}
    clang = os.path.join(llvm, "clang-cl.exe")
    work = []
    for kind, src in srcs:
        rel = os.path.relpath(src, out if src.startswith(out) else (WORK if src.startswith(WORK) else ROOT))
        obj = os.path.join(out, "obj", rel.replace(os.sep, "_") + ".obj")
        cmd = [clang] + module_flags(kind, src) + ["/c", src, "/Fo" + obj]
        h = hashlib.sha256((" ".join(cmd) + "\n" + headers + "\n").encode())
        with open(src, "rb") as f:
            h.update(f.read())
        work.append((kind, src, obj, cmd, h.hexdigest()))
    os.makedirs(os.path.join(out, "obj"), exist_ok=True)
    todo = []
    for w in work:
        stamp = w[2] + ".stamp"
        if os.path.isfile(w[2]) and os.path.isfile(stamp) and open(stamp).read() == w[4]:
            continue
        todo.append(w)
    jobs = module_jobs(args.jobs)
    say("[module] %d translation units, %d to compile, %d at a time" % (len(work), len(todo), jobs))
    stats, failed, done = [], [], [0]
    lock = threading.Lock()

    def compile_one(w):
        kind, src, obj, cmd, digest = w
        if failed:
            return
        start = time.monotonic()
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=clean, cwd=out)
        text = p.communicate()[0].decode("utf-8", "replace")
        ws, commit = peak_mb(p)
        secs = time.monotonic() - start
        with lock:
            done[0] += 1
            log.write("> %s\n%s" % (" ".join(cmd), text))
            if p.returncode:
                failed.append((src, text))
                return
            open(obj + ".stamp", "w").write(digest)
            stats.append((kind, src, secs, ws, commit))
            if done[0] % 10 == 0 or done[0] == len(todo):
                el = time.monotonic() - t0
                say("[module] %d/%d compiled, %d:%02d" % (done[0], len(todo), el // 60, el % 60))

    # The big ones first, so the last minutes are not one long compile alone.
    todo.sort(key=lambda w: -os.path.getsize(w[1]))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        list(ex.map(compile_one, todo))
    if failed:
        src, text = failed[0]
        say(text[-4000:])
        die("%s did not compile (%d failed; %s)" % (src, len(failed), os.path.join(out, "module.log")))

    lib = os.path.join(out, "kernel32.lib")
    if not os.path.isfile(lib):
        step("llvm-lib", [os.path.join(llvm, "llvm-lib.exe"), "/nologo", "/machine:x64", "/def:" + os.path.join(KIT_DIR, "kernel32.def"),
                          "/out:" + lib])
    rsp = os.path.join(out, "objects.rsp")
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join('"%s"' % w[2] for w in work) + "\n")
    dll = os.path.join(out, MODULE_DLL)
    say("[module] linking")
    step("lld-link", [os.path.join(llvm, "lld-link.exe"), "/nologo", "/dll", "/NODEFAULTLIB", "/NOENTRY", "/MANIFEST:NO", "/machine:x64",
                      "/INCREMENTAL:NO", "/MAP:" + os.path.join(out, "ascension_game.map"), "/OUT:" + dll, "@" + rsp, lib], clean)
    el = time.monotonic() - t0
    if stats:
        cpu = sum(s[2] for s in stats)
        top = max(stats, key=lambda s: s[4])
        say("[module] compile: %.0f s of compiler time; the most memory one took: %d MB (%s)"
            % (cpu, top[4], os.path.basename(top[1])))
        with open(os.path.join(out, "module-stats.tsv"), "w", encoding="utf-8") as f:
            f.write("kind\tsource\tseconds\tpeak_ws_mb\tpeak_commit_mb\n")
            for s in stats:
                f.write("%s\t%s\t%.1f\t%d\t%d\n" % (s[0], os.path.relpath(s[1], ROOT) if s[1].startswith(ROOT) else s[1], s[2], s[3], s[4]))
    say("[module] %s: %.1f MB, %d:%02d" % (dll, os.path.getsize(dll) / 1048576.0, el // 60, el % 60))
    return 0


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
    """build/ascension-r.ini: this machine's paths, so Ascension-R.exe starts
    without this script. A player's settings in the file are kept."""
    path = os.path.join(BUILD, "ascension-r.ini")
    old_name = os.path.join(BUILD, "gowa.ini")
    if not os.path.exists(path) and os.path.exists(old_name):
        shutil.copyfile(old_name, path)  # the name before the rename; the old file stays as it is
    lines = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    else:
        lines = ["; Ascension-R settings. [paths] is written by `gowa.py build`;",
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
    exe = os.path.join(BUILD, "Ascension-R" + (".exe" if os.name == "nt" else ""))
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


def cmd_replay(args):
    """SPU_JOB_REPLAY: run the jobs an SPU_JOB_CAPTURE run recorded, alone, and report the
    time per job and whether each one's output came out the same (runtime/spu/spu_jobcap.c)."""
    exe = os.path.abspath(args.exe or os.path.join(BUILD, "Ascension-R" + (".exe" if os.name == "nt" else "")))
    if not os.path.isfile(exe):
        die("no executable: " + exe)
    env = dict(os.environ)
    env["SPU_JOB_REPLAY"] = os.path.abspath(args.dir)
    env["SPU_JOB_REPLAY_REPS"] = str(args.reps)
    if args.only:
        env["SPU_JOB_REPLAY_ONLY"] = args.only
    env["SPU_JOB_REPLAY_CMP"] = args.cmp
    if args.cmp == "state":
        env["SPU_JOB_REPLAY_STATE"] = args.state
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
    exe = os.path.join(BUILD, "Ascension-R.exe")
    if not os.path.exists(os.path.join(BUILD, "Ascension-R.pdb")):
        die("no build/Ascension-R.pdb: build with --spu-lines")
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
    mp = args.map or os.path.join(BUILD, "Ascension-R.map")
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
    sub.add_parser("patches", help="list the switchable title patches and check them against the ELF")
    p = sub.add_parser("lift", help="PPU -> C++ in recompiled/")
    p.add_argument("--jobs", default="6", help="lifter worker processes (default 6)")
    p.add_argument("--chunks", default="96",
                   help="number of source files to split into (default 96; more "
                        "files = less memory per compile job)")
    p.add_argument("--lazy-flags", choices=("off", "on", "check"), default="on",
                   help="the volatile CR fields and XER[CA] in locals of each lifted function; check keeps "
                        "the exact accesses too and reports where the two differ")
    p.add_argument("--dead-saves", choices=("off", "on", "poison"), default="off",
                   help="no guest stores for register saves nothing reads back; poison writes a poison "
                        "value there instead, to catch a reader the pass did not foresee")
    p.add_argument("--jobmods-only", action="store_true",
                   help="lift only the SPU code this port lifts itself: the Wws_Job policy module, "
                        "the job modules and the position-independent pieces "
                        "(spu/wwspm/, spu/jobmods/, spu/picfrags/; minutes, not the PPU)")
    p.add_argument("--no-missed", action="store_true",
                   help="leave out what the sessions missed (build/logs/missed_code.txt)")
    p = sub.add_parser("missed", help="what the sessions ran without a lift of it, by what the next lift would add")
    p.add_argument("--profile", default=MISSED_PROFILE, help="the profile (default build/logs/missed_code.txt)")
    p.add_argument("--apply", action="store_true", help="write it into the lists now; `lift` does this by itself")
    p = sub.add_parser("jobmods-entries",
                       help="add the job-module interpreter entries of runs (SPU_INTERP_ENTRIES) as extra functions")
    p.add_argument("logs", nargs="+", help="run logs")
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
                                 "build/Ascension-R.map; a later build moves the runtime's functions)")
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
    p.add_argument("--exe", help="the executable (default build/Ascension-R.exe; built with --jobcap, "
                                  "the same runtime as the capture)")
    p.add_argument("--cmp", choices=("state", "strict"), default="state",
                   help="state (default): the requests, PUT and mailbox bytes, the local store in --state "
                        "and r0 r1 r3 r80-r127, what a job leaves; strict: also the frames below the job's "
                        "main frame and every register (native code that replaces lifted calls leaves "
                        "those frames and the scratch registers as it likes)")
    p.add_argument("--state", default="0-3E000,3F780-40000",
                   help="local-store ranges, hex lo-hi, that --cmp state compares (default all but "
                        "0x3E000-0x3F780, the frames below the job's main frame)")
    p = sub.add_parser("frames", help="frame report of a PS3_FTRACE trace (workloads, waits, the RSX, a grid)")
    frames.add_args(p)
    p = sub.add_parser("module", help="the game's DLL with clang-cl and lld-link alone, as the Kit builds it")
    p.add_argument("--out", help="where (default build-module/ in the work folder)")
    p.add_argument("--jobs", default="auto", help="compiles at a time (default: the cores, 1 GB of free memory each)")
    p = sub.add_parser("ffmpeg", help="build the small FFmpeg a release carries, from its source tarball in ../ffmpeg-release")
    p.add_argument("--jobs", default="6", help="parallel compile jobs (default 6)")
    p = sub.add_parser("package", help="build and zip a release into dist/<version>/<release>/; nothing is uploaded")
    p.add_argument("release", choices=sorted(RELEASES),
                   help="prebuilt: the game's code is in the exe, unpack and play; kit: the player's PC builds that part")
    p.add_argument("--jobs", default="3", help="parallel compile jobs (default 3 for 16 GB RAM)")
    p.add_argument("--test", action="store_true", help="from the tree as it is, into dist/test/<release>/, which is replaced each time")
    args = ap.parse_args()
    return {"doctor": cmd_doctor, "setup": cmd_setup, "analyze": cmd_analyze,
            "lift": cmd_lift, "patches": cmd_patches, "build": cmd_build,
            "run": cmd_run, "jobmods-entries": cmd_jobmods_entries, "missed": cmd_missed, "symbolize": cmd_symbolize,
            "bench": bench.main, "frames": frames.main, "replay": cmd_replay,
            "ffmpeg": cmd_ffmpeg, "package": cmd_package, "module": cmd_module}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
