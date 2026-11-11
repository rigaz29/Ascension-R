/*
 * Precompile: the game's vertex programs, from its archives, into the shader cache before it runs.
 * Ascension-R.exe --precompile-shaders [--settings <file>]; the launcher's Precompile shaders runs it.
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
 * Beside the cache it leaves <cache>.vprogs, the programs found (read instead of the archives while their count and
 * bytes are the same: a run after an update takes seconds), and when the build is done <cache>.precompiled, which
 * says the version it was built for: the game's start does it first while that is missing or another version's
 * (loading_screen.cpp). GOWA_PRECOMPILE_FOREGROUND keeps the process at normal priority, for that start.
 *
 * Progress, one line each on stdout, for the launcher:
 *   precompile: read <archive n> <of> <name>
 *   precompile: found <programs>
 *   precompile: compile <done> <of>
 *   precompile: done <compiled> <had> <failed> <seconds>
 *   precompile: error <why>
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include "third_party/zlib/zlib.h"
#include "ps3emu/ps3_settings.h"
#include "rsx_d3d12_backend.h"
#include "version.h"

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

/* <cache>.precompiled: built for this version (loading_screen.cpp's gate reads the first line). */
static void marker_write(const std::string& cache, const uint32_t stats[3])
{
    FILE* f = fopen((cache + ".precompiled").c_str(), "wb");
    if (!f) return;
    fprintf(f, "Ascension-R %s\n%u compiled, %u had, %u failed\n", AR_VERSION, stats[0], stats[1], stats[2]);
    fclose(f);
}

static void compile_progress(uint32_t done, uint32_t total)
{
    static DWORD last;
    const DWORD now = GetTickCount();
    if (done < total && now - last < 250) return;
    last = now;
    printf("precompile: compile %u %u\n", done, total);
}

extern "C" int gowa_precompile(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Beside the player, who may be doing something else meanwhile; from the gate before the game
     * (loading_screen.cpp) the player is waiting for it. */
    if (!getenv("GOWA_PRECOMPILE_FOREGROUND")) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const DWORD t0 = GetTickCount();
    const char* elf = ps3_settings_str("paths", "elf", NULL, NULL);
    if (!elf || !*elf) {
        printf("precompile: error the game's folder is not set\n");
        return 1;
    }
    std::string dir(elf);
    const size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
    std::vector<std::string> arcs;
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA((dir + "\\*.PSARC").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) arcs.push_back(fd.cFileName);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    std::sort(arcs.begin(), arcs.end());
    if (arcs.empty()) {
        printf("precompile: error no PSARC archives in %s\n", dir.c_str());
        return 1;
    }
    char why[256] = "";
    if (rsx_d3d12_precompile_open(why, sizeof why) != 0) {   /* before 7 GB are read for nothing */
        printf("precompile: error %s\n", why);
        return 1;
    }
    const std::string sc = cache_file();
    uint64_t arc_bytes = 0;
    for (const std::string& a : arcs) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA((dir + "\\" + a).c_str(), GetFileExInfoStandard, &fa))
            arc_bytes += (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    }
    std::unordered_set<std::string> found;
    const bool listed = !sc.empty() && vp_list_read(sc + ".vprogs", (uint32_t)arcs.size(), arc_bytes, found);
    std::vector<uint8_t> buf, span;
    for (size_t a = 0; !listed && a < arcs.size(); a++) {
        printf("precompile: read %u %u %s\n", (unsigned)a + 1, (unsigned)arcs.size(), arcs[a].c_str());
        HANDLE f = CreateFileA((dir + "\\" + arcs[a]).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
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
                if (psarc_read(f, *e, zs, block, buf, span)) scan_vps(buf, found);
        }
        CloseHandle(f);
    }
    if (!listed && !sc.empty() && !found.empty()) vp_list_write(sc + ".vprogs", (uint32_t)arcs.size(), arc_bytes, found);
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
    if (!sc.empty()) marker_write(sc, stats);
    printf("precompile: done %u %u %u %.0f\n", stats[0], stats[1], stats[2], (GetTickCount() - t0) / 1000.0);
    return 0;
}
