"""The game's text: write it out for a translator, and build a translation back into a mod folder.

    gowa.py text export [--part story] [--context] [--out build/text/en-story.tsv]
    gowa.py text build build/text/id-story.tsv [more files] [--out build/mods]

Where the text is (docs/notes/2026-11-11-game-text.md): the MSGS_TXT table of R_LANG_<language>.WAD, which each update
from TPPS1.PSARC (1.06) to TPPS5.PSARC (1.11) replaces through a .WAD.PATCH, and the multiplayer R_LANG_<language>_MP
the same way, with the same text. The game reads PATCHES, opens TPPS1 to TPPS<n>, and takes a .WAD.PATCH from the
newest that has it. So `build` writes TPPS<n+1>.PSARC with both patches, and PATCHES holding n+1, into a folder laid
out like the PS3's paths, for `paths.mods`. The game's folder is only read.

The export has a line for each line of text: `<id>.<k><TAB><text>`, k counting the entry's lines that have a letter.
The subtitle cues (`[[S:...]]`), the `[[F:n]]` lines and lines of tokens alone stay out; `build` takes them from the
game's table. `--context` adds a third column, the entry's sound, which says who speaks and where. A translation is a
file of the same keys, as many as are done: `<id>.<k><TAB><translation>`, anything after a second tab ignored, lines
starting with # and empty lines skipped. `build` refuses a line that drops or adds a [..] token, that is empty, or
that has a character the English text never uses (the font may lack it); curly quotes, dashes and the ellipsis
become their ASCII forms. One line stays one line, so the table's counts stay right; `\\n` breaks a line inside one.

What both write is made from the game and stays on this machine: build/ is not tracked.
"""
import argparse
import hashlib
import io
import os
import re
import string
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TITLE_ID = "NPUA80918"
WAD_DIR = "/WAD/PS3_BE/"
BLOCK = 65536

ID_LINE = re.compile(r"\*(\d+)\*([A-Z]*)")
TOKEN = re.compile(r"\[\[[^\]]*\]\]|\[[^\[\]]*\]")
CUE = re.compile(r"\[\[S:([^:\]]+)")
LETTER = re.compile(r"[^\W\d_]")
PARTS = ("story", "mp", "bonus", "menus")
# What a translator's editor tends to type where the English has ASCII.
ASCII_FORMS = {"“": '"', "”": '"', "„": '"', "‘": "'", "’": "'", "…": "...",
               "–": "-", "—": "-", " ": " "}


class TextError(Exception):
    pass


# ---------------------------------------------------------------------------
# PSARC (1.4, zlib, absolute names)
# ---------------------------------------------------------------------------

def _be(b):
    return int.from_bytes(b, "big")


def _psarc_entries(f, wanted, what):
    """{name: bytes} of the entries named in wanted that the archive open in f has."""
    h = f.read(32)
    if h[:4] != b"PSAR" or h[8:12] != b"zlib":
        raise TextError("%s is not a zlib PSARC" % what)
    toc_len, esz, n, block = _be(h[12:16]), _be(h[16:20]), _be(h[20:24]), _be(h[24:28])
    t = f.read(toc_len - 32)
    bsz = 2 if block <= 65536 else 3 if block <= 16777216 else 4
    zs = [_be(t[k:k + bsz]) for k in range(n * esz, len(t) - bsz + 1, bsz)]
    ents = [(_be(t[i * esz + 16:i * esz + 20]), _be(t[i * esz + 20:i * esz + 25]), _be(t[i * esz + 25:i * esz + 30]))
            for i in range(n)]

    def read(ent):
        first, size, off = ent
        data = bytearray()
        f.seek(off)
        b = first
        while len(data) < size:
            want = min(block, size - len(data))
            z = zs[b] if b < len(zs) else 0
            take = z if z else block
            s = f.read(take)
            zhead = len(s) >= 2 and s[0] == 0x78 and s[1] in (0xDA, 0x9C, 0x01, 0x5E)
            if not z or (take == want and not zhead):    # stored: a whole block, or packed no smaller
                data += s[:want]
            else:
                data += zlib.decompress(s)
            b += 1
        return bytes(data)

    names = read(ents[0]).decode("utf-8", "replace").split("\n")
    return {name: read(ent) for name, ent in zip(names, ents[1:]) if name in wanted}


def psarc_read(path, wanted):
    with open(path, "rb") as f:
        return _psarc_entries(f, wanted, path)


def psarc_read_bytes(data, wanted):
    return _psarc_entries(io.BytesIO(data), wanted, "the archive")


def psarc_pack(files):
    """A PSARC as the game's are made: entries sorted by name, each keyed by the MD5 of its name as written, the
    manifest (entry 0, the names a line each) first, 64 KiB blocks packed with zlib unless that is no smaller."""
    names = sorted(files)
    datas = [("\n".join(names)).encode()] + [files[n] for n in names]
    blocks, zsizes, firsts = [], [], []
    for d in datas:
        firsts.append(len(zsizes))
        for at in range(0, len(d), BLOCK):
            raw = d[at:at + BLOCK]
            z = zlib.compress(raw, 9)
            if len(z) < len(raw):
                blocks.append(z)
                zsizes.append(len(z))
            else:
                blocks.append(raw)
                zsizes.append(0 if len(raw) == BLOCK else len(raw))
    toc_len = 32 + 30 * len(datas) + 2 * len(zsizes)
    out = bytearray(b"PSAR" + struct.pack(">HH", 1, 4) + b"zlib"
                    + struct.pack(">IIIII", toc_len, 30, len(datas), BLOCK, 2))   # flag 2: absolute names
    at = toc_len
    for i, d in enumerate(datas):
        md5 = b"\0" * 16 if i == 0 else hashlib.md5(names[i - 1].encode()).digest()
        out += md5 + struct.pack(">I", firsts[i]) + len(d).to_bytes(5, "big") + at.to_bytes(5, "big")
        last = firsts[i + 1] if i + 1 < len(datas) else len(blocks)
        at += sum(len(b) for b in blocks[firsts[i]:last])
    for z in zsizes:
        out += struct.pack(">H", z)
    for b in blocks:
        out += b
    return bytes(out)


# ---------------------------------------------------------------------------
# .WAD.PATCH and its MSGS_TXT
# ---------------------------------------------------------------------------

def _patch_parts(patch):
    """(version, steps, start, k): steps are [offset, length]; k is the step that takes the patch's own data."""
    ver, n = struct.unpack(">HH", patch[:4])
    steps = [list(struct.unpack(">II", patch[4 + 8 * i:12 + 8 * i])) for i in range(n)]
    own = [i for i, (off, _) in enumerate(steps) if off & 0x80000000]
    if len(own) != 1 or steps[own[0]][0] & 0x7FFFFFFF:
        raise TextError("a .WAD.PATCH of another shape: %r" % steps)
    return ver, steps, 4 + 8 * n, own[0]


def _wad_entries(stream):
    """(header, body) for each entry of a WAD stream: a 64-byte header (u16 tag, u16 flags, u32 size, the name),
    the data, padding to 16 bytes."""
    o = 0
    while o + 64 <= len(stream):
        size = struct.unpack(">I", stream[o + 4:o + 8])[0]
        yield stream[o:o + 64], stream[o + 64:o + 64 + size]
        o = (o + 64 + size + 15) & ~15


def patch_msgs(patch):
    _, steps, start, k = _patch_parts(patch)
    for hdr, body in _wad_entries(patch[start:start + steps[k][1]]):
        if hdr[8:17] == b"MSGS_TXT\0":
            return body
    raise TextError("no MSGS_TXT in the patch")


def patch_with_msgs(patch, msgs):
    """The patch with another MSGS_TXT; its own step's length follows. The rest is the patch's, byte for byte."""
    ver, steps, start, k = _patch_parts(patch)
    stream = bytearray()
    for hdr, body in _wad_entries(patch[start:start + steps[k][1]]):
        hdr = bytearray(hdr)
        if hdr[8:17] == b"MSGS_TXT\0":
            body = msgs
            struct.pack_into(">I", hdr, 4, len(body))
        stream += hdr + body
        stream += b"\0" * (-len(stream) & 15)
    steps[k][1] = len(stream)
    out = bytearray(struct.pack(">HH", ver, len(steps)))
    for off, size in steps:
        out += struct.pack(">II", off, size)
    return bytes(out + stream)


class Table:
    """A MSGS_TXT table: its lines as they are (CRLF between them, the last one the NUL), and for each line of text a
    key, `<id>.<k>`, with its entry's part and sound."""

    def __init__(self, raw):
        self.lines = raw.decode("utf-8").split("\r\n")
        # The first line holds the counts; the last is the NUL after the final CRLF.
        self.end = len(self.lines) - 1 if self.lines[-1] in ("\0", "") else len(self.lines)
        self.keys = {}       # key -> line index
        self.part = {}       # key -> part
        self.sound = {}      # key -> the entry's first subtitle sound, or ""
        entry = None
        entries = []
        for i, ln in enumerate(self.lines[1:self.end], 1):
            m = ID_LINE.fullmatch(ln)
            if m:
                entry = [int(m.group(1)), []]
                entries.append(entry)
            elif entry is not None:
                entry[1].append(i)
        for eid, idx in entries:
            body = [self.lines[i] for i in idx]
            sounds = [c for ln in body for c in CUE.findall(ln)]
            part = self._part(sounds)
            k = 0
            for i in idx:
                if LETTER.search(TOKEN.sub("", self.lines[i]).replace("\\n", " ")):
                    k += 1
                    key = "%d.%d" % (eid, k)
                    self.keys[key] = i
                    self.part[key] = part
                    self.sound[key] = sounds[0] if sounds else ""

    @staticmethod
    def _part(sounds):
        if not sounds:
            return "menus"
        if any(s.startswith("BTS_") for s in sounds):
            return "bonus"
        if any(re.match(r"(a\d+_|AN_|HD_|MP)", s) for s in sounds):
            return "mp"
        return "story"

    def text(self, key):
        return self.lines[self.keys[key]]

    def chars(self):
        return set("".join(self.lines[1:self.end]))

    def raw(self):
        return "\r\n".join(self.lines).encode("utf-8")


# ---------------------------------------------------------------------------
# the game's folder
# ---------------------------------------------------------------------------

def default_game_dir():
    cands = [os.path.join(ROOT, "gamedata", "dev_hdd0", "game", TITLE_ID)]
    local = os.path.join(ROOT, "local.json")
    if os.path.isfile(local):
        import json
        g = json.load(open(local)).get("game_dir")
        if g:
            cands.append(g)
    cands.append(os.path.join(ROOT, "vfs", "PS3_GAME"))
    for c in cands:
        if os.path.isfile(os.path.join(c, "USRDIR", "PATCHES")):
            return c
    return cands[0]


def patch_count(usrdir):
    with open(os.path.join(usrdir, "PATCHES"), encoding="ascii", errors="replace") as f:
        first = (f.read().splitlines() or [""])[0].strip()
    return int(first) if first.isdigit() else 0


def newest_patches(usrdir, lang):
    """{name: (archive, bytes)} of the language's two text patches, each from the newest update archive that has it,
    and the number of updates PATCHES names."""
    n = patch_count(usrdir)
    names = [WAD_DIR + "R_LANG_%s.WAD.PATCH" % lang, WAD_DIR + "R_LANG_%s_MP.WAD.PATCH" % lang]
    found = {}
    for k in range(n, 0, -1):
        arc = os.path.join(usrdir, "TPPS%d.PSARC" % k)
        left = [nm for nm in names if nm not in found]
        if not left or not os.path.isfile(arc):
            continue
        for nm, data in psarc_read(arc, set(left)).items():
            found[nm] = ("TPPS%d.PSARC" % k, data)
    if names[0] not in found:
        raise TextError("no update archive in %s has %s: the text comes with the updates, 1.04 to 1.12"
                        % (usrdir, names[0].rsplit("/", 1)[1]))
    return found, n


# ---------------------------------------------------------------------------
# export and build
# ---------------------------------------------------------------------------

def export_lines(table, parts, context):
    out = []
    for key, i in table.keys.items():
        if table.part[key] not in parts:
            continue
        ln = table.lines[i]
        if "\t" in ln:
            raise TextError("a tab in %s's text" % key)
        out.append("%s\t%s%s" % (key, ln, "\t" + table.sound[key] if context else ""))
    return out


def read_translation(paths):
    """{key: text} from translation files; a key twice is an error."""
    got, errors = {}, []
    for p in paths:
        with open(p, encoding="utf-8-sig") as f:
            for no, ln in enumerate(f.read().split("\n"), 1):
                ln = ln.rstrip("\r")
                if not ln.strip() or ln.startswith("#"):
                    continue
                cols = ln.split("\t")
                if len(cols) < 2:
                    errors.append("%s:%d: no tab between the key and the text" % (p, no))
                    continue
                key = cols[0].strip()
                if key in got:
                    errors.append("%s:%d: %s a second time" % (p, no, key))
                    continue
                got[key] = cols[1]
    return got, errors


def apply(table, tr):
    """Put the translations in, after checking each. Returns (lines changed, errors). ASCII letters, digits and the
    space are always there (a capitals font draws lower case as capitals); any other character only if the English
    uses it: one of the title's fonts has no # $ < > ^ ` { | } ~."""
    allowed = table.chars() | set(string.ascii_letters + string.digits + " ")
    errors, changed = [], 0
    for key, text in tr.items():
        if key not in table.keys:
            errors.append("%s: no such line in the game's text" % key)
            continue
        text = "".join(ASCII_FORMS.get(c, c) if c not in allowed else c for c in text).strip()
        en = table.text(key)
        if not text:
            errors.append("%s: empty" % key)
            continue
        want, have = sorted(TOKEN.findall(en)), sorted(TOKEN.findall(text))
        if want != have:
            errors.append("%s: the tokens are %s, the English has %s" % (key, " ".join(have) or "none", " ".join(want)))
            continue
        bad = sorted({c for c in text if c not in allowed})
        if bad:
            errors.append("%s: %s, which the English never uses (the font may lack it)"
                          % (key, ", ".join("U+%04X" % ord(c) for c in bad)))
            continue
        if text != en:
            table.lines[table.keys[key]] = text
            changed += 1
    return changed, errors


def cmd_export(args):
    usr = os.path.join(args.game_dir, "USRDIR")
    found, _ = newest_patches(usr, args.lang)
    arc, patch = found[WAD_DIR + "R_LANG_%s.WAD.PATCH" % args.lang]
    table = Table(patch_msgs(patch))
    parts = set(PARTS) if "all" in args.part else set(args.part)
    lines = export_lines(table, parts, args.context)
    entries = {k.split(".")[0] for k, _ in (ln.split("\t", 1) for ln in lines)}
    named = "all" if parts == set(PARTS) else "+".join(sorted(parts))
    out = args.out or os.path.join(ROOT, "build", "text", "%s-%s.tsv" % (args.lang.lower(), named))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    head = ["# Ascension-R text: R_LANG_%s from %s, %s: %d lines of %d entries."
            % (args.lang, arc, ", ".join(sorted(parts)), len(lines), len(entries)),
            "# A line: <key><TAB><text>. Translate the text, keep the key. Keep every [..] token as it is; \\n breaks",
            "# a line inside one. One line stays one line. A translation file needs only the lines done."]
    if args.context:
        head.append("# The third column, the line's sound, says who speaks and where. It is not translated.")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(head + lines) + "\n")
    shown = os.path.relpath(out, ROOT) if os.path.abspath(out).startswith(ROOT) else out
    print("%s: %d lines of %d entries (%s), %d characters"
          % (shown, len(lines), len(entries), ", ".join(sorted(parts)), sum(len(ln) for ln in lines)))
    return 0


def cmd_build(args):
    usr = os.path.join(args.game_dir, "USRDIR")
    found, n = newest_patches(usr, args.lang)
    tr, errors = read_translation(args.files)
    sp = WAD_DIR + "R_LANG_%s.WAD.PATCH" % args.lang
    table = Table(patch_msgs(found[sp][1]))
    changed, more = apply(table, tr)
    errors += more
    if errors:
        for e in errors[:40]:
            print("error: " + e)
        if len(errors) > 40:
            print("error: and %d more" % (len(errors) - 40))
        print("nothing written")
        return 1
    files = {}
    for name, (_, patch) in found.items():
        t = table if name == sp else Table(patch_msgs(patch))
        if t is not table:
            apply(t, tr)
        files[name] = patch_with_msgs(patch, t.raw())
    root = os.path.abspath(args.out or os.path.join(ROOT, "build", "mods"))
    dst = os.path.join(root, "dev_hdd0", "game", TITLE_ID, "USRDIR")
    os.makedirs(dst, exist_ok=True)
    arc = os.path.join(dst, "TPPS%d.PSARC" % (n + 1))
    with open(arc, "wb") as f:
        f.write(psarc_pack(files))
    with open(os.path.join(dst, "PATCHES"), "wb") as f:
        f.write(b"%d" % (n + 1))
    print("%s: TPPS%d.PSARC (%d KB) and PATCHES (%d); %d lines translated, %d of them changed"
          % (root, n + 1, os.path.getsize(arc) // 1024, n + 1, len(tr), changed))
    print("the game reads it with `paths.mods = %s` in [paths] of its settings, or --set paths.mods=%s"
          % (root, root.replace("\\", "/")))
    return 0


def add_args(p):
    sub = p.add_subparsers(dest="text_cmd", required=True)
    for name, helptext in (("export", "the game's text, a line for each line, for a translator"),
                           ("build", "a translation into a mod folder: TPPS<n+1>.PSARC and PATCHES")):
        q = sub.add_parser(name, help=helptext)
        q.add_argument("--game-dir", default=default_game_dir(),
                       help="the game's folder, holding USRDIR (default: the one this tree runs, or local.json's)")
        q.add_argument("--lang", default="EN", help="the language whose text is read and replaced (default EN)")
        if name == "export":
            q.add_argument("--part", nargs="+", default=["all"], choices=PARTS + ("all",),
                           help="story (the game's own voices), mp, bonus (the behind-the-scenes videos), menus "
                                "(everything without a sound: menus, tutorials, lore, items, trophies), all")
            q.add_argument("--context", action="store_true", help="a third column, the line's sound")
            q.add_argument("--out", help="the file (default build/text/<lang>-<parts>.tsv)")
        else:
            q.add_argument("files", nargs="+", help="translation files")
            q.add_argument("--out", help="the mod folder (default build/mods)")


def main(args):
    try:
        return cmd_export(args) if args.text_cmd == "export" else cmd_build(args)
    except TextError as e:
        print("error: %s" % e)
        return 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    add_args(ap)
    sys.exit(main(ap.parse_args()))
