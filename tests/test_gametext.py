#!/usr/bin/env python3
"""gowa.py text (scripts/gametext.py): the export and the build, on a made-up game folder, so no game file is needed.

The folder has PATCHES (5) and a TPPS5.PSARC with the two English text patches, made the way the game's are: a
.WAD.PATCH whose second step brings a WAD stream with MSGS_TXT in it.

Usage:
    python tests/test_gametext.py
"""
import argparse
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gametext as gt  # noqa: E402

MSGS = ("4 1 1 9\r\n"
        "*1*\r\nPause\r\n"
        "*2*T\r\nPress [XButton] to jump\r\nOptions...\r\n"
        "*7*\r\n[[S:ORKINT_OR_04:*]]\r\nI do. \"Yes\"\r\n"
        "*8*\r\n[[S:BTS_Delphi_01:600-4733]]\r\nWe built it.\r\n"
        "*9*\r\n[[F:2]]\r\n[*]\r\nBreakable!\r\n\0").encode()
SP = gt.WAD_DIR + "R_LANG_EN.WAD.PATCH"
MP = gt.WAD_DIR + "R_LANG_EN_MP.WAD.PATCH"


def wad_entry(name, body, tag=7):
    data = struct.pack(">HHI", tag, 0, len(body)) + name.encode().ljust(56, b"\0") + body
    return data + b"\0" * (-len(data) & 15)


def patch(msgs, third):
    stream = wad_entry("MSGS_TXT", msgs) + wad_entry("SMF_0", b"a font table")
    steps = [(0, 0xD00), (0x80000000, len(stream)), (third, 0xC630)]
    return struct.pack(">HH", 4, len(steps)) + b"".join(struct.pack(">II", *s) for s in steps) + stream


class Game:
    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        usr = os.path.join(self.dir, "USRDIR")
        os.makedirs(usr)
        Path(usr, "PATCHES").write_bytes(b"5")
        self.sp, self.mp = patch(MSGS, 0xBBA20), patch(MSGS, 0xCCE40)
        Path(usr, "TPPS5.PSARC").write_bytes(gt.psarc_pack({SP: self.sp, MP: self.mp}))
        return self

    def __exit__(self, *a):
        self.tmp.cleanup()

    def args(self, **kw):
        a = dict(game_dir=self.dir, lang="EN", part=["all"], context=False, out=None, files=[])
        a.update(kw)
        return argparse.Namespace(**a)

    def built(self, out):
        arc = os.path.join(out, "dev_hdd0", "game", gt.TITLE_ID, "USRDIR", "TPPS6.PSARC")
        return gt.psarc_read(arc, {SP, MP}), Path(arc).with_name("PATCHES").read_bytes()


class Formats(unittest.TestCase):
    def test_a_psarc_reads_back(self):
        files = {"/A/B.BIN": bytes(range(256)) * 700, "/A/A.TXT": b"x" * 10}
        self.assertEqual(gt.psarc_read_bytes(gt.psarc_pack(files), set(files)), files)

    def test_a_patch_rebuilds_byte_for_byte(self):
        p = patch(MSGS, 0xBBA20)
        self.assertEqual(gt.patch_msgs(p), MSGS)
        self.assertEqual(gt.patch_with_msgs(p, gt.patch_msgs(p)), p)

    def test_the_keys_leave_out_cues_and_lines_of_tokens(self):
        t = gt.Table(MSGS)
        self.assertEqual(list(t.keys), ["1.1", "2.1", "2.2", "7.1", "8.1", "9.1"])
        self.assertEqual([t.part[k] for k in t.keys], ["menus", "menus", "menus", "story", "bonus", "menus"])
        self.assertEqual(t.sound["7.1"], "ORKINT_OR_04")
        self.assertEqual(t.raw(), MSGS)


class ExportAndBuild(unittest.TestCase):
    def test_export(self):
        with Game() as g:
            out = os.path.join(g.dir, "en.tsv")
            self.assertEqual(gt.cmd_export(g.args(part=["story", "menus"], context=True, out=out)), 0)
            lines = [ln for ln in Path(out).read_text(encoding="utf-8").split("\n") if ln and not ln.startswith("#")]
            self.assertEqual(lines, ["1.1\tPause\t", "2.1\tPress [XButton] to jump\t", "2.2\tOptions...\t",
                                     "7.1\tI do. \"Yes\"\tORKINT_OR_04", "9.1\tBreakable!\t"])

    def test_nothing_translated_gives_the_game_s_patches(self):
        with Game() as g:
            tr = Path(g.dir, "none.tsv")
            tr.write_text("# nothing yet\n", encoding="utf-8")
            out = os.path.join(g.dir, "mods")
            self.assertEqual(gt.cmd_build(g.args(files=[str(tr)], out=out)), 0)
            files, patches = g.built(out)
            self.assertEqual(files, {SP: g.sp, MP: g.mp})
            self.assertEqual(patches, b"6")

    def test_a_translation_goes_into_both_patches(self):
        with Game() as g:
            tr = Path(g.dir, "id.tsv")
            tr.write_text("1.1\tJeda\n2.1\tTekan [XButton] untuk melompat\textra column\n"
                          "7.1\t“Ya”…\n", encoding="utf-8")
            out = os.path.join(g.dir, "mods")
            self.assertEqual(gt.cmd_build(g.args(files=[str(tr)], out=out)), 0)
            files, _ = g.built(out)
            for name in (SP, MP):
                lines = gt.patch_msgs(files[name]).decode().split("\r\n")
                self.assertEqual(len(lines), len(MSGS.decode().split("\r\n")))
                self.assertIn("Jeda", lines)
                self.assertIn("Tekan [XButton] untuk melompat", lines)
                self.assertIn('"Ya"...', lines)                 # curly quotes and the ellipsis made ASCII
                self.assertIn("[[S:ORKINT_OR_04:*]]", lines)     # the cue kept
                self.assertIn("Options...", lines)               # what was not translated kept
            self.assertEqual(files[MP][20:24], struct.pack(">I", 0xCCE40))   # each patch keeps its own third step

    def test_refused_lines_write_nothing(self):
        with Game() as g:
            tr = Path(g.dir, "bad.tsv")
            tr.write_text("2.1\tTekan untuk melompat\n"        # the token dropped
                          "1.1\t\n"                             # empty
                          "3.1\tTidak ada\n"                    # no such line
                          "9.1\tPecahé #\n"                # a letter and a sign the English never uses
                          "2.2\tPilihan\n2.2\tPilihan lagi\n",  # a key twice
                          encoding="utf-8")
            out = os.path.join(g.dir, "mods")
            self.assertEqual(gt.cmd_build(g.args(files=[str(tr)], out=out)), 1)
            self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
