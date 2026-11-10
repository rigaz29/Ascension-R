/* Ascension-R -- the C library's declarations for the game's DLL as the Kit builds it (port/kit/README.md). */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define M_PI 3.14159265358979323846
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

#define GOWA_MATH(n) __declspec(dllimport) double __cdecl n(double); __declspec(dllimport) float __cdecl n##f(float);
GOWA_MATH(sqrt) GOWA_MATH(fabs) GOWA_MATH(floor) GOWA_MATH(ceil) GOWA_MATH(trunc) GOWA_MATH(round) GOWA_MATH(rint)
GOWA_MATH(nearbyint) GOWA_MATH(exp) GOWA_MATH(exp2) GOWA_MATH(log) GOWA_MATH(log2) GOWA_MATH(log10) GOWA_MATH(sin)
GOWA_MATH(cos) GOWA_MATH(tan) GOWA_MATH(asin) GOWA_MATH(acos) GOWA_MATH(atan)
#undef GOWA_MATH
#define GOWA_MATH2(n) __declspec(dllimport) double __cdecl n(double, double); __declspec(dllimport) float __cdecl n##f(float, float);
GOWA_MATH2(pow) GOWA_MATH2(fmod) GOWA_MATH2(fmin) GOWA_MATH2(fmax) GOWA_MATH2(copysign) GOWA_MATH2(atan2)
#undef GOWA_MATH2
__declspec(dllimport) double __cdecl fma(double, double, double);
__declspec(dllimport) float __cdecl fmaf(float, float, float);
__declspec(dllimport) double __cdecl ldexp(double, int);
/* Not an export of the 64-bit Universal C Runtime: its header makes it of ldexp, as here. */
__inline float __cdecl ldexpf(float x, int e)
{
    return (float)ldexp(x, e);
}
__declspec(dllimport) double __cdecl frexp(double, int*);
__declspec(dllimport) long __cdecl lrint(double);
__declspec(dllimport) long __cdecl lrintf(float);

#ifdef __cplusplus
}
#endif
