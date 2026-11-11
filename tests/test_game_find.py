#!/usr/bin/env python3
"""game_find.cpp on a made-up tree: what a folder holds, the search, and the links.

Builds a small driver with clang-cl and runs it on folders under a temporary directory. No game
file is used: a PARAM.SFO is written here, and a GOWA.elf of the right hash cannot be, so the
best a made-up copy reaches is GAME_OTHER_BUILD. The game's files are three made-up ones, in a
list of the test's own (GAME_FILES_INC) worked out by gowa.py's ends_sha256, as `game-files` does.

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
    static const char* const n[] = { "none", "other_title", "incomplete", "no_elf", "other_build", "ok" };
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
    else if (!wcscmp(argv[1], L"files")) {
        game_probe(argv[2], &g);
        wchar_t text[300] = L"-";
        if (g.state == GAME_INCOMPLETE) game_files_text(&g, text, 300);
        printf("files %s %d %d %d %llu %s %s %d\n%ls\n", name(g.state), g.files, g.files_missing, g.files_wrong,
               g.bytes_missing, g.first_missing[0] ? g.first_missing : "-", g.first_wrong[0] ? g.first_wrong : "-",
               g.other_version, text);
    } else if (!wcscmp(argv[1], L"needed")) {
        char p[512];
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, p, sizeof p, NULL, NULL);
        printf("needed %d\n", game_file_needed(p));
    }
    else if (!wcscmp(argv[1], L"locate")) { game_locate(argv[2], &g); out(L"locate", g, 0); }
    else if (!wcscmp(argv[1], L"search") || !wcscmp(argv[1], L"search_first")) {
        /* search_first: as the launcher looks, the first copy that is this release will do */
        volatile long stop = 0;
        GameSearch s = { argv[2], &stop, NULL, 20, 0, !wcscmp(argv[1], L"search_first") ? GAME_NO_ELF : 0 };
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


# The game's files of the test's list: one larger than an end (64 KiB), one with ends that overlap, smaller ones; the
# base game's and update 1.12's (its TPPS archives and PATCHES), in the order game_files.inc has them.
TEST_FILES = (("USRDIR/GAME.PSARC", 200000), ("USRDIR/ENGLISH.PSARC", 70000), ("USRDIR/TPPS1.PSARC", 1000),
              ("USRDIR/ARCHIVES", 40), ("USRDIR/PATCHES", 1))


def file_bytes(rel, size):
    seed = sum(rel.encode())
    return bytes((i * 31 + (i >> 9) + seed) & 0xFF for i in range(size))


TEST_DATA = {rel: file_bytes(rel, size) for rel, size in TEST_FILES}


def make_game(path, elf=True, files=True, **kw):
    usr = Path(path) / "USRDIR"
    usr.mkdir(parents=True)
    (Path(path) / "PARAM.SFO").write_bytes(sfo(**kw))
    (usr / "GOWA.SELF").write_bytes(b"self")
    if elf:
        (usr / "GOWA.elf").write_bytes(b"not the real one")
    if files:
        for rel, data in TEST_DATA.items():
            Path(path, *rel.split("/")).write_bytes(data)


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
        cls.gowa = gowa
        # The test's list of the game's files, worked out from files on disk as `gowa.py game-files` does.
        rows = []
        for rel, data in TEST_DATA.items():
            f = os.path.join(cls.tmp, "file.bin")
            Path(f).write_bytes(data)
            rows.append('{ "%s", %dull, "%s" },' % (rel, len(data), gowa.ends_sha256(f)))
        cls.inc = os.path.join(cls.tmp, "test_files.inc")
        Path(cls.inc).write_text("\n".join(rows) + "\n")
        env = gowa.toolchain_env()
        clang = shutil.which("clang-cl", path=env.get("PATH"))
        if not clang:
            raise unittest.SkipTest("no clang-cl")
        cmd = [clang, "/nologo", "/O1", "/EHsc", "/std:c++17", "/I" + str(ROOT / "port"), "/I" + cls.tmp,
               '/DGAME_FILES_INC="test_files.inc"', src, str(ROOT / "port" / "game_find.cpp"),
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
        if a[0] in ("files", "needed"):
            first, _, text = r.stdout.strip().partition("\n")
            return first.split() + ([text] if text else [])
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

    def test_the_games_data_folder_is_no_game(self):
        # RPCS3 keeps NPUA80918-DATA beside the game: this title's PARAM.SFO, and no GOWA.SELF or GOWA.elf.
        data = Path(self.p("NPUA80918-DATA"))
        (data / "USRDIR").mkdir(parents=True)
        (data / "PARAM.SFO").write_bytes(sfo())
        self.assertEqual(self.run_t("probe", str(data))[1], "none")

    def test_the_search_takes_the_game_beside_its_data_folder(self):
        # As the launcher searches, stopping at the first copy of this release: the data folder, walked first, is
        # not one.
        game = self.p("RPCS3", "dev_hdd0", "game", "NPUA80918")
        make_game(game, elf=False)
        data = Path(game + "-DATA")
        (data / "USRDIR").mkdir(parents=True)
        (data / "PARAM.SFO").write_bytes(sfo())
        out = self.run_t("search_first", self.d)
        self.assertEqual(out[1], "no_elf")
        self.assertTrue(out[2].endswith("NPUA80918"), out)

    def test_the_three_files_alone_are_not_the_game(self):
        # PARAM.SFO and GOWA.SELF, as a license would decrypt: nothing to decrypt or build from.
        make_game(self.p("g"), elf=False, files=False)
        out = self.run_t("files", self.p("g"))
        self.assertEqual(out[1:5], ["incomplete", "5", "5", "0"])
        self.assertEqual(out[5], str(sum(size for _, size in TEST_FILES)))
        self.assertEqual(out[6], "USRDIR/GAME.PSARC")
        self.assertEqual(out[9], "Not the whole game: 5 of its 5 files missing (271041 bytes), GAME.PSARC first. "
                                 "Install the game again")

    def test_a_decrypted_elf_does_not_make_up_for_them(self):
        make_game(self.p("g"), files=False)
        self.assertEqual(self.run_t("files", self.p("g"))[1], "incomplete")

    def test_one_file_missing(self):
        make_game(self.p("g"))
        os.remove(self.p("g", "USRDIR", "ARCHIVES"))
        out = self.run_t("files", self.p("g"))
        self.assertEqual(out[1:6], ["incomplete", "5", "1", "0", "40"])
        self.assertEqual(out[6], "USRDIR/ARCHIVES")

    def test_a_file_cut_short(self):
        make_game(self.p("g"))
        f = Path(self.p("g", "USRDIR", "ENGLISH.PSARC"))
        f.write_bytes(f.read_bytes()[:-1])
        out = self.run_t("files", self.p("g"))
        self.assertEqual(out[1:5], ["incomplete", "5", "0", "1"])
        self.assertEqual(out[7], "USRDIR/ENGLISH.PSARC")
        self.assertEqual(out[9], "1 of the game's files is not as version 1.12 has it, ENGLISH.PSARC first. "
                                 "Install the game again")

    def test_the_update_missing(self):
        # Issue #2: the base game's data under 1.12's EBOOT, without PATCHES or the TPPS archives it names.
        make_game(self.p("g"))
        os.remove(self.p("g", "USRDIR", "TPPS1.PSARC"))
        os.remove(self.p("g", "USRDIR", "PATCHES"))
        out = self.run_t("files", self.p("g"))
        self.assertEqual(out[1:5], ["incomplete", "5", "2", "0"])
        self.assertEqual(out[6], "USRDIR/TPPS1.PSARC")
        self.assertEqual(out[9], "Not every update is installed: 2 files missing, TPPS1.PSARC first. "
                                 "Install 1.04 to 1.12, in order")

    def test_an_update_file_of_another_version(self):
        # The base game's own TPPS archive is another, much smaller one.
        make_game(self.p("g"))
        Path(self.p("g", "USRDIR", "TPPS1.PSARC")).write_bytes(b"PSAR" + bytes(108))
        out = self.run_t("files", self.p("g"))
        self.assertEqual(out[1:5], ["incomplete", "5", "0", "1"])
        self.assertEqual(out[9], "Not every update is installed: 1 file differs, TPPS1.PSARC first. "
                                 "Install 1.04 to 1.12, in order")

    def test_the_game_again_when_one_of_its_own_is_missing_too(self):
        make_game(self.p("g"))
        os.remove(self.p("g", "USRDIR", "ENGLISH.PSARC"))
        os.remove(self.p("g", "USRDIR", "PATCHES"))
        out = self.run_t("files", self.p("g"))
        self.assertEqual(out[1:5], ["incomplete", "5", "2", "0"])
        self.assertEqual(out[9], "Not the whole game: 2 of its 5 files missing (70001 bytes), ENGLISH.PSARC first. "
                                 "Install the game again")

    def test_a_file_of_its_size_with_other_bytes(self):
        for rel, size in TEST_FILES:
            with self.subTest(rel):
                g = self.p(rel.replace("/", "_"))
                make_game(g)
                Path(g, *rel.split("/")).write_bytes(bytes(size))
                out = self.run_t("files", g)
                self.assertEqual(out[1:5], ["incomplete", "5", "0", "1"])
                self.assertEqual(out[7], rel)

    def test_its_last_byte_counts(self):
        make_game(self.p("g"))
        f = Path(self.p("g", "USRDIR", "GAME.PSARC"))
        b = bytearray(f.read_bytes())
        b[-1] ^= 1
        f.write_bytes(bytes(b))
        self.assertEqual(self.run_t("files", self.p("g"))[1], "incomplete")

    def test_whole_and_not_decrypted(self):
        make_game(self.p("g"), elf=False)
        self.assertEqual(self.run_t("files", self.p("g"))[1:5], ["no_elf", "5", "0", "0"])

    def test_another_version_is_said_before_its_files(self):
        make_game(self.p("g"), elf=False, files=False, app_ver="01.00")
        out = self.run_t("files", self.p("g"))
        self.assertEqual(out[1], "other_build")
        self.assertEqual(out[8], "1")

    def test_the_search_takes_a_whole_copy_over_one_that_is_not(self):
        # Walked first, the copy with the three files alone would have ended the search before.
        make_game(self.p("a", "NPUA80918"), elf=False, files=False)
        make_game(self.p("b", "NPUA80918"), elf=False)
        out = self.run_t("search_first", self.d)
        self.assertEqual(out[1], "no_elf")
        self.assertTrue(out[2].endswith(os.path.join("b", "NPUA80918")), out)
        self.assertEqual(out[5], "2")

    def test_what_the_game_cannot_start_without(self):
        for path, want in (("USRDIR/GAME.PSARC", "1"), ("usrdir\\game.psarc", "1"), ("USRDIR/ARCHIVES", "1"),
                           ("USRDIR/PATCHES", "1"), ("USRDIR/TPPS1-ENGLISH.PSARC", "0"), ("USRDIR/GAME.PSARC.PATCH", "0"),
                           ("USRDIR", "0"), ("", "0")):
            with self.subTest(path):
                self.assertEqual(self.run_t("needed", path)[1], want)

    def test_which_files_come_with_the_update(self):
        for rel, want in (("USRDIR/TPPS1.PSARC", True), ("usrdir\\tpps.psarc", True), ("USRDIR/TPPS4-ENGLISH.PSARC", True),
                          ("USRDIR/PATCHES", True), ("USRDIR/PATCHES.BAK", False), ("USRDIR/GAME.PSARC", False),
                          ("USRDIR/ARCHIVES", False)):
            with self.subTest(rel):
                self.assertEqual(self.gowa.game_file_update(rel), want)

    def test_the_list_reads_back(self):
        self.assertEqual(self.gowa.read_game_files_inc(self.inc),
                         [(rel, len(data), self.gowa.ends_sha256(self.p_file(rel, data))) for rel, data in TEST_DATA.items()])

    def p_file(self, rel, data):
        f = self.p(rel.replace("/", "_") + ".bin")
        Path(f).write_bytes(data)
        return f

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
