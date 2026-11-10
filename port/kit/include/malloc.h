/* Ascension-R -- the C library's declarations for the game's DLL as the Kit builds it (port/kit/README.md). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

__declspec(dllimport) void* __cdecl malloc(size_t);
__declspec(dllimport) void* __cdecl calloc(size_t, size_t);
__declspec(dllimport) void* __cdecl realloc(void*, size_t);
__declspec(dllimport) void __cdecl free(void*);
__declspec(dllimport) void* __cdecl _aligned_malloc(size_t, size_t);
__declspec(dllimport) void __cdecl _aligned_free(void*);
#define _alloca(n) __builtin_alloca(n)
#define alloca(n) __builtin_alloca(n)

#ifdef __cplusplus
}
#endif
