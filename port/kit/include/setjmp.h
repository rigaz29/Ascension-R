/* Ascension-R -- the C library's declarations for the game's DLL as the Kit builds it (port/kit/README.md). clang's
 * intrin.h names jmp_buf; nothing in the DLL jumps with it. */
#pragma once

typedef struct __declspec(align(16)) { unsigned __int64 Part[2]; } SETJMP_FLOAT128;
typedef SETJMP_FLOAT128 jmp_buf[16];
