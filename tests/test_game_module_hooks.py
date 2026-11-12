#!/usr/bin/env python3
"""Every function scripts/ppu_hooks.json has the lift call is in the Kit's table (port/game_module.h).

The lifted code calls a hook by name, and in the Kit's ascension_game.dll that name is one jump through the table the
program fills, GAME_MODULE_FUNCS. A hook left out of it links in the one exe of a source build and fails the DLL's
link on the player's PC. CI has no game to build the DLL from, so this compares the two lists.

Usage:
    python tests/test_game_module_hooks.py
"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def funcs():
    src = (ROOT / "port" / "game_module.h").read_text(encoding="utf-8")
    body = re.search(r"#define GAME_MODULE_FUNCS\(X\)(.*?)(?:\n\n|\n/\*)", src, re.S).group(1)
    return set(re.findall(r"X\((\w+)\)", body))


class Hooks(unittest.TestCase):
    def test_every_hook_is_in_the_table(self):
        hooks = json.loads((ROOT / "scripts" / "ppu_hooks.json").read_text(encoding="utf-8"))["hooks"]
        self.assertTrue(hooks)
        have = funcs()
        missing = sorted({h["fn"] for h in hooks} - have)
        self.assertEqual(missing, [], "hooks the lift calls that GAME_MODULE_FUNCS does not have: %s" % missing)


if __name__ == "__main__":
    unittest.main()
