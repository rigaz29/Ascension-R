/*
 * Ascension-R -- what the port's own files need on every host
 *
 * The port was written against Win32 alone. These are the few things its files use on every host, spelt once:
 * on Windows each is what it always was, and on macOS (and other POSIX hosts) its nearest equivalent.
 *
 *   - the MSVC string functions (_strdup, _stricmp, _strnicmp, strtok_s);
 *   - port_mem_mapped: whether a range of host memory can be read without a fault (VirtualQuery on Windows);
 *   - PORT_TRY / PORT_EXCEPT: a read of the guest's memory that a fault must not end. On Windows that is SEH. The
 *     guest's 4 GB is mapped whole on POSIX (main.cpp, alloc_guest_vm), so such a read cannot fault there and the
 *     guard is the plain block;
 *   - port_cpu_relax: the spin-wait hint (pause on x86-64, yield on arm64);
 *   - port_exe_dir: the folder of the running program.
 */
#ifndef ASCENSION_PORT_COMPAT_H
#define ASCENSION_PORT_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <excpt.h>
#  define PORT_TRY    __try
#  define PORT_EXCEPT __except (EXCEPTION_EXECUTE_HANDLER)
#  define PORT_PATH_SEP '\\'
#else
#  include "win32_compat.h"   /* ps3recomp's: DWORD, LONG, the Interlocked calls, CreateThread, Sleep... */
#  include <stdlib.h>
#  include <strings.h>
#  include <unistd.h>
#  include <sys/mman.h>
#  include <limits.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
#  ifndef _strdup
#    define _strdup strdup
#  endif
#  ifndef _stricmp
#    define _stricmp strcasecmp
#  endif
#  ifndef _strnicmp
#    define _strnicmp strncasecmp
#  endif
#  ifndef strtok_s
#    define strtok_s strtok_r
#  endif
#  ifndef _byteswap_ushort
#    define _byteswap_ushort(x) __builtin_bswap16(x)
#    define _byteswap_ulong(x)  __builtin_bswap32(x)
#    define _byteswap_uint64(x) __builtin_bswap64(x)
#  endif
#  define PORT_TRY    if (1)
#  define PORT_EXCEPT else
#  define PORT_PATH_SEP '/'
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  include <immintrin.h>
#  define port_cpu_relax() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define port_cpu_relax() __asm__ __volatile__("yield")
#else
#  define port_cpu_relax() ((void)0)
#endif

/* Whether [p, p + n) is memory this process can read. */
static inline int port_mem_mapped(const void* p, size_t n)
{
#ifdef _WIN32
    const uint8_t* a = (const uint8_t*)p;
    const uint8_t* e = a + n;
    while (a < e) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mi.Protect)
            return 0;
        a = (const uint8_t*)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
#else
    if (!n) return 1;
    const uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE);
    const uintptr_t lo = (uintptr_t)p & ~(pg - 1);
    const uintptr_t hi = ((uintptr_t)p + n + pg - 1) & ~(pg - 1);
    /* mincore fails with ENOMEM on any page that is not mapped. Its vector is one byte a page; checked a chunk at a
     * time so that a large range needs no large buffer. */
#  if defined(__APPLE__)
    char vec[256];
#  else
    unsigned char vec[256];
#  endif
    for (uintptr_t a = lo; a < hi;) {
        uintptr_t len = hi - a;
        if (len > 256 * pg) len = 256 * pg;
        if (mincore((void*)a, (size_t)len, vec) != 0) return 0;
        a += len;
    }
    return 1;
#endif
}

/* The folder the program is in, without a separator at the end. 0 on failure. */
static inline int port_exe_dir(char* out, size_t n)
{
    if (!n) return 0;
    out[0] = 0;
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (!k || k >= n) { out[0] = 0; return 0; }
#elif defined(__APPLE__)
    char raw[4096], real[PATH_MAX];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0) return 0;
    const char* got = realpath(raw, real) ? real : raw;
    if (strlen(got) >= n) return 0;
    strcpy(out, got);
#else
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k <= 0) return 0;
    out[k] = 0;
#endif
    char* s = strrchr(out, PORT_PATH_SEP);
    if (s) *s = 0;
    return 1;
}

#endif /* ASCENSION_PORT_COMPAT_H */
