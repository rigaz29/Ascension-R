/*
 * gowa-recomp -- the launcher
 *
 * The settings window shown before the game starts. It reads and writes
 * gowa.ini through ps3_settings, so what it shows is what the game will use:
 * Play saves and starts, Save saves, Defaults puts this port's defaults back
 * in the window (saved on Save or Play), Quit leaves without starting.
 * launcher.show = off skips it; holding Shift while the game starts brings it
 * back.
 *
 * The window is drawn with Direct2D and DirectWrite: a sidebar of tabs (Play,
 * Graphics, Display, Controls, Game patches, Cheats, System), a header with the
 * game's own art (PIC1.PNG and ICON0.PNG of the PS3_GAME folder the player
 * installed; nothing of the game is in this repository), cards of settings
 * as segmented buttons, switches and lists, and a bar with Play. The keyboard
 * keys are set in a window of their own. GOWA_LAUNCHER_SHOT=<folder> draws
 * every tab into PNG files there and quits, to look at the window without
 * opening it.
 *
 * The System tab shows the game's folder: Browse picks it, Auto detect looks
 * for it on every disk (game_find.cpp), and a window that opens with no game
 * starts looking by itself. Either sets the two links the port reads it through.
 *
 * Clear shaders, down the sidebar, moves the compiled shaders to the Recycle Bin.
 *
 * This file also holds this port's defaults (gowa_port_defaults), so the
 * game and the Defaults button take them from the same tables.
 */
#include "win32_compat.h"
#include "ps3emu/ps3_settings.h"
#include "game_find.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include <commctrl.h>   /* SetWindowSubclass: the keyboard window's buttons */
#include <shellapi.h>   /* ShellExecuteA: Open save folder */
#include <objbase.h>
#include <shobjidl.h>   /* IFileOpenDialog: Browse */
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dxgi.h>
#include <dwmapi.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
extern "C" const char* ps3_pad_key_name(int vk);   /* libs/input/cellPad.c */
#endif

extern "C" void gowa_port_defaults(void);
extern "C" int  gowa_launcher(void);   /* 1 = play, 0 = quit */
/* patches.cpp: the title patches this build has (scripts/title_patches.json). */
extern "C" int  gowa_title_patch_count(void);
extern "C" const char* gowa_title_patch_id(int i);
extern "C" const char* gowa_title_patch_name(int i);
extern "C" int  gowa_title_patch_default(int i);

/* ---------------------------------------------------------------------------
 * What the window offers
 * -----------------------------------------------------------------------*/
enum { T_PLAY, T_GRAPHICS, T_DISPLAY, T_CONTROLS, T_PATCHES, T_CHEATS, T_SYSTEM, T_N };

struct Opt {
    int tab;                   /* the tab it is on (T_*) */
    const char* section;
    const char* key;
    const char* def;           /* this port's default */
    const char* label;
    int check;                 /* a switch: values[0] is off, values[1] on */
    const char* const* values;
    const char* const* names;  /* the list's entries */
    int n;
    const char* const* seg;    /* short names: shown as segmented buttons, else a list */
    const char* desc;          /* a line under the label */
    char cur[64];              /* the value shown, kept as written */
};

static const char* const k_onoff[] = { "off", "on" };
static const char* const k_mode[] = { "windowed", "borderless" };
static const char* const k_mode_n[] = { "Windowed", "Borderless fullscreen" };
static const char* const k_mode_s[] = { "Windowed", "Borderless" };
static const char* const k_size[] = { "", "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160" };
static const char* const k_size_n[] = { "Default (720p x Windows scale)", "1280 x 720", "1600 x 900", "1920 x 1080",
                                        "2560 x 1440", "3840 x 2160" };
static const char* const k_fps[] = { "off", "title", "overlay" };
static const char* const k_fps_n[] = { "Off", "In the title bar", "On screen, with CPU/GPU load" };
static const char* const k_fps_s[] = { "Off", "Title bar", "On screen" };
static const char* const k_aniso[] = { "game", "2", "4", "8", "16" };
static const char* const k_aniso_n[] = { "As the game sets it", "2x", "4x", "8x", "16x" };
static const char* const k_aniso_s[] = { "Game", "2x", "4x", "8x", "16x" };
static const char* const k_async[] = { "0", "2" };
static const char* const k_cap[] = { "off", "20", "30" };
static const char* const k_cap_n[] = { "Off", "20 fps, even pace", "30 fps, even pace" };
static const char* const k_cap_s[] = { "Off", "20 fps", "30 fps" };
static const char* const k_scale[] = { "bilinear", "sharp", "fsr1", "fsr3" };
static const char* const k_scale_n[] = { "Bilinear", "Sharp bilinear", "AMD FSR 1", "AMD FSR 3.1" };
static const char* const k_scale_s[] = { "Bilinear", "Sharp", "FSR 1", "FSR 3.1" };
static const char* const k_aa[] = { "off", "smaa" };
static const char* const k_aa_n[] = { "Off", "SMAA 1x" };
static const char* const k_aa_s[] = { "Off", "SMAA" };
static const char* const k_sharp[] = { "off", "low", "medium", "high", "max" };
static const char* const k_sharp_n[] = { "Off", "Low", "Medium", "High (AMD's)", "Max" };
static const char* const k_sharp_s[] = { "Off", "Low", "Medium", "High", "Max" };
static const char* const k_taken[] = { "1", "0.5", "0.25", "0" };
static const char* const k_taken_n[] = { "Normal", "Half", "Quarter", "None" };
static const char* const k_dealt[] = { "1", "2", "5", "kill" };
static const char* const k_dealt_n[] = { "Normal", "2x", "5x", "One hit" };
static const char* const k_dev[] = { "auto", "pad", "keyboard" };
static const char* const k_dev_n[] = { "Controller, else keyboard", "Controller only", "Keyboard only" };
static const char* const k_dev_s[] = { "Auto", "Controller", "Keyboard" };
static const char* const k_dz[] = { "0.10", "0.15", "0.20", "0.24", "0.30", "0.35", "0.40" };
static const char* const k_dz_n[] = { "10%", "15%", "20%", "24% (XInput's)", "30%", "35%", "40%" };
static const char* const k_enter[] = { "cross", "circle" };
static const char* const k_enter_n[] = { "Cross", "Circle" };
static const char* const k_bak[] = { "0", "3", "5", "10" };
static const char* const k_bak_n[] = { "Off", "Keep 3", "Keep 5", "Keep 10" };
static const char* const k_bak_s[] = { "Off", "3", "5", "10" };
static const char* const k_lang[] = { "auto", "en", "en-gb", "fr", "es", "de", "it", "nl", "pt", "pt-br",
                                      "ru", "ja", "ko", "zh-t", "zh-s", "fi", "sv", "da", "no", "pl", "tr" };
static const char* const k_lang_n[] = { "Windows language", "English (US)", "English (UK)", "French", "Spanish",
                                        "German", "Italian", "Dutch", "Portuguese", "Portuguese (Brazil)",
                                        "Russian", "Japanese", "Korean", "Chinese (traditional)",
                                        "Chinese (simplified)", "Finnish", "Swedish", "Danish", "Norwegian",
                                        "Polish", "Turkish" };

#define LIST(v, n) v, n, (int)(sizeof v / sizeof v[0])
#define SW k_onoff, NULL, 2, NULL
static Opt s_opt[] = {
    { T_DISPLAY, "display", "mode", "windowed", "Window", 0, LIST(k_mode, k_mode_n), k_mode_s,
      "Borderless covers the whole screen." },
    { T_DISPLAY, "display", "window_size", "", "Window size", 0, LIST(k_size, k_size_n), NULL,
      "In windowed mode. The game draws 720p; the picture is scaled to the window." },
    { T_DISPLAY, "display", "vsync", "on", "VSync", 1, SW,
      "Wait for the display's refresh: no tearing." },
    { T_DISPLAY, "display", "frame_cap", "off", "Frame rate cap", 0, LIST(k_cap, k_cap_n), k_cap_s,
      "An even pace instead of the highest rate the PC reaches. With frame generation, off follows the monitor." },
    { T_DISPLAY, "display", "show_fps", "title", "Frame rate display", 0, LIST(k_fps, k_fps_n), k_fps_s,
      "On screen shows FPS, frame time and the CPU, GPU, PPU, SPU and RSX load." },
    { T_DISPLAY, "display", "shader_hint", "on", "Shader build indicator", 1, SW,
      "A spinner in the bottom left corner while the game's new shaders are built, as RPCS3 shows." },
    { T_GRAPHICS, "graphics", "scaling", "bilinear", "Scaling", 0, LIST(k_scale, k_scale_n), k_scale_s,
      "How the 720p picture is scaled up to the window. FSR 3.1 builds the detail from past frames, "
      "in place of SMAA, and FSR 1 takes over where there is no scene." },
    { T_GRAPHICS, "graphics", "sharpening", "high", "FSR sharpening", 0, LIST(k_sharp, k_sharp_n), k_sharp_s,
      "The sharpening pass of FSR (RCAS). Only with FSR scaling." },
    { T_GRAPHICS, "graphics", "frame_generation", "off", "Frame generation", 1, SW,
      "FSR 3.1: twice the frames on screen, with VSync on. The game is held at half the monitor's refresh rate, "
      "30 at 60 Hz and 60 at 120 Hz. A frame is made between each two, which holds the picture back by about a "
      "frame, and below 25 it stands aside." },
    { T_GRAPHICS, "graphics", "antialiasing", "off", "Anti-aliasing", 0, LIST(k_aa, k_aa_n), k_aa_s,
      "Smooths jagged edges after the game has drawn the frame. Costs next to nothing. FSR 3.1 scaling "
      "does this itself in play." },
    { T_GRAPHICS, "graphics", "anisotropy", "game", "Anisotropic filtering", 0, LIST(k_aniso, k_aniso_n), k_aniso_s,
      "Sharper textures seen at an angle. The game itself asks for none." },
    { T_GRAPHICS, "graphics", "async_shaders", "2", "Build shaders in the background", 1, k_async, NULL, 2, NULL,
      "A new shader is built while the game goes on, instead of a stall." },
    { T_GRAPHICS, "graphics", "render_thread", "on", "Render thread", 1, SW,
      "The GPU's work is recorded and shown on a thread of its own, beside the one that reads the game's "
      "commands: about 10% more frames where that one holds the game back." },
    { T_GRAPHICS, "graphics", "pipeline_wait", "on", "Build pipelines first", 1, SW,
      "At the start the game waits until the pipelines kept from earlier sessions are built, so none is built "
      "while you play: a moment, a few seconds after a GPU driver update." },
    { T_CONTROLS, "input", "device", "auto", "Device", 0, LIST(k_dev, k_dev_n), k_dev_s,
      "Auto takes a controller when one is there, else the keyboard and mouse." },
    { T_CONTROLS, "input", "deadzone", "0.24", "Stick dead zone", 0, LIST(k_dz, k_dz_n), NULL,
      "How far a stick moves before it counts." },
    { T_CONTROLS, "input", "enter_button", "cross", "Confirm button", 0, LIST(k_enter, k_enter_n), k_enter_n,
      "The button that confirms in the menus." },
    { T_CONTROLS, "input", "vibration", "on", "Vibration", 1, SW, "Controller rumble." },
    { T_CHEATS, "cheats", "damage_taken", "1", "Damage to Kratos", 0, LIST(k_taken, k_taken_n), k_taken_n,
      "How much of a hit he takes. None: hits take no health from him." },
    { T_CHEATS, "cheats", "damage_dealt", "1", "Kratos's damage", 0, LIST(k_dealt, k_dealt_n), k_dealt_n,
      "How much his hits take from an enemy. One hit: all of its health." },
    { T_CHEATS, "cheats", "max_orbs", "off", "Max red orbs", 1, SW,
      "The red orbs are held at the most the game allows, 1,000,000. A save made with this on keeps them." },
    { T_CHEATS, "cheats", "infinite_magic", "off", "Infinite magic and items", 1, SW,
      "The magic and item meters stay full." },
    { T_CHEATS, "cheats", "infinite_rage", "off", "Infinite rage", 1, SW,
      "The rage meter stays full." },
    { T_CHEATS, "cheats", "infinite_jumps", "off", "Infinite double jumps", 1, SW,
      "Kratos can jump again in the air, as often as he likes." },
    { T_SYSTEM, "system", "language", "auto", "Language", 0, LIST(k_lang, k_lang_n), NULL,
      "The game's language: text and speech." },
    { T_SYSTEM, "system", "esc_quits", "off", "Esc closes the game", 1, SW,
      "Esc is not a PS3 button; off leaves it free." },
    { T_SYSTEM, "system", "save_backups", "5", "Save backups", 0, LIST(k_bak, k_bak_n), k_bak_s,
      "Copies kept of a save before the game writes over it." },
    { T_SYSTEM, "system", "steady_audio", "on", "Steady audio", 1, SW,
      "Runs the game's sound on a thread of its own, so heavy fights do not break it up." },
    { T_SYSTEM, "launcher", "show", "on", "Show this window at start", 1, SW,
      "When off, hold Shift while the game starts to bring it back." },
};
#undef SW
#undef LIST
enum { N_OPT = sizeof s_opt / sizeof s_opt[0] };

/* The Game patches tab: a switch per patch, [patches] <id> = on/off, off by
 * default unless the catalog says "default": true. Built from the patches
 * the build has, so a patch added to scripts/title_patches.json shows up here
 * with the next lift. */
enum { MAX_POPT = 32 };
static Opt s_popt[MAX_POPT];
static int s_npopt;

/* What each known patch does, for the line under it. */
static const char* patch_desc(const char* id)
{
    static const char* const d[][2] = {
        { "disable_mlaa", "The PS3's SPU anti-aliasing, whose picture this port never shows: about 5% "
                          "faster. The bloom goes with it. FSR 3.1 and frame generation need this on." },
        { "disable_depth_of_field", "The blur of what lies outside the camera's focus." },
        { "disable_motion_blur", "Saves the blur's work. With Disable MLAA on it never reaches the screen "
                                 "anyway. FSR 3.1 and frame generation keep its draws, for what moves." },
        { "disable_mesh_trimming", "For drawing above 720p, which this port does not do: here it only adds "
                                   "geometry (3% slower at the first level, 11% at the gate)." },
        { "disable_shadows", "No dynamic shadows: the biggest saving, about 25% faster in the Megaera "
                             "fight." },
    };
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++)
        if (!strcmp(d[i][0], id)) return d[i][1];
    return "";
}

static void popt_init(void)
{
    s_npopt = 0;
    const int n = gowa_title_patch_count();
    for (int i = 0; i < n && s_npopt < MAX_POPT; i++) {
        Opt& o = s_popt[s_npopt++];
        o = Opt();
        o.tab = T_PATCHES; o.section = "patches"; o.key = gowa_title_patch_id(i);
        o.def = gowa_title_patch_default(i) ? "on" : "off";
        o.label = gowa_title_patch_name(i); o.check = 1; o.values = k_onoff; o.n = 2;
        o.desc = patch_desc(o.key);
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
 * Values
 * -----------------------------------------------------------------------*/
static int opt_index(const Opt& o, const char* v)
{
    for (int i = 0; i < o.n; i++)
        if (ps3_settings_ieq(o.values[i], v)) return i;
    return -1;
}

/* A switch takes what ps3_settings_bool takes for on, so that a "1" written
 * by hand is not shown off and then saved so. */
static int opt_on(const Opt& o)
{
    return ps3_settings_ieq(o.cur, o.values[1]) ||
           (o.values == k_onoff && (ps3_settings_ieq(o.cur, "1") || ps3_settings_ieq(o.cur, "true") ||
                                    ps3_settings_ieq(o.cur, "yes")));
}

static void opt_set(Opt& o, const char* v) { snprintf(o.cur, sizeof o.cur, "%s", v ? v : ""); }

static Opt* opt_find(const char* section, const char* key)
{
    for (int i = 0; i < N_OPT; i++)
        if (!strcmp(s_opt[i].section, section) && !strcmp(s_opt[i].key, key)) return &s_opt[i];
    return NULL;
}

/* What a list shows for the option's value. */
static const char* opt_name(const Opt& o)
{
    const int i = opt_index(o, o.cur);
    return i >= 0 ? o.names[i] : o.cur;
}

#ifdef _WIN32
/* ---------------------------------------------------------------------------
 * The keyboard window (Win32 controls)
 * -----------------------------------------------------------------------*/
enum { ID_KOK = 150, ID_KCANCEL, ID_KDEFAULTS, ID_KEY = 200 };

static int   s_dpi = 96;      /* the monitor's */
static float s_ui = 96.0f;   /* what the launcher is drawn at: s_dpi, less if it would not fit */
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

static void dark_caption(HWND h)
{
    BOOL dark = TRUE;   /* DWMWA_USE_IMMERSIVE_DARK_MODE: 20, 19 before Windows 10 20H1 */
    if (FAILED(DwmSetWindowAttribute(h, 20, &dark, sizeof dark))) DwmSetWindowAttribute(h, 19, &dark, sizeof dark);
}

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
    dark_caption(s_keys);
    EnableWindow(s_main, FALSE);
    ShowWindow(s_keys, SW_SHOW);
}

/* The folder the game keeps its saves in -- /dev_hdd0/home/00000001/savedata
 * under [paths] hdd0. */
static int saves_dir(char* path, size_t n)
{
    const char* hdd0 = ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL);
    if (!hdd0 || !*hdd0) return 0;
    snprintf(path, n, "%s\\home\\00000001\\savedata", hdd0);
    for (char* p = path; *p; p++) if (*p == '/') *p = '\\';
    return 1;
}

/* Open it in Explorer, creating it if no save exists yet. */
static void open_saves(void)
{
    char path[1100];
    if (!saves_dir(path, sizeof path)) {
        MessageBoxA(s_main, "No [paths] hdd0 in the settings file; run gowa.py build.",
                    "Open save folder", MB_OK | MB_ICONINFORMATION);
        return;
    }
    for (char* p = path; *p; p++)
        if (*p == '\\' && p > path && p[-1] != ':') { *p = 0; CreateDirectoryA(path, NULL); *p = '\\'; }
    CreateDirectoryA(path, NULL);
    ShellExecuteA(s_main, "open", path, NULL, NULL, SW_SHOWNORMAL);
}

static int save_all(void)
{
    for (int i = 0; i < N_OPT; i++) ps3_settings_set(s_opt[i].section, s_opt[i].key, s_opt[i].cur);
    for (int i = 0; i < s_npopt; i++) ps3_settings_set(s_popt[i].section, s_popt[i].key, s_popt[i].cur);
    if (ps3_settings_save(NULL) != 0) {
        char m[1200];
        snprintf(m, sizeof m, "Could not write %s.", ps3_settings_path());
        MessageBoxA(s_main, m, "Settings", MB_OK | MB_ICONWARNING);
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * The ready check: read once when the window opens
 * -----------------------------------------------------------------------*/
static struct {
    int elf_ok;
    int profiles;
    char saves[1100];
    char gpu[128];
    char vfs[1100];
    int pad;        /* an XInput controller is plugged in */
} s_rc;

static const wchar_t* W(const char* s);

/* The game's folder, as the links give it. */
static struct {
    GameInfo g;                        /* what <vfs>\PS3_GAME leads to */
    wchar_t vfs[GAME_PATH_MAX], hdd0[GAME_PATH_MAX];              /* [paths], else next to the exe */
    wchar_t link_vfs[GAME_PATH_MAX], link_hdd0[GAME_PATH_MAX];    /* <vfs>\PS3_GAME, <hdd0>\game\NPUA80918 */
    wchar_t note[GAME_PATH_MAX + 128]; /* what the last search or Browse came to, if no folder was set */
} s_game;

/* The search, on a thread of its own. */
static struct {
    HANDLE thread;
    volatile long stop;
    wchar_t under[GAME_PATH_MAX];      /* the folder Browse gave; empty, every disk */
    GameInfo best;
    SRWLOCK lock;                      /* over now */
    wchar_t now[GAME_PATH_MAX];        /* the folder being read */
} s_find;

static void path_slashes(wchar_t* p) { for (; *p; p++) if (*p == L'/') *p = L'\\'; }

static void game_links(void)
{
    const char* vfs = ps3_settings_str("paths", "vfs", "PS3_VFS_ROOT", NULL);
    const char* hdd0 = ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL);
    wchar_t here[GAME_PATH_MAX];
    if (!GetModuleFileNameW(NULL, here, GAME_PATH_MAX)) here[0] = 0;
    if (wchar_t* s = wcsrchr(here, L'\\')) *s = 0;
    if (vfs && *vfs) swprintf(s_game.vfs, GAME_PATH_MAX, L"%ls", W(vfs));
    else swprintf(s_game.vfs, GAME_PATH_MAX, L"%ls\\vfs", here);
    if (hdd0 && *hdd0) swprintf(s_game.hdd0, GAME_PATH_MAX, L"%ls", W(hdd0));
    else swprintf(s_game.hdd0, GAME_PATH_MAX, L"%ls\\gamedata\\dev_hdd0", here);
    path_slashes(s_game.vfs);
    path_slashes(s_game.hdd0);
    swprintf(s_game.link_vfs, GAME_PATH_MAX, L"%ls\\PS3_GAME", s_game.vfs);
    swprintf(s_game.link_hdd0, GAME_PATH_MAX, L"%ls\\game\\NPUA80918", s_game.hdd0);
}

/* What the links lead to now, and whether the game can start. */
static void game_refresh(void)
{
    game_links();
    wchar_t dir[GAME_PATH_MAX];
    if (!game_link_target(s_game.link_vfs, dir, GAME_PATH_MAX))
        swprintf(dir, GAME_PATH_MAX, L"%ls", s_game.link_vfs);   /* a real folder, or none */
    game_probe(dir, &s_game.g);
    const char* elf = ps3_settings_str("paths", "elf", NULL, NULL);
    s_rc.elf_ok = elf && *elf && GetFileAttributesA(elf) != INVALID_FILE_ATTRIBUTES;
}

static const wchar_t* game_path_shown(void)
{
    return GetFileAttributesW(s_game.link_vfs) == INVALID_FILE_ATTRIBUTES ? L"Not set" : s_game.g.dir;
}

/* 1.12, from the 01.12 of PARAM.SFO. */
static const wchar_t* game_version(void)
{
    const char* v = s_game.g.app_ver;
    return W(*v == '0' ? v + 1 : v);
}

/* A line on what the folder holds; the level as check_line takes it. */
static int game_status(wchar_t* b, size_t n)
{
    const GameInfo& g = s_game.g;
    if (s_game.note[0]) { swprintf(b, n, L"%ls", s_game.note); return -1; }
    switch (g.state) {
    case GAME_OK:
        swprintf(b, n, L"God of War: Ascension \x00B7 %ls \x00B7 %ls", W(g.title_id), game_version());
        return 1;
    case GAME_OTHER_BUILD:
        swprintf(b, n, L"%ls %ls, but not the GOWA.elf this port was made from: it may not run", W(g.title_id),
                 game_version());
        return 0;
    case GAME_NO_ELF:
        swprintf(b, n, L"Decrypt USRDIR\\GOWA.SELF to GOWA.elf with RPCS3 (Utilities > Decrypt PS3 Binaries)");
        return -1;
    case GAME_OTHER_TITLE:
        swprintf(b, n, L"That is %ls %ls. This port runs NPUA80918 1.12", W(g.title_id), game_version());
        return -1;
    default:
        swprintf(b, n, L"No game there. Auto detect looks on every disk, or Browse to its folder");
        return -1;
    }
}

/* XInput, as cellPad finds it: from System32, at run time. */
static int pad_connected(void)
{
    typedef DWORD (WINAPI *get_state_fn)(DWORD, void*);
    static get_state_fn get_state;
    static int tried;
    if (!tried) {
        tried = 1;
        static const wchar_t* const names[] = { L"XInput1_4.dll", L"XInput1_3.dll", L"XInput9_1_0.dll" };
        for (size_t k = 0; k < sizeof names / sizeof names[0] && !get_state; k++) {
            HMODULE m = LoadLibraryExW(names[k], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (m) get_state = (get_state_fn)(void*)GetProcAddress(m, "XInputGetState");
        }
    }
    DWORD state[4];   /* XINPUT_STATE: 16 bytes */
    for (DWORD i = 0; get_state && i < 4; i++)
        if (get_state(i, state) == ERROR_SUCCESS) return 1;
    return 0;
}

static void ready_check(void)
{
    game_refresh();
    s_rc.profiles = 0;
    if (saves_dir(s_rc.saves, sizeof s_rc.saves)) {
        char pat[1200];
        snprintf(pat, sizeof pat, "%s\\*-PROFDATA-*", s_rc.saves);
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(pat, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) s_rc.profiles++; } while (FindNextFileA(f, &fd));
            FindClose(f);
        }
    }
    /* The adapter with the most memory of its own: the one a laptop's game runs on. */
    snprintf(s_rc.gpu, sizeof s_rc.gpu, "%s", "");
    IDXGIFactory1* fac = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac)) && fac) {
        SIZE_T best = 0;
        IDXGIAdapter1* a = NULL;
        for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > best) {
                best = d.DedicatedVideoMemory;
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, s_rc.gpu, sizeof s_rc.gpu, NULL, NULL);
            }
            a->Release();
        }
        fac->Release();
    }
    s_rc.pad = pad_connected();
}

/* ---------------------------------------------------------------------------
 * Presets: the patch sets the Play tab offers
 * -----------------------------------------------------------------------*/
struct Preset { const char* name; const char* desc; const char* on[6]; };
static const Preset k_presets[] = {
    { "Smooth", "The default and the fastest: no dynamic shadows, motion blur or MLAA.",
      { "disable_mlaa", "disable_motion_blur", "disable_shadows", NULL } },
    { "Balanced", "Keeps the dynamic shadows; no motion blur or MLAA.",
      { "disable_mlaa", "disable_motion_blur", NULL } },
    { "Faithful", "Everything drawn as on the PS3. The slowest.",
      { NULL } },
};
enum { N_PRESET = sizeof k_presets / sizeof k_presets[0] };

static int preset_has(const Preset& p, const char* id)
{
    for (int i = 0; p.on[i]; i++) if (!strcmp(p.on[i], id)) return 1;
    return 0;
}
/* The preset the patches match, or -1. */
static int preset_now(void)
{
    for (int k = 0; k < N_PRESET; k++) {
        int same = 1;
        for (int i = 0; i < s_npopt && same; i++) same = opt_on(s_popt[i]) == preset_has(k_presets[k], s_popt[i].key);
        if (same) return k;
    }
    return -1;
}
static void preset_apply(int k)
{
    for (int i = 0; i < s_npopt; i++) opt_set(s_popt[i], preset_has(k_presets[k], s_popt[i].key) ? "on" : "off");
}

/* ---------------------------------------------------------------------------
 * Drawing: Direct2D and DirectWrite
 * -----------------------------------------------------------------------*/
static const float WIN_W = 1120.0f, WIN_H = 720.0f, SIDE_W = 248.0f, FOOT_H = 72.0f;

/* Colours, 0xRRGGBB */
enum : unsigned {
    C_BG = 0x0E0E10, C_SIDE = 0x131315, C_CARD = 0x1A1A1E, C_CARD_HI = 0x222227, C_LINE = 0x2B2B32,
    C_TEXT = 0xECECEE, C_TEXT2 = 0xA3A3AD, C_MUTED = 0x71717B, C_RED = 0xB3261E, C_RED_HI = 0xD0352B,
    C_GOLD = 0xE2A23B, C_OK = 0x4ADE80, C_WARN = 0xFBBF24, C_BAD = 0xF87171,
};

static ID2D1Factory* s_d2d;
static IDWriteFactory* s_dw;
static IWICImagingFactory* s_wic;
static IWICFormatConverter* s_img_bg;     /* PIC1.PNG */
static IWICFormatConverter* s_img_logo;   /* ICON0.PNG */
static IDWriteTextFormat *s_tf_ui, *s_tf_ui_b, *s_tf_small, *s_tf_head, *s_tf_title, *s_tf_icon, *s_tf_play, *s_tf_cap;
static IDWriteTextFormat* s_tf_desc;   /* s_tf_small from the top of its box: the lines under labels */
static IDWriteTextFormat* s_tf_path;   /* s_tf_small on one line, cut before its last two folders */
static const wchar_t* s_icon_font = L"Segoe MDL2 Assets";

/* What the render target owns; made again with it. */
static ID2D1RenderTarget* s_rt;
static ID2D1SolidColorBrush* s_br;
static ID2D1Bitmap *s_bm_bg, *s_bm_logo;

static D2D1_COLOR_F col(unsigned rgb, float a = 1.0f)
{
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

/* UTF-8 to UTF-16, into one of a few buffers that rotate. */
static const wchar_t* W(const char* s)
{
    static wchar_t buf[8][1200];
    static int k;
    wchar_t* b = buf[k++ & 7];
    if (!MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, b, 1200)) b[0] = 0;
    return b;
}

static IDWriteTextFormat* tf(const wchar_t* family, float px, DWRITE_FONT_WEIGHT wt)
{
    IDWriteTextFormat* f = NULL;
    s_dw->CreateTextFormat(family, NULL, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", &f);
    if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return f;
}

static int font_exists(const wchar_t* family)
{
    IDWriteFontCollection* c = NULL;
    UINT32 idx = 0;
    BOOL ok = FALSE;
    if (SUCCEEDED(s_dw->GetSystemFontCollection(&c, FALSE)) && c) {
        c->FindFamilyName(family, &idx, &ok);
        c->Release();
    }
    return ok;
}

static IWICFormatConverter* load_png(const char* path)
{
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* fr = NULL;
    IWICFormatConverter* cv = NULL;
    if (!s_wic || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return NULL;
    if (SUCCEEDED(s_wic->CreateDecoderFromFilename(W(path), NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(s_wic->CreateFormatConverter(&cv)) &&
        FAILED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                              WICBitmapPaletteTypeMedianCut))) {
        cv->Release();
        cv = NULL;
    }
    if (fr) fr->Release();
    if (dec) dec->Release();
    return cv;
}

/* The game's pictures, through the link. */
static void art_load(void)
{
    char p[3700];
    WideCharToMultiByte(CP_UTF8, 0, s_game.link_vfs, -1, s_rc.vfs, sizeof s_rc.vfs, NULL, NULL);
    snprintf(p, sizeof p, "%s\\PIC1.PNG", s_rc.vfs);
    s_img_bg = load_png(p);
    snprintf(p, sizeof p, "%s\\ICON0.PNG", s_rc.vfs);
    s_img_logo = load_png(p);
}

static int ui_init(void)
{
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &s_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&s_dw)))
        return 0;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_wic));
    if (font_exists(L"Segoe Fluent Icons")) s_icon_font = L"Segoe Fluent Icons";
    const wchar_t* ui = font_exists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* serif = font_exists(L"Palatino Linotype") ? L"Palatino Linotype" : L"Georgia";
    s_tf_ui = tf(ui, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
    s_tf_ui_b = tf(ui, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    s_tf_small = tf(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    s_tf_cap = tf(ui, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    s_tf_head = tf(serif, 17.0f, DWRITE_FONT_WEIGHT_BOLD);
    s_tf_title = tf(serif, 30.0f, DWRITE_FONT_WEIGHT_BOLD);
    s_tf_play = tf(serif, 22.0f, DWRITE_FONT_WEIGHT_BOLD);
    s_tf_icon = tf(s_icon_font, 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
    s_tf_desc = tf(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    s_tf_path = tf(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    if (!s_tf_ui || !s_tf_ui_b || !s_tf_small || !s_tf_cap || !s_tf_head || !s_tf_title || !s_tf_play || !s_tf_icon ||
        !s_tf_desc || !s_tf_path)
        return 0;
    s_tf_desc->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    {
        DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* dots = NULL;
        s_dw->CreateEllipsisTrimmingSign(s_tf_ui, &dots);
        IDWriteTextFormat* one_line[] = { s_tf_ui, s_tf_ui_b, s_tf_cap, s_tf_head, s_tf_title, s_tf_play };
        for (size_t i = 0; i < sizeof one_line / sizeof one_line[0]; i++) {
            one_line[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (dots) one_line[i]->SetTrimming(&trim, dots);
        }
        /* A path keeps its end: "C:\...\build\gowa.ini". */
        DWRITE_TRIMMING at_folder = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, L'\\', 2 };
        s_tf_path->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (dots) s_tf_path->SetTrimming(&at_folder, dots);
        if (dots) dots->Release();
    }
    art_load();
    return 1;
}

static void rt_release(void)
{
    if (s_bm_bg) { s_bm_bg->Release(); s_bm_bg = NULL; }
    if (s_bm_logo) { s_bm_logo->Release(); s_bm_logo = NULL; }
    if (s_br) { s_br->Release(); s_br = NULL; }
    if (s_rt) { s_rt->Release(); s_rt = NULL; }
}

static void rt_bitmaps(void)
{
    if (s_img_bg) s_rt->CreateBitmapFromWicBitmap(s_img_bg, NULL, &s_bm_bg);
    if (s_img_logo) s_rt->CreateBitmapFromWicBitmap(s_img_logo, NULL, &s_bm_logo);
}

static void rt_resources(void)
{
    s_rt->CreateSolidColorBrush(col(C_TEXT), &s_br);
    rt_bitmaps();
}

/* The pictures of another folder. */
static void art_reload(void)
{
    if (s_bm_bg) { s_bm_bg->Release(); s_bm_bg = NULL; }
    if (s_bm_logo) { s_bm_logo->Release(); s_bm_logo = NULL; }
    if (s_img_bg) { s_img_bg->Release(); s_img_bg = NULL; }
    if (s_img_logo) { s_img_logo->Release(); s_img_logo = NULL; }
    art_load();
    if (s_rt) rt_bitmaps();
}

static void fill(D2D1_RECT_F r, unsigned rgb, float a = 1.0f)
{
    s_br->SetColor(col(rgb, a));
    s_rt->FillRectangle(r, s_br);
}
static void fill_rr(D2D1_RECT_F r, float rad, unsigned rgb, float a = 1.0f)
{
    s_br->SetColor(col(rgb, a));
    s_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), s_br);
}
static void stroke_rr(D2D1_RECT_F r, float rad, unsigned rgb, float a = 1.0f, float w = 1.0f)
{
    s_br->SetColor(col(rgb, a));
    s_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), s_br, w);
}
static void text(const wchar_t* s, IDWriteTextFormat* f, D2D1_RECT_F r, unsigned rgb, float a = 1.0f,
                 DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING)
{
    f->SetTextAlignment(al);
    s_br->SetColor(col(rgb, a));
    s_rt->DrawText(s, (UINT32)wcslen(s), f, r, s_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static D2D1_RECT_F R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }

/* How tall a text is, wrapped at a width. */
static float text_h(const wchar_t* s, IDWriteTextFormat* f, float w)
{
    IDWriteTextLayout* l = NULL;
    float h = 0.0f;
    if (SUCCEEDED(s_dw->CreateTextLayout(s, (UINT32)wcslen(s), f, w, 1000.0f, &l)) && l) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) h = m.height;
        l->Release();
    }
    return h;
}

/* ---------------------------------------------------------------------------
 * The window's state and what can be clicked
 * -----------------------------------------------------------------------*/
enum {
    H_NAV = 1000, H_SEG = 2000, H_SW = 5000, H_DD = 6000, H_PSW = 7000, H_PRESET = 7500,
    H_PLAY = 8000, H_SAVE, H_DEFAULTS, H_QUIT, H_KEYS, H_SAVES, H_SHADERS, H_GAME_BROWSE, H_GAME_AUTO, H_DDITEM = 9000,
};
struct Hit { D2D1_RECT_F r; int id; };
static Hit s_hit[400];
static int s_nhit;
static int s_tab = T_PLAY;
static int s_hover = -1, s_press = -1;
static float s_mx = -1, s_my = -1;
static int s_dd = -1;            /* the option whose list is open, or -1 */
static D2D1_RECT_F s_dd_at;      /* the box it opened from */
static int s_dd_top;             /* the first entry shown */
static char s_toast[64];
static int s_toast_bad;          /* not good news: red, with a cross */
static ULONGLONG s_toast_until;

/* A tab whose card is taller than the window scrolls it, by the wheel: what it hides is not there to
 * click either. */
static float s_scroll[T_N];       /* how far each tab is scrolled */
static float s_scroll_max[T_N];   /* how far it can be, from the tab's last drawing */
static const D2D1_RECT_F NO_CLIP = { -1e9f, -1e9f, 1e9f, 1e9f };
static D2D1_RECT_F s_clip = NO_CLIP;

static void hit(D2D1_RECT_F r, int id)
{
    if (r.top < s_clip.top) r.top = s_clip.top;
    if (r.bottom > s_clip.bottom) r.bottom = s_clip.bottom;
    if (r.bottom <= r.top) return;
    if (s_nhit < (int)(sizeof s_hit / sizeof s_hit[0])) { s_hit[s_nhit].r = r; s_hit[s_nhit].id = id; s_nhit++; }
}
static int inside(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static int hovered(int id) { return s_hover == id; }

/* --- widgets --------------------------------------------------------------- */
static const float SEG_H = 34.0f;

/* Buttons side by side, one of them chosen: the option's values. */
static void w_segmented(Opt& o, int oi, float x, float y, float w)
{
    const D2D1_RECT_F box = R(x, y, w, SEG_H);
    fill_rr(box, 8, C_BG);
    stroke_rr(box, 8, C_LINE);
    const int sel = opt_index(o, o.cur);
    const float sw = (w - 4.0f) / (float)o.n;
    for (int i = 0; i < o.n; i++) {
        const D2D1_RECT_F s = R(x + 2.0f + sw * (float)i, y + 2.0f, sw, SEG_H - 4.0f);
        const int id = H_SEG + oi * 16 + i;
        if (i == sel) fill_rr(s, 6, C_RED);
        else if (hovered(id)) fill_rr(s, 6, C_CARD_HI);
        text(W((o.seg ? o.seg : o.names)[i]), s_tf_ui, s, i == sel ? 0xFFFFFF : C_TEXT2, 1.0f,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(s, id);
    }
}

/* A switch, on the right of its row. */
static void w_switch(int on, float x, float y, int id)
{
    const D2D1_RECT_F t = R(x, y, 46, 26);
    fill_rr(t, 13, on ? C_RED : 0x3A3A42);
    if (hovered(id)) stroke_rr(t, 13, on ? C_RED_HI : 0x55555E, 1.0f, 1.5f);
    s_br->SetColor(col(0xFFFFFF));
    s_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(on ? x + 33.0f : x + 13.0f, y + 13.0f), 9.0f, 9.0f), s_br);
    hit(R(x - 4, y - 4, 54, 34), id);
}

/* A box showing the value; it opens the list. */
static void w_dropdown(Opt& o, int oi, float x, float y, float w)
{
    const int id = H_DD + oi;
    const D2D1_RECT_F box = R(x, y, w, SEG_H);
    fill_rr(box, 8, hovered(id) || s_dd == oi ? C_CARD_HI : C_BG);
    stroke_rr(box, 8, s_dd == oi ? C_RED : C_LINE);
    text(W(opt_name(o)), s_tf_ui, R(x + 12, y, w - 40, SEG_H), C_TEXT);
    text(L"\xE70D", s_tf_icon, R(x + w - 30, y, 20, SEG_H), C_TEXT2, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    hit(box, id);
}

static void w_button(const wchar_t* label, D2D1_RECT_F r, int id, int primary)
{
    const int hov = hovered(id);
    if (primary) {
        fill_rr(r, 8, hov ? C_RED_HI : C_RED);
        stroke_rr(r, 8, 0xE5534B, 0.6f);
        text(label, s_tf_play, r, 0xFFF4E0, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        fill_rr(r, 8, hov ? C_CARD_HI : C_CARD);
        stroke_rr(r, 8, C_LINE);
        text(label, s_tf_ui_b, r, C_TEXT, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    hit(r, id);
}

static void w_card(D2D1_RECT_F r, const wchar_t* title)
{
    fill_rr(r, 12, C_CARD);
    stroke_rr(r, 12, C_LINE);
    if (title) text(title, s_tf_head, R(r.left + 22, r.top + 14, r.right - r.left - 44, 26), C_GOLD);
}

/* One setting: its label and the line under it on the left, its control on the right. */
/* The width of a row's control, and the height of its label and the line under it. */
static float ctl_w(const Opt& o)
{
    return o.check ? 46.0f : o.seg ? (o.n <= 2 ? 220.0f : (float)o.n * (o.n >= 5 ? 66.0f : 82.0f)) : 300.0f;
}
static float label_h(const wchar_t* desc, float tw, float* dh)
{
    *dh = desc && *desc ? text_h(desc, s_tf_desc, tw) : 0.0f;
    return 22.0f + (*dh > 0.0f ? 4.0f + *dh : 0.0f);
}
static float row_h(float lh) { return lh + 28.0f < 64.0f ? 64.0f : lh + 28.0f; }

/* A label and the line under it, in the middle of a row's height. */
static void w_label(const wchar_t* label, const wchar_t* desc, float x, float y, float tw, float rh, float lh, float dh)
{
    const float ty = y + (rh - lh) * 0.5f;
    text(label, s_tf_ui_b, R(x, ty, tw, 22), C_TEXT);
    if (dh > 0.0f) text(desc, s_tf_desc, R(x, ty + 26, tw, dh + 2), C_MUTED);
}

static void w_rule(float x, float y, float w)
{
    s_br->SetColor(col(C_LINE, 0.6f));
    s_rt->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x + w, y), s_br, 1.0f);
}

static float opt_row_h(const Opt& o, float w)
{
    float dh;
    return row_h(label_h(o.desc ? W(o.desc) : NULL, w - ctl_w(o) - 32, &dh));
}

static float w_row(Opt& o, int oi, float x, float y, float w, int sw_id)
{
    const float cw = ctl_w(o), tw = w - cw - 32;
    float dh;
    const wchar_t* desc = o.desc ? W(o.desc) : NULL;
    const float lh = label_h(desc, tw, &dh), rh = row_h(lh);
    w_label(W(o.label), desc, x, y, tw, rh, lh, dh);
    const float cx = x + w - cw, cy = y + (rh - (o.check ? 26.0f : SEG_H)) * 0.5f;
    if (o.check) w_switch(opt_on(o), cx, cy, sw_id);
    else if (o.seg) w_segmented(o, oi, cx, cy, cw);
    else w_dropdown(o, oi, cx, cy, cw);
    return rh;
}

/* --- the parts of the window ---------------------------------------------- */
static const wchar_t* const k_tab_name[T_N] = { L"Play", L"Graphics", L"Display", L"Controls", L"Game patches", L"Cheats", L"System" };
static const wchar_t* const k_tab_icon[T_N] = { L"\xE768", L"\xE790", L"\xE7F4", L"\xE7FC", L"\xE90F", L"\xE945", L"\xE770" };
static const wchar_t* const k_tab_desc[T_N] = {
    L"", L"How the picture is scaled, smoothed and filtered.", L"The window, its pace and the frame rate display.",
    L"Controller and keyboard.", L"Switches that change what the game draws, for speed.",
    L"For an easier fight. The game reads these when it starts.",
    L"The game's folder, language, saves and this window." };

static void draw_sidebar(void)
{
    fill(R(0, 0, SIDE_W, WIN_H), C_SIDE);
    s_br->SetColor(col(C_LINE));
    s_rt->DrawLine(D2D1::Point2F(SIDE_W - 0.5f, 0), D2D1::Point2F(SIDE_W - 0.5f, WIN_H), s_br, 1.0f);
    /* The game's logo (ICON0.PNG: 320 x 176), else its name. */
    if (s_bm_logo) s_rt->DrawBitmap(s_bm_logo, R(24, 18, 200, 110), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    else text(L"GOD OF WAR\nASCENSION", s_tf_head, R(24, 30, 200, 80), C_GOLD);
    text(L"PC PORT \x00B7 RECOMPILED", s_tf_cap, R(26, 132, 200, 18), C_GOLD, 0.9f);
    {
        static const char d[] = __DATE__;   /* "Mmm dd yyyy", the day padded with a space */
        char b[40];
        snprintf(b, sizeof b, "Built %d %.3s %.4s", atoi(d + 4), d, d + 7);
        text(W(b), s_tf_small, R(26, 150, 200, 18), C_MUTED);
    }
    float y = 186.0f;
    for (int t = 0; t < T_N; t++) {
        const D2D1_RECT_F r = R(14, y, SIDE_W - 28, 40);
        const int id = H_NAV + t;
        if (t == s_tab) {
            fill_rr(r, 8, C_CARD_HI);
            fill_rr(R(14, y + 9, 3, 22), 1.5f, C_RED);
        } else if (hovered(id)) {
            fill_rr(r, 8, C_CARD);
        }
        text(k_tab_icon[t], s_tf_icon, R(30, y, 24, 40), t == s_tab ? C_GOLD : C_TEXT2, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        text(k_tab_name[t], t == s_tab ? s_tf_ui_b : s_tf_ui, R(64, y, 160, 40), t == s_tab ? C_TEXT : C_TEXT2);
        hit(r, id);
        y += 44.0f;
    }
    /* Down at the bottom: the compiled shaders and the save folder. */
    const D2D1_RECT_F c = R(14, WIN_H - 104, SIDE_W - 28, 40);
    if (hovered(H_SHADERS)) fill_rr(c, 8, C_CARD);
    text(L"\xE74D", s_tf_icon, R(30, c.top, 24, 40), C_TEXT2, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(L"Clear shaders", s_tf_ui, R(64, c.top, 160, 40), C_TEXT2);
    hit(c, H_SHADERS);
    const D2D1_RECT_F r = R(14, WIN_H - 60, SIDE_W - 28, 40);
    if (hovered(H_SAVES)) fill_rr(r, 8, C_CARD);
    text(L"\xE8B7", s_tf_icon, R(30, r.top, 24, 40), C_TEXT2, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(L"Open save folder", s_tf_ui, R(64, r.top, 160, 40), C_TEXT2);
    hit(r, H_SAVES);
}

/* The header: the game's art (PIC1.PNG, 1920 x 1080) under a dark fade. */
static void draw_hero(float h)
{
    const D2D1_RECT_F r = R(SIDE_W, 0, WIN_W - SIDE_W, h);
    fill(r, 0x1A1410);
    if (s_bm_bg) {
        /* Kratos is in the upper middle: a band from 18% of the height, the full width. */
        const D2D1_SIZE_F sz = s_bm_bg->GetSize();
        const float aspect = (WIN_W - SIDE_W) / h;
        const float sh = sz.width / aspect, sy = sz.height * 0.16f;
        s_rt->DrawBitmap(s_bm_bg, r, 0.85f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                         D2D1::RectF(0, sy, sz.width, sy + sh));
    }
    /* Dark from the left for the text, and into the page at the bottom. */
    ID2D1GradientStopCollection* gs = NULL;
    D2D1_GRADIENT_STOP st[3] = { { 0.0f, col(C_BG, 0.96f) }, { 0.45f, col(C_BG, 0.55f) }, { 1.0f, col(C_BG, 0.05f) } };
    ID2D1LinearGradientBrush* g = NULL;
    if (SUCCEEDED(s_rt->CreateGradientStopCollection(st, 3, &gs)) &&
        SUCCEEDED(s_rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(SIDE_W, 0), D2D1::Point2F(WIN_W, 0)), gs, &g))) {
        s_rt->FillRectangle(r, g);
        g->Release();
    }
    if (gs) { gs->Release(); gs = NULL; }
    D2D1_GRADIENT_STOP sb[2] = { { 0.0f, col(C_BG, 0.0f) }, { 1.0f, col(C_BG, 1.0f) } };
    if (SUCCEEDED(s_rt->CreateGradientStopCollection(sb, 2, &gs)) &&
        SUCCEEDED(s_rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, h * 0.55f), D2D1::Point2F(0, h)), gs, &g))) {
        s_rt->FillRectangle(r, g);
        g->Release();
    }
    if (gs) gs->Release();
}

static void draw_chip(const wchar_t* s, float x, float y, float w)
{
    const D2D1_RECT_F r = R(x, y, w, 28);
    fill_rr(r, 14, C_BG, 0.55f);
    stroke_rr(r, 14, C_GOLD, 0.7f);
    text(s, s_tf_small, r, C_TEXT, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
}

static void check_line(float x, float y, float w, int state, const wchar_t* s)
{
    /* state: 1 fine, 2 for information, 0 a warning, -1 missing */
    const unsigned c = state == 1 ? C_OK : state == 2 ? C_TEXT2 : state == 0 ? C_WARN : C_BAD;
    text(state == 1 ? L"\xE73E" : state == 2 ? L"\xE946" : state == 0 ? L"\xE7BA" : L"\xE711", s_tf_icon,
         R(x, y, 22, 26), c, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(s, s_tf_ui, R(x + 32, y, w - 32, 26), state < 0 ? C_BAD : C_TEXT);
}

static void draw_play(void)
{
    const float hh = 270.0f;
    draw_hero(hh);
    const float x0 = SIDE_W + 40;
    text(L"NATIVE PC PORT \x00B7 WINDOWS", s_tf_cap, R(x0, 44, 400, 18), C_GOLD);
    text(L"GOD OF WAR", s_tf_title, R(x0, 66, 520, 44), 0xF3E6D0);
    text(L"ASCENSION", s_tf_head, R(x0 + 2, 108, 520, 26), C_GOLD);
    text(L"Recompiled from the PlayStation 3 version: PPU and SPU code lifted to x86, the RSX drawn with Direct3D 12.",
         s_tf_small, R(x0, 138, 470, 40), C_TEXT2);
    {
        wchar_t b[60];
        if (s_game.g.title_id[0]) swprintf(b, 60, L"%ls \x00B7 %ls", W(s_game.g.title_id), game_version());
        else swprintf(b, 60, L"NPUA80918 \x00B7 1.12");
        draw_chip(b, x0, 190, 150);
    }
    draw_chip(L"Direct3D 12", x0 + 160, 190, 110);
    draw_chip(L"720p internal", x0 + 280, 190, 120);

    /* Ready check */
    const float cy = hh + 8, chh = WIN_H - FOOT_H - cy - 20;
    const float cw = (WIN_W - SIDE_W - 80 - 20) * 0.5f;
    const D2D1_RECT_F c1 = R(x0, cy, cw, chh);
    w_card(c1, L"READY CHECK");
    float y = cy + 54;
    if (s_find.thread) {
        check_line(x0 + 22, y, cw - 44, 2, L"Looking for the game\x2026");
    } else if (!s_rc.elf_ok) {
        check_line(x0 + 22, y, cw - 150, -1, s_game.g.state == GAME_NO_ELF ? L"GOWA.elf missing (System tab)"
                                                                           : L"Game not found");
        w_button(L"Find game", R(x0 + cw - 118, y - 2, 96, 30), H_GAME_AUTO, 0);
    } else if (s_game.g.state == GAME_OTHER_BUILD) {
        check_line(x0 + 22, y, cw - 44, 0, L"Game found, but another GOWA.elf");
    } else {
        wchar_t b[100];
        swprintf(b, 100, L"Game found: %ls %ls", s_game.g.title_id[0] ? W(s_game.g.title_id) : L"GOWA.elf", game_version());
        check_line(x0 + 22, y, cw - 44, 1, b);
    }
    y += 34;
    {
        wchar_t b[200];
        if (s_rc.profiles > 0) swprintf(b, 200, L"Saves: %d profile%ls", s_rc.profiles, s_rc.profiles == 1 ? L"" : L"s");
        else swprintf(b, 200, L"No saves yet: a new game makes one");
        check_line(x0 + 22, y, cw - 44, s_rc.profiles > 0 ? 1 : 0, b);
    }
    y += 34;
    {
        wchar_t b[300];
        swprintf(b, 300, L"Graphics: %ls", s_rc.gpu[0] ? W(s_rc.gpu) : L"no Direct3D adapter found");
        check_line(x0 + 22, y, cw - 44, s_rc.gpu[0] ? 1 : -1, b);
    }
    y += 34;
    {
        int n = 0;
        for (int i = 0; i < s_npopt; i++) n += opt_on(s_popt[i]);
        wchar_t b[200];
        swprintf(b, 200, L"Game patches: %d of %d on", n, s_npopt);
        check_line(x0 + 22, y, cw - 44, 1, b);
    }
    y += 34;
    {
        const Opt* dev = opt_find("input", "device");
        if (dev && ps3_settings_ieq(dev->cur, "keyboard"))
            check_line(x0 + 22, y, cw - 44, 2, L"Keyboard and mouse (Controls tab)");
        else if (s_rc.pad)
            check_line(x0 + 22, y, cw - 44, 1, L"Controller connected");
        else
            check_line(x0 + 22, y, cw - 44, dev && ps3_settings_ieq(dev->cur, "pad") ? 0 : 2,
                       dev && ps3_settings_ieq(dev->cur, "pad") ? L"No controller found (Controls: controller only)"
                                                                : L"No controller: keyboard and mouse");
    }
    Opt* fps = opt_find("display", "show_fps");
    text(L"Game folder", s_tf_cap, R(x0 + 22, cy + chh - 94, cw - 44, 18), C_MUTED);
    text(game_path_shown(), s_tf_path, R(x0 + 22, cy + chh - 76, cw - 44, 20), C_TEXT2);
    text(L"Settings file", s_tf_cap, R(x0 + 22, cy + chh - 52, cw - 44, 18), C_MUTED);
    text(W(ps3_settings_path()), s_tf_path, R(x0 + 22, cy + chh - 34, cw - 44, 20), C_TEXT2);

    /* Your setup */
    const float x1 = x0 + cw + 20;
    const D2D1_RECT_F c2 = R(x1, cy, cw, chh);
    w_card(c2, L"YOUR SETUP");
    const int pn = preset_now();
    const float pw = cw - 44, segw = (pw - 4) / (float)N_PRESET;
    const D2D1_RECT_F box = R(x1 + 22, cy + 54, pw, SEG_H);
    fill_rr(box, 8, C_BG);
    stroke_rr(box, 8, C_LINE);
    for (int k = 0; k < N_PRESET; k++) {
        const D2D1_RECT_F s = R(x1 + 24 + segw * (float)k, cy + 56, segw, SEG_H - 4);
        if (k == pn) fill_rr(s, 6, C_RED);
        else if (hovered(H_PRESET + k)) fill_rr(s, 6, C_CARD_HI);
        text(W(k_presets[k].name), s_tf_ui, s, k == pn ? 0xFFFFFF : C_TEXT2, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(s, H_PRESET + k);
    }
    text(pn >= 0 ? W(k_presets[pn].desc) : L"Your own mix of game patches (Game patches tab).", s_tf_desc,
         R(x1 + 22, cy + 98, pw, 36), C_TEXT2);
    float ry = cy + 142;
    const char* quick[][2] = { { "display", "mode" }, { "display", "frame_cap" } };
    for (int q = 0; q < 2; q++) {
        Opt* o = opt_find(quick[q][0], quick[q][1]);
        if (!o) continue;
        text(W(o->label), s_tf_ui_b, R(x1 + 22, ry, 140, SEG_H), C_TEXT);
        w_segmented(*o, (int)(o - s_opt), x1 + 22 + 140, ry, pw - 140);
        ry += SEG_H + 12;
    }
    ry += 4;
    if (Opt* vs = opt_find("display", "vsync")) {
        text(L"VSync", s_tf_ui_b, R(x1 + 22, ry, 220, 26), C_TEXT);
        w_switch(opt_on(*vs), x1 + 22 + pw - 46, ry, H_SW + (int)(vs - s_opt));
        ry += 26 + 14;
    }
    if (fps) {
        text(L"Frame rate and load on screen", s_tf_ui_b, R(x1 + 22, ry, 260, 26), C_TEXT);
        w_switch(ps3_settings_ieq(fps->cur, "overlay"), x1 + 22 + pw - 46, ry, H_SW + (int)(fps - s_opt));
    }
}

/* The game's folder: the first row of the System tab. */
static const float GAME_ROW_H = 106.0f;

static void draw_game_row(float x, float y, float w)
{
    const float bw = 116.0f;
    const int busy = s_find.thread != NULL;
    text(L"Game folder", s_tf_ui_b, R(x, y + 16, w - 2 * bw - 30, 22), C_TEXT);
    w_button(L"Browse\x2026", R(x + w - 2 * bw - 10, y + 10, bw, SEG_H), H_GAME_BROWSE, 0);
    w_button(busy ? L"Stop" : L"Auto detect", R(x + w - bw, y + 10, bw, SEG_H), H_GAME_AUTO, 0);
    text(game_path_shown(), s_tf_path, R(x, y + 50, w, 20), C_TEXT2);
    if (busy) {
        wchar_t b[GAME_PATH_MAX + 16];
        AcquireSRWLockShared(&s_find.lock);
        swprintf(b, sizeof b / sizeof b[0], L"Searching: %ls", s_find.now);
        ReleaseSRWLockShared(&s_find.lock);
        text(L"\xE721", s_tf_icon, R(x, y + 74, 22, 26), C_GOLD, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        text(b, s_tf_path, R(x + 32, y + 74, w - 32, 26), C_TEXT2);
    } else {
        wchar_t b[GAME_PATH_MAX + 128];
        const int level = game_status(b, sizeof b / sizeof b[0]);
        check_line(x, y + 74, w, level, b);
    }
}

static void draw_settings(int tab)
{
    const float x0 = SIDE_W + 40, hh = 120.0f;
    draw_hero(hh);
    text(k_tab_name[tab], s_tf_title, R(x0, 30, 600, 44), 0xF3E6D0);
    text(k_tab_desc[tab], s_tf_ui, R(x0 + 2, 74, 640, 24), C_TEXT2);
    const float cw = WIN_W - SIDE_W - 80, cy = hh + 4, rx = x0 + 24, rw = cw - 48;
    /* The rows of the tab, then what follows them. */
    Opt* rows[MAX_POPT + N_OPT];
    int nr = 0;
    if (tab == T_PATCHES) for (int i = 0; i < s_npopt; i++) rows[nr++] = &s_popt[i];
    else for (int i = 0; i < N_OPT; i++) if (s_opt[i].tab == tab) rows[nr++] = &s_opt[i];
    const wchar_t* keys_desc = L"Which key or mouse button plays each PS3 button.";
    float kdh = 0.0f, klh = 0.0f;
    float ch = 16.0f;
    for (int r = 0; r < nr; r++) ch += opt_row_h(*rows[r], rw);
    if (tab == T_CONTROLS) { klh = label_h(keys_desc, rw - 212, &kdh); ch += row_h(klh); }
    if (tab == T_PATCHES) ch += 34.0f;
    if (tab == T_SYSTEM) ch += GAME_ROW_H;
    ch += 16.0f;
    const float view = WIN_H - FOOT_H - 12.0f - cy;
    s_scroll_max[tab] = ch > view ? ch - view : 0.0f;
    if (s_scroll[tab] > s_scroll_max[tab]) s_scroll[tab] = s_scroll_max[tab];
    if (s_scroll[tab] < 0.0f) s_scroll[tab] = 0.0f;
    const float off = s_scroll[tab];
    const int scrolls = s_scroll_max[tab] > 0.0f;
    if (scrolls) {
        s_clip = R(SIDE_W, cy, WIN_W - SIDE_W, view);
        s_rt->PushAxisAlignedClip(s_clip, D2D1_ANTIALIAS_MODE_ALIASED);
    }
    w_card(R(x0, cy - off, cw, ch), NULL);
    float y = cy + 16 - off;
    if (tab == T_SYSTEM) {
        draw_game_row(rx, y, rw);
        y += GAME_ROW_H;
        w_rule(rx, y, rw);
    }
    for (int r = 0; r < nr; r++) {
        Opt& o = *rows[r];
        if (r) w_rule(rx, y, rw);
        y += tab == T_PATCHES ? w_row(o, -1, rx, y, rw, H_PSW + (int)(&o - s_popt))
                              : w_row(o, (int)(&o - s_opt), rx, y, rw, H_SW + (int)(&o - s_opt));
    }
    if (tab == T_CONTROLS) {
        const float rh = row_h(klh);
        w_rule(rx, y, rw);
        w_label(L"Keyboard keys", keys_desc, rx, y, rw - 212, rh, klh, kdh);
        w_button(L"Edit keys\x2026", R(rx + rw - 180, y + (rh - SEG_H) * 0.5f, 180, SEG_H), H_KEYS, 0);
    }
    if (tab == T_PATCHES) {
        w_rule(rx, y, rw);
        text(L"\xE946", s_tf_icon, R(rx, y + 8, 18, 26), C_MUTED, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"Off: drawn as on the PS3. The game reads these when it starts.", s_tf_small,
             R(rx + 26, y + 8, rw - 26, 26), C_MUTED);
    }
    if (scrolls) {
        s_rt->PopAxisAlignedClip();
        s_clip = NO_CLIP;
        /* Where in the card the window is, beside it. */
        const float bx = x0 + cw + 14, thumb = view * view / ch < 40.0f ? 40.0f : view * view / ch;
        fill_rr(R(bx, cy, 6, view), 3, C_CARD);
        fill_rr(R(bx, cy + (view - thumb) * off / s_scroll_max[tab], 6, thumb), 3, C_MUTED);
    }
}

/* An option's short name: its segment's, else its list entry. */
static const char* opt_short(const Opt* o)
{
    if (!o) return "";
    const int i = opt_index(*o, o->cur);
    return i < 0 ? o->cur : o->seg ? o->seg[i] : o->names[i];
}

static void summary(wchar_t* out, size_t n)
{
    const Opt* a = opt_find("graphics", "antialiasing");
    const Opt* c = opt_find("display", "frame_cap");
    const Opt* g = opt_find("graphics", "frame_generation");
    swprintf(out, n, L"%ls  \x00B7  %ls  \x00B7  %ls  \x00B7  %ls",
             W(opt_short(opt_find("display", "mode"))), W(opt_short(opt_find("graphics", "scaling"))),
             a && ps3_settings_ieq(a->cur, "smaa") ? L"SMAA" : L"no AA",
             g && ps3_settings_ieq(g->cur, "on") ? L"frame generation" :
             c && !ps3_settings_ieq(c->cur, "off") ? W(opt_short(c)) : L"no cap");
}

static void draw_footer(void)
{
    const float y = WIN_H - FOOT_H;
    fill(R(SIDE_W, y, WIN_W - SIDE_W, FOOT_H), C_SIDE);
    s_br->SetColor(col(C_LINE));
    s_rt->DrawLine(D2D1::Point2F(SIDE_W, y + 0.5f), D2D1::Point2F(WIN_W, y + 0.5f), s_br, 1.0f);
    wchar_t sum[300];
    summary(sum, 300);
    const int pn = preset_now();
    if (s_toast[0] && GetTickCount64() < s_toast_until) {
        const unsigned c = s_toast_bad ? C_BAD : C_OK;
        text(s_toast_bad ? L"\xE711" : L"\xE73E", s_tf_icon, R(SIDE_W + 32, y, 20, FOOT_H), c, 1.0f,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        text(W(s_toast), s_tf_ui_b, R(SIDE_W + 60, y, 300, FOOT_H), c);
    } else {
        wchar_t p[80];
        if (pn >= 0) swprintf(p, 80, L"%ls preset", W(k_presets[pn].name));
        else swprintf(p, 80, L"Custom patches");
        text(p, s_tf_ui_b, R(SIDE_W + 32, y + 15, 300, 22), C_TEXT);
        text(sum, s_tf_small, R(SIDE_W + 32, y + 37, 300, 20), C_TEXT2);
    }
    w_button(L"Defaults", R(WIN_W - 520, y + 16, 104, 40), H_DEFAULTS, 0);
    w_button(L"Save", R(WIN_W - 406, y + 16, 90, 40), H_SAVE, 0);
    w_button(L"Quit", R(WIN_W - 306, y + 16, 90, 40), H_QUIT, 0);
    w_button(L"PLAY", R(WIN_W - 204, y + 12, 180, 48), H_PLAY, 1);
}

/* The open list, over everything: as many entries as fit, the wheel scrolls. */
static void draw_dropdown(void)
{
    if (s_dd < 0) return;
    Opt& o = s_opt[s_dd];
    const float ih = 32.0f, w = s_dd_at.right - s_dd_at.left;
    int show = o.n;
    const float room = WIN_H - FOOT_H - s_dd_at.bottom - 12;
    if (show * ih > room) show = (int)(room / ih);
    if (show < 4) show = 4 < o.n ? 4 : o.n;
    if (s_dd_top > o.n - show) s_dd_top = o.n - show;
    if (s_dd_top < 0) s_dd_top = 0;
    float y0 = s_dd_at.bottom + 4;
    if (y0 + show * ih > WIN_H - 8) y0 = s_dd_at.top - 4 - show * ih;
    const D2D1_RECT_F box = R(s_dd_at.left, y0, w, show * ih + 8);
    fill_rr(R(box.left + 2, box.top + 4, w, box.bottom - box.top), 10, 0x000000, 0.45f);
    fill_rr(box, 10, C_CARD_HI);
    stroke_rr(box, 10, C_LINE);
    const int sel = opt_index(o, o.cur);
    for (int k = 0; k < show; k++) {
        const int i = s_dd_top + k;
        const D2D1_RECT_F r = R(box.left + 4, y0 + 4 + ih * (float)k, w - 8, ih);
        if (hovered(H_DDITEM + i)) fill_rr(r, 6, 0x34343C);
        if (i == sel) fill_rr(R(r.left + 2, r.top + 8, 3, ih - 16), 1.5f, C_RED);
        text(W(o.names[i]), i == sel ? s_tf_ui_b : s_tf_ui, R(r.left + 14, r.top, r.right - r.left - 20, ih),
             i == sel ? C_TEXT : C_TEXT2);
        hit(r, H_DDITEM + i);
    }
    if (o.n > show) {   /* where the shown part is */
        const float th = (show * ih) * (float)show / (float)o.n;
        const float ty = y0 + 4 + (show * ih - th) * (float)s_dd_top / (float)(o.n - show);
        fill_rr(R(box.right - 6, ty, 3, th), 1.5f, 0x55555E);
    }
}

static void paint(void)
{
    s_nhit = 0;
    s_rt->BeginDraw();
    s_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    s_rt->Clear(col(C_BG));
    if (s_tab == T_PLAY) draw_play();
    else draw_settings(s_tab);
    draw_sidebar();
    draw_footer();
    if (s_dd >= 0) {
        /* Under an open list only the list counts. */
        s_nhit = 0;
        fill(R(0, 0, WIN_W, WIN_H), 0x000000, 0.25f);
        draw_dropdown();
    }
    if (s_rt->EndDraw() == D2DERR_RECREATE_TARGET) {   /* the GPU was reset: a new target, drawn again */
        rt_release();
        if (s_main) InvalidateRect(s_main, NULL, FALSE);
    }
}

/* ---------------------------------------------------------------------------
 * Input
 * -----------------------------------------------------------------------*/
static void toast(const char* s)
{
    snprintf(s_toast, sizeof s_toast, "%s", s);
    s_toast_bad = 0;
    s_toast_until = GetTickCount64() + 2000;
}

static void toast_bad(const char* s)
{
    toast(s);
    s_toast_bad = 1;
    s_toast_until = GetTickCount64() + 4000;
}

/* --- the game's folder -------------------------------------------------------- */
enum { WM_GAME_FOUND = WM_APP + 1 };

static const char* U8(const wchar_t* s)
{
    static char b[2][GAME_PATH_MAX * 3];
    static int k;
    char* o = b[k++ & 1];
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, o, sizeof b[0], NULL, NULL)) o[0] = 0;
    return o;
}

/* [paths], for a copy of the port that no `gowa.py build` wrote them for. */
static void game_paths_fill(void)
{
    int changed = 0;
    const char* vfs = ps3_settings_str("paths", "vfs", "PS3_VFS_ROOT", NULL);
    const char* hdd0 = ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL);
    const char* elf = ps3_settings_str("paths", "elf", NULL, NULL);
    if (!vfs || !*vfs) { ps3_settings_set("paths", "vfs", U8(s_game.vfs)); changed = 1; }
    if (!hdd0 || !*hdd0) { ps3_settings_set("paths", "hdd0", U8(s_game.hdd0)); changed = 1; }
    if (!elf || !*elf || GetFileAttributesA(elf) == INVALID_FILE_ATTRIBUTES) {
        wchar_t p[GAME_PATH_MAX + 32];
        swprintf(p, sizeof p / sizeof p[0], L"%ls\\USRDIR\\GOWA.elf", s_game.link_vfs);
        ps3_settings_set("paths", "elf", U8(p));
        changed = 1;
    }
    if (changed) ps3_settings_save(NULL);
}

/* Both links to a folder, checked before either changes. */
static int game_use(const wchar_t* dir)
{
    wchar_t err[GAME_PATH_MAX + 64] = L"";
    const size_t n = sizeof err / sizeof err[0];
    s_game.note[0] = 0;
    if (!game_link_possible(s_game.link_vfs, dir, err, n) || !game_link_possible(s_game.link_hdd0, dir, err, n) ||
        !game_link(s_game.link_vfs, dir, err, n) || !game_link(s_game.link_hdd0, dir, err, n)) {
        swprintf(s_game.note, sizeof s_game.note / sizeof s_game.note[0], L"%ls", err);
        return 0;
    }
    game_paths_fill();
    game_refresh();
    art_reload();
    return 1;
}

static void find_now(const wchar_t* dir)
{
    AcquireSRWLockExclusive(&s_find.lock);
    swprintf(s_find.now, GAME_PATH_MAX, L"%ls", dir);
    ReleaseSRWLockExclusive(&s_find.lock);
}

static DWORD WINAPI find_thread(LPVOID)
{
    GameSearch s = { s_find.under[0] ? s_find.under : NULL, &s_find.stop, find_now, 60, 0 };
    GameInfo close, best;
    memset(&close, 0, sizeof close);
    if (s.under) game_locate(s.under, &close);   /* the folder itself, or the usual places in it */
    if (close.state >= GAME_NO_ELF) best = close;
    else {
        game_search(&s, &best);
        if (close.state > best.state) best = close;
    }
    s_find.best = best;
    PostMessageA(s_main, WM_GAME_FOUND, 0, 0);
    return 0;
}

static void find_start(const wchar_t* under)
{
    if (s_find.thread) return;
    swprintf(s_find.under, GAME_PATH_MAX, L"%ls", under ? under : L"");
    s_find.stop = 0;
    s_find.now[0] = 0;
    s_game.note[0] = 0;
    s_find.thread = CreateThread(NULL, 0, find_thread, NULL, 0, NULL);
}

static void find_end(void)
{
    if (!s_find.thread) return;
    WaitForSingleObject(s_find.thread, INFINITE);
    CloseHandle(s_find.thread);
    s_find.thread = NULL;
}

/* What the search came to. */
static void find_done(void)
{
    find_end();
    if (s_find.stop) return;
    const GameInfo& b = s_find.best;
    const int picked = s_find.under[0] != 0;
    const size_t n = sizeof s_game.note / sizeof s_game.note[0];
    if (b.state >= GAME_NO_ELF && (picked || b.state >= s_game.g.state)) {
        if (game_use(b.dir)) toast(b.state == GAME_OK ? "Game found" : "Game folder set");
        else toast_bad("The game's folder could not be set");
    } else if (b.state == GAME_OTHER_TITLE) {
        swprintf(s_game.note, n, L"Found %ls in %ls. This port runs NPUA80918 1.12", W(b.title_id), b.dir);
        toast_bad("Another release of the game");
    } else if (picked) {
        swprintf(s_game.note, n, L"No game in %ls", s_find.under);
        toast_bad("No game in that folder");
    } else if (s_game.g.state >= GAME_NO_ELF) {
        toast("No better copy found");
    } else {
        swprintf(s_game.note, n, L"Not found on this PC. Browse to its folder (NPUA80918)");
        toast_bad("Game not found");
    }
}

/* The compiled shaders on disk, both compilers' with their pipelines, to the Recycle Bin. The
 * game builds them again as it reaches them. */
static void clear_shaders(void)
{
    const char* sc = ps3_settings_str("graphics", "shader_cache", "D3D12_SHADER_CACHE", NULL);
    if (!sc || !*sc || ps3_settings_ieq(sc, "off")) {
        toast_bad("No shader cache in the settings");
        return;
    }
    /* Each compiler's pack and journal, what a fold cut short leaves, and the pipelines from before the store. */
    static const wchar_t* const ext[] = { L"", L".journal", L".tmp", L".journal.tmp", L".pso",
                                          L".dxil", L".dxil.journal", L".dxil.tmp", L".dxil.journal.tmp", L".dxil.pso" };
    enum { N = sizeof ext / sizeof ext[0], F = GAME_PATH_MAX + 16 };
    static wchar_t list[N * F + 1];     /* for the shell: one after the other, two zeros at the end */
    wchar_t base[GAME_PATH_MAX];
    if (!GetFullPathNameW(W(sc), GAME_PATH_MAX, base, NULL)) return;
    size_t n = 0;
    unsigned long long bytes = 0;
    for (int i = 0; i < N; i++) {
        wchar_t f[F];
        swprintf(f, F, L"%ls%ls", base, ext[i]);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(f, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        bytes += (unsigned long long)a.nFileSizeHigh << 32 | a.nFileSizeLow;
        wcscpy(list + n, f);
        n += wcslen(f) + 1;
    }
    list[n] = 0;
    if (!n) {
        toast("No compiled shaders to clear");
        return;
    }
    wchar_t m[200];
    swprintf(m, 200, L"Move %.0f MB of compiled shaders to the Recycle Bin?\n\n"
                     L"The game builds them again as it reaches them, so the first run after this is slower.",
             (double)bytes / 1048576.0);
    if (MessageBoxW(s_main, m, L"Clear shaders", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    SHFILEOPSTRUCTW op = {};
    op.hwnd = s_main;
    op.wFunc = FO_DELETE;
    op.pFrom = list;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) toast_bad("Not cleared: is the game running?");
    else toast("Shaders cleared");
}

static void game_browse(void)
{
    if (s_find.thread) {   /* a search under way gives way */
        s_find.stop = 1;
        find_end();
    }
    IFileOpenDialog* d = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(L"The game's folder: NPUA80918, or a folder it is under");
    IShellItem* start = NULL;
    if (s_game.g.state != GAME_NONE && SUCCEEDED(SHCreateItemFromParsingName(s_game.g.dir, NULL, IID_PPV_ARGS(&start)))) {
        d->SetFolder(start);
        start->Release();
    }
    if (SUCCEEDED(d->Show(s_main))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                find_start(p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}

static void act(int id)
{
    if (id >= H_DDITEM) {
        if (s_dd >= 0 && id - H_DDITEM < s_opt[s_dd].n) opt_set(s_opt[s_dd], s_opt[s_dd].values[id - H_DDITEM]);
        s_dd = -1;
        return;
    }
    if (id >= H_PLAY) {
        switch (id) {
        case H_PLAY:
            if (!s_rc.elf_ok) {
                s_tab = T_SYSTEM;
                toast_bad("Set the game's folder first");
                break;
            }
            /* The second link, if only the first was made. */
            if (s_game.g.state >= GAME_NO_ELF && GetFileAttributesW(s_game.link_hdd0) == INVALID_FILE_ATTRIBUTES)
                game_link(s_game.link_hdd0, s_game.g.dir, NULL, 0);
            if (save_all()) s_done = 1;
            break;
        case H_SAVE: if (save_all()) toast("Saved"); break;
        case H_QUIT: s_done = 2; break;
        case H_KEYS: if (!s_keys) open_keys(); break;
        case H_SAVES: open_saves(); break;
        case H_SHADERS: clear_shaders(); break;
        case H_GAME_BROWSE: game_browse(); break;
        case H_GAME_AUTO:
            if (s_find.thread) s_find.stop = 1;
            else find_start(NULL);
            break;
        case H_DEFAULTS:
            for (int i = 0; i < N_OPT; i++) opt_set(s_opt[i], s_opt[i].def);
            for (int i = 0; i < s_npopt; i++) opt_set(s_popt[i], s_popt[i].def);
            toast("Defaults back: Save or Play keeps them");
            break;
        }
        return;
    }
    if (id >= H_PRESET) { preset_apply(id - H_PRESET); return; }
    if (id >= H_PSW) { Opt& o = s_popt[id - H_PSW]; opt_set(o, opt_on(o) ? "off" : "on"); return; }
    if (id >= H_DD) {
        s_dd = id - H_DD;
        for (int i = 0; i < s_nhit; i++) if (s_hit[i].id == id) s_dd_at = s_hit[i].r;
        const int sel = opt_index(s_opt[s_dd], s_opt[s_dd].cur);
        s_dd_top = sel > 3 ? sel - 3 : 0;
        return;
    }
    if (id >= H_SW) {
        Opt& o = s_opt[id - H_SW];
        if (!strcmp(o.key, "show_fps")) opt_set(o, ps3_settings_ieq(o.cur, "overlay") ? "title" : "overlay");
        else opt_set(o, o.values[opt_on(o) ? 0 : 1]);
        return;
    }
    if (id >= H_SEG) { Opt& o = s_opt[(id - H_SEG) / 16]; opt_set(o, o.values[(id - H_SEG) % 16]); return; }
    if (id >= H_NAV) { s_tab = id - H_NAV; return; }
}

static int hit_at(float x, float y)
{
    for (int i = s_nhit - 1; i >= 0; i--) if (inside(s_hit[i].r, x, y)) return s_hit[i].id;
    return -1;
}

static void redraw(void) { InvalidateRect(s_main, NULL, FALSE); }

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* Size the window for a DPI on the monitor a rectangle is on, smaller when
 * its work area is: 1120 x 720 at 150% is more than a 1080p screen holds.
 * Centred, or at the rectangle's corner kept on the screen. */
static void place(HWND h, int dpi, const RECT* near_rc, int centre)
{
    typedef BOOL (WINAPI *adjust_fn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    HMODULE user = GetModuleHandleA("user32.dll");
    adjust_fn adjust = user ? (adjust_fn)(void*)GetProcAddress(user, "AdjustWindowRectExForDpi") : NULL;
    const DWORD style = (DWORD)GetWindowLongA(h, GWL_STYLE), ex = (DWORD)GetWindowLongA(h, GWL_EXSTYLE);
    RECT nc = { 0, 0, 0, 0 };
    if (!adjust || !adjust(&nc, style, FALSE, ex, (UINT)dpi)) AdjustWindowRectEx(&nc, style, FALSE, ex);
    const int ncw = nc.right - nc.left, nch = nc.bottom - nc.top;
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(MonitorFromRect(near_rc, MONITOR_DEFAULTTONEAREST), &mi))
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    const int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    float ui = (float)dpi;
    const float fw = (float)(aw - ncw - 16) * 96.0f / WIN_W, fh = (float)(ah - nch - 16) * 96.0f / WIN_H;
    if (fw < ui) ui = fw;
    if (fh < ui) ui = fh;
    if (ui < 60.0f) ui = 60.0f;
    s_dpi = dpi;
    s_ui = ui;
    const int ww = (int)(WIN_W * ui / 96.0f + 0.5f) + ncw, wh = (int)(WIN_H * ui / 96.0f + 0.5f) + nch;
    int x = centre ? mi.rcWork.left + (aw - ww) / 2 : near_rc->left;
    int y = centre ? mi.rcWork.top + (ah - wh) / 2 : near_rc->top;
    if (x + ww > mi.rcWork.right) x = mi.rcWork.right - ww;
    if (y + wh > mi.rcWork.bottom) y = mi.rcWork.bottom - wh;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(h, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        if (!s_rt) {
            RECT rc;
            GetClientRect(h, &rc);
            ID2D1HwndRenderTarget* hrt = NULL;
            const float dpi = s_ui;
            if (SUCCEEDED(s_d2d->CreateHwndRenderTarget(
                    D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                 D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                 dpi, dpi),
                    D2D1::HwndRenderTargetProperties(h, D2D1::SizeU((UINT32)(rc.right - rc.left), (UINT32)(rc.bottom - rc.top))),
                    &hrt))) {
                s_rt = hrt;
                rt_resources();
            }
        }
        if (s_rt) {
            paint();
            /* What is under the mouse now that the window has changed under it. */
            const int now = s_mx >= 0 ? hit_at(s_mx, s_my) : -1;
            if (now != s_hover) { s_hover = now; redraw(); }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DPICHANGED:
        place(h, HIWORD(w), (const RECT*)l, 0);
        rt_release();
        redraw();
        return 0;
    case WM_SIZE:
        if (s_rt && LOWORD(l) && HIWORD(l))
            static_cast<ID2D1HwndRenderTarget*>(s_rt)->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        s_mx = (float)(short)LOWORD(l) * 96.0f / s_ui;
        s_my = (float)(short)HIWORD(l) * 96.0f / s_ui;
        const int now = hit_at(s_mx, s_my);
        if (now != s_hover) { s_hover = now; redraw(); }
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        s_mx = s_my = -1;
        if (s_hover != -1) { s_hover = -1; redraw(); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorA(NULL, s_hover >= 0 ? (LPCSTR)IDC_HAND : (LPCSTR)IDC_ARROW));
            return TRUE;
        }
        break;
    /* A click counts where it was pressed and released, whatever moves came before (touch, pens). */
    case WM_LBUTTONDOWN:
        s_mx = (float)(short)LOWORD(l) * 96.0f / s_ui;
        s_my = (float)(short)HIWORD(l) * 96.0f / s_ui;
        s_press = hit_at(s_mx, s_my);
        if (s_dd >= 0 && s_press < 0) { s_dd = -1; redraw(); }   /* a click outside closes the list */
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        s_mx = (float)(short)LOWORD(l) * 96.0f / s_ui;
        s_my = (float)(short)HIWORD(l) * 96.0f / s_ui;
        const int at = hit_at(s_mx, s_my);
        if (s_press >= 0 && s_press == at) { act(s_press); redraw(); }
        s_press = -1;
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (s_dd >= 0) {
            s_dd_top += GET_WHEEL_DELTA_WPARAM(w) > 0 ? -2 : 2;
            redraw();
        } else if (s_tab != T_PLAY && s_scroll_max[s_tab] > 0.0f) {
            s_scroll[s_tab] -= (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 60.0f;   /* clamped as it is drawn */
            redraw();
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && s_dd >= 0) { s_dd = -1; redraw(); return 0; }
        if (w == VK_RETURN && s_dd < 0) { act(H_PLAY); return 0; }
        if (w == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) { act(H_SAVE); redraw(); return 0; }
        if (w == VK_TAB || w == VK_NEXT || w == VK_PRIOR) {   /* tabs by keyboard */
            const int back = w == VK_PRIOR || (w == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000));
            s_tab = (s_tab + (back ? T_N - 1 : 1)) % T_N;
            s_dd = -1;
            redraw();
            return 0;
        }
        break;
    case WM_GAME_FOUND:
        find_done();
        redraw();
        return 0;
    case WM_TIMER: {
        if (s_toast[0] && GetTickCount64() >= s_toast_until) { s_toast[0] = 0; redraw(); }   /* the toast goes */
        if (s_find.thread) redraw();   /* the folder it is in */
        static int tick;
        if (++tick % 4 == 0) {   /* every 2 s: a controller plugged in or out */
            const int pad = pad_connected();
            if (pad != s_rc.pad) { s_rc.pad = pad; redraw(); }
        }
        return 0;
    }
    case WM_CLOSE:
        s_done = 2;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* GOWA_LAUNCHER_SHOT=<folder>: every tab, the Display tab with its list open,
 * the System tab during a search, and the Cheats and Graphics tabs scrolled to
 * their end, into PNG files at the window's size times 1.25 (a 125% display). */
static void shots(const char* dir)
{
    const float dpi = 120.0f;
    const UINT pw = (UINT)(WIN_W * dpi / 96.0f), ph = (UINT)(WIN_H * dpi / 96.0f);
    IWICBitmap* bmp = NULL;
    if (!s_wic || FAILED(s_wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return;
    if (FAILED(s_d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi), &s_rt))) {
        bmp->Release();
        return;
    }
    rt_resources();
    CreateDirectoryA(dir, NULL);
    for (int t = 0; t <= T_N + 3; t++) {
        s_tab = t < T_N ? t : t == T_N ? T_DISPLAY : t == T_N + 1 ? T_SYSTEM : t == T_N + 2 ? T_CHEATS : T_GRAPHICS;
        s_scroll[s_tab] = t >= T_N + 2 ? 1e9f : 0.0f;   /* the last two: the cheats and the graphics, scrolled to their end */
        s_dd = -1;
        s_hover = -1;
        if (t == T_N + 1) {   /* a search under way: nothing runs, the row only says so */
            s_find.thread = GetCurrentThread();
            swprintf(s_find.now, GAME_PATH_MAX, L"D:\\Games\\PS3\\RPCS3\\dev_hdd0\\game");
        }
        if (t == T_N) {   /* the window size list, open */
            paint();
            for (int i = 0; i < s_nhit; i++)
                if (s_hit[i].id == H_DD + (int)(opt_find("display", "window_size") - s_opt)) {
                    s_dd = s_hit[i].id - H_DD;
                    s_dd_at = s_hit[i].r;
                }
        }
        paint();
        char path[1200];
        snprintf(path, sizeof path, "%s\\launcher_%d.png", dir, t);
        IWICStream* st = NULL;
        IWICBitmapEncoder* enc = NULL;
        IWICBitmapFrameEncode* fr = NULL;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(s_wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(W(path), GENERIC_WRITE)) &&
            SUCCEEDED(s_wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) &&
            SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, NULL)) &&
            SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->SetSize(pw, ph)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
            SUCCEEDED(fr->WriteSource(bmp, NULL)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
            printf("[launcher] %s\n", path);
        if (fr) fr->Release();
        if (enc) enc->Release();
        if (st) st->Release();
    }
    s_find.thread = NULL;
    rt_release();
    bmp->Release();
}

static void ui_release(void)
{
    rt_release();
    IUnknown* all[] = { s_tf_ui, s_tf_ui_b, s_tf_small, s_tf_cap, s_tf_head, s_tf_title, s_tf_play, s_tf_icon,
                        s_tf_desc, s_tf_path, s_img_bg, s_img_logo, s_wic, s_dw, s_d2d };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) if (all[i]) all[i]->Release();
    s_tf_ui = s_tf_ui_b = s_tf_small = s_tf_cap = s_tf_head = s_tf_title = s_tf_play = s_tf_icon = s_tf_desc = s_tf_path = NULL;
    s_img_bg = s_img_logo = NULL;
    s_wic = NULL;
    s_dw = NULL;
    s_d2d = NULL;
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

    for (int i = 0; i < N_OPT; i++) opt_set(s_opt[i], ps3_settings_str(s_opt[i].section, s_opt[i].key, NULL, s_opt[i].def));
    popt_init();
    for (int i = 0; i < s_npopt; i++)
        opt_set(s_popt[i], ps3_settings_str(s_popt[i].section, s_popt[i].key, NULL, s_popt[i].def));
    ready_check();

    /* COM for WIC, given back before the game starts (its audio takes it multithreaded). */
    const HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const char* shot_dir = getenv("GOWA_LAUNCHER_SHOT");
    if (!ui_init()) {
        printf("[launcher] no Direct2D or DirectWrite\n");
        ui_release();
        if (SUCCEEDED(com)) CoUninitialize();
        return shot_dir ? 0 : 1;   /* no Direct2D: just play */
    }
    if (shot_dir) {
        shots(shot_dir);
        ui_release();
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof wc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
    wc.lpfnWndProc = kb_proc;
    wc.lpszClassName = "gowa_keys";
    RegisterClassExA(&wc);
    wc.hbrBackground = NULL;
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = "gowa_launcher";
    RegisterClassExA(&wc);

    s_main = CreateWindowExA(0, "gowa_launcher", "God of War: Ascension (GowARecomp)",
                             WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                             S((int)WIN_W), S((int)WIN_H), NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!s_main) {
        ui_release();
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;   /* no window: just play */
    }
    {
        /* The DPI of the monitor it opened on (the exe is per-monitor aware). */
        typedef UINT (WINAPI *dpi_win_fn)(HWND);
        HMODULE user = GetModuleHandleA("user32.dll");
        dpi_win_fn dpi_win = user ? (dpi_win_fn)(void*)GetProcAddress(user, "GetDpiForWindow") : NULL;
        const int dpi = dpi_win ? (int)dpi_win(s_main) : s_dpi;
        RECT wr;
        GetWindowRect(s_main, &wr);
        place(s_main, dpi >= 96 ? dpi : 96, &wr, 1);
    }
    dark_caption(s_main);
    ShowWindow(s_main, SW_SHOW);
    SetForegroundWindow(s_main);
    SetTimer(s_main, 1, 500, NULL);
    if (!s_rc.elf_ok) find_start(NULL);   /* no game yet: look for it */

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
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    KillTimer(s_main, 1);
    s_find.stop = 1;
    find_end();
    if (s_keys) DestroyWindow(s_keys);
    DestroyWindow(s_main);
    /* Let the destroy messages through before the game opens its window. */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    ui_release();
    if (SUCCEEDED(com)) CoUninitialize();
    return s_done == 1;
}
#else
extern "C" int gowa_launcher(void) { return 1; }   /* no window here: just play */
#endif
