/*
 * Ascension-R -- turning the player's GOWA.SELF into GOWA.elf
 *
 * The SELF is encrypted and this port holds no keys. RPCS3 decrypts it with the game's own license
 * file, run with --decrypt on a copy of each, with its configuration folder pointed at a folder of
 * ours. So neither the game's folder nor the player's RPCS3 is written to.
 *
 * RPCS3 ends with exit code 0 even when it decrypted nothing, so what counts is the file it leaves,
 * and that file's hash.
 */
#include "game_prepare.h"
#include "game_find.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <urlmon.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "urlmon.lib")

/* The name RPCS3 looks the license up by: the content id inside the SELF. */
static const wchar_t k_rap_name[] = L"UP9000-NPUA80918_00-GOWADIGITALVERS1.rap";
static const DWORD k_decrypt_ms = 120000, k_unpack_ms = 300000;

/* The version of RPCS3 this was tried with. */
static const wchar_t k_rpcs3_url[] =
    L"https://github.com/RPCS3/rpcs3-binaries-win/releases/download/build-47599599a4cea37f350eb0936d7c5e4de6f7bf57/"
    L"rpcs3-v0.0.43-20573-47599599_win64_msvc.7z";
static const char k_rpcs3_sha256[] = "4876c2921d7a36fee007a3e9bf8cd42902f6f7da0bc67cb9d576ca386a1e4433";

static int is_file(const wchar_t* path)
{
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void make_path(const wchar_t* dir)
{
    wchar_t p[GAME_PATH_MAX];
    swprintf(p, GAME_PATH_MAX, L"%ls", dir);
    for (wchar_t* s = wcschr(p + 3, L'\\'); s; s = wcschr(s + 1, L'\\')) {
        *s = 0;
        CreateDirectoryW(p, NULL);
        *s = L'\\';
    }
    CreateDirectoryW(p, NULL);
}

/* Only for the folders made here. A link inside is removed as a link. */
static void remove_tree(const wchar_t* dir)
{
    wchar_t pat[GAME_PATH_MAX + 8];
    swprintf(pat, sizeof pat / sizeof pat[0], L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[GAME_PATH_MAX + MAX_PATH];
            swprintf(p, sizeof p / sizeof p[0], L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(p);
                else remove_tree(p);
            } else {
                SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static int fail(wchar_t* err, size_t n, const wchar_t* why)
{
    if (err && n) swprintf(err, n, L"%ls", why);
    return 0;
}

int game_find_rpcs3(const wchar_t* game_dir, wchar_t* exe, size_t n)
{
    wchar_t p[GAME_PATH_MAX];
    swprintf(p, GAME_PATH_MAX, L"%ls", game_dir);
    for (int up = 0; up < 3; up++) {
        wchar_t* s = wcsrchr(p, L'\\');
        if (!s) return 0;
        *s = 0;
    }
    swprintf(exe, n, L"%ls\\rpcs3.exe", p);
    if (is_file(exe)) return 1;
    exe[0] = 0;
    return 0;
}

int game_find_rap(const wchar_t* rpcs3_exe, wchar_t* rap, size_t n)
{
    wchar_t root[GAME_PATH_MAX], pat[GAME_PATH_MAX + 32];
    swprintf(root, GAME_PATH_MAX, L"%ls", rpcs3_exe);
    wchar_t* s = wcsrchr(root, L'\\');
    if (!s) return 0;
    *s = 0;
    swprintf(pat, sizeof pat / sizeof pat[0], L"%ls\\dev_hdd0\\home\\*", root);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        swprintf(rap, n, L"%ls\\dev_hdd0\\home\\%ls\\exdata\\%ls", root, fd.cFileName, k_rap_name);
        found = is_file(rap);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    if (!found) rap[0] = 0;
    return found;
}

static int is_ascii(const wchar_t* s)
{
    for (; *s; s++) if (*s > 126) return 0;
    return 1;
}

/* RPCS3 crashes on a file named with anything but ASCII (a Windows account name can be). Its short
 * form is ASCII where the disk keeps short names. */
static void short_form(const wchar_t* path, wchar_t* out, size_t n)
{
    const DWORD got = GetShortPathNameW(path, out, (DWORD)n);
    if (!got || got >= n) swprintf(out, n, L"%ls", path);
}

/* The folder to work in: under data, or the shared one if its path cannot be made ASCII there. */
static void work_folder(const wchar_t* data, wchar_t* work, size_t n)
{
    swprintf(work, n, L"%ls\\decrypt", data);
    make_path(work);
    wchar_t s[GAME_PATH_MAX + 128];
    short_form(work, s, sizeof s / sizeof s[0]);
    if (is_ascii(s)) return;
    wchar_t shared[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", shared, MAX_PATH)) return;
    RemoveDirectoryW(work);
    swprintf(work, n, L"%ls\\Ascension-R\\decrypt", shared);
    make_path(work);
}

/* Runs a program with no window and waits for it. The exit code, or -1 if it did not start or end. */
static int run_hidden(const wchar_t* exe, wchar_t* cmd, const wchar_t* cwd, DWORD wait_ms)
{
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) return -1;
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int game_prepare(const wchar_t* game_dir, const wchar_t* rpcs3_exe, const wchar_t* rap, const wchar_t* data,
                 wchar_t* err, size_t n)
{
    enum { P = GAME_PATH_MAX + 128 };
    wchar_t work[P], input[P], config[P], exdata[P], from[P], self[P], to_rap[P], made[P], final_elf[P];
    swprintf(from, P, L"%ls\\USRDIR\\GOWA.SELF", game_dir);
    if (!is_file(from)) return fail(err, n, L"No USRDIR\\GOWA.SELF in the game's folder");
    if (!is_file(rpcs3_exe)) return fail(err, n, L"rpcs3.exe is not there");
    if (!is_file(rap)) return fail(err, n, L"The game's license file (.rap) is not there");
    work_folder(data, work, P);
    swprintf(input, P, L"%ls\\input", work);
    swprintf(config, P, L"%ls\\rpcs3\\", work);
    swprintf(exdata, P, L"%ls\\rpcs3\\dev_hdd0\\home\\00000001\\exdata", work);
    swprintf(self, P, L"%ls\\GOWA.SELF", input);
    swprintf(to_rap, P, L"%ls\\%ls", exdata, k_rap_name);
    swprintf(made, P, L"%ls\\GOWA.elf", input);
    swprintf(final_elf, P, L"%ls\\GOWA.elf", data);

    remove_tree(work);
    make_path(input);
    make_path(exdata);
    int ok = 0;
    if (!CopyFileW(from, self, FALSE)) fail(err, n, L"Could not copy GOWA.SELF");
    else if (!CopyFileW(rap, to_rap, FALSE)) fail(err, n, L"Could not copy the license file");
    else {
        /* The folders in their short form; the file keeps its name, which says what it is. */
        wchar_t in_short[P], cfg_short[P], exe_short[P], arg[P + 16];
        short_form(input, in_short, P);
        short_form(config, cfg_short, P);
        short_form(rpcs3_exe, exe_short, P);
        swprintf(arg, sizeof arg / sizeof arg[0], L"%ls\\GOWA.SELF", in_short);
        static wchar_t cmd[P * 2 + 32];
        swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" --decrypt \"%ls\"", exe_short, arg);
        SetEnvironmentVariableW(L"RPCS3_CONFIG_DIR", cfg_short);
        const int code = run_hidden(exe_short, cmd, cfg_short, k_decrypt_ms);
        SetEnvironmentVariableW(L"RPCS3_CONFIG_DIR", NULL);
        if (code == -1) fail(err, n, L"RPCS3 did not start or did not finish");
        else if (!is_file(made)) fail(err, n, L"RPCS3 could not decrypt the game. Is the .rap this game's?");
        else if (!game_elf_ok(made)) fail(err, n, L"The decrypted file is not version 1.12 of this game");
        else if (!MoveFileExW(made, final_elf, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) fail(err, n, L"Could not keep GOWA.elf");
        else ok = 1;
    }
    remove_tree(work);
    return ok;
}

int game_fetch_rpcs3(const wchar_t* data, wchar_t* exe, size_t n, wchar_t* err, size_t en)
{
    enum { P = GAME_PATH_MAX + 128 };
    wchar_t dir[P], file[P], tar[MAX_PATH];
    swprintf(dir, P, L"%ls\\rpcs3-download", data);
    swprintf(file, P, L"%ls\\rpcs3.7z", dir);
    remove_tree(dir);
    make_path(dir);
    if (FAILED(URLDownloadToFileW(NULL, k_rpcs3_url, file, 0, NULL))) {
        remove_tree(dir);
        return fail(err, en, L"Could not download RPCS3");
    }
    char sum[65];
    if (!game_file_sha256(file, sum) || strcmp(sum, k_rpcs3_sha256)) {
        remove_tree(dir);
        return fail(err, en, L"The download of RPCS3 is not the expected file");
    }
    GetSystemDirectoryW(tar, MAX_PATH);
    wcscat_s(tar, MAX_PATH, L"\\tar.exe");
    static wchar_t cmd[P * 2 + 64];
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" -xf \"%ls\" -C \"%ls\"", tar, file, dir);
    const int code = run_hidden(tar, cmd, dir, k_unpack_ms);
    swprintf(exe, n, L"%ls\\rpcs3.exe", dir);
    if (code != 0 || !is_file(exe)) {
        exe[0] = 0;
        remove_tree(dir);
        return fail(err, en, L"Could not unpack RPCS3 here. Install it yourself and choose rpcs3.exe");
    }
    return 1;
}

void game_drop_rpcs3(const wchar_t* data)
{
    wchar_t dir[GAME_PATH_MAX + 32];
    swprintf(dir, sizeof dir / sizeof dir[0], L"%ls\\rpcs3-download", data);
    remove_tree(dir);
}

/* Ascension-R --prepare-game <game folder> [--rpcs3 <rpcs3.exe>] [--rap <file>]: the same work as the
 * launcher's, without the window. data is the settings' folder. */
extern "C" int gowa_prepare_cli(const char* game, const char* rpcs3, const char* rap, const char* settings)
{
    static wchar_t g[GAME_PATH_MAX], r[GAME_PATH_MAX], l[GAME_PATH_MAX], d[GAME_PATH_MAX], e[512];
    MultiByteToWideChar(CP_UTF8, 0, game, -1, g, GAME_PATH_MAX);
    MultiByteToWideChar(CP_UTF8, 0, settings, -1, d, GAME_PATH_MAX);
    if (wchar_t* s = wcsrchr(d, L'\\')) *s = 0;
    if (rpcs3) MultiByteToWideChar(CP_UTF8, 0, rpcs3, -1, r, GAME_PATH_MAX);
    else game_find_rpcs3(g, r, GAME_PATH_MAX);
    if (rap) MultiByteToWideChar(CP_UTF8, 0, rap, -1, l, GAME_PATH_MAX);
    else if (r[0]) game_find_rap(r, l, GAME_PATH_MAX);
    if (!game_prepare(g, r, l, d, e, sizeof e / sizeof e[0])) {
        char m[1024];
        WideCharToMultiByte(CP_UTF8, 0, e, -1, m, sizeof m, NULL, NULL);
        printf("[prepare] %s\n", m);
        return 1;
    }
    printf("[prepare] GOWA.elf is ready\n");
    return 0;
}
