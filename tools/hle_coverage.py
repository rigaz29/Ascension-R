#!/usr/bin/env python3
"""hle_coverage.py - which of a title's firmware imports the runtime answers.

Reads GOWA.imports.json (written by ps3recomp's ppu_loader.py) and checks every
imported NID against the two places ps3recomp registers HLE handlers:

  * libs/**/*.c       non-static cell*/sys*/sce* definitions, which
                      gen_hle_nids.py turns into the registration table
  * runtime/**/*.c*   explicit ps3_hle_register*(nid, "name", fn) calls
                      (sysPrxForUser, cellFs, cellGcm bodies live here)

A NID found in neither is unregistered: at run time the dispatcher reports it
once and answers CELL_OK (PS3_HLE_UNRESOLVED=ok, the default), so the title
keeps going on a faked result. Those are the functions to implement.

Usage:
    python tools/hle_coverage.py [--ps3recomp DIR] [--imports FILE] [--markdown]
"""
import argparse
import glob
import json
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REG_RE = re.compile(
    r'ps3_hle_register\w*\(\s*'
    r'(?:(0x[0-9A-Fa-f]+)[uU]?|ps3_compute_nid\(\s*"(\w+)"\s*\))\s*,\s*"(\w+)"')


def implemented_nids(root):
    sys.path.insert(0, os.path.join(root, "tools"))
    import gen_hle_nids
    from nid_database import compute_nid

    found = {}
    for p in glob.glob(os.path.join(root, "libs", "**", "*.c"), recursive=True):
        for nid, name in gen_hle_nids.scan(p):
            found.setdefault(nid, (name, os.path.relpath(p, root)))
    for ext in ("c", "cpp"):
        for p in glob.glob(os.path.join(root, "runtime", "**", "*." + ext), recursive=True):
            if os.sep + "tests" + os.sep in p:
                continue
            txt = open(p, encoding="latin1").read()
            for m in REG_RE.finditer(txt):
                hexnid, by_name, name = m.groups()
                nid = int(hexnid, 16) if hexnid else compute_nid(by_name)
                found.setdefault(nid, (name, os.path.relpath(p, root)))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ps3recomp", default=os.path.join(HERE, "ps3recomp"))
    ap.add_argument("--imports", default=os.path.join(HERE, "analysis", "GOWA.imports.json"))
    ap.add_argument("--markdown", action="store_true", help="emit a Markdown table")
    args = ap.parse_args()

    root = os.path.abspath(args.ps3recomp)
    found = implemented_nids(root)
    from nid_database import get_default_db
    db = get_default_db()

    by_lib = defaultdict(lambda: [0, []])
    for e in json.load(open(args.imports)):
        nid = int(e["nid"], 16)
        entry = by_lib[e["library"]]
        entry[0] += 1
        if nid not in found:
            hit = db.lookup_nid(nid)
            entry[1].append(hit[1] if hit else "0x%08x" % nid)

    total = sum(n for n, _ in by_lib.values())
    missing = sum(len(m) for _, m in by_lib.values())
    rows = sorted(by_lib.items(), key=lambda kv: (-len(kv[1][1]), kv[0]))
    if args.markdown:
        print("Registered: **%d/%d**\n" % (total - missing, total))
        print("| Library | Registered | Unregistered |")
        print("|---|---|---|")
        for lib, (n, m) in rows:
            print("| `%s` | %d/%d | %s |" % (lib, n - len(m), n,
                  ", ".join("`%s`" % x for x in m) or "-"))
    else:
        print("registered: %d/%d" % (total - missing, total))
        for lib, (n, m) in rows:
            print("  %-15s %3d/%-3d %s" % (lib, n - len(m), n,
                  ("missing: " + ", ".join(m)) if m else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
