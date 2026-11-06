#!/usr/bin/env python3
"""The options rsx_dxc.c gives DXC keep the decompilers' guards.

The fragment decompiler ends a program with `(x == x) ? x : 0`, a NaN test, and the vertex
decompiler multiplies through `(a == 0 || b == 0) ? 0 : a * b`, the RSX's 0 x Inf = 0. With fast
floats, DXC's default, the first one folds to `x`, with no warning, and programs still compile.
So this reads what DXC makes of them, with the options in rsx_dxc.c, and checks that the options
are still what keeps them.

Needs dxc.exe from a Windows SDK, skipped without one.

Usage:
    python tests/test_dxc_guards.py
"""

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gowa  # noqa: E402

HLSL = r'''
cbuffer C : register(b1) { float4 k[2]; };
float vp_mul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 main(float4 p : SV_Position, float4 c : COLOR0) : SV_Target {
    float4 x = c / k[0];
    float4 g = (x == x) ? x : (float4)0;
    return g + vp_mul1(c.x, k[1].x);
}
'''


def options():
    """What rsx_dxc_compile passes after the target, entry and level."""
    src = (ROOT / "ps3recomp/libs/video/rsx_dxc.c").read_text()
    words = re.findall(r'L"([^"]*)"', re.search(r"LPCWSTR args\[\] = \{(.*?)\};", src, re.S)[1])
    return words[words.index("main") + 1:]


def compile_main(opts):
    """The DXIL of the test program's main, as text."""
    folder = gowa.find_dxc()
    with tempfile.TemporaryDirectory() as tmp:
        f = os.path.join(tmp, "t.hlsl")
        Path(f).write_text(HLSL)
        r = subprocess.run([os.path.join(folder, "dxc.exe"), "-T", "ps_6_0", "-E", "main", "-O1"] + opts + [f],
                           capture_output=True, text=True, check=True)
    return r.stdout[r.stdout.index("define void @main"):]


@unittest.skipUnless(gowa.find_dxc() and os.path.isfile(os.path.join(gowa.find_dxc() or "", "dxc.exe")), "no dxc.exe")
class Guards(unittest.TestCase):
    def test_guards_are_kept(self):
        text = compile_main(options())
        self.assertEqual(len(re.findall(r"fcmp ord float", text)), 4, text)     # x == x, a lane each
        self.assertEqual(len(re.findall(r"fcmp oeq float", text)), 2, text)     # a == 0, b == 0
        self.assertNotIn(" fast ", text)

    def test_fast_floats_lose_the_nan_test(self):
        """Why -Gis is there: if this fails, DXC stopped folding it and the note in rsx_dxc.c can go."""
        text = compile_main([o for o in options() if o != "-Gis"])
        self.assertIn(" fast ", text)
        self.assertEqual(len(re.findall(r"fcmp fast ord", text)), 0, text)
        self.assertEqual(len(re.findall(r"fcmp fast oeq", text)), 2, text)


class Samplers(unittest.TestCase):
    """dxc_split_samplers works on the decompiler's text. Missed, pipelines are refused, the screen goes black."""

    def setUp(self):
        self.dxc = (ROOT / "ps3recomp/libs/video/rsx_dxc.c").read_text()
        self.fp = (ROOT / "ps3recomp/libs/video/rsx_fp_decompiler.c").read_text()

    def test_it_looks_for_the_declaration_the_decompiler_writes(self):
        self.assertIn(re.search(r'decl\[\] = "([^"]*)"', self.dxc)[1], self.fp)

    def test_the_decompiler_indexes_with_constants_only(self):
        self.assertEqual(sorted(set(re.findall(r"rsx_samp\[([^\]]*)\]", self.fp))), ["%u", "16"])


if __name__ == "__main__":
    unittest.main()
