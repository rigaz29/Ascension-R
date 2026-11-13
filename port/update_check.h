/*
 * Ascension-R -- a newer release on GitHub, for the launcher to say (launcher.cpp)
 *
 * update_feed.cpp reads GitHub's list of the repository's releases and picks the newest past this version, with no
 * Windows in it (tests/test_update_check.py builds it alone). update_check.cpp asks GitHub for that list on a thread of
 * its own, at most once every six hours, and keeps what it found in update_check.txt beside the settings file.
 */
#pragma once
#include <stddef.h>

/* 0.1.0-alpha.8: a release's number, and its pre-release part if it has one. */
struct ArVersion {
    int major, minor, patch;
    int pre;              /* a pre-release: below the release of the same number */
    char pre_label[16];   /* "alpha", "beta", "rc": in that order, as they sort */
    int pre_n;
};

/* "0.1.0", "0.1.0-alpha.8" or "v0.1.0-alpha.8": 1, or 0 if it is none of those. */
int update_version_parse(const char* s, ArVersion* v);
/* Below 0 if a is older than b, 0 if the same, above 0 if newer. */
int update_version_cmp(const ArVersion& a, const ArVersion& b);

struct UpdateRelease {
    char tag[48];    /* "v0.1.0-alpha.8" */
    char url[256];   /* its page on GitHub */
    int prerelease;
};

/* Where every release's page is. A page anywhere else is not offered. */
#define UPDATE_PAGE_PREFIX "https://github.com/rigaz29/Ascension-R/releases/"

/* GitHub's list of releases, as JSON: the newest that is not a draft, is newer than `current` and has its page under
 * UPDATE_PAGE_PREFIX, into *out. 1 found, 0 none newer, -1 not a list of releases (or `current` not a version). */
int update_pick(const char* json, size_t len, const char* current, UpdateRelease* out);

/* What the launcher shows. */
struct UpdateState {
    int known;              /* a newer release than this one is known: rel */
    UpdateRelease rel;
    char dismissed[48];     /* the release whose notice was closed */
};

/* What update_check.txt holds into *st; then, if that is six hours old or more (or GOWA_UPDATE_FEED names a file of
 * the list to read instead), a thread that asks GitHub and posts msg to hwnd when it is done. */
void update_check_begin(void* hwnd, unsigned msg, UpdateState* st);
/* On msg, with its wParam: what the thread found into *st and update_check.txt. */
void update_check_done(UpdateState* st, unsigned long long wparam);
/* The notice of st->rel closed, for that release: kept in update_check.txt. */
void update_check_dismiss(UpdateState* st);
/* The launcher closes: a request under way is dropped. */
void update_check_end(void);
