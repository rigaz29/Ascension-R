/*
 * Ascension-R -- the launcher
 *
 * The settings window shown before the game starts. It reads and writes
 * ascension-r.ini through ps3_settings, so what it shows is what the game will use:
 * Play saves and starts, Save saves, Defaults puts this port's defaults back
 * in the window (saved on Save or Play), Quit leaves without starting.
 * launcher.show = off skips it; holding Shift while the game starts brings it
 * back.
 *
 * The window is drawn with Direct2D and DirectWrite: a sidebar
 * of tabs (Play, Graphics, Display, Controls, Game patches, Cheats, Mods, Trophies, Multiplayer, System), a
 * header with the precompile screen's scene moving in it (loading_screen.cpp,
 * all of it this port's own), cards of settings as segmented buttons, switches
 * and lists, and a bar with Play. Window style (Display tab, launcher.style) is
 * Clean by default: Windows' Bahnschrift throughout, Windows' vector icons,
 * smooth rounded shapes, sharp at every Windows scale. Retro is the first look,
 * 8-bit and neon: Silkscreen from the exe (app.rc) for the headings, pixel
 * icons, bevels, and Retro effects (its glow and scanlines) as set.
 * The keyboard keys are set on a sheet over the window (Controls, Edit keys).
 * GOWA_LAUNCHER_SHOT=<folder> draws every tab into PNG files there and quits,
 * to look at the window without opening it; GOWA_LAUNCHER_SHOT_DPI sets their
 * DPI (120, a 125% display, by default).
 *
 * The System tab shows the game's folder: Browse picks it, Auto detect looks
 * for it on every disk (game_find.cpp), and a window that opens with no game
 * starts looking by itself. Either sets the two links the port reads it through.
 *
 * Report a problem, down the sidebar, makes the zip of the last sessions (report.cpp); a window that
 * opens after a session that did not end normally offers it.
 * Clear shaders, below it, moves the compiled shaders to the Recycle Bin. The
 * vertex shaders are built by the game itself before its first start of each
 * version (the precompile gate, loading_screen.cpp), so there is no button for
 * them here. The pixel shaders are built by Build all shaders now, below Clear
 * shaders (precompile.cpp), never while the game plays; it turns
 * graphics.shader_prebuild on, with which the game takes them.
 *
 * This file also holds this port's defaults (gowa_port_defaults), so the
 * game and the Defaults button take them from the same tables.
 */
#include "win32_compat.h"
#include "ps3emu/ps3_settings.h"
#include "game_find.h"
#include "i18n.h"
#include <stdarg.h>
#include "game_prepare.h"
#include "game_title.h"
#include "report.h"
#include "version.h"
#include "update_check.h"
#include "loading_screen.h"   /* the header's scene and the pixel font */
#include "trophies.h"         /* the Trophies tab */
#include "mods.h"             /* the Mods tab */
#include "anr_get.h"          /* ANR FG from its own releases, when the player says so */
#include "dlss_get.h"         /* NVIDIA's DLSS DLL from NVIDIA's own repository, when the player says so */
extern "C" void rsx_d3d12_backend_open_on(int x, int y);   /* rsx_d3d12_backend.h: the game's window opens there */
extern "C" int  rsx_d3d12_prebuilt_current(const char* cache);   /* rsx_d3d12_backend.h: the pixel shaders' file */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#ifdef _WIN32
#include <commctrl.h>   /* TASKDIALOGCONFIG: the offers after a session that did not end well */
#include <shellapi.h>   /* ShellExecuteA: Open save folder */
#include <objbase.h>
#include <shobjidl.h>   /* IFileOpenDialog: Browse */
#include <d2d1_1.h>
#include <d2d1effects.h>   /* the glow's blur */
#include <dwrite_3.h>      /* fonts read from memory */
#include <wincodec.h>
#include <dxgi1_6.h>   /* IDXGIFactory6: which GPU is which */
#include <dwmapi.h>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")   /* CLSID_D2D1GaussianBlur */
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
extern "C" const char* ps3_pad_key_name(int vk);   /* libs/input/cellPad.c */
#endif

extern "C" void gowa_port_defaults(void);
extern "C" int  gowa_launcher(void);   /* 1 = play, 0 = quit */
/* patches.cpp: the title patches this build has (scripts/title_patches.json). */
extern "C" int  gowa_title_patch_count(void);
extern "C" int cellSysutil_language_of(const char* value);   /* the runtime's: a system.language value's language */
extern "C" const char* gowa_title_patch_id(int i);
extern "C" const char* gowa_title_patch_name(int i);
extern "C" int  gowa_title_patch_default(int i);
/* multiplayer.cpp: the Multiplayer tab's checks, the addresses players reach this PC at, and Play's start of it. */
extern "C" int  gowa_online_name_ok(const char* name);
extern "C" int  gowa_online_address_ok(const char* s);
extern "C" int  gowa_online_nodes_ok(const char* s);
extern "C" const char* gowa_online_problem(void);
extern "C" int  gowa_online_start(char* why, size_t n);
extern "C" void gowa_online_lan(char* out, size_t n);
extern "C" void gowa_online_begin(void* notify, unsigned msg);   /* Auto: the node, on a thread of its own */
extern "C" int  gowa_online_status(char* why, size_t n);         /* 0 none, 1 running, 2 starting, -1 failed */
extern "C" void gowa_online_players(char* out, size_t n);       /* the node's players.txt */

/* ---------------------------------------------------------------------------
 * What the window offers
 * -----------------------------------------------------------------------*/
enum { T_PLAY, T_GRAPHICS, T_DISPLAY, T_CONTROLS, T_PATCHES, T_CHEATS, T_MODS, T_TROPHIES, T_ONLINE, T_SYSTEM, T_N };

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
    int text;                  /* a box typed into, this many characters at most (the Multiplayer tab's); else 0 */
    char cur[64];              /* the value shown, kept as written */
};

static const char* const k_onoff[] = { "off", "on" };
/* display.hdr_peak and display.hdr_paper_white (ps3recomp's hdr_start), in nits. */
static const char* const k_hdrpk[] = { "auto", "400", "600", "800", "1000", "1500", "2000" };
static const char* const k_hdrpk_n[] = { "What the display reports", "400 nits", "600 nits", "800 nits", "1000 nits",
                                         "1500 nits", "2000 nits" };
static const char* const k_hdrpk_s[] = { "Auto", "400", "600", "800", "1000", "1500", "2000" };
static const char* const k_hdrpw[] = { "100", "150", "200", "250", "300" };
static const char* const k_hdrpw_n[] = { "100 nits", "150 nits", "200 nits", "250 nits", "300 nits" };
static const char* const k_mode[] = { "windowed", "borderless" };
static const char* const k_mode_n[] = { "Windowed", "Borderless fullscreen" };
static const char* const k_mode_s[] = { "Windowed", "Borderless" };
static const char* const k_size[] = { "", "1280x720", "1600x900", "1920x1080", "2560x1080", "2560x1440", "3440x1440",
                                      "3840x2160", "5120x1440" };
static const char* const k_size_n[] = { "Default (720p x Windows scale)", "1280 x 720", "1600 x 900", "1920 x 1080",
                                        "2560 x 1080 (21:9)", "2560 x 1440", "3440 x 1440 (21:9)", "3840 x 2160",
                                        "5120 x 1440 (32:9)" };
static const char* const k_fps[] = { "off", "title", "overlay" };
static const char* const k_fps_n[] = { "Off", "In the title bar", "On screen, with CPU/GPU load" };
static const char* const k_fps_s[] = { "Off", "Title bar", "On screen" };
static const char* const k_aniso[] = { "game", "2", "4", "8", "16" };
static const char* const k_aniso_n[] = { "As the game sets it", "2x", "4x", "8x", "16x" };
static const char* const k_aniso_s[] = { "Game", "2x", "4x", "8x", "16x" };
static const char* const k_async[] = { "0", "2" };
static const char* const k_swait[] = { "off", "short", "full" };
static const char* const k_swait_n[] = { "Off: never, a gap for a moment instead", "Short: a frame waits 33 ms at most",
                                         "Full: up to two seconds" };
static const char* const k_swait_s[] = { "Off", "Short", "Full" };
/* launcher.language (i18n.cpp): each language by its own name. */
static const char* const k_uilang[] = { "auto", "en", "id", "zh-s", "zh-t", "ru", "es", "pt-br", "de" };
static const char* const k_uilang_n[] = { "Windows language", "English", "Bahasa Indonesia", "\u7B80\u4F53\u4E2D\u6587",
                                          "\u7E41\u9AD4\u4E2D\u6587", "\u0420\u0443\u0441\u0441\u043A\u0438\u0439",
                                          "Espa\u00F1ol", "Portugu\u00EAs (Brasil)", "Deutsch" };
static const char* const k_style[] = { "clean", "retro" };
static const char* const k_style_n[] = { "Clean", "Retro: pixel font and 8-bit bevels" };
static const char* const k_style_s[] = { "Clean", "Retro" };
static const char* const k_cap[] = { "off", "20", "30", "120", "144", "unlocked" };
static const char* const k_cap_n[] = { "Off: the game's own 60", "20 fps, even pace", "30 fps, even pace",
                                       "120 fps (experimental)", "144 fps (experimental)", "Unlocked (experimental)" };
static const char* const k_cap_s[] = { "Off", "20 fps", "30 fps", "120 fps", "144 fps", "Unlocked" };
static const char* const k_scale[] = { "bilinear", "sharp", "fsr1", "fsr3", "dlss" };
static const char* const k_scale_n[] = { "Bilinear", "Sharp bilinear", "AMD FSR 1", "AMD FSR", "NVIDIA DLSS (RTX cards)" };
static const char* const k_scale_s[] = { "Bilinear", "Sharp", "FSR 1", "FSR", "DLSS" };
/* graphics.dlss_preset: auto leaves each DLSS mode the DLL's own preset; a letter asks every mode for it (rsx_dlss.cpp). */
static const char* const k_dlssp[] = { "auto", "k", "l", "m", "j" };
static const char* const k_dlssp_n[] = { "Auto: the DLL's own for each mode", "K", "L", "M", "J" };
static const char* const k_dlssp_s[] = { "Auto", "K", "L", "M", "J" };
/* graphics.fsr_version: auto lets the FSR API take the newest the card has; a version pins it (rsx_fsr3.cpp). */
static const char* const k_fsrv[] = { "auto", "3.1" };
static const char* const k_fsrv_n[] = { "Auto: the newest the graphics card takes", "FSR 3.1" };
static const char* const k_fsrv_s[] = { "Auto", "3.1" };
static const char* const k_gpu[] = { "discrete", "integrated" };
static const char* const k_gpu_n[] = { "Discrete", "Integrated" };
static const char* const k_rscale[] = { "1", "1.5", "2", "3", "native" };
static const char* const k_rscale_n[] = { "720p, as the game draws it", "1080p", "1440p", "2160p", "The window's size" };
static const char* const k_rscale_s[] = { "720p", "1080p", "1440p", "4K", "Window" };
/* graphics.shadow_resolution and graphics.effects_resolution (ps3recomp's rs_dims). */
static const char* const k_shres[] = { "1", "2", "4" };
static const char* const k_shres_n[] = { "As the game draws them", "2x", "4x" };
static const char* const k_shres_s[] = { "Game's", "2x", "4x" };
static const char* const k_fxres[] = { "game", "internal" };
static const char* const k_fxres_n[] = { "As the game draws them", "The internal resolution" };
static const char* const k_fxres_s[] = { "Game's", "Internal" };
/* graphics.fov (ultrawide.cpp): the play camera's view, a factor of the game's. */
static const char* const k_fov[] = { "0.8", "0.9", "1", "1.1", "1.2", "1.3", "1.4" };
static const char* const k_fov_n[] = { "80%", "90%", "The game's", "110%", "120%", "130%", "140%" };
/* graphics.ambient_occlusion (ps3recomp's ao_draw). */
static const char* const k_ao[] = { "off", "on", "high" };
static const char* const k_ao_n[] = { "Off", "On", "High quality" };
static const char* const k_ao_s[] = { "Off", "On", "High" };
/* graphics.frame_generation: anr has ANR FG's DLL make the frames in FSR's swap chain (beta, PORT_ANR). dlss, NVIDIA
 * DLSS Frame Generation through Streamline (ps3recomp's rsx_sl.cpp), is turned off (main.cpp; D3D12_DLSSG_TEST=1
 * for tests) and has no choice here; the code that fetches and checks its DLL stays for then. */
static const char* const k_fg[] = { "off", "on", "anr" };
static const char* const k_fg_n[] = { "Off", "AMD FSR", "ANR FG (beta)" };
static const char* const k_fg_s[] = { "Off", "AMD FSR", "ANR (beta)" };
/* graphics.reflex: NVIDIA Reflex through Streamline (ps3recomp's rsx_sl.cpp), on NVIDIA GPUs. */
static const char* const k_reflex[] = { "off", "on", "boost" };
static const char* const k_reflex_n[] = { "Off", "On", "On + Boost" };
static const char* const k_aa[] = { "off", "smaa" };
static const char* const k_aa_n[] = { "Off", "SMAA 1x" };
static const char* const k_aa_s[] = { "Off", "SMAA" };
static const char* const k_sharp[] = { "off", "low", "medium", "high", "max" };
static const char* const k_sharp_n[] = { "Off", "Low", "Medium", "High (AMD's)", "Max" };
/* graphics.cas (ps3recomp's post_final): the same levels, CAS's. */
static const char* const k_cas_n[] = { "Off", "Low", "Medium", "High", "Max" };
static const char* const k_sharp_s[] = { "Off", "Low", "Medium", "High", "Max" };
static const char* const k_grade[] = { "off", "low", "medium", "high", "max" };       /* vibrance, contrast */
static const char* const k_grade_n[] = { "Off", "Low", "Medium", "High", "Max" };
static const char* const k_taken[] = { "1", "0.5", "0.25", "0" };
static const char* const k_taken_n[] = { "Normal", "Half", "Quarter", "None" };
static const char* const k_dealt[] = { "1", "2", "5", "kill" };
static const char* const k_dealt_n[] = { "Normal", "2x", "5x", "One hit" };
static const char* const k_dev[] = { "auto", "pad", "keyboard" };
static const char* const k_dev_n[] = { "Controller and keyboard", "Controller only", "Keyboard only" };
static const char* const k_dev_s[] = { "Auto", "Controller", "Keyboard" };
static const char* const k_api[] = { "auto", "xinput", "sdl3" };
static const char* const k_api_n[] = { "XInput, else SDL3", "XInput only", "SDL3 for every controller" };
static const char* const k_api_s[] = { "Auto", "XInput", "SDL3" };
static const char* const k_dz[] = { "0.10", "0.15", "0.20", "0.24", "0.30", "0.35", "0.40" };
static const char* const k_dz_n[] = { "10%", "15%", "20%", "24% (XInput's)", "30%", "35%", "40%" };
static const char* const k_enter[] = { "cross", "circle" };
static const char* const k_enter_n[] = { "Cross", "Circle" };
static const char* const k_bak[] = { "0", "3", "5", "10" };
static const char* const k_bak_n[] = { "Off", "Keep 3", "Keep 5", "Keep 10" };
static const char* const k_bak_s[] = { "Off", "3", "5", "10" };
static const char* const k_music[] = { "off", "low", "normal", "high" };
static const char* const k_music_n[] = { "Off", "Low", "Normal", "High" };
static const char* const k_spk[] = { "auto", "stereo", "5.1", "7.1" };
static const char* const k_spk_n[] = { "As Windows has the output device", "Stereo", "5.1", "7.1" };
static const char* const k_spk_s[] = { "Auto", "Stereo", "5.1", "7.1" };
static const char* const k_lang[] = { "auto", "en", "en-gb", "fr", "es", "de", "it", "nl", "pt", "pt-br",
                                      "ru", "ja", "ko", "zh-t", "zh-s", "fi", "sv", "da", "no", "pl", "tr" };
static const char* const k_lang_n[] = { "Windows language", "English (US)", "English (UK)", "French", "Spanish",
                                        "German", "Italian", "Dutch", "Portuguese", "Portuguese (Brazil)",
                                        "Russian", "Japanese", "Korean", "Chinese (traditional)",
                                        "Chinese (simplified)", "Finnish", "Swedish", "Danish", "Norwegian",
                                        "Polish", "Turkish" };

/* online.mode (multiplayer.cpp). */
static const char* const k_omode[] = { "off", "auto", "host", "join" };
static const char* const k_omode_n[] = { "Off", "Auto: find players", "Host: this PC", "Join a host" };
static const char* const k_omode_s[] = { "Off", "Auto", "Host", "Join" };

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
      "20 and 30: an even pace instead of the highest rate the PC reaches. 120, 144 and Unlocked (experimental): the "
      "game runs past its 60 at its own speed, as far as the PC and, with VSync, the monitor take it; frame generation "
      "is off with them, and the game goes back to 60 once it goes online for multiplayer. With frame generation, off "
      "follows the monitor." },
    { T_DISPLAY, "display", "show_fps", "title", "Frame rate display", 0, LIST(k_fps, k_fps_n), k_fps_s,
      "On screen shows FPS, frame time, the CPU's, GPU's, PPU's, SPU's and RSX's load and a graph of the last "
      "frames' times, in the top right corner." },
    { T_DISPLAY, "display", "ultrawide", "off", "Ultrawide", 1, SW,
      "Draws the game to the full width of a wider screen. The HUD, menus and movies stay 16:9. Takes effect at the next start." },
    { T_DISPLAY, "display", "shader_hint", "on", "Shader build indicator", 1, SW,
      "A spinner in the bottom left corner while the game's new shaders are built, as RPCS3 shows." },
    { T_DISPLAY, "display", "hdr", "off", "HDR", 1, SW,
      "HDR10 on a display Windows has HDR on for (Settings, System, Display, Use HDR): the game's picture with its "
      "highlights raised toward the peak below, the rest at paper white. Not with frame generation. Takes effect at "
      "the next start." },
    { T_DISPLAY, "display", "hdr_peak", "auto", "HDR peak brightness", 0, LIST(k_hdrpk, k_hdrpk_n), k_hdrpk_s,
      "How bright the brightest highlights get: fire, the sky, lightning. Auto takes what the display reports." },
    { T_DISPLAY, "display", "hdr_paper_white", "200", "HDR paper white", 0, LIST(k_hdrpw, k_hdrpw_n), k_hdrpw_n,
      "How bright white is in the rest of the picture and the HUD. Raise it in a bright room." },
    { T_DISPLAY, "launcher", "style", "clean", "Window style", 0, LIST(k_style, k_style_n), k_style_s,
      "How this window looks: Clean, smooth at every Windows scale, or Retro, a pixel font and 8-bit bevels. The game "
      "is not changed." },
    { T_DISPLAY, "launcher", "effects", "on", "Retro effects", 1, SW,
      "Scanlines and a neon glow in this window, with the Retro window style. The game is not changed." },
    { T_GRAPHICS, "graphics", "gpu", "discrete", "Graphics card", 0, LIST(k_gpu, k_gpu_n), k_gpu_n, NULL },   /* gpu_detect */
    { T_GRAPHICS, "graphics", "render_scale", "1", "Internal resolution", 0, LIST(k_rscale, k_rscale_n), k_rscale_s,
      "What the scene is drawn at. Shadow resolution and Effects resolution below say how large shadows, bloom and "
      "blur are drawn. AMD FSR and frame generation go no higher than the window." },
    { T_GRAPHICS, "graphics", "shadow_resolution", "1", "Shadow resolution", 0, LIST(k_shres, k_shres_n), k_shres_s,
      "Shadow maps drawn larger: finer shadow edges and less stair-stepping, most of all at a high internal "
      "resolution; where the game's shadows are soft the change is small. 4x takes about 120 MB more video memory. "
      "Takes effect at the next start." },
    { T_GRAPHICS, "graphics", "effects_resolution", "game", "Effects resolution", 0, LIST(k_fxres, k_fxres_n), k_fxres_s,
      "Bloom, blur and depth of field drawn at the internal resolution, not at the game's 640x360 and smaller: "
      "smoother blurred backgrounds and glow at 1440p and 4K, the game's brightness kept. Only with an internal "
      "resolution above 720p. Takes effect at the next start." },
    { T_GRAPHICS, "graphics", "fov", "1", "Field of view", 0, LIST(k_fov, k_fov_n), NULL,
      "How wide the view is in play, against the game's: wider shows more around Kratos, narrower brings the view in. "
      "The in-engine cinematics keep the game's own. Takes effect at the next start." },
    { T_GRAPHICS, "graphics", "ambient_occlusion", "off", "Ambient occlusion", 0, LIST(k_ao, k_ao_n), k_ao_s,
      "Soft shade in corners, gaps and where things meet the ground, for a deeper picture, worked out from the "
      "scene's depth before the HUD goes on. High draws it at the internal resolution, On at half of it. Off with "
      "an ultrawide picture. Takes effect at the next start." },
    { T_GRAPHICS, "graphics", "scaling", "fsr1", "Scaling", 0, LIST(k_scale, k_scale_n), k_scale_s,
      "How the picture is scaled to the window. AMD FSR builds the detail from past frames, "
      "in place of SMAA, and FSR 1 takes over where there is no scene. NVIDIA DLSS does the same with NVIDIA's model "
      "on RTX cards (beta), its mode set by the internal resolution against the window (DLAA when they match); "
      "its DLL comes from NVIDIA's own GitHub when you pick it." },
    { T_GRAPHICS, "graphics", "fsr_version", "auto", "FSR version", 0, LIST(k_fsrv, k_fsrv_n), k_fsrv_s,
      "Auto takes the newest the card and its driver have: FSR 4.1 on AMD Radeon RX 6000 and newer (RX 6000: "
      "driver 26.10.1 or later), else FSR 3.1. 4.1 gives cleaner edges and thin lines; 3.1 keeps textures a little "
      "sharper and is lighter on the GPU." },
    { T_GRAPHICS, "graphics", "dlss_preset", "auto", "DLSS preset", 0, LIST(k_dlssp, k_dlssp_n), k_dlssp_s,
      "Only with NVIDIA DLSS. Auto lets the DLL choose for each mode (K for DLAA, Quality and Balanced, M for Performance, L for "
      "Ultra Performance). L and M are sharper with less ghosting but cost more, best on RTX 40 and newer." },
    { T_GRAPHICS, "graphics", "sharpening", "high", "FSR sharpening", 0, LIST(k_sharp, k_sharp_n), k_sharp_s,
      "The sharpening pass of FSR (RCAS). Only with FSR scaling." },
    { T_GRAPHICS, "graphics", "cas", "off", "CAS sharpening", 0, LIST(k_sharp, k_cas_n), k_cas_n,
      "AMD's contrast adaptive sharpening on the picture with bilinear or sharp bilinear scaling, or NVIDIA DLSS: "
      "crisper detail, edges not overdone. AMD FSR has its own (FSR sharpening). Not with frame generation." },
    { T_GRAPHICS, "graphics", "deband", "off", "Debanding", 1, SW,
      "Smooths the steps in dark gradients, fog and skies, with a fine grain; edges and texture stay as they are. "
      "Not with frame generation." },
    { T_GRAPHICS, "graphics", "vibrance", "off", "Colour vibrance", 0, LIST(k_grade, k_grade_n), k_grade_n,
      "Livelier colours: the dull ones raised the most, skin and colours that are strong already far less. "
      "On the whole picture, the game's HUD and menus too." },
    { T_GRAPHICS, "graphics", "contrast", "off", "Contrast", 0, LIST(k_grade, k_grade_n), k_grade_n,
      "Lit surfaces and highlights stand out more; deep shadows keep their detail, black and white kept." },
    { T_GRAPHICS, "graphics", "frame_generation", "off", "Frame generation", 0, LIST(k_fg, k_fg_n), k_fg_s,
      "Twice the frames on screen, with VSync on: the game is held at half the monitor's refresh rate (60 at 120 Hz), "
      "a frame is made between each two, about a frame later, and below 25 it stands aside. ANR FG (beta) keeps what "
      "moves whole where AMD FSR's can tear it; chosen, the launcher offers to download it from Apex Compute's releases." },
    { T_GRAPHICS, "graphics", "reflex", "off", "NVIDIA Reflex", 0, LIST(k_reflex, k_reflex_n), k_reflex_n,
      "Less input lag on NVIDIA GeForce GTX 900 and newer (beta): each frame starts as late as the GPU allows, so fewer "
      "frames wait their turn when the GPU is the slower part. Boost keeps the GPU's clocks up too. Not with frame "
      "generation. Takes effect at the next start." },   /* gpu_detect: shown with an NVIDIA GPU */
    { T_GRAPHICS, "graphics", "antialiasing", "smaa", "Anti-aliasing", 0, LIST(k_aa, k_aa_n), k_aa_s,
      "Smooths jagged edges after the game has drawn the frame. Costs next to nothing. AMD FSR scaling "
      "does this itself in play." },
    { T_GRAPHICS, "graphics", "anisotropy", "16", "Anisotropic filtering", 0, LIST(k_aniso, k_aniso_n), k_aniso_s,
      "Sharper textures seen at an angle. The game itself asks for none." },
    { T_GRAPHICS, "graphics", "async_shaders", "2", "Build shaders in the background", 1, k_async, NULL, 2, NULL,
      "A new shader is built while the game goes on, instead of a stall. Walls, floors and other solid things "
      "wait for theirs; an effect turns up a moment late." },
    { T_GRAPHICS, "graphics", "shader_wait", "off", "Wait for new shaders", 0, LIST(k_swait, k_swait_n), k_swait_s,
      "How long walls and floors wait for their new shaders. Short: 33 ms in all for a frame, no pause; past it a "
      "gap shows there until the shader is built. Full: up to two seconds each, no gap but a pause. Off: no wait, "
      "a gap for a moment." },
    { T_GRAPHICS, "graphics", "render_thread", "on", "Render thread", 1, SW,
      "The GPU's work is recorded and shown on a thread of its own, beside the one that reads the game's "
      "commands: about 10% more frames where that one holds the game back." },
    { T_GRAPHICS, "graphics", "pipeline_wait", "on", "Build pipelines first", 1, SW,
      "At the start the game waits until the pipelines kept from earlier sessions are built, so none is built "
      "while you play: a moment, a few seconds after a GPU driver update." },
    { T_GRAPHICS, "graphics", "shader_prebuild", "on", "Pixel shaders built ahead", 1, SW,
      "The pixel shaders of the game's files are built before play, after the vertex shaders on the first start (Enter "
      "or Start leaves them for the next), or by Build all shaders now below Clear shaders: some minutes, once. Places "
      "you reach later wait for fewer of them." },
    { T_CONTROLS, "input", "device", "auto", "Device", 0, LIST(k_dev, k_dev_n), k_dev_s,
      "Auto reads a controller and the keyboard and mouse at the same time: either one plays." },
    { T_CONTROLS, "input", "api", "auto", "Controller API", 0, LIST(k_api, k_api_n), k_api_s,
      "Auto reads a controller through XInput when it has it (Xbox, most PC pads), else through SDL3 (DualShock 4, "
      "DualSense, Switch and others). With DS4Windows, XInput, or one controller counts twice." },
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
    { T_MODS, "mods", "use", "on", "Use mods", 1, SW,
      "Off: the game as it is, every mod left out. Below, each one is switched on its own." },
    { T_MODS, "mods", "dump_textures", "off", "Write out the game's textures", 1, SW,
      "For making texture mods: each texture the game loads goes once into textures-dump beside the settings, "
      "named by the id a picture in its place takes. The game is slower while it is on." },
    { T_ONLINE, "online", "mode", "off", "Multiplayer", 0, LIST(k_omode, k_omode_n), k_omode_s,
      "Auto: the game finds other players itself, as torrents find their peers: no host. Host: this PC runs the "
      "matchmaking, for its own game and the players who join it. Join: play through a host's PC. Then Multi-Player "
      "on the game's title screen, and Quick Play." },
    { T_ONLINE, "online", "name", "", "Online ID", 0, NULL, NULL, 0, NULL,
      "Your name in the game: 3 to 16 letters, digits, - and _, starting with a letter. Every player needs their own.",
      16 },
    { T_ONLINE, "online", "server", "", "Host's address", 0, NULL, NULL, 0, NULL,
      "To join: the address the host's Multiplayer tab shows, or the host's name.", 63 },
    { T_ONLINE, "online", "relay", "off", "Always relayed", 1, SW,
      "Your game's traffic goes by the host's PC (Host, Join) or the players' nodes (Auto) instead of straight to the "
      "other players: for a network where joining works and playing does not." },
    { T_ONLINE, "online", "upnp", "on", "Open the port on the router", 1, SW,
      "Auto: asks the router (UPnP) to let other players reach this PC, as torrent clients do. Players who can be "
      "reached carry for the ones who cannot." },
    { T_ONLINE, "online", "relay_others", "on", "Help others connect", 1, SW,
      "Auto: two players who cannot reach each other may talk through your PC (a few pairs at most)." },
    { T_ONLINE, "online", "nodes", "", "Community nodes", 0, NULL, NULL, 0, NULL,
      "Auto: nodes anyone can run on a reachable PC (scripts\\mp_p2p.py community), which introduce players and "
      "carry for them: address:port, a comma between.", 63 },
    { T_SYSTEM, "launcher", "language", "auto", "Launcher language", 0, LIST(k_uilang, k_uilang_n), NULL,
      "The language of this window. Windows language follows Windows' display language, and English where this window "
      "has no translation. The game's own text and speech are the next row's." },
    { T_SYSTEM, "system", "language", "auto", "Language", 0, LIST(k_lang, k_lang_n), NULL,
      "The game's language: text and speech. The EU disc takes it at its first start, where it asks (Cross for "
      "this one, Triangle for Greek), and keeps it." },
    { T_SYSTEM, "system", "esc_quits", "off", "Esc closes the game", 1, SW,
      "Esc is not a PS3 button; off leaves it free." },
    { T_SYSTEM, "system", "save_backups", "5", "Save backups", 0, LIST(k_bak, k_bak_n), k_bak_s,
      "Copies kept of a save before the game writes over it." },
    { T_SYSTEM, "system", "steady_audio", "on", "Steady audio", 1, SW,
      "Runs the game's sound on a thread of its own, so heavy fights do not break it up." },
    { T_SYSTEM, "system", "speakers", "auto", "Speakers", 0, LIST(k_spk, k_spk_n), k_spk_s,
      "Auto follows the output device; 7.1 for Windows Sonic or Dolby Atmos for Headphones. Surround also needs "
      "the game's Sound Mode (Options, Audio) on 5.1 or 7.1." },
    { T_SYSTEM, "system", "raw_audio", "off", "Bypass sound effects", 1, SW,
      "The game's sound skips Windows' and the sound card's effects (equalizers, loudness, virtual "
      "surround), where the device has a way past them." },
    { T_SYSTEM, "system", "precompile_music", "normal", "Precompile music", 0, LIST(k_music, k_music_n), k_music_n,
      "Music on the screen that builds the shaders at a version's first start, made by the program." },
    { T_SYSTEM, "launcher", "optimise", "off", "Optimise for this PC", 1, SW,
      "When the game's code is built, a training copy first plays the opening by itself, muted, in a window of its "
      "own (leave it open), and the build then uses what it counted: 2-3% more frames on the test laptop. It adds "
      "about 15 minutes to each build; if anything goes wrong there, the build goes on without it. Turned on after "
      "a build, Play builds the game's code again with it." },
    { T_SYSTEM, "launcher", "show", "on", "Show this window at start", 1, SW,
      "When off, hold Shift while the game starts to bring it back." },
    { T_SYSTEM, "launcher", "update_check", "on", "Check for updates", 1, SW,
      "At most every six hours, asks GitHub whether a newer release is out, and says so here with a link to it." },
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
        { "disable_mlaa", "The PS3's SPU anti-aliasing, whose picture this port never shows, does no work: "
                          "about 5% faster, and the bloom stays. AMD FSR and frame generation take the "
                          "game's order without MLAA, which has no bloom or motion blur." },
        { "disable_depth_of_field", "The blur of what lies outside the camera's focus." },
        { "disable_motion_blur", "No blur of what moves fast. With AMD FSR or frame generation it never "
                                 "reaches the screen anyway, and they keep its draws, for what moves." },
        { "disable_mesh_trimming", "Keeps the triangles the game drops as too thin for its 720p grid. For "
                                   "Internal resolution above 720p: at 720p it only adds geometry (3% slower "
                                   "at the first level, 11% at the gate)." },
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
        if (!strcmp(gowa_title_patch_id(i), "unlock_fps")) continue;   /* the Display tab's Frame rate cap switches it */
        if (!strcmp(gowa_title_patch_id(i), "mlaa_path_off")) continue;   /* AMD FSR and frame generation switch it (main.cpp) */
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
    /* Beside the settings, for a copy that no `gowa.py build` gave paths to. */
    char dir[1100], file[1200];
    snprintf(dir, sizeof dir, "%s", ps3_settings_path());
    char* slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) {
        slash[1] = 0;
        snprintf(file, sizeof file, "%sd3d12_shader_cache.bin", dir);
        ps3_settings_default("graphics", "shader_cache", file);
    }
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

/* What a list shows for the option's value, in the language shown. */
static const char* opt_name(const Opt& o)
{
    const int i = opt_index(o, o.cur);
    return i >= 0 ? T(o.names[i]) : o.cur;
}

/* An option's short name: its segment's, else its list entry. */
static const char* opt_short(const Opt* o)
{
    if (!o) return "";
    const int i = opt_index(*o, o->cur);
    return i < 0 ? o->cur : T(o->seg ? o->seg[i] : o->names[i]);
}

#ifdef _WIN32
/* ---------------------------------------------------------------------------
 * The window, and the keyboard keys: a sheet over it (kb_draw), each binding a
 * box that, clicked, waits for a key or a mouse button
 * -----------------------------------------------------------------------*/
static int   s_dpi = 96;      /* the monitor's */
static float s_ui = 96.0f;   /* what the launcher is drawn at: s_dpi, less if it would not fit */
static int   s_done;          /* 1 play, 2 quit */
static HWND  s_main;
static int   s_kb_open;       /* the keyboard keys are over the window */
static char  s_kb[N_KB][64];  /* their bindings while they are */
static int   s_capture = -1;  /* the binding waiting for a key or a mouse button, or -1 */

static int S(int v) { return MulDiv(v, s_dpi, 96); }
static void redraw(void);

/* The node on Auto, as the launcher shows it: its status, and the players it knows of (its players.txt), read on the
 * window's timer. Shots make them up (s_mp_fake). */
enum { WM_ONLINE = WM_APP + 33 };   /* gowa_online_begin's start ended */
struct MpPlayer {
    char name[20], state[12], how[10];
};
static int s_mp_status;              /* gowa_online_status */
static char s_mp_why[300];
static char s_mp_text[8192];         /* players.txt as last read */
static MpPlayer s_mp_players[24];
static int s_mp_n, s_mp_total;       /* shown, and in all */
static char s_mp_nat[8];
static int s_mp_fake;

static void mp_parse(void)
{
    s_mp_n = s_mp_total = 0;
    s_mp_nat[0] = 0;
    const char* p = s_mp_text;
    while (*p) {
        const char* e = strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[160];
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line - 1 ? len : sizeof line - 1), p);
        char* f[5] = {};
        int k = 0;
        for (char* q = line; k < 5 && q; k++) {
            f[k] = q;
            q = strchr(q, '\t');
            if (q) *q++ = 0;
        }
        if (k >= 2 && !strcmp(f[0], "me")) {
            snprintf(s_mp_nat, sizeof s_mp_nat, "%s", f[1]);
        } else if (k >= 4 && !strcmp(f[0], "player")) {
            s_mp_total++;
            if (s_mp_n < (int)(sizeof s_mp_players / sizeof s_mp_players[0])) {
                MpPlayer& m = s_mp_players[s_mp_n++];
                snprintf(m.state, sizeof m.state, "%s", f[1]);
                snprintf(m.how, sizeof m.how, "%s", f[2]);
                snprintf(m.name, sizeof m.name, "%s", f[3]);
            }
        }
        p += len + (e ? 1 : 0);
    }
}

/* The node's status and players read again: 1 when something shown changed. */
static int mp_refresh(void)
{
    if (s_mp_fake) return 0;
    char why[300], text[8192];
    const int st = gowa_online_status(why, sizeof why);
    gowa_online_players(text, sizeof text);
    if (st == s_mp_status && !strcmp(why, s_mp_why) && !strcmp(text, s_mp_text)) return 0;
    s_mp_status = st;
    snprintf(s_mp_why, sizeof s_mp_why, "%s", why);
    snprintf(s_mp_text, sizeof s_mp_text, "%s", text);
    mp_parse();
    return 1;
}

static const wchar_t* mp_state_name(const char* st)
{
    return TW(!strcmp(st, "lobby") ? L"in the multiplayer menu" : !strcmp(st, "searching") ? L"looking for a match" :
              !strcmp(st, "playing") ? L"in a match" : L"online");
}

/* Multiplayer as the settings are now: on Auto the node runs, starting again when what it takes changes; on anything
 * else none does (Host and Join start at Play). The online settings go to ps3_settings first: that is where the
 * node's start reads them (the file is written on Save or Play, as for every setting). */
static void online_sync(void)
{
    for (int i = 0; i < N_OPT; i++)
        if (!strcmp(s_opt[i].section, "online")) ps3_settings_set("online", s_opt[i].key, s_opt[i].cur);
    gowa_online_begin(s_main, WM_ONLINE);
    mp_refresh();
    redraw();
}

static void dark_caption(HWND h)
{
    BOOL dark = TRUE;   /* DWMWA_USE_IMMERSIVE_DARK_MODE: 20, 19 before Windows 10 20H1 */
    if (FAILED(DwmSetWindowAttribute(h, 20, &dark, sizeof dark))) DwmSetWindowAttribute(h, 19, &dark, sizeof dark);
    /* Windows 11: the title bar in the window's own colours (DWMWA_BORDER_COLOR, _CAPTION_COLOR, _TEXT_COLOR as
     * COLORREF). Windows 10 says no and keeps its dark one. */
    const COLORREF border = RGB(0x2C, 0x1A, 0x40), caption = RGB(0x0A, 0x06, 0x11), title = RGB(0xE8, 0xD2, 0xA8);
    DwmSetWindowAttribute(h, 34, &border, sizeof border);
    DwmSetWindowAttribute(h, 35, &caption, sizeof caption);
    DwmSetWindowAttribute(h, 36, &title, sizeof title);
}

/* The sheet opens with the bindings the settings have. */
static void kb_open(void)
{
    for (int i = 0; i < N_KB; i++)
        snprintf(s_kb[i], sizeof s_kb[i], "%s", ps3_settings_str("keyboard", k_kb[i][0], NULL, k_kb[i][2]));
    s_capture = -1;
    s_kb_open = 1;
}

/* OK: the bindings into the settings (written with Save or Play). Cancel: as they were. */
static void kb_close(int ok)
{
    if (ok)
        for (int i = 0; i < N_KB; i++) ps3_settings_set("keyboard", k_kb[i][0], s_kb[i]);
    s_capture = -1;
    s_kb_open = 0;
}

static void kb_set(int i, const char* name)
{
    snprintf(s_kb[i], sizeof s_kb[i], "%s", name ? name : "");
    s_capture = -1;
    redraw();
}

/* A key pressed while a binding waits for one. Esc gives up, Backspace and
 * Delete clear it, Alt is the window's own. */
static void kb_key(WPARAM vk, LPARAM lp)
{
    const int i = s_capture;
    if (vk == VK_ESCAPE) { s_capture = -1; redraw(); return; }
    if (vk == VK_BACK || vk == VK_DELETE) { kb_set(i, ""); return; }
    if (vk == VK_MENU) return;
    if (vk == VK_SHIFT) vk = MapVirtualKeyA((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) vk = (lp & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
    const char* name = ps3_pad_key_name((int)vk);
    if (name) kb_set(i, name);
    else MessageBeep(MB_OK);
}

/* A mouse button pressed on the binding that waits: that button is its key. */
static void kb_mouse(UINT m, WPARAM w)
{
    kb_set(s_capture, m == WM_LBUTTONDOWN ? "MouseLeft" : m == WM_RBUTTONDOWN ? "MouseRight"
                    : m == WM_MBUTTONDOWN ? "MouseMiddle" : HIWORD(w) == XBUTTON1 ? "Mouse4" : "Mouse5");
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

static void mods_keep(void);

static int save_all(void)
{
    for (int i = 0; i < N_OPT; i++) ps3_settings_set(s_opt[i].section, s_opt[i].key, s_opt[i].cur);
    for (int i = 0; i < s_npopt; i++) ps3_settings_set(s_popt[i].section, s_popt[i].key, s_popt[i].cur);
    mods_keep();
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
    int pad;        /* controllers plugged in, as the Controller API reads them */
    char pad_name[160];   /* the first one's name and what reads it */
} s_rc;

static const wchar_t* W(const char* s);
static const char* U8(const wchar_t* s);

/* The game's folder, as the links give it. */
static struct {
    GameInfo g;                        /* what <vfs>\PS3_GAME leads to */
    wchar_t vfs[GAME_PATH_MAX], hdd0[GAME_PATH_MAX];              /* [paths], else next to the exe */
    wchar_t link_vfs[GAME_PATH_MAX], link_hdd0[GAME_PATH_MAX];    /* <vfs>\PS3_GAME, <hdd0>\game\<id> (game_title.h) */
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

/* GOWA.SELF to GOWA.elf, on a thread of its own. */
static struct {
    HANDLE thread;
    GameProgress progress;
    int ok;
    int download;                      /* the player asked for RPCS3 to be downloaded */
    int disc;                          /* a disc: GOWA.SELF in its update, decrypted with no .rap (game_prepare_disc) */
    wchar_t rpcs3[GAME_PATH_MAX], rap[GAME_PATH_MAX], game[GAME_PATH_MAX], update[GAME_PATH_MAX], data[GAME_PATH_MAX];
    wchar_t err[320];
} s_prep;

/* Build all shaders now (pb_start, below clear_shaders): the pixel shaders' build as a process of its own. */
enum { WM_PREBUILD = WM_APP + 31, WM_ANR = WM_APP + 32, WM_DLSS = WM_APP + 34 };
static struct {
    HANDLE proc, out, job, reader;
    volatile LONG running, done, total;
    char result[200];   /* "prebuild: done ..." or "prebuild: error ...", for the toast */
} s_pb;

/* The pixel shaders built ahead, for the ready check: 0 none of this build's form, 1 some (a build stopped, or one
 * for another version: the next goes on, in seconds if nothing changed), 2 built for this version. Read when the
 * window opens and when a build ends. */
static int s_pbs;
static void pb_state_read(void)
{
    s_pbs = 0;
    const char* sc = ps3_settings_str("graphics", "shader_cache", "D3D12_SHADER_CACHE", NULL);
    if (!sc || !*sc || ps3_settings_ieq(sc, "off") || !rsx_d3d12_prebuilt_current(sc)) return;
    s_pbs = 1;
    char done[1200], line[128] = "", want[128];
    snprintf(done, sizeof done, "%s.dxil.prebuilt.done", sc);
    if (FILE* f = fopen(done, "rb")) {
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
    }
    snprintf(want, sizeof want, "Ascension-R %s\n", AR_VERSION);
    if (!strcmp(line, want)) s_pbs = 2;
}

#ifdef PORT_GAME_MODULE
extern "C" int game_module_state(void);                                        /* game_module_host.c */
extern "C" int game_module_pgo(void);                                          /* game_module_host.c */
extern "C" void game_module_built(wchar_t* out, size_t n, const wchar_t* name);
extern "C" void game_module_kit(wchar_t* out, size_t n, const wchar_t* name);

/* The Kit: the game's code built on this PC, once (kit_start). The Kit's Python runs gowa.py kit-build, which says how
 * far it is in lines that start with @; the rest of what it prints is in game-code\build.log. Stop ends it, and Build
 * goes on where it stopped. */
enum { WM_KIT_BUILD = WM_APP + 3 };
static struct {
    HANDLE proc, job, reader, out;     /* the Python, the job it and its compilers are in, the thread reading it */
    volatile LONG running;
    volatile LONG permille, left;      /* how far, and the seconds left (-1 while that is not known) */
    SRWLOCK lock;                      /* over what and err */
    wchar_t what[200];                 /* the step it is at, from @progress */
    wchar_t err[400];                  /* @error, or how it ended */
    char last[240];                    /* its last line that was not an @ line: Python's own complaint, if it had one */
    volatile LONG done, stopped;       /* @done came; Stop was pressed */
    int state;                         /* game_module_state(): 0 built, 1 not yet, 2 for another version, 3 no Kit */
    int pgo;                           /* game_module_pgo(): built with Optimise for this PC (kit_state: 4 when not) */
    int partly;                        /* a build that stopped left its work: Build goes on from there */
    ULONGLONG started;
} s_kit;

static void kit_start(void);

static void kit_refresh(void)
{
    wchar_t p[GAME_PATH_MAX];
    s_kit.state = game_module_state();
    s_kit.pgo = game_module_pgo();
    game_module_built(p, GAME_PATH_MAX, L"work");
    s_kit.partly = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* launcher.optimise, as the System tab has it now (saved or not): the build makes a training copy first, which plays
 * the opening, and builds with what it counted (kit-build --pgo). Off at first: it adds about 15 minutes to a build,
 * for 2-3% more frames on the test laptop (docs/notes/2026-11-14-kit-pgo.md). */
static int kit_optimise(void)
{
    const Opt* o = opt_find("launcher", "optimise");
    return ps3_settings_ieq(o ? o->cur : ps3_settings_str("launcher", "optimise", NULL, "off"), "on");
}

/* s_kit.state, and 4: built, but without Optimise for this PC, which is on now. Build makes it again with it; turning
 * it off plays what is there. */
static int kit_state(void)
{
    return s_kit.state == 0 && !s_kit.pgo && kit_optimise() ? 4 : s_kit.state;
}

/* UTF-8 into a buffer, cut to fit it. */
static void kit_copy(wchar_t* to, size_t n, const char* s)
{
    const int k = MultiByteToWideChar(CP_UTF8, 0, s, (int)strnlen(s, n - 1), to, (int)n - 1);
    to[k > 0 ? k : 0] = 0;
}

/* The line the ready check shows while it builds; the level as check_line takes it. */
static int kit_line(wchar_t* b, size_t n)
{
    AcquireSRWLockShared(&s_kit.lock);
    const long left = s_kit.left;
    const int secs = (int)((GetTickCount64() - s_kit.started) / 1000);
    if (s_kit.stopped) swprintf(b, n, TW(L"Stopping\x2026"));
    else if (!s_kit.what[0]) swprintf(b, n, TW(L"Starting\x2026"));
    /* The stage, as gowa.py's PROGRESS.phase names it (in the catalog, scripts/lang.py). */
    else if (left >= 90) swprintf(b, n, TW(L"%ls \x00B7 about %ld min left"), TW(s_kit.what), (left + 30) / 60);
    else if (left >= 0) swprintf(b, n, TW(L"%ls \x00B7 about a minute left"), TW(s_kit.what));
    else swprintf(b, n, L"%ls \x00B7 %d:%02d", TW(s_kit.what), secs / 60, secs % 60);
    ReleaseSRWLockShared(&s_kit.lock);
    return 2;
}

/* What the ready check says when the game's code is not there to play, and the line under it. */
static int kit_need(wchar_t* b, size_t n, const wchar_t** hint)
{
    *hint = kit_optimise() ? L"Once, on this PC: about 25 minutes" : L"Once, on this PC: about 10 minutes";
    AcquireSRWLockShared(&s_kit.lock);
    const int failed = s_kit.err[0] != 0;
    if (failed) swprintf(b, n, TW(L"Not built: %ls"), s_kit.err);
    ReleaseSRWLockShared(&s_kit.lock);
    if (failed) {
        *hint = L"";   /* Log and Try again leave no room: the log says the rest */
        return -1;
    }
    switch (kit_state()) {
    case 1:
        if (s_kit.partly) {
            swprintf(b, n, TW(L"The game's code is partly built"));
            *hint = L"Build goes on where it stopped";
        } else {
            swprintf(b, n, TW(L"The game's code is not built yet"));
        }
        return 0;
    case 2:
        swprintf(b, n, TW(L"The game's code is from another version"));
        *hint = kit_optimise() ? L"Again, on this PC: about 25 minutes" : L"Again, on this PC: about 10 minutes";
        return 0;
    case 4:
        swprintf(b, n, TW(L"The game's code is built without Optimise for this PC"));
        *hint = L"Again with it: about 25 minutes";   /* or Optimise off again, which plays what is there */
        return 0;
    default:
        swprintf(b, n, TW(L"No Kit folder to build the game's code with"));
        *hint = L"";
        return -1;
    }
}
#endif

static void path_slashes(wchar_t* p) { for (; *p; p++) if (*p == L'/') *p = L'\\'; }

/* A file beside the settings; "" gives the folder. */
static void data_file(wchar_t* out, size_t n, const wchar_t* name)
{
    wchar_t d[GAME_PATH_MAX];
    swprintf(d, GAME_PATH_MAX, L"%ls", W(ps3_settings_path()));
    path_slashes(d);
    if (wchar_t* s = wcsrchr(d, L'\\')) *s = 0;
    if (name[0]) swprintf(out, n, L"%ls\\%ls", d, name);
    else swprintf(out, n, L"%ls", d);
}

/* What the decrypting is doing; *frac is how far, negative when that is not known. */
static int prep_line(wchar_t* b, size_t n, float* frac)
{
    const GameProgress& p = s_prep.progress;
    const ULONGLONG now = GetTickCount64();
    const int secs = (int)((now - p.phase_at) / 1000);
    const wchar_t* tail = p.stop ? L" Stopping\x2026" : L"";
    float f = -1.0f;
    int level = 2;
    switch (p.phase) {
    case GP_DOWNLOAD: {
        const double mb = p.done / 1048576.0, total = p.total / 1048576.0;
        const int quiet = p.data_at ? (int)((now - p.data_at) / 1000) : secs;
        if (p.total > 0) f = (float)((double)p.done / (double)p.total);
        if (quiet >= 4 && p.total > 0) {
            swprintf(b, n, TW(L"Downloading RPCS3: %.1f of %.1f MB, no data for %d s%ls"), mb, total, quiet, tail);
            level = 0;
        } else if (quiet >= 4) {
            swprintf(b, n, TW(L"Downloading RPCS3: no answer from the server for %d s%ls"), quiet, tail);
            level = 0;
        } else if (p.speed > 0 && p.total > 0) {
            wchar_t again[32] = L"";
            if (p.retries) swprintf(again, 32, TW(L", %ld reconnected"), (long)p.retries);
            swprintf(b, n, TW(L"Downloading RPCS3: %.1f of %.1f MB, %.1f MB/s, about %d s left%ls%ls"), mb, total, p.speed / 1048576.0,
                     (int)((p.total - p.done) / p.speed), again, tail);
        } else if (p.done > 0) {
            swprintf(b, n, TW(L"Downloading RPCS3: %.1f of %.1f MB%ls"), mb, total, tail);
        } else {
            swprintf(b, n, TW(L"Downloading RPCS3: connecting\x2026%ls"), tail);
        }
        break;
    }
    case GP_VERIFY:  swprintf(b, n, TW(L"Checking the download\x2026%ls"), tail); break;
    case GP_UNPACK:  swprintf(b, n, TW(L"Unpacking RPCS3\x2026 %d s%ls"), secs, tail); break;
    case GP_COPY:    swprintf(b, n, TW(L"Copying the game's files\x2026%ls"), tail); break;
    case GP_DECRYPT: swprintf(b, n, TW(L"Decrypting the game\x2026 %d s%ls"), secs, tail); break;
    case GP_CHECK:   swprintf(b, n, TW(L"Checking the result\x2026%ls"), tail); break;
    default:         swprintf(b, n, TW(L"Starting\x2026%ls"), tail); break;
    }
    if (frac) *frac = f;
    return level;
}

static void game_links(void)
{
    const char* vfs = ps3_settings_str("paths", "vfs", "PS3_VFS_ROOT", NULL);
    const char* hdd0 = ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL);
    wchar_t here[GAME_PATH_MAX];                 /* the settings' folder: the links go beside them */
    swprintf(here, GAME_PATH_MAX, L"%ls", W(ps3_settings_path()));
    path_slashes(here);
    if (wchar_t* s = wcsrchr(here, L'\\')) *s = 0;
    else if (GetModuleFileNameW(NULL, here, GAME_PATH_MAX)) { if (wchar_t* e = wcsrchr(here, L'\\')) *e = 0; }
    if (vfs && *vfs) swprintf(s_game.vfs, GAME_PATH_MAX, L"%ls", W(vfs));
    else swprintf(s_game.vfs, GAME_PATH_MAX, L"%ls\\vfs", here);
    if (hdd0 && *hdd0) swprintf(s_game.hdd0, GAME_PATH_MAX, L"%ls", W(hdd0));
    else swprintf(s_game.hdd0, GAME_PATH_MAX, L"%ls\\gamedata\\dev_hdd0", here);
    path_slashes(s_game.vfs);
    path_slashes(s_game.hdd0);
    swprintf(s_game.link_vfs, GAME_PATH_MAX, L"%ls\\PS3_GAME", s_game.vfs);
    /* The second link is named for the title the first leads to (the default's, with none yet). */
    swprintf(s_game.link_hdd0, GAME_PATH_MAX, L"%ls\\game\\%hs", s_game.hdd0, game_title_of_folder(s_game.link_vfs)->id);
}

/* The ids the launcher takes, for a line to the player. */
static const wchar_t* game_titles_line(void)
{
    static wchar_t b[96];
    game_titles_text(b, sizeof b / sizeof b[0]);
    return b;
}

/* What the links lead to now, and whether the game can start. */
static void game_refresh(void)
{
    game_links();
    wchar_t dir[GAME_PATH_MAX], upd[GAME_PATH_MAX];
    if (!game_link_target(s_game.link_vfs, dir, GAME_PATH_MAX))
        swprintf(dir, GAME_PATH_MAX, L"%ls", s_game.link_vfs);   /* a real folder, or none */
    if (!game_link_target(s_game.link_hdd0, upd, GAME_PATH_MAX)) upd[0] = 0;   /* a disc's update */
    game_probe_pair(dir, upd, &s_game.g);
    if (s_game.g.state == GAME_NO_ELF) {   /* decrypted here: the game's folder is only read */
        wchar_t p[GAME_PATH_MAX + 16];
        data_file(p, sizeof p / sizeof p[0], L"GOWA.elf");
        if (game_elf_ok(p)) {
            s_game.g.state = GAME_OK;
            const char* was = ps3_settings_str("paths", "elf", NULL, NULL);
            if (!was || strcmp(was, U8(p))) {
                ps3_settings_set("paths", "elf", U8(p));
                ps3_settings_save(NULL);
            }
        }
    }
    const char* elf = ps3_settings_str("paths", "elf", NULL, NULL);
    s_rc.elf_ok = elf && *elf && GetFileAttributesA(elf) != INVALID_FILE_ATTRIBUTES;
    /* Not the whole game: nothing is built or played from it, whatever was decrypted before (game_status says what
     * the folder lacks). */
    if (s_game.g.state == GAME_INCOMPLETE) s_rc.elf_ok = 0;
#ifdef PORT_GAME_MODULE
    kit_refresh();
#endif
}

/* What to tell a player who pressed something that needs the game. */
static const char* game_needed_toast(void)
{
    return s_game.g.state == GAME_INCOMPLETE                              ? "Not the whole game: see the System tab"
           : s_game.g.state == GAME_OTHER_BUILD && s_game.g.other_version ? "Not version 1.12: see the System tab"
                                                                          : "Set the game's folder first";
}

static const wchar_t* game_path_shown(void)
{
    if (GetFileAttributesW(s_game.link_vfs) == INVALID_FILE_ATTRIBUTES) return L"Not set";
    if (!s_game.g.disc || !s_game.g.update[0]) return s_game.g.dir;
    static wchar_t b[2 * GAME_PATH_MAX + 32];   /* a disc: its folder and its update's */
    swprintf(b, sizeof b / sizeof b[0], TW(L"%ls  \x00B7  update: %ls"), s_game.g.dir, s_game.g.update);
    return b;
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
#ifdef PORT_GAME_MODULE
        if (s_kit.running) return kit_line(b, n);
#endif
        swprintf(b, n, TW(L"God of War: Ascension \x00B7 %ls \x00B7 %ls"), W(g.title_id), game_version());
        return 1;
    case GAME_OTHER_BUILD:
        if (g.other_version) {
            swprintf(b, n, TW(L"That is %ls %ls. This port runs version 1.12: install the game's update first"),
                     W(g.title_id), game_version());
            return -1;
        }
        swprintf(b, n, TW(L"%ls %ls, but not the GOWA.elf this port was made from: it may not run"), W(g.title_id),
                 game_version());
        return 0;
    case GAME_INCOMPLETE:
        game_files_text(&g, b, n);
        return -1;
    case GAME_NO_ELF:
        if (s_prep.thread) return prep_line(b, n, NULL);
        if (s_prep.err[0]) swprintf(b, n, L"%ls", s_prep.err);
        else if (g.disc) swprintf(b, n, TW(L"To decrypt the game this needs the RPCS3 its update is installed in"));
        else swprintf(b, n, !s_prep.rap[0] && !s_prep.rpcs3[0] ? TW(L"To decrypt the game this needs RPCS3 and its license file (.rap)")
                            : !s_prep.rap[0] ? TW(L"To decrypt the game this needs its license file (.rap)")
                                             : TW(L"To decrypt the game this needs RPCS3"));
        return -1;
    case GAME_OTHER_TITLE:
        swprintf(b, n, TW(L"That is %ls %ls. This port runs %ls 1.12"), W(g.title_id), game_version(), game_titles_line());
        return -1;
    default:
        swprintf(b, n, TW(L"No game there. Auto detect looks on every disk, or Browse to its folder"));
        return -1;
    }
}

extern "C" int ps3_pad_host_describe(const char* api, char* out, size_t n);   /* cellPad.c */

/* The controllers as cellPad would read them with the Controller API chosen here, saved or not: how many, and the
 * first one's name and what reads it in name. */
static int pad_connected(char* name, size_t n)
{
    const Opt* api = opt_find("input", "api");
    return ps3_pad_host_describe(api ? api->cur : NULL, name, n);
}

/* The first hardware adapter of a preference. */
static int gpu_first(IDXGIFactory6* f, DXGI_GPU_PREFERENCE pref, DXGI_ADAPTER_DESC1* d)
{
    int found = 0;
    IDXGIAdapter1* a = NULL;
    for (UINT i = 0; !found && SUCCEEDED(f->EnumAdapterByGpuPreference(i, pref, IID_PPV_ARGS(&a))); i++) {
        found = SUCCEEDED(a->GetDesc1(d)) && !(d->Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        a->Release();
    }
    return found;
}

/* The GPU Windows calls fast and the one it calls low-power: what the renderer asks for
 * graphics.gpu, so the name shown is the one the game runs on. The same adapter twice is a PC with
 * one GPU, and the row is not shown. */
static struct { char name[2][128]; char desc[1024]; int n; bool nvidia; } s_gpus;

/* The Graphics card row's line, in the language shown. */
static void gpu_desc(void)
{
    Opt* o = opt_find("graphics", "gpu");
    if (!o || s_gpus.n != 2) return;
    snprintf(s_gpus.desc, sizeof s_gpus.desc,
             T("Discrete: %s. Integrated: %s, slower, but it uses less power and makes less heat."), s_gpus.name[0],
             s_gpus.name[1]);
    o->desc = s_gpus.desc;
}

/* The window's language, and what is made from it once. */
static void lang_set(const char* code)
{
    i18n_set(code);
    gpu_desc();
}

/* A format's text as a string (the mods' status lines). */
static std::wstring fmt_w(const wchar_t* f, ...)
{
    wchar_t b[1024];
    va_list ap;
    va_start(ap, f);
    vswprintf(b, sizeof b / sizeof b[0], f, ap);
    va_end(ap);
    return b;
}

static void gpu_detect(void)
{
    s_gpus.n = 0;
    s_gpus.name[0][0] = 0;
    s_gpus.nvidia = false;
    IDXGIFactory6* f = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && f) {
        DXGI_ADAPTER_DESC1 fast, low;
        if (gpu_first(f, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &fast)) {
            WideCharToMultiByte(CP_UTF8, 0, fast.Description, -1, s_gpus.name[0], sizeof s_gpus.name[0], NULL, NULL);
            s_gpus.n = 1;
            s_gpus.nvidia = fast.VendorId == 0x10DE;
            if (gpu_first(f, DXGI_GPU_PREFERENCE_MINIMUM_POWER, &low) && memcmp(&fast.AdapterLuid, &low.AdapterLuid, sizeof(LUID))) {
                WideCharToMultiByte(CP_UTF8, 0, low.Description, -1, s_gpus.name[1], sizeof s_gpus.name[1], NULL, NULL);
                s_gpus.n = 2;
                s_gpus.nvidia = s_gpus.nvidia || low.VendorId == 0x10DE;
            }
        }
        f->Release();
    }
    /* NVIDIA Reflex: only an NVIDIA GPU has it (a row set on stays in the file: the game says why it is off). */
    if (!s_gpus.nvidia) opt_find("graphics", "reflex")->tab = -1;
    Opt* o = opt_find("graphics", "gpu");
    if (s_gpus.n == 2) gpu_desc();
    else o->tab = -1;
}

static const char* gpu_chosen(void)
{
    return s_gpus.n == 2 && ps3_settings_ieq(opt_find("graphics", "gpu")->cur, "integrated") ? s_gpus.name[1] : s_gpus.name[0];
}

/* graphics.anr_dll as a full path: as given, or from the exe's folder ("anr\\nvngx_dlssg.dll" by default). */
static bool anr_dll_path(std::wstring* out)
{
    const char* v = ps3_settings_str("graphics", "anr_dll", NULL, "anr\\nvngx_dlssg.dll");
    std::wstring w = v && *v ? std::wstring(W(v)) : std::wstring();
    if (w.empty()) return false;
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    if ((w.size() >= 2 && w[1] == L':') || (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\')) {
        *out = w;
        return true;
    }
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    while (n && exe[n - 1] != L'\\') n--;
    if (!n) return false;
    *out = std::wstring(exe, n) + w;
    return true;
}

static bool anr_present(void)
{
    std::wstring dll;
    return anr_dll_path(&dll) && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/* The folder under dir (down to depth more levels) that holds a file named name, or "". */
static std::wstring anr_find(const std::wstring& dir, const wchar_t* name, int depth)
{
    if (GetFileAttributesW((dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES) return dir;
    if (depth <= 0) return std::wstring();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring hit;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        hit = anr_find(dir + L"\\" + fd.cFileName, name, depth - 1);
    } while (hit.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return hit;
}

/* ANR FG's files from the zip of one of its releases (anr-fg-*-addon.zip or -full.zip): the folder in it that holds
 * the DLL (Apex Neural Rendering FG\dlssg) put where dll points (graphics.anr_dll), in place of what is there, with
 * the release's name in ascension-r-anr.txt beside it ("V.B20", from label or the zip's name). Unpacked as a mod's zip
 * is (mods_unzip). ANR has no licence: the project ships nothing of it; the player picks the zip or has the launcher
 * fetch it from Apex Compute's releases (anr_dl_run). The game checks the DLL's signature when it loads it. "" when
 * done, else why not. */
static std::string anr_install_to(const wchar_t* zip, const std::wstring& dll, std::string label)
{
    const size_t sl = dll.find_last_of(L'\\');
    if (sl == std::wstring::npos || sl < 3) return "graphics.anr_dll names no folder";
    const std::wstring name = dll.substr(sl + 1), dir = dll.substr(0, sl);
    const size_t up = dir.find_last_of(L'\\');
    if (up == std::wstring::npos) return "graphics.anr_dll names no folder";
    const std::wstring parent = dir.substr(0, up), tmp = dir + L".unpacking", old = dir + L".old";
    mods_remove_tree(tmp, parent);
    mods_remove_tree(old, parent);
    const std::string why = mods_unzip(zip, tmp);
    if (!why.empty()) {
        mods_remove_tree(tmp, parent);
        return "the zip was not unpacked: " + why;
    }
    std::wstring found = anr_find(tmp, name.c_str(), 8);
    if (found.empty()) found = anr_find(tmp, L"nvngx_dlssg.dll", 8);
    if (found.empty()) {
        mods_remove_tree(tmp, parent);
        return "it holds no nvngx_dlssg.dll: pick ANR FG's full or addon zip";
    }
    const bool had = GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (had && !MoveFileExW(dir.c_str(), old.c_str(), 0)) {
        mods_remove_tree(tmp, parent);
        return "the folder there could not be moved aside: is the game running?";
    }
    if (!MoveFileExW(found.c_str(), dir.c_str(), 0)) {
        if (had) MoveFileExW(old.c_str(), dir.c_str(), 0);
        mods_remove_tree(tmp, parent);
        return "its folder could not be put in place";
    }
    mods_remove_tree(old, parent);
    mods_remove_tree(tmp, parent);
    if (label.empty()) {
        char z[MAX_PATH * 3];
        const wchar_t* base = wcsrchr(zip, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : zip, -1, z, sizeof z, NULL, NULL);
        label = anr_version_of(z);
    }
    if (FILE* f = _wfopen((dir + L"\\ascension-r-anr.txt").c_str(), L"wb")) {
        fprintf(f, "%s\n", label.c_str());
        fclose(f);
    }
    return std::string();
}

static std::string anr_install(const wchar_t* zip)
{
    std::wstring dll;
    if (!anr_dll_path(&dll)) return "graphics.anr_dll is not a path";
    return anr_install_to(zip, dll, std::string());
}

/* The release the installed files came from ("V.B20"), or "". */
static std::string anr_label(void)
{
    std::wstring dll;
    if (!anr_dll_path(&dll)) return std::string();
    const size_t sl = dll.find_last_of(L'\\');
    char b[64] = "";
    if (FILE* f = _wfopen((dll.substr(0, sl == std::wstring::npos ? 0 : sl) + L"\\ascension-r-anr.txt").c_str(), L"rb")) {
        if (!fgets(b, sizeof b, f)) b[0] = 0;
        fclose(f);
    }
    std::string v(b);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

/* ANR FG fetched for the player (anr_get.cpp): the newest release's zip into dll's folder.download, checked, unpacked
 * (anr_install_to), and the DLL's signature checked; tag gets the release's name as soon as it is known. "" when done,
 * else why not. On the download thread, or for --anr-get. */
static std::string anr_dl_run(const std::wstring& dll, GameProgress* p, char* tag, size_t tn)
{
    AnrRelease r;
    std::string why = anr_latest(&r);
    if (!why.empty()) return why;
    snprintf(tag, tn, "%s", anr_version_of(r.tag).c_str());
    const size_t sl = dll.find_last_of(L'\\');
    if (sl == std::wstring::npos) return "graphics.anr_dll names no folder";
    const std::wstring dir = dll.substr(0, sl), dl = dir + L".download", parent = dir.substr(0, dir.find_last_of(L'\\'));
    mods_remove_tree(dl, parent);
    if (!CreateDirectoryW(dl.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return "no folder could be made for the download";
    const std::wstring zip = dl + L"\\" + std::wstring(r.name.begin(), r.name.end());
    why = anr_fetch(r, zip, p);
    if (why.empty()) why = anr_install_to(zip.c_str(), dll, anr_version_of(r.tag));
    mods_remove_tree(dl, parent);
    if (!why.empty()) return why;
    std::wstring who;
    if (!anr_signer(dll, &who) || who != L"Apex Compute, Inc.") {
        mods_remove_tree(dir, parent);
        return "its DLL is not signed by Apex Compute, Inc., so it was taken out again";
    }
    return std::string();
}

static void toast(const char* s);
static void toast_bad(const char* s);

/* The download on a thread of its own, its end posted to the window (WM_ANR). */
static struct {
    HANDLE thread;
    GameProgress p;
    volatile LONG running;
    std::wstring dll;
    char tag[32];
    char result[400];   /* "" done, else why not: for the toast */
} s_anr_dl;

static DWORD WINAPI anr_dl_thread(void*)
{
    const std::string why = anr_dl_run(s_anr_dl.dll, &s_anr_dl.p, s_anr_dl.tag, sizeof s_anr_dl.tag);
    snprintf(s_anr_dl.result, sizeof s_anr_dl.result, "%s", why.c_str());
    InterlockedExchange(&s_anr_dl.running, 0);
    PostMessageA(s_main, WM_ANR, 1, 0);
    return 0;
}

static void anr_dl_start(void)
{
    if (s_anr_dl.running) return;
    std::wstring dll;
    if (!anr_dll_path(&dll)) { toast_bad("ANR FG: graphics.anr_dll is not a path"); return; }
    if (s_anr_dl.thread) { CloseHandle(s_anr_dl.thread); s_anr_dl.thread = NULL; }
    memset((void*)&s_anr_dl.p, 0, sizeof s_anr_dl.p);
    s_anr_dl.dll = dll;
    s_anr_dl.tag[0] = s_anr_dl.result[0] = 0;
    InterlockedExchange(&s_anr_dl.running, 1);
    s_anr_dl.thread = CreateThread(NULL, 0, anr_dl_thread, NULL, 0, NULL);
    if (!s_anr_dl.thread) { InterlockedExchange(&s_anr_dl.running, 0); toast_bad("ANR FG: the download could not start"); return; }
    toast("Downloading ANR FG: the Play tab shows how far");
}

/* The download's end, on the launcher's thread. */
static void anr_dl_end(void)
{
    if (s_anr_dl.thread) { WaitForSingleObject(s_anr_dl.thread, 2000); CloseHandle(s_anr_dl.thread); s_anr_dl.thread = NULL; }
    char b[480];
    if (!s_anr_dl.result[0]) {
        snprintf(b, sizeof b, T("ANR FG %s is set up, signed by Apex Compute"), s_anr_dl.tag);
        toast(b);
    } else {
        snprintf(b, sizeof b, T("ANR FG: %s"), s_anr_dl.result);
        toast_bad(b);
    }
}

/* NVIDIA's DLSS DLL fetched for the player (dlss_get.cpp), on a thread of its own, its end posted to the window
 * (WM_DLSS). */
static struct {
    HANDLE thread;
    GameProgress p;
    volatile LONG running;
    int which;          /* DLSS_SR, DLSS_FG */
    char result[400];   /* "" done, else why not: for the toast */
} s_dlss_dl;

static DWORD WINAPI dlss_dl_thread(void*)
{
    const std::string why = dlss_fetch(&s_dlss_dl.p, s_dlss_dl.which);
    snprintf(s_dlss_dl.result, sizeof s_dlss_dl.result, "%s", why.c_str());
    InterlockedExchange(&s_dlss_dl.running, 0);
    PostMessageA(s_main, WM_DLSS, 1, 0);
    return 0;
}

static void dlss_dl_start(int which)
{
    if (s_dlss_dl.running) return;
    if (s_dlss_dl.thread) { CloseHandle(s_dlss_dl.thread); s_dlss_dl.thread = NULL; }
    memset((void*)&s_dlss_dl.p, 0, sizeof s_dlss_dl.p);
    s_dlss_dl.result[0] = 0;
    s_dlss_dl.which = which;
    InterlockedExchange(&s_dlss_dl.running, 1);
    s_dlss_dl.thread = CreateThread(NULL, 0, dlss_dl_thread, NULL, 0, NULL);
    if (!s_dlss_dl.thread) { InterlockedExchange(&s_dlss_dl.running, 0); toast_bad("NVIDIA DLSS: the download could not start"); return; }
    toast("Downloading NVIDIA DLSS: the Play tab shows how far");
}

static void dlss_dl_end(void)
{
    if (s_dlss_dl.thread) { WaitForSingleObject(s_dlss_dl.thread, 2000); CloseHandle(s_dlss_dl.thread); s_dlss_dl.thread = NULL; }
    char b[480];
    if (!s_dlss_dl.result[0]) {
        snprintf(b, sizeof b, T("NVIDIA DLSS %s is set up, signed by NVIDIA"), k_dlss_dll_version);
        toast(b);
    } else {
        snprintf(b, sizeof b, T("NVIDIA DLSS: %s"), s_dlss_dl.result);
        toast_bad(b);
    }
}

/* The card the game will use is one of NVIDIA's RTX cards ("NVIDIA GeForce RTX 3060", "NVIDIA RTX A2000", "Quadro
 * RTX 4000"), which DLSS runs on, or the launcher does not know which it is. */
static bool gpu_maybe_rtx(void)
{
    const char* g = gpu_chosen();
    if (!g || !*g) return true;
    bool nvidia = false, rtx = false;
    for (const char* p = g; *p; p++) {
        if (!_strnicmp(p, "NVIDIA", 6) || !_strnicmp(p, "Quadro", 6)) nvidia = true;
        if (!_strnicmp(p, "RTX", 3)) rtx = true;
    }
    return nvidia && rtx;
}

/* The AMD FSR upscalers the last sessions used (report.cpp reads their logs), for the ready check's line: the newest
 * one asked for what graphics.fsr_version asks now ("" for auto). Read with the rest of the ready check. */
static GowaFsrSeen s_fsr_seen[4];
static int s_fsr_seen_n;
static const char* fsr_seen_for(const char* asked)
{
    for (int i = 0; i < s_fsr_seen_n; i++)
        if (!strcmp(s_fsr_seen[i].asked, asked)) return s_fsr_seen[i].version;
    return NULL;
}

/* A game patch's option by its id, or NULL. */
static const Opt* popt_find(const char* key)
{
    for (int i = 0; i < s_npopt; i++)
        if (!strcmp(s_popt[i].key, key)) return &s_popt[i];
    return NULL;
}

static void ready_check(void)
{
    s_fsr_seen_n = gowa_fsr_seen(s_fsr_seen, 4);
    game_refresh();
    s_rc.profiles = 0;
    if (saves_dir(s_rc.saves, sizeof s_rc.saves)) {
        /* The profiles of the release the game folder holds: another's (the EU disc's beside the US release's, in one
         * gamedata) are not this game's to load. */
        const GameTitle* t = game_title_find(s_game.g.title_id);
        if (!t) t = game_title_of_folder(s_game.link_vfs);
        char pat[1200];
        snprintf(pat, sizeof pat, "%s\\%s-PROFDATA-*", s_rc.saves, t->saves);
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(pat, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) s_rc.profiles++; } while (FindNextFileA(f, &fd));
            FindClose(f);
        }
    }
    gpu_detect();
    s_rc.pad = pad_connected(s_rc.pad_name, sizeof s_rc.pad_name);
    pb_state_read();
}

/* ---------------------------------------------------------------------------
 * Presets: the patch sets the Play tab offers, and the graphics settings each one sets
 *
 * A preset is the game patches it switches on (the rest off) and the picture settings it gives a value; the three set
 * the same ones, as steps. MLAA is off in all: its picture is never shown, only its cost. None uses AMD FSR or frame
 * generation, which take the frame order without the bloom and the motion blur; AMD FSR 1 upscales without it.
 *   Smooth (this port's defaults, so a first start shows it): the game's 720p sharpened up to the window by FSR 1, SMAA
 *     and 16x filtering, no dynamic shadows or motion blur, no extras.
 *   Balanced: 1080p by FSR 1, the dynamic shadows at 2x, bloom, blur and depth of field at the internal resolution,
 *     ambient occlusion at half of it, debanding; no motion blur.
 *   Ultra: the game's every effect, the window's size with bilinear scaling, every extra at its most, but CAS: it
 *     sharpened the fine texture of the game's fog into a white grain (the doorway at the top of the stairs past the
 *     first fight).
 * -----------------------------------------------------------------------*/
struct Preset { const char* name; const char* desc; const char* on[6]; const char* set[12][3]; };
static const Preset k_presets[] = {
    { "Smooth", "The default and the fastest: 720p sharpened by AMD FSR 1, SMAA; no dynamic shadows or motion blur.",
      { "disable_mlaa", "disable_motion_blur", "disable_shadows", NULL },
      { { "graphics", "render_scale", "1" }, { "graphics", "scaling", "fsr1" }, { "graphics", "sharpening", "high" },
        { "graphics", "frame_generation", "off" }, { "graphics", "antialiasing", "smaa" },
        { "graphics", "anisotropy", "16" }, { "graphics", "shadow_resolution", "1" },
        { "graphics", "effects_resolution", "game" }, { "graphics", "ambient_occlusion", "off" },
        { "graphics", "deband", "off" }, { "graphics", "cas", "off" }, { NULL } } },
    { "Balanced", "1080p by AMD FSR 1, dynamic shadows at 2x, full-size bloom, ambient occlusion; no motion blur.",
      { "disable_mlaa", "disable_motion_blur", NULL },
      { { "graphics", "render_scale", "1.5" }, { "graphics", "scaling", "fsr1" }, { "graphics", "sharpening", "medium" },
        { "graphics", "frame_generation", "off" }, { "graphics", "antialiasing", "smaa" },
        { "graphics", "anisotropy", "16" }, { "graphics", "shadow_resolution", "2" },
        { "graphics", "effects_resolution", "internal" }, { "graphics", "ambient_occlusion", "on" },
        { "graphics", "deband", "on" }, { "graphics", "cas", "off" }, { NULL } } },
    { "Ultra", "Every effect at its most: window resolution, SMAA, 4x shadows, ambient occlusion and bloom. The "
               "slowest.",
      { "disable_mlaa", "disable_mesh_trimming", NULL },
      { { "graphics", "render_scale", "native" }, { "graphics", "scaling", "bilinear" }, { "graphics", "sharpening", "high" },
        { "graphics", "frame_generation", "off" }, { "graphics", "antialiasing", "smaa" },
        { "graphics", "anisotropy", "16" }, { "graphics", "shadow_resolution", "4" },
        { "graphics", "effects_resolution", "internal" }, { "graphics", "ambient_occlusion", "high" },
        { "graphics", "deband", "on" }, { "graphics", "cas", "off" }, { NULL } } },
};
enum { N_PRESET = sizeof k_presets / sizeof k_presets[0] };

static int preset_has(const Preset& p, const char* id)
{
    for (int i = 0; p.on[i]; i++) if (!strcmp(p.on[i], id)) return 1;
    return 0;
}
/* The preset the patches and the settings match, or -1. */
static int preset_now(void)
{
    for (int k = 0; k < N_PRESET; k++) {
        int same = 1;
        for (int i = 0; i < s_npopt && same; i++) same = opt_on(s_popt[i]) == preset_has(k_presets[k], s_popt[i].key);
        for (int i = 0; k_presets[k].set[i][0] && same; i++) {
            const Opt* o = opt_find(k_presets[k].set[i][0], k_presets[k].set[i][1]);
            same = !o || ps3_settings_ieq(o->cur, k_presets[k].set[i][2]);
        }
        if (same) return k;
    }
    return -1;
}
static void preset_apply(int k)
{
    for (int i = 0; i < s_npopt; i++) opt_set(s_popt[i], preset_has(k_presets[k], s_popt[i].key) ? "on" : "off");
    for (int i = 0; k_presets[k].set[i][0]; i++)
        if (Opt* o = opt_find(k_presets[k].set[i][0], k_presets[k].set[i][1])) opt_set(*o, k_presets[k].set[i][2]);
}

/* ---------------------------------------------------------------------------
 * Drawing: Direct2D and DirectWrite, 8-bit and neon
 *
 * Square edges a whole number of device pixels wide, buttons with a bevel, 16x16 pixel icons, the pixel fonts
 * Silkscreen (headings, buttons, captions) with Bahnschrift for the rest, and in the header the precompile screen's scene with
 * each of its pixels a whole number of device pixels. All but the scene is drawn into a layer (layer_draw), again only
 * when something changes; a frame (frame_draw) is the scene, the layer over it and, with Retro effects, the glow added
 * and the scanlines over all. So the scene moves 30 times a second for next to nothing.
 * -----------------------------------------------------------------------*/
static const float WIN_W = 1120.0f, WIN_H = 720.0f, SIDE_W = 220.0f, FOOT_H = 76.0f;
static const float HERO_PLAY = 236.0f, HERO_TAB = 120.0f;   /* the header: on the Play tab, on the others */

/* Colours, 0xRRGGBB */
enum : unsigned {
    C_BG = 0x120A1C, C_SIDE = 0x170D24, C_CARD = 0x1C1029, C_CARD_HI = 0x241436, C_CTL = 0x271839, C_CTL_HI = 0x34214C,
    C_LO = 0x0E0716, C_HI = 0x45305E,       /* a bevel's dark and lit edges */
    C_LINE = 0x2C1A40, C_EDGE = 0x3B2556,   /* rules; the edge of a card or a field */
    C_TEXT = 0xF8E6BC, C_TEXT2 = 0xE8D2A8, C_MUTED = 0xCDB8D8, C_DIM = 0x8C7398, C_OFF = 0x5A4270,
    C_GOLD = 0xFFCC6A, C_GOLD_HI = 0xFFDC96, C_INK = 0x1A0F28,   /* what is chosen, and the writing on it */
    C_NEON = 0xFF4FA8, C_CYAN = 0x4FE9FF,
    C_OK = 0x7DF0A0, C_WARN = 0xFF9A5A, C_WARN_T = 0xFFB27A, C_BAD = 0xFF6B7A,
};

static ID2D1Factory* s_d2d;
static IDWriteFactory* s_dw;
static IWICImagingFactory* s_wic;
static IDWriteFactory5* s_dw5;                      /* Windows 10 1709 on: the fonts read from memory */
static IDWriteInMemoryFontFileLoader* s_font_mem;
static IDWriteFontCollection1* s_fonts;             /* Silkscreen; NULL: none */
static IDWriteTextFormat *s_tf_ui, *s_tf_ui_b, *s_tf_small, *s_tf_cap, *s_tf_head, *s_tf_btn, *s_tf_title, *s_tf_play;
static IDWriteTextFormat* s_tf_desc;   /* s_tf_small from the top of its box: the lines under labels */
static IDWriteTextFormat* s_tf_path;   /* s_tf_small on one line, cut before its last two folders */

/* What the render target owns; made again with it. */
static ID2D1RenderTarget* s_rt;                     /* the window's, or the shots' */
static ID2D1RenderTarget* s_d;                      /* what is drawn on: the layer while it is drawn, else s_rt */
static ID2D1SolidColorBrush* s_br;
static ID2D1BitmapRenderTarget* s_layer;            /* all but the scene, clear where the scene shows */
static ID2D1DeviceContext* s_dc;                    /* s_rt's, to add the glow; none before Windows 8 */
static ID2D1BitmapRenderTarget *s_glow_src, *s_glow;   /* what glows; it blurred */
static ID2D1Effect *s_blur_near, *s_blur_far;       /* two widths of glow */
static ID2D1Bitmap *s_bm_scene, *s_bm_fx, *s_bm_icon, *s_bm_word, *s_bm_word_glow;
static float s_px = 1.0f;                           /* device pixels a DIP */
static int s_layer_old = 1;                         /* the layer is to be drawn again */
static ULONGLONG s_t0;                              /* when the window opened: the scene's clock */
static int s_anim;                                  /* the scene moves */

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

/* launcher.style = clean (GOWA_LAUNCHER_STYLE over it): headings in the body's font, antialiased shapes with rounded
 * corners, Windows' own vector icons, no scanlines or glow. retro: Silkscreen, 8-bit bevels, pixel icons, and Retro
 * effects as set. The header's scene stays pixel art in both, drawn at whole device pixels. Read from the row as shown,
 * so a choice changes the window at once (fonts_make). */
static IDWriteTextFormat *s_tf_icon[4], *s_tf_name;   /* the clean style's icons (by size) and its name on Play */
static int clean(void)
{
    static int env = -2;
    static const Opt* o;
    if (env == -2) {
        const char* e = getenv("GOWA_LAUNCHER_STYLE");
        env = e && *e ? !ps3_settings_ieq(e, "retro") : -1;
        o = opt_find("launcher", "style");
    }
    if (env >= 0) return env;
    return !o || !ps3_settings_ieq(o->cur, "retro");
}

/* Silkscreen, in the exe (app.rc), read from memory into a collection of its own. Without it (Windows 10 before
 * 1709, a resource missing) headings are in the body's font too. The body is Windows' own Bahnschrift, DIN-like
 * and easy to read at 14 and 15 (a pixel font there makes a 5 an S); Segoe UI before Windows 10 1709. */
enum { RES_SILKSCREEN = 101, RES_SILKSCREEN_BOLD = 102 };
static const wchar_t* s_f_pixel = L"Segoe UI";   /* Silkscreen, once read */

static void fonts_load(void)
{
    IDWriteFontSetBuilder1* b = NULL;
    IDWriteFontSet* set = NULL;
    int n = 0;
    if (FAILED(s_dw->QueryInterface(__uuidof(IDWriteFactory5), (void**)&s_dw5))) s_dw5 = NULL;
    if (s_dw5 && SUCCEEDED(s_dw5->CreateInMemoryFontFileLoader(&s_font_mem)) &&
        SUCCEEDED(s_dw5->RegisterFontFileLoader(s_font_mem)) && SUCCEEDED(s_dw5->CreateFontSetBuilder(&b))) {
        const int ids[] = { RES_SILKSCREEN, RES_SILKSCREEN_BOLD };
        for (int i = 0; i < 2; i++) {
            HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(ids[i]), MAKEINTRESOURCEW(10));   /* RT_RCDATA */
            HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
            const void* p = g ? LockResource(g) : NULL;
            const DWORD size = r ? SizeofResource(NULL, r) : 0;
            IDWriteFontFile* f = NULL;
            /* The exe's own bytes, there as long as the program is: the loader keeps no copy. */
            if (p && size && SUCCEEDED(s_font_mem->CreateInMemoryFontFileReference(s_dw5, p, size, NULL, &f))) {
                if (SUCCEEDED(b->AddFontFile(f))) n++;
                f->Release();
            }
        }
        if (n == 2 && SUCCEEDED(b->CreateFontSet(&set)) && SUCCEEDED(s_dw5->CreateFontCollectionFromFontSet(set, &s_fonts)))
            s_f_pixel = L"Silkscreen";
        else
            s_fonts = NULL;
    }
    if (set) set->Release();
    if (b) b->Release();
}

/* Whether Windows has a font family. */
static int system_font(const wchar_t* family)
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

/* A text format; Silkscreen's from the exe's collection, the rest from Windows'. */
static IDWriteTextFormat* tf(const wchar_t* family, float px, DWRITE_FONT_WEIGHT wt)
{
    IDWriteTextFormat* f = NULL;
    IDWriteFontCollection* c = s_fonts && family == s_f_pixel ? s_fonts : NULL;
    s_dw->CreateTextFormat(family, c, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", &f);
    if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return f;
}

static int fonts_make(void);
static int ui_init(void)
{
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &s_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&s_dw)))
        return 0;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_wic));
    fonts_load();
    return fonts_make();
}

/* The text formats, for the style shown (s_font_style); made again when it changes. */
static int s_font_style = -1;
static int fonts_make(void)
{
    IDWriteTextFormat** old[] = { &s_tf_ui, &s_tf_ui_b, &s_tf_small, &s_tf_cap, &s_tf_head, &s_tf_btn, &s_tf_title,
                                  &s_tf_play, &s_tf_desc, &s_tf_path };
    for (size_t i = 0; i < sizeof old / sizeof old[0]; i++)
        if (*old[i]) { (*old[i])->Release(); *old[i] = NULL; }
    s_font_style = clean();
    /* Silkscreen's dots are an eighth of its size: at 8, 16 and 24 they are whole pixels at 100%, 150% and 200%. */
    const wchar_t *pix = s_f_pixel, *body = system_font(L"Bahnschrift") ? L"Bahnschrift" : L"Segoe UI";
    s_tf_ui = tf(body, 15.0f, DWRITE_FONT_WEIGHT_NORMAL);
    s_tf_ui_b = tf(body, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    s_tf_small = tf(body, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
    s_tf_desc = tf(body, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
    s_tf_path = tf(body, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
    if (clean()) {   /* the headings in the body's font, heavier: no pixel font */
        s_tf_cap = tf(body, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        s_tf_head = tf(body, 19.0f, DWRITE_FONT_WEIGHT_BOLD);
        s_tf_btn = tf(body, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        s_tf_title = tf(body, 26.0f, DWRITE_FONT_WEIGHT_BOLD);
        s_tf_play = tf(body, 24.0f, DWRITE_FONT_WEIGHT_BOLD);
    } else {
        s_tf_cap = tf(pix, s_fonts ? 8.0f : 11.0f, DWRITE_FONT_WEIGHT_BOLD);
        s_tf_head = tf(pix, 16.0f, DWRITE_FONT_WEIGHT_BOLD);
        s_tf_btn = tf(pix, 16.0f, DWRITE_FONT_WEIGHT_NORMAL);
        s_tf_title = tf(pix, 24.0f, DWRITE_FONT_WEIGHT_BOLD);
        s_tf_play = tf(pix, 24.0f, DWRITE_FONT_WEIGHT_BOLD);
    }
    if (!s_tf_ui || !s_tf_ui_b || !s_tf_small || !s_tf_cap || !s_tf_head || !s_tf_btn || !s_tf_title || !s_tf_play ||
        !s_tf_desc || !s_tf_path)
        return 0;
    s_tf_desc->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    {
        DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* dots = NULL;
        s_dw->CreateEllipsisTrimmingSign(s_tf_ui, &dots);
        IDWriteTextFormat* one_line[] = { s_tf_ui, s_tf_ui_b, s_tf_cap, s_tf_head, s_tf_btn, s_tf_title, s_tf_play };
        for (size_t i = 0; i < sizeof one_line / sizeof one_line[0]; i++) {
            one_line[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (dots) one_line[i]->SetTrimming(&trim, dots);
        }
        /* A path keeps its end: "C:\...\build\ascension-r.ini". */
        DWRITE_TRIMMING at_folder = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, L'\\', 2 };
        s_tf_path->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (dots) s_tf_path->SetTrimming(&at_folder, dots);
        if (dots) dots->Release();
    }
    return 1;
}

static D2D1_RECT_F R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
/* On the device's pixels: what is drawn has hard edges, in whole pixels. */
static float snap(float v) { return floorf(v * s_px + 0.5f) / s_px; }
static D2D1_RECT_F SR(D2D1_RECT_F r) { return D2D1::RectF(snap(r.left), snap(r.top), snap(r.right), snap(r.bottom)); }
/* n DIPs in whole device pixels, at least one: the dots of what is drawn in pixels. Up from three quarters, so a 16
 * pixel icon stays 16 pixels at 150%, not 32. */
static int dots(float n)
{
    const int d = (int)floorf(n * s_px + 0.25f);
    return d < 1 ? 1 : d;
}
static float thick(float n) { return (float)dots(n) / s_px; }

static int effects_on(void)
{
    if (clean()) return 0;   /* scanlines and glow are the retro style's */
    static const Opt* o = opt_find("launcher", "effects");
    return !o || opt_on(*o);
}

/* The scene's clock: still, at a moment without lightning, when it does not move (shots, Windows' animations off). */
static ULONGLONG scene_ms(void) { return s_anim ? GetTickCount64() - s_t0 : 4000; }

/* "ASCENSION-R" in the precompile screen's pixel font as the banner has it (scripts/brand/brand.cpp): gold down its
 * rows, a dark outline a dot round it, a shadow under. A bitmap pixel a dot. glow: the dots alone, in neon. */
enum { WORD_W = 67, WORD_H = 10 };
static ID2D1Bitmap* word_bitmap(int glow)
{
    static const uint32_t rows[7] = { 0xFFF6DA, 0xFFE9A8, 0xFFD680, 0xFFC064, 0xFCA450, 0xF48640, 0xE2683A };
    static const char name[] = "ASCENSION-R";
    static uint32_t px[WORD_W * WORD_H];
    memset(px, 0, sizeof px);
    for (int pass = glow ? 2 : 0; pass < 3; pass++)
        for (int i = 0; name[i]; i++) {
            const uint8_t* g = gowa_pixel_glyph(name[i]);
            if (!g) continue;
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++) {
                    if (!(g[r] & (0x10 >> b))) continue;
                    const int x = 1 + i * 6 + b, y = 1 + r;
                    if (pass == 0) px[(y + 2) * WORD_W + x + 1] = 0xFF0E0410u;
                    else if (pass == 1)
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) px[(y + dy) * WORD_W + x + dx] = 0xFF2A0A22u;
                    else px[y * WORD_W + x] = 0xFF000000u | (glow ? (uint32_t)C_NEON : rows[r]);
                }
        }
    ID2D1Bitmap* bm = NULL;
    s_rt->CreateBitmap(D2D1::SizeU(WORD_W, WORD_H), px, WORD_W * 4,
                       D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                       &bm);
    return bm;
}
/* Where the word goes at (x, y), each dot about n DIPs. */
static D2D1_RECT_F word_rect(float x, float y, float n)
{
    const float d = (float)dots(n) / s_px;
    return D2D1::RectF(snap(x), snap(y), snap(x) + WORD_W * d, snap(y) + WORD_H * d);
}

/* The program's icon (app.ico), at the size of its own drawings nearest 32 DIPs: one of its pixels a device pixel. */
static ID2D1Bitmap* icon_bitmap(void)
{
    static const int have[] = { 16, 32, 48, 64 };
    const int want = (int)(32.0f * s_px + 0.5f);
    int n = have[0];
    for (int i = 1; i < 4; i++) if (abs(have[i] - want) < abs(n - want)) n = have[i];
    HICON hi = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, n, n, 0);
    IWICBitmap* wb = NULL;
    IWICFormatConverter* cv = NULL;
    ID2D1Bitmap* bm = NULL;
    if (hi && s_wic && SUCCEEDED(s_wic->CreateBitmapFromHICON(hi, &wb)) && SUCCEEDED(s_wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                                 WICBitmapPaletteTypeMedianCut))) {
        const float dpi = 96.0f * s_px;
        s_rt->CreateBitmapFromWicBitmap(
            cv, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi),
            &bm);
    }
    if (cv) cv->Release();
    if (wb) wb->Release();
    if (hi) DestroyIcon(hi);
    return bm;
}

/* Retro effects' overlay: a dark line every third device pixel or so, and the corners darker. Black, its alpha only. */
static void fx_make(void)
{
    const UINT pw = (UINT)(WIN_W * s_px + 0.5f), ph = (UINT)(WIN_H * s_px + 0.5f);
    uint32_t* p = (uint32_t*)malloc((size_t)pw * ph * 4);
    if (!p) return;
    const int line = dots(1.0f), period = line * 3;
    for (UINT y = 0; y < ph; y++) {
        const float scan = (int)(y % (UINT)period) < line ? 0.16f : 0.0f;
        const float dy = ((float)y + 0.5f) / (float)ph - 0.5f;
        for (UINT x = 0; x < pw; x++) {
            const float dx = ((float)x + 0.5f) / (float)pw - 0.5f;
            float v = (sqrtf(dx * dx + dy * dy) * 1.41421356f - 0.55f) / 0.45f;   /* 0 near the middle, 1 in a corner */
            v = v < 0.0f ? 0.0f : v * v * 0.35f;
            const float a = 1.0f - (1.0f - scan) * (1.0f - v);
            p[(size_t)y * pw + x] = (uint32_t)(a * 255.0f + 0.5f) << 24;
        }
    }
    const float dpi = 96.0f * s_px;
    s_rt->CreateBitmap(D2D1::SizeU(pw, ph), p, pw * 4,
                       D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi),
                       &s_bm_fx);
    free(p);
}

template <class T> static void drop(T*& p)
{
    if (p) p->Release();
    p = NULL;
}

static void tro_release(void);   /* the Trophies tab's pictures */

static void rt_release(void)
{
    tro_release();
    drop(s_blur_near);
    drop(s_blur_far);
    drop(s_glow);
    drop(s_glow_src);
    drop(s_dc);
    drop(s_layer);
    drop(s_bm_scene);
    drop(s_bm_fx);
    drop(s_bm_icon);
    drop(s_bm_word);
    drop(s_bm_word_glow);
    drop(s_br);
    drop(s_rt);
    s_d = NULL;
}

/* What a new target needs: the layer, the glow's targets and blurs (Windows 8 on), the bitmaps. */
static void rt_resources(void)
{
    float dx = 96.0f, dy = 96.0f;
    s_rt->GetDpi(&dx, &dy);
    s_px = dx / 96.0f;
    s_d = s_rt;
    s_rt->CreateSolidColorBrush(col(C_TEXT), &s_br);
    if (FAILED(s_rt->CreateCompatibleRenderTarget(&s_layer))) s_layer = NULL;
    if (FAILED(s_rt->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&s_dc))) s_dc = NULL;
    if (s_layer && s_dc && SUCCEEDED(s_rt->CreateCompatibleRenderTarget(&s_glow_src)) &&
        SUCCEEDED(s_rt->CreateCompatibleRenderTarget(&s_glow))) {
        ID2D1DeviceContext* g = NULL;
        ID2D1Bitmap* src = NULL;
        if (SUCCEEDED(s_glow->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&g)) && SUCCEEDED(s_glow_src->GetBitmap(&src))) {
            ID2D1Effect** fx[2] = { &s_blur_near, &s_blur_far };
            const float sd[2] = { 3.0f, 11.0f };   /* DIPs */
            for (int i = 0; i < 2; i++) {
                if (FAILED(g->CreateEffect(CLSID_D2D1GaussianBlur, fx[i]))) { *fx[i] = NULL; continue; }
                (*fx[i])->SetInput(0, src);
                (*fx[i])->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sd[i]);
            }
        }
        if (src) src->Release();
        if (g) g->Release();
    }
    s_rt->CreateBitmap(D2D1::SizeU(GOWA_SCENE_W, GOWA_SCENE_H), gowa_scene_frame(scene_ms()), GOWA_SCENE_W * 4,
                       D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                       &s_bm_scene);
    s_bm_word = word_bitmap(0);
    s_bm_word_glow = word_bitmap(1);
    s_bm_icon = icon_bitmap();
    fx_make();
    s_layer_old = 1;
}

/* The scene's next frame, into its bitmap. */
static void scene_tick(void)
{
    if (s_bm_scene) s_bm_scene->CopyFromMemory(NULL, gowa_scene_frame(scene_ms()), GOWA_SCENE_W * 4);
}

static void fill(D2D1_RECT_F r, unsigned rgb, float a = 1.0f)
{
    s_br->SetColor(col(rgb, a));
    s_d->FillRectangle(SR(r), s_br);
}
/* A frame w DIPs wide (whole device pixels) inside r. */
static void frame(D2D1_RECT_F r, unsigned rgb, float a = 1.0f, float w = 2.0f)
{
    r = SR(r);
    const float t = thick(w);
    s_br->SetColor(col(rgb, a));
    if (clean()) {
        const float h = t * 0.5f;
        s_d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), 6.0f, 6.0f),
                                  s_br, t);
        return;
    }
    s_d->FillRectangle(D2D1::RectF(r.left, r.top, r.right, r.top + t), s_br);
    s_d->FillRectangle(D2D1::RectF(r.left, r.bottom - t, r.right, r.bottom), s_br);
    s_d->FillRectangle(D2D1::RectF(r.left, r.top + t, r.left + t, r.bottom - t), s_br);
    s_d->FillRectangle(D2D1::RectF(r.right - t, r.top + t, r.right, r.bottom - t), s_br);
}
/* A block with an 8-bit bevel: lit along the top and left, dark along the bottom and right; pressed, the other way. */
static void bevel(D2D1_RECT_F r, unsigned face, unsigned hi, unsigned lo, float w, float a_hi, float a_lo, int pressed)
{
    r = SR(r);
    const float t = thick(w);
    if (pressed) {
        const unsigned c = hi; hi = lo; lo = c;
        const float a = a_hi; a_hi = a_lo; a_lo = a;
    }
    if (clean()) {   /* a rounded block, its edge a device pixel of the lit colour */
        const float px1 = 1.0f / s_px, h = px1 * 0.5f;
        s_br->SetColor(col(face, 1.0f));
        s_d->FillRoundedRectangle(D2D1::RoundedRect(r, 5.0f, 5.0f), s_br);
        s_br->SetColor(col(pressed ? lo : hi, (pressed ? a_lo : a_hi) * 0.7f));
        s_d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), 5.0f, 5.0f),
                                  s_br, px1);
        return;
    }
    fill(r, face);
    s_br->SetColor(col(hi, a_hi));
    s_d->FillRectangle(D2D1::RectF(r.left, r.top, r.right - t, r.top + t), s_br);
    s_d->FillRectangle(D2D1::RectF(r.left, r.top + t, r.left + t, r.bottom - t), s_br);
    s_br->SetColor(col(lo, a_lo));
    s_d->FillRectangle(D2D1::RectF(r.left + t, r.bottom - t, r.right, r.bottom), s_br);
    s_d->FillRectangle(D2D1::RectF(r.right - t, r.top + t, r.right, r.bottom - t), s_br);
}

/* Silkscreen at a whole number of device pixels a dot is drawn unsmoothed: its dots stay square. */
static int sharp(IDWriteTextFormat* f)
{
    if (clean() || !s_fonts || (f != s_tf_cap && f != s_tf_head && f != s_tf_btn && f != s_tf_title && f != s_tf_play))
        return 0;
    if (!strncmp(i18n_code(), "zh", 2) || !strcmp(i18n_code(), "ru")) return 0;   /* other fonts' letters */
    const float dot = f->GetFontSize() * s_px / 8.0f;
    return fabsf(dot - floorf(dot + 0.5f)) < 0.01f;
}
/* A layout of s in f, in a box w x h. */
static IDWriteTextLayout* layout(const wchar_t* s, IDWriteTextFormat* f, float w, float h)
{
    IDWriteTextLayout* l = NULL;
    s = TW(s);   /* the language shown: its translation of the English, or the English (i18n.cpp) */
    const UINT32 n = (UINT32)wcslen(s);
    if (FAILED(s_dw->CreateTextLayout(s, n, f, w, h, &l))) return NULL;
    /* The font Windows falls back to for Han characters follows the locale: Microsoft YaHei for zh-CN, JhengHei for
     * zh-TW; with en-us it would be the simplified forms in both. */
    const char* c = i18n_code();
    if (!strcmp(c, "zh-s")) l->SetLocaleName(L"zh-cn", DWRITE_TEXT_RANGE{ 0, n });
    else if (!strcmp(c, "zh-t")) l->SetLocaleName(L"zh-tw", DWRITE_TEXT_RANGE{ 0, n });
    else if (!strcmp(c, "ru")) l->SetLocaleName(L"ru-ru", DWRITE_TEXT_RANGE{ 0, n });
    return l;
}
/* Text in a box, its first pixel on a whole device pixel. */
static void text(const wchar_t* s, IDWriteTextFormat* f, D2D1_RECT_F r, unsigned rgb, float a = 1.0f,
                 DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING)
{
    const float w = r.right - r.left, h = r.bottom - r.top;
    IDWriteTextLayout* l = NULL;
    if (!s || !*s || w <= 0.0f || h <= 0.0f) return;
    f->SetTextAlignment(al);
    if (!(l = layout(s, f, w, h))) return;
    DWRITE_TEXT_METRICS m;
    float x = snap(r.left);
    if (SUCCEEDED(l->GetMetrics(&m))) {
        x = snap(r.left + m.left) - m.left;
        /* Shots: a text its box cuts (a translation longer than the English, scripts/lang.py) is named in the log. */
        static const int shots = getenv("GOWA_LAUNCHER_SHOT") != NULL;
        const float line = m.lineCount ? m.height / (float)m.lineCount : m.height;   /* a heading's box is a line's ink */
        float whole = 0.0f;   /* a one-line text's own width: wider than its box, it is cut to "..." */
        if (shots && f != s_tf_path && f->GetWordWrapping() == DWRITE_WORD_WRAPPING_NO_WRAP) {
            IDWriteTextLayout* u = layout(s, f, 4096.0f, h);
            DWRITE_TEXT_METRICS um;
            if (u && SUCCEEDED(u->GetMetrics(&um))) whole = um.width;
            if (u) u->Release();
        }
        if (shots && (m.height - h > 0.5f * line || m.width > w + 1.0f || whole > w + 1.0f)) {
            char u[1600];
            if (!WideCharToMultiByte(CP_UTF8, 0, TW(s), -1, u, sizeof u, NULL, NULL)) u[0] = 0;
            fprintf(stderr, "[launcher] does not fit (%.0fx%.0f in %.0fx%.0f): %s\n", m.width, m.height, w, h, u);
        }
    }
    s_d->SetTextAntialiasMode(sharp(f) ? D2D1_TEXT_ANTIALIAS_MODE_ALIASED : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    s_br->SetColor(col(rgb, a));
    s_d->DrawTextLayout(D2D1::Point2F(x, r.top), l, s_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    l->Release();
}
/* How wide a text is on one line. */
static float text_w(const wchar_t* s, IDWriteTextFormat* f)
{
    IDWriteTextLayout* l = layout(s, f, 4096.0f, 256.0f);
    float w = 0.0f;
    if (l) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    return w;
}
/* Upper case, into one of a few buffers that rotate: Silkscreen's capitals. */
static const wchar_t* UP(const wchar_t* s)
{
    static wchar_t buf[4][160];
    static int k;
    wchar_t* b = buf[k++ & 3];
    size_t i = 0;
    s = TW(s);   /* translated before its capitals */
    for (; s[i] && i < 159; i++) b[i] = s[i];
    b[i] = 0;
    CharUpperBuffW(b, (DWORD)i);   /* every script's capitals, as Windows has them */
    return b;
}

/* 16x16 icons, '#' a dot, a dot a DIP: whole pixels at 100% and 200%; between, their two-dot lines stay even (3
 * pixels at 150%) and a one-dot detail comes out one pixel or two. */
typedef const char (*Icon)[17];
static const char k_ic_play[16][17] = {
    "................", "....##..........", "....##..........", "....####........", "....####........",
    "....######......", "....######......", "....########....", "....########....", "....######......",
    "....######......", "....####........", "....####........", "....##..........", "....##..........",
    "................" };
static const char k_ic_pic[16][17] = {
    "................", "................", ".##############.", ".##############.", ".##..........##.",
    ".##......##..##.", ".##......##..##.", ".##..........##.", ".##...##.....##.", ".##..####..#.##.",
    ".##.######.####.", ".##############.", ".##############.", "................", "................",
    "................" };
static const char k_ic_screen[16][17] = {
    "................", ".##############.", ".##############.", ".##..........##.", ".##..........##.",
    ".##..........##.", ".##..........##.", ".##..........##.", ".##############.", ".##############.",
    "................", ".......##.......", ".......##.......", "....########....", "....########....",
    "................" };
static const char k_ic_pad[16][17] = {
    "................", "................", "................", "..############..", ".##############.",
    "####.######.####", "###...####.#.###", "####.######.####", "################", "######....######",
    "####........####", ".##..........##.", "................", "................", "................",
    "................" };
static const char k_ic_wrench[16][17] = {
    "................", "..........####..", ".........##.....", "........##...##.", "........##..###.",
    "........#######.", ".......######...", "......###.......", ".....###........", "....###.........",
    "...###..........", "..###...........", ".###............", ".##.............", "................",
    "................" };
static const char k_ic_bolt[16][17] = {
    "................", "................", "........####....", ".......####.....", "......####......",
    ".....####.......", "....#########...", "....########....", ".......####.....", "......####......",
    ".....####.......", "....###.........", "...##...........", "..#.............", "................",
    "................" };
static const char k_ic_chip[16][17] = {
    "................", ".....##..##.....", ".....##..##.....", "..############..", "..##........##..",
    "####.######.####", "####.######.####", "..##.##..##.##..", "..##.##..##.##..", "####.######.####",
    "####.######.####", "..##........##..", "..############..", ".....##..##.....", ".....##..##.....",
    "................" };
static const char k_ic_net[16][17] = {   /* a globe */
    "................", ".....######.....", "...##..##..##...", "..##...##...##..", ".##....##....##.",
    ".##############.", ".##....##....##.", ".##....##....##.", ".##############.", ".##....##....##.",
    "..##...##...##..", "...##..##..##...", ".....######.....", "................", "................",
    "................" };
static const char k_ic_cup[16][17] = {
    "................", "...##########...", ".##############.", ".##.########.##.", ".##.########.##.",
    "..#.########.#..", "...##########...", ".....######.....", "......####......", ".......##.......",
    ".......##.......", "......####......", ".....######.....", "....########....", "....########....",
    "................" };
static const char k_ic_flag[16][17] = {
    "................", "..##............", "..############..", "..###########...", "..##########....",
    "..###########...", "..############..", "..##............", "..##............", "..##............",
    "..##............", "..##............", "..##............", "..##............", "..##............",
    "................" };
static const char k_ic_trash[16][17] = {
    "................", "......####......", "......#..#......", ".##############.", ".##############.",
    "..############..", "..##.##..##.##..", "..##.##..##.##..", "..##.##..##.##..", "..##.##..##.##..",
    "..##.##..##.##..", "..##.##..##.##..", "..############..", "...##########...", "................",
    "................" };
static const char k_ic_mods[16][17] = {   /* a piece of a puzzle */
    "................", ".....###........", "....#####.......", "....#####.......", ".###########....",
    ".###########....", ".###########....", ".#############..", "...############.", "...############.",
    "...############.", ".#############..", ".###########....", ".###########....", ".###########....",
    "................" };
static const char k_ic_raise[16][17] = {   /* k_ic_down upside down */
    "................", "................", "................", "................", ".......##.......",
    "......####......", ".....######.....", "....###..###....", "...###....###...", "..###......###..",
    "..##........##..", "................", "................", "................", "................",
    "................" };
static const char k_ic_folder[16][17] = {
    "................", "................", ".#####..........", ".######.........", ".##############.",
    ".##############.", ".##..........##.", ".##..........##.", ".##..........##.", ".##..........##.",
    ".##..........##.", ".##############.", ".##############.", "................", "................",
    "................" };
static const char k_ic_check[16][17] = {
    "................", "................", "................", "................", "............##..",
    "...........###..", "..##......###...", "..###....###....", "...###..###.....", "....######......",
    ".....####.......", "......##........", "................", "................", "................",
    "................" };
static const char k_ic_warn[16][17] = {
    "................", "................", ".......##.......", "......####......", "......####......",
    ".....######.....", ".....##..##.....", "....###..###....", "....###..###....", "...####..####...",
    "...##########...", "..#####..#####..", "..############..", ".##############.", "................",
    "................" };
static const char k_ic_info[16][17] = {
    "................", "................", ".....######.....", "...##########...", "..#####..#####..",
    ".##############.", ".######..######.", ".######..######.", ".######..######.", ".######..######.",
    "..#####..#####..", "...##########...", ".....######.....", "................", "................",
    "................" };
static const char k_ic_cross[16][17] = {
    "................", "................", "................", "...##......##...", "...###....###...",
    "....###..###....", ".....######.....", "......####......", ".....######.....", "....###..###....",
    "...###....###...", "...##......##...", "................", "................", "................",
    "................" };
static const char k_ic_down[16][17] = {
    "................", "................", "................", "................", "................",
    "..##........##..", "..###......###..", "...###....###...", "....###..###....", ".....######.....",
    "......####......", ".......##.......", "................", "................", "................",
    "................" };
static const char k_ic_keys[16][17] = {
    "................", "................", "################", "################", "##............##",
    "##.#.#.#.#.#..##", "##............##", "##..#.#.#.#.#.##", "##............##", "##..########..##",
    "##............##", "################", "################", "................", "................",
    "................" };
static const char k_ic_search[16][17] = {
    "................", "....####........", "..########......", ".###....###.....", ".##......##.....",
    ".##......##.....", ".##......##.....", ".###....###.....", "..########......", "....####.##.....",
    ".........###....", "..........###...", "...........###..", "............##..", "................",
    "................" };
static const char k_ic_up[16][17] = {   /* a newer release */
    "................", ".......##.......", "......####......", ".....######.....", "....###..###....",
    "...###.##.###...", "..###..##..###..", ".......##.......", ".......##.......", ".......##.......",
    ".......##.......", "................", "..############..", "..############..", "................",
    "................" };

/* Each pixel icon's counterpart in Windows' icon font (Segoe Fluent Icons, Segoe MDL2 Assets), for the clean style. */
static const wchar_t* icon_glyph(Icon rows)
{
    static const struct { Icon ic; const wchar_t* g; } k[] = {
        { k_ic_play, L"\xE768" }, { k_ic_pic, L"\xE8B9" }, { k_ic_screen, L"\xE7F4" }, { k_ic_pad, L"\xE7FC" },
        { k_ic_wrench, L"\xE90F" }, { k_ic_bolt, L"\xE945" }, { k_ic_chip, L"\xE770" }, { k_ic_net, L"\xE774" },
        { k_ic_cup, L"\xE734" }, { k_ic_flag, L"\xE7C1" }, { k_ic_trash, L"\xE74D" }, { k_ic_mods, L"\xEA86" },
        { k_ic_raise, L"\xE70E" }, { k_ic_folder, L"\xE8B7" }, { k_ic_check, L"\xE73E" }, { k_ic_warn, L"\xE7BA" },
        { k_ic_info, L"\xE946" }, { k_ic_cross, L"\xE711" }, { k_ic_down, L"\xE70D" }, { k_ic_keys, L"\xE765" },
        { k_ic_search, L"\xE721" }, { k_ic_up, L"\xE898" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].ic == rows) return k[i].g;
    return NULL;
}

/* An icon with its top left at (x, y), k DIPs a dot (16 x 16 DIPs at 1), its runs of dots filled unsmoothed (the
 * targets draw aliased). */
static void icon(Icon rows, float x, float y, unsigned rgb, float a = 1.0f, float k = 1.0f)
{
    if (clean()) {   /* the same icon from Windows' icon font, 16 DIPs a k */
        IDWriteTextFormat** fmt = s_tf_icon;
        const int i = k <= 1.0f ? 0 : k <= 1.5f ? 1 : k <= 2.0f ? 2 : 3;
        if (!fmt[i]) {
            const wchar_t* fam = system_font(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
            fmt[i] = tf(fam, 15.0f * k, DWRITE_FONT_WEIGHT_NORMAL);
        }
        const wchar_t* g = icon_glyph(rows);
        if (fmt[i] && g) {
            text(g, fmt[i], D2D1::RectF(x, y, x + 16.0f * k, y + 16.0f * k), rgb, a, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
    }
    const float ox = snap(x), oy = snap(y);
    s_br->SetColor(col(rgb, a));
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16;) {
            if (rows[r][c] != '#') { c++; continue; }
            int e = c;
            while (e < 16 && rows[r][e] == '#') e++;
            s_d->FillRectangle(D2D1::RectF(ox + (float)c * k, oy + (float)r * k, ox + (float)e * k, oy + (float)(r + 1) * k), s_br);
            c = e;
        }
}

/* How tall a text is, wrapped at a width. */
static float text_h(const wchar_t* s, IDWriteTextFormat* f, float w)
{
    IDWriteTextLayout* l = layout(s, f, w, 1000.0f);
    float h = 0.0f;
    if (l) {
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
    H_NAV = 1000, H_SEG = 2000, H_SW = 5000, H_DD = 6000, H_PSW = 7000, H_PRESET = 7500, H_TXT = 7700,
    H_PLAY = 8000, H_SAVE, H_DEFAULTS, H_QUIT, H_KEYS, H_SAVES, H_EXPORT_SAVES, H_IMPORT_SAVES, H_SHADERS, H_GAME_BROWSE, H_GAME_AUTO, H_REPORT,
    H_PICK_RPCS3, H_GET_RPCS3, H_PICK_RAP, H_PREP_RETRY, H_PREP_STOP, H_KB_OK, H_KB_CANCEL, H_KB_DEFAULTS,
    H_UPDATE_OPEN, H_UPDATE_CLOSE, H_PREBUILD, H_MODS_OPEN, H_ANR_ZIP, H_ANR_STOP, H_DLSS_GET, H_DLSS_STOP, H_SUPPORT,
#ifdef PORT_GAME_MODULE
    H_KIT_BUILD, H_KIT_STOP, H_KIT_LOG,
#endif
    H_KBIND = 8500,   /* the keyboard keys' bindings */
    H_DDITEM = 9000,
    H_MOD = 10000,    /* the Mods tab: a mod's switch, up and down (H_MOD + 3 * its place + 0, 1, 2) */
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

/* --- the glow --------------------------------------------------------------
 * What glows is listed while the layer is drawn, then drawn into a layer of its own, blurred twice and added over
 * the frame (glow_draw): neon. Only with Retro effects. */
struct Glow {
    D2D1_RECT_F r, clip;
    unsigned rgb;
    float a, w;                 /* w: a frame that wide; 0, the box filled */
    IDWriteTextFormat* f;       /* else s, in f */
    DWRITE_TEXT_ALIGNMENT al;
    ID2D1Bitmap* bm;            /* else bm, over r */
    wchar_t s[40];
};
static Glow s_glows[64];
static int s_nglow;
static D2D1_RECT_F s_holes[16];   /* what the glow goes round but not over: the gold buttons */
static int s_nhole;

static Glow* glow_add(D2D1_RECT_F r, unsigned rgb, float a)
{
    if (s_nglow >= (int)(sizeof s_glows / sizeof s_glows[0])) return NULL;
    Glow* g = &s_glows[s_nglow++];
    memset(g, 0, sizeof *g);
    g->r = r;
    g->clip = s_clip;
    g->rgb = rgb;
    g->a = a;
    return g;
}
static void glow_rect(D2D1_RECT_F r, unsigned rgb, float a, float w = 0.0f)
{
    if (Glow* g = glow_add(r, rgb, a)) g->w = w;
}
static void glow_text(const wchar_t* s, IDWriteTextFormat* f, D2D1_RECT_F r, unsigned rgb, float a,
                      DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING)
{
    if (Glow* g = glow_add(r, rgb, a)) {
        g->f = f;
        g->al = al;
        swprintf(g->s, sizeof g->s / sizeof g->s[0], L"%ls", s);
    }
}
static void glow_bitmap(ID2D1Bitmap* bm, D2D1_RECT_F r, float a)
{
    if (!bm) return;
    if (Glow* g = glow_add(r, 0, a)) g->bm = bm;
}
static void glow_hole(D2D1_RECT_F r)
{
    if (s_nhole < (int)(sizeof s_holes / sizeof s_holes[0])) s_holes[s_nhole++] = r;
}

static void glow_draw(void)
{
    if (!s_glow_src || !s_glow || !s_blur_near) return;
    ID2D1RenderTarget* keep = s_d;
    s_d = s_glow_src;
    s_d->BeginDraw();
    s_d->SetTransform(D2D1::Matrix3x2F::Identity());
    s_d->SetAntialiasMode(clean() ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
    s_d->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (int i = 0; i < s_nglow; i++) {
        const Glow& g = s_glows[i];
        const int clipped = g.clip.left > NO_CLIP.left;   /* in a card that scrolls */
        if (clipped) s_d->PushAxisAlignedClip(g.clip, D2D1_ANTIALIAS_MODE_ALIASED);
        if (g.bm) s_d->DrawBitmap(g.bm, g.r, g.a, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        else if (g.f) text(g.s, g.f, g.r, g.rgb, g.a, g.al);
        else if (g.w > 0.0f) frame(g.r, g.rgb, g.a, g.w);
        else fill(g.r, g.rgb, g.a);
        if (clipped) s_d->PopAxisAlignedClip();
    }
    s_d->EndDraw();
    s_d = keep;
    ID2D1DeviceContext* dc = NULL;
    if (FAILED(s_glow->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&dc))) return;
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawImage(s_blur_near, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
    if (s_blur_far) dc->DrawImage(s_blur_far, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
    for (int i = 0; i < s_nhole; i++) {
        dc->PushAxisAlignedClip(SR(s_holes[i]), D2D1_ANTIALIAS_MODE_ALIASED);
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->PopAxisAlignedClip();
    }
    dc->EndDraw();
    dc->Release();
}

/* --- widgets --------------------------------------------------------------- */
static const float SEG_H = 34.0f, SW_W = 52.0f, SW_H = 26.0f;

/* One of a row of buttons: chosen, gold; else dark, lighter under the mouse; pressed, sunk. */
static void w_choice(D2D1_RECT_F r, int chosen, int id, const wchar_t* label)
{
    const int down = s_press == id && hovered(id);
    if (chosen) bevel(r, C_GOLD, 0xFFFFFF, 0x000000, 3.0f, 0.45f, 0.28f, 0);
    else bevel(r, hovered(id) ? C_CTL_HI : C_CTL, C_HI, C_LO, 3.0f, 1.0f, 1.0f, down);
    const float o = down ? 1.0f : 0.0f;
    text(label, chosen ? s_tf_ui_b : s_tf_ui, D2D1::RectF(r.left + o, r.top + o, r.right + o, r.bottom + o),
         chosen ? C_INK : C_TEXT2, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
}

/* Buttons side by side, one of them chosen: the option's values. */
static void w_segmented(Opt& o, int oi, float x, float y, float w)
{
    const int sel = opt_index(o, o.cur);
    const float gap = 4.0f, sw = (w - gap * (float)(o.n - 1)) / (float)o.n;
    for (int i = 0; i < o.n; i++) {
        const D2D1_RECT_F s = R(x + (sw + gap) * (float)i, y, sw, SEG_H);
        const int id = H_SEG + oi * 16 + i;
        w_choice(s, i == sel, id, W((o.seg ? o.seg : o.names)[i]));
        hit(s, id);
    }
}
/* How wide w_segmented's buttons need to be for the option's names: all as wide as the widest. */
static float seg_w(const Opt& o)
{
    float m = 0.0f;
    for (int i = 0; i < o.n; i++) {
        const float w = text_w(W((o.seg ? o.seg : o.names)[i]), s_tf_ui_b);
        if (w > m) m = w;
    }
    return (m + 26.0f) * (float)o.n + 4.0f * (float)(o.n - 1);
}

/* A switch, on the right of its row: a square that moves over, cyan when on. */
static void w_switch(int on, float x, float y, int id)
{
    const D2D1_RECT_F t = R(x, y, SW_W, SW_H);
    const unsigned c = on ? C_CYAN : hovered(id) ? C_DIM : C_OFF;
    if (clean()) {   /* a pill with a round knob */
        const D2D1_RECT_F r = SR(t);
        const float rad = (r.bottom - r.top) * 0.5f, k = rad - 4.0f;
        s_br->SetColor(col(on ? C_CYAN : C_BG, on ? 0.25f : 1.0f));
        s_d->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), s_br);
        s_br->SetColor(col(c, 1.0f));
        s_d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), rad - 1,
                                                    rad - 1), s_br, 2.0f);
        const float cx = on ? r.right - rad : r.left + rad;
        s_d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, r.top + rad), k, k), s_br);
        hit(R(x - 4, y - 4, SW_W + 8, SW_H + 8), id);
        return;
    }
    fill(t, C_BG);
    frame(t, c, 1.0f, 2.0f);
    fill(R(on ? x + SW_W - 21.0f : x + 5.0f, y + 5.0f, 16.0f, 16.0f), c);
    if (on) glow_rect(t, C_CYAN, 0.6f, 2.0f);
    hit(R(x - 4, y - 4, SW_W + 8, SW_H + 8), id);
}

/* A box showing the value, sunk; it opens the list. */
static void w_dropdown(Opt& o, int oi, float x, float y, float w)
{
    const int id = H_DD + oi, open = s_dd == oi;
    const D2D1_RECT_F box = R(x, y, w, SEG_H);
    bevel(box, hovered(id) || open ? C_CARD_HI : C_BG, C_LO, C_EDGE, 2.0f, 1.0f, 1.0f, 0);
    if (open) frame(box, C_GOLD, 1.0f, 2.0f);
    text(W(opt_name(o)), s_tf_ui, R(x + 12, y, w - 44, SEG_H), C_TEXT);
    icon(k_ic_down, x + w - 28, y + (SEG_H - 16.0f) * 0.5f, open ? C_GOLD : C_MUTED);
    hit(box, id);
}

/* A box typed into (Opt.text): the value, a caret while it is typed into, else an example in grey; a red edge when
 * what it holds will not do. Clicked, it takes the keys (s_edit); Enter, Esc, Tab or a click elsewhere end that. */
static int s_edit = -1;          /* the option typed into, or -1 */
static void online_sync(void);
static void edit_end(void)
{
    if (s_edit < 0) return;
    const int was = s_edit;
    s_edit = -1;
    if (!strcmp(s_opt[was].section, "online")) online_sync();
}
static int text_ok(const Opt& o)
{
    if (!strcmp(o.key, "name")) return gowa_online_name_ok(o.cur);
    if (!strcmp(o.key, "nodes")) return gowa_online_nodes_ok(o.cur);
    return gowa_online_address_ok(o.cur);
}
static void w_textbox(Opt& o, int oi, float x, float y, float w)
{
    const int id = H_TXT + oi, on = s_edit == oi, bad = o.cur[0] && !text_ok(o);
    const D2D1_RECT_F box = R(x, y, w, SEG_H);
    bevel(box, hovered(id) || on ? C_CARD_HI : C_BG, C_LO, C_EDGE, 2.0f, 1.0f, 1.0f, 0);
    if (on || bad) frame(box, bad ? C_BAD : C_GOLD, 1.0f, 2.0f);
    const wchar_t* shown = o.cur[0] ? W(o.cur) : on ? L"" : !strcmp(o.key, "name") ? L"Kratos" :
                           !strcmp(o.key, "nodes") ? L"node.example.org:36102" : L"192.168.1.20";
    text(shown, s_tf_ui, R(x + 12, y, w - 24, SEG_H), o.cur[0] ? C_TEXT : C_DIM);
    if (on) fill(R(x + 13 + text_w(o.cur[0] ? shown : L"", s_tf_ui), y + 8, 2, SEG_H - 16), C_GOLD);
    hit(box, id);
}
/* What a box takes: a name's letters, digits, - and _; an address's letters, digits, - and dots; the community
 * nodes', colons and commas too. */
static int text_char(const Opt& o, int c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') return 1;
    if (!strcmp(o.key, "name")) return c == '_';
    return c == '.' || (!strcmp(o.key, "nodes") && (c == ':' || c == ','));
}
static void text_put(Opt& o, int c)
{
    const size_t n = strlen(o.cur);
    if (text_char(o, c) && n < (size_t)o.text && n + 1 < sizeof o.cur) {
        o.cur[n] = (char)c;
        o.cur[n + 1] = 0;
    }
}
static void text_paste(Opt& o)
{
    if (!OpenClipboard(s_main)) return;
    if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* t = (const wchar_t*)GlobalLock(hd)) {
            for (; *t; t++) if (*t < 128) text_put(o, (int)*t);   /* spaces and the like are not taken */
            GlobalUnlock(hd);
        }
    }
    CloseClipboard();
}

/* A button in a card; Play; Defaults, Save and Quit in the bar; a gold one in the bar's letters (OK). */
enum { B_PLAIN, B_PLAY, B_BAR, B_GOLD };
static void w_button(const wchar_t* label, D2D1_RECT_F r, int id, int kind)
{
    const int hov = hovered(id), down = s_press == id && hov;
    const float o = down ? 1.0f : 0.0f;   /* pressed: what is on it a pixel down and right */
    if (kind == B_PLAY) {
        const D2D1_RECT_F ring = R(r.left - 2, r.top - 2, r.right - r.left + 4, r.bottom - r.top + 4);
        frame(ring, C_NEON, 1.0f, 2.0f);
        bevel(r, hov ? C_GOLD_HI : C_GOLD, 0xFFFFFF, 0x000000, 4.0f, 0.5f, 0.3f, down);
        glow_rect(ring, C_NEON, 1.0f, 2.0f);
        glow_rect(r, C_NEON, 0.35f);
        glow_hole(r);
        const float tw = text_w(label, s_tf_play), x = snap(r.left + (r.right - r.left - 28.0f - tw) * 0.5f) + o;
        icon(k_ic_play, x, (r.top + r.bottom) * 0.5f - 8.0f + o, C_INK);
        text(label, s_tf_play, D2D1::RectF(x + 28.0f, r.top + o, x + 36.0f + tw, r.bottom + o), C_INK);
    } else if (kind == B_GOLD) {
        bevel(r, hov ? C_GOLD_HI : C_GOLD, 0xFFFFFF, 0x000000, 3.0f, 0.45f, 0.28f, down);
        glow_rect(r, C_GOLD, 0.4f);
        glow_hole(r);
        text(label, s_tf_btn, D2D1::RectF(r.left + o, r.top + o, r.right + o, r.bottom + o), C_INK, 1.0f,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        bevel(r, hov ? C_CTL_HI : C_CTL, C_HI, C_LO, kind == B_BAR ? 3.0f : 2.0f, 1.0f, 1.0f, down);
        text(label, kind == B_BAR ? s_tf_btn : s_tf_ui_b, D2D1::RectF(r.left + o, r.top + o, r.right + o, r.bottom + o),
             C_TEXT, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    hit(r, id);
}

static void w_card(D2D1_RECT_F r, const wchar_t* title)
{
    fill(r, C_CARD);
    frame(r, C_EDGE, 1.0f, 2.0f);
    frame(R(r.left - 1, r.top - 1, r.right - r.left + 2, r.bottom - r.top + 2), C_NEON, 0.22f, 1.0f);
    glow_rect(r, C_NEON, 0.12f, 2.0f);
    if (title) {
        const D2D1_RECT_F t = R(r.left + 18, r.top + 14, r.right - r.left - 36, 18);
        text(title, s_tf_head, t, C_GOLD);
        glow_text(title, s_tf_head, t, C_GOLD, 0.5f);
    }
}

/* One setting: its label and the line under it on the left, its control on the right. */
/* The width of a row's control, and the height of its label and the line under it. */
static float ctl_w(const Opt& o)
{
    if (o.check) return SW_W;
    if (o.text) return 300.0f;
    if (!o.seg) return 300.0f;
    const float w = o.n <= 2 ? 220.0f : (float)o.n * (o.n >= 5 ? 66.0f : 82.0f), need = seg_w(o);
    return need > w ? need : w;
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

static void w_rule(float x, float y, float w) { fill(R(x, y - 1.0f, w, 2.0f), C_LINE); }

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
    const float cx = x + w - cw, cy = y + (rh - (o.check ? SW_H : SEG_H)) * 0.5f;
    if (o.check) w_switch(opt_on(o), cx, cy, sw_id);
    else if (o.text) w_textbox(o, oi, cx, cy, cw);
    else if (o.seg) w_segmented(o, oi, cx, cy, cw);
    else w_dropdown(o, oi, cx, cy, cw);
    return rh;
}

/* --- the parts of the window ---------------------------------------------- */
static const wchar_t* const k_tab_name[T_N] = { L"Play", L"Graphics", L"Display", L"Controls", L"Game patches", L"Cheats",
                                                L"Mods", L"Trophies", L"Multiplayer", L"System" };
static const Icon k_tab_icon[T_N] = { k_ic_play, k_ic_pic, k_ic_screen, k_ic_pad, k_ic_wrench, k_ic_bolt, k_ic_mods,
                                      k_ic_cup, k_ic_net, k_ic_chip };
static const wchar_t* const k_tab_desc[T_N] = {
    L"", L"How the picture is scaled, smoothed and filtered.", L"The window, its pace and the frame rate display.",
    L"Controller and keyboard.", L"Switches that change what the game draws, for speed.",
    L"For an easier fight. The game reads these when it starts; multiplayer turns them off until the next start.",
    L"Files that change the game, from the mods folder. The game reads them when it starts.",
    L"What you have won, from the game's own trophy list.",
    L"Play with other players: found by the game itself, or through a host.",
    L"The game's folder, language, saves and this window." };

/* A link down the sidebar: an icon and a name, lit under the mouse. */
static void side_link(Icon ic, const wchar_t* s, float y, int id)
{
    const D2D1_RECT_F r = R(14, y, SIDE_W - 30, 32);
    const int hov = hovered(id);
    if (hov) fill(r, C_CARD);
    icon(ic, 28, y + 8, hov ? C_TEXT : C_MUTED);
    text(s, s_tf_small, R(54, y, SIDE_W - 72, 32), hov ? C_TEXT : C_MUTED);
    hit(r, id);
}

static void draw_progress(float x, float y, float w, float frac, int warn, float h = 10.0f);

/* A newer release (update_check.cpp): in gold under the version in the sidebar, and on the Play tab a notice until it
 * is closed for that release. Either opens its page. Check for updates off: neither. */
static UpdateState s_upd;
enum { WM_UPDATE = WM_APP + 30 };

static int update_shown(void)
{
    const Opt* o = opt_find("launcher", "update_check");
    return s_upd.known && (!o || opt_on(*o));
}

static int update_notice(void) { return update_shown() && strcmp(s_upd.dismissed, s_upd.rel.tag) != 0; }

/* The release's number as the sidebar shows it: its tag without the v. */
static const char* update_ver(void) { return s_upd.rel.tag[0] == 'v' ? s_upd.rel.tag + 1 : s_upd.rel.tag; }

static void update_open(void)
{
    if (!strncmp(s_upd.rel.url, UPDATE_PAGE_PREFIX, sizeof UPDATE_PAGE_PREFIX - 1))
        ShellExecuteW(s_main, L"open", W(s_upd.rel.url), NULL, NULL, SW_SHOWNORMAL);
}

/* Support, beside the version: the website's Support section (the donation addresses are there and in the README,
 * nowhere in the program). A link, nothing more: it asks for nothing and nothing depends on it. */
#define SUPPORT_PAGE L"https://rigaz29.github.io/Ascension-R/#support"

static void draw_sidebar(void)
{
    fill(R(0, 0, SIDE_W, WIN_H), C_SIDE);
    fill(R(SIDE_W - 2, 0, 2, WIN_H), C_LINE);
    /* The program's icon, its name in the precompile screen's pixel font, its version and the day it was built. */
    if (s_bm_icon) {
        const D2D1_SIZE_F z = s_bm_icon->GetSize();
        const float x = snap(16.0f + (32.0f - z.width) * 0.5f), y = snap(18.0f + (32.0f - z.height) * 0.5f);
        s_d->DrawBitmap(s_bm_icon, D2D1::RectF(x, y, x + z.width, y + z.height), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    if (clean()) {
        text(L"ASCENSION-R", s_tf_head, R(56, 14, SIDE_W - 66, 24), C_GOLD, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    } else if (s_bm_word) {
        const D2D1_RECT_F wr = word_rect(56.0f, 18.0f, 2.0f);
        s_d->DrawBitmap(s_bm_word, wr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        glow_bitmap(s_bm_word_glow, wr, 0.55f);
    }
    text(W("v" AR_VERSION), s_tf_small, R(58, 38, SIDE_W - 72, 16), hovered(H_UPDATE_OPEN) ? C_TEXT2 : C_MUTED);
    {   /* Support, small, at the end of the version's line */
        const D2D1_RECT_F sr = R(SIDE_W - 30 - 90, 38, 90, 16);
        text(L"Support", s_tf_small, sr, hovered(H_SUPPORT) ? C_GOLD_HI : C_DIM, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        hit(R(SIDE_W - 30 - 90, 35, 92, 22), H_SUPPORT);
    }
    if (update_shown()) {   /* in place of the day it was built: the newer release, a link to its page */
        wchar_t b[80];
        swprintf(b, 80, TW(L"%ls available \x203A"), W(update_ver()));
        const D2D1_RECT_F t = R(18, 59, SIDE_W - 36, 16);
        const int hov = hovered(H_UPDATE_OPEN);
        text(b, s_tf_small, t, hov ? C_GOLD_HI : C_GOLD);
        glow_text(b, s_tf_small, t, C_GOLD, hov ? 0.7f : 0.4f);
        hit(R(14, 36, SIDE_W - 30 - 94, 20), H_UPDATE_OPEN);   /* the version's line, but for Support */
        hit(R(14, 56, SIDE_W - 30, 22), H_UPDATE_OPEN);
    } else {
        static const char d[] = __DATE__;   /* "Mmm dd yyyy", the day padded with a space */
        static const char k_mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* mp = strstr(k_mon, std::string(d, 3).c_str());
        wchar_t day[64], b[96];
        if (!i18n_date(atoi(d + 7), mp ? (int)(mp - k_mon) / 3 + 1 : 1, atoi(d + 4), day, 64)) day[0] = 0;
        swprintf(b, 96, TW(L"Built %ls"), day);
        text(UP(b), s_tf_cap, R(18, 62, SIDE_W - 36, 12), C_DIM);
    }
    float y = 86.0f;
    for (int t = 0; t < T_N; t++) {
        const D2D1_RECT_F r = R(14, y, SIDE_W - 30, 40);
        const int id = H_NAV + t, on = t == s_tab;
        if (on) {
            fill(r, C_CARD_HI);
            frame(r, C_NEON, 1.0f, 2.0f);
            glow_rect(r, C_NEON, 0.85f, 2.0f);
        } else if (hovered(id)) {
            fill(r, C_CARD);
            frame(r, C_EDGE, 1.0f, 2.0f);
        }
        icon(k_tab_icon[t], 28, y + 12, on ? C_GOLD : C_TEXT2);
        text(k_tab_name[t], on ? s_tf_ui_b : s_tf_ui, R(54, y, SIDE_W - 72, 40), on ? 0xFFFFFF : C_TEXT2);
        hit(r, id);
        y += 46.0f;
    }
    /* Down at the bottom: Report a problem, the compiled shaders, building them now and the save folder. */
    const float by = WIN_H - 160.0f;
    fill(R(18, by - 10, SIDE_W - 38, 2), C_LINE);
    side_link(k_ic_flag, L"Report a problem", by, H_REPORT);
    side_link(k_ic_trash, L"Clear shaders", by + 36, H_SHADERS);
    wchar_t pb[64];
    if (!s_pb.running) swprintf(pb, 64, TW(L"Build all shaders now"));
    else if (s_pb.total > 0) swprintf(pb, 64, TW(L"Building shaders %ld%%"), (long)((s_pb.done * 100LL) / s_pb.total));
    else swprintf(pb, 64, TW(L"Reading the game's files"));
    side_link(k_ic_bolt, pb, by + 72, H_PREBUILD);
    side_link(k_ic_folder, L"Open save folder", by + 108, H_SAVES);
}

static D2D1_RECT_F hero_rect(void) { return R(SIDE_W, 0, WIN_W - SIDE_W, s_tab == T_PLAY ? HERO_PLAY : HERO_TAB); }

/* The header. The scene is under the layer (scene_draw), which is clear here: this only fades it into the page at the
 * bottom and, where text goes over it (shade), from the left; and a neon edge under it. */
static void draw_hero(int shade)
{
    const D2D1_RECT_F r = hero_rect();
    const float h = r.bottom;
    ID2D1GradientStopCollection* gs = NULL;
    ID2D1LinearGradientBrush* g = NULL;
    if (shade) {
        D2D1_GRADIENT_STOP st[3] = { { 0.0f, col(C_BG, 0.9f) }, { 0.55f, col(C_BG, 0.6f) }, { 1.0f, col(C_BG, 0.0f) } };
        if (SUCCEEDED(s_d->CreateGradientStopCollection(st, 3, &gs)) &&
            SUCCEEDED(s_d->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(SIDE_W, 0), D2D1::Point2F(SIDE_W + 680.0f, 0)), gs, &g))) {
            s_d->FillRectangle(SR(r), g);
            g->Release();
            g = NULL;
        }
        if (gs) { gs->Release(); gs = NULL; }
    }
    const float fade = h > 160.0f ? 64.0f : 40.0f;
    D2D1_GRADIENT_STOP sb[2] = { { 0.0f, col(C_BG, 0.0f) }, { 1.0f, col(C_BG, 0.92f) } };
    if (SUCCEEDED(s_d->CreateGradientStopCollection(sb, 2, &gs)) &&
        SUCCEEDED(s_d->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, h - fade), D2D1::Point2F(0, h)), gs, &g))) {
        s_d->FillRectangle(SR(R(SIDE_W, h - fade, WIN_W - SIDE_W, fade)), g);
        g->Release();
    }
    if (gs) gs->Release();
    fill(R(SIDE_W, h - 2, WIN_W - SIDE_W, 2), C_NEON);
    glow_rect(R(SIDE_W, h - 2, WIN_W - SIDE_W, 2), C_NEON, 0.7f);
}

/* A label in a cyan frame, as wide as its text: how wide it came out. */
static float draw_chip(const wchar_t* s, float x, float y)
{
    const float w = snap(text_w(s, s_tf_cap) + 18.0f);
    const D2D1_RECT_F r = R(x, y, w, 20);
    fill(r, C_BG, 0.6f);
    frame(r, C_CYAN, 1.0f, 2.0f);
    glow_rect(r, C_CYAN, 0.45f, 2.0f);
    text(s, s_tf_cap, r, C_CYAN, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    return w;
}

/* A bar of segments, as the precompile screen's. frac: how far, or negative when that is not known. */
static void draw_progress(float x, float y, float w, float frac, int warn, float h)
{
    const D2D1_RECT_F r = R(x, y, w, h);
    fill(r, C_BG);
    frame(r, C_EDGE, 1.0f, 2.0f);
    const float in = 3.0f, seg = 6.0f, gap = 2.0f;
    const int n = (int)((w - 2.0f * in + gap) / (seg + gap));
    if (n <= 0) return;
    int from = 0, to;
    if (frac >= 0.0f) {
        to = (int)((float)n * (frac > 1.0f ? 1.0f : frac) + 0.5f);
    } else {   /* back and forth, a quarter of the bar */
        const float t = (float)(GetTickCount64() % 1600) / 1600.0f, at = t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
        const int span = n / 4 > 1 ? n / 4 : 1;
        from = (int)((float)(n - span) * at + 0.5f);
        to = from + span;
    }
    for (int i = from; i < to; i++) fill(R(x + in + (float)i * (seg + gap), y + in, seg, h - 2.0f * in), warn ? C_WARN : C_GOLD);
}

static void check_line(float x, float y, float w, int state, const wchar_t* s)
{
    /* state: 1 fine, 2 for information, 0 a warning, -1 missing */
    const unsigned c = state == 1 ? C_OK : state == 2 ? C_MUTED : state == 0 ? C_WARN : C_BAD;
    icon(state == 1 ? k_ic_check : state == 2 ? k_ic_info : state == 0 ? k_ic_warn : k_ic_cross, x, y + 5, c);
    text(s, s_tf_ui, R(x + 26, y, w - 26, 26), state < 0 ? C_BAD : state == 0 ? C_WARN_T : C_TEXT);
}

static void draw_play(void)
{
    draw_hero(0);
    /* The title over the scene, in a box of its own: the port's name as the banner draws it, what it is, and chips. */
    {
        const float px = SIDE_W + 28, py = 24, pw = 440;
        const wchar_t* sub = L"PS3 code recompiled to x86-64, drawn with Direct3D 12.";
        const float sh = text_h(sub, s_tf_desc, pw - 36);
        const D2D1_RECT_F panel = R(px, py, pw, 128 + sh);
        fill(panel, C_BG, 0.8f);
        frame(panel, C_NEON, 1.0f, 2.0f);
        glow_rect(panel, C_NEON, 0.75f, 2.0f);
        text(L"NATIVE PC PORT \x00B7 STATIC RECOMPILATION", s_tf_cap, R(px + 18, py + 14, pw - 36, 12), C_CYAN);
        if (clean()) {
            if (!s_tf_name) s_tf_name = tf(system_font(L"Bahnschrift") ? L"Bahnschrift" : L"Segoe UI", 40.0f, DWRITE_FONT_WEIGHT_BOLD);
            if (s_tf_name) text(L"ASCENSION-R", s_tf_name, R(px + 16, py + 30, pw - 32, 46), C_GOLD, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
        } else if (s_bm_word) {
            const D2D1_RECT_F wr = word_rect(px + 14, py + 34, 4.0f);
            s_d->DrawBitmap(s_bm_word, wr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            glow_bitmap(s_bm_word_glow, wr, 0.9f);
        }
        text(sub, s_tf_desc, R(px + 18, py + 82, pw - 36, sh + 2), C_TEXT);
        wchar_t b[80];
        float cx = px + 18;
        const float cy = py + 94 + sh;
        if (s_game.g.title_id[0]) swprintf(b, 80, L"%ls \x00B7 %ls", W(s_game.g.title_id), game_version());
        else swprintf(b, 80, L"%hs \x00B7 1.12", game_title_default()->id);
        cx += draw_chip(UP(b), cx, cy) + 8;
        cx += draw_chip(L"DIRECT3D 12", cx, cy) + 8;
        const Opt* rs = opt_find("graphics", "render_scale");
        if (rs && ps3_settings_ieq(rs->cur, "native")) swprintf(b, 80, TW(L"INTERNAL: WINDOW"));
        else swprintf(b, 80, TW(L"%ls INTERNAL"), UP(W(rs ? opt_short(rs) : "720p")));
        draw_chip(b, cx, cy);
    }
    /* A newer release, over the scene on the right, until its notice is closed (then the sidebar's line stays). */
    if (update_notice()) {
        const float bw = 404, bh = 64, bx = WIN_W - 16 - bw, by = 24;
        const D2D1_RECT_F p = R(bx, by, bw, bh);
        fill(p, C_BG, 0.85f);
        frame(p, C_GOLD, 1.0f, 2.0f);
        glow_rect(p, C_GOLD, 0.6f, 2.0f);
        icon(k_ic_up, bx + 16, by + 24, C_GOLD);
        text(s_upd.rel.prerelease ? L"NEW PRE-RELEASE" : L"NEW RELEASE", s_tf_cap, R(bx + 44, by + 14, 180, 12), C_CYAN);
        wchar_t b[80];
        swprintf(b, 80, TW(L"%ls is out"), W(update_ver()));
        const wchar_t* go = L"Download";   /* its release's page, where the downloads are */
        const float gw = snap(text_w(go, s_tf_btn) + 32.0f), gx = bx + bw - 42 - gw;
        text(b, s_tf_ui_b, R(bx + 44, by + 30, gx - bx - 52, 22), C_TEXT);
        w_button(go, R(gx, by + 17, gw, 30), H_UPDATE_OPEN, B_GOLD);
        const D2D1_RECT_F x = R(bx + bw - 34, by + 6, 26, 26);
        const int hov = hovered(H_UPDATE_CLOSE);
        if (hov) fill(x, C_CARD_HI);
        icon(k_ic_cross, x.left + 5, x.top + 5, hov ? C_TEXT : C_MUTED);
        hit(x, H_UPDATE_CLOSE);
    }

    /* Ready check */
    const float x0 = SIDE_W + 16, cy = HERO_PLAY + 14, chh = WIN_H - FOOT_H - 16 - cy;
    const float cw = (WIN_W - SIDE_W - 48) * 0.5f, lx = x0 + 18, lw = cw - 36;
    const D2D1_RECT_F c1 = R(x0, cy, cw, chh);
    w_card(c1, L"READY CHECK");
    float y = cy + 44;
    if (s_find.thread) {
        check_line(lx, y, lw, 2, L"Looking for the game\x2026");
    } else if (s_prep.thread) {
        wchar_t b[160];
        float f;
        const int level = prep_line(b, 160, &f);
        check_line(lx, y, lw, level, b);
        draw_progress(lx + 26, y + 28, lw - 26, f, level == 0, 8.0f);
        y += 10;
#ifdef PORT_GAME_MODULE
    } else if (s_kit.running || (s_rc.elf_ok && kit_state() != 0)) {
        /* The game's code, built here: what it is at, and under it the bar and Stop, or what Build would do. */
        wchar_t b[480];
        const wchar_t* hint = L"";
        const int level = s_kit.running ? kit_line(b, 480) : kit_need(b, 480, &hint);
        check_line(lx, y, lw, level, b);
        if (s_kit.running) {
            y += 34;
            draw_progress(lx + 26, y + 10, lw - 26 - 106, (float)s_kit.permille / 1000.0f, s_kit.stopped != 0);
            w_button(L"Stop", R(lx + lw - 96, y, 96, 30), H_KIT_STOP, B_PLAIN);
        } else if (kit_state() != 3) {
            y += 34;
            const int log = s_kit.err[0] != 0;
            text(hint, s_tf_small, R(lx + 26, y, lw - 26 - (log ? 212 : 106), 26), C_MUTED);
            if (log) w_button(L"Log", R(lx + lw - 202, y - 2, 96, 30), H_KIT_LOG, B_PLAIN);
            w_button(log ? L"Try again" : L"Build", R(lx + lw - 96, y - 2, 96, 30), H_KIT_BUILD, B_PLAIN);
        }
#endif
    } else if (!s_rc.elf_ok) {
        const GameInfo& g = s_game.g;
        check_line(lx, y, lw - 106, -1, g.state == GAME_NO_ELF ? L"Not decrypted yet (System tab)"
                                        : g.state == GAME_INCOMPLETE ? (g.files_missing ? L"Game files missing (System tab)"
                                                                                        : L"Wrong game files (System tab)")
                                        : g.state == GAME_OTHER_BUILD && g.other_version ? L"Not version 1.12 (System tab)"
                                        : L"Game not found");
        w_button(L"Find game", R(lx + lw - 96, y - 2, 96, 30), H_GAME_AUTO, B_PLAIN);
    } else if (s_game.g.state == GAME_OTHER_BUILD) {
        check_line(lx, y, lw, 0, s_game.g.other_version ? L"Game found, but not version 1.12"
                                                        : L"Game found, but another GOWA.elf");
    } else {
        wchar_t b[100];
        swprintf(b, 100, TW(L"Game found: %ls %ls"), s_game.g.title_id[0] ? W(s_game.g.title_id) : TW(L"GOWA.elf"), game_version());
        check_line(lx, y, lw, 1, b);
    }
    y += 32;
    {
        wchar_t b[200];
        if (s_rc.profiles > 0) swprintf(b, 200, s_rc.profiles == 1 ? TW(L"Saves: %d profile") : TW(L"Saves: %d profiles"), s_rc.profiles);
        else swprintf(b, 200, TW(L"No saves yet: a new game makes one"));
        check_line(lx, y, lw, s_rc.profiles > 0 ? 1 : 0, b);
    }
    y += 32;
    {
        wchar_t b[300];
        swprintf(b, 300, TW(L"Graphics: %ls"), gpu_chosen()[0] ? W(gpu_chosen()) : TW(L"no Direct3D adapter found"));
        check_line(lx, y, lw, gpu_chosen()[0] ? 1 : -1, b);
    }
    y += 32;
    {
        int n = 0;
        for (int i = 0; i < s_npopt; i++) n += opt_on(s_popt[i]);
        wchar_t b[200];
        swprintf(b, 200, TW(L"Game patches: %d of %d on"), n, s_npopt);
        check_line(lx, y, lw, 1, b);
    }
    y += 32;
    {
        const Opt* dev = opt_find("input", "device");
        if (dev && ps3_settings_ieq(dev->cur, "keyboard"))
            check_line(lx, y, lw, 2, L"Keyboard and mouse (Controls tab)");
        else if (s_rc.pad) {
            wchar_t b[220];
            if (s_rc.pad > 1) swprintf(b, 220, TW(L"Controller: %ls, and %d more"), W(s_rc.pad_name), s_rc.pad - 1);
            else swprintf(b, 220, TW(L"Controller: %ls"), W(s_rc.pad_name));
            check_line(lx, y, lw, 1, b);
        }
        else
            check_line(lx, y, lw, dev && ps3_settings_ieq(dev->cur, "pad") ? 0 : 2,
                       dev && ps3_settings_ieq(dev->cur, "pad") ? L"No controller found (Controls: controller only)"
                                                                : L"No controller: keyboard and mouse");
    }
    y += 32;
    {   /* The pixel shaders built ahead: Build (or Stop) as the sidebar's link, until they are built for this version. */
        const Opt* pb = opt_find("graphics", "shader_prebuild");
        if (s_pb.running) {
            wchar_t b[120];
            if (s_pb.total > 0)
                swprintf(b, 120, TW(L"Building pixel shaders: %ld%%"), (long)((s_pb.done * 100LL) / s_pb.total));
            else
                swprintf(b, 120, TW(L"Building pixel shaders: reading the game"));
            check_line(lx, y, lw - 106, 2, b);
            w_button(L"Stop", R(lx + lw - 96, y - 2, 96, 30), H_PREBUILD, B_PLAIN);
        } else if (pb && !opt_on(*pb)) {
            check_line(lx, y, lw, 2, L"Pixel shaders compiled as you play");
        } else if (s_pbs == 2) {
            check_line(lx, y, lw, 1, L"Pixel shaders built ahead");
        } else {
            check_line(lx, y, lw - 106, 2, s_pbs ? L"Pixel shaders partly built" : L"Pixel shaders not built yet");
            w_button(L"Build", R(lx + lw - 96, y - 2, 96, 30), H_PREBUILD, B_PLAIN);
        }
    }
    const float cb = cy + chh;
    y += 32;
    if (y + 26.0f <= cb - 102.0f) {   /* where the lines above left room: not under the Kit's bar */
        /* The scaling; with AMD FSR the version a last session asked as this one asks had, else what it asks for; and
         * frame generation's maker. ANR FG without its files: Get ANR (anr_offer); while it downloads, how far. */
        const Opt* sc = opt_find("graphics", "scaling");
        const Opt* fg = opt_find("graphics", "frame_generation");
        const bool fsr = sc && ps3_settings_ieq(sc->cur, "fsr3");
        const bool dlss = sc && ps3_settings_ieq(sc->cur, "dlss");
        const bool anr = fg && ps3_settings_ieq(fg->cur, "anr");
        const bool dlssg = fg && ps3_settings_ieq(fg->cur, "dlss");
        const bool fgon = anr || (fg && ps3_settings_ieq(fg->cur, "on"));
        /* NVIDIA's DLLs the settings take, and those not here yet. */
        const int dneed = (dlss ? DLSS_SR : 0) | (dlssg ? DLSS_FG : 0);
        const int dmiss = dlss_missing(dneed);
        wchar_t anr_tail[48];
        const std::string label = anr && !s_anr_dl.running ? anr_label() : std::string();
        swprintf(anr_tail, 48, TW(L" + ANR FG%hs%hs"), label.empty() ? "" : " ", label.c_str());
        const Opt* rx = opt_find("graphics", "reflex");
        const bool reflex = rx && rx->tab >= 0 && !ps3_settings_ieq(rx->cur, "off");
        const wchar_t* tail = anr ? anr_tail : fgon ? L" + FSR frames" : dlssg ? L" + DLSS frames" : reflex ? L" + Reflex" : L"";
        wchar_t b[200];
        if (anr && s_anr_dl.running) {
            const long long total = s_anr_dl.p.total, done = s_anr_dl.p.done;
            if (total > 0)
                swprintf(b, 200, TW(L"Downloading ANR FG %hs: %lld%%"), s_anr_dl.tag, done * 100 / total);
            else
                swprintf(b, 200, TW(L"Downloading ANR FG %hs"), s_anr_dl.tag);
            check_line(lx, y, lw - 106, 2, b);
            draw_progress(lx + 26, y + 28, lw - 26 - 106, total > 0 ? (float)done / (float)total : -1.0f, 0, 8.0f);
            w_button(L"Stop", R(lx + lw - 96, y - 2, 96, 30), H_ANR_STOP, B_PLAIN);
        } else if (anr && !anr_present()) {
            check_line(lx, y, lw - 106, 0, L"ANR FG's files are not here yet");
            w_button(L"Get ANR", R(lx + lw - 96, y - 2, 96, 30), H_ANR_ZIP, B_PLAIN);
        } else if (dneed && !gpu_maybe_rtx()) {
            check_line(lx, y, lw, 0, dlss ? L"DLSS needs an RTX card: AMD FSR instead"
                                          : L"DLSS frame generation needs an RTX 40 or newer: off");
        } else if (dneed && s_dlss_dl.running) {
            const long long total = s_dlss_dl.p.total, done = s_dlss_dl.p.done;
            if (total > 0)
                swprintf(b, 200, TW(L"Downloading DLSS %hs: %lld%%"), k_dlss_dll_version, done * 100 / total);
            else
                swprintf(b, 200, TW(L"Downloading DLSS %hs"), k_dlss_dll_version);
            check_line(lx, y, lw - 106, 2, b);
            draw_progress(lx + 26, y + 28, lw - 26 - 106, total > 0 ? (float)done / (float)total : -1.0f, 0, 8.0f);
            w_button(L"Stop", R(lx + lw - 96, y - 2, 96, 30), H_DLSS_STOP, B_PLAIN);
        } else if (dmiss) {
            check_line(lx, y, lw - 106, 0, dmiss == DLSS_FG ? L"DLSS frame generation's DLL is not here yet"
                                                            : L"DLSS's DLL is not here yet");
            w_button(L"Get DLSS", R(lx + lw - 96, y - 2, 96, 30), H_DLSS_GET, B_PLAIN);
        } else if (dlss) {
            swprintf(b, 200, TW(L"Upscaling: NVIDIA DLSS (beta)%ls"), tail);
            check_line(lx, y, lw, 1, b);
        } else if (!fsr) {
            swprintf(b, 200, TW(L"Scaling: %ls%ls"), W(sc ? opt_name(*sc) : T("Bilinear")), tail);
            check_line(lx, y, lw, 2, b);
        } else {
            const Opt* fv = opt_find("graphics", "fsr_version");
            const char* want = fv && !ps3_settings_ieq(fv->cur, "auto") ? fv->cur : "";
            const char* seen = fsr_seen_for(want);
            if (seen) swprintf(b, 200, TW(L"Upscaling: AMD FSR %ls%ls"), W(seen), tail);
            else if (*want) swprintf(b, 200, TW(L"Upscaling: AMD FSR %ls%ls"), W(want), tail);
            else swprintf(b, 200, TW(L"Upscaling: AMD FSR%ls"), *tail ? tail : TW(L", the newest the card has"));
            check_line(lx, y, lw, 1, b);
        }
    }
    w_rule(lx, cb - 98, lw);
    text(L"GAME FOLDER", s_tf_cap, R(lx, cb - 86, lw, 12), C_MUTED);
    text(game_path_shown(), s_tf_path, R(lx, cb - 72, lw, 20), C_TEXT2);
    text(L"SETTINGS FILE", s_tf_cap, R(lx, cb - 46, lw, 12), C_MUTED);
    text(W(ps3_settings_path()), s_tf_path, R(lx, cb - 32, lw, 20), C_TEXT2);

    /* Your setup */
    const float x1 = x0 + cw + 16, rx = x1 + 18, rw = cw - 36;
    const D2D1_RECT_F c2 = R(x1, cy, cw, chh);
    w_card(c2, L"YOUR SETUP");
    const int pn = preset_now();
    const float pgap = 6.0f, pbw = (rw - 2.0f * pgap) / (float)N_PRESET;
    for (int k = 0; k < N_PRESET; k++) {
        const D2D1_RECT_F s = R(rx + (pbw + pgap) * (float)k, cy + 44, pbw, 38);
        w_choice(s, k == pn, H_PRESET + k, W(k_presets[k].name));
        if (k == pn) {
            glow_rect(s, C_GOLD, 0.45f);
            glow_hole(s);
        }
        hit(s, H_PRESET + k);
    }
    text(pn >= 0 ? W(k_presets[pn].desc) : L"Your own mix of game patches and graphics settings.", s_tf_desc,
         R(rx, cy + 92, rw, 36), C_MUTED);
    float ry = cy + 136;
    const char* quick[][2] = { { "display", "mode" }, { "display", "frame_cap" } };
    for (int q = 0; q < 2; q++) {
        Opt* o = opt_find(quick[q][0], quick[q][1]);
        if (!o) continue;
        float sw = seg_w(*o);
        const int fits = sw <= rw - 150.0f;
        if (!fits) sw = rw - 150.0f < 240.0f ? rw - 150.0f : 240.0f;
        text(W(o->label), s_tf_ui, R(rx, ry, rw - sw - 10, SEG_H), C_TEXT);
        if (fits) w_segmented(*o, (int)(o - s_opt), rx + rw - sw, ry, sw);
        else w_dropdown(*o, (int)(o - s_opt), rx + rw - sw, ry, sw);
        ry += SEG_H + 14;
    }
    if (Opt* vs = opt_find("display", "vsync")) {
        text(L"VSync", s_tf_ui, R(rx, ry, rw - SW_W - 10, SW_H), C_TEXT);
        w_switch(opt_on(*vs), rx + rw - SW_W, ry, H_SW + (int)(vs - s_opt));
        ry += SW_H + 16;
    }
    if (Opt* fps = opt_find("display", "show_fps")) {
        text(L"Frame rate on screen", s_tf_ui, R(rx, ry, rw - SW_W - 10, SW_H), C_TEXT);
        w_switch(ps3_settings_ieq(fps->cur, "overlay"), rx + rw - SW_W, ry, H_SW + (int)(fps - s_opt));
        ry += SW_H + 16;
    }
    if (const Opt* m = opt_find("online", "mode")) {   /* Multiplayer: what it is on; on Auto, who is online */
        wchar_t b[120];
        if (ps3_settings_ieq(m->cur, "auto"))
            swprintf(b, 120, s_mp_status == 1 ? TW(L"Auto \x00B7 %d online") : s_mp_status == 2 ? TW(L"Auto \x00B7 starting") :
                     s_mp_status < 0 ? TW(L"Auto \x00B7 did not start") : TW(L"Auto"), s_mp_total);
        else
            swprintf(b, 120, L"%ls", W(opt_short(m)));
        const D2D1_RECT_F r = R(rx, ry, rw, SW_H);
        const int hov = hovered(H_NAV + T_ONLINE);
        text(L"Multiplayer", s_tf_ui, R(rx, ry, 160, SW_H), C_TEXT);
        text(b, s_tf_ui, R(rx + 160, ry, rw - 160, SW_H), hov ? C_GOLD_HI : C_TEXT2, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        hit(r, H_NAV + T_ONLINE);
    }
}

/* The game's folder: the first row of the System tab. */
static const float GAME_ROW_H = 106.0f, GAME_ROW_ASK_H = 46.0f;

/* Whether the row asks for what the decrypting needs. */
static int prep_asks(void) { return s_game.g.state == GAME_NO_ELF && !s_prep.thread && !s_find.thread; }
#ifdef PORT_GAME_MODULE
static float game_row_h(void) { return GAME_ROW_H + (prep_asks() ? GAME_ROW_ASK_H : s_prep.thread || s_kit.running ? 36.0f : 0.0f); }
#else
static float game_row_h(void) { return GAME_ROW_H + (prep_asks() ? GAME_ROW_ASK_H : s_prep.thread ? 36.0f : 0.0f); }
#endif

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
        swprintf(b, sizeof b / sizeof b[0], TW(L"Searching: %ls"), s_find.now);
        ReleaseSRWLockShared(&s_find.lock);
        icon(k_ic_search, x, y + 79, C_GOLD);
        text(b, s_tf_path, R(x + 26, y + 74, w - 26, 26), C_TEXT2);
    } else {
        wchar_t b[GAME_PATH_MAX + 128];
        const int level = game_status(b, sizeof b / sizeof b[0]);
        check_line(x, y + 74, w, level, b);
    }
    if (s_prep.thread) {
        wchar_t b[32];
        float f;
        const int level = prep_line(b, 32, &f);
        draw_progress(x + 26, y + 110, w - 26 - 110, f, level == 0);
        w_button(L"Stop", R(x + w - 96, y + 98, 96, SEG_H), H_PREP_STOP, B_PLAIN);
    }
#ifdef PORT_GAME_MODULE
    else if (s_kit.running) {   /* the game's code, after the decrypting: the status line above says how far */
        draw_progress(x + 26, y + 110, w - 26 - 110, (float)s_kit.permille / 1000.0f, s_kit.stopped != 0);
        w_button(L"Stop", R(x + w - 96, y + 98, 96, SEG_H), H_KIT_STOP, B_PLAIN);
    }
#endif
    if (prep_asks()) {   /* what the decrypting needs, left to right, each button as wide as its name */
        const float by = y + 106;
        float bx = x;
        const wchar_t* name[4] = { L"Choose rpcs3.exe\x2026", L"Download RPCS3", L"Choose .rap\x2026", L"Try again" };
        const int id[4] = { H_PICK_RPCS3, H_GET_RPCS3, H_PICK_RAP, H_PREP_RETRY };
        const int disc = s_game.g.disc;   /* no .rap for a disc */
        const int shown[4] = { 1, !s_prep.rpcs3[0], !disc, s_prep.err[0] && s_prep.rpcs3[0] && (disc || s_prep.rap[0]) };
        for (int i = 0; i < 4; i++) {
            if (!shown[i]) continue;
            const float bw = snap(text_w(name[i], s_tf_ui_b) + 32.0f);
            w_button(name[i], R(bx, by, bw, SEG_H), id[i], B_PLAIN);
            bx += bw + 10;
        }
    }
}

static const float SAVES_ROW_H = 76.0f;
static void draw_saves_row(float x, float y, float w)
{
    const float bw = 108.0f;
    text(L"Save files", s_tf_ui_b, R(x, y + 16, w - 3 * bw - 30, 22), C_TEXT);
    w_button(L"Open folder", R(x + w - 3 * bw - 20, y + 10, bw, SEG_H), H_SAVES, 0);
    w_button(L"Export zip\x2026", R(x + w - 2 * bw - 10, y + 10, bw, SEG_H), H_EXPORT_SAVES, 0);
    w_button(L"Import zip\x2026", R(x + w - bw, y + 10, bw, SEG_H), H_IMPORT_SAVES, 0);
    char path[1100];
    if (saves_dir(path, sizeof path)) text(W(path), s_tf_path, R(x, y + 48, w, 20), C_TEXT2);
}

/* --- the Trophies tab -------------------------------------------------------------------------------------------------
 * The game's own list (its TROPHY.TRP: under its update or the digital game's folder, else on its disc) in the game's
 * language, and what the player has won (the file the game keeps beside the saves): read when the tab is first shown,
 * and again when the game's folder or the language changes. Each trophy's picture as the game has it, grey until it is
 * won; a hidden one shows neither picture nor name until then, as on the PS3. */
static GowaTrophies* s_tro;                       /* NULL until the tab is first shown */
static int s_tro_ok;                              /* the set was read */
static char s_tro_key[GAME_PATH_MAX * 9 + 16];    /* what it was read for */
static ID2D1Bitmap* s_tro_bm[PS3_TROPHY_SET_MAX];
static unsigned char s_tro_tried[PS3_TROPHY_SET_MAX];
static const float TRO_HEAD_H = 112.0f, TRO_ROW_H = 76.0f, TRO_ICON = 56.0f;

static void tro_release(void)
{
    for (int i = 0; i < PS3_TROPHY_SET_MAX; i++) drop(s_tro_bm[i]);
    memset(s_tro_tried, 0, sizeof s_tro_tried);
}

/* A grade's colour, as the game's notice has it: platinum, gold, silver, bronze. */
static unsigned tro_grade_rgb(int g) { return g == 1 ? 0xB3CCF0 : g == 2 ? 0xEBBF3D : g == 3 ? 0xC7CCD4 : 0xD18C54; }

static void tro_refresh(void)
{
    const Opt* lo = opt_find("system", "language");
    const int lang = cellSysutil_language_of(lo ? lo->cur : NULL);
    static char hdd0[GAME_PATH_MAX * 3], app[GAME_PATH_MAX * 3], disc[GAME_PATH_MAX * 3], key[sizeof s_tro_key];
    snprintf(hdd0, sizeof hdd0, "%s", U8(s_game.hdd0));
    snprintf(app, sizeof app, "%s", U8(s_game.link_hdd0));
    snprintf(disc, sizeof disc, "%s", U8(s_game.link_vfs));
    snprintf(key, sizeof key, "%s|%s|%s|%d", hdd0, app, disc, lang);
    if (s_tro && !strcmp(key, s_tro_key)) return;
    if (!s_tro && !(s_tro = (GowaTrophies*)calloc(1, sizeof *s_tro))) return;
    snprintf(s_tro_key, sizeof s_tro_key, "%s", key);
    tro_release();
    s_tro_ok = !gowa_trophies_read(hdd0, app, disc, lang, s_tro);
}

static float tro_h(void)
{
    if (!s_tro_ok) return 72.0f;
    int n = 0;
    for (int i = 0; i < s_tro->set.count; i++) n += ps3_trophy_set_entry(&s_tro->set, i) != NULL;
    return TRO_HEAD_H + (float)n * TRO_ROW_H;
}

/* Trophy id's picture, TRO_ICON DIPs a side and a texel a device pixel: the mean of the set's pixels under each; grey
 * and darker until it is won. Made the first time it is in view. */
static ID2D1Bitmap* tro_bitmap(int id, int won)
{
    if (s_tro_bm[id] || s_tro_tried[id]) return s_tro_bm[id];
    s_tro_tried[id] = 1;
    int sw = 0, sh = 0;
    unsigned char* src = gowa_trophies_icon(s_tro->set.path, id, &sw, &sh);
    if (!src) return NULL;
    const int n = (int)(TRO_ICON * s_px + 0.5f);
    if (uint32_t* px = (uint32_t*)malloc((size_t)n * n * 4)) {
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                int x0 = x * sw / n, x1 = (x + 1) * sw / n, y0 = y * sh / n, y1 = (y + 1) * sh / n;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                int k = 0;
                for (int sy = y0; sy < y1 && sy < sh; sy++)
                    for (int sx = x0; sx < x1 && sx < sw; sx++) {
                        const unsigned char* s = src + ((size_t)sy * sw + sx) * 4;
                        const float al = (float)s[3] / 255.0f;
                        r += (float)s[0] * al;
                        g += (float)s[1] * al;
                        b += (float)s[2] * al;
                        a += al;
                        k++;
                    }
                if (k) { r /= (float)k; g /= (float)k; b /= (float)k; a /= (float)k; }
                if (!won) r = g = b = (0.299f * r + 0.587f * g + 0.114f * b) * 0.5f;
                px[(size_t)y * n + x] = (uint32_t)(a * 255.0f + 0.5f) << 24 | (uint32_t)(r + 0.5f) << 16 |
                                        (uint32_t)(g + 0.5f) << 8 | (uint32_t)(b + 0.5f);
            }
        const float dpi = 96.0f * s_px;
        s_rt->CreateBitmap(D2D1::SizeU(n, n), px, n * 4,
                           D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                  dpi, dpi),
                           &s_tro_bm[id]);
        free(px);
    }
    gowa_trophies_free(src);
    return s_tro_bm[id];
}

/* "Won 18 Nov 2026", from Unix seconds. */
static void tro_date(long long t, wchar_t* out, size_t n)
{
    const time_t tt = (time_t)t;
    struct tm tm;
    wchar_t d[64];
    if (t > 0 && !localtime_s(&tm, &tt) && i18n_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, d, 64))
        swprintf(out, n, TW(L"Won %ls"), d);
    else swprintf(out, n, TW(L"Won"));
}

/* The tab's card from y: what is won of the set and of each grade, then each trophy as the game numbers them. top,
 * bottom: the part in view, the only rows drawn. */
static void tro_draw(float x, float y, float w, float top, float bottom)
{
    if (!s_tro_ok) {
        icon(k_ic_info, x, y + 25, C_MUTED);
        text(L"The trophies come from the game's own files: once the game is found (System tab), they show here.",
             s_tf_desc, R(x + 26, y + 20, w - 26, 40), C_TEXT2);
        return;
    }
    static const wchar_t* const gname[5] = { L"", L"Platinum", L"Gold", L"Silver", L"Bronze" };
    const GowaTrophies& t = *s_tro;
    int won = 0, all = 0, g_won[5] = { 0, 0, 0, 0, 0 }, g_all[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < t.set.count; i++) {
        const PS3TrophyEntry* e = ps3_trophy_set_entry(&t.set, i);
        if (!e || t.unobtainable[i]) continue;
        all++;
        g_all[e->grade]++;
        if (t.unlocked[i]) { won++; g_won[e->grade]++; }
    }
    wchar_t b[96];
    const D2D1_RECT_F tr = R(x, y + 18, w - 250, 20);
    text(UP(W(t.set.title)), s_tf_head, tr, C_GOLD);
    glow_text(UP(W(t.set.title)), s_tf_head, tr, C_GOLD, 0.5f);
    swprintf(b, 96, TW(L"%d of %d won  \x00B7  %d%%"), won, all, all ? won * 100 / all : 0);
    text(b, s_tf_ui_b, R(x + w - 250, y + 16, 250, 22), C_TEXT, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_progress(x, y + 48, w, all ? (float)won / (float)all : 0.0f, 0, 12.0f);
    float gx = x;
    for (int g = 1; g <= 4; g++) {
        fill(R(gx, y + 80, 10, 10), tro_grade_rgb(g));
        swprintf(b, 96, L"%ls  %d / %d", TW(gname[g]), g_won[g], g_all[g]);
        text(b, s_tf_small, R(gx + 18, y + 75, 200, 20), C_TEXT2);
        gx += snap(18.0f + text_w(b, s_tf_small) + 32.0f);
    }
    float ry = y + TRO_HEAD_H;
    for (int i = 0; i < t.set.count; i++) {
        const PS3TrophyEntry* e = ps3_trophy_set_entry(&t.set, i);
        if (!e) continue;
        if (ry + TRO_ROW_H < top || ry > bottom) { ry += TRO_ROW_H; continue; }
        w_rule(x, ry, w);
        const int got = t.unlocked[i], na = t.unobtainable[i] && !got, secret = e->hidden && !got && !na;
        const unsigned gc = tro_grade_rgb(e->grade);
        const float iy = ry + (TRO_ROW_H - TRO_ICON) * 0.5f;
        const D2D1_RECT_F ib = R(x + 2, iy, TRO_ICON, TRO_ICON);
        if (ID2D1Bitmap* bm = secret ? NULL : tro_bitmap(i, got)) {
            s_d->DrawBitmap(bm, SR(ib), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            fill(ib, C_BG);
            icon(k_ic_cup, ib.left + (TRO_ICON - 32.0f) * 0.5f, iy + (TRO_ICON - 32.0f) * 0.5f, C_OFF, 1.0f, 2.0f);
        }
        frame(R(ib.left - 2, iy - 2, TRO_ICON + 4, TRO_ICON + 4), got ? gc : C_EDGE, 1.0f, 2.0f);
        const float tx = x + TRO_ICON + 20.0f, tw = w - TRO_ICON - 20.0f - 176.0f;
        text(secret ? L"Hidden trophy" : W(e->name), s_tf_ui_b, R(tx, ry + 13, tw, 22), got ? C_TEXT : C_TEXT2);
        text(secret ? L"What it is for shows once it is won."
             : na ? L"Won in the multiplayer, which this port does not have. The platinum does without it."
                  : W(e->detail),
             s_tf_desc, R(tx, ry + 37, tw, 36), C_MUTED);
        const float cx = x + w - 168.0f;   /* on the right: its grade, and when it was won */
        text(UP(gname[e->grade]), s_tf_cap, R(cx, ry + 18, 168, 14), gc, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (got) tro_date(t.when[i], b, 96);
        else swprintf(b, 96, L"%ls", na ? TW(L"Not in this port") : TW(L"Not won yet"));
        text(b, s_tf_small, R(cx, ry + 38, 168, 20), got ? C_OK : C_DIM, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        ry += TRO_ROW_H;
    }
}

/* --- the Mods tab -----------------------------------------------------------------------------------------------------
 * The mods folder's mods (mods.h), in their order, the first one winning where two have the same file. Each is
 * switched on or off and moved up or down here; the settings take that at once, and Save or Play writes it. The
 * folder is read again when the tab is shown and when the window comes back to the front: a mod dropped into it then
 * shows, a zip unpacked first. */
static void toast_bad(const char* s);
static ModList s_mods;
static int s_mods_read;        /* read once at least: Save writes the order */
static int s_mods_stale = 1;   /* to be read again */
static const float MOD_FOLDER_H = 64.0f, MOD_EMPTY_H = 72.0f, MOD_NOTE_H = 40.0f, MOD_CTL_W = 134.0f;

static void mods_refresh(void)
{
    if (!s_mods_stale) return;
    s_mods_stale = 0;
    mods_scan(&s_mods, 1);
    s_mods_read = 1;
}

/* The order and the switches as the tab has them, into the settings (save_all writes them). */
static void mods_keep(void)
{
    if (s_mods_read) mods_store(s_mods);
}

/* A mod's switch, up or down: the settings have it at once, and the tab is read again for what each one gives. */
static void mods_act(int k)
{
    const size_t i = (size_t)(k / 3);
    if (i >= s_mods.mods.size()) return;
    std::vector<ModInfo>& v = s_mods.mods;
    if (k % 3 == 0) v[i].on = !v[i].on;
    else if (k % 3 == 1 && i > 0) std::swap(v[i], v[i - 1]);
    else if (k % 3 == 2 && i + 1 < v.size()) std::swap(v[i], v[i + 1]);
    else return;
    mods_store(s_mods);
    s_mods_stale = 1;
}

/* The folder, made if it is not there yet (with a read-me), in Explorer. */
static const char k_mods_readme[] =
    "Mods for Ascension-R: one folder or .zip for each mod, here.\r\n"
    "\r\n"
    "A mod's files are laid out as the game's USRDIR folder has them, in a folder named files\r\n"
    "(or USRDIR, PS3_GAME\\USRDIR or dev_hdd0\\game\\<id>\\USRDIR, as mods for RPCS3 come, or right in\r\n"
    "the mod's folder): files\\TPPS6.PSARC stands for USRDIR\\TPPS6.PSARC. A file there is read in place\r\n"
    "of the game's, or added where the game has none. The game's own folder is never changed.\r\n"
    "\r\n"
    "The launcher's Mods tab lists them: each one on or off, and their order. The first one wins\r\n"
    "where two have the same file. A mod.ini in a mod's folder names it:\r\n"
    "\r\n"
    "  [mod]\r\n"
    "  name = Readable name\r\n"
    "  version = 1.0\r\n"
    "  author = Who made it\r\n"
    "  description = A line about what it changes.\r\n";

static void mods_open(void)
{
    const std::wstring dir = s_mods.dir.empty() ? mods_folder() : s_mods.dir;
    if (CreateDirectoryW(dir.c_str(), NULL)) {
        const std::wstring rm = dir + L"\\README.txt";
        if (FILE* f = _wfopen(rm.c_str(), L"wb")) {
            fwrite(k_mods_readme, 1, sizeof k_mods_readme - 1, f);
            fclose(f);
        }
    } else if (GetLastError() != ERROR_ALREADY_EXISTS) {
        toast_bad("The mods folder could not be made");
        return;
    }
    ShellExecuteW(s_main, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
    s_mods_stale = 1;
}

/* A small square button with an icon; not enabled: greyed, and nothing to click. */
static void w_icon_button(Icon ic, D2D1_RECT_F r, int id, int enabled)
{
    const int hov = enabled && hovered(id), down = enabled && s_press == id && hov;
    bevel(r, hov ? C_CTL_HI : C_CTL, C_HI, C_LO, 2.0f, 1.0f, 1.0f, down);
    const float o = down ? 1.0f : 0.0f;
    icon(ic, snap(r.left + (r.right - r.left - 16.0f) * 0.5f) + o, snap(r.top + (r.bottom - r.top - 16.0f) * 0.5f) + o,
         enabled ? C_TEXT2 : C_OFF);
    if (enabled) hit(r, id);
}

static float mod_desc_h(const ModInfo& m, float tw)
{
    if (m.desc.empty()) return 0.0f;
    const float h = text_h(W(m.desc.c_str()), s_tf_desc, tw);
    return h > 38.0f ? 38.0f : h;   /* two lines at most */
}
static float mod_row_h(const ModInfo& m, float tw)
{
    const float dh = mod_desc_h(m, tw);
    return 14.0f + 22.0f + (dh > 0.0f ? dh + 4.0f : 0.0f) + 24.0f + 12.0f;
}
static float mods_h(float w)
{
    float h = MOD_FOLDER_H + MOD_NOTE_H;
    if (s_mods.mods.empty()) return h + MOD_EMPTY_H;
    for (const ModInfo& m : s_mods.mods) h += mod_row_h(m, w - MOD_CTL_W - 24.0f);
    return h;
}

/* The tab's card from y, under Use mods: the folder, then each mod in its order, then how the order counts. top,
 * bottom: the part in view, the only rows drawn. */
static void mods_draw(float x, float y, float w, float top, float bottom)
{
    const Opt* uo = opt_find("mods", "use");
    const int use = !uo || opt_on(*uo);
    w_rule(x, y, w);
    text(L"Mods folder", s_tf_ui_b, R(x, y + 11, w - 212, 22), C_TEXT);
    text(s_mods.dir.c_str(), s_tf_path, R(x, y + 35, w - 212, 20), C_MUTED);
    w_button(L"Open folder\x2026", R(x + w - 180, y + (MOD_FOLDER_H - SEG_H) * 0.5f, 180, SEG_H), H_MODS_OPEN, B_PLAIN);
    y += MOD_FOLDER_H;
    if (s_mods.mods.empty()) {
        w_rule(x, y, w);
        icon(k_ic_info, x, y + 25, C_MUTED);
        text(L"No mods yet. Put a mod's folder or its .zip into the mods folder: it shows here, switched on.", s_tf_desc,
             R(x + 26, y + 20, w - 26, 40), C_TEXT2);
        y += MOD_EMPTY_H;
    }
    const float tw = w - MOD_CTL_W - 24.0f;
    for (size_t i = 0; i < s_mods.mods.size(); i++) {
        const ModInfo& m = s_mods.mods[i];
        const float rh = mod_row_h(m, tw);
        if (y + rh < top || y > bottom) { y += rh; continue; }
        w_rule(x, y, w);
        const int lit = use && m.on;
        /* Its name, then its version, its author and its zip, smaller. */
        const wchar_t* name = W(m.name.c_str());
        const float nw = text_w(name, s_tf_ui_b), name_w = nw < tw ? nw : tw;
        text(name, s_tf_ui_b, R(x, y + 14, name_w + 2, 22), lit ? C_TEXT : C_TEXT2);
        std::string meta = m.version;
        if (!m.author.empty()) meta += (meta.empty() ? "by " : "  \xC2\xB7  by ") + m.author;
        if (m.zip) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + std::string(U8(m.id.c_str()));
        if (!meta.empty() && name_w + 14.0f < tw)
            text(W(meta.c_str()), s_tf_small, R(x + name_w + 14, y + 16, tw - name_w - 14, 20), C_DIM);
        float ly = y + 14 + 22;
        const float dh = mod_desc_h(m, tw);
        if (dh > 0.0f) {
            text(W(m.desc.c_str()), s_tf_desc, R(x, ly + 4, tw, dh + 2), C_MUTED);
            ly += dh + 4;
        }
        /* What it gives: its files, those a mod above has too, what this build does not read yet, or why it is not
         * read at all. */
        std::wstring st;
        unsigned sc = C_DIM;
        const wchar_t* const sep = L"  \x00B7  ";
        if (!m.error.empty()) {
            st = fmt_w(TW(L"Not read: %ls"), W(m.error.c_str()));
            sc = C_WARN_T;
        } else {
            if (m.files || !m.usrdirs.empty()) st = fmt_w(TW(m.files == 1 ? L"%d file" : L"%d files"), m.files);
            if (m.on && m.shadowed) {
                const ModInfo* by = NULL;
                for (const ModInfo& o : s_mods.mods) if (o.id == m.shadowed_by) by = &o;
                st += sep + fmt_w(TW(L"%d of them from %ls, above"), m.shadowed,
                                  by ? W(by->name.c_str()) : m.shadowed_by.c_str());
                sc = C_WARN_T;
            }
            /* Its archive\ and text\: what the last start packed of them, or that the next one packs them. */
            if (!m.packed.empty()) {
                st += (st.empty() ? L"" : sep) + std::wstring(W(m.packed.c_str()));
                if (m.pack_warn) sc = C_WARN_T;
            } else if (m.archive_files || m.text_files) {
                std::wstring p;
                if (m.archive_files)
                    p += fmt_w(TW(m.archive_files == 1 ? L"archive: %d file" : L"archive: %d files"), m.archive_files);
                if (m.text_files)
                    p += (p.empty() ? L"" : L", ") +
                         fmt_w(TW(m.text_files == 1 ? L"text: %d file" : L"text: %d files"), m.text_files);
                st += (st.empty() ? L"" : sep) + p + (lit ? TW(L", packed when the game starts") : L"");
            }
            if (m.texture_files || m.texture_aliases)
                st += (st.empty() ? L"" : sep) +
                      fmt_w(TW(m.texture_files == 1 ? L"textures: %d picture" : L"textures: %d pictures"), m.texture_files) +
                      (m.texture_aliases ? fmt_w(TW(L", %d more by aliases.txt"), m.texture_aliases) : std::wstring());
            if (!m.later.empty()) {
                st += sep + std::wstring(TW(L"not read by this build yet:"));
                for (size_t j = 0; j < m.later.size(); j++) st += (j ? L", " : L" ") + std::wstring(W(m.later[j].c_str()));
                sc = C_WARN_T;
            }
        }
        if (!lit && sc == C_DIM) sc = C_OFF;
        if (!m.error.empty()) icon(k_ic_warn, x, ly + 5, C_WARN);
        text(st.c_str(), s_tf_small, R(x + (m.error.empty() ? 0.0f : 24.0f), ly + 4, tw - (m.error.empty() ? 0.0f : 24.0f), 20),
             sc);
        /* On the right: up, down, and its switch. */
        const int base = H_MOD + 3 * (int)i;
        const float cy = y + (rh - 32.0f) * 0.5f;
        const float sx = x + w - SW_W;
        w_switch(m.on, sx, y + (rh - SW_H) * 0.5f, base);
        w_icon_button(k_ic_down, R(sx - 12 - 32, cy, 32, 32), base + 2, i + 1 < s_mods.mods.size());
        w_icon_button(k_ic_raise, R(sx - 12 - 32 - 6 - 32, cy, 32, 32), base + 1, i > 0);
        y += rh;
    }
    w_rule(x, y, w);
    if (use && !s_mods.pack_error.empty()) {   /* the last start could not pack them */
        icon(k_ic_warn, x, y + 13, C_WARN);
        text(fmt_w(TW(L"At the last start the mods' archive and text were not packed: %ls"), W(s_mods.pack_error.c_str()))
                 .c_str(),
             s_tf_small, R(x + 26, y + 8, w - 26, 26), C_WARN_T);
        return;
    }
    icon(k_ic_info, x, y + 13, C_MUTED);
    text(use ? L"The first one in the list wins where two mods have the same file. Mods are files alone: the game's "
               L"folder is never changed."
             : L"Use mods is off: the game starts without any of them.",
         s_tf_small, R(x + 26, y + 8, w - 26, 26), C_MUTED);
}


/* The Multiplayer tab's notes, under its rows: why Play could not start multiplayer (until it does), where the players
 * who join find this PC (Host), and what they all see of it. */
static char s_online_err[300];
static int online_notes(const wchar_t* out[5], unsigned rgb[5])
{
    static wchar_t host[1400], err[400], node[400];
    static char lan[400];
    static int lan_read;
    int n = 0;
    if (s_online_err[0]) {
        swprintf(err, 400, L"%ls", W(s_online_err));
        out[n] = err;
        rgb[n++] = C_BAD;
    }
    const Opt* m = opt_find("online", "mode");
    if (m && ps3_settings_ieq(m->cur, "auto")) {
        const wchar_t* reach = TW(!strcmp(s_mp_nat, "open") ? L"other players can reach it" :
                                  !strcmp(s_mp_nat, "easy") ? L"others reach it once introduced" :
                                  !strcmp(s_mp_nat, "hard") ? L"others reach it only through another node" :
                                  L"how others reach it is not known yet");
        if (s_mp_status == 1) swprintf(node, 400, TW(L"Your node is on: %ls."), reach);
        else if (s_mp_status == 2) swprintf(node, 400, TW(L"Your node is starting\x2026"));
        else if (s_mp_status < 0) swprintf(node, 400, TW(L"Your node did not start: %ls"), W(s_mp_why));
        else swprintf(node, 400, TW(L"Your node starts once your online ID is set."));
        out[n] = node;
        rgb[n++] = s_mp_status < 0 ? C_BAD : C_TEXT2;
        out[n] = L"Auto finds the other players through the BitTorrent DHT and on this PC's network: anyone who looks "
                 L"for Ascension-R players there can see this PC's address while this window or the game is open on Auto. "
                 L"Windows may ask to let Python through: allow it.";
        rgb[n++] = C_TEXT2;
    }
    if (m && ps3_settings_ieq(m->cur, "host")) {
        if (!lan_read) {
            gowa_online_lan(lan, sizeof lan);
            lan_read = 1;
        }
        swprintf(host, 1400, TW(L"Players on this PC's network join at %ls. From the internet: forward TCP 10070 and 10012 "
                 L"and TCP and UDP 36101 to this PC on its router and give them its internet address, or have everyone "
                 L"join one virtual network (Tailscale, ZeroTier, Radmin VPN) and give its address. Windows may ask to "
                 L"let Python through: allow it."), lan[0] ? W(lan) : TW(L"this PC's address"));
        out[n] = host;
        rgb[n++] = C_TEXT2;
    }
    out[n] = L"The players you play with see your PC's address. Nothing goes to Sony's servers.";
    rgb[n++] = C_MUTED;
    return n;
}
static float online_h(float w)
{
    const wchar_t* t[5];
    unsigned c[5];
    const int n = online_notes(t, c);
    float h = 10.0f;
    for (int i = 0; i < n; i++) h += text_h(t[i], s_tf_desc, w - 26) + 10.0f;
    const Opt* m = opt_find("online", "mode");
    if (m && ps3_settings_ieq(m->cur, "auto") && s_mp_status == 1) {   /* Online now: its title, a row a player */
        const int rows = s_mp_n > 10 ? 11 : s_mp_n ? s_mp_n : 1;
        h += 34.0f + 24.0f * (float)rows;
    }
    return h;
}
static void online_draw(float x, float y, float w)
{
    const wchar_t* t[5];
    unsigned c[5];
    const int n = online_notes(t, c);
    w_rule(x, y, w);
    y += 12.0f;
    for (int i = 0; i < n; i++) {
        const float h = text_h(t[i], s_tf_desc, w - 26);
        icon(c[i] == C_BAD ? k_ic_cross : k_ic_info, x, y + 1, c[i] == C_BAD ? C_BAD : C_MUTED);
        text(t[i], s_tf_desc, R(x + 26, y, w - 26, h + 2), c[i]);
        y += h + 10.0f;
    }
    const Opt* m = opt_find("online", "mode");
    if (!m || !ps3_settings_ieq(m->cur, "auto") || s_mp_status != 1) return;
    /* Online now: the players the node knows of, what each is doing, and how it reaches them. */
    w_rule(x, y, w);
    wchar_t b[200];
    swprintf(b, 200, TW(L"ONLINE NOW: %d"), s_mp_total);
    text(b, s_tf_head, R(x, y + 10, w, 18), C_GOLD);
    y += 34.0f;
    if (!s_mp_n) {
        text(L"No other player yet. Players show up here while their launcher or game runs on Auto.", s_tf_desc,
             R(x, y, w, 22), C_MUTED);
        return;
    }
    for (int i = 0; i < s_mp_n && i < 10; i++) {
        const MpPlayer& p = s_mp_players[i];
        const unsigned dot = !strcmp(p.state, "searching") ? C_GOLD : !strcmp(p.state, "playing") ? C_CYAN : C_OK;
        fill(R(x + 2, y + 8, 8, 8), dot);
        text(W(p.name), s_tf_ui_b, R(x + 22, y, 200, 22), C_TEXT);
        swprintf(b, 200, L"%ls%ls", mp_state_name(p.state), !strcmp(p.how, "through") ? TW(L" \x00B7 through another node") :
                 !strcmp(p.how, "heard") ? TW(L" \x00B7 heard of from another node") : L"");
        text(b, s_tf_desc, R(x + 230, y + 1, w - 230, 22), C_TEXT2);
        y += 24.0f;
    }
    if (s_mp_n > 10 || s_mp_total > s_mp_n) {
        swprintf(b, 200, TW(L"and %d more"), s_mp_total - (s_mp_n < 10 ? s_mp_n : 10));
        text(b, s_tf_desc, R(x + 22, y, w - 22, 22), C_MUTED);
    }
}

static void draw_settings(int tab)
{
    const float x0 = SIDE_W + 16, hh = HERO_TAB;
    draw_hero(1);
    {   /* the tab's icon twice its size, its name, and what is in it */
        icon(k_tab_icon[tab], SIDE_W + 28, 30, C_GOLD, 1.0f, 2.0f);
        const D2D1_RECT_F t = R(SIDE_W + 72, 30, 600, 32);
        text(UP(k_tab_name[tab]), s_tf_title, t, C_GOLD);
        glow_text(UP(k_tab_name[tab]), s_tf_title, t, C_NEON, 0.7f);
    }
    /* The tab's line: two smaller lines where it is longer than the box (Cheats', and many in other languages). */
    if (text_w(k_tab_desc[tab], s_tf_ui) <= 640.0f) text(k_tab_desc[tab], s_tf_ui, R(SIDE_W + 72, 68, 640, 24), C_TEXT);
    else text(k_tab_desc[tab], s_tf_desc, R(SIDE_W + 72, 62, 640, 42), C_TEXT);
    const float cw = WIN_W - SIDE_W - 48, cy = hh + 14, rx = x0 + 24, rw = cw - 48;
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
    if (tab == T_SYSTEM) ch += game_row_h() + SAVES_ROW_H;
    if (tab == T_TROPHIES) {
        tro_refresh();
        ch += tro_h();
    }
    if (tab == T_MODS) {
        mods_refresh();
        ch += mods_h(rw);
    }
    if (tab == T_ONLINE) ch += online_h(rw);
    ch += 16.0f;
    const float view = WIN_H - FOOT_H - 12.0f - cy;
    s_scroll_max[tab] = ch > view ? ch - view : 0.0f;
    if (s_scroll[tab] > s_scroll_max[tab]) s_scroll[tab] = s_scroll_max[tab];
    if (s_scroll[tab] < 0.0f) s_scroll[tab] = 0.0f;
    const float off = s_scroll[tab];
    const int scrolls = s_scroll_max[tab] > 0.0f;
    if (scrolls) {
        /* Where the card is cut, the rows stop under the edge drawn there; where it is not, its own edge shows, and the
         * neon line a DIP out of it. */
        const float top = off > 0.5f ? cy + 2.0f : cy - 1.0f;
        const float bottom = off < s_scroll_max[tab] - 0.5f ? cy + view - 2.0f : cy + view + 1.0f;
        s_clip = SR(D2D1::RectF(SIDE_W, top, WIN_W, bottom));
        s_d->PushAxisAlignedClip(s_clip, D2D1_ANTIALIAS_MODE_ALIASED);
    }
    w_card(R(x0, cy - off, cw, ch), NULL);
    float y = cy + 16 - off;
    if (tab == T_SYSTEM) {
        draw_game_row(rx, y, rw);
        y += game_row_h();
        w_rule(rx, y, rw);
        draw_saves_row(rx, y, rw);
        y += SAVES_ROW_H;
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
        w_button(L"Edit keys\x2026", R(rx + rw - 180, y + (rh - SEG_H) * 0.5f, 180, SEG_H), H_KEYS, B_PLAIN);
    }
    if (tab == T_TROPHIES) tro_draw(rx, y, rw, cy, cy + view);
    if (tab == T_MODS) mods_draw(rx, y, rw, cy, cy + view);
    if (tab == T_ONLINE) online_draw(rx, y, rw);
    if (tab == T_PATCHES) {
        w_rule(rx, y, rw);
        icon(k_ic_info, rx, y + 13, C_MUTED);
        text(L"Off: drawn as on the PS3. The game reads these when it starts.", s_tf_small,
             R(rx + 26, y + 8, rw - 26, 26), C_MUTED);
    }
    if (scrolls) {
        s_d->PopAxisAlignedClip();
        s_clip = NO_CLIP;
        /* Where the card is cut by the view: an edge, and the rows fading into it. */
        ID2D1GradientStopCollection* gs = NULL;
        ID2D1LinearGradientBrush* g = NULL;
        D2D1_GRADIENT_STOP st[3] = { { 0.0f, col(C_CARD, 1.0f) }, { 0.25f, col(C_CARD, 1.0f) }, { 1.0f, col(C_CARD, 0.0f) } };
        const float fade = 24.0f;
        if (SUCCEEDED(s_d->CreateGradientStopCollection(st, 3, &gs))) {
            for (int end = 0; end < 2; end++) {
                if (end ? off >= s_scroll_max[tab] - 0.5f : off <= 0.5f) continue;   /* nothing cut there */
                const float ey = end ? cy + view - 2.0f : cy, fy = end ? ey - fade : ey + 2.0f;
                if (SUCCEEDED(s_d->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, end ? ey : fy), D2D1::Point2F(0, end ? fy : fy + fade)),
                        gs, &g))) {
                    s_d->FillRectangle(SR(R(x0 + 2, fy, cw - 4, fade)), g);
                    g->Release();
                    g = NULL;
                }
                fill(R(x0, ey, cw, 2), C_EDGE);
            }
            gs->Release();
        }
        /* Where in the card the window is, beside it. */
        const float bx = x0 + cw + 6, thumb = view * view / ch < 40.0f ? 40.0f : view * view / ch;
        const D2D1_RECT_F track = R(bx, cy, 10, view);
        fill(track, C_BG);
        frame(track, C_EDGE, 1.0f, 2.0f);
        fill(R(bx + 3, cy + 3 + (view - 6 - thumb) * off / s_scroll_max[tab], 4, thumb), C_DIM);
    }
}

static void summary(wchar_t* out, size_t n)
{
    const Opt* a = opt_find("graphics", "antialiasing");
    const Opt* c = opt_find("display", "frame_cap");
    const Opt* g = opt_find("graphics", "frame_generation");
    const bool high = c && (ps3_settings_ieq(c->cur, "unlocked") || atof(c->cur) > 60.5);   /* frame generation goes off */
    swprintf(out, n, L"%ls  \x00B7  %ls  \x00B7  %ls  \x00B7  %ls",
             W(opt_short(opt_find("display", "mode"))), W(opt_short(opt_find("graphics", "scaling"))),
             a && ps3_settings_ieq(a->cur, "smaa") ? TW(L"SMAA") : TW(L"no AA"),
             high ? W(opt_short(c)) :
             g && ps3_settings_ieq(g->cur, "on") ? TW(L"frame generation") : g && ps3_settings_ieq(g->cur, "anr") ? TW(L"ANR FG") :
             c && !ps3_settings_ieq(c->cur, "off") ? W(opt_short(c)) : TW(L"no cap"));
}

static void draw_footer(void)
{
    const float y = WIN_H - FOOT_H;
    fill(R(SIDE_W, y, WIN_W - SIDE_W, FOOT_H), C_SIDE);
    fill(R(SIDE_W, y, WIN_W - SIDE_W, 2), C_LINE);
    /* Play on the right, and from it leftward Quit, Save and Defaults, each as wide as its name. */
    const D2D1_RECT_F play = R(WIN_W - 20 - 196, y + 11, 196, 54);
    w_button(L"PLAY", play, H_PLAY, B_PLAY);
    const wchar_t* names[3] = { L"QUIT", L"SAVE", L"DEFAULTS" };
    const int ids[3] = { H_QUIT, H_SAVE, H_DEFAULTS };
    float bx = play.left - 6;
    for (int i = 0; i < 3; i++) {
        const float bw = snap(text_w(names[i], s_tf_btn) + 32.0f);
        bx -= 10 + bw;
        w_button(names[i], R(bx, y + 16, bw, 44), ids[i], B_BAR);
    }
    const float tw = bx - 16 - (SIDE_W + 20);
    wchar_t sum[300];
    summary(sum, 300);
    const int pn = preset_now();
    if (s_toast[0] && GetTickCount64() < s_toast_until) {   /* a badge in its colour: green done, red not */
        const unsigned c = s_toast_bad ? C_BAD : C_OK;
        const wchar_t* msg = W(s_toast);
        float bw = snap(text_w(msg, s_tf_ui_b) + 58.0f);
        if (bw > tw) bw = tw;
        const D2D1_RECT_F box = R(SIDE_W + 20, y + (FOOT_H - 40.0f) * 0.5f, bw, 40);
        fill(box, C_BG);
        frame(box, c, 1.0f, 2.0f);
        glow_rect(box, c, 0.6f, 2.0f);
        icon(s_toast_bad ? k_ic_cross : k_ic_check, box.left + 14, y + (FOOT_H - 16.0f) * 0.5f, c);
        text(msg, s_tf_ui_b, R(box.left + 40, box.top, bw - 50, 40), C_TEXT);
    } else {
        wchar_t p[80];
        if (pn >= 0) swprintf(p, 80, TW(L"%ls preset"), TW(W(k_presets[pn].name)));
        else swprintf(p, 80, TW(L"Custom setup"));
        text(UP(p), s_tf_head, R(SIDE_W + 20, y + 18, tw, 18), C_GOLD);
        text(sum, s_tf_small, R(SIDE_W + 20, y + 40, tw, 20), C_MUTED);
    }
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
    fill(R(box.left + 4, box.top + 4, w, box.bottom - box.top), 0x000000, 0.5f);   /* a hard shadow */
    fill(box, C_CARD_HI);
    frame(box, C_GOLD, 1.0f, 2.0f);
    const int sel = opt_index(o, o.cur);
    for (int k = 0; k < show; k++) {
        const int i = s_dd_top + k;
        const D2D1_RECT_F r = R(box.left + 4, y0 + 4 + ih * (float)k, w - 8, ih);
        if (hovered(H_DDITEM + i)) fill(r, C_CTL_HI);
        if (i == sel) fill(R(r.left + 6, r.top + 12, 8, 8), C_GOLD);
        text(W(o.names[i]), i == sel ? s_tf_ui_b : s_tf_ui, R(r.left + 22, r.top, r.right - r.left - 28, ih),
             i == sel ? C_TEXT : C_TEXT2);
        hit(r, H_DDITEM + i);
    }
    if (o.n > show) {   /* where the shown part is */
        const float th = (show * ih) * (float)show / (float)o.n;
        const float ty = y0 + 4 + (show * ih - th) * (float)s_dd_top / (float)(o.n - show);
        fill(R(box.right - 8, ty, 4, th), C_DIM);
    }
}

/* The keyboard keys, over the window: what each PS3 button is on the keyboard and the mouse, in two columns. A
 * binding clicked waits, gold, for a key or a mouse button. */
static void kb_draw(void)
{
    const float pw = 860.0f, ph = 600.0f, px = snap((WIN_W - pw) * 0.5f), py = snap((WIN_H - ph) * 0.5f);
    const D2D1_RECT_F panel = R(px, py, pw, ph);
    fill(R(px + 6, py + 6, pw, ph), 0x000000, 0.5f);   /* a hard shadow */
    fill(panel, C_CARD);
    frame(panel, C_NEON, 1.0f, 2.0f);
    glow_rect(panel, C_NEON, 0.8f, 2.0f);
    icon(k_ic_keys, px + 24, py + 22, C_GOLD);
    const D2D1_RECT_F t = R(px + 50, py + 21, 400, 18);
    text(L"KEYBOARD KEYS", s_tf_head, t, C_GOLD);
    glow_text(L"KEYBOARD KEYS", s_tf_head, t, C_GOLD, 0.5f);
    text(L"Click an action, then press a key or a mouse button. Backspace clears it, Esc gives up. Edit "
         L"ascension-r.ini to give an action more than one key.", s_tf_desc, R(px + 24, py + 52, pw - 48, 40), C_MUTED);
    const float colw = (pw - 48.0f - 24.0f) * 0.5f;
    float lw = 130.0f;   /* as wide as the longest action's name in the language shown */
    for (int i = 0; i < N_KB; i++) {
        const float nw = snap(text_w(W(k_kb[i][1]), s_tf_ui) + 8.0f);
        if (nw > lw) lw = nw;
    }
    if (lw > colw * 0.45f) lw = snap(colw * 0.45f);
    for (int i = 0; i < N_KB; i++) {
        const float x = px + 24.0f + (float)(i % 2) * (colw + 24.0f), y = py + 100.0f + (float)(i / 2) * 36.0f;
        const int id = H_KBIND + i, wait = s_capture == i;
        text(W(k_kb[i][1]), s_tf_ui, R(x, y, lw, 30), C_TEXT2);
        const D2D1_RECT_F box = R(x + lw + 8.0f, y, colw - lw - 8.0f, 30);
        bevel(box, wait || hovered(id) ? C_CARD_HI : C_BG, C_LO, C_EDGE, 2.0f, 1.0f, 1.0f, 0);
        if (wait) {
            frame(box, C_GOLD, 1.0f, 2.0f);
            glow_rect(box, C_GOLD, 0.5f, 2.0f);
        }
        text(wait ? L"Press a key or button" : s_kb[i][0] ? W(s_kb[i]) : L"(none)", s_tf_ui,
             R(box.left + 10, box.top, box.right - box.left - 20, 30), wait ? C_GOLD : s_kb[i][0] ? C_TEXT : C_DIM);
        hit(box, id);
    }
    /* Defaults on the left; Cancel and OK on the right. */
    const float by = py + ph - 24.0f - 40.0f;
    const float dw = snap(text_w(L"DEFAULTS", s_tf_btn) + 32.0f), cw = snap(text_w(L"CANCEL", s_tf_btn) + 32.0f);
    w_button(L"DEFAULTS", R(px + 24, by, dw, 40), H_KB_DEFAULTS, B_BAR);
    w_button(L"OK", R(px + pw - 24 - 96, by, 96, 40), H_KB_OK, B_GOLD);
    w_button(L"CANCEL", R(px + pw - 24 - 96 - 10 - cw, by, cw, 40), H_KB_CANCEL, B_BAR);
}

/* All but the scene, on s_d. */
static void ui_draw(void)
{
    if (s_font_style != clean()) fonts_make();
    s_nhit = 0;
    s_nglow = 0;
    s_nhole = 0;
    s_d->SetAntialiasMode(clean() ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
    if (s_tab == T_PLAY) draw_play();
    else draw_settings(s_tab);
    draw_sidebar();
    draw_footer();
    if (s_dd >= 0) {
        /* Under an open list only the list counts, and what glows under it glows less. */
        s_nhit = 0;
        for (int i = 0; i < s_nglow; i++) s_glows[i].a *= 0.35f;
        fill(R(0, 0, WIN_W, WIN_H), 0x000000, 0.35f);
        draw_dropdown();
    }
    if (s_kb_open) {   /* the same under the keyboard keys, darker */
        s_nhit = 0;
        for (int i = 0; i < s_nglow; i++) s_glows[i].a *= 0.25f;
        fill(R(0, 0, WIN_W, WIN_H), 0x000000, 0.6f);
        kb_draw();
    }
}

/* The layer, drawn again, and what glows. */
static void layer_draw(void)
{
    s_d = s_layer;
    s_layer->BeginDraw();
    s_layer->SetTransform(D2D1::Matrix3x2F::Identity());
    s_layer->Clear(D2D1::ColorF(0, 0, 0, 0));
    ui_draw();
    s_layer->EndDraw();   /* a lost device shows in the frame's EndDraw too */
    s_d = s_rt;
    s_layer_old = 0;
    if (effects_on()) glow_draw();
}

/* The scene in the header, each of its pixels k x k device pixels: the smallest k at which it fills the header's
 * width, its right edge (the temple) at the header's right, about the temple's middle at the header's. */
static void scene_draw(void)
{
    if (!s_bm_scene) return;
    const D2D1_RECT_F h = SR(hero_rect());
    const float pw = floorf((h.right - h.left) * s_px + 0.5f), ph = floorf((h.bottom - h.top) * s_px + 0.5f);
    if (pw < 1.0f || ph < 1.0f) return;
    int k = (int)ceilf(pw / (float)GOWA_SCENE_W - 0.001f);
    if (k < 1) k = 1;
    while (ph / (float)k > (float)GOWA_SCENE_H) k++;
    const float sw = pw / (float)k, sh = ph / (float)k;
    const float sx = floorf((float)GOWA_SCENE_W - sw);
    float sy = floorf(110.0f - sh * 0.5f);
    if (sy > (float)GOWA_SCENE_H - sh) sy = floorf((float)GOWA_SCENE_H - sh);
    if (sy < 0.0f) sy = 0.0f;
    s_rt->DrawBitmap(s_bm_scene, h, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                     D2D1::RectF(sx, sy, sx + sw, sy + sh));
}

/* A frame: the scene, the layer (drawn again first if something changed), the glow added and the scanlines. */
static void frame_draw(void)
{
    if (s_layer && s_layer_old) layer_draw();
    s_d = s_rt;
    s_rt->BeginDraw();
    s_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    s_rt->Clear(col(C_BG));
    scene_draw();
    ID2D1Bitmap* b = NULL;
    if (s_layer && SUCCEEDED(s_layer->GetBitmap(&b))) {
        s_rt->DrawBitmap(b, NULL, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        b->Release();
    } else {
        ui_draw();   /* no layer: all of it every time, without the glow */
    }
    if (effects_on()) {
        if (s_dc && s_glow && SUCCEEDED(s_glow->GetBitmap(&b))) {
            s_dc->DrawImage(b, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_PLUS);
            b->Release();
        }
        if (s_bm_fx) s_rt->DrawBitmap(s_bm_fx, NULL, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    if (s_rt->EndDraw() == D2DERR_RECREATE_TARGET) {   /* the GPU was reset: a new target, drawn again */
        rt_release();
        if (s_main) InvalidateRect(s_main, NULL, FALSE);
    }
}

/* The scene moves at 30 frames a second while the window is up, not while it is minimised or when Windows'
 * "Show animations in Windows" is off. */
static void anim(int on)
{
    BOOL ok = TRUE;
    if (on && SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &ok, 0) && !ok) on = 0;
    if (on == s_anim || !s_main) return;
    s_anim = on;
    if (on) SetTimer(s_main, 3, 33, NULL);
    else KillTimer(s_main, 3);
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
enum { WM_GAME_FOUND = WM_APP + 1, WM_GAME_PREPARED = WM_APP + 20 };

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
static int game_use(const GameInfo& b)
{
    wchar_t err[GAME_PATH_MAX + 64] = L"";
    const size_t n = sizeof err / sizeof err[0];
    s_game.note[0] = 0;
    /* The second link is named for the title found. A digital release's two links lead to its folder; a disc's lead
     * to the disc's PS3_GAME and to its update, which a disc that lacks it does not get. */
    const GameTitle* t = game_title_find(b.title_id);
    swprintf(s_game.link_hdd0, GAME_PATH_MAX, L"%ls\\game\\%hs", s_game.hdd0, (t ? t : game_title_default())->id);
    const wchar_t* dir = b.dir;
    const wchar_t* second = b.disc ? b.update : b.dir;
    if (!game_link_possible(s_game.link_vfs, dir, err, n) ||
        (second[0] && !game_link_possible(s_game.link_hdd0, second, err, n)) ||
        !game_link(s_game.link_vfs, dir, err, n) || (second[0] && !game_link(s_game.link_hdd0, second, err, n))) {
        swprintf(s_game.note, sizeof s_game.note / sizeof s_game.note[0], L"%ls", err);
        return 0;
    }
    game_paths_fill();
    game_refresh();
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
    GameSearch s = { s_find.under[0] ? s_find.under : NULL, &s_find.stop, find_now, 60, 0, GAME_NO_ELF };
    GameInfo close, best;
    memset(&close, 0, sizeof close);
    if (s.under) game_locate(s.under, &close);   /* the folder itself, or the usual places in it */
    if (close.state == GAME_OK || close.state == GAME_NO_ELF) best = close;
    else {
        game_search(&s, &best);
        if (game_rank(&close) > game_rank(&best)) best = close;
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

static DWORD WINAPI prepare_thread(LPVOID)
{
    wchar_t exe[GAME_PATH_MAX], err[320] = L"";
    int fetched = 0, ok = 1;
    swprintf(exe, GAME_PATH_MAX, L"%ls", s_prep.rpcs3);
    if (!exe[0]) ok = fetched = game_fetch_rpcs3(s_prep.data, exe, GAME_PATH_MAX, err, 320, &s_prep.progress);
    if (ok)
        ok = s_prep.disc ? game_prepare_disc(s_prep.update, exe, s_prep.data, err, 320, &s_prep.progress)
                         : game_prepare(s_prep.game, exe, s_prep.rap, s_prep.data, err, 320, &s_prep.progress);
    if (fetched) game_drop_rpcs3(s_prep.data);
    swprintf(s_prep.err, 320, L"%ls", err);
    s_prep.ok = ok;
    PostMessageA(s_main, WM_GAME_PREPARED, 0, 0);
    return 0;
}

/* Decrypts the game if what that takes is known; otherwise the game's row asks for it. */
static void prepare_begin(void)
{
    if (s_prep.thread || s_game.g.state != GAME_NO_ELF) return;
    /* GOWA.SELF is in the game's folder, or in a disc's update: its RPCS3 is the one that folder is in. */
    const wchar_t* exe_dir = s_game.g.disc ? s_game.g.update : s_game.g.dir;
    if (!s_prep.rpcs3[0]) game_find_rpcs3(exe_dir, s_prep.rpcs3, GAME_PATH_MAX);
    /* A disc needs no .rap: its update's EBOOT.BIN holds the key (game_prepare_disc). */
    if (!s_game.g.disc && !s_prep.rap[0])
        game_find_rap(s_prep.rpcs3[0] ? s_prep.rpcs3 : game_rpcs3_slot(exe_dir), s_game.g.title_id, s_prep.rap,
                      GAME_PATH_MAX);
    s_prep.err[0] = 0;
    if ((!s_game.g.disc && !s_prep.rap[0]) || (!s_prep.rpcs3[0] && !s_prep.download)) return;
    data_file(s_prep.data, GAME_PATH_MAX, L"");
    swprintf(s_prep.game, GAME_PATH_MAX, L"%ls", s_game.g.dir);
    swprintf(s_prep.update, GAME_PATH_MAX, L"%ls", s_game.g.update);
    s_prep.disc = s_game.g.disc;
    memset((void*)&s_prep.progress, 0, sizeof s_prep.progress);
    s_prep.thread = CreateThread(NULL, 0, prepare_thread, NULL, 0, NULL);
    SetTimer(s_main, 2, 100, NULL);   /* the bar */
}

static void prepare_end(void)
{
    if (!s_prep.thread) return;
    WaitForSingleObject(s_prep.thread, INFINITE);
    CloseHandle(s_prep.thread);
    s_prep.thread = NULL;
}

static void prepare_done(void)
{
    prepare_end();
    KillTimer(s_main, 2);
    s_prep.download = 0;
    if (!s_prep.ok) {
        toast_bad(s_prep.progress.stop ? "Stopped" : "The game could not be prepared");
        return;
    }
    game_paths_fill();
    game_refresh();
#ifdef PORT_GAME_MODULE
    if (kit_state() != 0 && kit_state() != 3) {   /* the game's code next, built here once */
        kit_start();
        return;
    }
#endif
    toast("Game ready");
}

/* What the search came to. */
static void find_done(void)
{
    find_end();
    if (s_find.stop) return;
    const GameInfo& b = s_find.best;
    const int picked = s_find.under[0] != 0;
    const size_t n = sizeof s_game.note / sizeof s_game.note[0];
    /* What the search came to, in the session's log: a problem report from a copy the port does not take yet says
     * which release and version it is. */
    static const char* const states[] = { "none", "another release", "incomplete", "not decrypted", "another build", "ok" };
    printf("[find] %s: %s %s, %ls%ls%ls\n", states[b.state], b.title_id[0] ? b.title_id : "-",
           b.app_ver[0] ? b.app_ver : "-", b.dir[0] ? b.dir : L"-", b.update[0] ? L", update " : L"", b.update);
    if (b.state >= GAME_NO_ELF && (picked || game_rank(&b) >= game_rank(&s_game.g))) {
        if (game_use(b)) {
            toast(b.state == GAME_OK ? "Game found" : "Game folder set");
            prepare_begin();
        }
        else toast_bad("The game's folder could not be set");
    } else if (b.state == GAME_OTHER_TITLE) {
        swprintf(s_game.note, n, TW(L"Found %ls in %ls. This port runs %ls 1.12"), W(b.title_id), b.dir, game_titles_line());
        toast_bad("Another release of the game");
    } else if (b.state == GAME_INCOMPLETE && b.disc && (b.disc_lack == DISC_NO_GAME || b.disc_lack == DISC_ISO) &&
               (picked || s_game.g.state < GAME_NO_ELF)) {
        /* A disc's update without the disc: nothing to link the game's folder to; the line says what to do. */
        game_files_text(&b, s_game.note, n);
        toast_bad(b.disc_lack == DISC_ISO ? "The disc is an ISO" : "Found the update, not the disc");
    } else if (b.state == GAME_INCOMPLETE && (picked || s_game.g.state < GAME_NO_ELF)) {
        /* Set, so the System tab says what the folder lacks: with the rest copied in, it is the game. */
        if (game_use(b))
            toast_bad(b.disc && b.disc_lack == DISC_NO_UPDATE ? "Found the disc, not its update"
                      : b.disc && b.disc_lack == DISC_ENCRYPTED ? "The disc's files are encrypted"
                      : b.files_missing ? "Found the game, but not all of it" : "Found the game, but not 1.12's files");
        else toast_bad("The game's folder could not be set");
    } else if (picked) {
        swprintf(s_game.note, n, TW(L"No game in %ls"), s_find.under);
        toast_bad("No game in that folder");
    } else if (s_game.g.state >= GAME_NO_ELF) {
        toast("No better copy found");
    } else {
        swprintf(s_game.note, n, TW(L"Not found on this PC. Browse to its folder (%ls)"), game_titles_line());
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
    /* Each compiler's pack and journal, what a fold cut short leaves, the pipelines from before the store, the
     * mark that the vertex shaders were built (the next start builds them again; the lists of the programs found,
     * .vprogs and .fprogs, stay: they are not shaders), and the pixel shaders built ahead with their mark. */
    static const wchar_t* const ext[] = { L"", L".journal", L".tmp", L".journal.tmp", L".pso",
                                          L".dxil", L".dxil.journal", L".dxil.tmp", L".dxil.journal.tmp", L".dxil.pso",
                                          L".precompiled", L".dxil.prebuilt", L".dxil.prebuilt.done" };
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
    swprintf(m, 200, TW(L"Move %.0f MB of compiled shaders to the Recycle Bin?\n\n"
                     L"The next start builds the vertex shaders first; the rest come as the game reaches them."),
             (double)bytes / 1048576.0);
    if (MessageBoxW(s_main, m, TW(L"Clear shaders"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    SHFILEOPSTRUCTW op = {};
    op.hwnd = s_main;
    op.wFunc = FO_DELETE;
    op.pFrom = list;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) toast_bad("Not cleared: is the game running?");
    else toast("Shaders cleared");
}

/* --- Build all shaders now: the pixel shaders' build, before play ---------------------------------------------
 * The same exe with --prebuild-pixel-shaders (precompile.cpp, gowa_prebuild) and this window's settings, at the
 * normal priority on every thread but one, in a job that ends it with this window (pb_stop: Play or Quit), so it
 * never runs beside the game; its lines read on a thread of their own. The sidebar's link shows how far it is and
 * stops it at a second click; the next click goes on where it stopped. One build a file at a time: another says so. */
static DWORD WINAPI pb_reader(LPVOID)
{
    char buf[4096], line[512];
    size_t n = 0;
    DWORD got = 0;
    while (ReadFile(s_pb.out, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] != '\n' && n + 1 < sizeof line) {
                if (buf[i] != '\r') line[n++] = buf[i];
                continue;
            }
            line[n] = 0;
            n = 0;
            unsigned a = 0, b = 0;
            if (sscanf(line, "prebuild: compile %u %u", &a, &b) == 2) {
                InterlockedExchange(&s_pb.done, (LONG)a);
                InterlockedExchange(&s_pb.total, (LONG)b);
                PostMessageA(s_main, WM_PREBUILD, 0, 0);
            } else if (sscanf(line, "prebuild: found %u", &a) == 1) {
                InterlockedExchange(&s_pb.total, (LONG)a);
                PostMessageA(s_main, WM_PREBUILD, 0, 0);
            } else if (!strncmp(line, "prebuild: done", 14) || !strncmp(line, "prebuild: error", 15)) {
                snprintf(s_pb.result, sizeof s_pb.result, "%s", line);
            }
        }
    }
    WaitForSingleObject(s_pb.proc, INFINITE);
    PostMessageA(s_main, WM_PREBUILD, 1, 0);
    return 0;
}

static void pb_end(void)
{
    if (s_pb.reader) { WaitForSingleObject(s_pb.reader, 2000); CloseHandle(s_pb.reader); s_pb.reader = NULL; }
    pb_state_read();
    CloseHandle(s_pb.proc);
    CloseHandle(s_pb.out);
    if (s_pb.job) CloseHandle(s_pb.job);
    s_pb.proc = s_pb.out = s_pb.job = NULL;
    InterlockedExchange(&s_pb.running, 0);
    unsigned c = 0, had = 0, failed = 0;
    if (sscanf(s_pb.result, "prebuild: done %u %u %u", &c, &had, &failed) == 3) {
        char m[160];
        snprintf(m, sizeof m, T("Pixel shaders built: %u new, %u kept from before%s"), c, had, failed ? T(", some failed") : "");
        toast(m);
    } else if (!strncmp(s_pb.result, "prebuild: error ", 16)) {
        char m[220];
        snprintf(m, sizeof m, T("Shaders not built: %s"), s_pb.result + 16);
        toast_bad(m);
    } else {
        toast("Building shaders stopped");
    }
}

static void pb_start(void)
{
    if (s_pb.running) {   /* a second click: closing the job ends it */
        if (s_pb.job) { CloseHandle(s_pb.job); s_pb.job = NULL; }
        return;
    }
    const char* sc = ps3_settings_str("graphics", "shader_cache", "D3D12_SHADER_CACHE", NULL);
    if (!sc || !*sc || ps3_settings_ieq(sc, "off")) { toast_bad("No shader cache in the settings"); return; }
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    /* The game reads what it builds only with Pixel shaders built ahead on: on with it, and saved at once, so a Quit
     * after the build does not leave it off (the window's other changes wait for Save or Play, as ever). */
    if (Opt* o = opt_find("graphics", "shader_prebuild"))
        if (!opt_on(*o)) {
            opt_set(*o, o->values[1]);
            ps3_settings_set(o->section, o->key, o->cur);
            ps3_settings_save(NULL);
        }
    static wchar_t cmd[8192];
    int at = swprintf(cmd, 8192, L"\"%ls\" --prebuild-pixel-shaders --set \"graphics.shader_prebuild=on\"", exe);
    const char* sp = ps3_settings_path();
    if (sp && *sp && !strchr(sp, '"')) at += swprintf(cmd + at, 8192 - at, L" --settings \"%hs\"", sp);
    static const struct { const char* sec; const char* key; } keep[] = {
        { "graphics", "shader_cache" }, { "paths", "elf" }, { "paths", "hdd0" }, { "paths", "vfs" },
        { "graphics", "gpu" } };
    for (const auto& k : keep) {
        const char* v = ps3_settings_str(k.sec, k.key, NULL, NULL);
        if (v && *v && !strchr(v, '"') && at < 7000) at += swprintf(cmd + at, 8192 - at, L" --set \"%hs.%hs=%hs\"", k.sec, k.key, v);
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) || !CreatePipe(&rd, &wr, &sa, 0)) {
        if (job) CloseHandle(job);
        toast_bad("Could not start building shaders");
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    SetEnvironmentVariableA("GOWA_PRECOMPILE_FOREGROUND", "1");   /* the player asked for it: not below normal */
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("GOWA_PRECOMPILE_FOREGROUND", NULL);
    CloseHandle(wr);
    if (ok && !AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ok = FALSE;
    }
    if (!ok) {
        CloseHandle(rd);
        CloseHandle(job);
        toast_bad("Could not start building shaders");
        return;
    }
    s_pb.proc = pi.hProcess;
    s_pb.out = rd;
    s_pb.job = job;
    s_pb.done = s_pb.total = 0;
    s_pb.result[0] = 0;
    InterlockedExchange(&s_pb.running, 1);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    s_pb.reader = CreateThread(NULL, 0, pb_reader, NULL, 0, NULL);
    redraw();
}

/* The window's end, Play or Quit: a build it started ends with it, so that it takes none of the cores the game wants.
 * The next one goes on where it stopped. */
static void pb_stop(void)
{
    if (!s_pb.running) return;
    if (s_pb.job) { CloseHandle(s_pb.job); s_pb.job = NULL; }   /* the job ends the build */
    if (s_pb.reader) { WaitForSingleObject(s_pb.reader, 5000); CloseHandle(s_pb.reader); s_pb.reader = NULL; }
    CloseHandle(s_pb.proc);
    CloseHandle(s_pb.out);
    s_pb.proc = s_pb.out = NULL;
    InterlockedExchange(&s_pb.running, 0);
}

#ifdef PORT_GAME_MODULE
/* --- the Kit: the game's code built here ----------------------------------------- */
static void kit_parse(const char* l)
{
    if (l[0] != '@') {
        if (l[0]) snprintf(s_kit.last, sizeof s_kit.last, "%s", l);
        return;
    }
    long pm = 0, left = -1;
    int at = 0;
    if (sscanf(l, "@progress %ld %ld %n", &pm, &left, &at) == 2 && at) {
        AcquireSRWLockExclusive(&s_kit.lock);
        kit_copy(s_kit.what, sizeof s_kit.what / sizeof s_kit.what[0], l + at);
        ReleaseSRWLockExclusive(&s_kit.lock);
        InterlockedExchange(&s_kit.permille, pm);
        InterlockedExchange(&s_kit.left, left);
    } else if (!strncmp(l, "@error ", 7)) {
        AcquireSRWLockExclusive(&s_kit.lock);
        kit_copy(s_kit.err, sizeof s_kit.err / sizeof s_kit.err[0], l + 7);
        ReleaseSRWLockExclusive(&s_kit.lock);
    } else if (!strncmp(l, "@done", 5)) {
        InterlockedExchange(&s_kit.done, 1);
    } else {
        return;
    }
    PostMessageA(s_main, WM_KIT_BUILD, 0, 0);
}

static DWORD WINAPI kit_reader(LPVOID)
{
    char buf[4096], line[512];
    size_t n = 0;
    DWORD got = 0;
    while (ReadFile(s_kit.out, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || n + 1 >= sizeof line) {
                line[n] = 0;
                n = 0;
                kit_parse(line);
            } else if (buf[i] != '\r') {
                line[n++] = buf[i];
            }
        }
    }
    WaitForSingleObject(s_kit.proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(s_kit.proc, &code);
    AcquireSRWLockExclusive(&s_kit.lock);
    if (s_kit.stopped) s_kit.err[0] = 0;   /* stopped is not failed */
    else if (!s_kit.done && !s_kit.err[0]) {
        swprintf(s_kit.err, sizeof s_kit.err / sizeof s_kit.err[0], TW(L"it stopped (%lu)%ls"), code, s_kit.last[0] ? L": " : L"");
        const size_t k = wcslen(s_kit.err);
        kit_copy(s_kit.err + k, sizeof s_kit.err / sizeof s_kit.err[0] - k, s_kit.last);
    }
    ReleaseSRWLockExclusive(&s_kit.lock);
    PostMessageA(s_main, WM_KIT_BUILD, 1, 0);
    return 0;
}

/* The Kit's Python, in a job of its own at below normal priority: closing the job ends it and every compiler it
 * started, and so does this window's end, whatever way it ends. */
static void kit_start(void)
{
    if (s_kit.running) return;
    static wchar_t py[GAME_PATH_MAX], script[GAME_PATH_MAX], out[GAME_PATH_MAX], work[GAME_PATH_MAX + 8], dir[GAME_PATH_MAX];
    game_module_kit(py, GAME_PATH_MAX, L"python\\python.exe");
    game_module_kit(script, GAME_PATH_MAX, L"scripts\\gowa.py");
    game_module_kit(dir, GAME_PATH_MAX, L"");
    if (GetFileAttributesW(py) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        toast_bad("No Kit folder beside the program");
        return;
    }
    const char* elf = ps3_settings_str("paths", "elf", NULL, NULL);
    if (!s_rc.elf_ok || !elf) {
        s_tab = T_SYSTEM;
        toast_bad(game_needed_toast());
        return;
    }
    game_module_built(out, GAME_PATH_MAX, L"");
    swprintf(work, GAME_PATH_MAX + 8, L"%ls\\work", out);
    CreateDirectoryW(out, NULL);
    /* This environment, with the Kit's two and nothing of another Python's. */
    wchar_t* cur = GetEnvironmentStringsW();
    size_t len = 0;
    for (const wchar_t* p = cur; *p; p += wcslen(p) + 1) len += wcslen(p) + 1;
    const size_t cap = len + 2 * GAME_PATH_MAX + 64;
    wchar_t* env = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (!env) { FreeEnvironmentStringsW(cur); return; }
    size_t at = 0;
    for (const wchar_t* p = cur; *p; p += wcslen(p) + 1) {
        if (!_wcsnicmp(p, L"GOWA_", 5) || !_wcsnicmp(p, L"PYTHON", 6)) continue;
        wcscpy(env + at, p);
        at += wcslen(p) + 1;
    }
    FreeEnvironmentStringsW(cur);
    at += swprintf(env + at, cap - at, L"GOWA_WORK=%ls", work) + 1;
    at += swprintf(env + at, cap - at, L"GOWA_ELF=%ls", W(elf)) + 1;
    env[at] = 0;

    static wchar_t cmd[4 * GAME_PATH_MAX];
    const int pgo = kit_optimise();
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" -E -s -B -X utf8 \"%ls\" kit-build --out \"%ls\"%ls", py, script, out,
             pgo ? L" --pgo" : L"");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
    li.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) || !CreatePipe(&rd, &wr, &sa, 0)) {
        if (job) CloseHandle(job);
        free(env);
        toast_bad("Could not start the build");
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(py, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                             env, dir, &si, &pi);
    free(env);
    CloseHandle(wr);
    if (ok && !AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ok = FALSE;
    }
    if (!ok) {
        CloseHandle(rd);
        CloseHandle(job);
        toast_bad("Could not start the build");
        return;
    }
    s_kit.proc = pi.hProcess;
    s_kit.job = job;
    s_kit.out = rd;
    s_kit.permille = 0;
    s_kit.left = -1;
    s_kit.what[0] = s_kit.err[0] = 0;
    s_kit.last[0] = 0;
    s_kit.done = s_kit.stopped = 0;
    s_kit.started = GetTickCount64();
    s_kit.running = 1;
    s_kit.reader = CreateThread(NULL, 0, kit_reader, NULL, 0, NULL);
    if (!s_kit.reader) {   /* nothing would read it: stop it */
        CloseHandle(job);   /* ends it */
        CloseHandle(pi.hThread);
        CloseHandle(s_kit.proc);
        CloseHandle(s_kit.out);
        s_kit.running = 0;
        toast_bad("Could not start the build");
        return;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    toast(pgo ? "Building the game's code: about 25 minutes" : "Building the game's code: about 10 minutes");
}

static void kit_stop(void)
{
    if (!s_kit.running || s_kit.stopped) return;
    InterlockedExchange(&s_kit.stopped, 1);
    TerminateJobObject(s_kit.job, 1);
}

/* Its end: the handles closed, what is there now looked at again, and the toast. */
static void kit_end(void)
{
    if (!s_kit.running) return;
    WaitForSingleObject(s_kit.reader, INFINITE);
    CloseHandle(s_kit.reader);
    CloseHandle(s_kit.proc);
    CloseHandle(s_kit.out);
    CloseHandle(s_kit.job);
    s_kit.reader = s_kit.proc = s_kit.out = s_kit.job = NULL;
    s_kit.running = 0;
    kit_refresh();
    if (s_kit.done && s_kit.state == 0) toast("The game's code is built");
    else if (s_kit.stopped) toast("Stopped: Build goes on from there");
    else toast_bad("The game's code could not be built");
}

/* The window closed under it: it stops, and Build goes on from there next time. */
static void kit_kill(void)
{
    if (!s_kit.running) return;
    InterlockedExchange(&s_kit.stopped, 1);
    TerminateJobObject(s_kit.job, 1);
    WaitForSingleObject(s_kit.reader, 5000);
    CloseHandle(s_kit.reader);
    CloseHandle(s_kit.proc);
    CloseHandle(s_kit.out);
    CloseHandle(s_kit.job);
    s_kit.running = 0;
}

static void kit_log(void)
{
    wchar_t p[GAME_PATH_MAX];
    game_module_built(p, GAME_PATH_MAX, L"build.log");
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) toast_bad("No build log yet");
    else ShellExecuteW(s_main, L"open", p, NULL, NULL, SW_SHOWNORMAL);
}
#endif


/* --- reporting a problem ------------------------------------------------------- */
static const wchar_t k_issue_url[] = L"https://github.com/rigaz29/Ascension-R/issues/new?template=problem.yml";

/* Not imported: only version 6 of comctl32 has it, and the manifest (gowa.manifest) makes LoadLibrary give that one.
 * GetModuleHandle found it only once something else had loaded it, Browse's file dialog: otherwise the dialogs fell back
 * to a message box, and the report's "Include my saves" box was never seen (issue #27). */
typedef HRESULT (WINAPI *task_dialog_fn)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
static task_dialog_fn task_dialog(void)
{
    static HMODULE m;
    if (!m) m = LoadLibraryW(L"comctl32.dll");
    return m ? (task_dialog_fn)(void*)GetProcAddress(m, "TaskDialogIndirect") : NULL;
}

/* heading: why it is offered, or NULL when the player asked. */
static void report_make(const wchar_t* heading)
{
    /* Two whole texts, not one with a part between #ifdefs: each is translated as it is shown. */
#ifdef PORT_GAME_MODULE
    static const wchar_t what[] =
        L"The report is a zip file for you to attach to an issue on GitHub. The game sends nothing by itself.\n\n"
        L"It holds the logs of your last sessions, your button presses and the files the game opened in them, your "
        L"settings, the log of building the game's code, this PC's hardware and driver versions, and Windows' records of "
        L"crashes of this game.\n\n"
        L"Folder paths and your Windows name are hidden. No game files are in it.";
#else
    static const wchar_t what[] =
        L"The report is a zip file for you to attach to an issue on GitHub. The game sends nothing by itself.\n\n"
        L"It holds the logs of your last sessions, your button presses and the files the game opened in them, your "
        L"settings, this PC's hardware and driver versions, and Windows' records of crashes of this game.\n\n"
        L"Folder paths and your Windows name are hidden. No game files are in it.";
#endif
    const task_dialog_fn td = task_dialog();
    BOOL saves = FALSE;
    if (td) {
        TASKDIALOG_BUTTON make = { 100, TW(L"Make the report") };
        TASKDIALOGCONFIG c = {};
        c.cbSize = sizeof c;
        c.hwndParent = s_main;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle = TW(L"Report a problem");
        c.pszMainIcon = heading ? TD_WARNING_ICON : TD_INFORMATION_ICON;
        c.pszMainInstruction = TW(heading ? heading : L"Make a problem report?");
        c.pszContent = TW(what);
        c.pszVerificationText = TW(L"Include my saves, so the problem can be loaded where it happened");
        c.cButtons = 1;
        c.pButtons = &make;
        c.nDefaultButton = 100;
        int pressed = 0;
        if (FAILED(td(&c, &pressed, NULL, &saves)) || pressed != 100) return;
    } else if (MessageBoxW(s_main, TW(what), TW(L"Report a problem"), MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
        return;
    }
    HCURSOR was = SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_WAIT));
    static char made[1200];
    const int failed = gowa_report_create(saves, made, sizeof made);
    SetCursor(was);
    if (failed) {
        toast_bad("Could not make the report");
        return;
    }
    wchar_t path[1300];
    MultiByteToWideChar(CP_ACP, 0, made, -1, path, 1300);
    wchar_t done[1500];
    swprintf(done, 1500, TW(L"%ls\n\nAttach it to a new issue and say what happened and where you were in the game."), path);
    int pressed = 0;
    if (td) {
        TASKDIALOG_BUTTON b[2] = { { 101, TW(L"Open GitHub") }, { 102, TW(L"Show the file") } };
        TASKDIALOGCONFIG c = {};
        c.cbSize = sizeof c;
        c.hwndParent = s_main;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        c.pszWindowTitle = TW(L"Report a problem");
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.pszMainInstruction = TW(L"The report is ready");
        c.pszContent = done;
        c.cButtons = 2;
        c.pButtons = b;
        c.nDefaultButton = 102;
        td(&c, &pressed, NULL, NULL);
    } else {
        MessageBoxW(s_main, done, TW(L"Report a problem"), MB_OK | MB_ICONINFORMATION);
        pressed = 102;
    }
    if (pressed == 101) ShellExecuteW(s_main, L"open", k_issue_url, NULL, NULL, SW_SHOWNORMAL);
    if (pressed == 102) {
        wchar_t args[1400];
        swprintf(args, 1400, L"/select,\"%ls\"", path);
        ShellExecuteW(s_main, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    }
}

/* The last session froze with the GPU stopped behind frame generation's swap chain, and it is still on: AMD FSR's
 * frame generation swap chain has stopped the GPU at the start of a session on this kind of PC (a laptop whose display
 * hangs off the other GPU, 2026-11-10; what set it off: docs/notes/2026-11-11-fg-freeze-exit.md). One click turns it
 * off; FSR's scaling stays. 1 = asked. */
static int fg_offer(void)
{
    Opt* fg = opt_find("graphics", "frame_generation");
    if (!fg || !(ps3_settings_ieq(fg->cur, "on") || ps3_settings_ieq(fg->cur, "anr"))) return 0;
    static const wchar_t what[] =
        L"The GPU stopped behind frame generation's swap chain (AMD FSR), which has happened at the start of the "
        L"game on some PCs. Turning it off keeps everything else, FSR's scaling too. You can turn it on again in "
        L"the Graphics tab.";
    int pressed = 0;
    const task_dialog_fn td = task_dialog();
    if (td) {
        TASKDIALOG_BUTTON b[2] = { { 100, TW(L"Turn frame generation off") }, { 101, TW(L"Keep it on") } };
        TASKDIALOGCONFIG c = {};
        c.cbSize = sizeof c;
        c.hwndParent = s_main;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.pszWindowTitle = TW(L"Frame generation");
        c.pszMainIcon = TD_WARNING_ICON;
        c.pszMainInstruction = TW(L"The game froze in the last session, with frame generation on");
        c.pszContent = TW(what);
        c.cButtons = 2;
        c.pButtons = b;
        c.nDefaultButton = 100;
        if (FAILED(td(&c, &pressed, NULL, NULL))) return 1;
    } else {
        pressed = MessageBoxW(s_main, TW(what), TW(L"The game froze with frame generation on"), MB_YESNO | MB_ICONWARNING) == IDYES
                      ? 100 : 101;
    }
    if (pressed != 100) return 1;
    opt_set(*fg, "off");
    ps3_settings_set("graphics", "frame_generation", "off");
    if (ps3_settings_save(NULL) == 0) toast("Frame generation is off");
    else toast_bad("Could not save the settings");
    return 1;
}

/* The last session lost the graphics device (the screen froze while the game and sound went on): on some GPUs and
 * drivers (such as NVIDIA Pascal / GTX 10-series), the asynchronous Render thread can hang the device during heavy
 * scenes. Turning it off keeps the game in synchronous rendering, preventing the GPU timeout. */
static int gpu_lost_offer(void)
{
    Opt* rt = opt_find("graphics", "render_thread");
    if (!rt || !ps3_settings_ieq(rt->cur, "on")) return 0;
    static const wchar_t what[] =
        L"The graphics device was lost during the last session (the screen froze while the game and sound went on). "
        L"On some GPUs, running the Render thread asynchronously can hang the graphics device. Turning it off keeps "
        L"the rendering synchronous and prevents the hang. You can turn it on again in the Graphics tab.";
    int pressed = 0;
    const task_dialog_fn td = task_dialog();
    if (td) {
        TASKDIALOG_BUTTON b[2] = { { 100, L"Turn Render thread off" }, { 101, L"Keep it on" } };
        TASKDIALOGCONFIG c = {};
        c.cbSize = sizeof c;
        c.hwndParent = s_main;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.pszWindowTitle = L"Graphics device recovery";
        c.pszMainIcon = TD_WARNING_ICON;
        c.pszMainInstruction = L"The screen froze in the last session (device lost)";
        c.pszContent = what;
        c.cButtons = 2;
        c.pButtons = b;
        c.nDefaultButton = 100;
        if (FAILED(td(&c, &pressed, NULL, NULL))) return 0;
    } else {
        pressed = MessageBoxW(s_main, what, L"The screen froze in the last session", MB_YESNO | MB_ICONWARNING) == IDYES
                      ? 100 : 101;
    }
    if (pressed != 100) return 0;
    opt_set(*rt, "off");
    ps3_settings_set("graphics", "render_thread", "off");
    if (ps3_settings_save(NULL) == 0) toast("Render thread is off");
    else toast_bad("Could not save the settings");
    return 1;
}

/* Once per session: the next session's own log takes its place as the last one. */
static void report_offer(void)
{
    const wchar_t* h = NULL;
    int fg_stuck = 0;
    const SessionEnd end = gowa_last_session(&fg_stuck);
    if (end == SESSION_FROZE && fg_stuck && fg_offer()) return;
    if (end == SESSION_GPU_LOST) gpu_lost_offer();
    switch (end) {
    case SESSION_CRASH:    h = L"The last session crashed. Make a report of it?"; break;
    case SESSION_GPU_LOST: h = L"The last session lost the graphics device. Make a report of it?"; break;
    case SESSION_FROZE:    h = L"The game froze in the last session. Make a report of it?"; break;
    case SESSION_SUDDEN:   h = L"The last session stopped without saying why. Make a report of it?"; break;
    default:               return;
    }
    report_make(h);
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
    {
        wchar_t t[160];
        swprintf(t, sizeof t / sizeof t[0], TW(L"The game's folder: %ls, or a folder it is under"), game_titles_line());
        d->SetTitle(t);
    }
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

/* A file the player points to. 1 = picked. */
static int pick_file(const wchar_t* title, const wchar_t* kind, const wchar_t* spec, wchar_t* out, size_t n)
{
    IFileOpenDialog* d = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return 0;
    COMDLG_FILTERSPEC f = { TW(kind), spec };
    d->SetFileTypes(1, &f);
    d->SetTitle(TW(title));
    int ok = 0;
    if (SUCCEEDED(d->Show(s_main))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                swprintf(out, n, L"%ls", p);
                ok = 1;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
    return ok;
}

/* A destination file to save to. 1 = picked. */
static int pick_save_file(const wchar_t* title, const wchar_t* kind, const wchar_t* spec, const wchar_t* def_name, wchar_t* out, size_t n)
{
    IFileSaveDialog* d = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return 0;
    COMDLG_FILTERSPEC f = { kind, spec };
    d->SetFileTypes(1, &f);
    d->SetTitle(title);
    if (def_name && *def_name) d->SetFileName(def_name);
    d->SetDefaultExtension(L"zip");
    int ok = 0;
    if (SUCCEEDED(d->Show(s_main))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                swprintf(out, n, L"%ls", p);
                ok = 1;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
    return ok;
}

struct SaveExportTask {
    std::wstring sdir;
    std::wstring out_path;
};

static DWORD WINAPI export_saves_worker(LPVOID param)
{
    SaveExportTask* task = (SaveExportTask*)param;
    char why[256] = "";
    int rc = gowa_zip_folder(task->sdir.c_str(), task->out_path.c_str(), why, sizeof why);
    delete task;
    if (rc == 0) toast("Saves exported to zip");
    else toast_bad(why[0] ? why : "Export failed");
    InvalidateRect(s_main, NULL, FALSE);
    return 0;
}

static void export_saves(void)
{
    char sdir[1100];
    if (!saves_dir(sdir, sizeof sdir)) {
        toast_bad("No saves folder configured");
        return;
    }
    wchar_t out_path[MAX_PATH] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t def_name[64];
    swprintf(def_name, sizeof def_name / sizeof def_name[0],
             L"Ascension-Saves-%04d%02d%02d-%02d%02d.zip",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    if (!pick_save_file(L"Export save backup", L"Zip archive (*.zip)", L"*.zip", def_name, out_path, MAX_PATH))
        return;

    SaveExportTask* task = new SaveExportTask{ W(sdir), out_path };
    HANDLE th = CreateThread(NULL, 0, export_saves_worker, task, 0, NULL);
    if (th) CloseHandle(th);
    else { delete task; toast_bad("Could not start export thread"); }
}

struct SaveImportTask {
    std::wstring sdir;
    std::wstring in_path;
    std::wstring backup_dir;
};

static DWORD WINAPI import_saves_worker(LPVOID param)
{
    SaveImportTask* task = (SaveImportTask*)param;
    CreateDirectoryW(task->backup_dir.c_str(), NULL);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t bzip[MAX_PATH];
    swprintf(bzip, MAX_PATH, L"%ls\\pre-import-%04d%02d%02d-%02d%02d%02d.zip",
             task->backup_dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    gowa_zip_folder(task->sdir.c_str(), bzip, NULL, 0);

    std::string err = mods_unzip(task->in_path, task->sdir);
    delete task;

    if (err.empty()) toast("Saves imported successfully");
    else toast_bad(err.c_str());
    InvalidateRect(s_main, NULL, FALSE);
    return 0;
}

static void import_saves(void)
{
    char sdir[1100];
    if (!saves_dir(sdir, sizeof sdir)) {
        toast_bad("No saves folder configured");
        return;
    }
    wchar_t in_path[MAX_PATH] = {};
    if (!pick_file(L"Import save backup", L"Zip archive (*.zip)", L"*.zip", in_path, MAX_PATH))
        return;

    static const wchar_t warn[] =
        L"Importing saves will replace the files in your save folder with the contents of this backup. "
        L"A safety copy of your current saves will be archived first in save-backups\\.\n\n"
        L"Do you want to continue?";
    if (MessageBoxW(s_main, warn, L"Import save backup", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    const char* hdd0 = ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL);
    std::wstring bdir = (hdd0 && *hdd0 ? W(hdd0) : settings_dir()) + L"\\home\\00000001\\save-backups";

    SaveImportTask* task = new SaveImportTask{ W(sdir), in_path, bdir };
    HANDLE th = CreateThread(NULL, 0, import_saves_worker, task, 0, NULL);
    if (th) CloseHandle(th);
    else { delete task; toast_bad("Could not start import thread"); }
}

/* --anr-get (main.cpp): anr_dl_run without the window, the player having asked on the command line. 0 = set up,
 * with the release in why; else why not. */
extern "C" int gowa_anr_get(char* why, size_t n)
{
    std::wstring dll;
    if (!anr_dll_path(&dll)) { snprintf(why, n, "graphics.anr_dll is not a path"); return 1; }
    GameProgress p = {};
    char tag[32] = "";
    const std::string r = anr_dl_run(dll, &p, tag, sizeof tag);
    snprintf(why, n, "%s", r.empty() ? tag : r.c_str());
    return r.empty() ? 0 : 1;
}

/* --dlss-get (main.cpp): dlss_fetch without the window, the player having asked on the command line. 0 = set up;
 * else why not in why. */
extern "C" int gowa_dlss_get(char* why, size_t n)
{
    GameProgress p = {};
    const std::string r = dlss_fetch(&p, DLSS_SR | DLSS_FG);
    snprintf(why, n, "%s", r.empty() ? k_dlss_dll_version : r.c_str());
    return r.empty() ? 0 : 1;
}

/* --anr-zip <zip> (main.cpp): anr_install without the window, for a script or a player who would rather. zip in UTF-8
 * (the exe's code page). 0 = done, else why not in why. */
extern "C" int gowa_anr_zip(const char* zip, char* why, size_t n)
{
    wchar_t w[MAX_PATH];
    if (!zip || !MultiByteToWideChar(CP_UTF8, 0, zip, -1, w, MAX_PATH)) {
        snprintf(why, n, "no zip named");
        return 1;
    }
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(w, MAX_PATH, full, NULL)) wcscpy(full, w);
    const std::string r = anr_install(full);
    snprintf(why, n, "%s", r.c_str());
    return r.empty() ? 0 : 1;
}

/* ANR FG's files from the zip the player picks (anr_install). */
static void anr_zip_pick(void)
{
    wchar_t zip[MAX_PATH];
    if (!pick_file(L"ANR FG's zip (anr-fg-...-addon.zip, or -full.zip)", L"Zip files", L"*.zip", zip, MAX_PATH)) return;
    const std::string why = anr_install(zip);
    if (why.empty()) toast("ANR FG is in place: the game uses it from the next start");
    else toast_bad(("ANR FG: " + why).c_str());
}

/* ANR FG chosen without its files, or Get ANR on the ready check: asks before anything is fetched. Download and set
 * up (anr_dl_start), Choose a zip (anr_zip_pick) or Not now; Frame generation stays ANR's either way (until its files
 * are there the game uses FSR's own). */
static void anr_offer(void)
{
    if (s_anr_dl.running) return;
    static const wchar_t what[] =
        L"ANR FG is Apex Neural Rendering's frame generation, by Apex Compute, in beta. It is not part of Ascension-R and "
        L"has no licence of its own, so it is not in the download.\n\n"
        L"Ascension-R can download the newest release's addon zip (about 4 MB) from Apex Compute's own releases on "
        L"GitHub (github.com/apex-compute/ANR), check it against the SHA-256 GitHub lists and Apex Compute's signature, "
        L"and set it up in the anr folder beside the program. Nothing else is sent or installed.";
    int pressed = 0;
    const task_dialog_fn td = task_dialog();
    if (td) {
        TASKDIALOG_BUTTON b[3] = { { 100, TW(L"Download and set up") }, { 101, TW(L"Choose a zip I have") }, { 102, TW(L"Not now") } };
        TASKDIALOGCONFIG c = {};
        c.cbSize = sizeof c;
        c.hwndParent = s_main;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.pszWindowTitle = TW(L"ANR FG (beta)");
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.pszMainInstruction = TW(L"Download ANR FG?");
        c.pszContent = TW(what);
        c.cButtons = 3;
        c.pButtons = b;
        c.nDefaultButton = 100;
        if (FAILED(td(&c, &pressed, NULL, NULL))) return;
    } else {
        pressed = MessageBoxW(s_main, TW(what), TW(L"Download ANR FG?"), MB_YESNO | MB_ICONQUESTION) == IDYES ? 100 : 102;
    }
    if (pressed == 100) anr_dl_start();
    else if (pressed == 101) anr_zip_pick();
}

/* NVIDIA's DLLs the settings take (DLSS_*), those not here yet: DLSS scaling, DLSS frame generation. */
static int dlss_wanted(void)
{
    const Opt* sc = opt_find("graphics", "scaling");
    const Opt* fg = opt_find("graphics", "frame_generation");
    return dlss_missing((sc && ps3_settings_ieq(sc->cur, "dlss") ? DLSS_SR : 0) |
                        (fg && ps3_settings_ieq(fg->cur, "dlss") ? DLSS_FG : 0));
}

/* NVIDIA DLSS (scaling or frame generation) chosen without its DLL, or Get DLSS on the ready check: asks before
 * anything is fetched. The setting stays either way (until the DLL is there the game uses AMD FSR for the scaling, and
 * no frame generation). */
static void dlss_offer(int which)
{
    if (s_dlss_dl.running || !which) return;
    wchar_t what[1200], files[160];
    swprintf(files, 160, TW(L"%ls%ls%ls %hs (%d MB)"), which & DLSS_SR ? L"nvngx_dlss.dll" : L"",
             which == (DLSS_SR | DLSS_FG) ? TW(L" and ") : L"", which & DLSS_FG ? L"nvngx_dlssg.dll" : L"",
             k_dlss_dll_version, dlss_download_mb(which));
    swprintf(what, 1200,
             TW(L"%ls Its DLL is NVIDIA's, under NVIDIA's RTX SDK license, so it is not in the download.\n\n"
             L"Ascension-R can download %ls from NVIDIA's own DLSS repository on GitHub (github.com/NVIDIA/DLSS), check "
             L"it against the SHA-256 Ascension-R keeps and NVIDIA's signature, and put it in the dlss folder beside the "
             L"program. Nothing else is sent or installed."),
             which == DLSS_FG ? TW(L"NVIDIA DLSS frame generation makes a frame between each two with NVIDIA's own model, on "
                                L"GeForce RTX 40 and newer cards.")
                              : TW(L"NVIDIA DLSS builds the picture with NVIDIA's own model on GeForce RTX and other NVIDIA RTX "
                                L"cards."),
             files);
    int pressed = 0;
    const task_dialog_fn td = task_dialog();
    if (td) {
        TASKDIALOG_BUTTON b[2] = { { 100, TW(L"Download and set up") }, { 102, TW(L"Not now") } };
        TASKDIALOGCONFIG c = {};
        c.cbSize = sizeof c;
        c.hwndParent = s_main;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.pszWindowTitle = TW(L"NVIDIA DLSS (beta)");
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.pszMainInstruction = TW(L"Download NVIDIA DLSS?");
        c.pszContent = TW(what);
        c.cButtons = 2;
        c.pButtons = b;
        c.nDefaultButton = 100;
        if (FAILED(td(&c, &pressed, NULL, NULL))) return;
    } else {
        pressed = MessageBoxW(s_main, TW(what), TW(L"Download NVIDIA DLSS?"), MB_YESNO | MB_ICONQUESTION) == IDYES ? 100 : 102;
    }
    if (pressed == 100) dlss_dl_start(which);
}

/* What follows a choice: ANR FG or NVIDIA DLSS chosen without its files asks whether to fetch them. */
static void opt_changed(const Opt& o)
{
    if (!strcmp(o.section, "launcher") && !strcmp(o.key, "language")) lang_set(o.cur);
    if (!strcmp(o.section, "online")) online_sync();
    if (!strcmp(o.key, "frame_generation") && ps3_settings_ieq(o.cur, "anr") && !anr_present() && !s_anr_dl.running)
        anr_offer();
    if (!strcmp(o.key, "scaling") && ps3_settings_ieq(o.cur, "dlss") && gpu_maybe_rtx() && !dlss_present() &&
        !s_dlss_dl.running)
        dlss_offer(DLSS_SR);
    if (!strcmp(o.key, "frame_generation") && ps3_settings_ieq(o.cur, "dlss") && gpu_maybe_rtx() && !dlssg_present() &&
        !s_dlss_dl.running)
        dlss_offer(DLSS_FG);
}

static void act(int id)
{
    if (id >= H_MOD) { mods_act(id - H_MOD); return; }
    if (id >= H_DDITEM) {
        if (s_dd >= 0 && id - H_DDITEM < s_opt[s_dd].n) {
            opt_set(s_opt[s_dd], s_opt[s_dd].values[id - H_DDITEM]);
            const int was = s_dd;
            s_dd = -1;
            opt_changed(s_opt[was]);
        }
        s_dd = -1;
        return;
    }
    if (id >= H_KBIND) {   /* a binding of the keyboard keys: it waits for a key */
        s_capture = id - H_KBIND;
        return;
    }
    if (id >= H_PLAY) {
        switch (id) {
        case H_PLAY:
            if (!s_rc.elf_ok) {
                s_tab = T_SYSTEM;
                toast_bad(game_needed_toast());
                break;
            }
#ifdef PORT_GAME_MODULE
            /* The game's code first: Play builds it, and the player plays once it is there. */
            if (s_kit.running) { toast("The game's code is still being built"); break; }
            if (kit_state() == 3) { toast_bad("No Kit folder to build the game's code with"); break; }
            if (kit_state() != 0) { s_tab = T_PLAY; kit_start(); break; }
#endif
            /* The second link, if only the first was made. */
            if (s_game.g.state >= GAME_NO_ELF && GetFileAttributesW(s_game.link_hdd0) == INVALID_FILE_ATTRIBUTES)
                game_link(s_game.link_hdd0, s_game.g.disc ? s_game.g.update : s_game.g.dir, NULL, 0);
            if (!save_all()) break;
            s_online_err[0] = 0;
            if (const char* p = gowa_online_problem()) {
                s_tab = T_ONLINE;
                toast_bad(p);
                break;
            }
            {
                char why[300];
                if (!gowa_online_start(why, sizeof why)) {   /* its node, and the game's variables from it */
                    snprintf(s_online_err, sizeof s_online_err, "%s", why);
                    s_tab = T_ONLINE;
                    toast_bad("Multiplayer could not start");
                    break;
                }
            }
            s_done = 1;
            break;
        case H_SAVE: if (save_all()) toast("Saved"); break;
        case H_QUIT: s_done = 2; break;
        case H_KEYS: s_dd = -1; kb_open(); break;
        case H_KB_OK: kb_close(1); break;
        case H_KB_CANCEL: kb_close(0); break;
        case H_KB_DEFAULTS:
            for (int i = 0; i < N_KB; i++) snprintf(s_kb[i], sizeof s_kb[i], "%s", k_kb[i][2]);
            s_capture = -1;
            break;
        case H_SAVES: open_saves(); break;
        case H_EXPORT_SAVES: export_saves(); break;
        case H_IMPORT_SAVES: import_saves(); break;
        case H_UPDATE_OPEN: update_open(); break;
        case H_SUPPORT: ShellExecuteW(s_main, L"open", SUPPORT_PAGE, NULL, NULL, SW_SHOWNORMAL); break;
        case H_UPDATE_CLOSE: update_check_dismiss(&s_upd); break;
        case H_REPORT: report_make(NULL); break;
        case H_SHADERS: clear_shaders(); break;
        case H_PREBUILD: pb_start(); break;
        case H_ANR_ZIP: anr_offer(); break;
        case H_ANR_STOP: game_progress_stop(&s_anr_dl.p); break;
        case H_DLSS_GET: dlss_offer(dlss_wanted()); break;
        case H_DLSS_STOP: game_progress_stop(&s_dlss_dl.p); break;
        case H_MODS_OPEN: mods_open(); break;
        case H_GAME_BROWSE: game_browse(); break;
        case H_PICK_RPCS3:
            if (pick_file(L"Choose rpcs3.exe", L"rpcs3.exe", L"rpcs3.exe", s_prep.rpcs3, GAME_PATH_MAX)) prepare_begin();
            break;
        case H_GET_RPCS3:
            s_prep.download = 1;
            prepare_begin();
            if (!s_prep.thread && !s_game.g.disc) toast("Now choose the game's license file (.rap)");
            break;
        case H_PICK_RAP:
            if (pick_file(L"The game's license file", L"License file (*.rap)", L"*.rap", s_prep.rap, GAME_PATH_MAX)) prepare_begin();
            break;
        case H_PREP_RETRY: prepare_begin(); break;
        case H_PREP_STOP: if (s_prep.thread) game_progress_stop(&s_prep.progress); break;
#ifdef PORT_GAME_MODULE
        case H_KIT_BUILD: kit_start(); break;
        case H_KIT_STOP: kit_stop(); break;
        case H_KIT_LOG: kit_log(); break;
#endif
        case H_GAME_AUTO:
            if (s_find.thread) s_find.stop = 1;
            else find_start(NULL);
            break;
        case H_DEFAULTS:
            for (int i = 0; i < N_OPT; i++) if (!s_opt[i].text) opt_set(s_opt[i], s_opt[i].def);   /* a name stays */
            for (int i = 0; i < s_npopt; i++) opt_set(s_popt[i], s_popt[i].def);
            online_sync();
            if (const Opt* l = opt_find("launcher", "language")) lang_set(l->cur);
            toast("Defaults back: Save or Play keeps them");
            break;
        }
        return;
    }
    if (id >= H_TXT) { s_edit = id - H_TXT; return; }
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
        if (!strcmp(o.section, "online")) online_sync();
        return;
    }
    if (id >= H_SEG) {
        Opt& o = s_opt[(id - H_SEG) / 16];
        opt_set(o, o.values[(id - H_SEG) % 16]);
        opt_changed(o);
        return;
    }
    if (id >= H_NAV) {
        s_tab = id - H_NAV;
        if (s_tab == T_MODS) s_mods_stale = 1;   /* what the folder has now */
        return;
    }
}

static int hit_at(float x, float y)
{
    for (int i = s_nhit - 1; i >= 0; i--) if (inside(s_hit[i].r, x, y)) return s_hit[i].id;
    return -1;
}

/* Something changed: the layer is drawn again in the next frame. */
static void redraw(void)
{
    s_layer_old = 1;
    if (s_main) InvalidateRect(s_main, NULL, FALSE);   /* NULL would be every window there is */
}

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
            frame_draw();
            /* What is under the mouse now that the window has changed under it. */
            const int now = s_mx >= 0 ? hit_at(s_mx, s_my) : -1;
            if (now != s_hover) { s_hover = now; redraw(); }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:   /* back to the front: a mod may have been put into the folder meanwhile */
        if (LOWORD(w) != WA_INACTIVE) {
            s_mods_stale = 1;
            if (s_tab == T_MODS) redraw();
        }
        break;
    case WM_DPICHANGED:
        place(h, HIWORD(w), (const RECT*)l, 0);
        rt_release();
        redraw();
        return 0;
    case WM_SIZE:
        anim(w != SIZE_MINIMIZED);
        if (s_rt && LOWORD(l) && HIWORD(l)) {   /* a new size: a new target, and layers of that size */
            const D2D1_SIZE_U z = static_cast<ID2D1HwndRenderTarget*>(s_rt)->GetPixelSize();
            if (z.width != LOWORD(l) || z.height != HIWORD(l)) {
                rt_release();
                redraw();
            }
        }
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
        if (s_edit >= 0 && s_press != H_TXT + s_edit) edit_end();   /* a click elsewhere ends the typing */
        if (s_capture >= 0 && s_press == H_KBIND + s_capture) {   /* on the binding that waits: the left button */
            kb_mouse(m, w);
            s_press = -1;
            return 0;
        }
        if (s_dd >= 0 && s_press < 0) s_dd = -1;   /* a click outside closes the list */
        redraw();   /* a button pressed is drawn sunk */
        SetCapture(h);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:   /* the other buttons count only on the binding that waits */
        if (s_capture >= 0 && hit_at((float)(short)LOWORD(l) * 96.0f / s_ui, (float)(short)HIWORD(l) * 96.0f / s_ui) ==
                                  H_KBIND + s_capture) {
            kb_mouse(m, w);
            return m == WM_XBUTTONDOWN ? TRUE : 0;
        }
        break;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        s_mx = (float)(short)LOWORD(l) * 96.0f / s_ui;
        s_my = (float)(short)HIWORD(l) * 96.0f / s_ui;
        const int at = hit_at(s_mx, s_my);
        const int pressed = s_press;
        s_press = -1;
        if (pressed >= 0 && pressed == at) act(pressed);
        redraw();
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (s_kb_open) return 0;
        if (s_dd >= 0) {
            s_dd_top += GET_WHEEL_DELTA_WPARAM(w) > 0 ? -2 : 2;
            redraw();
        } else if (s_tab != T_PLAY && s_scroll_max[s_tab] > 0.0f) {
            s_scroll[s_tab] -= (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 60.0f;   /* clamped as it is drawn */
            redraw();
        }
        return 0;
    case WM_KEYDOWN:
        if (s_kb_open) {   /* over the keyboard keys: Esc is Cancel, Enter OK, the rest waits for them */
            if (w == VK_ESCAPE || w == VK_RETURN) {
                kb_close(w == VK_RETURN);
                redraw();
            }
            return 0;
        }
        if (s_edit >= 0) {   /* typing into a box: Enter, Esc and Tab end it; the characters come as WM_CHAR */
            if (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB) {
                edit_end();
                redraw();
                return 0;
            }
            break;
        }
        if (w == VK_ESCAPE && s_dd >= 0) { s_dd = -1; redraw(); return 0; }
        if (w == VK_RETURN && s_dd < 0) { act(H_PLAY); return 0; }
        if (w == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) { act(H_SAVE); redraw(); return 0; }
        if (w == VK_TAB || w == VK_NEXT || w == VK_PRIOR) {   /* tabs by keyboard */
            const int back = w == VK_PRIOR || (w == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000));
            s_tab = (s_tab + (back ? T_N - 1 : 1)) % T_N;
            if (s_tab == T_MODS) s_mods_stale = 1;
            s_dd = -1;
            redraw();
            return 0;
        }
        break;
    case WM_CHAR:
        if (s_edit >= 0) {
            Opt& o = s_opt[s_edit];
            const size_t n = strlen(o.cur);
            if (w == 8) { if (n) o.cur[n - 1] = 0; }   /* Backspace */
            else if (w == 0x7F) o.cur[0] = 0;          /* Ctrl+Backspace: all of it */
            else if (w == 0x16) text_paste(o);         /* Ctrl+V */
            else if (w >= 32 && w < 127) text_put(o, (int)w);
            redraw();
            return 0;
        }
        break;
    case WM_ONLINE:
        mp_refresh();
        redraw();
        return 0;
    case WM_GAME_FOUND:
        find_done();
        redraw();
        return 0;
    case WM_GAME_PREPARED:
        prepare_done();
        redraw();
        return 0;
    case WM_UPDATE:
        update_check_done(&s_upd, (unsigned long long)w);
        redraw();
        return 0;
#ifdef PORT_GAME_MODULE
    case WM_KIT_BUILD:
        if (w == 1) kit_end();   /* it exited */
        redraw();
        return 0;
#endif
    case WM_PREBUILD:
        if (w == 1) pb_end();   /* it exited */
        redraw();
        return 0;
    case WM_ANR:
        if (w == 1) anr_dl_end();
        redraw();
        return 0;
    case WM_DLSS:
        if (w == 1) dlss_dl_end();
        redraw();
        return 0;
    case WM_TIMER: {
        if (w == 3) {   /* the scene's next frame: the layer stays as it is */
            scene_tick();
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (w == 2) { redraw(); return 0; }
        if (s_toast[0] && GetTickCount64() >= s_toast_until) { s_toast[0] = 0; redraw(); }   /* the toast goes */
        if (mp_refresh()) redraw();   /* the node's players, as it knows them now */
        if (s_find.thread) redraw();   /* the folder it is in */
#ifdef PORT_GAME_MODULE
        if (s_kit.running) redraw();    /* the time it has taken */
#endif
        static int tick;
        if (++tick % 4 == 0) {   /* every 2 s: a controller plugged in or out */
            char name[sizeof s_rc.pad_name];
            const int pad = pad_connected(name, sizeof name);
            if (pad != s_rc.pad || strcmp(name, s_rc.pad_name)) {
                s_rc.pad = pad;
                snprintf(s_rc.pad_name, sizeof s_rc.pad_name, "%s", name);
                redraw();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        if (s_kb_open) {   /* the keyboard keys go first, as their own window's close did */
            kb_close(0);
            redraw();
            return 0;
        }
        s_done = 2;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* GOWA_LAUNCHER_SHOT=<folder>: every tab, the Display tab with its list open,
 * the System tab during a search, the Cheats, Graphics and System tabs scrolled
 * to their end, a toast, the System tab asking for what the decrypting needs, the
 * keyboard keys (one waiting for a key), and in the
 * Kit the game's code being built and not built yet, the Play tab with a
 * newer release's notice, and last the Play tab while Build all shaders now
 * runs, into PNG files at the
 * window's size times 1.25 (a 125% display),
 * or at GOWA_LAUNCHER_SHOT_DPI. The scene stands still in them, and nothing
 * runs: each state is only set while its shot is drawn, then put back. */
enum { S_TAB, S_LIST, S_SEARCH, S_END, S_TOAST, S_DECRYPT, S_KEYS, S_KEYS_WAIT, S_KIT_RUN, S_KIT_NEED, S_KIT_ROW,
       S_UPDATE, S_PREBUILD, S_ONLINE_JOIN, S_ONLINE_LIST };
static const int k_shots[][2] = {
    { T_PLAY, S_TAB }, { T_GRAPHICS, S_TAB }, { T_DISPLAY, S_TAB }, { T_CONTROLS, S_TAB }, { T_PATCHES, S_TAB },
    { T_CHEATS, S_TAB }, { T_MODS, S_TAB }, { T_TROPHIES, S_TAB }, { T_TROPHIES, S_END }, { T_SYSTEM, S_TAB },
    { T_DISPLAY, S_LIST },
    { T_SYSTEM, S_SEARCH }, { T_CHEATS, S_END },
    { T_GRAPHICS, S_END }, { T_DISPLAY, S_END }, { T_SYSTEM, S_END }, { T_PLAY, S_TOAST }, { T_SYSTEM, S_DECRYPT },
    { T_CONTROLS, S_KEYS }, { T_CONTROLS, S_KEYS_WAIT },
#ifdef PORT_GAME_MODULE
    { T_PLAY, S_KIT_RUN }, { T_PLAY, S_KIT_NEED }, { T_SYSTEM, S_KIT_ROW },
#endif
    { T_PLAY, S_UPDATE }, { T_PLAY, S_PREBUILD }, { T_MODS, S_END },
    { T_ONLINE, S_TAB }, { T_ONLINE, S_ONLINE_JOIN }, { T_ONLINE, S_END }, { T_ONLINE, S_ONLINE_LIST },
    { T_PLAY, S_ONLINE_LIST },
};

static void shot_state(int what, int on)
{
    static GameInfo game;
    static int elf_ok;
    if (what == S_SEARCH) {   /* a search under way: the row only says so */
        s_find.thread = on ? GetCurrentThread() : NULL;
        swprintf(s_find.now, GAME_PATH_MAX, L"%ls", on ? L"D:\\Games\\PS3\\RPCS3\\dev_hdd0\\game" : L"");
    }
    if (what == S_TOAST) {
        if (on) toast("Saved");
        else s_toast[0] = 0;
    }
    if (what == S_PREBUILD) {   /* Build all shaders now under way: its link says how far */
        s_pb.running = on;
        s_pb.done = on ? 21000 : 0;
        s_pb.total = on ? 49996 : 0;
    }
    if (what == S_DECRYPT) {
        if (on) { game = s_game.g; s_game.g.state = GAME_NO_ELF; }
        else s_game.g = game;
    }
    if (what == S_UPDATE) {   /* the next pre-release (or release) after this one, known */
        static UpdateState was;
        if (on) {
            was = s_upd;
            memset(&s_upd, 0, sizeof s_upd);
            ArVersion v;
            update_version_parse(AR_VERSION, &v);
            s_upd.known = 1;
            s_upd.rel.prerelease = v.pre;
            if (v.pre)
                snprintf(s_upd.rel.tag, sizeof s_upd.rel.tag, "v%d.%d.%d-%s.%d", v.major, v.minor, v.patch, v.pre_label,
                         v.pre_n + 1);
            else
                snprintf(s_upd.rel.tag, sizeof s_upd.rel.tag, "v%d.%d.%d", v.major, v.minor, v.patch + 1);
            snprintf(s_upd.rel.url, sizeof s_upd.rel.url, "%stag/%s", UPDATE_PAGE_PREFIX, s_upd.rel.tag);
        } else {
            s_upd = was;
        }
    }
    if (what == S_ONLINE_LIST) {   /* Auto, the node on, and the players it knows of */
        static char mode[64];
        Opt* m = opt_find("online", "mode");
        if (on) {
            snprintf(mode, sizeof mode, "%s", m->cur);
            opt_set(*m, "auto");
            s_mp_fake = 1;
            s_mp_status = 1;
            snprintf(s_mp_text, sizeof s_mp_text, "%s",
                     "me\teasy\tready\t4\nplayer\tsearching\tdirect\tKratosFan\nplayer\tplaying\tthrough\tDeimos_77\n"
                     "player\tlobby\tdirect\tCallisto\nplayer\tready\theard\tSpartanX\n");
            mp_parse();
        } else {
            opt_set(*m, mode);
            s_mp_fake = 0;
            s_mp_status = 0;
            s_mp_text[0] = 0;
            mp_parse();
        }
    }
    if (what == S_ONLINE_JOIN) {   /* Join, the host's address being typed, and why the last Play did not start it */
        static char mode[64], server[64];
        Opt* m = opt_find("online", "mode");
        Opt* sv = opt_find("online", "server");
        if (on) {
            snprintf(mode, sizeof mode, "%s", m->cur);
            snprintf(server, sizeof server, "%s", sv->cur);
            opt_set(*m, "join");
            opt_set(*sv, "192.168.1.2");
            s_edit = (int)(sv - s_opt);
            snprintf(s_online_err, sizeof s_online_err, "%s", T("Multiplayer: cannot find gowa-host: is the address right?"));
        } else {
            opt_set(*m, mode);
            opt_set(*sv, server);
            s_edit = -1;
            s_online_err[0] = 0;
        }
    }
    if (what == S_KEYS || what == S_KEYS_WAIT) {   /* the keyboard keys, and one waiting for a key; never kept */
        if (on) kb_open();
        else kb_close(0);
        if (on && what == S_KEYS_WAIT) s_capture = 9;
    }
#ifdef PORT_GAME_MODULE
    if (what == S_KIT_RUN || what == S_KIT_ROW) {
        s_kit.running = on;
        s_kit.permille = on ? 420 : 0;
        s_kit.left = on ? 360 : -1;
        s_kit.started = GetTickCount64();
        swprintf(s_kit.what, 200, L"%ls", on ? TW(L"Compiling the game's code") : L"");
    }
    if (what == S_KIT_NEED) {
        static int state;
        if (on) { state = s_kit.state; elf_ok = s_rc.elf_ok; s_kit.state = 1; s_rc.elf_ok = 1; }
        else { s_kit.state = state; s_rc.elf_ok = elf_ok; }
    }
#else
    (void)elf_ok;
#endif
}

static void shots(const char* dir)
{
    float dpi = 120.0f;
    if (const char* d = getenv("GOWA_LAUNCHER_SHOT_DPI")) {
        const float v = (float)atof(d);
        if (v >= 48.0f && v <= 480.0f) dpi = v;
    }
    const UINT pw = (UINT)(WIN_W * dpi / 96.0f + 0.5f), ph = (UINT)(WIN_H * dpi / 96.0f + 0.5f);
    IWICBitmap* bmp = NULL;
    if (!s_wic || FAILED(s_wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return;
    if (FAILED(s_d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi), &s_rt))) {
        bmp->Release();
        return;
    }
    rt_resources();
    CreateDirectoryA(dir, NULL);
    for (int t = 0; t < (int)(sizeof k_shots / sizeof k_shots[0]); t++) {
        const int what = k_shots[t][1];
        s_tab = k_shots[t][0];
        s_scroll[s_tab] = what == S_END || what == S_ONLINE_LIST ? 1e9f : 0.0f;   /* to its end */
        s_dd = -1;
        s_hover = -1;
        shot_state(what, 1);
        if (what == S_LIST) {   /* the window size list, open */
            s_layer_old = 1;
            frame_draw();
            for (int i = 0; i < s_nhit; i++)
                if (s_hit[i].id == H_DD + (int)(opt_find("display", "window_size") - s_opt)) {
                    s_dd = s_hit[i].id - H_DD;
                    s_dd_at = s_hit[i].r;
                }
        }
        s_layer_old = 1;
        frame_draw();
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
        shot_state(what, 0);
    }
    rt_release();
    bmp->Release();
}

static void ui_release(void)
{
    rt_release();
    IDWriteTextFormat** f[] = { &s_tf_ui, &s_tf_ui_b, &s_tf_small, &s_tf_cap, &s_tf_head, &s_tf_btn, &s_tf_title,
                                &s_tf_play, &s_tf_desc, &s_tf_path };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) drop(*f[i]);
    for (int i = 0; i < 4; i++) drop(s_tf_icon[i]);
    drop(s_tf_name);
    drop(s_fonts);
    if (s_dw5 && s_font_mem) s_dw5->UnregisterFontFileLoader(s_font_mem);
    drop(s_font_mem);
    drop(s_dw5);
    drop(s_wic);
    drop(s_dw);
    drop(s_d2d);
}

extern "C" int gowa_launcher(void)
{
    s_t0 = GetTickCount64();
    {
        typedef UINT (WINAPI *get_dpi_fn)(void);
        HMODULE user = GetModuleHandleA("user32.dll");
        get_dpi_fn get_dpi = user ? (get_dpi_fn)(void*)GetProcAddress(user, "GetDpiForSystem") : NULL;
        s_dpi = get_dpi ? (int)get_dpi() : 96;
        if (s_dpi < 96) s_dpi = 96;
    }

    for (int i = 0; i < N_OPT; i++) opt_set(s_opt[i], ps3_settings_str(s_opt[i].section, s_opt[i].key, NULL, s_opt[i].def));
    {   /* the window's language (GOWA_LAUNCHER_LANG over the row, for the shots) */
        const char* e = getenv("GOWA_LAUNCHER_LANG");
        const Opt* l = opt_find("launcher", "language");
        lang_set(e && *e ? e : l ? l->cur : "auto");
    }
    popt_init();
    for (int i = 0; i < s_npopt; i++)
        opt_set(s_popt[i], ps3_settings_str(s_popt[i].section, s_popt[i].key, NULL, s_popt[i].def));
    /* NVIDIA DLSS frame generation is turned off (see k_fg): a file from a test build that chose it gets frame
     * generation off, written back. */
    {
        Opt* fg = opt_find("graphics", "frame_generation");
        if (fg && ps3_settings_ieq(fg->cur, "dlss")) {
            opt_set(*fg, "off");
            ps3_settings_set("graphics", "frame_generation", "off");
            ps3_settings_save(NULL);
        }
    }
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
    /* The program's icon (app.rc), and its 16-pixel drawing for the title bar; Windows' own if there is none. */
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hIconSm = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = "gowa_launcher";
    RegisterClassExA(&wc);

    s_main = CreateWindowExA(0, "gowa_launcher", "Ascension-R",
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
    anim(1);
    if (!s_rc.elf_ok) find_start(NULL);   /* no game yet: look for it */
    if (const Opt* u = opt_find("launcher", "update_check"))
        if (opt_on(*u)) update_check_begin(s_main, WM_UPDATE, &s_upd);   /* a newer release on GitHub */
    report_offer();
    online_sync();   /* on Auto, the node from now on: players found before Play */
    redraw();   /* what the offer's answer did (a toast) */

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
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    KillTimer(s_main, 1);
    KillTimer(s_main, 2);
    anim(0);
    if (s_prep.thread) game_progress_stop(&s_prep.progress);
    prepare_end();
#ifdef PORT_GAME_MODULE
    kit_kill();
#endif
    s_find.stop = 1;
    find_end();
    update_check_end();
    pb_stop();
    if (s_anr_dl.thread) {   /* a download of ANR FG stops before the game could load a half-made folder */
        game_progress_stop(&s_anr_dl.p);
        WaitForSingleObject(s_anr_dl.thread, 15000);
        CloseHandle(s_anr_dl.thread);
        s_anr_dl.thread = NULL;
    }
    if (s_done == 1) {   /* the game opens on the monitor the launcher was on, and Ultrawide goes by that monitor */
        RECT lr;
        if (GetWindowRect(s_main, &lr)) rsx_d3d12_backend_open_on((lr.left + lr.right) / 2, (lr.top + lr.bottom) / 2);
    }
    DestroyWindow(s_main);
    /* Let the destroy messages through before the game opens its window. */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    ui_release();
    if (SUCCEEDED(com)) CoUninitialize();
    return s_done == 1;
}
#else
extern "C" int gowa_launcher(void) { return 1; }   /* no window here: just play */
extern "C" int gowa_anr_zip(const char*, char* why, size_t n) { snprintf(why, n, "ANR FG is Windows only"); return 1; }
extern "C" int gowa_anr_get(char* why, size_t n) { snprintf(why, n, "ANR FG is Windows only"); return 1; }
#endif
