#!/usr/bin/env python3
"""update_feed.cpp: the launcher's version numbers and its reading of GitHub's list of releases.

Builds a small driver with clang-cl around port/update_feed.cpp (no Windows, no network) and gives it versions and
made-up lists shaped as api.github.com's: each release's own "html_url" after the "html_url" of its author and assets,
notes with braces and quotes in them, a draft, a page elsewhere, an error from the API, a list cut short.

Usage:
    python tests/test_update_check.py
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r'''
#include "update_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
int main(int argc, char** argv)
{
    if (argc == 3 && !strcmp(argv[1], "parse")) {
        ArVersion v;
        if (!update_version_parse(argv[2], &v)) { printf("bad\n"); return 0; }
        printf("%d %d %d %d %s %d\n", v.major, v.minor, v.patch, v.pre, v.pre ? v.pre_label : "-", v.pre_n);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "cmp")) {
        ArVersion a, b;
        if (!update_version_parse(argv[2], &a) || !update_version_parse(argv[3], &b)) { printf("bad\n"); return 0; }
        const int c = update_version_cmp(a, b);
        printf("%d\n", c < 0 ? -1 : c > 0 ? 1 : 0);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "pick")) {
        FILE* f = fopen(argv[2], "rb");
        if (!f) return 2;
        std::string s;
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, got);
        fclose(f);
        UpdateRelease r = {};
        const int st = update_pick(s.data(), s.size(), argv[3], &r);
        if (st > 0) printf("found %s %s %d\n", r.tag, r.url, r.prerelease);
        else printf(st == 0 ? "none\n" : "error\n");
        return 0;
    }
    return 2;
}
'''

PAGE = "https://github.com/rigaz29/Ascension-R/releases/tag/"


def release(tag, pre=True, draft=False, url=None, body="Notes."):
    """A release as GitHub's list has it: the author and an asset, each with an html_url, before the release's own."""
    return {
        "url": "https://api.github.com/repos/rigaz29/Ascension-R/releases/1",
        "author": {"login": "someone", "html_url": "https://github.com/someone", "type": "User"},
        "assets": [{"name": "Ascension-R-kit.zip", "browser_download_url": "https://github.com/x/y.zip",
                    "uploader": {"html_url": "https://github.com/someone"}}],
        "html_url": url if url is not None else PAGE + tag,
        "id": 1,
        "tag_name": tag,
        "name": "Ascension-R " + tag,
        "draft": draft,
        "prerelease": pre,
        "body": body,
        "reactions": {"total_count": 0},
    }


class UpdateFeed(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="update-check-")
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
        cmd = [clang, "/nologo", "/O1", "/EHsc", "/std:c++17", "/I" + str(ROOT / "port"), src,
               str(ROOT / "port" / "update_feed.cpp"), "/Fe" + cls.exe, "/Fo" + cls.tmp + "\\"]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=cls.tmp)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_t(self, *a):
        r = subprocess.run([self.exe, *a], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.strip()

    def pick(self, text, current="0.1.0-alpha.7", encoding="utf-8"):
        path = os.path.join(self.tmp, "list.json")
        Path(path).write_text(text, encoding=encoding)
        return self.run_t("pick", path, current)

    def test_versions_read(self):
        self.assertEqual(self.run_t("parse", "0.1.0-alpha.7"), "0 1 0 1 alpha 7")
        self.assertEqual(self.run_t("parse", "v0.1.0-alpha.8"), "0 1 0 1 alpha 8")
        self.assertEqual(self.run_t("parse", "1.2.3"), "1 2 3 0 - 0")
        self.assertEqual(self.run_t("parse", "0.2.0-RC.1"), "0 2 0 1 rc 1")
        self.assertEqual(self.run_t("parse", "0.2.0-beta"), "0 2 0 1 beta 0")
        for bad in ["", "0.1", "0.1.0-", "0.1.0-alpha.", "0.1.0-alpha.8x", "x0.1.0", "0.1.0+build", "0.1.0-alpha.8-dev",
                    "0.1.0-2", "1..0"]:
            self.assertEqual(self.run_t("parse", bad), "bad", bad)

    def test_versions_in_order(self):
        order = ["0.1.0-alpha.7", "0.1.0-alpha.8", "0.1.0-alpha.10", "0.1.0-beta.1", "0.1.0-rc.1", "0.1.0",
                 "0.1.1-alpha.1", "0.1.1", "0.2.0", "1.0.0"]
        for a, b in zip(order, order[1:]):
            self.assertEqual(self.run_t("cmp", a, b), "-1", (a, b))
            self.assertEqual(self.run_t("cmp", b, a), "1", (b, a))
        self.assertEqual(self.run_t("cmp", "v0.1.0-alpha.8", "0.1.0-alpha.8"), "0")

    def test_the_newest_release_past_this_one(self):
        rels = [release("v0.1.0-alpha.8"), release("v0.1.0-alpha.10"), release("v0.1.0-alpha.9"),
                release("v0.1.0-alpha.7"), release("v0.1.0-alpha.5")]
        self.assertEqual(self.pick(json.dumps(rels, indent=2)),
                         "found v0.1.0-alpha.10 %sv0.1.0-alpha.10 1" % PAGE)
        self.assertEqual(self.pick(json.dumps(rels, separators=(",", ":"))),
                         "found v0.1.0-alpha.10 %sv0.1.0-alpha.10 1" % PAGE)

    def test_a_file_with_a_utf8_mark(self):
        rels = [release("v0.1.0-alpha.8")]
        self.assertEqual(self.pick(json.dumps(rels), encoding="utf-8-sig"),
                         "found v0.1.0-alpha.8 %sv0.1.0-alpha.8 1" % PAGE)

    def test_a_release_and_not_a_pre_release(self):
        rels = [release("v0.1.0", pre=False), release("v0.1.0-rc.1")]
        self.assertEqual(self.pick(json.dumps(rels)), "found v0.1.0 %sv0.1.0 0" % PAGE)

    def test_none_newer(self):
        rels = [release("v0.1.0-alpha.7"), release("v0.1.0-alpha.6")]
        self.assertEqual(self.pick(json.dumps(rels)), "none")
        self.assertEqual(self.pick("[]"), "none")
        self.assertEqual(self.pick(json.dumps([release("v0.1.0-alpha.8")]), current="0.1.0-alpha.8"), "none")

    def test_a_draft_is_passed_over(self):
        rels = [release("v0.1.0-alpha.9", draft=True), release("v0.1.0-alpha.8")]
        self.assertEqual(self.pick(json.dumps(rels)), "found v0.1.0-alpha.8 %sv0.1.0-alpha.8 1" % PAGE)

    def test_a_page_elsewhere_is_passed_over(self):
        rels = [release("v0.1.0-alpha.9", url="https://example.com/releases/tag/v0.1.0-alpha.9"),
                release("v0.1.0-alpha.8", url="https://github.com/someone/Ascension-R/releases/tag/v0.1.0-alpha.8")]
        self.assertEqual(self.pick(json.dumps(rels)), "none")

    def test_tags_that_are_not_versions_are_passed_over(self):
        rels = [release("nightly"), release("v0.1.0-alpha.8-dev"), release("v0.1.0-alpha.8")]
        self.assertEqual(self.pick(json.dumps(rels)), "found v0.1.0-alpha.8 %sv0.1.0-alpha.8 1" % PAGE)

    def test_notes_with_braces_and_quotes(self):
        body = 'Fixes {"tag_name": "v9.9.9", "html_url": "%sv9.9.9"} and a \\ "quote" ] } [ {' % PAGE
        rels = [release("v0.1.0-alpha.8", body=body)]
        self.assertEqual(self.pick(json.dumps(rels)), "found v0.1.0-alpha.8 %sv0.1.0-alpha.8 1" % PAGE)

    def test_not_a_list(self):
        self.assertEqual(self.pick('{"message": "API rate limit exceeded", "documentation_url": "x"}'), "error")
        self.assertEqual(self.pick(""), "error")
        cut = json.dumps([release("v0.1.0-alpha.8"), release("v0.1.0-alpha.9")])
        self.assertEqual(self.pick(cut[:len(cut) // 2]), "error")
        self.assertEqual(self.pick(json.dumps([release("v0.1.0-alpha.8")]), current="dev"), "error")


if __name__ == "__main__":
    unittest.main()
