/*
 * The precompile gate. Before the game starts, every vertex shader in the game's archives is built into the shader
 * cache, once for each version of this program (precompile.cpp, `--precompile-shaders`): a player never meets a
 * vertex shader the first time in play. It is not up to the player: the game starts when they are built.
 *
 * It runs where the game's window is about to open, in a window of its own laid out as that one will be (windowed at
 * its size, or borderless over the monitor), before the D3D12 window, its swap chain and the shader cache are made: a
 * process of its own builds them (the same exe with --precompile-shaders and this one's settings), and the cache is
 * free for the game again when it ends. That one prints how far it is; this one shows it on an 8-bit screen: the
 * pictures of port/art behind (panning when wider than the screen, one after the other through an ordered dither),
 * a pixel font, a bar of segments. The screen goes when the game's window is up (gowa_loading_screen_close, from the
 * frame clock).
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
 * The pictures stay out of git like every picture (check_repo.py): port/art/loading1.png, loading2.png (1080x360,
 * panned) and loading3.png (640x360), indexed PNG, embedded by the build that has them (CMakeLists.txt makes
 * loading_art.rc). A tree without them draws the screen on a plain ground. Made from the full-size pictures with
 *   ffmpeg -i <in> -filter_complex "[0:v]scale=<W>:360:flags=area,split[a][b];[a]palettegen=max_colors=128:\
 *   stats_mode=full[p];[b][p]paletteuse=dither=none" -pix_fmt pal8 <out>.png
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
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

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

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
 * The pictures (port/art, in app.rc as RCDATA 201-203)
 * -----------------------------------------------------------------------*/
struct Art { int w = 0, h = 0; std::vector<uint32_t> px; };
static Art s_art[3];
static int s_art_n;

static bool art_load(int id, Art& a)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const void* data = g ? LockResource(g) : NULL;
    const DWORD size = r ? SizeofResource(NULL, r) : 0;
    if (!data || !size) return false;
    IWICImagingFactory* f = NULL;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream* s = NULL;
    IWICBitmapDecoder* d = NULL;
    IWICBitmapFrameDecode* fr = NULL;
    IWICFormatConverter* cv = NULL;
    bool ok = false;
    if (SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromMemory((BYTE*)data, size)) &&
        SUCCEEDED(f->CreateDecoderFromStream(s, NULL, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                                 WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        cv->GetSize(&w, &h);
        if (w >= CW && h == CH) {
            a.w = (int)w;
            a.h = (int)h;
            a.px.resize((size_t)w * h);
            ok = SUCCEEDED(cv->CopyPixels(NULL, w * 4, w * h * 4, (BYTE*)a.px.data()));
        }
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (d) d->Release();
    if (s) s->Release();
    f->Release();
    return ok;
}

static void art_load_all(void)
{
    static const int ids[3] = { 203, 201, 202 };   /* the one that fits first, then the two to pan */
    s_art_n = 0;
    for (int id : ids)
        if (art_load(id, s_art[s_art_n])) s_art_n++;
}

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
static const int SHOW_MS = 14000, FADE_MS = 1200;

/* x of the picture's view: from its left side to its right over the time it is shown, eased. */
static int art_pan(const Art& a, int t)
{
    if (a.w <= CW) return 0;
    const double u = std::min(1.0, std::max(0.0, (double)t / SHOW_MS));
    return (int)((a.w - CW) * (0.5 - 0.5 * cos(3.14159265358979 * u)));
}

static void draw_background(ULONGLONG t)
{
    if (!s_art_n) {
        fill(0, 0, CW, CH, 0x1A0E0A);
        return;
    }
    const int i = (int)((t / SHOW_MS) % (ULONGLONG)s_art_n), j = (i + 1) % s_art_n;
    const int local = (int)(t % SHOW_MS);
    const Art& a = s_art[i];
    const Art& b = s_art[j];
    const int ax = art_pan(a, local), bx = art_pan(b, 0);
    /* the dissolve: the next picture comes in through a 4x4 ordered dither on 2x2 blocks */
    const int f16 = s_art_n > 1 && local > SHOW_MS - FADE_MS ? (local - (SHOW_MS - FADE_MS)) * 17 / FADE_MS : 0;
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            const int next = f16 > k_bayer[(y >> 1) & 3][(x >> 1) & 3];
            s_canvas[y * CW + x] = (next ? b.px[(size_t)y * b.w + x + bx] : a.px[(size_t)y * a.w + x + ax]) & 0xFFFFFF;
        }
}

/* The lower part darker, in dithered steps, so the panel reads over any picture. */
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
    draw_background(t);
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
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    art_load_all();
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
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
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    art_load_all();
    printf("loading preview: %d pictures\n", s_art_n);
    struct Shot { const char* name; ULONGLONG t; Progress p; };
    std::vector<Shot> shots;
    Progress p;
    p.phase = PH_READ; p.a = 12; p.n = 48; snprintf(p.name, sizeof p.name, "STAT.PSARC");
    shots.push_back({ "1_reading", 3000, p });
    p = Progress(); p.phase = PH_BUILD; p.found = 3455; p.done = 2418; p.total = 6910;
    shots.push_back({ "2_building", 19000, p });
    p.done = 5200;
    shots.push_back({ "3_building_dissolve", 27400, p });
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
