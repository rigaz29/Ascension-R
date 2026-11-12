/*
 * The precompile gate. Before the game starts, every vertex shader in the game's archives is built into the shader
 * cache, once for each version of this program (precompile.cpp, `--precompile-shaders`): a player never meets a
 * vertex shader the first time in play. It is not up to the player: the game starts when they are built.
 *
 * It runs where the game's window is about to open, in a window of its own laid out as that one will be (windowed at
 * its size, or borderless over the monitor), before the D3D12 window, its swap chain and the shader cache are made: a
 * process of its own builds them (the same exe with --precompile-shaders and this one's settings), and the cache is
 * free for the game again when it ends. That one prints how far it is; this one shows it on an 8-bit screen: a scene
 * drawn here (draw_scene: Greek ruins at sunset, all of it this file's own), a pixel font, a bar of segments. The
 * screen goes when the game's window is up (gowa_loading_screen_close, from the frame clock).
 *
 * Done for this version: <shader cache>.precompiled says "Ascension-R <version>" (precompile.cpp writes it), and a
 * pack of the cache is there. Clear shaders moves it to the Recycle Bin with the cache, so the next start builds them
 * again. If the build fails, the screen says why and the game starts all the same: the next start tries again.
 * GOWA_PRECOMPILE_FIRST=0 (graphics.precompile_first) leaves the gate out, for benches and scripts that bring their
 * own cache.
 *
 * Everything is drawn on a 640x360 canvas, 16:9, scaled up by whole pixels where that fills most of the window.
 * Ascension-R.exe --loading-preview <folder> writes the screen in a few states to <folder> as BMP files, 2x.
 *
 * The scene is code, not a picture: a sky of dithered bands and a striped sun, four layers of drifting clouds, a
 * stroke of lightning now and then, far mountains with ruins on a peak, the sea and the sun on it, birds, a cliff
 * with a temple and two waterfalls, and before it all broken columns and a brazier whose fire flickers and sends up
 * embers. The layers move a little apart as a slow camera sways.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include "ps3emu/ps3_settings.h"
#include "version.h"
#include "loading_screen.h"

/* ---------------------------------------------------------------------------
 * The pixel font: 5x7 in a cell of 6x8, upper case (lower case is drawn as upper)
 * -----------------------------------------------------------------------*/
struct Glyph { char c; uint8_t row[7]; };
static const Glyph k_glyphs[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}}, {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'-', {0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}, {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'\'', {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}}, {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}}, {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
};
static const uint8_t* glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (const Glyph& g : k_glyphs)
        if (g.c == c) return g.row;
    return NULL;   /* a space, or a character the font has not got */
}

/* ---------------------------------------------------------------------------
 * The canvas
 * -----------------------------------------------------------------------*/
enum { CW = 640, CH = 360 };
static_assert(CW == GOWA_SCENE_W && CH == GOWA_SCENE_H, "loading_screen.h gives the launcher this size");
static uint32_t s_canvas[CW * CH];   /* 0x00RRGGBB, as a 32-bit DIB takes it */

static inline uint32_t rgb_mix(uint32_t d, uint32_t s, int a)
{
    const int na = 255 - a;
    const uint32_t r = (((s >> 16) & 255) * a + ((d >> 16) & 255) * na) / 255;
    const uint32_t g = (((s >> 8) & 255) * a + ((d >> 8) & 255) * na) / 255;
    const uint32_t b = ((s & 255) * a + (d & 255) * na) / 255;
    return r << 16 | g << 8 | b;
}

static inline uint32_t rgb_scale(uint32_t c, int k)   /* k/256 */
{
    const uint32_t r = ((c >> 16) & 255) * k >> 8, g = ((c >> 8) & 255) * k >> 8, b = (c & 255) * k >> 8;
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static void fill(int x0, int y0, int x1, int y1, uint32_t c, int a = 255)
{
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, (int)CW); y1 = std::min(y1, (int)CH);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t& p = s_canvas[y * CW + x];
            p = a >= 255 ? c : rgb_mix(p, c, a);
        }
}

static const uint8_t k_bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };

static int text_w(const char* s, int scale)
{
    const int n = (int)strlen(s);
    return n ? (n * 6 - 1) * scale : 0;
}

/* Text at (x, y), its cells `scale` pixels a dot, with a shadow one dot down and right. */
static void text(int x, int y, int scale, uint32_t color, uint32_t shadow, const char* s)
{
    for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
        const int ox = pass ? 0 : scale, oy = pass ? 0 : scale;
        const uint32_t c = pass ? color : shadow;
        int cx = x;
        for (const char* p = s; *p; p++, cx += 6 * scale) {
            const uint8_t* g = glyph(*p);
            if (!g) continue;
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++)
                    if (g[r] & (0x10 >> b)) fill(cx + b * scale + ox, y + r * scale + oy, cx + (b + 1) * scale + ox,
                                                 y + (r + 1) * scale + oy, c);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The scene: Greek ruins at sunset, drawn here
 * -----------------------------------------------------------------------*/
enum { HORIZON = 212, WATERLINE = 250 };   /* the sea's far edge; where the cliff stands in it */

static inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}
static inline float hashf(uint32_t x) { return (float)(hash32(x) & 0xFFFFFF) / 16777216.0f; }

/* Smooth value noise in [0, 1), and three octaves of it. */
static float vnoise(float x, uint32_t seed)
{
    const float fl = floorf(x);
    const int i = (int)fl;
    const float f = x - fl, u = f * f * (3.0f - 2.0f * f);
    const float a = hashf((uint32_t)i * 0x9E3779B1u ^ seed), b = hashf((uint32_t)(i + 1) * 0x9E3779B1u ^ seed);
    return a + (b - a) * u;
}
static float fbm(float x, uint32_t seed)
{
    return vnoise(x, seed) * 0.6f + vnoise(x * 2.13f, seed * 3u + 7u) * 0.28f + vnoise(x * 4.37f, seed * 5u + 13u) * 0.12f;
}

static inline void put(int x, int y, uint32_t c)
{
    if ((unsigned)x < CW && (unsigned)y < CH) s_canvas[y * CW + x] = c;
}
static inline uint32_t get(int x, int y) { return (unsigned)x < CW && (unsigned)y < CH ? s_canvas[y * CW + x] : 0; }
/* The ordered dither at a level of 0 to 16: true on that many pixels of each 16. */
static inline bool dith(int x, int y, int level) { return level > k_bayer[y & 3][x & 3]; }

/* A colour along a palette of n, u from 0 to 1, in bands: each colour solid, dithered into the next over the last
 * half of its band. */
static uint32_t band(const uint32_t* pal, int n, float u, int x, int y)
{
    u = std::min(1.0f, std::max(0.0f, u)) * (float)(n - 1);
    const int i = std::min((int)u, n - 2);
    const float f = u - (float)i;
    return dith(x, y, f < 0.5f ? 0 : (int)((f - 0.5f) * 32.0f)) ? pal[i + 1] : pal[i];
}

static inline int luma(uint32_t c) { return (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 5 + (c & 255) * 2) / 10; }

static void stroke(int x0, int y0, int x1, int y1, uint32_t c, uint32_t glow)
{
    for (int pass = glow ? 0 : 1; pass < 2; pass++) {
        int x = x0, y = y0;
        const int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int e = dx + dy;
        for (;;) {
            if (pass) put(x, y, c);
            else { put(x - 1, y, glow); put(x + 1, y, glow); }
            if (x == x1 && y == y1) break;
            const int e2 = 2 * e;
            if (e2 >= dy) { e += dy; x += sx; }
            if (e2 <= dx) { e += dx; y += sy; }
        }
    }
}

static void scene_sky(ULONGLONG t, float flash)
{
    static const uint32_t pal[] = { 0x140A24, 0x1E0E34, 0x2E1444, 0x44184E, 0x601C52, 0x822452,
                                    0xA2304C, 0xC04444, 0xDA603C, 0xEC823C, 0xF6A64E, 0xFCC874 };
    const int n = (int)(sizeof pal / sizeof pal[0]);
    for (int y = 0; y < HORIZON; y++) {
        const float f = powf((float)y / (HORIZON - 1), 1.25f);
        for (int x = 0; x < CW; x++) {
            uint32_t c = band(pal, n, f, x, y);
            if (flash > 0.0f) c = rgb_mix(c, 0xD8CCFF, (int)(flash * (1.0f - f) * 150.0f));
            s_canvas[y * CW + x] = c;
        }
    }
    /* stars in the dark at the top, each twinkling at its own pace */
    for (uint32_t i = 0; i < 40; i++) {
        const uint32_t h = hash32(i * 2654435761u + 977u);
        const int x = (int)(h % CW), y = (int)((h >> 10) % 78u);
        const int tw = (int)((t / (220 + (h >> 24) * 3) + i) % 7u);
        if (tw == 0) continue;
        const uint32_t c = tw == 3 ? 0xFFF4E8 : y < 40 ? 0xB8A8D8 : 0x9A7AB8;
        put(x, y, c);
        if (tw == 3 && (h & 3) == 0) { put(x - 1, y, 0x7A64A8); put(x + 1, y, 0x7A64A8); put(x, y - 1, 0x7A64A8); put(x, y + 1, 0x7A64A8); }
    }
}

/* The sun going down into the sea, its lower half cut into stripes, in a glow of dithered rings. */
static void scene_sun(int cx, int cy, int r)
{
    for (int y = cy - r - 24; y < HORIZON; y++)
        for (int x = cx - r - 24; x <= cx + r + 24; x++) {
            const float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            if (d <= (float)r || d > (float)r + 24.0f) continue;
            if (dith(x, y, (int)((1.0f - (d - (float)r) / 24.0f) * 10.0f))) put(x, y, rgb_mix(get(x, y), 0xFFD890, 110));
        }
    static const uint32_t disc[] = { 0xFFF8D8, 0xFFE8A0, 0xFFCC6A, 0xFFA84A, 0xF8843E };
    for (int y = cy - r; y < HORIZON; y++) {
        const int dy = y - cy;
        if (dy > 0) {
            const int gap = 1 + dy / 9;
            if (dy % 6 >= 6 - gap) continue;
        }
        const int half = (int)sqrtf((float)std::max(0, r * r - dy * dy));
        for (int x = cx - half; x <= cx + half; x++) put(x, y, band(disc, 5, (float)(dy + r) / (float)(2 * r), x, y));
    }
}

/* Four layers of cloud drifting at their own speeds, lit from below. */
static void scene_clouds(float t_s, float cam)
{
    struct Layer { int y, thick; float freq, speed; uint32_t seed, top, mid, lit; };
    static const Layer layers[] = {
        {  30, 10, 0.012f, 2.0f, 11, 0x2C1442, 0x3E184A, 0x6E2852 },
        {  70, 16, 0.009f, 3.5f, 23, 0x3E1648, 0x5A1E4E, 0xB04446 },
        { 112, 16, 0.011f, 5.5f, 37, 0x561C4C, 0x7A2A4E, 0xE06A3E },
        { 176,  5, 0.015f, 8.0f, 53, 0x7A2A48, 0xA84046, 0xFCB45A },   /* thin, across the sun */
    };
    for (const Layer& l : layers) {
        const float off = t_s * l.speed + cam * 0.3f;
        for (int x = 0; x < CW; x++) {
            const float wx = (float)x + off, n = fbm(wx * l.freq, l.seed);
            if (n < 0.55f) continue;
            /* flat below, heaped above */
            const float h = std::min(1.0f, (n - 0.55f) * 6.0f) * (float)l.thick;
            const float heap = 0.6f + 0.4f * vnoise(wx * 0.09f, l.seed + 99u);
            const int top = l.y - (int)(h * heap), bot = l.y + 1 + (int)(h * 0.15f);
            for (int y = top; y <= bot; y++) {
                if (y == top && !dith(x, y, 8)) continue;   /* a soft edge */
                const float rel = bot > top ? (float)(y - top) / (float)(bot - top) : 1.0f;
                uint32_t c = rel > 0.7f ? l.lit : rel > 0.3f ? l.mid : l.top;
                if (rel > 0.6f && rel <= 0.7f && dith(x, y, 8)) c = l.lit;
                if (rel > 0.2f && rel <= 0.3f && dith(x, y, 8)) c = l.mid;
                put(x, y, c);
            }
        }
    }
}

/* Now and then a stroke of lightning far off, with a fork, the sky lit with it (scene_sky's flash): the first a few
 * seconds in, not as the screen opens. */
static const ULONGLONG LIGHTNING_MS = 9500, LIGHTNING_FIRST_MS = 6000;
static inline ULONGLONG lightning_t(ULONGLONG t) { return t + LIGHTNING_MS - LIGHTNING_FIRST_MS; }
static void scene_lightning(ULONGLONG t, float cam)
{
    const ULONGLONG ph = lightning_t(t) % LIGHTNING_MS;
    if (ph >= 230 || (ph > 60 && ph < 120)) return;   /* on, off, on again */
    const uint32_t seed = (uint32_t)(lightning_t(t) / LIGHTNING_MS) * 2654435761u;
    int x = 140 + (int)(hash32(seed) % 90) + (int)(cam * 0.15f), y = 22, fx = 0, fy = 0;
    const int fork = 3 + (int)(hash32(seed + 1) % 5);
    for (int k = 0; y < HORIZON - 20; k++) {
        const int nx = x + (int)(hash32(seed + 17u * (uint32_t)k + 3u) % 15) - 7, ny = y + 9;
        stroke(x, y, nx, ny, 0xF4F0FF, 0x9A86D0);
        if (k == fork) { fx = nx; fy = ny; }
        x = nx;
        y = ny;
    }
    for (int k = 0; k < 4 && fx; k++) {
        const int nx = fx + 4 + (int)(hash32(seed + 31u * (uint32_t)k) % 8), ny = fy + 8;
        stroke(fx, fy, nx, ny, 0xE8E0FF, 0x7E6CB8);
        fx = nx;
        fy = ny;
    }
}

/* A range of mountains: its top for x, falling away toward the sun, where they open onto the sea. */
static int ridge(int x, float shift, int sun_x, uint32_t seed, float tall)
{
    const float wx = (float)x + shift;
    const float d = std::min(1.0f, fabsf((float)(x - sun_x)) / 160.0f), open = d * d * (3.0f - 2.0f * d);
    return HORIZON - (int)((fbm(wx * 0.0055f, seed) * tall + fbm(wx * 0.031f, seed + 2u) * tall * 0.2f) * open);
}
static void mountains(float shift, int sun_x, uint32_t seed, float tall, uint32_t body, uint32_t rim, uint32_t rim2)
{
    for (int x = 0; x < CW; x++) {
        const int r = ridge(x, shift, sun_x, seed, tall);
        for (int y = r; y < HORIZON; y++) put(x, y, y == r ? rim : (y == r + 1 && dith(x, y, 8)) ? rim2 : body);
    }
}
/* Two ranges, the far one paler in the haze, and ruins on a peak of the near one. */
static void scene_far(float cam, int sun_x)
{
    mountains(cam * 0.08f, sun_x, 151, 76.0f, 0x8A3858, 0xC85E62, 0xA44A5E);
    const float shift = cam * 0.15f;
    mountains(shift, sun_x, 101, 50.0f, 0x4A1C4C, 0x8A3A5A, 0x6A2A54);
    int px = 150, py = HORIZON;
    for (int x = 150; x < 230; x++) {
        const int r = ridge(x, shift, sun_x, 101, 50.0f);
        if (r < py) { py = r; px = x; }
    }
    for (int k = -2; k <= 2; k++) {
        const int cx = px + k * 4;
        for (int y = py - 9; y <= ridge(cx, shift, sun_x, 101, 50.0f); y++) put(cx, y, 0x4A1C4C);
    }
    fill(px - 10, py - 11, px + 11, py - 9, 0x4A1C4C);
    fill(px - 10, py - 11, px + 11, py - 10, 0x8A3A5A);
}

/* The sea: darker as it comes near, small waves catching the sky, the sun on it in dashes that shimmer. */
static void scene_sea(ULONGLONG t, int sun_x)
{
    static const uint32_t pal[] = { 0x7A2C56, 0x5A2452, 0x44204C, 0x321A40, 0x241430, 0x1A0E24 };
    const uint32_t frame = (uint32_t)(t / 150), slow = (uint32_t)(t / 420);
    for (int y = HORIZON; y < CH; y++) {
        const int depth = y - HORIZON;
        const float u = std::min(1.0f, (float)depth / 80.0f);
        for (int x = 0; x < CW; x++) put(x, y, band(pal, 6, u, x, y));
        if (depth % 4 == 2)
            for (int x = 0; x < CW; x += 4)
                if (hash32((uint32_t)x * 131u + (uint32_t)y * 977u + slow) % 19u == 0)
                    fill(x, y, x + 3, y + 1, depth < 20 ? 0xA8486A : 0x7A3462);
        if (depth & 1) continue;
        const int w = 10 + depth * 2;   /* the sun's path, wider as it comes near */
        for (int x = sun_x - w; x <= sun_x + w; x += 4) {
            const uint32_t h = hash32((uint32_t)(x - sun_x + 4096) * 7919u + (uint32_t)y * 104729u + frame * 31u);
            const int edge = abs(x - sun_x) * 16 / (w + 1);
            if ((int)(h % 16u) < 11 - edge * 2 / 3)
                fill(x, y, x + 3, y + 1, depth < 10 ? 0xFFE8B0 : depth < 30 ? 0xFFC870 : 0xF09A50);
        }
    }
}

static void scene_birds(ULONGLONG t)
{
    const float s = (float)t / 1000.0f;
    for (int i = 0; i < 3; i++) {
        const int x = (int)fmodf(s * (14.0f + (float)i * 3.0f) + (float)i * 230.0f, (float)CW + 80.0f) - 40;
        const int y = 116 + i * 13 + (int)(sinf(s * 0.7f + (float)i) * 4.0f);
        const int up = (int)((t / 180 + (ULONGLONG)i) & 1);
        const uint32_t c = 0x22102C;
        put(x, y, c); put(x - 1, y, c); put(x + 1, y, c);
        put(x - 2, y + (up ? -1 : 1), c); put(x + 2, y + (up ? -1 : 1), c);
        put(x - 3, y + (up ? -2 : 1), c); put(x + 3, y + (up ? -2 : 1), c);
    }
}

/* A fire w wide standing on its base row, about h tall, flickering. */
static void flame(int x0, int base, int w, int h, ULONGLONG t, uint32_t seed)
{
    const uint32_t frame = (uint32_t)(t / 90);
    for (int i = 0; i < w; i++) {
        const float centre = 1.0f - fabsf(((float)i + 0.5f) / (float)w * 2.0f - 1.0f);
        const float n = vnoise((float)i * 0.45f + (float)t * 0.011f, seed);
        const int fh = (int)((float)h * (0.35f + 0.65f * centre) * (0.65f + 0.6f * n));
        for (int k = 0; k < fh; k++) {
            const float rel = (float)k / (float)std::max(fh, 1);
            if (rel > 0.7f && (hash32(seed + (uint32_t)i * 977u + (uint32_t)k * 131u + frame * 7u) & 1)) continue;
            const uint32_t c = rel < 0.2f && centre > 0.5f ? 0xFFF4C4 : rel < 0.45f ? 0xFFD058 : rel < 0.72f ? 0xF68A2C : 0xC23A1C;
            put(x0 + i, base - k, c);
        }
    }
}

/* Water falling from y0 into the sea, w wide, lit by the sunset on its left, and spray where it lands. */
static void waterfall(int x0, int w, int y0, ULONGLONG t)
{
    static const uint32_t pal[] = { 0xFFF0E4, 0xF2C8B8, 0xC898AC, 0xE4B4B4 };
    const int flow = (int)(t / 22), y1 = WATERLINE;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x0 + w; x++) {
            const int v = (y - flow + (int)(hash32((uint32_t)x * 2654435761u) % 9u)) & 0x3FFF;
            put(x, y, x == x0 ? 0xFFE0B8 : x == x0 + w - 1 ? 0x7A4A6A : pal[(v / 3) % 4]);
        }
    for (int y = y1 - 14; y < y1 + 5; y++)
        for (int x = x0 - 12; x < x0 + w + 12; x++) {
            const float dx = ((float)x - ((float)x0 + (float)w * 0.5f)) / ((float)w * 0.5f + 12.0f);
            const float dy = (float)(y - (y1 - 2)) / (y < y1 - 2 ? 12.0f : 6.0f), d = dx * dx + dy * dy;
            if (d < 1.0f && dith(x + (int)(t / 120), y, (int)((1.0f - d) * 12.0f))) put(x, y, 0xF0DCDC);
        }
}

/* Light from a fire, in dithered rings that waver with it, on what is dark near it (not on the sky behind). */
static void glow(int cx, int cy, int r, uint32_t c, float strength, ULONGLONG t)
{
    const float flick = 0.85f + 0.15f * vnoise((float)t * 0.008f, 503);
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            const float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy))) / (float)r;
            if (d >= 1.0f) continue;
            const uint32_t p = get(x, y);
            if (luma(p) > 90) continue;
            if (dith(x, y, (int)((1.0f - d) * strength * flick * 16.0f))) put(x, y, rgb_mix(p, c, 110));
        }
}

/* A temple on its steps, x0 to x0 + 150, against the sunset: dark, lit on its edges toward the sun, and inside by
 * the fires at the ends of its floor. */
static void temple(int x0, int base, ULONGLONG t)
{
    const int w = 150;
    for (int s = 0; s < 3; s++) {
        const int y0 = base - 4 * (s + 1), inset = 5 * s;
        fill(x0 + inset, y0, x0 + w - inset, y0 + 4, 0x3A1A2C);
        fill(x0 + inset, y0, x0 + w - inset, y0 + 1, 0xC87048);
        put(x0 + inset, y0 + 1, 0xA85A40);
    }
    const int floor_y = base - 12, col_top = floor_y - 34;
    fill(x0 + 13, col_top, x0 + w - 11, floor_y, 0x12080E);
    for (int y = floor_y - 12; y < floor_y; y++)
        for (int x = x0 + 13; x < x0 + w - 11; x++)
            if (dith(x, y, (y - (floor_y - 12)) * 10 / 12)) put(x, y, 0x5A2018);
    for (int k = 0; k < 8; k++) {
        const int cx = x0 + 13 + k * 17;
        fill(cx, col_top, cx + 7, floor_y, 0x40202E);
        fill(cx, col_top, cx + 1, floor_y, 0xF0A462);
        fill(cx + 1, col_top, cx + 2, floor_y, 0xA85A42);
        fill(cx + 4, col_top, cx + 5, floor_y, 0x341A28);
        fill(cx - 1, col_top - 2, cx + 8, col_top, 0x5A2C36);
        fill(cx - 1, col_top - 2, cx + 8, col_top - 1, 0xE09A5C);
    }
    fill(x0 + 8, col_top - 9, x0 + w - 8, col_top - 2, 0x4A2430);
    fill(x0 + 8, col_top - 9, x0 + w - 8, col_top - 8, 0xF0B070);
    fill(x0 + 8, col_top - 5, x0 + w - 8, col_top - 4, 0x2E1422);
    for (int x = x0 + 12; x < x0 + w - 10; x += 6) fill(x, col_top - 7, x + 2, col_top - 5, 0x6A3438);
    const int py = col_top - 9, apex = py - 20, mid = x0 + w / 2;
    int prev = 0;
    for (int y = apex; y < py; y++) {
        const int half = (y - apex) * (w / 2 - 8) / (py - apex);
        fill(mid - half, y, mid + half + 1, y + 1, 0x42202E);
        fill(mid - half, y, mid - prev + 2, y + 1, 0xF4B474);   /* its slope toward the sun */
        prev = half;
    }
    glow(x0 + 6, floor_y - 6, 22, 0xD0703A, 0.5f, t);
    glow(x0 + w - 6, floor_y - 6, 22, 0xD0703A, 0.5f, t);
    flame(x0 + 3, floor_y - 1, 6, 9, t, 401);
    flame(x0 + w - 9, floor_y - 1, 6, 9, t, 409);
}

/* The cliff on the right, standing in the sea: dark rock in layers, lit on its edges toward the sun, the temple on
 * it, two waterfalls down it, its shadow in the water and the sea breaking at its foot. */
static int cliff_edge(int y, float ox) { return (int)(400.0f + ox - (float)(y - 148) * 0.24f + fbm((float)y * 0.09f, 211) * 14.0f); }
static void scene_cliff(ULONGLONG t, float cam)
{
    const float ox = cam * 0.5f;
    const int top = 148;
    for (int y = top; y < WATERLINE; y++) {
        const int e = cliff_edge(y, ox);
        for (int x = std::max(e, 0); x < CW; x++) {
            const int wx = x - (int)ox;
            const uint32_t h = hash32((uint32_t)(wx >> 1) * 73856093u ^ (uint32_t)(y >> 1) * 19349663u);
            const int layer = (y + (int)(vnoise((float)wx * 0.06f, 223) * 6.0f)) % 12;
            uint32_t c = (h & 7) == 0 ? 0x3C1E30 : (h & 7) == 1 ? 0x221020 : 0x2E1626;
            if (layer == 0) c = 0x180A14;
            else if (layer == 1) c = 0x462434;
            const int in = x - e;
            if (in < 2) c = in == 0 ? 0xC86040 : 0x8A3E38;
            else if (in < 18 && dith(x, y, (18 - in) / 2)) c = rgb_mix(c, 0x8A3E38, 80);
            if (y - top < 2) c = y == top ? 0xE07A44 : 0x9A4638;
            put(x, y, c);
        }
    }
    for (int y = WATERLINE; y < WATERLINE + 30; y++) {
        const int my = std::max(WATERLINE - 1 - (y - WATERLINE) * 3, top);
        const int wob = (int)(sinf((float)y * 1.3f + (float)t * 0.004f) * 1.5f);
        for (int x = std::max(cliff_edge(my, ox) + wob, 0); x < CW; x++)
            if ((y & 1) || dith(x, y, 10)) put(x, y, rgb_mix(get(x, y), 0x100610, 150));
    }
    const int foot = cliff_edge(WATERLINE - 1, ox);
    for (int x = std::max(foot - 6, 0); x < std::min(foot + 96, (int)CW); x++)
        if (dith(x, WATERLINE, 13 - std::max(0, x - foot) / 8) && hash32((uint32_t)x * 31u + (uint32_t)(t / 200)) % 3u)
            put(x, WATERLINE, 0xB890B0);
    temple(456 + (int)ox, top, t);
    waterfall(420 + (int)ox, 9, top + 1, t);
    waterfall(441 + (int)ox, 6, top + 3, t);
}

/* A column from ytop to ybot, w wide, lit on its right; broken: its top ragged. */
static void column(int x0, int w, int ytop, int ybot, int broken, uint32_t seed)
{
    for (int x = x0; x < x0 + w; x++) {
        const int rx = x - x0;
        const int top = broken ? ytop + (int)(hash32(seed + (uint32_t)rx / 2u) % 9u) : ytop;
        for (int y = top; y < ybot; y++) {
            uint32_t c = rx % 5 == 0 ? 0x140A14 : 0x1C0E1A;
            if (rx >= w - 5) c = rx >= w - 2 ? 0xC2643E : 0x8A4436;
            if (y == top && broken) c = 0x9A4A3A;
            put(x, y, c);
        }
    }
}

static void embers(int cx, int base, ULONGLONG t)
{
    for (uint32_t i = 0; i < 14; i++) {
        const float life = 2600.0f + (float)(hash32(611u + i) % 1400u);
        const float ph = fmodf((float)t + (float)(hash32(631u + i) % 4000u), life) / life;
        const int y = base - (int)(ph * 110.0f);
        const int x = cx + (int)(sinf(ph * 9.0f + (float)i) * (2.0f + ph * 10.0f)) + (int)(hash32(641u + i) % 9u) - 4;
        if (ph > 0.85f && (hash32((uint32_t)(t / 80) + i) & 1)) continue;
        put(x, y, ph < 0.4f ? 0xFFD070 : ph < 0.75f ? 0xF68A30 : 0xA83A1C);
    }
}

/* Before it all on the left: a ledge, a column under a broken beam, a broken column, a fallen drum, a brazier. */
static int ledge_top(int wx)
{
    int top = 204 + wx * 24 / 230 + (int)(fbm((float)wx * 0.07f, 307) * 6.0f);
    if (wx > 196) top += (wx - 196) * (wx - 196) / 6;   /* where it breaks off */
    return top;
}
static void scene_near(ULONGLONG t, float cam)
{
    const int ox = (int)cam;
    for (int x = 0; x < CW; x++) {
        const int wx = x - ox, top = ledge_top(wx);
        if (top >= CH) break;
        for (int y = top; y < CH; y++) {
            const uint32_t h = hash32((uint32_t)(wx >> 1) * 83492791u ^ (uint32_t)(y >> 1) * 2971215073u);
            put(x, y, y == top ? 0xB0503C : y == top + 1 ? 0x5A2630 : (h & 7) == 0 ? 0x24101E : 0x140A16);
        }
    }
    static const int tufts[] = { 6, 20, 66, 108, 140, 200 };   /* grass, in the wind */
    for (int wx : tufts) {
        const int base = ledge_top(wx);
        for (int b = 0; b < 4; b++) {
            const int gx = wx + ox + b * 2 - 3, tall = 3 + (int)(hash32((uint32_t)(wx * 4 + b)) % 4u);
            const int sway = (int)(sinf((float)t * 0.0021f + (float)(wx + b)) * 1.3f);
            stroke(gx, base, gx + sway, base - tall, 0x1C0E1A, 0);
            put(gx + sway, base - tall, 0xA04A38);
        }
    }
    column(34 + ox, 24, 100, ledge_top(46) + 1, 0, 0);
    fill(30 + ox, 94, 62 + ox, 100, 0x22101C);          /* its capital */
    fill(30 + ox, 94, 62 + ox, 95, 0xA85A3A);
    fill(58 + ox, 94, 62 + ox, 100, 0xC2643E);
    fill(-6 + ox, 82, 66 + ox, 94, 0x1A0C18);           /* the beam it still holds, broken off at the left */
    fill(-6 + ox, 82, 66 + ox, 83, 0xB0603C);
    fill(60 + ox, 82, 66 + ox, 94, 0x9A4A38);
    column(116 + ox, 20, 176, ledge_top(126) + 1, 1, 77);
    {   /* a fallen drum of a column, lying on the ledge, its end toward the sun */
        const int y1 = ledge_top(166) + 1, y0 = y1 - 13, x0 = 144 + ox, x1 = 184 + ox;
        for (int y = y0; y < y1; y++) {
            const int k = y - y0;
            fill(x0, y, x1, y + 1, k == 0 ? 0xA85A3C : k % 3 == 2 ? 0x140A14 : 0x1E0E1A);
        }
        for (int y = y0; y < y1; y++) {
            const float v = ((float)(y - y0) + 0.5f) / 13.0f * 2.0f - 1.0f;
            const int half = (int)(3.5f * sqrtf(std::max(0.0f, 1.0f - v * v)) + 0.5f);
            fill(x1 - half, y, x1 + half + 1, y + 1, 0x7A3C34);
            put(x1 + half, y, 0xC2643E);
        }
    }
    /* the brazier: three legs, a bowl, its fire, its light, its embers */
    const int bx = 92 + ox, bowl = ledge_top(92) - 24;
    stroke(bx, bowl + 8, bx - 9, ledge_top(83) + 1, 0x2A1418, 0);
    stroke(bx, bowl + 8, bx + 9, ledge_top(101) + 1, 0x2A1418, 0);
    stroke(bx, bowl + 8, bx, ledge_top(92) + 1, 0x2A1418, 0);
    for (int y = bowl; y < bowl + 8; y++) {
        const int half = 13 - (y - bowl);
        fill(bx - half, y, bx + half + 1, y + 1, y == bowl ? 0xE07A3C : 0x2A1418);
    }
    glow(bx, bowl - 8, 72, 0xD0703A, 0.55f, t);
    flame(bx - 11, bowl - 1, 23, 22, t, 331);
    embers(bx, bowl - 6, t);
}

static void draw_scene(ULONGLONG t)
{
    const float t_s = (float)t / 1000.0f;
    const float cam = sinf(t_s * 6.2831853f / 45.0f) * 8.0f;
    const ULONGLONG lp = lightning_t(t) % LIGHTNING_MS;
    const float flash = lp < 60 ? 1.0f : lp >= 120 && lp < 200 ? 0.6f : 0.0f;
    const int sun_x = 300 + (int)(cam * 0.1f), sun_y = HORIZON - 24;
    scene_sky(t, flash);
    scene_sun(sun_x, sun_y, 44);
    scene_clouds(t_s, cam);
    scene_lightning(t, cam);
    scene_far(cam, sun_x);
    scene_sea(t, sun_x);
    scene_birds(t);
    scene_cliff(t, cam);
    scene_near(t, cam);
}

/* The scene alone, for the launcher's header (launcher.cpp). The launcher has closed before the gate opens this screen,
 * so the two never draw on the canvas at once. */
const uint32_t* gowa_scene_frame(uint64_t t_ms)
{
    draw_scene((ULONGLONG)t_ms);
    return s_canvas;
}

const uint8_t* gowa_pixel_glyph(char c) { return glyph(c); }

/* ---------------------------------------------------------------------------
 * What the build says
 * -----------------------------------------------------------------------*/
enum Phase { PH_START, PH_READ, PH_BUILD, PH_DONE, PH_ERROR };
struct Progress {
    Phase phase = PH_START;
    unsigned a = 0, n = 0;          /* reading: archive a of n */
    char name[64] = "";
    unsigned found = 0;              /* programs found */
    unsigned done = 0, total = 0;    /* shaders built of total */
    char why[160] = "";
    ULONGLONG t_build = 0;           /* when the build started (ms) */
};
static CRITICAL_SECTION s_lock;
static Progress s_prog;

/* ---------------------------------------------------------------------------
 * A frame
 * -----------------------------------------------------------------------*/
/* The lower part darker, in dithered steps, so the panel reads over the scene. */
static void draw_shade(void)
{
    const int top = 196;
    for (int y = top; y < CH; y++) {
        const double u = (double)(y - top) / (CH - top);
        const int level = (int)(u * u * 7.0 * 16.0);   /* in 16ths of a step, 7 steps */
        for (int x = 0; x < CW; x++) {
            const int step = level / 16 + ((level & 15) > k_bayer[y & 3][x & 3] ? 1 : 0);
            uint32_t& p = s_canvas[y * CW + x];
            p = rgb_scale(p, 256 - step * 22);
        }
    }
}

static void draw_frame_box(int x0, int y0, int x1, int y1)
{
    fill(x0, y0, x1, y1, 0x120A08, 214);
    /* a dark line, a gold one and a dark one inside, the corners cut by a pixel */
    fill(x0 + 1, y0, x1 - 1, y0 + 1, 0x050302);
    fill(x0 + 1, y1 - 1, x1 - 1, y1, 0x050302);
    fill(x0, y0 + 1, x0 + 1, y1 - 1, 0x050302);
    fill(x1 - 1, y0 + 1, x1, y1 - 1, 0x050302);
    fill(x0 + 2, y0 + 1, x1 - 2, y0 + 3, 0xD8A23A);
    fill(x0 + 2, y1 - 3, x1 - 2, y1 - 1, 0x9A6A24);
    fill(x0 + 1, y0 + 2, x0 + 3, y1 - 2, 0xD8A23A);
    fill(x1 - 3, y0 + 2, x1 - 1, y1 - 2, 0x9A6A24);
    fill(x0 + 3, y0 + 3, x1 - 3, y0 + 4, 0x4A2E12);
    fill(x0 + 3, y0 + 3, x0 + 4, y1 - 3, 0x4A2E12);
}

static uint32_t seg_color(int i, int n)
{
    /* red to orange to gold along the bar */
    static const uint32_t stop[3] = { 0xA8201A, 0xE8622A, 0xF6C64A };
    const double u = n > 1 ? (double)i / (n - 1) : 1.0;
    const int k = u < 0.5 ? 0 : 1;
    const double v = u < 0.5 ? u * 2.0 : (u - 0.5) * 2.0;
    const uint32_t c0 = stop[k], c1 = stop[k + 1];
    return rgb_mix(c0, c1, (int)(v * 255.0));
}

static void draw_bar(int x0, int y0, int x1, int y1, double frac, int blink)
{
    fill(x0, y0, x1, y1, 0x050302);
    fill(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 0xD8A23A);
    fill(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 0x1C100A);
    const int n = 28, gap = 2, iw = (x1 - x0) - 6, sw = (iw - (n - 1) * gap) / n;
    const int sx = x0 + 3 + (iw - (n * sw + (n - 1) * gap)) / 2, top = y0 + 3, bot = y1 - 3;
    const int lit = (int)(frac * n + 1e-9);
    for (int i = 0; i < n; i++) {
        const int a = sx + i * (sw + gap), b = a + sw;
        if (i < lit) {
            const uint32_t c = seg_color(i, n);
            fill(a, top, b, bot, c);
            fill(a, top, b, top + 1, rgb_scale(c, 330));   /* lit from above */
            fill(a, bot - 1, b, bot, rgb_scale(c, 150));
        } else if (i == lit && blink && frac < 1.0) {
            fill(a, top, b, bot, rgb_scale(seg_color(i, n), 140));
        } else {
            fill(a, top, b, bot, 0x2E1A10);
            fill(a, bot - 1, b, bot, 0x1A0E08);
        }
    }
}

static void upper(char* s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

static void compose(ULONGLONG t, const Progress& p)
{
    draw_scene(t);
    draw_shade();
    const int px0 = 24, py0 = 272, px1 = CW - 24, py1 = 352;
    draw_frame_box(px0, py0, px1, py1);

    const uint32_t gold = 0xF6D27A, shadow = 0x2A160C, cream = 0xF0E2C8, muted = 0xB8A688, white = 0xFFF2DA;
    const int blink = (t / 330) & 1;
    char title[64], pct[16] = "", left[96] = "", right[48] = "", foot[96];
    double frac = 0.0;
    uint32_t title_c = gold;
    snprintf(foot, sizeof foot, "Once for each version. The game starts by itself.");
    switch (p.phase) {
    case PH_START:
        snprintf(title, sizeof title, "Getting ready");
        break;
    case PH_READ:
        snprintf(title, sizeof title, "Reading the game's files");
        frac = p.n ? (double)(p.a ? p.a - 1 : 0) / p.n : 0.0;
        snprintf(left, sizeof left, "Archive %u of %u: %.40s", p.a, p.n, p.name);
        break;
    case PH_BUILD: {
        snprintf(title, sizeof title, "Building shaders");
        frac = p.total ? (double)p.done / p.total : 0.0;
        if (p.total) snprintf(left, sizeof left, "%u of %u shaders", p.done, p.total);
        else snprintf(left, sizeof left, "%u vertex programs found", p.found);
        const double ms = (double)(GetTickCount64() - p.t_build);
        if (p.done >= 50 && p.done < p.total && ms > 3000.0) {
            const double left_s = ms / p.done * (p.total - p.done) / 1000.0;
            if (left_s < 60.0) snprintf(right, sizeof right, "Less than a minute left");
            else snprintf(right, sizeof right, "About %d min left", (int)(left_s / 60.0 + 0.999));
        }
        break;
    }
    case PH_DONE:
        snprintf(title, sizeof title, "Shaders ready");
        frac = 1.0;
        snprintf(left, sizeof left, "Starting the game%s", blink ? "..." : "");
        break;
    case PH_ERROR:
        snprintf(title, sizeof title, "Shaders not built");
        title_c = 0xFF6A4A;
        frac = p.total ? (double)p.done / p.total : 0.0;
        snprintf(left, sizeof left, "%.90s", p.why[0] ? p.why : "The build stopped");
        snprintf(foot, sizeof foot, "The game starts all the same. The next start tries again.");
        break;
    }
    if (p.phase == PH_READ || p.phase == PH_BUILD || p.phase == PH_DONE)
        snprintf(pct, sizeof pct, "%d%%", (int)(frac * 100.0 + 1e-9));
    upper(title); upper(left); upper(right); upper(foot);

    text(px0 + 12, py0 + 10, 2, title_c, shadow, title);
    if (pct[0]) text(px1 - 12 - text_w(pct, 2), py0 + 10, 2, white, shadow, pct);
    draw_bar(px0 + 10, py0 + 30, px1 - 10, py0 + 46, frac, blink);
    text(px0 + 12, py0 + 52, 1, cream, shadow, left);
    if (right[0]) text(px1 - 12 - text_w(right, 1), py0 + 52, 1, cream, shadow, right);
    text(px0 + 12, py0 + 64, 1, muted, shadow, foot);
    char ver[48];
    snprintf(ver, sizeof ver, "Ascension-R %s", AR_VERSION);
    upper(ver);
    text(px1 - 12 - text_w(ver, 1), py0 + 64, 1, muted, shadow, ver);
}

/* ---------------------------------------------------------------------------
 * The window
 * -----------------------------------------------------------------------*/
static HWND s_hwnd;
static HANDLE s_ui_thread, s_ui_ready;
static volatile LONG s_user_closed, s_close_now;
static HANDLE s_child;
static ULONGLONG s_t0;

static void blit(HDC dc)
{
    RECT rc;
    GetClientRect(s_hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    /* whole pixels where that fills most of the window, else the largest fit */
    int dw, dh;
    const int s = std::min(w / CW, h / CH);
    if (s >= 1 && (CW * s * 10 >= w * 9 || CH * s * 10 >= h * 9)) { dw = CW * s; dh = CH * s; }
    else if ((double)w / h > (double)CW / CH) { dh = h; dw = h * CW / CH; }
    else { dw = w; dh = w * CH / CW; }
    const int dx = (w - dw) / 2, dy = (h - dh) / 2;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT r;
    if (dy > 0) { SetRect(&r, 0, 0, w, dy); FillRect(dc, &r, black); SetRect(&r, 0, dy + dh, w, h); FillRect(dc, &r, black); }
    if (dx > 0) { SetRect(&r, 0, dy, dx, dy + dh); FillRect(dc, &r, black); SetRect(&r, dx + dw, dy, w, dy + dh); FillRect(dc, &r, black); }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = CW;
    bi.bmiHeader.biHeight = -CH;   /* top down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);   /* nearest: the pixels stay square */
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, CW, CH, s_canvas, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_TIMER: {
        Progress p;
        EnterCriticalSection(&s_lock);
        p = s_prog;
        LeaveCriticalSection(&s_lock);
        compose(GetTickCount64() - s_t0, p);
        HDC dc = GetDC(h);
        blit(dc);
        ReleaseDC(h, dc);
        if (s_close_now) DestroyWindow(h);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        blit(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE: {
        /* While it builds, closing it is quitting the game; once done, the game's own window is coming. */
        EnterCriticalSection(&s_lock);
        const Phase ph = s_prog.phase;
        LeaveCriticalSection(&s_lock);
        if (ph == PH_DONE || ph == PH_ERROR) { DestroyWindow(h); return 0; }
        InterlockedExchange(&s_user_closed, 1);
        if (s_child) TerminateProcess(s_child, 1);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* The window as the game's will be (rsx_d3d12_backend.c: d3d12_create_window, d3d12_set_borderless). */
static DWORD WINAPI ui_main(LPVOID)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));   /* the program's (app.rc) */
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"ascension_r_loading";
    RegisterClassExW(&wc);
    UINT dpi = 96;
    {
        typedef UINT(WINAPI * get_dpi_fn)(void);
        HMODULE user = GetModuleHandleA("user32.dll");
        get_dpi_fn get_dpi = user ? (get_dpi_fn)(void*)GetProcAddress(user, "GetDpiForSystem") : NULL;
        if (get_dpi) dpi = get_dpi();
        if (dpi < 96) dpi = 96;
    }
    int cw = 1280 * (int)dpi / 96, ch = 720 * (int)dpi / 96;
    const char* ws = ps3_settings_str("display", "window_size", NULL, NULL);
    if (ws && *ws) {
        unsigned a = 0, c = 0;
        char x = 0;
        if (sscanf(ws, "%u%c%u", &a, &x, &c) == 3 && (x == 'x' || x == 'X') && a >= 320 && c >= 180 && a <= 16384 &&
            c <= 16384) { cw = (int)a; ch = (int)c; }
    }
    const char* title = getenv("PS3_TITLE");
    wchar_t wtitle[128];
    swprintf(wtitle, 128, L"%hs", title && *title ? title : "Ascension-R");
    const int borderless = ps3_settings_ieq(ps3_settings_str("display", "mode", NULL, "windowed"), "borderless");
    if (borderless) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        POINT o = {0, 0};
        GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
        s_hwnd = CreateWindowExW(0, wc.lpszClassName, wtitle, WS_POPUP | WS_VISIBLE, mi.rcMonitor.left, mi.rcMonitor.top,
                                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, NULL, NULL,
                                 wc.hInstance, NULL);
    } else {
        RECT wr = {0, 0, cw, ch};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        s_hwnd = CreateWindowExW(0, wc.lpszClassName, wtitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                 wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, wc.hInstance, NULL);
    }
    SetEvent(s_ui_ready);
    if (!s_hwnd) return 1;
    SetForegroundWindow(s_hwnd);
    SetTimer(s_hwnd, 1, 33, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    s_hwnd = NULL;
    return 0;
}

/* ---------------------------------------------------------------------------
 * The gate
 * -----------------------------------------------------------------------*/
static std::string cache_path(void)
{
    const char* sc = ps3_settings_str("graphics", "shader_cache", "D3D12_SHADER_CACHE", NULL);
    if (!sc || !*sc || ps3_settings_ieq(sc, "off")) return std::string();
    return sc;
}

static int file_exists(const std::string& p)
{
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Built for this version: the marker says so, and a pack of the cache is there to hold them. */
static int marker_current(const std::string& cache)
{
    if (!file_exists(cache) && !file_exists(cache + ".dxil")) return 0;
    FILE* f = fopen((cache + ".precompiled").c_str(), "rb");
    if (!f) return 0;
    char line[128] = "";
    const int got = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!got) return 0;
    line[strcspn(line, "\r\n")] = 0;
    char want[64];
    snprintf(want, sizeof want, "Ascension-R %s", AR_VERSION);
    return !strcmp(line, want);
}

static void parse_line(const char* l)
{
    if (strncmp(l, "precompile: ", 12)) return;
    l += 12;
    unsigned a = 0, b = 0, c = 0;
    char name[64] = "";
    EnterCriticalSection(&s_lock);
    if (sscanf(l, "read %u %u %63s", &a, &b, name) >= 2) {
        s_prog.phase = PH_READ;
        s_prog.a = a;
        s_prog.n = b;
        snprintf(s_prog.name, sizeof s_prog.name, "%s", name);
    } else if (sscanf(l, "found %u", &a) == 1) {
        s_prog.phase = PH_BUILD;
        s_prog.found = a;
        s_prog.t_build = GetTickCount64();
    } else if (sscanf(l, "compile %u %u", &a, &b) == 2) {
        if (s_prog.phase != PH_BUILD) { s_prog.phase = PH_BUILD; s_prog.t_build = GetTickCount64(); }
        s_prog.done = a;
        s_prog.total = b;
    } else if (sscanf(l, "done %u %u %u", &a, &b, &c) == 3) {
        s_prog.phase = PH_DONE;
        s_prog.done = s_prog.total = s_prog.total ? s_prog.total : a + b;
    } else if (!strncmp(l, "error ", 6)) {
        snprintf(s_prog.why, sizeof s_prog.why, "%s", l + 6);
    }
    LeaveCriticalSection(&s_lock);
}

static void set_phase(Phase ph, const char* why)
{
    EnterCriticalSection(&s_lock);
    s_prog.phase = ph;
    if (why && !s_prog.why[0]) snprintf(s_prog.why, sizeof s_prog.why, "%s", why);
    LeaveCriticalSection(&s_lock);
}

/* The build, as a process of its own with this one's settings, its progress on a pipe. NULL if it did not start. */
static HANDLE start_build(HANDLE* out)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return NULL;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --precompile-shaders";
    const char* sp = ps3_settings_path();
    wchar_t buf[2200];
    if (sp && *sp) {
        swprintf(buf, 2200, L" --settings \"%hs\"", sp);
        cmd += buf;
    }
    /* What this run takes from its command line too: the cache and the game it builds for, the GPU it picks. */
    static const struct { const char* sec; const char* key; } keep[] = {
        { "graphics", "shader_cache" }, { "paths", "elf" }, { "graphics", "gpu" } };
    for (const auto& k : keep) {
        const char* v = ps3_settings_str(k.sec, k.key, NULL, NULL);
        if (v && *v && !strchr(v, '"')) {
            swprintf(buf, 2200, L" --set \"%hs.%hs=%hs\"", k.sec, k.key, v);
            cmd += buf;
        }
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE r = NULL, w = NULL;
    if (!CreatePipe(&r, &w, &sa, 0)) return NULL;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError = w;
    si.hStdInput = NULL;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    SetEnvironmentVariableA("GOWA_PRECOMPILE_FOREGROUND", "1");   /* the player is waiting: not below normal */
    const BOOL ok = CreateProcessW(exe, line.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("GOWA_PRECOMPILE_FOREGROUND", NULL);
    CloseHandle(w);
    if (!ok) { CloseHandle(r); return NULL; }
    CloseHandle(pi.hThread);
    *out = r;
    return pi.hProcess;
}

int gowa_precompile_gate(void)
{
    if (!ps3_settings_bool("graphics", "precompile_first", "GOWA_PRECOMPILE_FIRST", 1)) return 0;
    const std::string cache = cache_path();
    if (cache.empty() || marker_current(cache)) return 0;
    printf("[precompile] the vertex shaders are not built for %s yet: building them first\n", AR_VERSION);
    InitializeCriticalSection(&s_lock);
    s_t0 = GetTickCount64();
    s_ui_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_ui_thread = CreateThread(NULL, 0, ui_main, NULL, 0, NULL);
    if (s_ui_thread) WaitForSingleObject(s_ui_ready, 10000);

    HANDLE pipe = NULL;
    s_child = start_build(&pipe);
    if (s_child && s_user_closed) TerminateProcess(s_child, 1);   /* closed before it started */
    if (!s_child) {
        set_phase(PH_ERROR, "Could not start the build");
    } else {
        std::string acc;
        char chunk[512];
        DWORD got = 0;
        while (ReadFile(pipe, chunk, sizeof chunk, &got, NULL) && got) {
            acc.append(chunk, got);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string l = acc.substr(0, nl);
                if (!l.empty() && l.back() == '\r') l.pop_back();
                parse_line(l.c_str());
                acc.erase(0, nl + 1);
            }
        }
        CloseHandle(pipe);
        WaitForSingleObject(s_child, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(s_child, &code);
        CloseHandle(s_child);
        s_child = NULL;
        if (s_user_closed) {
            printf("[precompile] the window was closed while it built: quitting\n");
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 0);
        }
        EnterCriticalSection(&s_lock);
        const int done = s_prog.phase == PH_DONE && code == 0;
        LeaveCriticalSection(&s_lock);
        if (!done) set_phase(PH_ERROR, "The build stopped");
    }
    EnterCriticalSection(&s_lock);
    const Phase ph = s_prog.phase;
    char why[160];
    snprintf(why, sizeof why, "%s", s_prog.why);
    LeaveCriticalSection(&s_lock);
    if (ph == PH_ERROR) {
        printf("[precompile] the vertex shaders were not built: %s. The game starts all the same\n", why);
        Sleep(6000);   /* long enough to read it */
    } else {
        printf("[precompile] the vertex shaders are built for %s (%.0f s)\n", AR_VERSION,
               (GetTickCount64() - s_t0) / 1000.0);
    }
    if (!s_ui_thread || !s_hwnd) gowa_loading_screen_close();
    return 0;
}

void gowa_loading_screen_close(void)
{
    InterlockedExchange(&s_close_now, 1);   /* the next tick destroys it, on its own thread */
}

/* ---------------------------------------------------------------------------
 * --loading-preview <folder>: the screen in a few states, as BMP files at 2x
 * -----------------------------------------------------------------------*/
static int write_bmp2x(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const int w = CW * 2, h = CH * 2, row = w * 3, size = 54 + row * h;
    uint8_t hd[54] = { 'B', 'M' };
    memcpy(hd + 2, &size, 4);
    const int off = 54, dib = 40, planes = 1, bpp = 24;
    memcpy(hd + 10, &off, 4);
    memcpy(hd + 14, &dib, 4);
    memcpy(hd + 18, &w, 4);
    memcpy(hd + 22, &h, 4);
    memcpy(hd + 26, &planes, 2);
    memcpy(hd + 28, &bpp, 2);
    fwrite(hd, 1, 54, f);
    std::vector<uint8_t> line((size_t)row);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const uint32_t c = s_canvas[(y / 2) * CW + x / 2];
            line[(size_t)x * 3] = (uint8_t)c;
            line[(size_t)x * 3 + 1] = (uint8_t)(c >> 8);
            line[(size_t)x * 3 + 2] = (uint8_t)(c >> 16);
        }
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
    return 1;
}

int gowa_loading_preview(const char* dir)
{
    struct Shot { const char* name; ULONGLONG t; Progress p; };
    std::vector<Shot> shots;
    Progress p;
    p.phase = PH_READ; p.a = 12; p.n = 48; snprintf(p.name, sizeof p.name, "STAT.PSARC");
    shots.push_back({ "1_reading", 3000, p });
    p = Progress(); p.phase = PH_BUILD; p.found = 3455; p.done = 2418; p.total = 6910;
    shots.push_back({ "2_building", 21500, p });
    p.done = 5200;
    shots.push_back({ "3_building_lightning", LIGHTNING_FIRST_MS + 2 * LIGHTNING_MS + 30, p });
    p = Progress(); p.phase = PH_DONE; p.done = p.total = 6910;
    shots.push_back({ "4_done", 33000, p });
    p = Progress(); p.phase = PH_ERROR; p.done = 1200; p.total = 6910;
    snprintf(p.why, sizeof p.why, "the game has the shader cache open");
    shots.push_back({ "5_error", 5000, p });
    for (Shot& s : shots) {
        if (s.p.phase == PH_BUILD) s.p.t_build = GetTickCount64() - 40000;
        compose(s.t, s.p);
        char path[1200];
        snprintf(path, sizeof path, "%s\\%s.bmp", dir, s.name);
        printf("loading preview: %s %s\n", path, write_bmp2x(path) ? "written" : "NOT written");
    }
    return 0;
}
