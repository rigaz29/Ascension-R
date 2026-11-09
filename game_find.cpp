/*
 * gowa-recomp -- finding the game on this PC
 *
 * The game's folder reaches the port through two directory junctions, <vfs>\PS3_GAME and
 * <hdd0>\game\NPUA80918, as `gowa.py setup` makes them. The launcher reads the first to show where
 * the game is, and sets both when the player picks another folder or Auto detect finds one.
 *
 * What a folder holds comes from its PARAM.SFO and its GOWA.elf: the port is lifted from one
 * release, so another version or another GOWA.elf is told apart (scripts/gowa.py has the same three
 * values, for `doctor`). The search only reads. No game file is copied or changed.
 */
#include "game_find.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <string>
#pragma comment(lib, "bcrypt.lib")

static const char k_title_id[] = "NPUA80918";
static const char k_app_ver[] = "01.12";
static const char k_elf_sha256[] = "4bf75c897b82d1beb25514cbadaff3d03e1b0c7bf849fa5d289be4f9183462b5";
static const wchar_t k_title_dir[] = L"NPUA80918";

static uint32_t rd16(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const unsigned char* p) { return rd16(p) | rd16(p + 2) << 16; }

static void copy_value(char* out, size_t n, const unsigned char* v, uint32_t len)
{
    size_t k = len < n - 1 ? len : n - 1;
    memcpy(out, v, k);
    out[k] = 0;
}

/* TITLE_ID, APP_VER and TITLE out of a PARAM.SFO. Everything is checked against the file's size: a
 * search reads whatever is called PARAM.SFO. */
static int sfo_read(const wchar_t* path, GameInfo* g)
{
    static const size_t CAP = 65536;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned char* d = (unsigned char*)malloc(CAP);
    DWORD n = 0;
    const BOOL ok = d && ReadFile(h, d, (DWORD)CAP, &n, NULL);
    CloseHandle(h);
    int found = 0;
    if (ok && n >= 20 && !memcmp(d, "\0PSF", 4)) {
        const uint32_t keys = rd32(d + 8), data = rd32(d + 12), count = rd32(d + 16);
        if (keys < n && data < n && count <= 256 && 20 + count * 16 <= n) {
            for (uint32_t i = 0; i < count; i++) {
                const unsigned char* e = d + 20 + i * 16;
                const uint32_t ko = keys + rd16(e), len = rd32(e + 4), vo = data + rd32(e + 12);
                if (ko >= n || vo >= n || len > n - vo || !memchr(d + ko, 0, n - ko)) continue;
                const char* key = (const char*)d + ko;
                if (!strcmp(key, "TITLE_ID")) { copy_value(g->title_id, sizeof g->title_id, d + vo, len); found = 1; }
                else if (!strcmp(key, "APP_VER")) copy_value(g->app_ver, sizeof g->app_ver, d + vo, len);
                else if (!strcmp(key, "TITLE")) copy_value(g->title, sizeof g->title, d + vo, len);
            }
        }
    }
    free(d);
    return found;
}

/* Whether a file's SHA-256 is k_elf_sha256. */
static int elf_is_ours(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    int same = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        static const DWORD CHUNK = 1u << 20;
        unsigned char* b = (unsigned char*)malloc(CHUNK);
        DWORD n = 0;
        int ok = b != NULL;
        while (ok && ReadFile(f, b, CHUNK, &n, NULL) && n) ok = BCryptHashData(hash, b, n, 0) == 0;
        free(b);
        unsigned char sum[32];
        if (ok && BCryptFinishHash(hash, sum, sizeof sum, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", sum[i]);
            same = !strcmp(hex, k_elf_sha256);
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return same;
}

static int is_file(const wchar_t* path)
{
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* A folder's path without the backslash at its end, a drive's root keeping it. */
static void folder_copy(wchar_t* out, size_t n, const wchar_t* in)
{
    swprintf(out, n, L"%ls", in);
    size_t k = wcslen(out);
    while (k > 3 && (out[k - 1] == L'\\' || out[k - 1] == L'/')) out[--k] = 0;
}

GameState game_probe(const wchar_t* dir, GameInfo* g)
{
    memset(g, 0, sizeof *g);
    folder_copy(g->dir, GAME_PATH_MAX, dir);
    wchar_t p[GAME_PATH_MAX + 32];
    swprintf(p, sizeof p / sizeof p[0], L"%ls\\PARAM.SFO", g->dir);
    if (!sfo_read(p, g)) return g->state = GAME_NONE;
    if (strcmp(g->title_id, k_title_id)) return g->state = GAME_OTHER_TITLE;
    swprintf(p, sizeof p / sizeof p[0], L"%ls\\USRDIR\\GOWA.elf", g->dir);
    if (!is_file(p)) return g->state = GAME_NO_ELF;
    return g->state = strcmp(g->app_ver, k_app_ver) || !elf_is_ours(p) ? GAME_OTHER_BUILD : GAME_OK;
}

int game_read_info(const wchar_t* dir, GameInfo* g)
{
    memset(g, 0, sizeof *g);
    folder_copy(g->dir, GAME_PATH_MAX, dir);
    wchar_t p[GAME_PATH_MAX + 32];
    swprintf(p, sizeof p / sizeof p[0], L"%ls\\PARAM.SFO", g->dir);
    return sfo_read(p, g);
}

GameState game_locate(const wchar_t* path, GameInfo* g)
{
    wchar_t base[GAME_PATH_MAX];
    folder_copy(base, GAME_PATH_MAX, path);
    /* USRDIR, or anything in it picked by mistake: the folder above. */
    const wchar_t* end = wcsrchr(base, L'\\');
    if (end && !_wcsicmp(end + 1, L"USRDIR")) base[end - base] = 0;
    static const wchar_t* const under[] = {
        L"", L"\\PS3_GAME", L"\\NPUA80918", L"\\game\\NPUA80918", L"\\dev_hdd0\\game\\NPUA80918",
    };
    memset(g, 0, sizeof *g);
    for (size_t i = 0; i < sizeof under / sizeof under[0]; i++) {
        wchar_t p[GAME_PATH_MAX + 32];
        swprintf(p, sizeof p / sizeof p[0], L"%ls%ls", base, under[i]);
        GameInfo t;
        if (game_probe(p, &t) > g->state) *g = t;
        if (g->state == GAME_OK) break;
    }
    return g->state;
}

/* --- the search ------------------------------------------------------------- */
static int has_word(const wchar_t* name, const wchar_t* word)
{
    wchar_t low[260];
    size_t i = 0;
    for (; name[i] && i < 259; i++) low[i] = towlower(name[i]);
    low[i] = 0;
    return wcsstr(low, word) != NULL;
}

/* Folders worth looking in before the rest. */
static int sounds_like_games(const wchar_t* name)
{
    static const wchar_t* const words[] = {
        L"rpcs3", L"ps3", L"playstation", L"dev_hdd0", L"game", L"emul", L"npua", L"god of war", L"gow", L"rom",
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (has_word(name, words[i])) return 1;
    return 0;
}

/* Folders no game is in, and that take long to walk. */
static int not_worth_it(const wchar_t* name)
{
    static const wchar_t* const names[] = {
        L"Windows", L"Windows.old", L"$Recycle.Bin", L"System Volume Information", L"Recovery", L"PerfLogs",
        L"ProgramData", L"AppData", L"$WinREAgent", L"Config.Msi", L"MSOCache", L"node_modules", L".git", L".svn",
        L"__pycache__", L".venv", L"WindowsApps", L"WinSxS",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!_wcsicmp(name, names[i])) return 1;
    return 0;
}

struct Dir { std::wstring path; int depth; };

static void keep_best(GameSearch* s, GameInfo* best, const GameInfo& g)
{
    /* Another title counts only if it is this game: another release of it. */
    if (g.state == GAME_OTHER_TITLE && !strstr(g.title, "Ascension")) return;
    if (g.state >= GAME_OTHER_TITLE) s->copies++;
    if (g.state > best->state) *best = g;
}

GameState game_search(GameSearch* s, GameInfo* best)
{
    memset(best, 0, sizeof *best);
    s->copies = 0;
    const ULONGLONG until = GetTickCount64() + (ULONGLONG)(s->seconds > 0 ? s->seconds : 60) * 1000u;
    std::deque<Dir> hot, cold;
    if (s->under) {
        hot.push_back({ s->under, 0 });
    } else {
        /* The places RPCS3 keeps its files when it is installed for one user. */
        static const wchar_t* const vars[] = { L"LOCALAPPDATA", L"APPDATA", L"USERPROFILE" };
        for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++) {
            wchar_t v[GAME_PATH_MAX], p[GAME_PATH_MAX + 16];
            if (!GetEnvironmentVariableW(vars[i], v, GAME_PATH_MAX)) continue;
            swprintf(p, sizeof p / sizeof p[0], L"%ls\\rpcs3", v);
            GameInfo g;
            if (game_locate(p, &g) > GAME_NONE) keep_best(s, best, g);
        }
        /* Every disk, the system's last: games are mostly on another one. */
        wchar_t sys[MAX_PATH] = L"C";
        GetSystemWindowsDirectoryW(sys, MAX_PATH);
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (!(drives & (1u << i))) continue;
            wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
            const UINT type = GetDriveTypeW(root);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
            if (towupper(root[0]) == towupper(sys[0])) cold.push_back({ root, 0 });
            else cold.push_front({ root, 0 });
        }
    }
    const int max_depth = s->under ? 12 : 9;
    while (best->state != GAME_OK && (!hot.empty() || !cold.empty())) {
        if ((s->stop && *s->stop) || GetTickCount64() >= until) break;
        Dir d;
        if (!hot.empty()) { d = std::move(hot.back()); hot.pop_back(); }
        else { d = std::move(cold.front()); cold.pop_front(); }
        if (s->now) s->now(d.path.c_str());
        std::wstring pat = d.path;
        if (pat.empty() || pat.back() != L'\\') pat += L'\\';
        const std::wstring prefix = pat;
        pat += L'*';
        if (pat.size() >= GAME_PATH_MAX - 40) continue;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::deque<Dir> found_hot, found_cold;
        int game_here = 0;
        do {
            const wchar_t* n = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (!_wcsicmp(n, L"PARAM.SFO")) game_here = 1;
                continue;
            }
            if (!wcscmp(n, L".") || !wcscmp(n, L"..")) continue;
            /* A link leads to a folder found on its own way, or round in a circle. */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (d.depth >= max_depth || not_worth_it(n)) continue;
            if (sounds_like_games(n) || !_wcsicmp(n, k_title_dir) || !_wcsicmp(n, L"PS3_GAME"))
                found_hot.push_back({ prefix + n, d.depth + 1 });
            else
                found_cold.push_back({ prefix + n, d.depth + 1 });
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        if (game_here) {
            GameInfo g;
            if (game_probe(d.path.c_str(), &g) > GAME_NONE) keep_best(s, best, g);
            continue;   /* nothing of interest under a game */
        }
        for (Dir& x : found_hot) hot.push_back(std::move(x));
        for (Dir& x : found_cold) cold.push_back(std::move(x));
    }
    return best->state;
}

/* --- links ------------------------------------------------------------------- */
/* What FSCTL_GET_REPARSE_POINT and FSCTL_SET_REPARSE_POINT take: REPARSE_DATA_BUFFER of the
 * driver kit, which the SDK's headers leave out. */
struct Reparse {
    ULONG tag;
    USHORT data_len, reserved;
    USHORT sub_off, sub_len, print_off, print_len;
    union {
        struct { ULONG flags; WCHAR link_path[1]; };   /* IO_REPARSE_TAG_SYMLINK */
        WCHAR mount_path[1];                           /* IO_REPARSE_TAG_MOUNT_POINT */
    };
};
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif
#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif
enum { REPARSE_MAX = 16 * 1024 };

int game_link_target(const wchar_t* link, wchar_t* out, size_t n)
{
    if (n) out[0] = 0;
    const DWORD a = GetFileAttributesW(link);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    HANDLE h = CreateFileW(link, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    alignas(8) unsigned char buf[REPARSE_MAX];
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof buf, &got, NULL);
    CloseHandle(h);
    if (!ok) return 0;
    const Reparse* r = (const Reparse*)buf;
    const WCHAR* base = r->tag == IO_REPARSE_TAG_MOUNT_POINT ? r->mount_path :
                        r->tag == IO_REPARSE_TAG_SYMLINK ? r->link_path : NULL;
    if (!base) return 0;
    /* The name to print, else the one the system uses: \??\D:\Games */
    const WCHAR* s = r->print_len ? base + r->print_off / 2 : base + r->sub_off / 2;
    size_t len = (r->print_len ? r->print_len : r->sub_len) / 2;
    if (len >= 4 && !wcsncmp(s, L"\\??\\", 4)) { s += 4; len -= 4; }
    if (len >= n) return 0;
    wmemcpy(out, s, len);
    out[len] = 0;
    return 1;
}

static int same_folder(const wchar_t* a, const wchar_t* b)
{
    wchar_t x[GAME_PATH_MAX], y[GAME_PATH_MAX];
    folder_copy(x, GAME_PATH_MAX, a);
    folder_copy(y, GAME_PATH_MAX, b);
    for (wchar_t* p = x; *p; p++) if (*p == L'/') *p = L'\\';
    for (wchar_t* p = y; *p; p++) if (*p == L'/') *p = L'\\';
    return !_wcsicmp(x, y);
}

static void say(wchar_t* err, size_t n, const wchar_t* what, const wchar_t* path)
{
    if (err && n) swprintf(err, n, L"%ls: %ls", what, path);
}

int game_link_possible(const wchar_t* link, const wchar_t* target, wchar_t* err, size_t n)
{
    if (same_folder(link, target)) return 1;
    const DWORD a = GetFileAttributesW(link);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return 1;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { say(err, n, L"A file is in the way", link); return 0; }
    wchar_t pat[GAME_PATH_MAX + 8];
    swprintf(pat, sizeof pat / sizeof pat[0], L"%ls\\*", link);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    int empty = 1;
    if (f != INVALID_HANDLE_VALUE) {
        do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = 0; } while (empty && FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (!empty) say(err, n, L"A real folder with files is in the way", link);
    return empty;
}

static void make_parents(const wchar_t* link)
{
    wchar_t p[GAME_PATH_MAX];
    folder_copy(p, GAME_PATH_MAX, link);
    for (wchar_t* q = p; *q; q++) {
        if (*q != L'\\' || q == p || q[-1] == L':') continue;
        *q = 0;
        CreateDirectoryW(p, NULL);
        *q = L'\\';
    }
}

static int make_junction(const wchar_t* link, const wchar_t* target, wchar_t* err, size_t n)
{
    wchar_t full[GAME_PATH_MAX];
    if (!GetFullPathNameW(target, GAME_PATH_MAX, full, NULL)) { say(err, n, L"Not a folder", target); return 0; }
    wchar_t clean[GAME_PATH_MAX];
    folder_copy(clean, GAME_PATH_MAX, full);
    if (!wcsncmp(clean, L"\\\\", 2)) { say(err, n, L"A link cannot lead to a network folder", clean); return 0; }
    make_parents(link);
    if (!CreateDirectoryW(link, NULL)) { say(err, n, L"Cannot make the link", link); return 0; }
    HANDLE h = CreateFileW(link, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link);
        say(err, n, L"Cannot make the link", link);
        return 0;
    }
    alignas(8) unsigned char buf[REPARSE_MAX];
    memset(buf, 0, sizeof buf);
    Reparse* r = (Reparse*)buf;
    const size_t len = wcslen(clean);
    const USHORT sub = (USHORT)((len + 4) * 2), print = (USHORT)(len * 2);
    r->tag = IO_REPARSE_TAG_MOUNT_POINT;
    r->sub_off = 0;
    r->sub_len = sub;
    r->print_off = (USHORT)(sub + 2);
    r->print_len = print;
    wmemcpy(r->mount_path, L"\\??\\", 4);
    wmemcpy(r->mount_path + 4, clean, len);
    wmemcpy(r->mount_path + len + 5, clean, len);
    r->data_len = (USHORT)(8 + sub + 2 + print + 2);
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf, 8u + r->data_len, NULL, 0, &got, NULL);
    CloseHandle(h);
    if (!ok) {
        RemoveDirectoryW(link);
        say(err, n, L"Cannot make the link", link);
    }
    return ok;
}

int game_link(const wchar_t* link, const wchar_t* target, wchar_t* err, size_t n)
{
    if (same_folder(link, target)) return 1;
    wchar_t now[GAME_PATH_MAX];
    if (game_link_target(link, now, GAME_PATH_MAX) && same_folder(now, target)) return 1;
    if (!game_link_possible(link, target, err, n)) return 0;
    /* A link, or an empty folder: RemoveDirectory takes the link itself, never what it leads to. */
    if (GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES && !RemoveDirectoryW(link)) {
        say(err, n, L"Cannot replace the link", link);
        return 0;
    }
    return make_junction(link, target, err, n);
}

#endif /* _WIN32 */
