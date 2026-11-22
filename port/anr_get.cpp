/*
 * Ascension-R -- ANR FG from Apex Compute's own releases, for the launcher's Frame generation choice (anr_get.h)
 *
 * Only when the player says so (the launcher asks first). One GET of the newest release of github.com/apex-compute/ANR
 * from api.github.com, as update_check.cpp asks for ours (WinHTTP, Windows' proxy settings, "Ascension-R/<version>"
 * as the User-Agent and nothing else sent); then its addon zip through game_download, checked against the SHA-256
 * GitHub gives for the file (else the release's SHA256SUMS.txt); the launcher unpacks it and checks Apex Compute's
 * signature on the DLL. ANR comes with no licence: the project keeps nothing of it and fetches it, for a player who asks,
 * only from Apex Compute's own releases.
 */
#ifdef _WIN32   /* Windows only for now: port/posix_stubs.cpp stands in for it elsewhere (docs/MACOS.md) */
#include "anr_get.h"
#include "game_find.h"
#include "version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

static const char k_repo_downloads[] = "https://github.com/apex-compute/ANR/releases/download/";

/* A small HTTPS GET (an API answer, a list of sums), redirects followed. 1 with the body in out. */
static int https_get(const wchar_t* host, const wchar_t* path, std::string& out, size_t most, DWORD* status_out)
{
    wchar_t agent[64];
    swprintf(agent, 64, L"Ascension-R/%hs", AR_VERSION);
    *status_out = 0;
    HINTERNET ses = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 10000, 10000, 10000, 15000);
    HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE)
                        : NULL;
    int ok = 0;
    DWORD status = 0, size = sizeof status;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n",
                                  (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        *status_out = status;
        if (status == 200) {
            ok = 1;
            for (;;) {
                DWORD ready = 0, got = 0;
                if (!WinHttpQueryDataAvailable(req, &ready)) { ok = 0; break; }
                if (!ready) break;
                if (out.size() + ready > most) { ok = 0; break; }
                const size_t at = out.size();
                out.resize(at + ready);
                if (!WinHttpReadData(req, &out[at], ready, &got)) { ok = 0; break; }
                out.resize(at + got);
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

/* The value of the first "key": "..." in a piece of JSON, with its escapes undone (none of these values has more
 * than \" \\ \/). "" when there is none. */
static std::string json_string(const std::string& j, const char* key)
{
    const std::string k = std::string("\"") + key + "\"";
    size_t at = j.find(k);
    while (at != std::string::npos) {
        size_t p = at + k.size();
        while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) p++;
        if (p < j.size() && j[p] == ':') {
            p++;
            while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) p++;
            if (p >= j.size() || j[p] != '"') return std::string();
            std::string v;
            for (p++; p < j.size() && j[p] != '"'; p++) {
                if (j[p] == '\\' && p + 1 < j.size()) p++;
                v += j[p];
            }
            return v;
        }
        at = j.find(k, at + 1);
    }
    return std::string();
}

static long long json_number(const std::string& j, const char* key)
{
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = j.find(k);
    return at == std::string::npos ? -1 : atoll(j.c_str() + at + k.size());
}

/* The objects of the release's "assets" array, as text each. */
static std::vector<std::string> json_assets(const std::string& j)
{
    std::vector<std::string> out;
    size_t p = j.find("\"assets\"");
    if (p == std::string::npos) return out;
    p = j.find('[', p);
    if (p == std::string::npos) return out;
    int depth = 0;
    bool str = false;
    size_t start = 0;
    for (p++; p < j.size(); p++) {
        const char c = j[p];
        if (str) {
            if (c == '\\') p++;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == '{') { if (depth++ == 0) start = p; }
        else if (c == '}') { if (--depth == 0) out.push_back(j.substr(start, p - start + 1)); }
        else if (c == ']' && depth == 0) break;
    }
    return out;
}

static bool ends_with(const std::string& s, const char* t)
{
    const size_t n = strlen(t);
    return s.size() >= n && !_stricmp(s.c_str() + s.size() - n, t);
}

static std::wstring wide(const std::string& s)
{
    std::wstring w(s.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
    w.resize(n > 0 ? (size_t)n - 1 : 0);
    return w;
}

static std::string lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
    return s;
}

/* The SHA-256 a release's SHA256SUMS.txt lists for name ("<hex>  <name>" or "<hex> *<name>"), or "". */
static std::string sum_for(const std::string& sums, const std::string& name)
{
    size_t at = 0;
    while (at < sums.size()) {
        size_t end = sums.find('\n', at);
        if (end == std::string::npos) end = sums.size();
        std::string l = sums.substr(at, end - at);
        at = end + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.size() < 66) continue;
        std::string n = l.substr(64);
        while (!n.empty() && (n[0] == ' ' || n[0] == '*')) n.erase(0, 1);
        if (n == name) return lower(l.substr(0, 64));
    }
    return std::string();
}

std::string anr_latest(AnrRelease* r)
{
    *r = AnrRelease();
    std::string body;
    DWORD status = 0;
    if (!https_get(L"api.github.com", L"/repos/apex-compute/ANR/releases/latest", body, 4u << 20, &status))
        return status ? "GitHub answered " + std::to_string(status) + " for ANR's newest release"
                      : "GitHub could not be reached: check the connection";
    r->tag = json_string(body, "tag_name");
    const std::vector<std::string> assets = json_assets(body);
    const std::string* pick = NULL;
    for (int want = 0; want < 2 && !pick; want++)
        for (const std::string& a : assets) {
            const std::string n = json_string(a, "name");
            if (!_strnicmp(n.c_str(), "anr-fg-", 7) && ends_with(n, want ? "-full.zip" : "-addon.zip")) { pick = &a; break; }
        }
    if (r->tag.empty() || !pick) return "ANR's newest release (" + (r->tag.empty() ? std::string("?") : r->tag) + ") has no ANR FG zip";
    r->name = json_string(*pick, "name");
    const std::string url = json_string(*pick, "browser_download_url");
    /* Only from Apex Compute's own releases. */
    if (url.compare(0, sizeof k_repo_downloads - 1, k_repo_downloads)) return "the zip's address is not ANR's own releases";
    r->url = wide(url);
    r->size = json_number(*pick, "size");
    const std::string digest = json_string(*pick, "digest");
    if (!_strnicmp(digest.c_str(), "sha256:", 7) && digest.size() == 71) r->sha256 = lower(digest.substr(7));
    if (r->sha256.empty()) {
        /* Older answers have no digest: the release's own list of sums. */
        for (const std::string& a : assets) {
            if (json_string(a, "name") != "SHA256SUMS.txt") continue;
            const std::string su = json_string(a, "browser_download_url");
            if (su.compare(0, sizeof k_repo_downloads - 1, k_repo_downloads)) break;
            std::string sums;
            if (https_get(L"github.com", wide(su.substr(18)).c_str(), sums, 1u << 16, &status)) r->sha256 = sum_for(sums, r->name);
        }
    }
    if (r->sha256.size() != 64) return "no SHA-256 to check " + r->name + " against";
    return std::string();
}

std::string anr_fetch(const AnrRelease& r, const std::wstring& file, GameProgress* p)
{
    wchar_t err[400] = L"";
    if (!game_download(r.url.c_str(), file.c_str(), L"ANR FG", p, err, 400)) {
        DeleteFileW(file.c_str());
        char e[800];
        WideCharToMultiByte(CP_UTF8, 0, err, -1, e, sizeof e, NULL, NULL);
        return e[0] ? std::string(e) : std::string("the download failed");
    }
    char hex[65];
    if (!game_file_sha256(file.c_str(), hex) || lower(hex) != r.sha256) {
        DeleteFileW(file.c_str());
        return "the download is not the file GitHub lists (its SHA-256 differs)";
    }
    return std::string();
}

bool anr_signer(const std::wstring& path, std::wstring* who)
{
    who->clear();
    WINTRUST_FILE_INFO fi = {};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = {};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    const LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (r == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : NULL;
        CRYPT_PROVIDER_CERT* pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : NULL;
        wchar_t name[160] = L"";
        if (pc && pc->pCert) CertGetNameStringW(pc->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 160);
        *who = name;
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    return r == ERROR_SUCCESS && !who->empty();
}

std::string anr_version_of(const std::string& s)
{
    size_t at = s.find("V.");
    if (at == std::string::npos) return std::string();
    size_t end = at + 2;
    while (end < s.size() && ((s[end] >= 'A' && s[end] <= 'Z') || (s[end] >= '0' && s[end] <= '9') || s[end] == '.')) end++;
    while (end > at + 2 && s[end - 1] == '.') end--;
    return end > at + 2 ? s.substr(at, end - at) : std::string();
}

#endif /* _WIN32 */
