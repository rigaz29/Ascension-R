/*
 * gowa-recomp -- the launcher
 *
 * The settings window shown before the game starts (carried over from the God
 * of War HD port). It reads and writes gowa.ini through ps3_settings, so what it
 * shows is what the game will use: Play saves and starts, Save saves, Defaults
 * puts this port's defaults back in the window (saved on Save or Play), Quit
 * leaves without starting. launcher.show = off skips it; holding Shift while
 * the game starts brings it back.
 *
 * This file also holds this port's defaults (gowa_port_defaults), so the
 * game and the Defaults button take them from the same tables.
 */
#include "win32_compat.h"
#include "ps3emu/ps3_settings.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <commctrl.h>   /* SetWindowSubclass: the keyboard window's buttons */
#include <shellapi.h>   /* ShellExecuteA: Open save folder */
#pragma comment(lib, "comctl32.lib")
extern "C" const char* ps3_pad_key_name(int vk);   /* libs/input/cellPad.c */
#endif

extern "C" void gowa_port_defaults(void);
extern "C" int  gowa_launcher(void);   /* 1 = play, 0 = quit */
/* patches.cpp: the title patches this build has (scripts/title_patches.json). */
extern "C" int  gowa_title_patch_count(void);
extern "C" const char* gowa_title_patch_id(int i);
extern "C" const char* gowa_title_patch_name(int i);

/* ---------------------------------------------------------------------------
 * What the window offers
 * -----------------------------------------------------------------------*/
struct Opt {
    int group;                 /* 0 Display, 1 Graphics, 2 Input, 3 System */
    const char* section;
    const char* key;
    const char* def;           /* this port's default */
    const char* label;         /* the row's label, or the checkbox's text */
    int check;                 /* a checkbox: values[0] is off, values[1] on */
    const char* const* values;
    const char* const* names;
    int n;
    HWND ctl;
    char extra[64];            /* a value from the file that the list lacks */
};

static const char* const k_onoff[] = { "off", "on" };
static const char* const k_mode[] = { "windowed", "borderless" };
static const char* const k_mode_n[] = { "Windowed", "Borderless fullscreen" };
static const char* const k_size[] = { "", "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160" };
static const char* const k_size_n[] = { "Default", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160" };
static const char* const k_fps[] = { "off", "title", "overlay" };
static const char* const k_fps_n[] = { "Off", "In the title bar", "On screen" };
static const char* const k_aniso[] = { "game", "2", "4", "8", "16" };
static const char* const k_aniso_n[] = { "As the game sets it", "2x", "4x", "8x", "16x" };
static const char* const k_async[] = { "0", "2" };
static const char* const k_cap[] = { "off", "20", "30" };
static const char* const k_cap_n[] = { "Off", "20 fps, even pace", "30 fps, even pace" };
static const char* const k_scale[] = { "bilinear", "sharp", "fsr1" };
static const char* const k_scale_n[] = { "Bilinear", "Sharp bilinear", "AMD FSR 1" };
static const char* const k_aa[] = { "off", "smaa" };
static const char* const k_aa_n[] = { "Off", "SMAA 1x" };
static const char* const k_sharp[] = { "off", "low", "medium", "high", "max" };
static const char* const k_sharp_n[] = { "Off", "Low", "Medium", "High (AMD's)", "Max" };
static const char* const k_dev[] = { "auto", "pad", "keyboard" };
static const char* const k_dev_n[] = { "Controller, else keyboard", "Controller only", "Keyboard only" };
static const char* const k_dz[] = { "0.10", "0.15", "0.20", "0.24", "0.30", "0.35", "0.40" };
static const char* const k_dz_n[] = { "10%", "15%", "20%", "24% (XInput's)", "30%", "35%", "40%" };
static const char* const k_enter[] = { "cross", "circle" };
static const char* const k_enter_n[] = { "Cross", "Circle" };
static const char* const k_bak[] = { "0", "3", "5", "10" };
static const char* const k_bak_n[] = { "Off", "Keep 3", "Keep 5", "Keep 10" };
static const char* const k_lang[] = { "auto", "en", "en-gb", "fr", "es", "de", "it", "nl", "pt", "pt-br",
                                      "ru", "ja", "ko", "zh-t", "zh-s", "fi", "sv", "da", "no", "pl", "tr" };
static const char* const k_lang_n[] = { "Windows language", "English (US)", "English (UK)", "French", "Spanish",
                                        "German", "Italian", "Dutch", "Portuguese", "Portuguese (Brazil)",
                                        "Russian", "Japanese", "Korean", "Chinese (traditional)",
                                        "Chinese (simplified)", "Finnish", "Swedish", "Danish", "Norwegian",
                                        "Polish", "Turkish" };

#define LIST(v, n) v, n, (int)(sizeof v / sizeof v[0])
static Opt s_opt[] = {
    { 0, "display", "mode", "windowed", "Window", 0, LIST(k_mode, k_mode_n) },
    { 0, "display", "window_size", "", "Window size", 0, LIST(k_size, k_size_n) },
    { 0, "display", "vsync", "on", "Wait for the display's refresh (VSync)", 1, k_onoff, NULL, 2 },
    { 0, "display", "frame_cap", "off", "Frame rate cap", 0, LIST(k_cap, k_cap_n) },
    { 0, "display", "show_fps", "title", "Frame rate", 0, LIST(k_fps, k_fps_n) },
    { 1, "graphics", "scaling", "bilinear", "Scaling", 0, LIST(k_scale, k_scale_n) },
    { 1, "graphics", "sharpening", "high", "FSR 1 sharpening", 0, LIST(k_sharp, k_sharp_n) },
    { 1, "graphics", "antialiasing", "off", "Anti-aliasing", 0, LIST(k_aa, k_aa_n) },
    { 1, "graphics", "anisotropy", "game", "Anisotropic filtering", 0, LIST(k_aniso, k_aniso_n) },
    { 1, "graphics", "async_shaders", "2", "Build new shaders in the background", 1, k_async, NULL, 2 },
    { 2, "input", "device", "auto", "Device", 0, LIST(k_dev, k_dev_n) },
    { 2, "input", "deadzone", "0.24", "Stick dead zone", 0, LIST(k_dz, k_dz_n) },
    { 2, "input", "enter_button", "cross", "Confirm button", 0, LIST(k_enter, k_enter_n) },
    { 2, "input", "vibration", "on", "Vibration", 1, k_onoff, NULL, 2 },
    { 3, "system", "language", "auto", "Language", 0, LIST(k_lang, k_lang_n) },
    { 3, "system", "esc_quits", "off", "Esc closes the game", 1, k_onoff, NULL, 2 },
    { 3, "system", "save_backups", "5", "Save backups", 0, LIST(k_bak, k_bak_n) },
    { 3, "launcher", "show", "on", "Show this window at start (else hold Shift)", 1, k_onoff, NULL, 2 },
};
#undef LIST
enum { N_OPT = sizeof s_opt / sizeof s_opt[0] };

/* The Game patches group: a checkbox per patch, [patches] <id> = on/off, all
 * off by default. Built from the patches the build has, so a patch added to
 * scripts/title_patches.json shows up here with the next lift. */
enum { MAX_POPT = 32 };
static Opt s_popt[MAX_POPT];
static int s_npopt;

static void popt_init(void)
{
    s_npopt = 0;
    const int n = gowa_title_patch_count();
    for (int i = 0; i < n && s_npopt < MAX_POPT; i++) {
        Opt& o = s_popt[s_npopt++];
        o = Opt();
        o.group = 4; o.section = "patches"; o.key = gowa_title_patch_id(i); o.def = "off";
        o.label = gowa_title_patch_name(i); o.check = 1; o.values = k_onoff; o.n = 2;
    }
}

/* The keyboard: [keyboard] keys, their labels, this port's layout. */
static const char* const k_kb[][3] = {
    { "up", "D-pad up", "1" },             { "lstick_up", "Left stick up", "W" },
    { "down", "D-pad down", "3" },         { "lstick_down", "Left stick down", "S" },
    { "left", "D-pad left", "4" },         { "lstick_left", "Left stick left", "A" },
    { "right", "D-pad right", "2" },       { "lstick_right", "Left stick right", "D" },
    { "cross", "Cross", "Space" },         { "rstick_up", "Right stick up", "Up" },
    { "circle", "Circle", "L, F" },        { "rstick_down", "Right stick down", "Down" },
    { "square", "Square", "J, MouseLeft" }, { "rstick_left", "Right stick left", "Left" },
    { "triangle", "Triangle", "K, MouseRight" }, { "rstick_right", "Right stick right", "Right" },
    { "l1", "L1", "LShift" },              { "r1", "R1", "E" },
    { "l2", "L2", "Q" },                   { "r2", "R2", "R" },
    { "l3", "L3", "C" },                   { "r3", "R3", "V" },
    { "start", "Start", "Enter" },         { "select", "Select", "Tab" },
};
enum { N_KB = sizeof k_kb / sizeof k_kb[0] };

extern "C" void gowa_port_defaults(void)
{
    /* Esc is not a PS3 button; closing the game is the window's job. */
    ps3_settings_default("system", "esc_quits", "off");
    /* Before a save is written over, the copy it replaces is kept in
     * savedata_backup next to the saves (PS3_SAVEDATA_BACKUPS). */
    ps3_settings_default("system", "save_backups", "5");
    /* A keyboard layout to start from: the left stick on WASD, evade (the
     * right stick) on the arrows, the D-pad on 1-4. */
    for (int i = 0; i < N_KB; i++)
        ps3_settings_default("keyboard", k_kb[i][0], k_kb[i][2]);
}

/* ---------------------------------------------------------------------------
 * The window
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
enum { ID_PLAY = 100, ID_SAVE, ID_DEFAULTS, ID_QUIT, ID_KEYS, ID_SAVES,
       ID_KOK = 150, ID_KCANCEL, ID_KDEFAULTS, ID_KEY = 200 };

static int   s_dpi = 96;
static HFONT s_font;
static int   s_done;          /* 1 play, 2 quit */
static HWND  s_main, s_keys;
static char  s_kb[N_KB][64];  /* the keyboard window's bindings while it is open */
static HWND  s_kb_btn[N_KB];
static int   s_capture = -1;  /* the binding waiting for a key, or -1 */

static int S(int v) { return MulDiv(v, s_dpi, 96); }

static HWND add(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static int opt_index(const Opt& o, const char* v)
{
    for (int i = 0; i < o.n; i++)
        if (ps3_settings_ieq(o.values[i], v)) return i;
    return -1;
}

/* Put a value into an option's control. */
static void opt_show(Opt& o, const char* v)
{
    if (o.check) {
        /* An on/off box takes what ps3_settings_bool takes for on, so that a
         * "1" written by hand is not shown off and then saved so. */
        const int on = ps3_settings_ieq(v, o.values[1]) ||
                       (o.values == k_onoff && (ps3_settings_ieq(v, "1") || ps3_settings_ieq(v, "true") ||
                                                ps3_settings_ieq(v, "yes")));
        SendMessageA(o.ctl, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        return;
    }
    int i = opt_index(o, v);
    if (i < 0) {   /* a value the list lacks: keep it as an entry of its own */
        snprintf(o.extra, sizeof o.extra, "%s", v);
        if (SendMessageA(o.ctl, CB_GETCOUNT, 0, 0) == o.n) SendMessageA(o.ctl, CB_ADDSTRING, 0, (LPARAM)o.extra);
        else {
            SendMessageA(o.ctl, CB_DELETESTRING, o.n, 0);
            SendMessageA(o.ctl, CB_ADDSTRING, 0, (LPARAM)o.extra);
        }
        i = o.n;
    }
    SendMessageA(o.ctl, CB_SETCURSEL, i, 0);
}

static const char* opt_value(const Opt& o)
{
    if (o.check) return o.values[SendMessageA(o.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0];
    const int i = (int)SendMessageA(o.ctl, CB_GETCURSEL, 0, 0);
    return i >= 0 && i < o.n ? o.values[i] : o.extra;
}

static int save_all(void)
{
    for (int i = 0; i < N_OPT; i++) ps3_settings_set(s_opt[i].section, s_opt[i].key, opt_value(s_opt[i]));
    for (int i = 0; i < s_npopt; i++) ps3_settings_set(s_popt[i].section, s_popt[i].key, opt_value(s_popt[i]));
    if (ps3_settings_save(NULL) != 0) {
        char m[1200];
        snprintf(m, sizeof m, "Could not write %s.", ps3_settings_path());
        MessageBoxA(s_main, m, "Settings", MB_OK | MB_ICONWARNING);
        return 0;
    }
    return 1;
}

/* --- the keyboard window -------------------------------------------------- */
static void kb_label(int i)
{
    SetWindowTextA(s_kb_btn[i], s_capture == i ? "Press a key or a mouse button..." : (s_kb[i][0] ? s_kb[i] : "(none)"));
}

static void kb_set(int i, const char* name)
{
    snprintf(s_kb[i], sizeof s_kb[i], "%s", name ? name : "");
    s_capture = -1;
    kb_label(i);
}

/* A key pressed while a binding waits for one. Esc gives up, Backspace and
 * Delete clear it, Alt is the window's own. */
static void kb_key(WPARAM vk, LPARAM lp)
{
    const int i = s_capture;
    if (vk == VK_ESCAPE) { s_capture = -1; kb_label(i); return; }
    if (vk == VK_BACK || vk == VK_DELETE) { kb_set(i, ""); return; }
    if (vk == VK_MENU) return;
    if (vk == VK_SHIFT) vk = MapVirtualKeyA((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) vk = (lp & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
    const char* name = ps3_pad_key_name((int)vk);
    if (name) kb_set(i, name);
    else MessageBeep(MB_OK);
}

static LRESULT CALLBACK kb_btn_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR idx)
{
    const int i = (int)idx;
    if (s_capture == i) {
        switch (m) {
        case WM_LBUTTONDOWN: kb_set(i, "MouseLeft"); return 0;
        case WM_RBUTTONDOWN: kb_set(i, "MouseRight"); return 0;
        case WM_MBUTTONDOWN: kb_set(i, "MouseMiddle"); return 0;
        case WM_XBUTTONDOWN: kb_set(i, HIWORD(w) == XBUTTON1 ? "Mouse4" : "Mouse5"); return TRUE;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: return 0;
        }
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK kb_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND: {
        const int id = LOWORD(w);
        if (id >= ID_KEY && id < ID_KEY + N_KB) {
            const int prev = s_capture;
            s_capture = id - ID_KEY;
            if (prev >= 0 && prev != s_capture) kb_label(prev);
            kb_label(s_capture);
            SetFocus(s_kb_btn[s_capture]);
        } else if (id == ID_KDEFAULTS) {
            for (int i = 0; i < N_KB; i++) { s_capture = -1; kb_set(i, k_kb[i][2]); }
        } else if (id == ID_KOK || id == ID_KCANCEL) {
            if (id == ID_KOK)
                for (int i = 0; i < N_KB; i++) ps3_settings_set("keyboard", k_kb[i][0], s_kb[i]);
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        s_capture = -1;
        s_keys = NULL;
        EnableWindow(s_main, TRUE);
        SetForegroundWindow(s_main);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void open_keys(void)
{
    RECT r = { 0, 0, S(580), S(470) };
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    s_keys = CreateWindowExA(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, "gowa_keys", "Keyboard keys",
                             WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, s_main, NULL, GetModuleHandleA(NULL), NULL);
    if (!s_keys) return;
    add(s_keys, "STATIC", "Click an action, then press a key or a mouse button. Backspace clears it, "
        "Esc gives up. Edit gowa.ini to give an action more than one key.", 0, 12, 10, 556, 34, -1);
    for (int i = 0; i < N_KB; i++) {
        const int col = i % 2, row = i / 2, x = 12 + col * 286, y = 52 + row * 30;
        add(s_keys, "STATIC", k_kb[i][1], SS_CENTERIMAGE, x, y, 110, 24, -1);
        s_kb_btn[i] = add(s_keys, "BUTTON", "", WS_TABSTOP | BS_PUSHBUTTON, x + 112, y, 160, 24, ID_KEY + i);
        SetWindowSubclass(s_kb_btn[i], kb_btn_proc, 1, (DWORD_PTR)i);
        snprintf(s_kb[i], sizeof s_kb[i], "%s", ps3_settings_str("keyboard", k_kb[i][0], NULL, k_kb[i][2]));
        kb_label(i);
    }
    add(s_keys, "BUTTON", "Defaults", WS_TABSTOP | BS_PUSHBUTTON, 12, 432, 90, 28, ID_KDEFAULTS);
    add(s_keys, "BUTTON", "OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 382, 432, 90, 28, ID_KOK);
    add(s_keys, "BUTTON", "Cancel", WS_TABSTOP | BS_PUSHBUTTON, 478, 432, 90, 28, ID_KCANCEL);
    EnableWindow(s_main, FALSE);
    ShowWindow(s_keys, SW_SHOW);
}

/* Open the folder the game keeps its saves in -- /dev_hdd0/home/00000001/
 * savedata under [paths] hdd0 -- in Explorer, creating it if no save exists
 * yet. */
static void open_saves(void)
{
    const char* hdd0 = ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL);
    if (!hdd0 || !*hdd0) {
        MessageBoxA(s_main, "No [paths] hdd0 in the settings file; run gowa.py build.",
                    "Open save folder", MB_OK | MB_ICONINFORMATION);
        return;
    }
    char path[1100];
    snprintf(path, sizeof path, "%s\\home\\00000001\\savedata", hdd0);
    for (char* p = path; *p; p++) {
        if (*p == '/') *p = '\\';
        if (*p == '\\' && p > path && p[-1] != ':') { *p = 0; CreateDirectoryA(path, NULL); *p = '\\'; }
    }
    CreateDirectoryA(path, NULL);
    ShellExecuteA(s_main, "open", path, NULL, NULL, SW_SHOWNORMAL);
}

/* --- the main window ------------------------------------------------------ */
static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_PLAY:     if (save_all()) s_done = 1; return 0;
        case ID_SAVE:     save_all(); return 0;
        case ID_QUIT:     s_done = 2; return 0;
        case ID_KEYS:     if (!s_keys) open_keys(); return 0;
        case ID_SAVES:    open_saves(); return 0;
        case ID_DEFAULTS:
            for (int i = 0; i < N_OPT; i++) opt_show(s_opt[i], s_opt[i].def);
            for (int i = 0; i < s_npopt; i++) opt_show(s_popt[i], s_popt[i].def);
            return 0;
        }
        break;
    case WM_CLOSE:
        s_done = 2;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

extern "C" int gowa_launcher(void)
{
    {
        typedef UINT (WINAPI *get_dpi_fn)(void);
        HMODULE user = GetModuleHandleA("user32.dll");
        get_dpi_fn get_dpi = user ? (get_dpi_fn)(void*)GetProcAddress(user, "GetDpiForSystem") : NULL;
        s_dpi = get_dpi ? (int)get_dpi() : 96;
        if (s_dpi < 96) s_dpi = 96;
    }
    NONCLIENTMETRICSA ncm;
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) s_font = CreateFontIndirectA(&ncm.lfMessageFont);
    if (!s_font) s_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof wc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = "gowa_launcher";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = kb_proc;
    wc.lpszClassName = "gowa_keys";
    RegisterClassExA(&wc);

    popt_init();
    const int prows = (s_npopt + 1) / 2, pgh = s_npopt ? 26 + prows * 30 : 0;
    /* 450: Display's five rows and Graphics' five end at y 378, the buttons under them. */
    RECT r = { 0, 0, S(660), S(450 + (pgh ? pgh + 8 : 0)) };
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    s_main = CreateWindowExA(WS_EX_CONTROLPARENT, "gowa_launcher", "God of War: Ascension (GowARecomp)",
                             WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!s_main) return 1;   /* no window: just play */

    /* Two columns of groups: Display and Graphics, then Input and System. */
    static const char* const groups[] = { "Display", "Graphics", "Input", "System" };
    int y_next[2] = { 10, 10 };
    for (int g = 0; g < 4; g++) {
        const int col = g / 2, x = 12 + col * 324;
        int rows = g == 2;   /* Input also has the keyboard button */
        for (int i = 0; i < N_OPT; i++) rows += s_opt[i].group == g;
        const int y0 = y_next[col], gh = 26 + rows * 30;
        add(s_main, "BUTTON", groups[g], BS_GROUPBOX, x, y0, 312, gh, -1);
        int y = y0 + 22;
        for (int i = 0; i < N_OPT; i++) {
            Opt& o = s_opt[i];
            if (o.group != g) continue;
            if (o.check) {
                o.ctl = add(s_main, "BUTTON", o.label, WS_TABSTOP | BS_AUTOCHECKBOX, x + 12, y, 290, 24, -1);
            } else {
                add(s_main, "STATIC", o.label, SS_CENTERIMAGE, x + 12, y, 110, 24, -1);
                o.ctl = add(s_main, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                            x + 124, y, 176, 300, -1);
                for (int k = 0; k < o.n; k++) SendMessageA(o.ctl, CB_ADDSTRING, 0, (LPARAM)o.names[k]);
            }
            opt_show(o, ps3_settings_str(o.section, o.key, NULL, o.def));
            y += 30;
        }
        if (g == 2) add(s_main, "BUTTON", "Keyboard keys...", WS_TABSTOP | BS_PUSHBUTTON, x + 12, y, 160, 26, ID_KEYS);
        y_next[col] = y0 + gh + 8;
    }
    int yb = y_next[0] > y_next[1] ? y_next[0] : y_next[1];
    /* Game patches, under both columns, two checkboxes a row. */
    if (s_npopt) {
        add(s_main, "BUTTON", "Game patches (off: as the game was made)", BS_GROUPBOX, 12, yb, 636, pgh, -1);
        for (int i = 0; i < s_npopt; i++) {
            Opt& o = s_popt[i];
            o.ctl = add(s_main, "BUTTON", o.label, WS_TABSTOP | BS_AUTOCHECKBOX,
                        24 + (i % 2) * 318, yb + 22 + (i / 2) * 30, 300, 24, -1);
            opt_show(o, ps3_settings_str(o.section, o.key, NULL, o.def));
        }
        yb += pgh + 8;
    }
    char where[1200];
    snprintf(where, sizeof where, "Settings: %s", ps3_settings_path());
    add(s_main, "STATIC", where, SS_PATHELLIPSIS, 12, yb, 636, 20, -1);
    add(s_main, "BUTTON", "Defaults", WS_TABSTOP | BS_PUSHBUTTON, 12, yb + 28, 90, 28, ID_DEFAULTS);
    add(s_main, "BUTTON", "Open save folder", WS_TABSTOP | BS_PUSHBUTTON, 108, yb + 28, 130, 28, ID_SAVES);
    add(s_main, "BUTTON", "Save", WS_TABSTOP | BS_PUSHBUTTON, 366, yb + 28, 90, 28, ID_SAVE);
    add(s_main, "BUTTON", "Quit", WS_TABSTOP | BS_PUSHBUTTON, 462, yb + 28, 90, 28, ID_QUIT);
    HWND play = add(s_main, "BUTTON", "Play", WS_TABSTOP | BS_DEFPUSHBUTTON, 558, yb + 28, 90, 28, ID_PLAY);
    ShowWindow(s_main, SW_SHOW);
    SetForegroundWindow(s_main);
    SetFocus(play);

    MSG msg;
    while (!s_done && GetMessageA(&msg, NULL, 0U, 0U) > 0) {
        /* While a binding waits for a key, every key goes to it: Tab, the
         * arrows, Enter and Space would otherwise move the focus or click. */
        if (s_capture >= 0 && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
            kb_key(msg.wParam, msg.lParam);
            continue;
        }
        if (s_capture >= 0 && (msg.message == WM_CHAR || msg.message == WM_SYSCHAR ||
                               msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP))
            continue;
        if (s_keys && IsDialogMessageA(s_keys, &msg)) continue;
        if (IsDialogMessageA(s_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (s_keys) DestroyWindow(s_keys);
    DestroyWindow(s_main);
    /* Let the destroy messages through before the game opens its window. */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return s_done == 1;
}
#else
extern "C" int gowa_launcher(void) { return 1; }   /* no window here: just play */
#endif
