/*
 * The precompile screen's music (precompile_music.cpp).
 */
#pragma once

#include <stdint.h>

/* The precompile screen is up; t0_ms is its clock's start (GetTickCount64), the time its lightning keeps. The music is
 * made on a thread of its own and plays from 2.5 s on, unless [system] precompile_music is off. */
void gowa_music_begin(uint64_t t0_ms);

/* The screen goes: the music fades out over a second on its own thread. Nothing when none was begun. */
void gowa_music_end(void);

/* --music-preview <file.wav> [seconds]: what the screen plays from its start, at full level, into a WAV file. */
int gowa_music_preview(const char* path, double seconds);
