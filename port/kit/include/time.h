/* Ascension-R -- the C library's declarations for the game's DLL as the Kit builds it (port/kit/README.md). */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef long clock_t;
#define CLOCKS_PER_SEC ((clock_t)1000)
__declspec(dllimport) clock_t __cdecl clock(void);

#ifdef __cplusplus
}
#endif
