#!/usr/bin/env python3
"""The native EDGE loops (native/spu_edge_*.c) against the lifted loops they
stand in for: tests/native/edge_native_test.c on random registers and local
store, with each lifted loop taken from its lifted source (spu/picfrags,
spu/jobmods) as ref_<name>. Needs gcc with AVX2; from Windows it runs itself
in WSL (wsl -d Ubuntu).

    py tests/native/test_edge_native.py [cases]
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SPU = os.path.join(ROOT, "ps3recomp", "runtime", "spu")

# name -> (lifted source, lifted function[, the line that ends the loop]). With the third,
# the function is cut after that line (the loop's back edge) and returns there: a loop the
# lifted function goes on from into the rest of its stage.
LOOPS = {
    "30310": ("spu/picfrags/pic_0075EB40/spu_recomp.c", "pic_0075EB40_spu_func_00030310"),
    "304A0": ("spu/picfrags/pic_00758590/spu_recomp.c", "pic_00758590_spu_func_000304A0"),
    "7BB4": ("spu/jobmods/jobmod_e8f44fbc6dbf_e6D98/spu_recomp.c", "jobmod_e8f44fbc6dbf_e6D98_spu_func_00007BB4",
             "if (ctx->gpr[17]._u32[0] == 0) goto loc_00007BB4;"),
    "7ED8": ("spu/jobmods/jobmod_e8f44fbc6dbf_e6D98/spu_recomp.c", "jobmod_e8f44fbc6dbf_e6D98_spu_func_00007ED8",
             "if (ctx->gpr[58]._u32[0] == 0) goto loc_00007ED8;"),
}

# The lifted SPU code's definitions and options (CMakeLists.txt: spu_defs, spu_opts).
DEFS = ["SPU_USE_DISPATCH_MT", "SPU_INLINE_MFC_PARAMS", "SPU_LS_NO_DIAG", "SPU_FLOAT_RZ_ONLY"]
OPTS = ["-std=gnu17", "-O2", "-msse4.1", "-mavx2", "-mfma", "-ffp-contract=off"]


def ref_source(name, path, func, cut=None):
    """The lifted function as static ref_<name>, without a native hook call; with cut, only
    up to the line holding it, and a return after it."""
    src = open(os.path.join(ROOT, path), encoding="utf-8").read()
    m = re.search(r"^void %s\(spu_context\*[^)]*\) \{\n(.*?)^\}" % re.escape(func), src, re.M | re.S)
    if not m:
        sys.exit("%s not found in %s" % (func, path))
    body = [l for l in m.group(1).split("\n") if not re.match(r"\s*gowa_\w+\(ctx\);\s*$", l)]
    if cut:
        at = next((i for i, l in enumerate(body) if cut in l), None)
        if at is None:
            sys.exit("%s: no line with %r" % (func, cut))
        body = body[:at + 1] + ["    return;", ""]
    body = "\n".join(body)
    if "SPU_DIRECT" in body or "spu_indirect" in body:
        sys.exit("%s leaves the function other than by SPU_RET; the test cannot run it" % func)
    body = body.replace("SPU_RET(ctx);", "return;")
    return "static void ref_%s(spu_context* ctx) {\n%s}\n" % (name, body)


def main():
    cases = sys.argv[1] if len(sys.argv) > 1 else "3000"
    if os.name == "nt":
        wsl_path = "/mnt/" + ROOT[0].lower() + ROOT[2:].replace("\\", "/")
        return subprocess.call(["wsl", "-d", "Ubuntu", "--", "python3",
                                wsl_path + "/tests/native/test_edge_native.py", cases])
    with tempfile.TemporaryDirectory(prefix="edge-native-") as tmp:
        with open(os.path.join(tmp, "edge_refs.h"), "w") as f:
            for name, spec in LOOPS.items():
                f.write(ref_source(name, *spec))
        exe = os.path.join(tmp, "edge_native_test")
        cmd = [os.environ.get("CC", "gcc")] + OPTS + ["-D" + d for d in DEFS] + [
            "-I" + tmp, "-I" + SPU, "-I" + os.path.join(ROOT, "ps3recomp", "include"),
            "-I" + os.path.join(ROOT, "native"),
            os.path.join(HERE, "edge_native_test.c"), "-o", exe, "-lm"]
        if subprocess.call(cmd):
            return 1
        return subprocess.call([exe, cases])


if __name__ == "__main__":
    sys.exit(main())
