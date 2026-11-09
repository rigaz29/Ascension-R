/*
 * Ascension-R -- turning the player's GOWA.SELF into GOWA.elf (game_prepare.cpp)
 */
#ifndef GOWA_GAME_PREPARE_H
#define GOWA_GAME_PREPARE_H

#include <stddef.h>
#include <wchar.h>

/* How far the work is, for whoever shows it. The worker writes, others read. */
enum GamePhase { GP_NONE, GP_DOWNLOAD, GP_VERIFY, GP_UNPACK, GP_COPY, GP_DECRYPT, GP_CHECK };

struct GameProgress {
    volatile long phase;
    volatile long long done, total;        /* bytes of the download */
    volatile long long speed;              /* bytes a second, lately */
    volatile unsigned long long data_at;   /* GetTickCount64 when the last bytes came */
    volatile unsigned long long phase_at;  /* when this phase began */
    volatile long retries;                 /* connections dropped and taken up again */
    volatile long stop;                    /* game_progress_stop: give up */
};

/* Asks the work to stop, a download at once. */
void game_progress_stop(GameProgress* p);

/* rpcs3.exe of the RPCS3 a game folder is installed in: <RPCS3>\dev_hdd0\game\NPUA80918. 1 = there is one. */
int game_find_rpcs3(const wchar_t* game_dir, wchar_t* exe, size_t n);

/* Where rpcs3.exe would be for a game folder under <RPCS3>\dev_hdd0\game, whether it is there or not. */
const wchar_t* game_rpcs3_slot(const wchar_t* game_dir);

/* This game's license file in that RPCS3. 1 = there is one. */
int game_find_rap(const wchar_t* rpcs3_exe, wchar_t* rap, size_t n);

/* GOWA.SELF of the game's folder, decrypted to <data>\GOWA.elf by that RPCS3 and checked against the
 * build this port was made from. It works on copies in a folder of its own: neither the game's folder
 * nor RPCS3's own is written to. 1 = done; otherwise err says why. */
int game_prepare(const wchar_t* game_dir, const wchar_t* rpcs3_exe, const wchar_t* rap, const wchar_t* data,
                 wchar_t* err, size_t n, GameProgress* p);

/* The official RPCS3 build, of a fixed version and checked by its SHA-256, into a folder of its own under data.
 * 1 = done, with exe set; otherwise err says why. game_drop_rpcs3 removes it again. */
int game_fetch_rpcs3(const wchar_t* data, wchar_t* exe, size_t n, wchar_t* err, size_t en, GameProgress* p);
void game_drop_rpcs3(const wchar_t* data);

#endif
