#!/usr/bin/env python3
"""check_repo.py - what must never be committed: the game's files, code lifted from them, binaries and keys.

Looks at every file git tracks here; the ps3recomp submodule is a repository of its own. Prints each file it rejects
and exits 1. CI runs it on every push and pull request.
"""
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Top-level folders that hold the game or what is made from it (.gitignore keeps them out; this catches `git add -f`).
BAD_FOLDERS = {"analysis", "dist", "firmware", "gamedata", "logs", "recompiled", "scratch", "spu", "vfs"}
BAD_TYPES = {
    ".elf", ".self", ".sprx", ".prx", ".pkg", ".rap", ".rif", ".edat", ".sdat", ".psarc", ".iso", ".pup",  # game, firmware
    ".exe", ".dll", ".lib", ".obj", ".o", ".a", ".pdb", ".map", ".bin", ".zip", ".7z", ".rar",             # builds, archives
    ".bik", ".pam", ".mp4", ".at3", ".png", ".jpg", ".jpeg", ".dds", ".gtf", ".pad",                       # media, recordings
    ".ttf", ".otf", ".woff", ".woff2",                                                                      # fonts
}
BAD_NAMES = {"eboot.bin", "param.sfo", "local.json"}
# The port's own pictures, by path and SHA-256: scripts/brand/brand.py draws them from the precompile screen's scene, and
# prints these when they change. Any other picture, or these changed, is turned away like the rest.
OWN_PICTURES = {
    "docs/images/banner.png": "90f7745d759e8ec44c58387e2504d25b0f2eb1a13194b2d1a17a3902a4e1ee42",
    "docs/images/icon.png": "0fc50d06490490a9819497f4c2c040e6f4d5d26e7cc3516af1c0155038b001d8",
}
# The launcher's fonts, unchanged from google/fonts, under the SIL Open Font License (THIRD_PARTY_NOTICES.md).
FONTS = {
    "third_party/silkscreen/Silkscreen-Regular.ttf": "c845473330b94c2079ce9af01c51ac8ba2d99c24f4d14c039843bbb8e642ebd8",
    "third_party/silkscreen/Silkscreen-Bold.ttf": "768476aa712d4f5c3e18d3bce80f980a8bd3f72b7094d22ec5e768df3acfed61",
    "third_party/pixelify-sans/PixelifySans-VariableFont_wght.ttf":
        "9ba86cd010a4de309d263ceff8e8044092c9db7efda869620cb9ff1c4389e8a5",
}
MAGIC = ((b"\x7fELF", "an ELF"), (b"SCE\0", "a SELF"), (b"PSAR", "a PSARC archive"), (b"NPD\0", "an EDAT"), (b"\x7fPKG", "a PKG"))
MAX_BYTES = 1024 * 1024
# Lifted code touches guest registers on nearly every line; the hand-written routines in native/ stay near a hundred.
LIFTED_MARK = b"ctx->gpr["
LIFTED_MAX = 2000
# SHA-256 of the 16-byte PS3 keys, so that this file can look for them without holding them.
KEY_SHA256 = frozenset((
    "cfa5223b91a38855b43bc5a7e7ccaf15ae6a68331b53419acfc767e22d6a6330",
    "aeba3d895e250ea2bc55ed9513a67cf9d27150b96381944c016b60b92dde7053",
    "54978dc85d27e051d571cbd3cdf330ff444ee31a909f75d46f9ea6139d86968b",
    "7eddc78a29cd6ecaecf46d101ab2143d018861bebecf3608f84909ed825fdc63",
    "c8e21f6dd8476c3a10df2e7c64fa3020ad59eedd3c6541c5afe764b15ec461df",
    "5947fd14bf3071aded6844ea37ad122e9b3ed8c09346662e3a5713d30ae24cf5",
    "bf6c55cc1eba27306d02aaece4b6c7dc7abe944a32bc7fd08dd5bbfd25c066d6",
    "9f59799419060804ae46982ba023f56c897206b951daeb507b8dcb384e597b5f",
    "3071f966c90bf91e16a81e14b4dc7ac24267dcd95d9311fc0be97acd910d1e5f",
    "e72fbe08bd7e9970564baaa97f84a6962865b3843f893637470df6df47727c56",
))
HEX_LIST = re.compile(rb"(?:(?:0[xX])?[0-9a-fA-F]{2}[,\s]+){15,}(?:0[xX])?[0-9a-fA-F]{2}")
HEX_ITEM = re.compile(rb"(?:0[xX])?([0-9a-fA-F]{2})(?![0-9a-fA-F])")
HEX_STRING = re.compile(rb"[0-9a-fA-F]{32,}")


def holds_key(buf):
    return any(hashlib.sha256(buf[i:i + 16]).hexdigest() in KEY_SHA256 for i in range(len(buf) - 15))


def has_key(data):
    if holds_key(data):
        return True
    for m in HEX_LIST.finditer(data):
        if holds_key(bytes(int(x, 16) for x in HEX_ITEM.findall(m.group(0)))):
            return True
    for m in HEX_STRING.finditer(data):
        s = m.group(0)
        if holds_key(bytes.fromhex(s[:len(s) & ~1].decode("ascii"))):
            return True
    return False


def problems(path, data):
    """Why this tracked file must not be in the repository: an empty list if it may."""
    out = []
    parts = path.split("/")
    top = parts[0].lower()
    if len(parts) > 1 and (top in BAD_FOLDERS or top.startswith("build")):
        out.append("in %s/, which stays on your machine" % parts[0])
    name = parts[-1].lower()
    digest = hashlib.sha256(data).hexdigest()
    own = OWN_PICTURES.get(path) == digest or FONTS.get(path) == digest
    if name in BAD_NAMES or (os.path.splitext(name)[1] in BAD_TYPES and not own):
        out.append("a file type that stays out of git")
    for magic, what in MAGIC:
        if data.startswith(magic):
            out.append("%s, by its first bytes" % what)
    if data.startswith(b"MZ") and b"\0" in data[:1024]:
        out.append("a Windows binary, by its first bytes")
    if len(data) > MAX_BYTES:
        out.append("%.1f MB, over the limit of %d MB" % (len(data) / 1048576.0, MAX_BYTES // 1048576))
    if data.count(LIFTED_MARK) > LIFTED_MAX:
        out.append("looks like lifted code")
    if has_key(data):
        out.append("holds a PS3 key")
    return out


def main():
    listing = subprocess.run(["git", "-C", ROOT, "ls-files", "-s", "-z"], stdout=subprocess.PIPE, check=True).stdout
    checked = rejected = 0
    for rec in listing.split(b"\0"):
        if not rec:
            continue
        meta, path = rec.split(b"\t", 1)
        if meta.startswith(b"160000"):  # a submodule
            continue
        path = path.decode("utf-8", "surrogateescape")
        with open(os.path.join(ROOT, path), "rb") as f:
            why = problems(path, f.read())
        checked += 1
        if why:
            rejected += 1
            print("%s: %s" % (path, "; ".join(why)))
    print("check_repo: %d files checked, %d rejected" % (checked, rejected))
    return 1 if rejected else 0


if __name__ == "__main__":
    sys.exit(main())
