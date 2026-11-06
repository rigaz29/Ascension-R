#!/usr/bin/env python3
"""game_find.cpp on a made-up tree: what a folder holds, the search, and the links.

Builds a small driver with clang-cl and runs it on folders under a temporary directory. No game
file is used: a PARAM.SFO is written here, and a GOWA.elf of the right hash cannot be, so the
best a made-up copy reaches is GAME_OTHER_BUILD.

Usage:
    python tests/test_game_find.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''
#include "game_find.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
static const char* name(GameState s)
{
    static const char* const n[] = { "none", "other_title", "no_elf", "other_build", "ok" };
    return n[s];
}
static void out(const wchar_t* tag, const GameInfo& g, int copies)
{
    printf("%ls %s %ls %s %s %d\n", tag, name(g.state), g.dir[0] ? g.dir : L"-", g.title_id[0] ? g.title_id : "-",
           g.app_ver[0] ? g.app_ver : "-", copies);
}
int wmain(int argc, wchar_t** argv)
{
    GameInfo g;
    wchar_t err[2048] = L"", t[2048] = L"";
    if (!wcscmp(argv[1], L"probe")) { game_probe(argv[2], &g); out(L"probe", g, 0); }
    else if (!wcscmp(argv[1], L"locate")) { game_locate(argv[2], &g); out(L"locate", g, 0); }
    else if (!wcscmp(argv[1], L"search")) {
        volatile long stop = 0;
        GameSearch s = { argv[2], &stop, NULL, 20, 0 };
        game_search(&s, &g);
        out(L"search", g, s.copies);
    } else if (!wcscmp(argv[1], L"link")) {
        const int ok = game_link(argv[2], argv[3], err, 2048);
        printf("link %d %ls\n", ok, err);
    } else if (!wcscmp(argv[1], L"target")) {
        const int ok = game_link_target(argv[2], t, 2048);
        printf("target %d %ls\n", ok, t);
    }
    return 0;
}
'''


def sfo(title_id="NPUA80918", app_ver="01.12", title="God of War: Ascension"):
    """A PARAM.SFO with three text entries."""
    items = [("APP_VER", app_ver), ("TITLE", title), ("TITLE_ID", title_id)]
    keys = b""
    data = b""
    table = b""
    for k, v in items:
        raw = v.encode("utf-8") + b"\0"
        table += struct.pack("<HHIII", len(keys), 0x0204, len(raw), len(raw), len(data))
        keys += k.encode() + b"\0"
        data += raw
    key_tab = 20 + len(table)
    data_tab = key_tab + len(keys)
    return struct.pack("<4sIIII", b"\0PSF", 0x101, key_tab, data_tab, len(items)) + table + keys + data


def make_game(path, elf=True, **kw):
    usr = Path(path) / "USRDIR"
    usr.mkdir(parents=True)
    (Path(path) / "PARAM.SFO").write_bytes(sfo(**kw))
    (usr / "GOWA.SELF").write_bytes(b"self")
    if elf:
        (usr / "GOWA.elf").write_bytes(b"not the real one")


@unittest.skipUnless(os.name == "nt", "Windows only")
class GameFind(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="game-find-")
        cls.exe = os.path.join(cls.tmp, "t.exe")
        src = os.path.join(cls.tmp, "t.cpp")
        with open(src, "w") as f:
            f.write(DRIVER)
        sys.path.insert(0, str(ROOT / "scripts"))
        import gowa
        env = gowa.toolchain_env()
        clang = shutil.which("clang-cl", path=env.get("PATH"))
        if not clang:
            raise unittest.SkipTest("no clang-cl")
        cmd = [clang, "/nologo", "/O1", "/EHsc", "/std:c++17", "/I" + str(ROOT), src, str(ROOT / "game_find.cpp"),
               "/Fe" + cls.exe, "/Fo" + cls.tmp + "\\"]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=cls.tmp)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="tree-", dir=self.tmp)

    def run_t(self, *a):
        r = subprocess.run([self.exe, *a], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip().split(" ", 5) if a[0] != "link" and a[0] != "target" else r.stdout.strip().split(" ", 2)

    def p(self, *parts):
        return os.path.join(self.d, *parts)

    def test_a_folder_with_no_game(self):
        self.assertEqual(self.run_t("probe", self.d)[1], "none")

    def test_this_title_with_no_elf(self):
        make_game(self.p("g"), elf=False)
        self.assertEqual(self.run_t("probe", self.p("g"))[1], "no_elf")

    def test_an_elf_of_another_build(self):
        make_game(self.p("g"))
        out = self.run_t("probe", self.p("g"))
        self.assertEqual(out[1], "other_build")
        self.assertEqual(out[3:5], ["NPUA80918", "01.12"])

    def test_another_title(self):
        make_game(self.p("g"), title_id="BCUS98232", title="God of War: Ascension")
        self.assertEqual(self.run_t("probe", self.p("g"))[1], "other_title")

    def test_a_broken_param_sfo(self):
        (Path(self.d) / "PARAM.SFO").write_bytes(b"\0PSF" + b"\xff" * 40)
        self.assertEqual(self.run_t("probe", self.d)[1], "none")

    def test_what_a_player_might_pick(self):
        make_game(self.p("emu", "dev_hdd0", "game", "NPUA80918"))
        for pick in (("emu",), ("emu", "dev_hdd0"), ("emu", "dev_hdd0", "game"),
                     ("emu", "dev_hdd0", "game", "NPUA80918"), ("emu", "dev_hdd0", "game", "NPUA80918", "USRDIR")):
            out = self.run_t("locate", self.p(*pick))
            self.assertEqual(out[1], "other_build", pick)
            self.assertTrue(out[2].endswith("NPUA80918"), out)

    def test_a_disc_layout(self):
        make_game(self.p("dump", "PS3_GAME"))
        self.assertTrue(self.run_t("locate", self.p("dump"))[2].endswith("PS3_GAME"))

    def test_the_search_finds_it_deep_down_and_counts_copies(self):
        make_game(self.p("a", "b", "c", "d", "e", "RPCS3", "dev_hdd0", "game", "NPUA80918"))
        make_game(self.p("x", "renamed game"), elf=False)
        make_game(self.p("y", "BLES00000"), title_id="BLES00000", title="Another game")
        out = self.run_t("search", self.d)
        self.assertEqual(out[1], "other_build")
        self.assertTrue(out[2].endswith("NPUA80918"), out)
        self.assertEqual(out[5], "2")

    def test_the_search_goes_past_links(self):
        make_game(self.p("real", "g"))
        os.makedirs(self.p("l"))
        self.assertEqual(self.run_t("link", self.p("l", "loop"), self.d)[1], "1")
        self.assertEqual(self.run_t("search", self.d)[5], "1")

    def test_a_link_is_made_read_and_replaced(self):
        make_game(self.p("one"))
        make_game(self.p("two"))
        link = self.p("deep", "er", "PS3_GAME")
        self.assertEqual(self.run_t("link", link, self.p("one"))[1], "1")
        self.assertTrue(os.path.isfile(os.path.join(link, "PARAM.SFO")))
        self.assertEqual(self.run_t("target", link)[2], self.p("one"))
        self.assertEqual(self.run_t("link", link, self.p("two"))[1], "1")
        self.assertEqual(self.run_t("target", link)[2], self.p("two"))
        self.assertTrue(os.path.isfile(self.p("one", "PARAM.SFO")), "what it led to stays")

    def test_a_folder_with_files_stays(self):
        os.makedirs(self.p("mine"))
        Path(self.p("mine", "save.dat")).write_bytes(b"x")
        make_game(self.p("g"))
        self.assertEqual(self.run_t("link", self.p("mine"), self.p("g"))[1], "0")
        self.assertTrue(os.path.isfile(self.p("mine", "save.dat")))

    def test_an_empty_folder_gives_way(self):
        os.makedirs(self.p("empty"))
        make_game(self.p("g"))
        self.assertEqual(self.run_t("link", self.p("empty"), self.p("g"))[1], "1")
        self.assertTrue(os.path.isfile(self.p("empty", "PARAM.SFO")))

    def test_the_game_in_the_links_place_needs_no_link(self):
        make_game(self.p("here"))
        self.assertEqual(self.run_t("link", self.p("here"), self.p("here"))[1], "1")
        self.assertTrue(os.path.isfile(self.p("here", "PARAM.SFO")))


if __name__ == "__main__":
    unittest.main()
