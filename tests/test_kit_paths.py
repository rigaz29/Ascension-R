#!/usr/bin/env python3
"""gowa.py's messages name a path on another drive as it is (issue #1).

On a player's PC the Kit is beside Ascension-R.exe and GOWA_WORK in the data folder. With the Kit on E: and the data on
C:, os.path.relpath(<the data>, <the Kit>) raised "path is on mount 'C:', start on mount 'E:'", and the Kit's build
stopped on the message after its first lift.

Usage:
    python tests/test_kit_paths.py
"""
import os
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gowa  # noqa: E402


class Shown(unittest.TestCase):
    def test_a_path_in_the_tree_is_relative(self):
        self.assertEqual(gowa.shown(os.path.join(gowa.ROOT, "spu", "wwspm")), os.path.join("spu", "wwspm"))

    @unittest.skipUnless(os.name == "nt", "drives are Windows'")
    def test_a_path_on_another_drive_is_as_it_is(self):
        drive = "F:" if os.path.splitdrive(gowa.ROOT)[0].upper() == "E:" else "E:"
        p = drive + "\\Ascension-R\\work\\spu\\wwspm"
        with self.assertRaises(ValueError):
            os.path.relpath(p, gowa.ROOT)      # what the Kit's build stopped on
        self.assertEqual(gowa.shown(p), p)

    def test_no_message_takes_a_work_folder_relative_to_the_tree(self):
        """The folders under GOWA_WORK, and GOWA_ELF, go through shown() in every message."""
        src = (ROOT / "scripts" / "gowa.py").read_text(encoding="utf-8")
        work = re.findall(r"^([A-Z_]+) = os\.path\.join\((?:WORK|ANALYSIS|RECOMPILED)\b", src, re.M)
        self.assertIn("WWS_PM_DIR", work)
        for name in work + ["elf", "WORK", "ANALYSIS", "RECOMPILED"]:
            self.assertNotIn("os.path.relpath(%s, ROOT)" % name, src, name)


if __name__ == "__main__":
    unittest.main()
