#!/bin/sh
# Applies patches/ps3recomp/*.patch to the ps3recomp submodule: the runtime's own changes this fork needs (the macOS
# build, docs/MACOS.md), kept here until they live in a ps3recomp fork of this fork's own. Run from anywhere, any
# number of times: a patch already in place is left as it is.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
for p in "$root"/patches/ps3recomp/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$root/ps3recomp" apply --reverse --check "$p" 2>/dev/null; then
        echo "already applied: $(basename "$p")"
    else
        git -C "$root/ps3recomp" apply "$p"
        echo "applied: $(basename "$p")"
    fi
done
