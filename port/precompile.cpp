/*
 * Precompile: the game's vertex programs, from its archives, into the shader cache before it runs.
 * Ascension-R.exe --precompile-shaders [--settings <file>]; the game's start runs it while this version's vertex
 * programs are not built yet (the precompile gate, loading_screen.cpp).
 *
 * The programs sit in the WAD and AS files inside the PSARC archives beside the executable, each behind a header
 * of the engine's own: the instruction count (u16), 0 (u16), the input and output masks (u32 each), 0020FFFF, and
 * the program's offset from the header (u32: 0x20 or 0x30 here). The program follows there, in big-endian words,
 * with bit 0 of the last word set in its last instruction and in no other. A scan of every archive found 3,455
 * distinct ones (2026-11-08), 94 of the 100 a session of the first level drew with
 * (docs/notes/2026-11-08-shader-precompile-review.md). Only WAD and AS entries are read, about 7 GB unpacked of
 * the 37 GB of archives, archive by archive and each in the order its entries lie, as the game's folder may be on
 * a hard disk.
 *
 * The backend compiles each program's text and its raw-fetch text as a draw would
 * (rsx_d3d12_precompile_vertex_programs). Fragment programs are not done: their text depends on the textures and
 * state of their draws, which the files do not say.
 *
 * The vertex pass, the port's one compute program for EDGE's lighting on the GPU (native/spu_edge_gpulight.c),
 * is built too, on a thread of its own while the archives are read and the programs compiled: in play the CPU lit
 * the draws until it was there (33 s at a first start after it changed, the frame rate down to 8 at the first
 * level). Its text is the game DLL's in the Kit (ascension_game_vertex_pass, the DLL loaded for that alone), the
 * program's own otherwise; it is one more program in the progress. Without it (no DLL, an older one, a compile
 * that fails) the game compiles it at its first lit draw, as before.
 *
 * Beside the cache it leaves <cache>.vprogs, the programs found (read instead of the archives while their count and
 * bytes are the same: a run after an update takes seconds), and when the build is done <cache>.precompiled, which
 * says the version it was built for: the game's start does it first while that is missing or another version's
 * (loading_screen.cpp). GOWA_PRECOMPILE_FOREGROUND keeps the process at normal priority, for that start.
 *
 * Progress, one line each on stdout, for the gate's screen:
 *   precompile: read <archive n> <of> <name>
 *   precompile: found <programs>
 *   precompile: compile <done> <of>
 *   precompile: done <compiled> <had> <failed> <seconds>
 *   precompile: error <why>
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32   /* Windows only for now: port/posix_stubs.cpp stands in for it elsewhere (docs/MACOS.md) */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include "third_party/zlib/zlib.h"
#include "ps3emu/ps3_settings.h"
#include "rsx_d3d12_backend.h"
#include "version.h"
#include "game_title.h"

static uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be40(const uint8_t* p) { return (uint64_t)p[0] << 32 | be32(p + 1); }

static int read_at(HANDLE f, uint64_t off, void* buf, uint32_t n)
{
    OVERLAPPED o = {};
    o.Offset = (DWORD)off;
    o.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(f, buf, n, &got, &o) && got == n;
}

struct PsarcEntry { std::string name; uint32_t block; uint64_t size, off; };

/* A PSARC's table of contents: its entries (named from the manifest, entry 0), the
 * block size, and each block's packed size (0: a whole block stored). */
static int psarc_toc(HANDLE f, std::vector<PsarcEntry>& ents, std::vector<uint32_t>& zs, uint32_t* block)
{
    uint8_t h[32];
    if (!read_at(f, 0, h, 32) || memcmp(h, "PSAR", 4) || memcmp(h + 8, "zlib", 4)) return 0;
    const uint32_t toc_len = be32(h + 12), esz = be32(h + 16), n = be32(h + 20);
    *block = be32(h + 24);
    if (toc_len < 32 || toc_len > (64u << 20) || esz < 30 || !n || (uint64_t)n * esz > toc_len - 32 || !*block) return 0;
    std::vector<uint8_t> t(toc_len - 32);
    if (!read_at(f, 32, t.data(), (uint32_t)t.size())) return 0;
    const uint32_t bsz = *block <= 65536 ? 2 : *block <= 16777216 ? 3 : 4;
    for (size_t k = (size_t)n * esz; k + bsz <= t.size(); k += bsz) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < bsz; i++) v = v << 8 | t[k + i];
        zs.push_back(v);
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = t.data() + (size_t)i * esz;
        ents.push_back({ std::string(), be32(e + 16), be40(e + 20), be40(e + 25) });
    }
    return 1;
}

/* An entry, unpacked into out: its blocks read as one span, then each inflated
 * (or taken as it is: stored whole, or packed no smaller). */
static int psarc_read(HANDLE f, const PsarcEntry& e, const std::vector<uint32_t>& zs, uint32_t block,
                      std::vector<uint8_t>& out, std::vector<uint8_t>& span)
{
    if (e.size > (1ull << 31)) return 0;
    out.resize((size_t)e.size);
    uint64_t packed = 0;
    for (uint64_t got = 0, b = e.block; got < e.size; b++) {
        const uint32_t want = (uint32_t)std::min<uint64_t>(block, e.size - got);
        const uint32_t z = b < zs.size() ? zs[(size_t)b] : 0;
        packed += z ? z : block;
        got += want;
    }
    if (packed > (2ull << 30)) return 0;
    span.resize((size_t)packed);
    /* The last block's stored size may run past the file's end: read what there is. */
    LARGE_INTEGER fs;
    if (!GetFileSizeEx(f, &fs)) return 0;
    uint64_t avail = (uint64_t)fs.QuadPart > e.off ? (uint64_t)fs.QuadPart - e.off : 0;
    const uint32_t nread = (uint32_t)std::min<uint64_t>(packed, avail);
    if (!read_at(f, e.off, span.data(), nread)) return 0;
    size_t at = 0;
    for (uint64_t got = 0, b = e.block; got < e.size; b++) {
        const uint32_t want = (uint32_t)std::min<uint64_t>(block, e.size - got);
        const uint32_t z = b < zs.size() ? zs[(size_t)b] : 0;
        const uint32_t take = z ? z : block;
        if (at + std::min<uint32_t>(take, want) > nread) return 0;
        const uint8_t* s = span.data() + at;
        const int zlib_head = take >= 2 && s[0] == 0x78 && (s[1] == 0xDA || s[1] == 0x9C || s[1] == 0x01 || s[1] == 0x5E);
        if (!z || (take == want && !zlib_head)) {
            memcpy(out.data() + got, s, want);
        } else {
            z_stream zst = {};
            if (inflateInit(&zst) != Z_OK) return 0;
            zst.next_in = (Bytef*)s;
            zst.avail_in = take;
            zst.next_out = out.data() + got;
            zst.avail_out = want;
            const int r = inflate(&zst, Z_FINISH);
            inflateEnd(&zst);
            if (r != Z_STREAM_END || zst.avail_out) return 0;
        }
        at += take;
        got += want;
    }
    return 1;
}

/* The vertex programs in an entry, into found (each once). */
static void scan_vps(const std::vector<uint8_t>& d, std::unordered_set<std::string>& found)
{
    const uint8_t* p = d.data();
    const size_t n = d.size();
    for (size_t i = 0x0C; i + 8 <= n; i++) {
        if (p[i] != 0x00 || p[i + 1] != 0x20 || p[i + 2] != 0xFF || p[i + 3] != 0xFF) continue;
        const size_t h = i - 0x0C;
        const uint32_t ni = be16(p + h), z = be16(p + h + 2), off = be32(p + h + 0x10);
        if (!ni || ni > 512 || z || off < 0x20 || off > 0xFC || (off & 3) || h + off + 16ull * ni > n) continue;
        const uint8_t* u = p + h + off;
        int ok = 1;
        for (uint32_t k = 0; k < ni && ok; k++) ok = ((u[16 * k + 15] & 1) != 0) == (k == ni - 1);
        if (ok) found.emplace((const char*)u, 16ull * ni);
    }
}

/* A fragment program's walk: its 16-byte instructions, each followed by its inline constant when a source of it is
 * one (register type 2 in the low bits of a source word), until the instruction with the end bit; the words are
 * stored with their halves swapped. True when that end is exactly at size. */
static bool fp_ends_at(const uint8_t* p, uint32_t size)
{
    uint32_t k = 0;
    while (k + 16 <= size) {
        uint32_t d[4];
        for (int w = 0; w < 4; w++) {
            const uint32_t be = be32(p + k + 4 * w);
            d[w] = be << 16 | be >> 16;
        }
        k += 16;
        if ((d[1] & 3u) == 2u || (d[2] & 3u) == 2u || (d[3] & 3u) == 2u) k += 16;
        if (d[0] & 1u) return k == size;
    }
    return false;
}

/* The fragment programs in an entry, each once, with their control bits. Behind the engine's header: the program's
 * size (u32, a multiple of 16), its texture coordinate inputs (u32, at most 0xFFFF), its register count and control
 * bits (u32, 0x84 in the second byte in the files, 0x04 once loaded), 0 (u32: the program's RSX address once loaded),
 * the program's offset from the header (u32), then the table of the inline constants the engine patches. The offset
 * is 0x80, or 0x100 and 0x180 when that table is longer: the scan of 2026-11-18 took 0x80 alone and missed 36 of the
 * 74 programs a replay met that were not built ahead (docs/notes/2026-11-18-pixel-shader-prebuild-step2.md). Every one
 * ends at its size. Found by the 0x84, as each header has it in the files. */
static void scan_fps(const std::vector<uint8_t>& d, std::unordered_map<std::string, uint32_t>& found)
{
    const uint8_t* p = d.data();
    const size_t n = d.size();
    if (n < 0x14) return;
    for (const uint8_t* q = p + 0x0A; q < p + n; q++) {
        q = (const uint8_t*)memchr(q, 0x84, (size_t)(p + n - q));
        if (!q) break;
        const size_t h = (size_t)(q - p) - 0x0A;   /* 0x84 is the third byte of the u32 at +0x08 */
        if (h + 0x14 > n || be32(p + h + 0x0C)) continue;
        const uint32_t size = be32(p + h), tcm = be32(p + h + 4), rc = be32(p + h + 8), off = be32(p + h + 0x10);
        if (!size || (size & 15) || size > 0x10000 || tcm > 0xFFFF || off < 0x80 || off > 0x1000 || (off & 15) ||
            h + off + (size_t)size > n || !fp_ends_at(p + h + off, size))
            continue;
        found.emplace(std::string((const char*)p + h + off, size), rc & 0xFFu);
    }
}

/* The cache file the build fills (graphics.shader_cache), or "" for none. */
static std::string cache_file(void)
{
    const char* sc = ps3_settings_str("graphics", "shader_cache", "D3D12_SHADER_CACHE", NULL);
    if (!sc || !*sc || ps3_settings_ieq(sc, "off")) return std::string();
    return sc;
}

/* The programs found, beside the cache as <cache>.vprogs, for the next run: the archives are read once, not again
 * at every version. Kept with the archives' count and bytes, and read only while those are the same. The game's own
 * code: it stays on this PC like the cache, and nothing sends it anywhere. */
static const char k_vpl_magic[8] = { 'G', 'O', 'W', 'A', 'V', 'P', 'L', '1' };

static bool vp_list_read(const std::string& path, uint32_t narc, uint64_t bytes, std::unordered_set<std::string>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[8];
    uint32_t na = 0, n = 0;
    uint64_t by = 0;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, k_vpl_magic, 8) && fread(&na, 4, 1, f) == 1 &&
              fread(&by, 8, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && na == narc && by == bytes && n && n < 100000;
    std::vector<char> b;
    for (uint32_t i = 0; ok && i < n; i++) {
        uint32_t len = 0;
        ok = fread(&len, 4, 1, f) == 1 && len && len <= 512u * 16u && !(len & 15);
        if (!ok) break;
        b.resize(len);
        ok = fread(b.data(), 1, len, f) == len;
        if (ok) out.emplace(b.data(), len);
    }
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

static void vp_list_write(const std::string& path, uint32_t narc, uint64_t bytes,
                          const std::unordered_set<std::string>& progs)
{
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)progs.size();
    bool ok = fwrite(k_vpl_magic, 1, 8, f) == 8 && fwrite(&narc, 4, 1, f) == 1 && fwrite(&bytes, 8, 1, f) == 1 &&
              fwrite(&n, 4, 1, f) == 1;
    for (const std::string& s : progs) {
        const uint32_t len = (uint32_t)s.size();
        if (!ok) break;
        ok = fwrite(&len, 4, 1, f) == 1 && fwrite(s.data(), 1, len, f) == len;
    }
    ok = !fclose(f) && ok;
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp.c_str());
}

/* The fragment programs found, as <cache>.fprogs: the same, each with its control bits. 2: the scan takes the headers
 * longer than 0x80 too (scan_fps), so a list of 1 is read again. */
static const char k_fpl_magic[8] = { 'G', 'O', 'W', 'A', 'F', 'P', 'L', '2' };

static bool fp_list_read(const std::string& path, uint32_t narc, uint64_t bytes,
                         std::unordered_map<std::string, uint32_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[8];
    uint32_t na = 0, n = 0;
    uint64_t by = 0;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, k_fpl_magic, 8) && fread(&na, 4, 1, f) == 1 &&
              fread(&by, 8, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && na == narc && by == bytes && n && n < 1000000;
    std::vector<char> b;
    for (uint32_t i = 0; ok && i < n; i++) {
        uint32_t len = 0, ctrl = 0;
        ok = fread(&len, 4, 1, f) == 1 && fread(&ctrl, 4, 1, f) == 1 && len && len <= 0x10000u && !(len & 15);
        if (!ok) break;
        b.resize(len);
        ok = fread(b.data(), 1, len, f) == len;
        if (ok) out.emplace(std::string(b.data(), len), ctrl);
    }
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

static void fp_list_write(const std::string& path, uint32_t narc, uint64_t bytes,
                          const std::unordered_map<std::string, uint32_t>& progs)
{
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const uint32_t n = (uint32_t)progs.size();
    bool ok = fwrite(k_fpl_magic, 1, 8, f) == 8 && fwrite(&narc, 4, 1, f) == 1 && fwrite(&bytes, 8, 1, f) == 1 &&
              fwrite(&n, 4, 1, f) == 1;
    for (const auto& s : progs) {
        const uint32_t len = (uint32_t)s.first.size(), ctrl = s.second;
        if (!ok) break;
        ok = fwrite(&len, 4, 1, f) == 1 && fwrite(&ctrl, 4, 1, f) == 1 && fwrite(s.first.data(), 1, len, f) == len;
    }
    ok = !fclose(f) && ok;
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp.c_str());
}

/* <cache>.precompiled: built for this version (loading_screen.cpp's gate reads the first line). */
static void marker_write(const std::string& cache, const uint32_t stats[3])
{
    FILE* f = fopen((cache + ".precompiled").c_str(), "wb");
    if (!f) return;
    fprintf(f, "Ascension-R %s\n%u compiled, %u had, %u failed\n", AR_VERSION, stats[0], stats[1], stats[2]);
    fclose(f);
}

/* The vertex pass (EDGE's lighting on the GPU) is built beside the programs, on a thread of its own: one more
 * program in the progress. */
static volatile LONG s_vp_done;
static uint32_t s_pc_done, s_pc_total;   /* the programs' last progress */

static void compile_progress(uint32_t done, uint32_t total)
{
    static DWORD last;
    s_pc_done = done;
    s_pc_total = total;
    const DWORD now = GetTickCount();
    const uint32_t d = done + (uint32_t)s_vp_done, t = total + 1;
    if (d < t && now - last < 250) return;
    last = now;
    printf("precompile: compile %u %u\n", d, t);
}

/* The vertex pass's text: the game's DLL has it in the Kit (loaded for that alone, nothing of it run but the
 * function that returns it), the program itself otherwise. NULL, with why, if there is none. PORT_GAME_MODULE
 * (CMakeLists.txt, this file's own definition): PS3_GAME_MODULE_HOST reaches only the runtime's sources, and
 * with it the Kit's exe was linked against the program's text, which is in the DLL. */
#ifdef PORT_GAME_MODULE
extern "C" int game_module_where(wchar_t* path, size_t n);
static const char* vertex_pass_text(char* why, size_t why_len)
{
    wchar_t p[MAX_PATH + 64];
    if (!game_module_where(p, sizeof p / sizeof p[0])) { snprintf(why, why_len, "no game DLL"); return NULL; }
    HMODULE dll = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { snprintf(why, why_len, "the game DLL did not load (%lu)", GetLastError()); return NULL; }
    typedef const char* (*VertexPassFn)(void);
    const VertexPassFn f = (VertexPassFn)(void*)GetProcAddress(dll, "ascension_game_vertex_pass");
    if (!f) { snprintf(why, why_len, "the game DLL is from before the vertex pass's precompile"); return NULL; }
    return f();      /* the DLL stays loaded: the text is its constant */
}
#else
extern "C" const char* edge_gpul_vertex_pass_hlsl(void);
static const char* vertex_pass_text(char* why, size_t why_len)
{
    (void)why; (void)why_len;
    return edge_gpul_vertex_pass_hlsl();
}
#endif

struct VertexPassJob {
    const char* text;
    int rc, compiled;
    double seconds;
    char why[600];
};
static DWORD WINAPI vertex_pass_worker(LPVOID arg)
{
    VertexPassJob* j = (VertexPassJob*)arg;
    const DWORD t0 = GetTickCount();
    j->rc = rsx_d3d12_precompile_vertex_pass(j->text, &j->compiled, j->why, sizeof j->why);
    j->seconds = (GetTickCount() - t0) / 1000.0;
    InterlockedExchange(&s_vp_done, 1);
    return 0;
}

/* The game's PSARC archives, sorted, the folder of each, and their bytes in all. False, said as "<tag>: error ...",
 * when there are none. */
static bool game_archives(const char* tag, std::vector<std::string>& arcs, std::vector<std::string>& arc_dir,
                          uint64_t* arc_bytes)
{
    /* The archives where the game opens them, /dev_hdd0/game/<id>/USRDIR (game_title.h), then through the vfs link, then beside
     * the ELF. Not the ELF's folder first: a game folder with only GOWA.SELF has it decrypted into the port's own
     * folder (launcher.cpp, game_refresh), and paths.elf names that copy, beside no archive (issue #9's Kit). */
    const char* hdd0 = ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL);
    const char* vfs = ps3_settings_str("paths", "vfs", "PS3_VFS_ROOT", NULL);
    const char* elf = ps3_settings_str("paths", "elf", NULL, NULL);
    std::vector<std::string> dirs;
    if (hdd0 && *hdd0) dirs.push_back(std::string(hdd0) + "\\game\\" + game_title_of_vfs(vfs)->id + "\\USRDIR");
    if (vfs && *vfs) dirs.push_back(std::string(vfs) + "\\PS3_GAME\\USRDIR");
    if (elf && *elf) {
        const std::string e(elf);
        const size_t slash = e.find_last_of("/\\");
        dirs.push_back(slash == std::string::npos ? std::string(".") : e.substr(0, slash));
    }
    if (dirs.empty()) {
        printf("%s: error the game's folder is not set\n", tag);
        return false;
    }
    /* Every PSARC of those folders, one of each name, from the first folder that has it: a disc's base archives are
     * on the disc and its update's TPPS ones in the update's folder; for an HDD game the two are one folder. */
    {
        std::vector<std::pair<std::string, std::string>> all;
        std::unordered_set<std::string> seen;
        for (const std::string& d : dirs) {
            WIN32_FIND_DATAA fd;
            HANDLE fh = FindFirstFileA((d + "\\*.PSARC").c_str(), &fd);
            if (fh == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string key(fd.cFileName);
                for (char& c : key) c = (char)toupper((unsigned char)c);
                if (seen.insert(key).second) all.push_back({ fd.cFileName, d });
            } while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
        std::sort(all.begin(), all.end());
        for (const auto& x : all) {
            arcs.push_back(x.first);
            arc_dir.push_back(x.second);
        }
    }
    if (arcs.empty()) {
        std::string where;
        for (const std::string& d : dirs) where += (where.empty() ? "" : ", ") + d;
        printf("%s: error no PSARC archives in %s\n", tag, where.c_str());
        return false;
    }
    *arc_bytes = 0;
    for (size_t a = 0; a < arcs.size(); a++) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA((arc_dir[a] + "\\" + arcs[a]).c_str(), GetFileExInfoStandard, &fa))
            *arc_bytes += (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    }
    return true;
}

/* Each WAD and AS entry of the archives, unpacked, archive by archive and each in the order its entries lie (the
 * game's folder may be on a hard disk), to scan. Says "<tag>: read <n> <of> <name>" at each archive. */
template <typename Scan>
static void read_archives(const char* tag, const std::vector<std::string>& arcs, const std::vector<std::string>& arc_dir,
                          Scan scan)
{
    std::vector<uint8_t> buf, span;
    for (size_t a = 0; a < arcs.size(); a++) {
        printf("%s: read %u %u %s\n", tag, (unsigned)a + 1, (unsigned)arcs.size(), arcs[a].c_str());
        HANDLE f = CreateFileA((arc_dir[a] + "\\" + arcs[a]).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::vector<PsarcEntry> ents;
        std::vector<uint32_t> zs;
        uint32_t block = 0;
        if (psarc_toc(f, ents, zs, &block) && psarc_read(f, ents[0], zs, block, buf, span)) {
            size_t s = 0;
            for (size_t k = 1; k < ents.size(); k++) {   /* the manifest: one name a line */
                size_t e = s;
                while (e < buf.size() && buf[e] != '\n') e++;
                if (s < buf.size()) ents[k].name.assign((const char*)buf.data() + s, e - s);
                s = e + 1;
            }
            std::vector<const PsarcEntry*> want;
            for (size_t k = 1; k < ents.size(); k++) {
                const std::string& nm = ents[k].name;
                const size_t l = nm.size();
                if ((l > 4 && !_stricmp(nm.c_str() + l - 4, ".WAD")) || (l > 3 && !_stricmp(nm.c_str() + l - 3, ".AS")))
                    want.push_back(&ents[k]);
            }
            std::sort(want.begin(), want.end(), [](const PsarcEntry* x, const PsarcEntry* y) { return x->off < y->off; });
            for (const PsarcEntry* e : want)
                if (psarc_read(f, *e, zs, block, buf, span)) scan(buf);
        }
        CloseHandle(f);
    }
}

extern "C" int gowa_precompile(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Beside the player, who may be doing something else meanwhile; from the gate before the game
     * (loading_screen.cpp) the player is waiting for it. */
    if (!getenv("GOWA_PRECOMPILE_FOREGROUND")) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const DWORD t0 = GetTickCount();
    std::vector<std::string> arcs, arc_dir;   /* the names, sorted, and the folder of each */
    uint64_t arc_bytes = 0;
    if (!game_archives("precompile", arcs, arc_dir, &arc_bytes)) return 1;
    char why[256] = "";
    if (rsx_d3d12_precompile_open(why, sizeof why) != 0) {   /* before 7 GB are read for nothing */
        printf("precompile: error %s\n", why);
        return 1;
    }
    /* the vertex pass meanwhile: the archives' reading and the programs take longer than its one compile */
    static VertexPassJob vp;
    vp.text = vertex_pass_text(vp.why, sizeof vp.why);
    HANDLE vp_thread = vp.text ? CreateThread(NULL, 0, vertex_pass_worker, &vp, 0, NULL) : NULL;
    if (!vp_thread) {
        if (vp.text) snprintf(vp.why, sizeof vp.why, "no thread for it");
        vp.rc = -1;
        InterlockedExchange(&s_vp_done, 1);
    }
    struct VpJoin {   /* every way out waits for it: it writes the cache */
        HANDLE h;
        ~VpJoin() { if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); } }
    } vp_join = { vp_thread };
    const std::string sc = cache_file();
    std::unordered_set<std::string> found;
    const bool listed = !sc.empty() && vp_list_read(sc + ".vprogs", (uint32_t)arcs.size(), arc_bytes, found);
    if (!listed) {
        /* The fragment programs in the same reading, for the pixel shaders' build (gowa_prebuild): it then reads no
         * archive. */
        std::unordered_map<std::string, uint32_t> fps;
        read_archives("precompile", arcs, arc_dir, [&](const std::vector<uint8_t>& d) {
            scan_vps(d, found);
            scan_fps(d, fps);
        });
        if (!sc.empty() && !found.empty()) vp_list_write(sc + ".vprogs", (uint32_t)arcs.size(), arc_bytes, found);
        if (!sc.empty() && !fps.empty()) fp_list_write(sc + ".fprogs", (uint32_t)arcs.size(), arc_bytes, fps);
    }
    printf("precompile: found %u\n", (unsigned)found.size());
    std::vector<const uint8_t*> ptr;
    std::vector<uint32_t> len;
    for (const std::string& s : found) {
        ptr.push_back((const uint8_t*)s.data());
        len.push_back((uint32_t)s.size());
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int threads = std::max(1, std::min(16, (int)si.dwNumberOfProcessors - 1));
    uint32_t stats[3] = {};
    if (rsx_d3d12_precompile_vertex_programs(ptr.data(), len.data(), (uint32_t)ptr.size(), threads, compile_progress,
                                             stats, why, sizeof why) != 0) {
        printf("precompile: error %s\n", why);
        return 1;
    }
    /* the vertex pass, if it is still being compiled: it lands in the cache's journal after the pack, which the
     * game's start reads as well */
    while (vp_join.h && WaitForSingleObject(vp_join.h, 250) == WAIT_TIMEOUT) compile_progress(s_pc_done, s_pc_total);
    if (vp.rc == 0) {
        stats[vp.compiled ? 0 : 1]++;
        printf("precompile: vertex pass %s in %.1f s\n", vp.compiled ? "compiled" : "already built", vp.seconds);
    } else {
        /* not a failure of the precompile: the game compiles it at its first lit draw, as before */
        printf("precompile: vertex pass left to the game: %s\n", vp.why);
    }
    compile_progress(s_pc_done, s_pc_total);
    if (!sc.empty()) marker_write(sc, stats);
    printf("precompile: done %u %u %u %.0f\n", stats[0], stats[1], stats[2], (GetTickCount() - t0) / 1000.0);
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * The pixel shaders ahead of play (graphics.shader_prebuild): Ascension-R.exe --prebuild-pixel-shaders
 * [--settings <file>], as the launcher's "Build all shaders now" runs it (launcher.cpp, pb_start), on every thread but
 * one. The fragment programs of the archives (<cache>.fprogs, from the vertex programs' reading or read here) into
 * <cache>.dxil.prebuilt, each in the text a draw of it makes with no alpha test, sign expansion or depth comparison
 * (rsx_d3d12_prebuild_fragment_programs), those it has passed over. The game reads the file when its store has not a
 * source. <cache>.dxil.prebuilt.done says the version it was finished for. Not while the game plays: built beside it,
 * on six cores, it took 7% of the frames and would have taken hours (docs/notes/2026-11-18-pixel-shader-prebuild-step2.md).
 *
 * Progress, one line each on stdout:
 *   prebuild: read <archive n> <of> <name>
 *   prebuild: found <programs>
 *   prebuild: compile <done> <of>
 *   prebuild: done <compiled> <had> <failed> <seconds>
 *   prebuild: error <why>
 * ------------------------------------------------------------------------------------------------------------- */
static void prebuild_progress(uint32_t done, uint32_t total)
{
    static DWORD last;
    static uint32_t last_done = UINT32_MAX;
    const DWORD now = GetTickCount();
    if (done < total && (now - last < 500 || done == last_done)) return;   /* a line when it moves */
    last = now;
    last_done = done;
    printf("prebuild: compile %u %u\n", done, total);
}

extern "C" int gowa_prebuild(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!getenv("GOWA_PRECOMPILE_FOREGROUND")) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const DWORD t0 = GetTickCount();
    const std::string sc = cache_file();
    if (sc.empty()) {
        printf("prebuild: error no shader cache in the settings (graphics.shader_cache)\n");
        return 1;
    }
    std::vector<std::string> arcs, arc_dir;
    uint64_t arc_bytes = 0;
    if (!game_archives("prebuild", arcs, arc_dir, &arc_bytes)) return 1;
    std::unordered_map<std::string, uint32_t> found;
    if (!fp_list_read(sc + ".fprogs", (uint32_t)arcs.size(), arc_bytes, found)) {
        read_archives("prebuild", arcs, arc_dir, [&](const std::vector<uint8_t>& d) { scan_fps(d, found); });
        if (!found.empty()) fp_list_write(sc + ".fprogs", (uint32_t)arcs.size(), arc_bytes, found);
    }
    printf("prebuild: found %u\n", (unsigned)found.size());
    std::vector<const uint8_t*> ptr;
    std::vector<uint32_t> len, ctrl;
    for (const auto& p : found) {
        ptr.push_back((const uint8_t*)p.first.data());
        len.push_back((uint32_t)p.first.size());
        ctrl.push_back(p.second);
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int lp = (int)si.dwNumberOfProcessors;
    const int threads = std::max(1, std::min(16, lp - 1));
    uint32_t stats[3] = {};
    char why[320] = "";
    if (rsx_d3d12_prebuild_fragment_programs(ptr.data(), len.data(), ctrl.data(), (uint32_t)ptr.size(), threads,
                                             0, prebuild_progress, stats, why, sizeof why) != 0) {
        printf("prebuild: error %s\n", why);
        return 1;
    }
    FILE* f = fopen((sc + ".dxil.prebuilt.done").c_str(), "wb");
    if (f) {
        fprintf(f, "Ascension-R %s\n%u found, %u compiled, %u had, %u failed\n", AR_VERSION, (unsigned)ptr.size(),
                stats[0], stats[1], stats[2]);
        fclose(f);
    }
    printf("prebuild: done %u %u %u %.0f\n", stats[0], stats[1], stats[2], (GetTickCount() - t0) / 1000.0);
    return 0;
}

#endif /* _WIN32 */
