#!/usr/bin/env python3
"""Optimise for this PC (gowa.py kit-build --pgo): the compile options, what the training copy links against, and the
launcher's bar.

The training copy links clang's profile runtime into a DLL that has no C runtime. What the runtime takes from one comes
from ucrtbase.dll and kernel32.dll (port/kit/ucrtbase_pgo.def, kernel32_pgo.def), from the program's table, or from
port/game_module.c under GOWA_PGO_GEN. CI has no clang to link it, so this checks every function the runtime of LLVM
23.1.3 leaves to others (llvm-nm --undefined-only clang_rt.profile-x86_64.lib) against those, and that the DLL a
player plays with is built as before.

Usage:
    python tests/test_kit_pgo.py
"""
import io
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import gowa  # noqa: E402

# clang_rt.profile-x86_64.lib of LLVM 23.1.3: what it does not define. The two bias variables are left out: the runtime
# names a default for each with /alternatename, which the link takes.
RUNTIME_NEEDS = set("""
    __acrt_iob_func __chkstk __imp_CloseHandle __imp_CreateEventW __imp_CreateFileA __imp_CreateFileMappingW
    __imp_FlushViewOfFile __imp_GetComputerNameExW __imp_GetLastError __imp_GetNativeSystemInfo
    __imp_GetOverlappedResult __imp_LockFileEx __imp_MapViewOfFile __imp_UnlockFile __imp_UnmapViewOfFile
    __imp_VirtualUnlock __imp_WideCharToMultiByte __security_check_cookie __security_cookie __stdio_common_vfprintf
    __stdio_common_vsprintf _chsize _errno _fdopen _fileno _get_osfhandle _mkdir _open_osfhandle _putenv atexit atoi
    calloc fclose fdopen feof fflush fileno fopen fread free fseek ftell fwrite getenv getpid malloc memcpy memset open
    realloc strchr strcmp strdup strerror strlen strncpy strrchr
""".split())


def def_names(name):
    text = (ROOT / "port" / "kit" / name).read_text(encoding="utf-8")
    body = text.split("EXPORTS", 1)[1]
    return {line.split(";")[0].strip() for line in body.splitlines() if line.split(";")[0].strip()}


def module_names():
    """What port/game_module.c defines by name: the table's thunks (game_module.h's GAME_MODULE_CRT), __chkstk, and
    under GOWA_PGO_GEN its cookie and the PLAIN and ALIAS thunks."""
    src = (ROOT / "port" / "game_module.c").read_text(encoding="utf-8")
    hdr = (ROOT / "port" / "game_module.h").read_text(encoding="utf-8")
    crt = re.search(r"#define GAME_MODULE_CRT\(X\)(.*?)\n\n", hdr, re.S).group(1)
    names = set(re.findall(r"X\((\w+)\)", crt)) | {"__chkstk"}
    pgo = src.split("#ifdef GOWA_PGO_GEN", 1)[1].split("#endif", 1)[0]
    names |= set(re.findall(r"PLAIN\((\w+)\)", pgo)) | set(re.findall(r"ALIAS\((\w+),", pgo))
    names |= set(re.findall(r"^(?:unsigned long long|void) (__security_\w+)", pgo, re.M))
    return names


class Flags(unittest.TestCase):
    def test_the_players_dll_is_built_as_before(self):
        for kind, src in (("ppu", "x.cpp"), ("spu", "x.c"), ("c", "game_module.c")):
            f = gowa.module_flags(kind, src)
            self.assertFalse([x for x in f if "profile" in x or "PGO" in x], kind)

    def test_gen_instruments_the_lifted_code_and_gives_the_module_its_writer(self):
        self.assertIn("-fprofile-instr-generate", gowa.module_flags("ppu", "x.cpp", "gen"))
        self.assertIn("-fprofile-instr-generate", gowa.module_flags("spu", "x.c", "gen"))
        c = gowa.module_flags("c", "game_module.c", "gen")
        self.assertIn("/DGOWA_PGO_GEN", c)
        self.assertFalse([x for x in c if x.startswith("-fprofile")])

    def test_a_profile_is_named_and_the_writer_left_out(self):
        prof = r"C:\work\train\game.profdata"
        self.assertIn("-fprofile-instr-use=" + prof, gowa.module_flags("spu", "x.c", prof))
        self.assertIn("-fprofile-instr-use=" + prof, gowa.module_flags("ppu", "x.cpp", prof))
        self.assertNotIn("/DGOWA_PGO_GEN", gowa.module_flags("c", "game_module.c", prof))


class Imports(unittest.TestCase):
    def test_every_function_the_runtime_takes_from_others_is_given(self):
        k32, ucrt, mod = def_names("kernel32_pgo.def"), def_names("ucrtbase_pgo.def"), module_names()
        missing = sorted(n for n in RUNTIME_NEEDS
                         if not (n[6:] in k32 if n.startswith("__imp_") else (n in ucrt or n in mod)))
        self.assertEqual(missing, [])

    def test_no_name_comes_from_two_places(self):
        # A ucrtbase name the module also defines would bring a second __imp_ of it: a duplicate at the link.
        self.assertEqual(sorted(def_names("ucrtbase_pgo.def") & module_names()), [])

    def test_the_training_kernel32_has_the_players(self):
        self.assertLessEqual(def_names("kernel32.def"), def_names("kernel32_pgo.def"))
        self.assertLessEqual({"CreateThread", "Sleep", "MoveFileExA"}, def_names("kernel32_pgo.def"))


class Bar(unittest.TestCase):
    def test_the_training_is_on_the_bar_only_when_asked(self):
        off = gowa.KitProgress(io.StringIO(), 4, gowa.KIT_CHUNKS)
        on = gowa.KitProgress(io.StringIO(), 4, gowa.KIT_CHUNKS, pgo=True)
        off.closed = on.closed = True
        self.assertNotIn("train", off.ORDER)
        self.assertEqual(off.ORDER, gowa.KitProgress.ORDER)
        for name in ("train_spu_programs", "train_compile", "train_link", "train", "merge"):
            self.assertIn(name, on.ORDER)
        self.assertLess(on.ORDER.index("merge"), on.ORDER.index("compile"))
        self.assertEqual(set(on.w), set(on.ORDER))


class Settings(unittest.TestCase):
    def test_ini_read_takes_sections_keys_and_quotes(self):
        p = ROOT / "tests" / "_kit_pgo_ini.tmp"
        p.write_text('[paths]\nHDD0 = "C:\\a b\\hdd0"\n; a note\n[patches]\ndisable_shadows = on\n', encoding="utf-8")
        try:
            ini = gowa.ini_read(str(p))
        finally:
            p.unlink()
        self.assertEqual(ini["paths"]["hdd0"], "C:\\a b\\hdd0")
        self.assertEqual(ini["patches"], {"disable_shadows": "on"})

    def test_nothing_to_take_off_is_taken_off(self):
        self.assertTrue(gowa.junction_off(str(ROOT / "tests" / "no-such-junction")))


if __name__ == "__main__":
    unittest.main()
