// Ascension-R -- the icon and the GitHub banner, drawn with the precompile screen's own scene and pixel font
// (port/loading_screen.cpp). scripts/brand/brand.py builds and runs this and writes docs/images/ and port/app.ico.
//   brand banner <out.raw> <t_ms> <t_bolt_ms> <bolt_shift>   the scene at t, the title over it (640x360 0x00RRGGBB)
//   brand icon <out.raw> <n>                                 the icon at n x n (16, 32, 48 or 64), at the top left
#include "../../port/loading_screen.cpp"

const char* ps3_settings_path(void) { return ""; }
const char* ps3_settings_str(const char*, const char*, const char*, const char* def) { return def; }
int ps3_settings_bool(const char*, const char*, const char*, int def) { return def; }
int ps3_settings_ieq(const char* a, const char* b) { return _stricmp(a, b) == 0; }

/* The title: the pixel font a dot `s` pixels, a gradient of gold down its rows, a dark outline one dot round it and a
 * shadow two dots under it. */
static void title_text(int x, int y, int s, const char* str)
{
    static const uint32_t rows[7] = { 0xFFF6DA, 0xFFE9A8, 0xFFD680, 0xFFC064, 0xFCA450, 0xF48640, 0xE2683A };
    for (int pass = 0; pass < 3; pass++) {
        int cx = x;
        for (const char* p = str; *p; p++, cx += 6 * s) {
            const uint8_t* g = glyph(*p);
            if (!g) continue;
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++) {
                    if (!(g[r] & (0x10 >> b))) continue;
                    const int px = cx + b * s, py = y + r * s;
                    if (pass == 0) fill(px + s, py + 2 * s, px + 2 * s, py + 3 * s, 0x0E0410);
                    else if (pass == 1) fill(px - s, py - s, px + 2 * s, py + 2 * s, 0x2A0A22);
                    else fill(px, py, px + s, py + s, rows[r]);
                }
        }
    }
}

/* draw_scene's layers, the lightning at a time of the banner's choosing and moved left behind the ruins, clear of the
 * title; then the title. */
static void banner(ULONGLONG t, ULONGLONG t_bolt, float bolt_shift)
{
    const float t_s = (float)t / 1000.0f;
    const float cam = sinf(t_s * 6.2831853f / 45.0f) * 8.0f;
    const int sun_x = 300 + (int)(cam * 0.1f), sun_y = HORIZON - 24;
    scene_sky(t, 0.0f);
    scene_sun(sun_x, sun_y, 44);
    scene_clouds(t_s, cam);
    if (t_bolt) scene_lightning(t_bolt, bolt_shift / 0.15f);
    scene_far(cam, sun_x);
    scene_sea(t, sun_x);
    scene_birds(t);
    scene_cliff(t, cam);
    scene_near(t, cam);
    const char* title = "ASCENSION-R";
    title_text((CW - text_w(title, 5)) / 2, 50, 5, title);
    const char* sub = "STATIC RECOMPILATION";
    text((CW - text_w(sub, 2)) / 2, 102, 2, 0xF8E6BC, 0x1A0814, sub);
}

/* The icon: a temple on a rock in the sea, against the sun going down, under a dark sky. Each size is drawn, not
 * scaled; at 16 the lit edges would eat the temple, so it is drawn there by hand, a dark shape on the sun. */
struct IconLayout {
    int n, horizon, sun_x, sun_y, sun_r, glow;
    int tx0, tx1, apex, ent0, ent1, floor_y, cols, col_w, rock_y, steps;
};
static const IconLayout k_icons[] = {
    /*  n  hor  sx  sy   r glow  tx0 tx1 apex ent0 ent1 floor cols cw rock steps */
    { 32, 22, 16, 15, 11, 3,    7, 25,  12,  16,  18,  23,  4, 2, 25, 2 },
    { 48, 33, 24, 22, 16, 4,   10, 38,  18,  24,  27,  35,  5, 2, 37, 2 },
    { 64, 44, 32, 30, 21, 5,   14, 50,  24,  32,  35,  46,  5, 3, 49, 2 },
};
static const char* const k_icon16[16] = {
    "00*0000000000000",
    "111111YYYY111111",
    "2222YYYYYYYY2222",
    "333yyyyyyyyyy333",
    "44yyyyyTTyyyyy44",
    "55OOOTTTTTTOOO55",
    "6OOTTTTTTTTTTOO6",
    "7oTTTTTTTTTTTTo7",
    "8ooTooTooTooToo8",
    "999T99T99T99T999",
    "99qTqqTqqTqqTq99",
    "hhTTTTTTTTTTTThh",
    "wgeRRRRRRRRRRegw",
    "wwreRRRRRRRRerww",
    "wgwweRRRRRRewwgw",
    "wwwwwRRRRRRwwwww",
};
static const uint32_t k_icon_sky[] = { 0x140A24, 0x1E0E34, 0x2E1444, 0x44184E, 0x601C52, 0x822452,
                                       0xA2304C, 0xC04444, 0xDA603C, 0xEC823C };

static uint32_t icon16_color(char c)
{
    if (c >= '0' && c <= '9') return k_icon_sky[c - '0'];
    switch (c) {
    case 'Y': return 0xFFF8D8; case 'y': return 0xFFE8A0; case 'O': return 0xFFCC6A; case 'o': return 0xFFA84A;
    case 'q': return 0xF8843E; case 'T': return 0x2A1424; case 'h': return 0x5A2448; case 'w': return 0x1E0E2E;
    case 'g': return 0xFFC870; case 'r': return 0xC86A4A; case 'R': return 0x140A16; case 'e': return 0x8A3E38;
    case '*': return 0xFFF4E8;
    }
    return 0xFF00FF;
}

static void icon(int n)
{
    if (n == 16) {
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) put(x, y, icon16_color(k_icon16[y][x]));
        return;
    }
    const IconLayout* L = nullptr;
    for (const IconLayout& l : k_icons)
        if (l.n == n) L = &l;
    if (!L) return;
    const int ns = (int)(sizeof k_icon_sky / sizeof k_icon_sky[0]);
    for (int y = 0; y < L->horizon; y++)
        for (int x = 0; x < n; x++) put(x, y, band(k_icon_sky, ns, powf((float)y / (float)(L->horizon - 1), 1.1f), x, y));
    for (uint32_t i = 0; i < (uint32_t)n / 8; i++) {   /* a few stars in the dark corners */
        const uint32_t h = hash32(i * 2654435761u + 31u);
        const int x = (int)(h % (uint32_t)n), y = (int)((h >> 12) % (uint32_t)(L->horizon / 3));
        if (abs(x - L->sun_x) > L->sun_r) put(x, y, (h >> 24) & 1 ? 0xFFF4E8 : 0xB8A8D8);
    }
    /* the sun's glow, then the sun: bands down it, its lower half cut into stripes */
    const int cx = L->sun_x, cy = L->sun_y, r = L->sun_r;
    for (int y = cy - r - L->glow; y < L->horizon; y++)
        for (int x = cx - r - L->glow; x <= cx + r + L->glow; x++) {
            const float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            if (d <= (float)r || d > (float)(r + L->glow)) continue;
            if (dith(x, y, (int)((1.0f - (d - (float)r) / (float)L->glow) * 10.0f))) put(x, y, rgb_mix(get(x, y), 0xFFD890, 110));
        }
    static const uint32_t disc[] = { 0xFFF8D8, 0xFFE8A0, 0xFFCC6A, 0xFFA84A, 0xF8843E };
    const int period = n >= 48 ? 5 : 4;
    for (int y = cy - r; y < L->horizon; y++) {
        const int dy = y - cy;
        if (dy > 0 && dy % period >= period - std::min(1 + dy * 2 / r, period - 1)) continue;
        const int half = (int)(sqrtf((float)std::max(0, r * r - dy * dy)) + 0.35f);
        for (int x = cx - half; x <= cx + half; x++) put(x, y, band(disc, 5, (float)(dy + r) / (float)(2 * r), x, y));
    }
    /* the sea, and the sun's light on it */
    for (int y = L->horizon; y < n; y++)
        for (int x = 0; x < n; x++) {
            uint32_t c = y == L->horizon ? 0x5A2448 : 0x1E0E2E;
            const int depth = y - L->horizon, reach = std::max(1, r - depth);
            if (depth > 0 && (y & 1) == 0 && abs(x - cx) <= reach && hash32((uint32_t)(x * 7 + y * 131)) % 3u)
                c = abs(x - cx) < reach / 2 ? 0xFFC870 : 0xC86A4A;
            put(x, y, c);
        }
    /* the rock the temple stands on, and its steps */
    for (int x = L->tx0 - 1; x < L->tx1 + 1; x++) {
        const int edge = std::min(abs(x - (L->tx0 - 1)), abs(x - L->tx1));
        const int top = L->rock_y + std::max(0, 2 - edge) + (int)(hash32((uint32_t)x * 977u) % 2u);
        for (int y = top; y < n; y++) put(x, y, y == top ? 0xB0503C : (hash32((uint32_t)(x * 31 + y * 17)) & 7) == 0 ? 0x2E1626 : 0x140A16);
    }
    for (int s = 0; s < L->steps; s++) {
        const int h = n >= 48 ? 2 : 1, y0 = L->rock_y - (s + 1) * h, inset = s * 2;
        fill(L->tx0 + inset, y0, L->tx1 - inset, y0 + h, 0x3A1A2C);
        fill(L->tx0 + inset, y0, L->tx1 - inset, y0 + 1, 0xC87048);
    }
    /* its columns, the sun between them */
    const int span = (L->tx1 - 2) - (L->tx0 + 2);
    for (int k = 0; k < L->cols; k++) {
        const int x0 = L->tx0 + 2 + k * (span - L->col_w) / (L->cols - 1);
        fill(x0, L->ent1, x0 + L->col_w, L->floor_y, 0x2A1424);
        fill(x0, L->ent1, x0 + 1, L->floor_y, 0xE09A5C);
        if (L->col_w >= 3) fill(x0 + L->col_w - 1, L->ent1, x0 + L->col_w, L->floor_y, 0x8A4436);
    }
    /* the beam over them, and the roof's gable */
    fill(L->tx0, L->ent0, L->tx1, L->ent1, 0x4A2430);
    fill(L->tx0, L->ent0, L->tx1, L->ent0 + 1, 0xF0B070);
    const int mid2 = L->tx0 + L->tx1 - 1;   /* twice its middle */
    for (int y = L->apex; y < L->ent0; y++) {
        const int half2 = (y - L->apex) * (L->tx1 - L->tx0 - 1) / (L->ent0 - L->apex);
        const int a = (mid2 - half2) / 2, b = (mid2 + half2 + 1) / 2;
        fill(a, y, b + 1, y + 1, 0x42202E);
        put(a, y, 0xF4B474);
        put(b, y, 0xF4B474);
    }
}

int main(int argc, char** argv)
{
    if (argc >= 6 && !strcmp(argv[1], "banner")) {
        banner((ULONGLONG)atoll(argv[3]), (ULONGLONG)atoll(argv[4]), (float)atof(argv[5]));
    } else if (argc >= 4 && !strcmp(argv[1], "icon")) {
        memset(s_canvas, 0, sizeof s_canvas);
        icon(atoi(argv[3]));
    } else {
        fprintf(stderr, "usage: brand banner <out.raw> <t_ms> <t_bolt_ms> <bolt_shift> | brand icon <out.raw> <n>\n");
        return 1;
    }
    FILE* f = fopen(argv[2], "wb");
    if (!f) return 2;
    fwrite(s_canvas, 4, CW * CH, f);
    fclose(f);
    return 0;
}
