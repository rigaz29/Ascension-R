/*
 * The host's vector instructions for the native SPU routines.
 *
 * On x86-64 every routine has its AVX2 and FMA3 form, and the few SSE helpers beside them. On arm64 (an Apple
 * Silicon Mac) a routine's AVX2 form is left out (#if defined(__AVX2__) && defined(__FMA__)) and its stand-in says
 * "not handled", so the lifted SPU code does that work instead: slower, the same result. What the routines use
 * outside those blocks has a plain form here or in the routine (NATIVE_SIMD_X86).
 */
#ifndef GOWA_SPU_NATIVE_SIMD_H
#define GOWA_SPU_NATIVE_SIMD_H

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  include <immintrin.h>
#  define NATIVE_SIMD_X86 1
#else
#  define NATIVE_SIMD_X86 0
#endif

#endif
