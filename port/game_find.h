/*
 * Ascension-R -- finding the game on this PC (game_find.cpp)
 */
#ifndef GOWA_GAME_FIND_H
#define GOWA_GAME_FIND_H

#include <stddef.h>
#include <wchar.h>

enum { GAME_PATH_MAX = 1024 };

/* What a folder holds, worst to best. */
enum GameState {
    GAME_NONE,          /* no PARAM.SFO */
    GAME_OTHER_TITLE,   /* another title, or another release of this one */
    GAME_NO_ELF,        /* this release, GOWA.SELF not decrypted to GOWA.elf yet */
    GAME_OTHER_BUILD,   /* this title, but another version, or another GOWA.elf */
    GAME_OK             /* what this port was lifted from */
};

struct GameInfo {
    wchar_t dir[GAME_PATH_MAX];   /* the folder holding PARAM.SFO and USRDIR */
    char title[128], title_id[16], app_ver[16];
    GameState state;
};

/* Whether a file is the GOWA.elf this port was made from. */
int game_elf_ok(const wchar_t* path);

/* A file's SHA-256 in lower-case hex. 1 = read. */
int game_file_sha256(const wchar_t* path, char hex[65]);

/* What a folder given as it is holds. */
GameState game_probe(const wchar_t* dir, GameInfo* g);

/* Only the title's id and version, from the folder's PARAM.SFO; no hash of the game's code. 1 = read. */
int game_read_info(const wchar_t* dir, GameInfo* g);

/* The same for what a player would pick: the game's folder, the one holding PS3_GAME, an RPCS3
 * folder, its dev_hdd0, its game folder, or USRDIR. The best of them; dir says which. */
GameState game_locate(const wchar_t* path, GameInfo* g);

struct GameSearch {
    const wchar_t* under;                    /* the folder to look under; NULL, every drive */
    volatile long* stop;                     /* set by another thread: ends the search */
    void (*now)(const wchar_t* dir);         /* the folder being read; may be NULL */
    int seconds;                             /* how long it may take */
    int copies;                              /* out: how many copies of this title it saw */
    int good_enough;                         /* the state that ends the search; 0, GAME_OK */
};

/* Walks the folders, the ones that sound like games first, and stops at the first copy that is
 * GAME_OK. The best one seen goes in *best. Only reads. */
GameState game_search(GameSearch* s, GameInfo* best);

/* The folder a link points at, kept even if that is gone. 0: path is no link. */
int game_link_target(const wchar_t* link, wchar_t* out, size_t n);

/* Whether game_link could put a link to target at link: nothing there, a link, an empty folder, or
 * target itself. A folder with files in it is the player's and stays. */
int game_link_possible(const wchar_t* link, const wchar_t* target, wchar_t* err, size_t n);

/* Makes link a directory junction to target (no administrator rights needed), as `gowa.py setup`
 * does. A link already there is replaced; what it pointed at is not touched. */
int game_link(const wchar_t* link, const wchar_t* target, wchar_t* err, size_t n);

#endif
