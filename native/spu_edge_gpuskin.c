/*
 * EDGE's skinning in the vertex pass: the CPU side.
 *
 * A skinned job's vertexes, weights and indexes do not change, only the palette and the transforms
 * do. So its inputs are kept (spu_edge_memo.c), the job hands over the palette and the transforms'
 * constants, and the vertex pass works the vertex out (spu_edge_gpulight.c has the shader; this file
 * is the same arithmetic on the CPU, for the check and the backend's fallback).
 *
 * Per vertex, from 0xE550 (spu_edge_skin.c, spu_edge_skinmat.c), 0xF538 and 0xBBC8:
 *  - the rows R0 R1 R2 = sum over the four bones of weight * the matrix's rows, weight = byte / 255;
 *  - the position: (x y z w) * (R0.xyz 1) + (y z x w) * (R1.xyz 0) + (z x y w) * (R2.xyz 0)
 *    + (R0.w R1.w R2.w 0);
 *  - the normal and the tangent the same, without the last term, and scaled to length 1, w kept, if
 *    0xE550 ends by normalizing them.
 * The packing is the output piece's (spu_edge_cmp.c): the position as it is, the normal I16N, the
 * tangent X11Y11Z10.
 *
 * Exact against the SPU but for the length: under its MXCSR the position is the job's, bit for bit
 * (GOWA_EDGE_SKIN=2 in the fight: 565 million vertexes), the tangent at most a unit off. 0xBBC8 scales by
 * an estimate and this by 1 / sqrt, so the normal is a few units of 32767 off. The vertex pass works
 * the rows and the position out the same way, the SPU's rounding done in integer operations
 * (spu_edge_gpulight.c): the game's depth pass and lit pass must land on the same depth, and either
 * may be skinned there or on the SPU. Its normal and tangent keep the GPU's rounding.
 *
 * Only what the game was seen to do. A job that goes on to 0xF538 stays on the SPU (spu_edge_memo.c):
 * no skinned job in the gate or the fight does.
 */
#include "spu_edge_gpuskin.h"
#include <immintrin.h>
#include <math.h>
#include <string.h>

static inline float ek_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t ek_u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline uint32_t ek_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void ek_wr32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void ek_wr16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* the local store's quadword as host words */
static inline __m128i ek_q(const uint8_t* p)
{
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)p),
                            _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}

/* (a.z a.z a.z b.w) for the word w of b: the matrix's last column goes to a row's lane 3 */
#define EK_ROW(a, b, c, q3, w) \
    _mm_blend_epi32(_mm_blend_epi32(_mm_blend_epi32((a), (b), 0x2), (c), 0x4), _mm_shuffle_epi32((q3), (w) * 0x55), 0x8)

void ek_rows(ek_rec* r, const uint8_t* m, uint32_t nmat)
{
    r->nmat = nmat;
    for (uint32_t i = 0; i < nmat; i++, m += 64) {
        const __m128i q0 = ek_q(m), q1 = ek_q(m + 16), q2 = ek_q(m + 32), q3 = ek_q(m + 48);
        _mm_storeu_si128((__m128i*)r->rows[i][0], EK_ROW(q0, q1, q2, q3, 0));
        _mm_storeu_si128((__m128i*)r->rows[i][1], EK_ROW(q1, q2, q0, q3, 1));
        _mm_storeu_si128((__m128i*)r->rows[i][2], EK_ROW(q2, q0, q1, q3, 2));
    }
}

void ek_take(uint8_t* in, const uint8_t* p, const uint8_t* n, const uint8_t* t, const uint8_t* bones)
{
    _mm_storeu_si128((__m128i*)in, ek_q(p));
    if (n) {
        _mm_storeu_si128((__m128i*)(in + 16), ek_q(n));
        _mm_storeu_si128((__m128i*)(in + 32), ek_q(t));
        memcpy(in + 48, bones, 8);
    } else {
        memcpy(in + 16, bones, 8);
    }
}

void ek_give(const uint8_t* in, uint8_t* p, uint8_t* n, uint8_t* t)
{
    const __m128i bs = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    _mm_storeu_si128((__m128i*)p, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)in), bs));
    if (n) {
        _mm_storeu_si128((__m128i*)n, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(in + 16)), bs));
        _mm_storeu_si128((__m128i*)t, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(in + 32)), bs));
    }
}

static void ek_blend(const ek_rec* r, const uint8_t* b, float R[3][4])
{
    const float inv = ek_f(0x3B808080u);
    for (int k = 0; k < 4; k++) {
        const float w = (float)b[2 * k] * inv;
        const uint32_t i = b[2 * k + 1] < r->nmat ? b[2 * k + 1] : 0;
        for (int row = 0; row < 3; row++)
            for (int l = 0; l < 4; l++)
                R[row][l] = k ? fmaf(w, r->rows[i][row][l], R[row][l]) : w * r->rows[i][row][l];
    }
}

/* v by the blended rows, into o */
static void ek_rot(const float R[3][4], const float v[4], float o[4])
{
    o[0] = fmaf(v[2], R[2][0], fmaf(v[1], R[1][0], v[0] * R[0][0]));
    o[1] = fmaf(v[0], R[2][1], fmaf(v[2], R[1][1], v[1] * R[0][1]));
    o[2] = fmaf(v[1], R[2][2], fmaf(v[0], R[1][2], v[2] * R[0][2]));
    o[3] = v[3];
}

static void ek_norm(float v[4])
{
    const float L = fmaf(v[2], v[2], fmaf(v[1], v[1], v[0] * v[0]));
    const float s = 1.0f / sqrtf(L);
    v[0] *= s; v[1] *= s; v[2] *= s;
}

static void ek_position(const float R[3][4], const float* p, float P[3])
{
    P[0] = fmaf(p[2], R[2][0], fmaf(p[1], R[1][0], fmaf(p[0], R[0][0], R[0][3])));
    P[1] = fmaf(p[0], R[2][1], fmaf(p[2], R[1][1], fmaf(p[1], R[0][1], R[1][3])));
    P[2] = fmaf(p[1], R[2][2], fmaf(p[0], R[1][2], fmaf(p[2], R[0][2], R[2][3])));
}

void ek_vertex_pos(const ek_rec* r, const uint8_t* in, uint8_t* out)
{
    float R[3][4], P[3], p[4];
    memcpy(p, in, 16);
    ek_blend(r, in + 16, R);
    ek_position(R, p, P);
    for (int l = 0; l < 3; l++) ek_wr32(out + 4 * l, ek_u(P[l]));
}

void ek_vertex_lit(const ek_rec* r, const uint8_t* in, uint8_t* out)
{
    float R[3][4], P[3], p[4], n[4], t[4], N[4], T[4];
    memcpy(p, in, 16); memcpy(n, in + 16, 16); memcpy(t, in + 32, 16);
    ek_blend(r, in + 48, R);
    ek_position(R, p, P);
    ek_rot(R, n, N);
    ek_rot(R, t, T);
    if (r->flags & EK_PN) ek_norm(N);
    if (r->flags & EK_PT) ek_norm(T);
    for (int l = 0; l < 3; l++) ek_wr32(out + 4 * l, ek_u(P[l]));
    for (int l = 0; l < 4; l++) {
        const float x = fmaf(N[l], ek_f(0x46FFFF00u), ek_f(0xBF000000u));
        ek_wr16(out + 12 + 2 * l, (uint32_t)(int32_t)(x * 65536.0f) >> 16);
    }
    const float tx = fmaf(T[0], ek_f(0x3F7FE000u), ek_f(0xBA000000u));
    const float ty = fmaf(T[1], ek_f(0x3F7FE000u), ek_f(0xBA000000u));
    const float tz = fmaf(T[2], ek_f(0x3F7FC000u), ek_f(0xBA800000u));
    const uint32_t ix = (uint32_t)(int32_t)(tx * 2147483648.0f), iy = (uint32_t)(int32_t)(ty * 2147483648.0f);
    const uint32_t iz = (uint32_t)(int32_t)(tz * 2147483648.0f);
    ek_wr32(out + 20, ((ix >> 21) & 0x7FFu) | (((iy >> 21) & 0x7FFu) << 11) | (((iz >> 22) & 0x3FFu) << 22));
}
