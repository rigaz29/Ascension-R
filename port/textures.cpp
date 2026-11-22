/*
 * Texture mods for the D3D12 backend's hooks (textures.h, ps3recomp's rsx_tex_replace.h). The pictures are read
 * and the dump is written with Windows' own imaging (WIC).
 */
#ifdef _WIN32   /* Windows only for now: port/posix_stubs.cpp stands in for it elsewhere (docs/MACOS.md) */
#include "textures.h"
#include "mods.h"

#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ps3emu/ps3_settings.h"
#include "rsx_tex_replace.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

struct Pic {
    std::wstring path;
    int kind;     /* 1 in the texture's place, 2 over the game's own */
    /* A copy's: the size of the copy the picture is for (another id of the same texture of the game's), which a
     * smaller copy has the picture scaled down by. 0 for a picture of its own. */
    uint32_t ref_w = 0, ref_h = 0;
};

/* The textures the game keeps in more than one copy (scripts/texture_copies.py): a whole one, in an archive's stream
 * or a WAD, and the levels a stream does not hold, kept apart in a level's WAD from level 2 down, which the game draws
 * with as a texture of their own size until the stream's arrive (all along in some cinematics). Each reaches the GPU
 * with its own id, so a picture for one copy goes to the others too. */
struct TexCopy { uint64_t id; uint32_t w, h, group; };
#include "texture_copies.inc"

/* A texture for the dump's thread to write. */
struct DumpJob {
    std::wstring path;
    uint32_t w, h;
    std::vector<uint8_t> rgba;
};

/* Pictures decoded once. A mod gives one picture to many textures, under a file of each one's name: a character's
 * atlas to every texture of the character, 9 pictures in 35 files. Each upload of each of those textures read its
 * file and decoded it again, and an upload comes again whenever the texture cache lets a texture go and it is bound
 * again: with 4096x4096 atlases, 9-53 ms a frame of decoding on the render thread for the first minute of a level.
 * A file is known by its size and time, and its bytes' XXH64 once read; its texels are kept by that, and a smaller
 * copy's by that and its size, the least recently used going past GOWA_PICTURE_CACHE_MB (default 512). The same
 * texels also get one key for the backend (rsx_tex_picture.key), which uploads them once for all the textures. */
struct Decoded {
    uint32_t w = 0, h = 0;
    std::shared_ptr<std::vector<uint8_t>> rgba;
    uint64_t used = 0;                 /* the tick of its last use */
};
struct FileSeen { uint64_t size = 0, time = 0, content = 0; };

struct State {
    std::mutex mu;
    std::unordered_map<uint64_t, Pic> pics;
    std::wstring dump;                 /* the dump's folder, or "" */
    std::unordered_set<uint64_t> dumped;
    std::unordered_set<uint64_t> bad;  /* pictures WIC could not read: said once */
    std::vector<uint8_t> out;          /* the last picture handed over, when made for this call (over the game's own) */
    std::shared_ptr<std::vector<uint8_t>> held;          /* else the cached texels handed over last */
    std::unordered_map<std::wstring, FileSeen> files;    /* a picture file as last seen */
    std::unordered_map<uint64_t, Decoded> decoded;       /* texels by content (and size) */
    size_t decoded_bytes = 0;
    uint64_t tick = 0;
    /* The dump is written on a thread of its own, so the game does not wait for PNGs; the frame waits only when
     * more than k_dump_queue bytes are queued. */
    std::mutex dmu;
    std::condition_variable dcv;
    std::deque<DumpJob> jobs;
    size_t queued = 0;
    bool writer = false;
};
State& st()
{
    static State* s = new State;   /* never freed: the backend may call in as the process ends */
    return *s;
}

const int k_max_dump = 50000;
const size_t k_dump_queue = 256u << 20;

IWICImagingFactory* wic(void)
{
    static thread_local bool com = false;
    if (!com) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* RPC_E_CHANGED_MODE: the thread has COM already */
        com = true;
    }
    static IWICImagingFactory* f = NULL;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

template <class T> void release(T*& p)
{
    if (p) p->Release();
    p = NULL;
}

/* A PNG from its file, or from its file's bytes already read (bytes). */
bool load_png(const std::wstring& path, uint32_t* w, uint32_t* h, std::vector<uint8_t>* rgba,
              const std::vector<uint8_t>* bytes = NULL)
{
    IWICImagingFactory* f = wic();
    if (!f) return false;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* fr = NULL;
    IWICFormatConverter* cv = NULL;
    IWICStream* mem = NULL;
    bool ok = false, opened = false;
    UINT fw = 0, fh = 0;
    if (bytes && !bytes->empty()) {
        opened = SUCCEEDED(f->CreateStream(&mem)) &&
                 SUCCEEDED(mem->InitializeFromMemory((BYTE*)bytes->data(), (DWORD)bytes->size())) &&
                 SUCCEEDED(f->CreateDecoderFromStream(mem, NULL, WICDecodeMetadataCacheOnDemand, &dec));
    } else {
        opened = SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                        &dec));
    }
    if (opened &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&fw, &fh)) && fw && fh && fw <= 16384 && fh <= 16384 &&
        SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0,
                                 WICBitmapPaletteTypeCustom))) {
        rgba->resize((size_t)fw * fh * 4);
        ok = SUCCEEDED(cv->CopyPixels(NULL, fw * 4, (UINT)rgba->size(), rgba->data()));
        *w = fw;
        *h = fh;
    }
    release(cv);
    release(fr);
    release(dec);
    release(mem);
    return ok;
}

bool save_png(const std::wstring& path, uint32_t w, uint32_t h, const uint8_t* rgba)
{
    IWICImagingFactory* f = wic();
    if (!f) return false;
    std::vector<uint8_t> bgra((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    IWICStream* stream = NULL;
    IWICBitmapEncoder* enc = NULL;
    IWICBitmapFrameEncode* fe = NULL;
    IPropertyBag2* props = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fe, &props)) &&
              SUCCEEDED(fe->Initialize(props)) && SUCCEEDED(fe->SetSize(w, h)) && SUCCEEDED(fe->SetPixelFormat(&pf)) &&
              IsEqualGUID(pf, GUID_WICPixelFormat32bppBGRA) &&
              SUCCEEDED(fe->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data())) && SUCCEEDED(fe->Commit()) &&
              SUCCEEDED(enc->Commit());
    release(props);
    release(fe);
    release(enc);
    release(stream);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

/* A picture's id and kind from its file's name: sixteen hex digits first, then the end of the name, '_' or '.'. */
bool parse_name(const wchar_t* name, uint64_t* id, int* kind)
{
    const size_t n = wcslen(name);
    if (n < 20 || _wcsicmp(name + n - 4, L".png")) return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        const wchar_t c = name[i];
        const int d = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10
                    : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint64_t)d;
    }
    if (name[16] != L'.' && name[16] != L'_') return false;
    std::wstring low(name);
    for (wchar_t& c : low) c = (wchar_t)towlower(c);
    *id = v;
    *kind = low.size() >= 12 && !low.compare(low.size() - 12, 12, L".overlay.png") ? 2 : 1;
    return true;
}

/* The other copies of the textures pictures are for: each given the picture, with the size of the copy it is for.
 * A copy with a picture of its own keeps that. The number of copies given one. */
int add_copies(std::unordered_map<uint64_t, Pic>* pics)
{
    static std::unordered_map<uint64_t, size_t> by_id;     /* id -> its entry */
    static std::vector<size_t> first;                       /* group -> its first entry */
    const size_t n = sizeof k_tex_copies / sizeof k_tex_copies[0];
    if (by_id.empty()) {
        by_id.reserve(n);
        for (size_t i = 0; i < n; i++) {
            by_id.emplace(k_tex_copies[i].id, i);
            if (k_tex_copies[i].group >= first.size()) first.resize(k_tex_copies[i].group + 1, i);
        }
    }
    std::vector<std::pair<uint64_t, Pic>> add;
    for (const auto& kv : *pics) {
        if (kv.second.ref_w) continue;
        auto e = by_id.find(kv.first);
        if (e == by_id.end()) continue;
        const TexCopy& mine = k_tex_copies[e->second];
        for (size_t i = first[mine.group]; i < n && k_tex_copies[i].group == mine.group; i++) {
            const TexCopy& c = k_tex_copies[i];
            if (c.id == mine.id || pics->count(c.id)) continue;
            Pic p = kv.second;
            p.ref_w = mine.w;
            p.ref_h = mine.h;
            add.emplace_back(c.id, p);
        }
    }
    int k = 0;
    for (auto& a : add)
        if (pics->emplace(a.first, a.second).second) k++;   /* two pictures for copies of one texture: the first */
    return k;
}

/* A picture scaled down to w x h, each texel the mean of the ones it covers. */
void shrink(std::vector<uint8_t>* rgba, uint32_t pw, uint32_t ph, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> out((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t y0 = (uint32_t)((uint64_t)y * ph / h), y1 = (std::max)(y0 + 1, (uint32_t)((uint64_t)(y + 1) * ph / h));
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t x0 = (uint32_t)((uint64_t)x * pw / w), x1 = (std::max)(x0 + 1, (uint32_t)((uint64_t)(x + 1) * pw / w));
            uint32_t sum[4] = { 0, 0, 0, 0 };
            for (uint32_t sy = y0; sy < y1; sy++) {
                const uint8_t* p = rgba->data() + ((size_t)sy * pw + x0) * 4;
                for (uint32_t sx = x0; sx < x1; sx++, p += 4)
                    for (int c = 0; c < 4; c++) sum[c] += p[c];
            }
            const uint32_t cnt = (y1 - y0) * (x1 - x0);
            for (int c = 0; c < 4; c++) out[((size_t)y * w + x) * 4 + c] = (uint8_t)((sum[c] + cnt / 2) / cnt);
        }
    }
    rgba->swap(out);
}

void index_dir(const std::wstring& dir, int depth, std::unordered_map<uint64_t, Pic>* out, int* found)
{
    if (depth > 16) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            index_dir(p, depth + 1, out, found);
            continue;
        }
        uint64_t id;
        int kind;
        if (!parse_name(fd.cFileName, &id, &kind)) continue;
        (*found)++;
        if (!out->count(id)) out->emplace(id, Pic{ p, kind });   /* a mod above has it already: that one */
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Sixteen hex digits, the whole of s: the id they make. */
bool parse_hex16(const std::string& s, uint64_t* id)
{
    if (s.size() != 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint64_t)d;
    }
    *id = v;
    return true;
}

/* textures\aliases.txt: more textures for the pictures a mod has, one a line: the texture's id, then the picture,
 * either the id of one the folder has or a file of the folder by its path there (any name, as long as it is a .png:
 * a picture reached only through the list need not be named by an id). '#' starts a comment line. A texture the
 * folder has a picture of its own for keeps that. Into mine; added counts the ids given, bad the lines not taken,
 * and why the first was. */
void read_aliases(const std::wstring& dir, std::unordered_map<uint64_t, Pic>* mine, int* added, int* bad,
                  std::string* why)
{
    HANDLE h = CreateFileW((dir + L"\\aliases.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string text;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
        text.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, &text[0], (DWORD)text.size(), &got, NULL)) got = 0;
        text.resize(got);
    }
    CloseHandle(h);
    if (text.size() >= 3 && !text.compare(0, 3, "\xEF\xBB\xBF")) text.erase(0, 3);
    std::vector<std::pair<uint64_t, Pic>> add;
    int line = 0;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string ln = text.substr(at, e - at);
        at = e + 1;
        line++;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ' || ln.back() == '\t')) ln.pop_back();
        size_t b = ln.find_first_not_of(" \t");
        if (b == std::string::npos || ln[b] == '#') continue;
        size_t sp = ln.find_first_of(" \t", b);
        const std::string first = ln.substr(b, sp == std::string::npos ? std::string::npos : sp - b);
        const size_t tb = sp == std::string::npos ? std::string::npos : ln.find_first_not_of(" \t", sp);
        const std::string target = tb == std::string::npos ? std::string() : ln.substr(tb);
        auto fail = [&](const std::string& w) {
            if (!(*bad)++) *why = "line " + std::to_string(line) + ": " + w;
        };
        uint64_t id, to;
        if (!parse_hex16(first, &id)) { fail("not a texture id: " + first); continue; }
        if (target.empty()) { fail("no picture after " + first); continue; }
        Pic p;
        if (parse_hex16(target, &to)) {
            auto it = mine->find(to);
            if (it == mine->end()) { fail("no picture of " + target + " in the folder"); continue; }
            p = it->second;
        } else {
            wchar_t w[1024];
            if (!MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, w, 1024)) { fail("a name too long"); continue; }
            std::wstring rel(w);
            for (wchar_t& c : rel) if (c == L'/') c = L'\\';
            if (rel.find(L':') != std::wstring::npos || rel[0] == L'\\' || (L"\\" + rel + L"\\").find(L"\\..\\") !=
                std::wstring::npos) {
                fail("a picture outside the folder: " + target);
                continue;
            }
            std::wstring low(rel);
            for (wchar_t& c : low) c = (wchar_t)towlower(c);
            const DWORD a = GetFileAttributesW((dir + L"\\" + rel).c_str());
            if (low.size() < 5 || low.compare(low.size() - 4, 4, L".png") || a == INVALID_FILE_ATTRIBUTES ||
                (a & FILE_ATTRIBUTE_DIRECTORY)) {
                fail("no picture " + target + " in the folder");
                continue;
            }
            p.path = dir + L"\\" + rel;
            p.kind = low.size() >= 12 && !low.compare(low.size() - 12, 12, L".overlay.png") ? 2 : 1;
        }
        add.emplace_back(id, p);
    }
    for (auto& a : add)
        if (mine->emplace(a.first, a.second).second) (*added)++;   /* a picture of its own for it: that one */
}

int has_hook(u64 id)
{
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.pics.find(id);
    return it == s.pics.end() || s.bad.count(id) ? 0 : it->second.kind;
}

/* The content of a picture: its file's bytes' XXH64, read again only when the file's size or time is not as last
 * seen (left in bytes when read now). 0 when the file cannot be read. */
uint64_t content_of(State& s, const Pic& p, std::vector<uint8_t>* bytes)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    const uint64_t size = (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    const uint64_t time = (uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
    auto f = s.files.find(p.path);
    if (f != s.files.end() && f->second.size == size && f->second.time == time) return f->second.content;
    if (!size || size > 0x7FFFFFFFu) return 0;
    HANDLE h = CreateFileW(p.path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    bytes->resize((size_t)size);
    DWORD got = 0;
    const bool ok = ReadFile(h, bytes->data(), (DWORD)size, &got, NULL) && got == size;
    CloseHandle(h);
    if (!ok) {
        bytes->clear();
        return 0;
    }
    const uint64_t c = rsx_tex_xxh64(bytes->data(), bytes->size(), 0) | 1u;
    s.files[p.path] = FileSeen{ size, time, c };
    return c;
}

/* A picture's texels at a size: the key they are cached by and the backend's key for them. */
uint64_t texels_key(uint64_t content, uint32_t w, uint32_t h)
{
    const uint64_t k[3] = { content, w, h };
    return rsx_tex_xxh64(k, sizeof k, 0x7069637475726573ull) | 1u;
}

Decoded* cached(State& s, uint64_t key)
{
    auto it = s.decoded.find(key);
    if (it == s.decoded.end()) return NULL;
    it->second.used = ++s.tick;
    return &it->second;
}

/* Texels kept under key; past the cache's size the least recently used go first. */
Decoded* keep(State& s, uint64_t key, uint32_t w, uint32_t h, std::vector<uint8_t>&& rgba)
{
    static size_t cap = 0;
    if (!cap) {
        const char* e = getenv("GOWA_PICTURE_CACHE_MB");
        const int mb = e ? atoi(e) : 0;
        cap = (size_t)(mb > 0 ? mb : 512) << 20;
    }
    auto had = s.decoded.find(key);
    if (had != s.decoded.end()) {
        s.decoded_bytes -= had->second.rgba->size();
        s.decoded.erase(had);
    }
    const size_t n = rgba.size();
    while (s.decoded_bytes + n > cap && !s.decoded.empty()) {
        auto old = s.decoded.begin();
        for (auto it = s.decoded.begin(); it != s.decoded.end(); ++it)
            if (it->second.used < old->second.used) old = it;
        s.decoded_bytes -= old->second.rgba->size();
        s.decoded.erase(old);
    }
    Decoded& d = s.decoded[key];
    d.w = w;
    d.h = h;
    d.used = ++s.tick;
    d.rgba = std::make_shared<std::vector<uint8_t>>(std::move(rgba));
    s.decoded_bytes += n;
    return &d;
}

int picture_hook(u64 id, u32 w, u32 h, const u8* base, rsx_tex_picture* out)
{
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.pics.find(id);
    if (it == s.pics.end()) return 0;
    const Pic& pic = it->second;
    std::vector<uint8_t> bytes;
    const uint64_t content = content_of(s, pic, &bytes);
    std::shared_ptr<std::vector<uint8_t>> tex;
    uint32_t pw = 0, ph = 0;
    if (Decoded* d = content ? cached(s, content) : NULL) {
        tex = d->rgba;
        pw = d->w;
        ph = d->h;
    } else {
        std::vector<uint8_t> png;
        if (!load_png(pic.path, &pw, &ph, &png, &bytes)) {
            if (s.bad.insert(id).second) {
                char u8[1200];
                WideCharToMultiByte(CP_UTF8, 0, pic.path.c_str(), -1, u8, sizeof u8, NULL, NULL);
                fprintf(stderr, "[tex] %s: not a picture this program can read%c", u8, 10);
            }
            return 0;
        }
        if (content) tex = keep(s, content, pw, ph, std::move(png))->rgba;
        else tex = std::make_shared<std::vector<uint8_t>>(std::move(png));   /* not read whole: not kept */
    }
    /* A smaller copy of the texture the picture is for: the picture smaller by as much. */
    if (pic.ref_w && (w < pic.ref_w || h < pic.ref_h)) {
        const uint32_t nw = w < pic.ref_w ? (std::max)(1u, (uint32_t)((uint64_t)pw * w / pic.ref_w)) : pw;
        const uint32_t nh = h < pic.ref_h ? (std::max)(1u, (uint32_t)((uint64_t)ph * h / pic.ref_h)) : ph;
        if (nw < pw || nh < ph) {
            const uint64_t sk = content ? texels_key(content, nw, nh) : 0;
            if (Decoded* d = sk ? cached(s, sk) : NULL) {
                tex = d->rgba;
            } else {
                std::vector<uint8_t> scaled(*tex);
                shrink(&scaled, pw, ph, nw, nh);
                if (sk) tex = keep(s, sk, nw, nh, std::move(scaled))->rgba;
                else tex = std::make_shared<std::vector<uint8_t>>(std::move(scaled));
            }
            pw = nw;
            ph = nh;
        }
    }
    if (pic.kind == 2) {   /* over the game's own, scaled to the overlay's size: made for this call, no key */
        if (!base) return 0;
        s.out.assign(tex->begin(), tex->end());
        for (uint32_t y = 0; y < ph; y++) {
            const uint8_t* brow = base + (size_t)((uint64_t)y * h / ph) * w * 4;
            for (uint32_t x = 0; x < pw; x++) {
                uint8_t* o = &s.out[((size_t)y * pw + x) * 4];
                const uint8_t* b = brow + (size_t)((uint64_t)x * w / pw) * 4;
                const uint32_t a = o[3];
                if (a == 0) {
                    memcpy(o, b, 4);
                } else if (a == 1) {
                    o[0] = o[1] = o[2] = o[3] = 0;
                } else {
                    for (int c = 0; c < 3; c++) o[c] = (uint8_t)((o[c] * a + b[c] * (255u - a) + 127u) / 255u);
                    o[3] = (uint8_t)(a + (b[3] * (255u - a) + 127u) / 255u);
                }
            }
        }
        out->w = pw;
        out->h = ph;
        out->rgba = s.out.data();
        out->key = 0;
        return 1;
    }
    s.held = tex;   /* the backend reads it until its next call */
    out->w = pw;
    out->h = ph;
    out->rgba = s.held->data();
    out->key = content ? texels_key(content, pw, ph) : 0;
    return 1;
}

void dump_writer(void)
{
    State& s = st();
    for (;;) {
        DumpJob j;
        {
            std::unique_lock<std::mutex> lk(s.dmu);
            s.dcv.wait(lk, [&] { return !s.jobs.empty(); });
            j = std::move(s.jobs.front());
            s.jobs.pop_front();
        }
        save_png(j.path, j.w, j.h, j.rgba.data());
        {
            std::lock_guard<std::mutex> lk(s.dmu);
            s.queued -= j.rgba.size();
        }
        s.dcv.notify_all();
    }
}

void dump_hook(u64 id, u32 w, u32 h, u32 fmt, const u8* base)
{
    State& s = st();
    std::wstring p;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.dump.empty() || (int)s.dumped.size() >= k_max_dump || !s.dumped.insert(id).second) return;
        wchar_t name[96];
        swprintf(name, 96, L"\\%016llx_%ux%u_%02X.png", (unsigned long long)id, w, h, fmt & 0xFFu);
        p = s.dump + name;
    }
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;   /* an earlier run's */
    DumpJob j;
    j.path = p;
    j.w = w;
    j.h = h;
    j.rgba.assign(base, base + (size_t)w * h * 4);
    std::unique_lock<std::mutex> lk(s.dmu);
    if (!s.writer) {
        std::thread(dump_writer).detach();
        s.writer = true;
    }
    s.dcv.wait(lk, [&] { return s.queued + j.rgba.size() <= k_dump_queue || s.jobs.empty(); });
    s.queued += j.rgba.size();
    s.jobs.push_back(std::move(j));
    s.dcv.notify_all();
}

std::wstring settings_dir(void)
{
    wchar_t p[MAX_PATH * 2];
    const char* s = ps3_settings_path();
    if (s && *s && MultiByteToWideChar(CP_UTF8, 0, s, -1, p, MAX_PATH * 2)) {
        for (wchar_t* c = p; *c; c++) if (*c == L'/') *c = L'\\';
        if (wchar_t* e = wcsrchr(p, L'\\')) { *e = 0; return p; }
    }
    const DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH);
    if (n && n < MAX_PATH) {
        if (wchar_t* e = wcsrchr(p, L'\\')) *e = 0;
        return p;
    }
    return L".";
}

}  // namespace

std::wstring gowa_textures_dump_folder(void)
{
    if (const wchar_t* e = _wgetenv(L"GOWA_TEXTURE_DUMP")) if (*e) return e;
    return settings_dir() + L"\\textures-dump";
}

int gowa_textures_start(const ModList& l, const std::wstring& dump_dir)
{
    State& s = st();
    int found = 0, copies = 0, aliased = 0;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.pics.clear();
        s.bad.clear();
        if (l.use)
            for (const ModInfo& m : l.mods) {
                if (!m.on || !m.error.empty() || m.textures.empty()) continue;
                std::unordered_map<uint64_t, Pic> mine;   /* the mod's own, then the ids its aliases.txt adds */
                index_dir(m.textures, 0, &mine, &found);
                int bad = 0;
                std::string why;
                read_aliases(m.textures, &mine, &aliased, &bad, &why);
                if (bad) {
                    char id8[512] = "";
                    WideCharToMultiByte(CP_UTF8, 0, m.id.c_str(), -1, id8, sizeof id8, NULL, NULL);
                    printf("[tex] %s: aliases.txt: %d line%s not taken; %s\n", id8, bad, bad == 1 ? "" : "s",
                           why.c_str());
                }
                for (auto& kv : mine) s.pics.emplace(kv.first, kv.second);   /* a mod above has it: that one */
            }
        copies = add_copies(&s.pics);
        s.dump = dump_dir;
        if (!s.dump.empty() && !CreateDirectoryW(s.dump.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            s.dump.clear();
    }
    if (s.pics.empty() && s.dump.empty()) {
        rsx_tex_set_hooks(NULL);
        return 0;
    }
    rsx_tex_hooks hk;
    hk.has = has_hook;
    hk.picture = picture_hook;
    hk.dump = s.dump.empty() ? NULL : dump_hook;
    rsx_tex_set_hooks(&hk);
    char u8[1200] = "";
    if (!s.dump.empty()) WideCharToMultiByte(CP_UTF8, 0, s.dump.c_str(), -1, u8, sizeof u8, NULL, NULL);
    char more[64] = "";
    if (aliased) snprintf(more, sizeof more, " (%d more textures by aliases.txt)", aliased);
    printf("[tex] %d pictures of mods%s, for %d textures (%d of them other copies of the game's)%s%s\n", found, more,
           (int)s.pics.size(), copies, u8[0] ? "; the dump: " : "", u8);
    return (int)s.pics.size();
}

#endif /* _WIN32 */
