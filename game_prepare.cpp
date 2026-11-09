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
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

/* The name RPCS3 looks the license up by: the content id inside the SELF. */
static const wchar_t k_rap_name[] = L"UP9000-NPUA80918_00-GOWADIGITALVERS1.rap";
static const DWORD k_decrypt_ms = 60000, k_unpack_ms = 300000, k_stall_ms = 30000, k_read_ms = 8000, k_slow_ms = 5000;

/* The version of RPCS3 this was tried with. */
static const wchar_t k_rpcs3_url[] =
    L"https://github.com/RPCS3/rpcs3-binaries-win/releases/download/build-47599599a4cea37f350eb0936d7c5e4de6f7bf57/"
    L"rpcs3-v0.0.43-20573-47599599_win64_msvc.7z";
static const char k_rpcs3_sha256[] = "4876c2921d7a36fee007a3e9bf8cd42902f6f7da0bc67cb9d576ca386a1e4433";

static void set_phase(GameProgress* p, long phase)
{
    if (!p) return;
    p->phase_at = GetTickCount64();
    p->phase = phase;
}

static int stopped(const GameProgress* p) { return p && p->stop; }

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

const wchar_t* game_rpcs3_slot(const wchar_t* game_dir)
{
    static wchar_t p[GAME_PATH_MAX];
    swprintf(p, GAME_PATH_MAX, L"%ls", game_dir);
    for (int up = 0; up < 3; up++)
        if (wchar_t* s = wcsrchr(p, L'\\')) *s = 0;
    wcscat_s(p, GAME_PATH_MAX, L"\\rpcs3.exe");
    return p;
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
static int run_hidden(const wchar_t* exe, wchar_t* cmd, const wchar_t* cwd, DWORD wait_ms, const GameProgress* p)
{
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) return -1;
    DWORD code = (DWORD)-1;
    const ULONGLONG until = GetTickCount64() + wait_ms;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &code);
            break;
        }
        if (stopped(p) || GetTickCount64() >= until) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 3000);
            break;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int game_prepare(const wchar_t* game_dir, const wchar_t* rpcs3_exe, const wchar_t* rap, const wchar_t* data,
                 wchar_t* err, size_t n, GameProgress* p)
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

    set_phase(p, GP_COPY);
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
        set_phase(p, GP_DECRYPT);
        const ULONGLONG t0 = GetTickCount64();
        SetEnvironmentVariableW(L"RPCS3_CONFIG_DIR", cfg_short);
        const int code = run_hidden(exe_short, cmd, cfg_short, k_decrypt_ms, p);
        SetEnvironmentVariableW(L"RPCS3_CONFIG_DIR", NULL);
        fprintf(stderr, "[prepare] RPCS3 --decrypt: exit 0x%08X, %.1f s\n", (unsigned)code, (GetTickCount64() - t0) / 1000.0);
        set_phase(p, GP_CHECK);
        /* RPCS3 refuses a copy of the Visual C++ runtime beside it: it has to be installed. */
        if (stopped(p)) fail(err, n, L"Stopped");
        else if ((DWORD)code == 0xC0000135u) fail(err, n, L"RPCS3 needs the Microsoft Visual C++ Redistributable (x64). Install it and try again");
        else if (code == -1) fail(err, n, L"RPCS3 did not start or did not finish. Start it once by itself to see why");
        else if (!is_file(made)) fail(err, n, L"RPCS3 could not decrypt the game. Is the .rap this game's?");
        else if (!game_elf_ok(made)) fail(err, n, L"The decrypted file is not version 1.12 of this game");
        else if (!MoveFileExW(made, final_elf, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) fail(err, n, L"Could not keep GOWA.elf");
        else ok = 1;
    }
    remove_tree(work);
    return ok;
}

/* The download: several connections, each taking the next piece of the file. A connection that goes
 * quiet or falls far behind the rest is dropped and its piece is taken up again from where it stopped. */
enum { DL_THREADS = 4, DL_CHUNK = 1 << 21, DL_BUF = 1 << 16 };

/* The requests being read, so that a read that is waiting can be ended. */
static void* volatile s_requests[DL_THREADS];

static void drop_requests(void)
{
    for (int i = 0; i < DL_THREADS; i++)
        if (void* h = InterlockedExchangePointer((void* volatile*)&s_requests[i], NULL)) WinHttpCloseHandle(h);
}

void game_progress_stop(GameProgress* p)
{
    p->stop = 1;
    drop_requests();
}

struct Fetch {
    wchar_t host[256], path[2048];
    INTERNET_PORT port;
    DWORD flags;
    HINTERNET session;
    HANDLE file;
    long long total, next;
    int ranges;                 /* the server can send parts of the file */
    volatile long abort;
    volatile long busy;         /* pieces being fetched */
    volatile DWORD why, status;
    GameProgress* p;
    ULONGLONG started;
    CRITICAL_SECTION cs;
    struct { long long a, b; } again[64];   /* what was left of a dropped piece */
    int nagain;
};

struct FetchWorker { Fetch* f; int slot; };

static int take_piece(Fetch* f, long long* a, long long* b)
{
    for (;;) {
        if (f->abort) return 0;
        EnterCriticalSection(&f->cs);
        int got = 0;
        if (f->nagain) {
            f->nagain--;
            *a = f->again[f->nagain].a;
            *b = f->again[f->nagain].b;
            got = 1;
        } else if (f->next < f->total) {
            *a = f->next;
            *b = f->ranges && f->next + DL_CHUNK < f->total ? f->next + DL_CHUNK : f->total;
            f->next = *b;
            got = 1;
        }
        if (got) f->busy++;
        const long busy = f->busy;
        LeaveCriticalSection(&f->cs);
        if (got) return 1;
        if (!busy) return 0;    /* all of it fetched */
        Sleep(50);              /* another connection may yet hand part of its piece back */
    }
}

static void end_piece(Fetch* f, long long a, long long b)
{
    EnterCriticalSection(&f->cs);
    if (a < b && f->nagain < 64) {
        f->again[f->nagain].a = a;
        f->again[f->nagain].b = b;
        f->nagain++;
    }
    f->busy--;
    LeaveCriticalSection(&f->cs);
}

static int fetch_range(Fetch* f, HINTERNET c, int slot, unsigned char* buf, long long* at, long long end)
{
    HINTERNET r = WinHttpOpenRequest(c, L"GET", f->path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, f->flags);
    if (!r) { f->why = GetLastError(); return 0; }
    InterlockedExchangePointer((void* volatile*)&s_requests[slot], r);
    wchar_t range[96];
    swprintf(range, 96, L"Range: bytes=%lld-%lld\r\n", *at, end - 1);
    DWORD status = 0, size = sizeof status;
    int ok = 0;
    if (!f->abort &&
        WinHttpSendRequest(r, f->ranges ? range : WINHTTP_NO_ADDITIONAL_HEADERS, f->ranges ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
        status == (f->ranges ? 206u : 200u)) {
        long long window = 0;
        ULONGLONG window_at = GetTickCount64();
        for (;;) {
            /* A read alone waits for its whole buffer; asking first gets whatever has come. */
            DWORD ready = 0, got = 0, put = 0;
            if (!WinHttpQueryDataAvailable(r, &ready)) { f->why = GetLastError(); break; }
            if (!ready) { f->why = ERROR_WINHTTP_CONNECTION_ERROR; break; }
            if (!WinHttpReadData(r, buf, ready < (DWORD)DL_BUF ? ready : (DWORD)DL_BUF, &got)) { f->why = GetLastError(); break; }
            if (!got) { f->why = ERROR_WINHTTP_CONNECTION_ERROR; break; }
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)*at;
            ov.OffsetHigh = (DWORD)(*at >> 32);
            if (!WriteFile(f->file, buf, got, &put, &ov) || put != got) { f->why = GetLastError(); f->abort = 1; break; }
            *at += got;
            InterlockedExchangeAdd64((volatile LONG64*)&f->p->done, got);
            const ULONGLONG now = GetTickCount64();
            f->p->data_at = now;
            if (*at >= end) { ok = 1; break; }
            window += got;
            if (f->ranges && now - window_at >= k_slow_ms) {
                /* Compared with the whole download so far, so that the last connection still running is judged too. */
                const long long average = f->p->done * 1000 / (long long)(now - f->started + 1);
                if (window * 1000 / (long long)(now - window_at) * 4 < average / DL_THREADS) break;
                window = 0;
                window_at = now;
            }
        }
    } else {
        f->why = GetLastError();
        if (status) {
            f->status = status;
            if (status >= 400 && status < 500) f->abort = 1;    /* asking again will not help */
        }
    }
    if (void* h = InterlockedExchangePointer((void* volatile*)&s_requests[slot], NULL)) WinHttpCloseHandle(h);
    return ok;
}

static DWORD WINAPI fetch_worker(LPVOID arg)
{
    const FetchWorker w = *(FetchWorker*)arg;
    Fetch* f = w.f;
    unsigned char* buf = (unsigned char*)malloc(DL_BUF);
    HINTERNET c = WinHttpConnect(f->session, f->host, f->port, 0);
    long long a, b;
    while (buf && c && take_piece(f, &a, &b)) {
        const int ok = fetch_range(f, c, w.slot, buf, &a, b);
        if (!ok && !f->abort) {
            if (f->ranges) {
                InterlockedIncrement(&f->p->retries);
                Sleep(300);
            } else {
                f->abort = 1;       /* the whole file or nothing: nothing to go on from */
            }
        }
        end_piece(f, a, b);
    }
    if (c) WinHttpCloseHandle(c);
    free(buf);
    return 0;
}

/* Whether the server can send parts of the file, and how long the file is. Every piece is asked for at this
 * address and not where it leads: the file's own host hands out links that run out within minutes. */
static int fetch_probe(Fetch* f, const wchar_t* url, wchar_t* err, size_t en)
{
    URL_COMPONENTS u = {};
    u.dwStructSize = sizeof u;
    u.lpszHostName = f->host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = f->path;
    u.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) return fail(err, en, L"Bad download address");
    f->port = u.nPort;
    f->flags = u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET c = WinHttpConnect(f->session, f->host, f->port, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", f->path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, f->flags) : NULL;
    if (r) InterlockedExchangePointer((void* volatile*)&s_requests[0], r);
    int ok = 0;
    DWORD status = 0, size = sizeof status;
    if (r && WinHttpSendRequest(r, L"Range: bytes=0-0\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        if (status == 206) {
            wchar_t range[128];
            size = sizeof range;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, range, &size, WINHTTP_NO_HEADER_INDEX))
                if (const wchar_t* s = wcschr(range, L'/')) f->total = _wtoi64(s + 1);
            f->ranges = 1;
            ok = f->total > 0;
        } else if (status == 200) {
            DWORD length = 0;
            size = sizeof length;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX);
            f->total = length;
            ok = length > 0;
        }
    }
    const DWORD why = GetLastError();
    if (void* h = InterlockedExchangePointer((void* volatile*)&s_requests[0], NULL)) WinHttpCloseHandle(h);
    if (c) WinHttpCloseHandle(c);
    if (ok) return 1;
    fprintf(stderr, "[prepare] download could not start: HTTP %u, error %u\n", (unsigned)status, (unsigned)why);
    if (err && en) {
        if (status) swprintf(err, en, L"Could not download RPCS3: the server said %u", (unsigned)status);
        else swprintf(err, en, L"Could not reach the download. Check the connection and try again (error %u)", (unsigned)why);
    }
    return 0;
}

static int http_download(const wchar_t* url, const wchar_t* file, GameProgress* progress, wchar_t* err, size_t en)
{
    GameProgress spare = {};
    GameProgress* p = progress ? progress : &spare;
    Fetch* f = (Fetch*)calloc(1, sizeof(Fetch));
    if (!f) return fail(err, en, L"Out of memory");
    f->p = p;
    f->file = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&f->cs);
    f->session = WinHttpOpen(L"Ascension-R", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    int ok = 0, stalled = 0;
    if (!f->session) {
        fail(err, en, L"Could not start the download");
    } else {
        WinHttpSetTimeouts(f->session, 10000, 10000, 10000, k_read_ms);
        int probed = 0;
        for (int i = 0; i < 3 && !probed && !stopped(p); i++) {
            if (i) Sleep(1000);
            probed = fetch_probe(f, url, err, en);
        }
        if (!probed && stopped(p) && err && en) swprintf(err, en, L"Stopped");
        if (probed) {
            p->total = f->total;
            p->data_at = f->started = GetTickCount64();
            f->file = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            LARGE_INTEGER end;
            end.QuadPart = f->total;
            if (f->file == INVALID_HANDLE_VALUE || !SetFilePointerEx(f->file, end, NULL, FILE_BEGIN) || !SetEndOfFile(f->file)) {
                fail(err, en, L"Could not write the download to disk");
            } else {
                const int n = f->ranges ? DL_THREADS : 1;
                FetchWorker w[DL_THREADS];
                HANDLE th[DL_THREADS];
                for (int i = 0; i < n; i++) {
                    w[i].f = f;
                    w[i].slot = i;
                    th[i] = CreateThread(NULL, 0, fetch_worker, &w[i], 0, NULL);
                }
                long long mark_done = 0;
                ULONGLONG mark_at = GetTickCount64();
                while (WaitForMultipleObjects(n, th, TRUE, 250) == WAIT_TIMEOUT) {
                    const ULONGLONG now = GetTickCount64();
                    if (now - mark_at >= 1000) {
                        p->speed = (p->done - mark_done) * 1000 / (long long)(now - mark_at);
                        mark_done = p->done;
                        mark_at = now;
                    }
                    if (p->stop) f->abort = 1;
                    if (now - p->data_at >= k_stall_ms && !f->abort) {
                        stalled = 1;
                        f->abort = 1;
                    }
                    if (f->abort) drop_requests();
                }
                for (int i = 0; i < n; i++) CloseHandle(th[i]);
                ok = !f->abort && p->done == f->total;
                fprintf(stderr, "[prepare] download: %d connections, %.1f of %.1f MB in %.1f s, %ld reconnects\n", n, p->done / 1048576.0,
                        f->total / 1048576.0, (GetTickCount64() - f->started) / 1000.0, (long)p->retries);
                if (!ok && err && en) {
                    const unsigned quiet = (unsigned)((GetTickCount64() - p->data_at) / 1000);
                    if (stopped(p)) swprintf(err, en, L"Stopped");
                    else if (f->status) swprintf(err, en, L"Could not download RPCS3: the server said %u", (unsigned)f->status);
                    else if (stalled || f->why == ERROR_WINHTTP_TIMEOUT) swprintf(err, en, L"The download stalled: no data for %u s. Check the connection and try again", quiet);
                    else swprintf(err, en, L"Could not download RPCS3 (error %u). Check the connection and try again", (unsigned)f->why);
                }
            }
        }
    }
    if (f->file != INVALID_HANDLE_VALUE) CloseHandle(f->file);
    if (f->session) WinHttpCloseHandle(f->session);
    DeleteCriticalSection(&f->cs);
    free(f);
    return ok;
}

int game_fetch_rpcs3(const wchar_t* data, wchar_t* exe, size_t n, wchar_t* err, size_t en, GameProgress* p)
{
    enum { P = GAME_PATH_MAX + 128 };
    wchar_t dir[P], file[P], tar[MAX_PATH];
    swprintf(dir, P, L"%ls\\rpcs3-download", data);
    swprintf(file, P, L"%ls\\rpcs3.7z", dir);
    remove_tree(dir);
    make_path(dir);
    set_phase(p, GP_DOWNLOAD);
    ULONGLONG t0 = GetTickCount64();
    if (!http_download(k_rpcs3_url, file, p, err, en)) {
        remove_tree(dir);
        return 0;
    }
    fprintf(stderr, "[prepare] RPCS3 downloaded in %.1f s\n", (GetTickCount64() - t0) / 1000.0);
    set_phase(p, GP_VERIFY);
    char sum[65];
    if (!game_file_sha256(file, sum) || strcmp(sum, k_rpcs3_sha256)) {
        remove_tree(dir);
        return fail(err, en, L"The download of RPCS3 is not the expected file");
    }
    GetSystemDirectoryW(tar, MAX_PATH);
    wcscat_s(tar, MAX_PATH, L"\\tar.exe");
    static wchar_t cmd[P * 2 + 64];
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" -xf rpcs3.7z", tar);   /* relative: tar takes no path that is not ASCII */
    set_phase(p, GP_UNPACK);
    t0 = GetTickCount64();
    const int code = run_hidden(tar, cmd, dir, k_unpack_ms, p);
    fprintf(stderr, "[prepare] RPCS3 unpacked: exit %d, %.1f s\n", code, (GetTickCount64() - t0) / 1000.0);
    swprintf(exe, n, L"%ls\\rpcs3.exe", dir);
    if (code != 0 || !is_file(exe)) {
        exe[0] = 0;
        remove_tree(dir);
        return fail(err, en, stopped(p) ? L"Stopped" : L"Could not unpack RPCS3 here. Install it yourself and choose rpcs3.exe");
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
    if (!game_prepare(g, r, l, d, e, sizeof e / sizeof e[0], NULL)) {
        char m[1024];
        WideCharToMultiByte(CP_UTF8, 0, e, -1, m, sizeof m, NULL, NULL);
        printf("[prepare] %s\n", m);
        return 1;
    }
    printf("[prepare] GOWA.elf is ready\n");
    return 0;
}
