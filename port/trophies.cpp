/*
 * Ascension-R -- the game's trophies.
 *
 * The runtime's sceNpTrophy reads the set from the game's own TROPHY.TRP and keeps what the game unlocks. Here each
 * unlock becomes a notice over the picture (rsx_d3d12_backend_trophy): the trophy's picture, its grade and its name
 * in the game's language. #1 Blood Oath is won in the multiplayer, which the port does not have: it is left out of
 * the platinum, Champion of the Gods, which the PS3 gives once every trophy that counts toward it is won. Every
 * release the port takes numbers the set alike.
 *
 * The launcher's Trophies page reads the same set and the same file of unlocks (gowa_trophies_read).
 */
#include "port/trophies.h"
#include "ps3recomp/libs/network/sceNpTrophy.h"
#include "ps3recomp/libs/system/cellSysutil.h"
#include "ps3recomp/libs/codec/stb_image.h"
#include "rsx_d3d12_backend.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include "port_compat.h"
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

static const int k_multiplayer[] = { 1 };   /* Blood Oath: "Complete the MP Training in Olympus" */

static void trophies_rules(void)
{
    static int done;
    if (done) return;
    done = 1;
    sceNpTrophySetUnobtainable(NULL, k_multiplayer, (int)(sizeof k_multiplayer / sizeof k_multiplayer[0]));
}

unsigned char* gowa_trophies_icon(const char* trp, int id, int* w, int* h)
{
    char name[16];
    snprintf(name, sizeof name, "TROP%03d.PNG", id);
    size_t n = 0;
    unsigned char* png = ps3_trophy_trp_file(trp, name, &n);
    if (!png) return NULL;
    int c = 0;
    unsigned char* rgba = stbi_load_from_memory(png, (int)n, w, h, &c, 4);
    free(png);
    return rgba;
}

void gowa_trophies_free(unsigned char* rgba)
{
    if (rgba) stbi_image_free(rgba);
}

/* --- the notices ----------------------------------------------------------------------------------------------------
 * Made on a thread of their own, in the order the trophies came: reading and decoding a picture takes a few
 * milliseconds the game's thread need not wait. */
struct Notice {
    std::string trp, name;
    int id, grade;
};
static std::mutex s_mu;
static std::condition_variable s_cv;
static std::deque<Notice> s_queue;

static void notice_show(const Notice& n)
{
    char caption[64];
    snprintf(caption, sizeof caption, "%s trophy earned", ps3_trophy_grade_name(n.grade));
    int w = 0, h = 0;
    unsigned char* icon = gowa_trophies_icon(n.trp.c_str(), n.id, &w, &h);
    rsx_d3d12_backend_trophy(caption, n.name.c_str(), n.grade, icon, w, h);
    gowa_trophies_free(icon);
}

static void notice_worker(void)
{
    for (;;) {
        Notice n;
        {
            std::unique_lock<std::mutex> lk(s_mu);
            s_cv.wait(lk, [] { return !s_queue.empty(); });
            n = std::move(s_queue.front());
            s_queue.pop_front();
        }
        notice_show(n);
    }
}

static void notice_add(const PS3TrophySet* set, int id)
{
    const PS3TrophyEntry* t = ps3_trophy_set_entry(set, id);
    if (!t) return;
    static std::once_flag started;
    static int threaded;
    std::call_once(started, [] {
        try {
            std::thread(notice_worker).detach();
            threaded = 1;
        } catch (...) {
        }
    });
    Notice n{ set->path, t->name, id, t->grade };
    if (!threaded) {   /* no thread to be had: here, then */
        notice_show(n);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(s_mu);
        s_queue.push_back(std::move(n));
    }
    s_cv.notify_one();
}

/* The runtime's observer: on the game's thread that unlocked the trophy. */
static void on_unlock(const PS3TrophySet* set, int id)
{
    notice_add(set, id);
}

/* --- the set and the unlocks ------------------------------------------------------------------------------------- */

/* The first TROPDIR\<set>\TROPHY.TRP under dir (UTF-8), into out. */
static int find_trp(const char* dir, char* out, size_t cap)
{
    if (!dir || !*dir) return -1;
#ifndef _WIN32
    /* POSIX: the same search, on UTF-8 paths. */
    char tropdir[1100];
    snprintf(tropdir, sizeof tropdir, "%s/TROPDIR", dir);
    DIR* d = opendir(tropdir);
    if (!d) return -1;
    int r = -1;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char trp[1400];
        snprintf(trp, sizeof trp, "%s/%s/TROPHY.TRP", tropdir, e->d_name);
        struct stat st;
        if (stat(trp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (strlen(trp) < cap) { strcpy(out, trp); r = 0; }
        break;
    }
    closedir(d);
    return r;
#else
    wchar_t wdir[1024], pat[1100];
    if (!MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, 1024)) return -1;
    swprintf(pat, 1100, L"%ls\\TROPDIR\\*", wdir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int r = -1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        wchar_t trp[1400];
        swprintf(trp, 1400, L"%ls\\TROPDIR\\%ls\\TROPHY.TRP", wdir, fd.cFileName);
        if (GetFileAttributesW(trp) == INVALID_FILE_ATTRIBUTES) continue;
        if (WideCharToMultiByte(CP_UTF8, 0, trp, -1, out, (int)cap, NULL, NULL)) r = 0;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return r;
#endif
}

int gowa_trophies_read(const char* hdd0, const char* app_dir, const char* disc_dir, int lang, GowaTrophies* t)
{
    memset(t, 0, sizeof *t);
    trophies_rules();
    char trp[1024];
    if (find_trp(app_dir, trp, sizeof trp) && find_trp(disc_dir, trp, sizeof trp)) return -1;
    if (ps3_trophy_set_load(trp, lang, &t->set)) {
        t->set.count = 0;
        return -1;
    }
    char id[16];   /* the set's id as the game gives it: NPWR00802, without the _00 of its folder */
    snprintf(id, sizeof id, "%s", t->set.npcommid);
    if (char* u = strchr(id, '_')) *u = 0;
    for (int i = 0; i < t->set.count; i++) t->unobtainable[i] = (unsigned char)sceNpTrophyIsUnobtainable(id, i);
    /* The runtime's file: { "trophies": [ { "id": N, "timestamp": T }, ... ] }, one trophy a line. */
    sceNpTrophyStorageFile(hdd0, id, 1, t->store, sizeof t->store);
    if (FILE* f = fopen(t->store, "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            const char* p = strstr(line, "\"id\":");
            unsigned n = 0;
            unsigned long long ts = 0;
            if (!p || sscanf(p, "\"id\": %u", &n) != 1 || n >= PS3_TROPHY_SET_MAX) continue;
            if (const char* q = strstr(line, "\"timestamp\":")) sscanf(q, "\"timestamp\": %llu", &ts);
            t->unlocked[n] = 1;
            t->when[n] = (long long)ts;
        }
        fclose(f);
    }
    return 0;
}

/* --- the start ------------------------------------------------------------------------------------------------------ */

extern "C" void gowa_trophies_start(void)
{
    trophies_rules();
    sceNpTrophy_unlock_observer = on_unlock;
    const char* demo = getenv("GOWA_TROPHY_DEMO");
    if (!demo || !*demo) return;
    /* As the game would find its set: the update's folder (or the digital game's), then the disc's. */
    const char* hdd0 = getenv("PS3_HDD0_ROOT");
    const char* vfs = getenv("PS3_VFS_ROOT");
    const char* title = getenv("PS3_TITLE");
    std::string app = std::string(hdd0 ? hdd0 : "") + "/game/" + (title ? title : "");
    std::string disc = std::string(vfs ? vfs : "") + "/PS3_GAME";
    static GowaTrophies t;
    if (gowa_trophies_read(hdd0, hdd0 && title ? app.c_str() : NULL, vfs ? disc.c_str() : NULL,
                           (int)cellSysutil_language(), &t)) {
        printf("[trophies] GOWA_TROPHY_DEMO: no trophy set found\n");
        return;
    }
    int n = 0;
    for (const char* p = demo; *p;) {
        char* end = NULL;
        const long id = strtol(p, &end, 10);
        if (end == p) { p++; continue; }
        p = end;
        if (ps3_trophy_set_entry(&t.set, (int)id)) {
            notice_add(&t.set, (int)id);
            n++;
        }
    }
    printf("[trophies] GOWA_TROPHY_DEMO: %d notices from %s\n", n, t.set.path);
}
