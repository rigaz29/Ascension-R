/* Ascension-R -- the C library's declarations for the game's DLL as the Kit builds it (port/kit/README.md). */
#pragma once
#include <stddef.h>
#include <malloc.h>
#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fff

__declspec(dllimport) char* __cdecl getenv(const char*);
__declspec(dllimport) int __cdecl atoi(const char*);
__declspec(dllimport) long __cdecl atol(const char*);
__declspec(dllimport) double __cdecl atof(const char*);
__declspec(dllimport) long __cdecl strtol(const char*, char**, int);
__declspec(dllimport) unsigned long __cdecl strtoul(const char*, char**, int);
__declspec(dllimport) long long __cdecl strtoll(const char*, char**, int);
__declspec(dllimport) unsigned long long __cdecl strtoull(const char*, char**, int);
__declspec(dllimport) double __cdecl strtod(const char*, char**);
__declspec(dllimport) float __cdecl strtof(const char*, char**);
__declspec(dllimport) __declspec(noreturn) void __cdecl abort(void);
__declspec(dllimport) __declspec(noreturn) void __cdecl exit(int);
__declspec(dllimport) void __cdecl qsort(void*, size_t, size_t, int(__cdecl*)(const void*, const void*));
__declspec(dllimport) int __cdecl rand(void);
__declspec(dllimport) void __cdecl srand(unsigned);
int __cdecl atexit(void(__cdecl*)(void));
unsigned short __cdecl _byteswap_ushort(unsigned short);
unsigned long __cdecl _byteswap_ulong(unsigned long);
unsigned __int64 __cdecl _byteswap_uint64(unsigned __int64);
unsigned int __cdecl _rotl(unsigned int, int);
unsigned int __cdecl _rotr(unsigned int, int);
unsigned __int64 __cdecl _rotl64(unsigned __int64, int);
unsigned __int64 __cdecl _rotr64(unsigned __int64, int);
int __cdecl abs(int);
long __cdecl labs(long);
long long __cdecl llabs(long long);

#ifdef __cplusplus
}
#endif
