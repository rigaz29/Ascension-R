#!/usr/bin/env python3
"""How many at a time the build runs (gowa.py: free_jobs, module_jobs), and the lazy-flags pass's quick path.

free_jobs: an explicit --jobs stands; "auto" takes the cores, as far as free memory allows with 2 GB left over, and
never less than one. ppu_lazy_flags.py: line_flags and the rewrite pass a line that names neither ctx->cr nor ctx->xer
without trying the forms, which holds while every form names one of them; a form that does not fails here.

Usage:
    python tests/test_build_jobs.py
"""
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "ps3recomp" / "tools"))
import gowa  # noqa: E402
import ppu_lazy_flags as lf  # noqa: E402


class FreeJobs(unittest.TestCase):
    def jobs(self, arg, cores, free_mb, each_mb=384):
        with mock.patch.object(gowa.os, "cpu_count", return_value=cores), \
             mock.patch.object(gowa.bench, "free_ram_mb", return_value=free_mb):
            return gowa.free_jobs(arg, each_mb)

    def test_an_explicit_count_stands(self):
        self.assertEqual(self.jobs("3", 16, 100), 3)
        self.assertEqual(self.jobs("0", 16, 100000), 1)

    def test_auto_takes_the_cores_when_memory_allows(self):
        self.assertEqual(self.jobs("auto", 12, 2048 + 12 * 384), 12)
        self.assertEqual(self.jobs("auto", 16, 17600), 16)

    def test_auto_takes_what_memory_allows(self):
        self.assertEqual(self.jobs("auto", 12, 4422), 6)
        self.assertEqual(self.jobs("auto", 12, 2048 + 4 * 384 - 1), 3)
        self.assertEqual(self.jobs("auto", 12, 2048 + 2 * 128, gowa.PPU_LIFT_MB), 2)

    def test_auto_is_one_at_least(self):
        self.assertEqual(self.jobs("auto", 8, 1500), 1)
        self.assertEqual(self.jobs("auto", None, 32000), 1)

    def test_a_compile_is_given_384_mb(self):
        with mock.patch.object(gowa.os, "cpu_count", return_value=12), \
             mock.patch.object(gowa.bench, "free_ram_mb", return_value=2048 + 5 * 384):
            self.assertEqual(gowa.module_jobs("auto"), 5)
            self.assertEqual(gowa.module_jobs("2"), 2)


def forms():
    """Every form the pass matches: the module's _W_, _R_, _CA_ and _XER_ patterns and _W_FIXED's."""
    for name, value in sorted(vars(lf).items()):
        if name.startswith(("_W_", "_R_", "_CA_", "_XER_")) and hasattr(value, "pattern"):
            yield name, value
    for rx, _field, _value in lf._W_FIXED:
        yield "_W_FIXED", rx


class LazyFlagsQuickPath(unittest.TestCase):
    def test_every_form_names_cr_or_xer(self):
        names = [n for n, _ in forms()]
        self.assertGreaterEqual(len(names), 20)
        for name, rx in forms():
            self.assertTrue("ctx->cr" in rx.pattern or "ctx->xer" in rx.pattern, "%s: %s" % (name, rx.pattern))

    def program(self, funcs):
        p = lf.Program(funcs)
        p.solve()
        return p

    def test_a_line_without_them_stays_as_it_is(self):
        line = "ctx->gpr[3] = ctx->gpr[4] + 0x10;"
        p = self.program([("func_00010000", [line, "return;"])])
        self.assertEqual(lf.line_flags(line), (0, 0))
        self.assertEqual(lf._lazy_line(line, p), line)

    def test_a_tail_call_still_hands_its_target_what_it_reads(self):
        tail = "{ g_trampoline_fn = (void(*)(void*))func_B; return; }"
        p = self.program([("func_A", [tail]),
                          ("func_B", ["if ((ctx->cr >> 29) & 1) goto loc_1;", "return;", "loc_1:", "return;"])])
        self.assertEqual(lf.line_flags(tail), (0, 0))
        self.assertEqual(lf._lazy_line(tail, p),
                         "{ PPU_LF_OUT(0); g_trampoline_fn = (void(*)(void*))func_B; return; }")

    def test_a_line_with_them_is_still_rewritten(self):
        p = self.program([("func_C", ["ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28);", "return;"])])
        self.assertEqual(lf.line_flags("ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28);"), (0, 0xF << 28))
        self.assertEqual(lf._lazy_line("ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28);", p),
                         "PPU_LF_SET(0, cr_val);")


if __name__ == "__main__":
    unittest.main()
