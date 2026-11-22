/*
 * Ascension-R -- multiplayer from the launcher's settings (docs/notes/2026-11-22-multiplayer-phase5.md).
 *
 * [online]
 *   mode       off | auto | host | join
 *   name       the player's online id: 3-16 letters, digits, - and _, starting with a letter
 *   server     join: the host's address (a name or an IPv4 address)
 *   relay      off | on: the game's datagrams always through the host's relay
 *   port       the UDP port other players reach this game at (3658)
 *   game_mode  host: every Quick Play match in this game mode (empty: as the players' searches suggest)
 *   players    host: Quick Play waits for this many players and puts them all in one world
 *   upnp       auto: on | off, the node asks the router to forward its port
 *   relay_others  auto: on | off, the node carries for players who cannot reach each other
 *   nodes      auto: community nodes, host[:port] with a comma between
 *   lan, dht   auto: on | off, the ways the node finds the others (tests turn them off)
 *
 * gowa_online_start runs scripts\mp_node.py with the Kit's Python (kit\ beside the program; in a build tree, scripts\
 * beside build\ with the .venv's Python or one on the PATH; GOWA_PYTHON names another) in a job that ends with this
 * process, and waits for the node's game.env: the variables the runtime's network and NP read, put into this
 * environment before anything reads them. host leaves the node running: the matchmaker and the signaling server for
 * this game and the players who join it; auto leaves the node of multiplayer with no host running (mp_p2p.py), which
 * finds the other players' nodes itself (the BitTorrent DHT, this network) and is this game's matchmaker and signaling
 * server (docs/notes/2026-11-22-multiplayer-phase6.md). The node's folder is online\ beside the settings: tss.bin, np_tus\ (the
 * multiplayer profile), node.log, and on auto players.txt, the players it knows of (the launcher's Online now).
 *
 * On auto the launcher starts the node as soon as it shows the settings (gowa_online_begin, on a thread of its own,
 * a message to the window when it is up or could not be), starts it again when a setting it takes changes, and stops
 * it when Multiplayer is no longer on auto (gowa_online_stop: the node ends and its variables leave the environment).
 * Play then finds it running (gowa_online_start waits for one that is starting). Host and join start at Play.
 */
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32   /* Windows only for now: port/posix_stubs.cpp stands in for it elsewhere (docs/MACOS.md) */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "ps3emu/ps3_settings.h"
#pragma comment(lib, "iphlpapi.lib")

namespace {

std::wstring wide(const char* s)
{
    if (!s || !*s) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 1) return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

bool is_file(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring full(const std::wstring& p)
{
    wchar_t b[MAX_PATH * 2];
    const DWORD k = GetFullPathNameW(p.c_str(), MAX_PATH * 2, b, NULL);
    return k && k < MAX_PATH * 2 ? std::wstring(b) : p;
}

std::wstring exe_dir(void)
{
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return L".";
    if (wchar_t* e = wcsrchr(exe, L'\\')) *e = 0;
    return exe;
}

/* The settings file's folder, else the program's (mods.cpp's rule). */
std::wstring settings_dir(void)
{
    std::wstring here = wide(ps3_settings_path());
    for (wchar_t& c : here) if (c == L'/') c = L'\\';
    const size_t s = here.find_last_of(L'\\');
    return s != std::wstring::npos ? here.substr(0, s) : exe_dir();
}

/* The Python and mp_node.py to run: the Kit's, else a build tree's (mods.cpp's find_packer rule). */
bool find_node(std::wstring* py, std::wstring* script)
{
    const std::wstring here = exe_dir();
    if (is_file(here + L"\\kit\\python\\python.exe") && is_file(here + L"\\kit\\scripts\\mp_node.py")) {
        *py = here + L"\\kit\\python\\python.exe";
        *script = here + L"\\kit\\scripts\\mp_node.py";
    } else if (is_file(here + L"\\..\\scripts\\mp_node.py")) {
        *script = full(here + L"\\..\\scripts\\mp_node.py");
        if (is_file(here + L"\\..\\..\\.venv\\Scripts\\python.exe")) *py = full(here + L"\\..\\..\\.venv\\Scripts\\python.exe");
        else {
            wchar_t found[MAX_PATH];
            if (SearchPathW(NULL, L"python.exe", NULL, MAX_PATH, found, NULL)) *py = found;
        }
    }
    if (const wchar_t* e = _wgetenv(L"GOWA_PYTHON")) if (*e) *py = e;
    return !py->empty() && !script->empty() && is_file(*py) && is_file(*script);
}

const char* setting(const char* key) { return ps3_settings_str("online", key, NULL, ""); }

/* A host's address as the player typed it: a name or an IPv4 address, nothing that could end the command line. */
bool address_ok(const char* s)
{
    size_t n = 0;
    for (; s[n]; n++) {
        const char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;
    }
    return n > 0 && n <= 253;
}

/* Community nodes as the launcher takes them: host[:port] items, a comma between; "" is none. */
bool nodes_ok(const char* s)
{
    for (size_t i = 0; s[i]; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == ':' || c == ','))
            return false;
    }
    return strlen(s) <= 253;
}

/* A number setting, "" when it is not one (then left out). */
std::wstring number(const char* key, long lo, long hi)
{
    const char* v = setting(key);
    if (!*v) return std::wstring();
    char* end = NULL;
    const long k = strtol(v, &end, 10);
    if (*end || k < lo || k > hi) return std::wstring();
    return std::to_wstring(k);
}

std::string read_small(const std::wstring& path, size_t cap)
{
    std::string s;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return s;
    char buf[4096];
    DWORD got = 0;
    while (s.size() < cap && ReadFile(h, buf, sizeof buf, &got, NULL) && got) s.append(buf, got);
    CloseHandle(h);
    return s;
}

/* The last line of a text that is not empty: a Python's own complaint, when the node could not say why. */
std::string last_line(const std::string& s)
{
    size_t end = s.find_last_not_of("\r\n \t");
    if (end == std::string::npos) return std::string();
    const size_t start = s.find_last_of('\n', end);
    return s.substr(start == std::string::npos ? 0 : start + 1, end + 1 - (start == std::string::npos ? 0 : start + 1));
}

/* What a node is started with: the settings as they were, so a thread can start it while they change. sig holds them
 * all: a node started with another sig is not the one the settings ask for now. */
struct Args {
    int mode = 0;
    std::string name, server, nodes, elf, sig;
    bool relay = false, upnp = true, relay_others = true, lan = true, dht = true;
    std::wstring port, game_mode, players, dir;
};

SRWLOCK s_lock = SRWLOCK_INIT;
int s_state;                     /* 0 none (or it failed), 1 running (its variables in), 2 starting */
std::string s_sig;               /* the settings of the node running or starting */
HANDLE s_job;                    /* the node's: closed only by gowa_online_stop, so the node ends with this process */
std::vector<std::string> s_vars; /* the variables its game.env put in */
char s_why[300];                 /* why the last start failed */
HANDLE s_worker;                 /* the thread of a start under way */
volatile LONG s_cancel;          /* that start is no longer wanted */

int fail(char* why, size_t n, const char* fmt, const char* a = "")
{
    char w[300];
    snprintf(w, sizeof w, fmt, a);
    AcquireSRWLockExclusive(&s_lock);
    snprintf(s_why, sizeof s_why, "%s", w);
    ReleaseSRWLockExclusive(&s_lock);
    if (why && n) snprintf(why, n, "%s", w);
    fprintf(stderr, "[online] %s\n", w);
    return 0;
}

int mode_of(const char* m) { return !_stricmp(m, "host") ? 1 : !_stricmp(m, "join") ? 2 : !_stricmp(m, "auto") ? 3 : 0; }

}  // namespace

/* 0 off, 1 host, 2 join, 3 auto. */
extern "C" int gowa_online_mode(void)
{
    return mode_of(setting("mode"));
}

/* The runtime's rule for an online id (np_psnr.c), taken as it is: 1 if name is one. */
extern "C" int gowa_online_name_ok(const char* name)
{
    size_t n = 0;
    if (!name) return 0;
    for (; name[n]; n++) {
        const char c = name[n];
        const int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!letter && !(c >= '0' && c <= '9') && c != '-' && c != '_') return 0;
        if (n == 0 && !letter) return 0;
    }
    return n >= 3 && n <= 16;
}

/* A host's address as the launcher takes it: a name or an IPv4 address. */
extern "C" int gowa_online_address_ok(const char* s) { return s && address_ok(s); }

/* The community nodes setting as the launcher takes it. */
extern "C" int gowa_online_nodes_ok(const char* s) { return s && nodes_ok(s); }

/* What stops multiplayer from starting with these settings, or NULL: the launcher says it before Play (a toast, so
 * short). */
extern "C" const char* gowa_online_problem(void)
{
    const int mode = gowa_online_mode();
    if (!mode) return NULL;
    if (!gowa_online_name_ok(setting("name"))) return "Multiplayer: set your online ID first";
    if (mode == 2 && !address_ok(setting("server"))) return "Multiplayer: set the host's address first";
    if (mode == 3 && !nodes_ok(setting("nodes"))) return "Multiplayer: the community nodes are not host:port";
    std::wstring py, script;
    if (!find_node(&py, &script)) return "Multiplayer needs the Kit's Python";
    return NULL;
}

/* This PC's IPv4 addresses that other players could reach, each with its network's name ("192.168.1.20 (Wi-Fi)"),
 * ", " between them; "" when there is none. Loopback and self-assigned (169.254) ones are left out. */
extern "C" void gowa_online_lan(char* out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    ULONG size = 16384;
    std::string buf;
    DWORD r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 NULL, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(&buf[0]), &size);
    }
    if (r != NO_ERROR) return;
    std::string list;
    int count = 0;
    for (const IP_ADAPTER_ADDRESSES* a = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buf.data()); a && count < 4;
         a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u && count < 4; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            if (!sa || sa->sa_family != AF_INET) continue;
            const unsigned char* b = reinterpret_cast<const unsigned char*>(
                &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr);
            if (b[0] == 127 || (b[0] == 169 && b[1] == 254)) continue;
            char ip[64], name[96] = "";
            snprintf(ip, sizeof ip, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (a->FriendlyName)
                WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, (int)sizeof name, NULL, NULL);
            if (!list.empty()) list += ", ";
            list += ip;
            if (name[0]) list += std::string(" (") + name + ")";
            count++;
        }
    }
    snprintf(out, n, "%s", list.c_str());
}

namespace {

/* The settings now, on the thread that may read them (the launcher's, or main's). */
Args args_now(void)
{
    Args a;
    a.mode = gowa_online_mode();
    a.name = setting("name");
    a.server = setting("server");
    a.nodes = setting("nodes");
    const char* elf = ps3_settings_str("paths", "elf", NULL, NULL);
    a.elf = elf ? elf : "";
    a.relay = ps3_settings_bool("online", "relay", NULL, 0) != 0;
    a.upnp = ps3_settings_bool("online", "upnp", NULL, 1) != 0;
    a.relay_others = ps3_settings_bool("online", "relay_others", NULL, 1) != 0;
    a.lan = ps3_settings_bool("online", "lan", NULL, 1) != 0;
    a.dht = ps3_settings_bool("online", "dht", NULL, 1) != 0;
    a.port = number("port", 1024, 65535);
    a.game_mode = number("game_mode", 1, 255);
    a.players = number("players", 1, 16);
    a.dir = settings_dir() + L"\\online";
    char sig[1200];
    snprintf(sig, sizeof sig, "%d|%s|%s|%s|%s|%d%d%d%d%d|%ls|%ls|%ls", a.mode, a.name.c_str(), a.server.c_str(),
             a.nodes.c_str(), a.elf.c_str(), a.relay, a.upnp, a.relay_others, a.lan, a.dht, a.port.c_str(),
             a.game_mode.c_str(), a.players.c_str());
    a.sig = sig;
    return a;
}

/* The node gone: asked to stop (node.stop: it says goodbye to the other nodes and ends), ended after a second
whatever it did, its variables out of the environment. The lock is held. */
void stop_locked(void)
{
    if (s_job) {
        const std::wstring stop = settings_dir() + L"\\online\\node.stop";
        HANDLE f = CreateFileW(stop.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        for (int t = 0; t < 20; t++) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai = {};
            if (!QueryInformationJobObject(s_job, JobObjectBasicAccountingInformation, &ai, sizeof ai, NULL) ||
                !ai.ActiveProcesses)
                break;
            Sleep(50);
        }
        TerminateJobObject(s_job, 0);
        CloseHandle(s_job);
        s_job = NULL;
    }
    for (const std::string& v : s_vars) _putenv_s(v.c_str(), "");
    s_vars.clear();
    s_state = 0;
    s_sig.clear();
}

/* A start under way, called off and waited for. */
void cancel_worker(void)
{
    AcquireSRWLockExclusive(&s_lock);
    HANDLE w = s_worker;
    if (w) InterlockedExchange(&s_cancel, 1);
    ReleaseSRWLockExclusive(&s_lock);
    if (w) WaitForSingleObject(w, 35000);
}

void stop_at_exit(void);

/* The node of a, started, and its variables put in: 1, or 0 and why. Any thread. */
int start_with(const Args& a, char* why, size_t n)
{
    if (a.elf.empty() || !is_file(wide(a.elf.c_str()))) return fail(why, n, "Multiplayer: no GOWA.elf where the settings say");
    std::wstring py, script;
    if (!find_node(&py, &script)) return fail(why, n, "Multiplayer needs the Kit's Python");
    const int mode = a.mode;
    const std::wstring dir = a.dir;
    CreateDirectoryW(dir.c_str(), NULL);
    const std::wstring env_file = dir + L"\\game.env", err_file = dir + L"\\node.error", out_file = dir + L"\\node.out";
    DeleteFileW(env_file.c_str());
    DeleteFileW(err_file.c_str());

    std::wstring cmd = L"\"" + py + L"\" -E -s -B -X utf8 \"" + script + L"\" " + (mode == 1 ? L"host" : mode == 2 ? L"join" : L"auto") +
                       L" --name " + wide(a.name.c_str()) + L" --elf \"" + wide(a.elf.c_str()) + L"\" --dir \"" + dir + L"\"";
    if (mode == 2) cmd += L" --server " + wide(a.server.c_str());
    if (a.relay) cmd += L" --relay";
    if (!a.port.empty()) cmd += L" --p2p-port " + a.port;
    if (mode == 3) {
        if (!a.nodes.empty()) cmd += L" --nodes " + wide(a.nodes.c_str());
        if (!a.upnp) cmd += L" --no-upnp";
        if (!a.relay_others) cmd += L" --no-relay-others";
        if (!a.lan) cmd += L" --no-lan";
        if (!a.dht) cmd += L" --no-dht";
    }
    if (mode == 1) {
        if (!a.game_mode.empty()) cmd += L" --mode " + a.game_mode;
        if (!a.players.empty()) cmd += L" --players " + a.players;
    }

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out = CreateFileW(out_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             NULL);
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) {
        if (job) CloseHandle(job);
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        return fail(why, n, "Multiplayer: the node could not be started");
    }
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    if (out != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = out;
        si.hStdInput = NULL;
    }
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(py.c_str(), &cmd[0], NULL, NULL, out != INVALID_HANDLE_VALUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, NULL, dir.c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (ok && !AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ok = FALSE;
    }
    if (!ok) {
        CloseHandle(job);
        return fail(why, n, "Multiplayer: the node could not be started");
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    /* Its game.env, or its end: about a second (Python, the TSS); 30 at most, and not past a cancel. */
    std::string env;
    for (int t = 0; t < 600 && !s_cancel; t++) {
        if (is_file(env_file)) {
            env = read_small(env_file, 1 << 16);
            if (!env.empty()) break;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0 && !is_file(env_file)) break;
    }
    if (s_cancel) {
        TerminateJobObject(job, 1);
        CloseHandle(job);
        CloseHandle(pi.hProcess);
        return fail(why, n, "Multiplayer: the start was called off");
    }
    if (env.empty()) {
        std::string w = last_line(read_small(err_file, 4096));
        if (w.empty()) w = last_line(read_small(out_file, 1 << 16));
        TerminateJobObject(job, 1);
        CloseHandle(job);
        CloseHandle(pi.hProcess);
        return fail(why, n, "Multiplayer: %s", w.empty() ? "the node gave no answer (online\\node.log)" : w.c_str());
    }
    CloseHandle(pi.hProcess);

    AcquireSRWLockExclusive(&s_lock);
    s_job = job;   /* kept: the node ends with this process, or with gowa_online_stop */
    size_t at = 0;
    int vars = 0;
    while (at < env.size()) {
        size_t end = env.find('\n', at);
        if (end == std::string::npos) end = env.size();
        std::string line = env.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || !eq) continue;
        _putenv_s(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str());   /* "" takes it away */
        s_vars.push_back(line.substr(0, eq));
        vars++;
    }
    s_state = 1;
    s_sig = a.sig;
    s_why[0] = 0;
    static LONG at_exit;
    if (!InterlockedExchange(&at_exit, 1)) atexit(stop_at_exit);
    ReleaseSRWLockExclusive(&s_lock);
    printf("[online] %s as %s%s%s: %d variables from %ls\n",
           mode == 1 ? "hosting" : mode == 2 ? "joining" : "finding players", a.name.c_str(),
           mode == 2 ? " " : "", mode == 2 ? a.server.c_str() : "", vars, env_file.c_str());
    return 1;
}

/* At the program's end (the launcher's Quit, the game's): the node says goodbye. A crash or a kill leaves that to the
other nodes' minute of silence. */
void stop_at_exit(void)
{
    AcquireSRWLockExclusive(&s_lock);
    stop_locked();
    ReleaseSRWLockExclusive(&s_lock);
}

struct Begin {
    Args a;
    HWND notify;
    UINT msg;
};

DWORD WINAPI begin_thread(void* p)
{
    Begin* b = static_cast<Begin*>(p);
    char why[300];
    const int ok = start_with(b->a, why, sizeof why);
    AcquireSRWLockExclusive(&s_lock);
    if (!ok && s_state == 2) {
        s_state = 0;
        s_sig.clear();
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (b->notify) PostMessageW(b->notify, b->msg, (WPARAM)ok, 0);
    delete b;
    return 0;
}

}  // namespace

/* The node gone, whatever it was doing: a start under way called off, a running one ended, its variables taken out of
 * the environment. */
extern "C" void gowa_online_stop(void)
{
    cancel_worker();
    AcquireSRWLockExclusive(&s_lock);
    if (s_worker) {
        CloseHandle(s_worker);
        s_worker = NULL;
    }
    stop_locked();
    ReleaseSRWLockExclusive(&s_lock);
}

/* The launcher's: on auto, the node of the settings as they are now started on a thread (a message to notify when it
 * is up or could not be), unless that one runs or starts already; any other node stopped. Nothing on host, join or
 * off but the stop: those start at Play. */
extern "C" void gowa_online_begin(void* notify, unsigned msg)
{
    Args a = args_now();
    AcquireSRWLockShared(&s_lock);
    const bool same = (s_state == 1 || s_state == 2) && s_sig == a.sig;
    ReleaseSRWLockShared(&s_lock);
    if (same) return;
    gowa_online_stop();
    if (a.mode != 3 || gowa_online_problem()) return;
    Begin* b = new Begin{ a, static_cast<HWND>(notify), msg };
    AcquireSRWLockExclusive(&s_lock);
    s_state = 2;
    s_sig = a.sig;
    s_why[0] = 0;
    InterlockedExchange(&s_cancel, 0);
    s_worker = CreateThread(NULL, 0, begin_thread, b, 0, NULL);
    if (!s_worker) {
        s_state = 0;
        s_sig.clear();
        delete b;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

/* What the node is doing: 0 none, 1 running, 2 starting, -1 the last start failed (why says why). */
extern "C" int gowa_online_status(char* why, size_t n)
{
    AcquireSRWLockShared(&s_lock);
    const int st = s_state ? s_state : s_why[0] ? -1 : 0;
    if (why && n) snprintf(why, n, "%s", s_why);
    ReleaseSRWLockShared(&s_lock);
    return st;
}

/* The node's players.txt (auto): "me\t<reachable>\t<state>\t<count>" and a "player\t<state>\t<how>\t<name>" line
 * for each player it knows of; "" when there is none. */
extern "C" void gowa_online_players(char* out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    AcquireSRWLockShared(&s_lock);
    const bool running = s_state == 1;
    ReleaseSRWLockShared(&s_lock);
    if (!running) return;
    const std::string text = read_small(settings_dir() + L"\\online\\players.txt", n - 1);
    snprintf(out, n, "%s", text.c_str());
}

/* Multiplayer as the settings say: the node started and its variables in this environment. 1 when that is done (or
 * a node the launcher started is that one) or multiplayer is off; 0 and why when it could not be (the game then
 * plays without it). */
extern "C" int gowa_online_start(char* why, size_t n)
{
    Args a = args_now();
    AcquireSRWLockShared(&s_lock);
    HANDLE w = s_worker;
    ReleaseSRWLockShared(&s_lock);
    if (w) WaitForSingleObject(w, 35000);     /* the launcher's start, under way: its end */
    AcquireSRWLockExclusive(&s_lock);
    if (s_worker) {
        CloseHandle(s_worker);
        s_worker = NULL;
    }
    const bool same = s_state == 1 && s_sig == a.sig;
    if (!same) stop_locked();
    ReleaseSRWLockExclusive(&s_lock);
    if (same || !a.mode) return 1;
    if (const char* p = gowa_online_problem()) return fail(why, n, "%s", p);
    return start_with(a, why, n);
}

#endif /* _WIN32 */
