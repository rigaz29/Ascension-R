/*
 * Ascension-R -- the CPU check
 *
 * The game's code is built for AVX2 and FMA3, and some of it runs in static constructors, so this
 * is the exe's entry point: it comes ahead of the C runtime. A CPU without them gets a message
 * instead of an illegal instruction. Built without either, like the rest of the port's own files.
 */
#include <windows.h>
#include <intrin.h>

int mainCRTStartup(void* peb);

static int cpu_has_avx2_fma(void)
{
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return 0;
    __cpuid(r, 1);
    const int need = (1 << 12) | (1 << 27) | (1 << 28);   /* FMA3, OSXSAVE, AVX */
    if ((r[2] & need) != need) return 0;
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) return 0;                          /* Windows saves the YMM registers */
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;                        /* AVX2 */
}

int gowa_entry(void* peb)
{
    if (!cpu_has_avx2_fma()) {
        MessageBoxW(NULL, L"Ascension-R needs a CPU with AVX2 and FMA3, and this PC's does not have them.",
                    L"Ascension-R", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }
    return mainCRTStartup(peb);
}
