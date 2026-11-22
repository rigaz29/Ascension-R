/*
 * Ascension-R -- a portable folder's first start takes the files of %LOCALAPPDATA%\Ascension-R along
 *
 * portable.txt beside Ascension-R.exe keeps the settings, the saves, the shader caches and what the launcher made in
 * that folder (default_settings_path in main.cpp). Put beside a copy that had run without it, the folder starts with
 * none of them: the game is found, decrypted and built again, the shaders too, and the saves are not there (issue
 * #15: a player who did so saw the Kit's build and the shader precompile come back, every shader built anew in play,
 * and a new game). So on that folder's first start, if %LOCALAPPDATA%\Ascension-R has a settings file, the player is
 * asked whether to copy its files here, before the settings are read.
 *
 * Copied: the settings (the paths in them that named the old folder moved to this one), the saves and their backups,
 * the decrypted GOWA.elf, the game's built code, the shader caches, the screenshots, what the game wrote under its
 * hdd0. Left out: the logs, and the game code build's work folder, which only a stopped build goes on from. A link to a
 * folder (vfs\PS3_GAME and hdd0\game\<id>, to the game's folders) is made again to the same folder, never followed. The
 * old folder is only read, and stays as it was.
 */
#ifdef _WIN32   /* Windows only for now: port/posix_stubs.cpp stands in for it elsewhere (docs/MACOS.md) */
#include "portable_copy.h"
#include "game_find.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace {

struct Plan {
    unsigned long long bytes = 0;
    unsigned files = 0, failed = 0;
    std::vector<std::pair<std::wstring, std::wstring>> links;   /* relative path, the folder it leads to */
    std::wstring first_failed;
};

int has(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

/* What is left out, by its path under the folder. */
int left_out(const std::wstring& rel)
{
    return !_wcsicmp(rel.c_str(), L"logs") || !_wcsicmp(rel.c_str(), L"game-code\\work") ||
           !_wcsicmp(rel.c_str(), L"decrypt");
}

void fail(Plan& p, const std::wstring& what)
{
    if (!p.failed++) p.first_failed = what;
}

/* The folder's files under rel, counted, and copied when copy is set. */
void walk(const std::wstring& from, const std::wstring& to, const std::wstring& rel, Plan& p, int copy)
{
    const std::wstring dir = rel.empty() ? from : from + L"\\" + rel;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const std::wstring r = rel.empty() ? std::wstring(fd.cFileName) : rel + L"\\" + fd.cFileName;
        const std::wstring src = from + L"\\" + r, dst = to + L"\\" + r;
        if (left_out(r)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            wchar_t target[GAME_PATH_MAX];
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && game_link_target(src.c_str(), target, GAME_PATH_MAX))
                p.links.push_back(std::make_pair(r, std::wstring(target)));
            continue;   /* a link to a file, or one not read: neither followed nor copied */
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (copy && !CreateDirectoryW(dst.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                fail(p, dst);
                continue;
            }
            walk(from, to, r, p, copy);
            continue;
        }
        p.files++;
        p.bytes += (unsigned long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        if (copy && !CopyFileW(src.c_str(), dst.c_str(), TRUE)) fail(p, dst);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int k = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)(k > 0 ? k : 0), '\0');
    if (k > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], k, NULL, NULL);
    return s;
}

/* Every place text names old, in either slash and any case, made to name now. */
void replace_path(std::string& text, std::string old, std::string now)
{
    for (int slash = 0; slash < 2; slash++) {
        if (slash) {
            for (char& c : old) if (c == '\\') c = '/';
            for (char& c : now) if (c == '\\') c = '/';
        }
        for (size_t at = 0; at + old.size() <= text.size();) {
            if (!_strnicmp(text.c_str() + at, old.c_str(), old.size())) {
                text.replace(at, old.size(), now);
                at += now.size();
            } else {
                at++;
            }
        }
    }
}

int rewrite_settings(const std::wstring& file, const std::wstring& from, const std::wstring& to)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    std::string text((size_t)(size.QuadPart < (1 << 20) ? size.QuadPart : 0), '\0');
    DWORD got = 0;
    const BOOL ok = text.empty() || ReadFile(h, &text[0], (DWORD)text.size(), &got, NULL);
    CloseHandle(h);
    if (!ok) return 0;
    text.resize(got);
    replace_path(text, utf8(from), utf8(to));
    h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    const BOOL wrote = WriteFile(h, text.data(), (DWORD)text.size(), &put, NULL) && put == text.size();
    CloseHandle(h);
    return wrote;
}

void make_parents(const std::wstring& path)
{
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1))
        CreateDirectoryW(path.substr(0, at).c_str(), NULL);
}

std::wstring trimmed(std::wstring p)
{
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

}  // namespace

int portable_copy_from(const wchar_t* from_dir, const wchar_t* to_dir, char* note, size_t n)
{
    const std::wstring from = trimmed(from_dir), to = trimmed(to_dir);
    Plan p;
    walk(from, to, L"", p, 1);
    if (has(to + L"\\ascension-r.ini") && !rewrite_settings(to + L"\\ascension-r.ini", from, to))
        fail(p, to + L"\\ascension-r.ini");
    unsigned linked = 0;
    for (const auto& l : p.links) {
        const std::wstring link = to + L"\\" + l.first;
        make_parents(link);
        wchar_t err[512] = L"";
        if (game_link(link.c_str(), l.second.c_str(), err, 512)) linked++;
        else fail(p, link);
    }
    if (note && n) {
        if (p.failed)
            snprintf(note, n, "[portable] from %s: %u files, %.1f MB, and %u links to the game's folders; %u could not "
                              "be copied or linked, the first %s", utf8(from).c_str(), p.files, p.bytes / 1048576.0,
                     (unsigned)p.links.size(), p.failed, utf8(p.first_failed).c_str());
        else
            snprintf(note, n, "[portable] copied from %s: %u files, %.1f MB, and %u links to the game's folders made "
                              "again", utf8(from).c_str(), p.files, p.bytes / 1048576.0, linked);
    }
    return (int)p.failed;
}

void portable_offer(const char* settings, char* note, size_t n)
{
    if (note && n) note[0] = 0;
    wchar_t w[1100];
    if (!MultiByteToWideChar(CP_ACP, 0, settings, -1, w, 1100)) return;
    std::wstring dir(w);
    const size_t cut = dir.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return;
    dir.resize(cut);
    /* A folder portable.txt marks, with none of the program's files in it yet. */
    if (!has(dir + L"\\portable.txt") || has(dir + L"\\ascension-r.ini") || has(dir + L"\\gamedata") ||
        has(dir + L"\\game-code") || has(dir + L"\\GOWA.elf"))
        return;
    wchar_t local[1024];
    const DWORD k = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1024);
    if (!k || k >= 1024) return;
    const std::wstring from = std::wstring(local) + L"\\Ascension-R";
    if (!has(from + L"\\ascension-r.ini") || !_wcsicmp(trimmed(from).c_str(), trimmed(dir).c_str())) return;
    Plan plan;
    walk(from, dir, L"", plan, 0);
    wchar_t ask[2400];
    swprintf(ask, sizeof ask / sizeof ask[0],
             L"This copy of Ascension-R keeps its files in its own folder (portable.txt), and that folder has none "
             L"yet.\n\nYour settings, saves, decrypted game, built game code and shaders are in\n%ls\n\nCopy them here "
             L"(%.0f MB)? That folder stays as it is.\n\nNo: this folder starts empty. The game is found, decrypted "
             L"and built again, and your saves stay in the other folder.",
             from.c_str(), plan.bytes / 1048576.0 + 0.5);
    if (MessageBoxW(NULL, ask, L"Ascension-R", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) != IDYES) {
        if (note && n) snprintf(note, n, "[portable] a new portable folder: the files of %s not copied (the "
                                         "player's choice)", utf8(from).c_str());
        return;
    }
    char done[1500];
    if (portable_copy_from(from.c_str(), dir.c_str(), done, sizeof done)) {
        wchar_t warn[1700];
        swprintf(warn, sizeof warn / sizeof warn[0], L"Not everything could be copied:\n\n%hs\n\nThe rest is here. "
                 L"What is missing is made again when it is needed.", done);
        MessageBoxW(NULL, warn, L"Ascension-R", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }
    if (note && n) snprintf(note, n, "%s", done);
}

#endif /* _WIN32 */
