#!/usr/bin/env python3
"""A Kit unpacked over an older one builds from its own files only (gowa.py: kit_files, kit_native_sources, kit_extras).

The Kit's DLL compiles every native/spu_*.c it finds. A player who unpacks a new Kit over the old folder keeps the old
Kit's files the new one no longer has, and an old source would go into the DLL. kit-tree writes kit-files.txt, every
file the Kit ships, and the build takes native sources from it. A made-up Kit folder here, no game file.

Usage:
    python tests/test_kit_files.py
"""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gowa  # noqa: E402


class KitFiles(unittest.TestCase):
    def setUp(self):
        self.kit = tempfile.mkdtemp(prefix="kit-")
        for r in ["native/spu_a.c", "native/spu_b.c", "native/spu_b.h", "scripts/gowa.py", "port/game_module.c",
                  "python/python.exe", gowa.KIT_ID]:
            p = Path(self.kit, r)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("x")

    def tearDown(self):
        shutil.rmtree(self.kit, ignore_errors=True)

    def ship(self, *rels):
        Path(self.kit, gowa.KIT_FILES).write_text("".join(r + "\n" for r in rels), encoding="utf-8")

    def test_a_source_tree_takes_every_native_source(self):
        self.assertIsNone(gowa.kit_files(self.kit))
        self.assertEqual(gowa.kit_native_sources(self.kit), ["spu_a.c", "spu_b.c"])
        self.assertEqual(gowa.kit_extras(self.kit), [])

    def test_a_kit_takes_only_the_sources_it_shipped(self):
        self.ship("native/spu_a.c", "native/spu_b.h", "scripts/gowa.py", "port/game_module.c", "python/python.exe")
        self.assertEqual(gowa.kit_native_sources(self.kit), ["spu_a.c"])
        self.assertEqual(gowa.kit_extras(self.kit), ["native/spu_b.c"])

    def test_its_id_list_and_python_caches_are_not_extras(self):
        self.ship("native/spu_a.c", "native/spu_b.c", "native/spu_b.h", "scripts/gowa.py", "port/game_module.c",
                  "python/python.exe")
        cache = Path(self.kit, "scripts", "__pycache__", "gowa.cpython-314.pyc")
        cache.parent.mkdir(parents=True)
        cache.write_text("x")
        self.assertEqual(gowa.kit_extras(self.kit), [])
        self.assertEqual(gowa.kit_native_sources(self.kit), ["spu_a.c", "spu_b.c"])

    def test_the_list_is_read_as_kit_tree_writes_it(self):
        """kit-tree writes the paths with / and one a line; blank lines and the line ends are nothing."""
        Path(self.kit, gowa.KIT_FILES).write_bytes(b"native/spu_a.c\r\n\r\nscripts/gowa.py\n")
        self.assertEqual(gowa.kit_files(self.kit), {"native/spu_a.c", "scripts/gowa.py"})


if __name__ == "__main__":
    unittest.main()
