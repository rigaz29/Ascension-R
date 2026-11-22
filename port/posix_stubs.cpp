/*
 * Ascension-R -- what stands in off Windows for the parts that are Windows only so far
 *
 * The port was written for Windows alone. On macOS (and other POSIX hosts) the files below are built empty
 * (#ifdef _WIN32), and this file gives the rest of the program what they would have, so that the game itself runs:
 *
 *   game_find.cpp        the edition of the game in a folder (game_title_of_vfs: read here from PARAM.SFO), and
 *                        the search for the game (--find-game: not here yet);
 *   game_prepare.cpp     the game decrypted and prepared from RPCS3's files (--prepare-game: not here yet; run
 *                        scripts/gowa.py, or prepare it on Windows, and point [paths] at it);
 *   precompile.cpp       the D3D12 shader cache built ahead (the Metal backend has its own path);
 *   mods.cpp,            the mods folder and textures in place of the game's (not here yet: no mods are read);
 *   textures.cpp
 *   multiplayer.cpp      the multiplayer node (not here yet: the game plays single player, as it does by default);
 *   rsx_d3d12_backend    the overlay line, the trophy notice and the ultrawide picture, D3D12 only so far: no
 *                        overlay line, the notice in the log, the title's own 16:9.
 *
 * docs/MACOS.md lists what is left, and how each could be done.
 */
#ifndef _WIN32

#include "game_title.h"
#include "mods.h"
#include "textures.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

/* --- the edition of the game (game_find.cpp on Windows) --------------------------------------------------------- */

/* PARAM.SFO's TITLE_ID, into id (at least 16 bytes). 1 when read. The file: a header ("\0PSF", version, the key
 * table's offset, the data table's, the count), then 16 bytes an entry (key offset u16, format u16, length u32, room
 * u32, data offset u32), all little-endian. */
static int sfo_title_id(const char* path, char* id, size_t n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[64 * 1024];
    const size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= got ? (uint32_t)(buf[o] | buf[o + 1] << 8) : 0; };
    auto u32 = [&](size_t o) -> uint32_t {
        return o + 4 <= got ? (uint32_t)buf[o] | (uint32_t)buf[o + 1] << 8 | (uint32_t)buf[o + 2] << 16 |
                                  (uint32_t)buf[o + 3] << 24
                            : 0;
    };
    if (got < 20 || memcmp(buf, "\0PSF", 4) != 0) return 0;
    const uint32_t keys = u32(8), data = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + (size_t)i * 16 + 16 <= got; i++) {
        const size_t e = 20 + (size_t)i * 16;
        const size_t k = (size_t)keys + u16(e);
        if (k >= got || strncmp((const char*)buf + k, "TITLE_ID", got - k) != 0) continue;
        const size_t d = (size_t)data + u32(e + 12);
        size_t len = u32(e + 4);
        if (d >= got || len == 0) return 0;
        if (d + len > got) len = got - d;
        if (len >= n) len = n - 1;
        memcpy(id, buf + d, len);
        id[len] = 0;
        return id[0] != 0;
    }
    return 0;
}

const GameTitle* game_title_of_vfs(const char* vfs)
{
    if (!vfs || !*vfs) return game_title_default();
    std::string p = std::string(vfs) + "/PS3_GAME/PARAM.SFO";
    char id[32];
    if (!sfo_title_id(p.c_str(), id, sizeof id)) return game_title_default();
    const GameTitle* t = game_title_find(id);
    return t ? t : game_title_default();
}

extern "C" int gowa_find_game_cli(const char* under)
{
    (void)under;
    fprintf(stderr, "--find-game: not on this platform yet; set [paths] vfs, hdd0 and elf in the settings\n");
    return 1;
}

/* --- preparing the game (game_prepare.cpp) ------------------------------------------------------------------------ */

extern "C" int gowa_prepare_cli(const char* game, const char* rpcs3, const char* rap, const char* settings)
{
    (void)game; (void)rpcs3; (void)rap; (void)settings;
    fprintf(stderr, "--prepare-game: not on this platform yet (docs/MACOS.md)\n");
    return 1;
}

/* --- the shader cache built ahead (precompile.cpp: D3D12's) ----------------------------------------------------- */

extern "C" int gowa_precompile(void)
{
    fprintf(stderr, "--precompile-shaders: D3D12's; the Metal backend builds its pipelines as the game draws\n");
    return 0;
}

extern "C" int gowa_prebuild(void) { return gowa_precompile(); }

/* --- multiplayer (multiplayer.cpp) ------------------------------------------------------------------------------- */

extern "C" int gowa_online_start(char* why, size_t n)
{
    /* 1: nothing to start. The game plays single player, as it does with multiplayer off. */
    if (why && n) why[0] = 0;
    return 1;
}

/* --- mods and their textures (mods.cpp, textures.cpp) ------------------------------------------------------------ */

std::wstring mods_folder(void) { return std::wstring(); }

void mods_scan(ModList* out, int unpack)
{
    (void)unpack;
    *out = ModList();
    out->use = 0;
}

void mods_store(const ModList& l) { (void)l; }

void mods_pack(ModList* l, const std::wstring& usrdir, const std::wstring& disc)
{
    (void)usrdir; (void)disc;
    if (l) l->pack_error = "mods: not on this platform yet";
}

std::string mods_dirs(const ModList& l) { (void)l; return std::string(); }
std::string mods_proportions(const ModList& l) { (void)l; return std::string(); }
std::string mods_summary(const ModList& l) { (void)l; return std::string(); }

std::string mods_unzip(const std::wstring& zip, const std::wstring& to)
{
    (void)zip; (void)to;
    return "mods: not on this platform yet";
}

void mods_remove_tree(const std::wstring& dir, const std::wstring& within) { (void)dir; (void)within; }

int gowa_textures_start(const ModList& l, const std::wstring& dump_dir)
{
    (void)l; (void)dump_dir;
    return 0;
}

std::wstring gowa_textures_dump_folder(void) { return std::wstring(); }

/* --- what the port asks of the D3D12 backend, off it ---------------------------------------------------------- */

extern "C" void rsx_d3d12_backend_overlay_line(int (*fn)(char* out, int max)) { (void)fn; }

extern "C" void rsx_d3d12_backend_trophy(const char* caption, const char* name, int grade,
                                         const unsigned char* icon, int w, int h)
{
    (void)grade; (void)icon; (void)w; (void)h;
    fprintf(stderr, "[trophy] %s: %s\n", caption ? caption : "", name ? name : "");
}

/* The title's own 16:9: display.ultrawide is D3D12's so far. */
extern "C" float rsx_d3d12_backend_wide_ratio(void) { return 1.0f; }

#endif /* !_WIN32 */
