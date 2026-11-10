/* Ascension-R -- the C library's declarations for the game's DLL as the Kit builds it (port/kit/README.md). */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

void* __cdecl memcpy(void*, const void*, size_t);
void* __cdecl memmove(void*, const void*, size_t);
void* __cdecl memset(void*, int, size_t);
int __cdecl memcmp(const void*, const void*, size_t);
__declspec(dllimport) const void* __cdecl memchr(const void*, int, size_t);
size_t __cdecl strlen(const char*);
int __cdecl strcmp(const char*, const char*);
char* __cdecl strcpy(char*, const char*);
char* __cdecl strcat(char*, const char*);
__declspec(dllimport) int __cdecl strncmp(const char*, const char*, size_t);
__declspec(dllimport) char* __cdecl strncpy(char*, const char*, size_t);
__declspec(dllimport) const char* __cdecl strchr(const char*, int);
__declspec(dllimport) const char* __cdecl strrchr(const char*, int);
__declspec(dllimport) const char* __cdecl strstr(const char*, const char*);
__declspec(dllimport) int __cdecl _stricmp(const char*, const char*);
__declspec(dllimport) int __cdecl _strnicmp(const char*, const char*, size_t);

#ifdef __cplusplus
}
#endif
