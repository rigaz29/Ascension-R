#!/usr/bin/env python3
"""The C runtime the Kit's game DLL calls is in its table (port/game_module.h).

The Kit builds ascension_game.dll on the player's PC against nothing: no import library and no C runtime. What it calls
of the C runtime comes through the program's table, GAME_MODULE_CRT and GAME_MODULE_IMPORTS. A native routine that
calls one more function links in the one exe of a source build, and the DLL's link fails, on the player's PC: calloc
and atol in native/spu_edge_memo.c did, and only the alpha.4 package found it. CI has no game to build the DLL from,
so this reads the DLL's hand-written sources (native/) for calls to C runtime functions and looks for each in the table.

Usage:
    python tests/test_game_module_crt.py
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The C runtime's functions a native routine might call.
CRT = set("""
    abort abs atexit atof atoi atol atoll bsearch calloc exit free getenv labs llabs malloc qsort rand realloc srand
    strtod strtof strtol strtoll strtoul strtoull _aligned_free _aligned_malloc _aligned_realloc aligned_alloc
    memchr memcmp memcpy memmove memset strcat strchr strcmp strcpy strcspn strdup _strdup strlen strncat strncmp
    strncpy strnlen strpbrk strrchr strspn strstr strtok _stricmp _strnicmp
    fclose feof ferror fflush fgetc fgets fopen fprintf fputc fputs fread fseek ftell fwrite getc printf putc putchar
    puts remove rename snprintf sprintf sscanf vfprintf vprintf vsnprintf vsprintf _snprintf
    acos acosf asin asinf atan atanf atan2 atan2f cbrt cbrtf ceil ceilf cos cosf cosh exp expf exp2 exp2f fabs fabsf
    floor floorf fma fmaf fmax fmaxf fmin fminf fmod fmodf frexp frexpf hypot hypotf ldexp ldexpf llrint llround log
    logf log10 log10f log2 log2f lrint lrintf lround lroundf modf modff nearbyint nearbyintf pow powf rint rintf
    round roundf sin sinf sinh sqrt sqrtf tan tanf tanh trunc truncf
    clock time localtime gmtime strftime difftime
    isalnum isalpha isdigit islower isspace isupper isxdigit tolower toupper
""".split())

# The UCRT's stdio functions are inline in its headers, each over one common function, which is what the DLL imports.
UCRT_INLINE = {"fprintf": "__stdio_common_vfprintf", "printf": "__stdio_common_vfprintf",
               "vfprintf": "__stdio_common_vfprintf", "vprintf": "__stdio_common_vfprintf",
               "snprintf": "__stdio_common_vsprintf", "sprintf": "__stdio_common_vsprintf",
               "vsnprintf": "__stdio_common_vsprintf", "vsprintf": "__stdio_common_vsprintf",
               "_snprintf": "__stdio_common_vsprintf", "sscanf": "__stdio_common_vsscanf"}
# Called in native/ and taken by the compiler itself: the alpha.4 DLL's map has no reference to any of them. memset of
# a known size is stores, fabs and fminf are instructions, fmaf is FMA, ldexpf is folded. A memset of a size known only
# when it runs would be a call: then memset goes into the table, as any of these must if the link says so.
INLINED = {"memset", "fabs", "fabsf", "fminf", "fmaf", "ldexpf"}
# In a branch for other systems only: spu_edge_verify.h takes _aligned_malloc on Windows.
OTHER_SYSTEMS = {"aligned_alloc"}


def table():
    src = (ROOT / "port" / "game_module.h").read_text(encoding="utf-8")
    names = set()
    for macro in ("GAME_MODULE_CRT", "GAME_MODULE_IMPORTS"):
        body = re.search(r"#define %s\(X\)(.*?)(?:\n\n|\n/\*)" % macro, src, re.S).group(1)
        names |= set(re.findall(r"X\((\w+)\)", body))
    return names


def calls(path):
    """The functions called in a C file, with its comments and string and character literals taken out."""
    text = path.read_text(encoding="utf-8", errors="replace")
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\\n])+'", "''", text)
    return set(re.findall(r"(?<![\w.>])([A-Za-z_]\w*)\s*\(", text))


class Table(unittest.TestCase):
    def test_the_native_routines_call_only_what_the_table_has(self):
        have = table()
        self.assertIn("malloc", have)
        files = sorted((ROOT / "native").glob("spu_*.c")) + sorted((ROOT / "native").glob("*.h"))
        self.assertTrue(files)
        missing = {}
        for f in files:
            for name in sorted(calls(f) & (CRT | set(UCRT_INLINE))):
                need = UCRT_INLINE.get(name, name)
                if need in have or name in INLINED or name in OTHER_SYSTEMS:
                    continue
                missing.setdefault("%s (%s)" % (name, need) if need != name else name, []).append(f.name)
        self.assertEqual(missing, {}, "C runtime calls the DLL's table lacks: put each in GAME_MODULE_IMPORTS "
                                      "(or GAME_MODULE_CRT), and change GAME_MODULE_ABI")

    def test_the_two_that_were_missing(self):
        self.assertTrue({"atol", "calloc"} <= table())


if __name__ == "__main__":
    unittest.main()
