/*
 * Ascension-R -- what a session leaves behind when something goes wrong (report.cpp)
 */
#ifndef GOWA_REPORT_H
#define GOWA_REPORT_H

#include <stddef.h>

/* How a session's log says it ended. */
enum SessionEnd {
    SESSION_NONE,       /* no log, or the game never started in it */
    SESSION_CLEAN,      /* the window closed, or the game quit */
    SESSION_SUDDEN,     /* the log just stops */
    SESSION_FROZE,      /* frames stopped, and the log stops there */
    SESSION_GPU_LOST,   /* the graphics device was lost */
    SESSION_CRASH       /* an exception nothing handled */
};

/* The session writes its log to path: its system lines go there now. */
void gowa_report_session_start(const char* log_path);

/* An address as module+offset, the game's own read against its map. */
void gowa_where(const void* addr, char* out, size_t n);

/* Watches for frames that stop, in a session with a log or with GOWA_FREEZE_WATCH set. */
void gowa_freeze_watch_start(void);

/* How the session before this one ended. */
SessionEnd gowa_last_session(void);

/* Makes the zip of the last sessions beside the logs, and puts its path in out. 0 = made. */
int gowa_report_create(int with_saves, char* out, size_t n);

#endif
