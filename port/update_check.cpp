/*
 * Ascension-R -- asking GitHub for a newer release, for the launcher (update_check.h)
 *
 * One GET of the repository's releases from api.github.com, with WinHTTP as game_prepare.cpp's downloads (Windows'
 * proxy settings, TLS), on a thread of its own: the launcher never waits for it, and a PC with no network sees
 * nothing. GitHub lets a client with no account ask 60 times an hour; this asks at most once every six hours, and
 * says who it is in its User-Agent ("Ascension-R/<version>") as GitHub wants, and nothing else.
 *
 * update_check.txt, beside the settings file, keeps what the last answer said, so the notice shows at once at the next
 * start and the settings file is not written behind the player's back:
 *     checked <unix seconds>
 *     latest <tag> <page>          (when a newer release was out)
 *     dismissed <tag>              (its notice closed)
 */
#include "update_check.h"
#include "version.h"
#include "ps3emu/ps3_settings.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>

#pragma comment(lib, "winhttp.lib")

static const long long k_every = 6 * 3600;   /* seconds between two questions */
static const size_t k_most = 8u << 20;       /* a list longer than this is not read */

static HANDLE s_thread;
static HWND s_hwnd;
static UINT s_msg;
static void* volatile s_request;   /* the request under way, for update_check_end to drop */
static UpdateRelease s_found;      /* the thread's answer, read on the launcher's thread at its message */

static std::string cache_path(void)
{
    const char* sp = ps3_settings_path();
    std::string p = sp ? sp : "";
    const size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "update_check.txt";
}

struct Cache {
    long long checked;
    UpdateRelease latest;   /* tag[0] == 0: none */
    char dismissed[48];
};

static void cache_read(Cache* c)
{
    memset(c, 0, sizeof *c);
    FILE* f = fopen(cache_path().c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char a[64], b[300];
        long long t;
        if (sscanf(line, "checked %lld", &t) == 1) c->checked = t;
        else if (sscanf(line, "latest %47s %255s", a, b) == 2) {
            snprintf(c->latest.tag, sizeof c->latest.tag, "%s", a);
            snprintf(c->latest.url, sizeof c->latest.url, "%s", b);
        } else if (sscanf(line, "dismissed %47s", a) == 1) snprintf(c->dismissed, sizeof c->dismissed, "%s", a);
    }
    fclose(f);
}

static void cache_write(const Cache& c)
{
    const std::string p = cache_path(), tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "checked %lld\n", c.checked);
    if (c.latest.tag[0]) fprintf(f, "latest %s %s\n", c.latest.tag, c.latest.url);
    if (c.dismissed[0]) fprintf(f, "dismissed %s\n", c.dismissed);
    const int ok = !ferror(f);
    if (fclose(f) || !ok || !MoveFileExA(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp.c_str());
}

/* A release from the file still worth saying: newer than this one, with its page where it should be. */
static int still_newer(const UpdateRelease& r)
{
    ArVersion v, cur;
    return r.tag[0] && update_version_parse(r.tag, &v) && update_version_parse(AR_VERSION, &cur) &&
           update_version_cmp(v, cur) > 0 && !strncmp(r.url, UPDATE_PAGE_PREFIX, sizeof UPDATE_PAGE_PREFIX - 1);
}

static int read_file(const char* path, std::string& out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < k_most) out.append(buf, got);
    fclose(f);
    return out.size() < k_most;
}

static int fetch(std::string& out)
{
    wchar_t agent[64];
    swprintf(agent, 64, L"Ascension-R/%hs", AR_VERSION);
    HINTERNET ses = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
    HINTERNET con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"/repos/rigaz29/Ascension-R/releases?per_page=30", NULL,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                        : NULL;
    int ok = 0;
    if (req) {
        InterlockedExchangePointer(&s_request, req);
        DWORD status = 0, size = sizeof status;
        if (WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                               (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, NULL) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            ok = 1;
            for (;;) {
                DWORD ready = 0, got = 0;
                if (!WinHttpQueryDataAvailable(req, &ready)) { ok = 0; break; }
                if (!ready) break;
                if (out.size() + ready > k_most) { ok = 0; break; }
                const size_t at = out.size();
                out.resize(at + ready);
                if (!WinHttpReadData(req, &out[at], ready, &got)) { ok = 0; break; }
                out.resize(at + got);
            }
        }
        if (InterlockedExchangePointer(&s_request, NULL)) WinHttpCloseHandle(req);
        else ok = 0;   /* dropped by update_check_end */
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static DWORD WINAPI check_thread(void*)
{
    std::string body;
    const char* feed = getenv("GOWA_UPDATE_FEED");
    const int got = feed && *feed ? read_file(feed, body) : fetch(body);
    UpdateRelease r = {};
    const int st = got ? update_pick(body.data(), body.size(), AR_VERSION, &r) : -1;
    s_found = r;
    PostMessageA(s_hwnd, s_msg, (WPARAM)(st + 1), 0);   /* 0 failed, 1 none newer, 2 found */
    return 0;
}

void update_check_begin(void* hwnd, unsigned msg, UpdateState* st)
{
    memset(st, 0, sizeof *st);
    Cache c;
    cache_read(&c);
    if (still_newer(c.latest)) {
        st->known = 1;
        st->rel = c.latest;
    }
    snprintf(st->dismissed, sizeof st->dismissed, "%s", c.dismissed);
    const char* feed = getenv("GOWA_UPDATE_FEED");
    const long long now = (long long)time(NULL);
    if (!(feed && *feed) && c.checked <= now && now - c.checked < k_every) return;   /* asked not long ago */
    if (s_thread) return;
    s_hwnd = (HWND)hwnd;
    s_msg = msg;
    s_thread = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
}

void update_check_done(UpdateState* st, unsigned long long wparam)
{
    if (s_thread) {
        WaitForSingleObject(s_thread, INFINITE);   /* it has posted: it is ending */
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    const int status = (int)wparam - 1;
    if (status < 0) return;   /* no answer: asked again at the next start */
    st->known = status == 1;
    if (st->known) st->rel = s_found;
    Cache c;
    cache_read(&c);
    c.checked = (long long)time(NULL);
    if (st->known) c.latest = st->rel;
    else memset(&c.latest, 0, sizeof c.latest);
    snprintf(c.dismissed, sizeof c.dismissed, "%s", st->dismissed);
    cache_write(c);
}

void update_check_dismiss(UpdateState* st)
{
    snprintf(st->dismissed, sizeof st->dismissed, "%s", st->rel.tag);
    Cache c;
    cache_read(&c);
    snprintf(c.dismissed, sizeof c.dismissed, "%s", st->dismissed);
    cache_write(c);
}

void update_check_end(void)
{
    if (!s_thread) return;
    if (void* r = InterlockedExchangePointer(&s_request, NULL)) WinHttpCloseHandle(r);   /* its wait ends */
    WaitForSingleObject(s_thread, 3000);
    CloseHandle(s_thread);
    s_thread = NULL;
}
