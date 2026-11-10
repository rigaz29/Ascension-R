/* Ascension-R -- the C library's declarations for the game's DLL as the Kit builds it (port/kit/README.md). */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct _iobuf { void* _Placeholder; } FILE;

__declspec(dllimport) FILE* __cdecl __acrt_iob_func(unsigned);
#define stdin  (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
#define EOF (-1)
#define BUFSIZ 512

/* The Universal C Runtime's formatting entries. Option 2 is C99's snprintf: the full length back, the end cut. */
__declspec(dllimport) int __cdecl __stdio_common_vfprintf(unsigned __int64, FILE*, const char*, void*, va_list);
__declspec(dllimport) int __cdecl __stdio_common_vsprintf(unsigned __int64, char*, size_t, const char*, void*, va_list);

__declspec(dllimport) int __cdecl fflush(FILE*);
__declspec(dllimport) int __cdecl fputc(int, FILE*);
__declspec(dllimport) int __cdecl fputs(const char*, FILE*);
__declspec(dllimport) int __cdecl puts(const char*);
__declspec(dllimport) size_t __cdecl fwrite(const void*, size_t, size_t, FILE*);
__declspec(dllimport) size_t __cdecl fread(void*, size_t, size_t, FILE*);
__declspec(dllimport) FILE* __cdecl fopen(const char*, const char*);
__declspec(dllimport) int __cdecl fclose(FILE*);

__inline int __cdecl vfprintf(FILE* f, const char* fmt, va_list a)
{
    return __stdio_common_vfprintf(0, f, fmt, NULL, a);
}
__inline int __cdecl fprintf(FILE* f, const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    const int r = vfprintf(f, fmt, a);
    va_end(a);
    return r;
}
__inline int __cdecl vprintf(const char* fmt, va_list a)
{
    return vfprintf(stdout, fmt, a);
}
__inline int __cdecl printf(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    const int r = vfprintf(stdout, fmt, a);
    va_end(a);
    return r;
}
__inline int __cdecl vsnprintf(char* buf, size_t n, const char* fmt, va_list a)
{
    const int r = __stdio_common_vsprintf(2, buf, n, fmt, NULL, a);
    return r < 0 ? -1 : r;
}
__inline int __cdecl snprintf(char* buf, size_t n, const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    const int r = vsnprintf(buf, n, fmt, a);
    va_end(a);
    return r;
}

#ifdef __cplusplus
}
#endif
