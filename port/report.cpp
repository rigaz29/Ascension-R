/*
 * Ascension-R -- what a session leaves behind when something goes wrong
 *
 * Everyone builds this port on their own PC, with their own GPU and driver, and the problems worth
 * knowing about are the ones that do not happen on the machine it was written on.
 *
 *  - At the start of a session's log: the build, Windows, the CPU, the memory, each GPU with its
 *    driver, the display and the game's version.
 *  - The freeze watch: no new frame for GOWA_FREEZE_WATCH seconds (15; 0 = off) and what each guest
 *    thread is inside, and each host thread's CPU and stack, go in the log; once more at four times
 *    that. A game that froze and was closed by force says nothing otherwise.
 *  - How a session ended, read back from its log, so the launcher can offer a report for one that
 *    did not end normally.
 *  - The report: a zip beside the logs, for the player to attach to an issue. Nothing is sent from
 *    here. It holds no file of the game. Folder paths and the account's name are hidden. Saves come
 *    only when asked for, without the game's own images and sounds. missed_code.txt stays out: its
 *    rows start with bytes of the game's code.
 *
 * `Ascension-R --report [--with-saves]` makes one without the window.
 */
#include "report.h"
#include "version.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <winevt.h>
#include <intrin.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "wevtapi.lib")

#include "game_find.h"
#include "ps3emu/ps3_settings.h"

#ifndef GOWA_PORT_COMMIT
#define GOWA_PORT_COMMIT "unknown"
#endif
#ifndef GOWA_FORK_COMMIT
#define GOWA_FORK_COMMIT "unknown"
#endif

extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" const char* g_hle_inflight[64];
extern "C" void ppu_report_guest_lrs(void);
extern "C" int rsx_d3d12_backend_generating(void);   /* rsx_d3d12_backend.c: frame generation runs */

static std::string s_session_log;   /* this session's, "" when a script took the output */

static std::string narrow(const wchar_t* w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    return s;
}

static std::wstring wide(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (n <= 1) return std::wstring();
    std::wstring w((size_t)n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::string strf(const char* fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

static std::string exe_path(void)
{
    char p[1100];
    const DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
    return n && n < sizeof p ? std::string(p) : std::string();
}

static std::string dir_of(const std::string& p)
{
    const size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? std::string() : p.substr(0, i);
}

static std::string base_of(const std::string& p)
{
    const size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? p : p.substr(i + 1);
}

static std::string logs_dir(void)
{
    const std::string d = dir_of(ps3_settings_path());
    return (d.empty() ? std::string(".") : d) + "\\logs";
}

/* The identity of the build, in the log and in a report: a stack from another build reads wrong. */
static uint32_t exe_stamp(void)
{
    const uint8_t* b = (const uint8_t*)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS* n = (const IMAGE_NT_HEADERS*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
    return n->FileHeader.TimeDateStamp;
}

void gowa_where(const void* a, char* out, size_t n)
{
    HMODULE m = NULL;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m)
        GetModuleFileNameA(m, mod, sizeof mod);
    const char* b = strrchr(mod, '\\');
    snprintf(out, n, "%s+0x%llX", b ? b + 1 : mod, (unsigned long long)((uintptr_t)a - (uintptr_t)m));
}

/* --- the system ------------------------------------------------------------- */
static void gpu_lines(std::string& s)
{
    IDXGIFactory1* f = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f)) || !f) return;
    IDXGIAdapter1* a = NULL;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            LARGE_INTEGER v;
            std::string drv = "unknown";
            if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v)))
                drv = strf("%u.%u.%u.%u", (unsigned)HIWORD(v.HighPart), (unsigned)LOWORD(v.HighPart),
                           (unsigned)HIWORD(v.LowPart), (unsigned)LOWORD(v.LowPart));
            s += strf("GPU %u: %s, driver %s, %llu MB\n", i, narrow(d.Description).c_str(), drv.c_str(),
                      (unsigned long long)d.DedicatedVideoMemory >> 20);
        }
        a->Release();
    }
    f->Release();
}

static std::string system_text(void)
{
    std::string s = strf("Version: %s\n", AR_VERSION);
#ifdef __clang__
    const std::string cc = strf("clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#else
    const std::string cc = "another compiler";
#endif
    s += strf("Build: port %s, ps3recomp %s, %s, exe linked %08X\n", GOWA_PORT_COMMIT, GOWA_FORK_COMMIT, cc.c_str(), exe_stamp());

    typedef LONG (WINAPI *rtl_ver_fn)(OSVERSIONINFOW*);
    const rtl_ver_fn ver = (rtl_ver_fn)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW os;
    memset(&os, 0, sizeof os);
    os.dwOSVersionInfoSize = sizeof os;
    if (ver && !ver(&os))
        s += strf("Windows: %lu.%lu.%lu\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);

    int r[4];
    char brand[49] = {0};
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] >= 0x80000004u)
        for (int i = 0; i < 3; i++) { __cpuid(r, (int)(0x80000002u + i)); memcpy(brand + 16 * i, r, 16); }
    for (int i = 47; i >= 0 && (brand[i] == ' ' || !brand[i]); i--) brand[i] = 0;
    const char* b = brand;
    while (*b == ' ') b++;
    s += strf("CPU: %s, %lu threads\n", *b ? b : "unknown", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m))
        s += strf("Memory: %.1f GB, %.1f GB free\n", m.ullTotalPhys / 1073741824.0, m.ullAvailPhys / 1073741824.0);

    gpu_lines(s);

    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
        s += strf("Display: %lux%lu at %lu Hz\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);

    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps))
        s += strf("Power: %s\n", ps.BatteryFlag & 128 ? "no battery" : ps.ACLineStatus == 1 ? "plugged in" : "on battery");

    const char* vfs = ps3_settings_str("paths", "vfs", "PS3_VFS_ROOT", NULL);
    GameInfo g;
    if (vfs && *vfs && game_read_info((wide(vfs) + L"\\PS3_GAME").c_str(), &g))
        s += strf("Game: %s, version %s\n", g.title_id, g.app_ver);
    else
        s += "Game: no PARAM.SFO found\n";
    return s;
}

/* A console closed by hand ends the process with no other word. */
static BOOL WINAPI console_closing(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        fprintf(stderr, "[session] ended: console event %lu\n", type);
        fflush(stderr);
    }
    return FALSE;
}

void gowa_report_session_start(const char* log_path)
{
    s_session_log = log_path ? log_path : "";
    const std::string s = system_text();
    size_t from = 0, e;
    while ((e = s.find('\n', from)) != std::string::npos) {
        printf("[system] %.*s\n", (int)(e - from), s.c_str() + from);
        from = e + 1;
    }
    fflush(stdout);
    SetConsoleCtrlHandler(console_closing, TRUE);
}

/* --- the freeze watch --------------------------------------------------------- */
namespace {
struct HostThread { DWORD tid; ULONGLONG cpu; char name[48]; };
enum { MAX_THREADS = 512, MAX_FRAMES = 10 };
}

static int threads_read(HostThread* out, int max)
{
    typedef HRESULT (WINAPI *get_desc_fn)(HANDLE, PWSTR*);
    static const get_desc_fn get_desc = (get_desc_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    int n = 0;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok && n < max; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            HostThread& t = out[n++];
            t.tid = te.th32ThreadID;
            t.cpu = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
            snprintf(t.name, sizeof t.name, "tid %lu", (unsigned long)t.tid);
            PWSTR w = NULL;
            if (get_desc && SUCCEEDED(get_desc(h, &w)) && w && w[0]) {
                int i = 0;
                for (; w[i] && i < 47; i++) t.name[i] = (char)(w[i] < 128 ? w[i] : '?');
                t.name[i] = 0;
            }
            if (w) LocalFree(w);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}

/* The return addresses of a thread, stopped for as long as that takes. Nothing here allocates or
 * prints: the thread may hold the heap's lock or stderr's. A bad frame ends the walk. */
static int thread_stack(DWORD tid, uintptr_t* pc, int max)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return 0;
    volatile int n = 0;
    if (SuspendThread(h) != (DWORD)-1) {
        static CONTEXT c;   /* aligned, and only this thread walks */
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c)) {
            __try {
                while (n < max && c.Rip) {
                    pc[n] = (uintptr_t)c.Rip;
                    n = n + 1;
                    DWORD64 img = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &img, NULL);
                    if (!fe) break;
                    PVOID hd = NULL;
                    DWORD64 est = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c.Rip, fe, &c, &hd, &est, NULL);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return n;
}

static void freeze_report(ULONGLONG idle, unsigned frames)
{
    static HostThread a[MAX_THREADS], b[MAX_THREADS];
    const int na = threads_read(a, MAX_THREADS);
    Sleep(1000);
    const int nb = threads_read(b, MAX_THREADS);
    fprintf(stderr, "[freeze] no new frame for %llu s, %u frames so far\n", idle, frames);
    if (rsx_d3d12_backend_generating())
        fprintf(stderr, "[freeze] frame generation is on: its swap chain has stopped the GPU at the start of a session on "
                        "some PCs. If this repeats, turn it off in the launcher's Graphics tab\n");
    for (unsigned t = 0; t < 64; t++)
        if (g_hle_inflight[t]) fprintf(stderr, "[freeze] guest thread %u is inside %s\n", t, g_hle_inflight[t]);
    ppu_report_guest_lrs();
    for (int i = 0; i < nb; i++) {
        ULONGLONG before = b[i].cpu;
        for (int j = 0; j < na; j++) if (a[j].tid == b[i].tid) { before = a[j].cpu; break; }
        uintptr_t pc[MAX_FRAMES];
        const int n = thread_stack(b[i].tid, pc, MAX_FRAMES);
        std::string line = strf("[freeze] %s, %u%% of a core:", b[i].name, (unsigned)((b[i].cpu - before) / 100000));
        for (int k = 0; k < n; k++) {
            char w[300];
            gowa_where((const void*)pc[k], w, sizeof w);
            line += k ? " < " : " ";
            line += w;
        }
        fprintf(stderr, "%s\n", line.c_str());
    }
    fflush(stderr);
}

static DWORD WINAPI freeze_watch(LPVOID arg)
{
    const ULONGLONG limit = (ULONGLONG)(uintptr_t)arg;
    unsigned last = 0;
    ULONGLONG since = GetTickCount64();
    int told = 0;
    for (;;) {
        Sleep(1000);
        const unsigned now = cellGcm_flip_request_count();
        const ULONGLONG t = GetTickCount64();
        if (now != last) {
            if (told) { fprintf(stderr, "[freeze] frames again after %llu s\n", (t - since) / 1000); fflush(stderr); }
            last = now;
            since = t;
            told = 0;
            continue;
        }
        if (!last || told >= 2) continue;   /* before the first frame the game is booting */
        const ULONGLONG idle = (t - since) / 1000;
        if (idle >= (told ? limit * 4 : limit)) {
            freeze_report(idle, now);
            told++;
        }
    }
}

void gowa_freeze_watch_start(void)
{
    const char* e = getenv("GOWA_FREEZE_WATCH");
    if (!e && s_session_log.empty()) return;   /* a script's run: as it was */
    const int s = e ? atoi(e) : 15;
    if (s > 0) CreateThread(NULL, 0, freeze_watch, (LPVOID)(uintptr_t)s, 0, NULL);
}

/* --- reading the logs back ------------------------------------------------------ */
/* A file that may still be open for writing, or all of it when it is not long. */
static int read_capped(const std::string& path, std::string& out, size_t head, size_t tail)
{
    out.clear();
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return 0; }
    auto read_n = [&](size_t n) {
        const size_t at = out.size();
        out.resize(at + n);
        DWORD got = 0;
        if (n && !ReadFile(h, &out[at], (DWORD)n, &got, NULL)) got = 0;
        out.resize(at + got);
    };
    if ((unsigned long long)sz.QuadPart <= head + tail) {
        read_n((size_t)sz.QuadPart);
    } else {
        read_n(head);
        out += strf("\n[report] %llu bytes left out here\n", (unsigned long long)sz.QuadPart - head - tail);
        LARGE_INTEGER to;
        to.QuadPart = sz.QuadPart - (LONGLONG)tail;
        SetFilePointerEx(h, to, NULL, FILE_BEGIN);
        read_n(tail);
    }
    CloseHandle(h);
    return 1;
}

static std::string line_at(const std::string& s, size_t p)
{
    size_t e = s.find('\n', p);
    if (e == std::string::npos) e = s.size();
    while (e > p && s[e - 1] == '\r') e--;
    return s.substr(p, e - p);
}

static SessionEnd session_end_of(const std::string& log, std::string* why)
{
    const size_t no = std::string::npos;
    why->clear();
    if (log.find("=== ps3recomp game runner ===") == no) return SESSION_NONE;
    size_t p;
    if ((p = log.rfind("[crash] exception")) != no) { *why = line_at(log, p); return SESSION_CRASH; }
    if ((p = log.rfind("[D3D12] device lost")) != no) { *why = line_at(log, p); return SESSION_GPU_LOST; }
    /* Frames that stopped and never came back: the player closing the window of a frozen game still saw it freeze. */
    if ((p = log.rfind("[freeze] no new frame")) != no && log.find("[freeze] frames again", p) == no) {
        *why = line_at(log, p);
        return SESSION_FROZE;
    }
    static const char* const clean[] = {
        "[rsx] window closed", "sys_process_exit", "[quit] ", "[boot] ppu_run returned", "[session] ended",
    };
    for (const char* c : clean)
        if ((p = log.rfind(c)) != no) { *why = line_at(log, p); return SESSION_CLEAN; }
    if ((p = log.rfind("[freeze] no new frame")) != no) { *why = line_at(log, p); return SESSION_FROZE; }
    return SESSION_SUDDEN;
}

static const char* session_end_name(SessionEnd e)
{
    switch (e) {
    case SESSION_CLEAN:    return "ended normally";
    case SESSION_SUDDEN:   return "stopped with no reason in the log";
    case SESSION_FROZE:    return "froze";
    case SESSION_GPU_LOST: return "lost the graphics device";
    case SESSION_CRASH:    return "crashed";
    default:               return "the game did not start";
    }
}

/* The session logs here, newest first, without this session's. */
static std::vector<std::string> session_logs(void)
{
    std::vector<std::string> v;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((logs_dir() + "\\session-*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    const std::string me = base_of(s_session_log);
    do {
        if (me != fd.cFileName) v.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(v.rbegin(), v.rend());
    return v;
}

SessionEnd gowa_last_session(int* generating)
{
    if (generating) *generating = 0;
    const std::vector<std::string> logs = session_logs();
    if (logs.empty()) return SESSION_NONE;
    const std::string path = logs_dir() + "\\" + logs[0];
    /* Refusing others' writes fails while a copy of the game still writes it: that one has not ended. */
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return SESSION_NONE;
    CloseHandle(h);
    std::string log, why;
    if (!read_capped(path, log, 1u << 20, 8u << 20)) return SESSION_NONE;
    if (generating) *generating = log.find("[D3D12] frame generation on:") != std::string::npos;
    return session_end_of(log, &why);
}

/* --- hiding who the player is --------------------------------------------------- */
static size_t find_ci(const std::string& s, const std::string& what, size_t from)
{
    const size_t n = what.size();
    if (!n || s.size() < n) return std::string::npos;
    const int c0 = tolower((unsigned char)what[0]);
    for (size_t i = from; i + n <= s.size(); i++)
        if (tolower((unsigned char)s[i]) == c0 && !_strnicmp(s.data() + i, what.data(), n)) return i;
    return std::string::npos;
}

static std::vector<std::pair<std::string, std::string>> s_hide;   /* longest first */
static std::vector<std::pair<std::string, std::string>> s_hide_word;

static void hide_path(std::string p, const char* token)
{
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    if (p.size() <= 3) return;
    std::string a = p, b = p;
    std::replace(a.begin(), a.end(), '/', '\\');
    std::replace(b.begin(), b.end(), '\\', '/');
    s_hide.push_back(std::make_pair(a, std::string(token)));
    s_hide.push_back(std::make_pair(b, std::string(token)));
}

/* A name is hidden as a whole word, unless the word is one a log uses by itself. */
static void hide_name(const wchar_t* env, const char* token)
{
    wchar_t w[256];
    if (!GetEnvironmentVariableW(env, w, 256)) return;
    std::string n = narrow(w);
    if (n.size() < 3) return;
    static const char* const common[] = { "user", "users", "admin", "administrator", "owner", "game", "system", "test",
                                          "guest", "home", "default", "player" };
    for (const char* c : common) if (!_stricmp(n.c_str(), c)) return;
    s_hide_word.push_back(std::make_pair(n, std::string(token)));
    char ansi[512];
    if (WideCharToMultiByte(CP_ACP, 0, w, -1, ansi, sizeof ansi, NULL, NULL) && n != ansi)
        s_hide_word.push_back(std::make_pair(std::string(ansi), std::string(token)));
}

static void hide_init(void)
{
    s_hide.clear();
    s_hide_word.clear();
    static const struct { const char* section, *key, *token; } settings[] = {
        { "paths", "elf", "<elf>" }, { "paths", "vfs", "<vfs>" }, { "paths", "hdd0", "<hdd0>" },
        { "paths", "ffmpeg", "<ffmpeg>" }, { "paths", "mods", "<mods>" },
        { "graphics", "shader_cache", "<shader_cache>" },
    };
    for (const auto& k : settings) {
        const char* v = ps3_settings_str(k.section, k.key, NULL, NULL);
        if (v && *v) hide_path(v, k.token);
    }
    const std::string here = dir_of(exe_path());
    hide_path(here, "<exe>");
    hide_path(dir_of(here), "<port>");
    hide_path(dir_of(ps3_settings_path()), "<settings>");
    static const struct { const char* name, *token; } env[] = {
        { "TEMP", "<temp>" }, { "TMP", "<temp>" }, { "LOCALAPPDATA", "<localappdata>" },
        { "APPDATA", "<appdata>" }, { "USERPROFILE", "<home>" },
    };
    for (const auto& k : env) {
        const char* v = getenv(k.name);
        if (v && *v) hide_path(v, k.token);
    }
    std::stable_sort(s_hide.begin(), s_hide.end(), [](const std::pair<std::string, std::string>& a,
                                                      const std::pair<std::string, std::string>& b) {
        return a.first.size() > b.first.size();
    });
    hide_name(L"USERNAME", "<user>");
    hide_name(L"COMPUTERNAME", "<pc>");
}

static void hide_in(std::string& s)
{
    for (const auto& h : s_hide) {
        for (size_t p = 0; (p = find_ci(s, h.first, p)) != std::string::npos; p += h.second.size())
            s.replace(p, h.first.size(), h.second);
    }
    /* Any other account's folder: X:\Users\<name>. */
    static const std::string users = "users";
    for (size_t i = 0; (i = find_ci(s, users, i)) != std::string::npos;) {
        const size_t after = i + 5;
        if (i >= 3 && s[i - 2] == ':' && isalpha((unsigned char)s[i - 3]) && (s[i - 1] == '\\' || s[i - 1] == '/') &&
            after < s.size() && (s[after] == '\\' || s[after] == '/')) {
            size_t e = after + 1;
            while (e < s.size() && !strchr("\\/\r\n\"'<>", s[e])) e++;
            s.replace(after + 1, e - after - 1, "<user>");
            i = after + 7;
        } else {
            i = after;
        }
    }
    for (const auto& h : s_hide_word) {
        const size_t n = h.first.size();
        for (size_t p = 0; (p = find_ci(s, h.first, p)) != std::string::npos;) {
            const int before = p ? (unsigned char)s[p - 1] : 0, after = p + n < s.size() ? (unsigned char)s[p + n] : 0;
            if (isalnum(before) || before == '_' || isalnum(after) || after == '_') { p += n; continue; }
            s.replace(p, n, h.second);
            p += h.second.size();
        }
    }
}

/* --- naming the game's own frames ------------------------------------------------ */
namespace { struct Sym { uint32_t rva, name; }; }
static std::vector<Sym> s_syms;
static std::string s_sym_names;
static int s_map_read;

static void map_read(void)
{
    if (s_map_read) return;
    s_map_read = 1;
    std::string path = exe_path();
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return;
    path = path.substr(0, dot) + ".map";
    std::string m;
    if (!read_capped(path, m, 1u << 28, 0)) return;
    uint64_t base = 0x140000000ull;
    const size_t bp = m.find("Preferred load address is ");
    if (bp != std::string::npos) base = strtoull(m.c_str() + bp + 26, NULL, 16);
    size_t from = 0, e;
    while ((e = m.find('\n', from)) != std::string::npos) {
        const char* l = m.c_str() + from;
        from = e + 1;
        while (*l == ' ') l++;
        if (strncmp(l, "0001:", 5)) continue;
        const char* n = strpbrk(l, " ");
        if (!n) continue;
        while (*n == ' ') n++;
        const char* ne = strpbrk(n, " \r\n");
        if (!ne) continue;
        const char* ap = ne;
        while (*ap == ' ') ap++;
        char* end = NULL;
        const uint64_t a = strtoull(ap, &end, 16);
        if (end == ap || a < base) continue;
        Sym s;
        s.rva = (uint32_t)(a - base);
        s.name = (uint32_t)s_sym_names.size();
        /* ?func_0041CBDC@@YAXPEAU... is func_0041CBDC */
        const char* cut = n;
        if (*cut == '?') { cut++; const char* at = strchr(cut, '@'); if (at && at < ne) ne = at; }
        s_sym_names.append(cut, (size_t)(ne - cut));
        s_sym_names.push_back(0);
        s_syms.push_back(s);
    }
    std::stable_sort(s_syms.begin(), s_syms.end(), [](const Sym& a, const Sym& b) { return a.rva < b.rva; });
}

/* Each offset in the game's own code named, if this log came from this build. */
static void name_frames(std::string& s)
{
    const std::string key = base_of(exe_path()) + "+0x";
    if (s.find(key) == std::string::npos) return;
    const std::string stamp = strf("exe linked %08X", exe_stamp());
    if (s.find(stamp) == std::string::npos) return;
    map_read();
    if (s_syms.empty()) return;
    std::string out;
    out.reserve(s.size() + 8192);
    size_t from = 0, p;
    while ((p = s.find(key, from)) != std::string::npos) {
        size_t e = p + key.size();
        uint32_t rva = 0;
        while (e < s.size() && isxdigit((unsigned char)s[e])) {
            const char c = s[e++];
            rva = rva * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        out.append(s, from, e - from);
        Sym probe;
        probe.rva = rva;
        probe.name = 0;
        auto it = std::upper_bound(s_syms.begin(), s_syms.end(), probe, [](const Sym& a, const Sym& b) { return a.rva < b.rva; });
        if (it != s_syms.begin()) {
            --it;
            out += strf(" (%s+0x%X)", s_sym_names.c_str() + it->name, rva - it->rva);
        }
        from = e;
    }
    out.append(s, from, std::string::npos);
    s.swap(out);
}

/* --- Windows' own records -------------------------------------------------------- */
/* What Windows wrote when this exe crashed or was closed as not responding, the last two weeks. It
 * sees the ends the game's own handler does not. */
static std::string windows_events(void)
{
    std::string out;
    const std::string exe = base_of(exe_path());
    EVT_HANDLE q = EvtQuery(NULL, L"Application",
                            L"*[System[(EventID=1000 or EventID=1002) and TimeCreated[timediff(@SystemTime) <= 1209600000]]]",
                            EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q) return out;
    EVT_HANDLE ev[16];
    DWORD got = 0;
    int kept = 0;
    std::vector<wchar_t> buf(1u << 16);
    while (kept < 8 && EvtNext(q, 16, ev, 2000, 0, &got)) {
        for (DWORD i = 0; i < got; i++) {
            DWORD used = 0, props = 0;
            if (kept < 8 && EvtRender(NULL, ev[i], EvtRenderEventXml, (DWORD)(buf.size() * sizeof(wchar_t)), buf.data(), &used, &props)) {
                const std::string x = narrow(buf.data());
                if (find_ci(x, exe, 0) != std::string::npos) { out += x + "\n\n"; kept++; }
            }
            EvtClose(ev[i]);
        }
    }
    EvtClose(q);
    return out;
}

/* --- a zip file ------------------------------------------------------------------ */
static uint32_t s_crc[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc[i] = c;
    }
}

static uint32_t crc_of(const std::string& d)
{
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : d) c = s_crc[(c ^ b) & 255] ^ (c >> 8);
    return ~c;
}

namespace {
struct Bits {
    std::vector<uint8_t>& o;
    uint32_t acc;
    int n;
    void put(uint32_t v, int bits)
    {
        acc |= v << n;
        n += bits;
        while (n >= 8) { o.push_back((uint8_t)acc); acc >>= 8; n -= 8; }
    }
    void end(void) { if (n) o.push_back((uint8_t)acc); acc = 0; n = 0; }
};
}

static uint32_t reversed(uint32_t v, int bits)
{
    uint32_t r = 0;
    for (int i = 0; i < bits; i++) { r = r << 1 | (v & 1); v >>= 1; }
    return r;
}

/* The fixed codes of RFC 1951, section 3.2.6. */
static void put_symbol(Bits& b, int s)
{
    if (s < 144)      b.put(reversed(0x30u + s, 8), 8);
    else if (s < 256) b.put(reversed(0x190u + s - 144, 9), 9);
    else if (s < 280) b.put(reversed((uint32_t)s - 256, 7), 7);
    else              b.put(reversed(0xC0u + s - 280, 8), 8);
}

/* DEFLATE with the fixed codes: a little of the ratio for no tables to build and send. A session's
 * log goes to about a tenth, and the saves, mostly zeros, to a few percent. */
static void deflate_fixed(const std::string& d, std::vector<uint8_t>& out)
{
    static const unsigned short len_base[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const unsigned char len_extra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const unsigned short dist_base[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const unsigned char dist_extra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    enum { WINDOW = 32768, HASH = 1 << 15, CHAIN = 32, MIN_MATCH = 3, MAX_MATCH = 258 };
    const uint8_t* in = (const uint8_t*)d.data();
    const size_t n = d.size();
    std::vector<int32_t> head(HASH, -1), prev(WINDOW, -1);
    auto hash = [&](size_t p) { return (uint32_t)((in[p] << 10) ^ (in[p + 1] << 5) ^ in[p + 2]) & (HASH - 1); };
    auto insert = [&](size_t p) {
        if (p + 2 >= n) return;
        const uint32_t h = hash(p);
        prev[p & (WINDOW - 1)] = head[h];
        head[h] = (int32_t)p;
    };
    Bits b = { out, 0, 0 };
    b.put(1, 1);   /* the last block */
    b.put(1, 2);   /* fixed codes */
    size_t i = 0;
    while (i < n) {
        size_t best = 0, dist = 0;
        if (i + MIN_MATCH <= n) {
            int32_t c = head[hash(i)];
            const size_t most = n - i < MAX_MATCH ? n - i : (size_t)MAX_MATCH;
            for (int k = 0; c >= 0 && i - (size_t)c < WINDOW && k < CHAIN; k++) {
                size_t l = 0;
                while (l < most && in[(size_t)c + l] == in[i + l]) l++;
                if (l > best) { best = l; dist = i - (size_t)c; if (l == most) break; }
                c = prev[(size_t)c & (WINDOW - 1)];
            }
        }
        if (best >= MIN_MATCH) {
            int ls = 28;
            while (len_base[ls] > best) ls--;
            put_symbol(b, 257 + ls);
            if (len_extra[ls]) b.put((uint32_t)(best - len_base[ls]), len_extra[ls]);
            int ds = 29;
            while (dist_base[ds] > dist) ds--;
            b.put(reversed((uint32_t)ds, 5), 5);
            if (dist_extra[ds]) b.put((uint32_t)(dist - dist_base[ds]), dist_extra[ds]);
            for (size_t k = 0; k < best; k++) insert(i + k);
            i += best;
        } else {
            put_symbol(b, in[i]);
            insert(i);
            i++;
        }
    }
    put_symbol(b, 256);
    b.end();
}

namespace {
struct ZipEntry { std::string name; uint32_t crc, csize, usize, offset; uint16_t method; };
struct Zip { FILE* f; std::vector<ZipEntry> e; uint16_t time, date; uint32_t at; int bad; };
}

static void put16(std::string& b, unsigned v) { b.push_back((char)v); b.push_back((char)(v >> 8)); }
static void put32(std::string& b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }

static void zip_add(Zip& z, const std::string& name, const std::string& data)
{
    ZipEntry e;
    e.name = name;
    e.usize = (uint32_t)data.size();
    e.crc = crc_of(data);
    e.offset = z.at;
    std::vector<uint8_t> c;
    deflate_fixed(data, c);
    const int store = c.size() >= data.size();
    e.method = store ? 0 : 8;
    e.csize = store ? e.usize : (uint32_t)c.size();
    std::string h;
    put32(h, 0x04034B50u); put16(h, 20); put16(h, 0x0800); put16(h, e.method);
    put16(h, z.time); put16(h, z.date); put32(h, e.crc); put32(h, e.csize); put32(h, e.usize);
    put16(h, (unsigned)name.size()); put16(h, 0);
    h += name;
    if (fwrite(h.data(), 1, h.size(), z.f) != h.size()) z.bad = 1;
    const void* body = store ? (const void*)data.data() : (const void*)c.data();
    if (e.csize && fwrite(body, 1, e.csize, z.f) != e.csize) z.bad = 1;
    z.at += (uint32_t)h.size() + e.csize;
    z.e.push_back(e);
}

static int zip_end(Zip& z)
{
    std::string cd;
    for (const ZipEntry& e : z.e) {
        put32(cd, 0x02014B50u); put16(cd, 20); put16(cd, 20); put16(cd, 0x0800); put16(cd, e.method);
        put16(cd, z.time); put16(cd, z.date); put32(cd, e.crc); put32(cd, e.csize); put32(cd, e.usize);
        put16(cd, (unsigned)e.name.size()); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0);
        put32(cd, e.offset);
        cd += e.name;
    }
    std::string eo;
    put32(eo, 0x06054B50u); put16(eo, 0); put16(eo, 0); put16(eo, (unsigned)z.e.size()); put16(eo, (unsigned)z.e.size());
    put32(eo, (uint32_t)cd.size()); put32(eo, z.at); put16(eo, 0);
    if (fwrite(cd.data(), 1, cd.size(), z.f) != cd.size() || fwrite(eo.data(), 1, eo.size(), z.f) != eo.size()) z.bad = 1;
    if (fclose(z.f)) z.bad = 1;
    return z.bad;
}

/* --- the report ------------------------------------------------------------------- */
/* The player's own progress, without the images and sounds the game puts beside it. */
static void add_saves(Zip& z, std::string& note)
{
    enum : unsigned long long { MOST = 96ull << 20 };
    const char* hdd0 = ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL);
    if (!hdd0 || !*hdd0) { note += "Saves: no [paths] hdd0\n"; return; }
    std::string root = std::string(hdd0) + "\\home\\00000001\\savedata";
    std::replace(root.begin(), root.end(), '/', '\\');
    WIN32_FIND_DATAA d, f;
    HANDLE hd = FindFirstFileA((root + "\\*").c_str(), &d);
    if (hd == INVALID_HANDLE_VALUE) { note += "Saves: none\n"; return; }
    unsigned long long total = 0;
    int files = 0;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == '.') continue;
        const std::string sub = root + "\\" + d.cFileName;
        HANDLE hf = FindFirstFileA((sub + "\\*").c_str(), &f);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const char* ext = strrchr(f.cFileName, '.');
            if (ext && (!_stricmp(ext, ".png") || !_stricmp(ext, ".pam") || !_stricmp(ext, ".at3"))) continue;
            if (total + f.nFileSizeLow > MOST) continue;
            std::string data;
            if (!read_capped(sub + "\\" + f.cFileName, data, MOST, 0)) continue;
            zip_add(z, std::string("save/") + d.cFileName + "/" + f.cFileName, data);
            total += data.size();
            files++;
        } while (FindNextFileA(hf, &f));
        FindClose(hf);
    } while (FindNextFileA(hd, &d));
    FindClose(hd);
    note += strf("Saves: %d files, %llu bytes\n", files, total);
}

int gowa_report_create(int with_saves, char* out, size_t n)
{
    enum { MAX_SESSIONS = 3 };
    crc_init();
    hide_init();
    const std::string dir = logs_dir();
    CreateDirectoryA(dir.c_str(), NULL);
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::string zip_path = dir + strf("\\report-%04u%02u%02u-%02u%02u%02u.zip", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Zip z;
    z.f = fopen(zip_path.c_str(), "wb");
    if (!z.f) return -1;
    z.time = (uint16_t)(st.wHour << 11 | st.wMinute << 5 | st.wSecond / 2);
    z.date = (uint16_t)((st.wYear - 1980) << 9 | st.wMonth << 5 | st.wDay);
    z.at = 0;
    z.bad = 0;

    std::string note = strf("Ascension-R problem report, made %04u-%02u-%02u %02u:%02u:%02u\n\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    note += system_text();
    GameInfo g;
    const char* vfs = ps3_settings_str("paths", "vfs", "PS3_VFS_ROOT", NULL);
    if (vfs && *vfs) {
        const GameState gs = game_probe((wide(vfs) + L"\\PS3_GAME").c_str(), &g);
        if (gs == GAME_INCOMPLETE) {
            wchar_t t[320];
            game_files_text(&g, t, sizeof t / sizeof t[0]);
            note += "Game files: " + narrow(t) + "\n";
        } else if (g.files) {
            note += strf("Game files: all %d as 1.12 has them\n", g.files);
        }
        note += strf("GOWA.elf: %s\n", gs == GAME_OK ? "the one this port is lifted from" : gs == GAME_NO_ELF ? "not there"
                                       : gs == GAME_INCOMPLETE ? "not looked at, the game's files come first"
                                                               : "not the one this port is lifted from");
    }
    note += "\nSessions:\n";

    /* The sessions the game ran in; with none, the newest there is. */
    const std::vector<std::string> logs = session_logs();
    int added = 0;
    for (int pass = 0; pass < 2 && !added; pass++) {
        for (const std::string& name : logs) {
            if (added >= (pass ? 1 : MAX_SESSIONS)) break;
            std::string log, why;
            if (!read_capped(dir + "\\" + name, log, 256u << 10, 8u << 20)) continue;
            const SessionEnd end = session_end_of(log, &why);
            if (!pass && end == SESSION_NONE) continue;
            hide_in(why);
            note += name + ": " + session_end_name(end) + (why.empty() ? "" : ": " + why) + "\n";
            hide_in(log);
            name_frames(log);
            zip_add(z, "logs/" + name, log);
            std::string pad;
            const std::string pad_name = name.substr(0, name.size() - 4) + ".pad";
            if (read_capped(dir + "\\" + pad_name, pad, 1u << 16, 24u << 20) && !pad.empty()) {
                hide_in(pad);
                zip_add(z, "logs/" + pad_name, pad);
            }
            added++;
        }
    }
    if (!added) note += "(no session logs)\n";
    note += "\n";

#ifdef PORT_GAME_MODULE
    /* The Kit: how the game's code was built on this PC, or how far that got (gowa.py kit-build). */
    {
        const std::string d = dir_of(ps3_settings_path());
        std::string blog;
        if (read_capped((d.empty() ? std::string(".") : d) + "\\game-code\\build.log", blog, 64u << 10, 192u << 10) && !blog.empty()) {
            hide_in(blog);
            zip_add(z, "build.log", blog);
            note += "The game's code: build.log\n\n";
        }
    }
#endif
    std::string ini;
    if (read_capped(ps3_settings_path(), ini, 1u << 20, 0) && !ini.empty()) {
        hide_in(ini);
        zip_add(z, "settings.ini", ini);
    }
    std::string events = windows_events();
    if (!events.empty()) {
        hide_in(events);
        zip_add(z, "windows_events.txt", events);
    }
    if (with_saves) add_saves(z, note);
    hide_in(note);
    zip_add(z, "report.txt", note);
    if (zip_end(z)) {
        DeleteFileA(zip_path.c_str());
        return -1;
    }
    snprintf(out, n, "%s", zip_path.c_str());
    return 0;
}

#else   /* not Windows */
void gowa_report_session_start(const char*) {}
void gowa_where(const void*, char* out, size_t n) { if (n) out[0] = 0; }
void gowa_freeze_watch_start(void) {}
SessionEnd gowa_last_session(int* generating) { if (generating) *generating = 0; return SESSION_NONE; }
int gowa_report_create(int, char*, size_t) { return -1; }
#endif
