/*
 * Ascension-R -- GitHub's list of releases read for the newest one past this version (update_check.h)
 *
 * The list is an array of releases, each an object whose own keys say what it is: "tag_name", "html_url", "draft",
 * "prerelease". The same names come again inside it ("author" and every asset have an "html_url" of their own), so only
 * the keys of a release itself, one level into the array, are taken. Strings are read through their escapes, so a
 * brace or a quote in a release's notes does not count.
 */
#include "update_check.h"
#include <ctype.h>
#include <string.h>

static int digits(const char*& s, int* out)
{
    if (!isdigit((unsigned char)*s)) return 0;
    long n = 0;
    while (isdigit((unsigned char)*s)) {
        n = n * 10 + (*s++ - '0');
        if (n > 1000000) return 0;
    }
    *out = (int)n;
    return 1;
}

int update_version_parse(const char* s, ArVersion* v)
{
    memset(v, 0, sizeof *v);
    if (!s) return 0;
    if (*s == 'v' || *s == 'V') s++;
    if (!digits(s, &v->major) || *s++ != '.' || !digits(s, &v->minor) || *s++ != '.' || !digits(s, &v->patch)) return 0;
    if (!*s) return 1;
    if (*s++ != '-') return 0;
    size_t i = 0;
    while (isalpha((unsigned char)*s)) {
        if (i + 1 >= sizeof v->pre_label) return 0;
        v->pre_label[i++] = (char)tolower((unsigned char)*s++);
    }
    if (!i) return 0;
    v->pre = 1;
    if (!*s) return 1;
    if (*s++ != '.' || !digits(s, &v->pre_n)) return 0;
    return !*s;
}

int update_version_cmp(const ArVersion& a, const ArVersion& b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre != b.pre) return a.pre ? -1 : 1;   /* 0.1.0 comes after every 0.1.0-something */
    if (!a.pre) return 0;
    const int c = strcmp(a.pre_label, b.pre_label);
    if (c) return c < 0 ? -1 : 1;
    return a.pre_n < b.pre_n ? -1 : a.pre_n > b.pre_n ? 1 : 0;
}

/* The JSON string that starts at s[i] (a quote): as much of its text as fits in out (cap bytes with the 0), *len its
 * whole length. The index past its closing quote, or 0 if it does not close. \uXXXX is taken as one '?'. */
static size_t json_string(const char* s, size_t n, size_t i, char* out, size_t cap, size_t* len)
{
    size_t o = 0;
    for (i++; i < n; i++) {
        char c = s[i];
        if (c == '"') {
            if (out && cap) out[o < cap ? o : cap - 1] = 0;
            if (len) *len = o;
            return i + 1;
        }
        if (c == '\\') {
            if (++i >= n) return 0;
            c = s[i];
            if (c == 'u') {
                if (i + 4 >= n) return 0;
                i += 4;
                c = '?';
            } else if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
        }
        if (out && o + 1 < cap) out[o] = c;
        o++;
    }
    return 0;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int update_pick(const char* s, size_t n, const char* current, UpdateRelease* out)
{
    ArVersion cur, best;
    if (!s || !update_version_parse(current, &cur)) return -1;
    size_t i = n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3) ? 3 : 0;   /* a file written with a UTF-8 mark (GOWA_UPDATE_FEED) */
    while (i < n && is_space(s[i])) i++;
    if (i >= n || s[i] != '[') return -1;   /* not a list: an error from GitHub is an object */
    int depth = 0, found = 0;
    /* the release being read */
    char tag[sizeof out->tag] = "", url[sizeof out->url] = "";
    int draft = 0, pre = 0;
    while (i < n) {
        const char c = s[i];
        if (c == '"') {
            if (depth != 2) {   /* anywhere but a release's own keys: passed over */
                const size_t e = json_string(s, n, i, NULL, 0, NULL);
                if (!e) return -1;
                i = e;
                continue;
            }
            char key[24];
            size_t klen = 0;
            const size_t e = json_string(s, n, i, key, sizeof key, &klen);
            if (!e) return -1;
            size_t k = e;
            while (k < n && is_space(s[k])) k++;
            if (k >= n || s[k] != ':') return -1;
            k++;
            while (k < n && is_space(s[k])) k++;
            const int known = klen < sizeof key;
            if (k < n && s[k] == '"') {
                char val[sizeof out->url];
                size_t vlen = 0;
                const size_t ve = json_string(s, n, k, val, sizeof val, &vlen);
                if (!ve) return -1;
                if (known && vlen < sizeof val) {
                    if (!strcmp(key, "tag_name") && vlen < sizeof tag) memcpy(tag, val, vlen + 1);
                    else if (!strcmp(key, "html_url")) memcpy(url, val, vlen + 1);
                }
                i = ve;
                continue;
            }
            if (k < n && (s[k] == 't' || s[k] == 'f')) {   /* true or false: the rest of the word is passed over */
                if (known && !strcmp(key, "draft")) draft = s[k] == 't';
                else if (known && !strcmp(key, "prerelease")) pre = s[k] == 't';
                i = k + 1;
                continue;
            }
            i = k;   /* an object, a list, a number or null: gone through below */
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
            if (depth == 2) {
                if (c != '{') return -1;   /* a list of releases holds objects */
                tag[0] = url[0] = 0;
                draft = pre = 0;
            }
        } else if (c == '}' || c == ']') {
            if (depth == 2) {   /* a release read */
                ArVersion v;
                if (tag[0] && url[0] && !draft && update_version_parse(tag, &v) && update_version_cmp(v, cur) > 0 &&
                    !strncmp(url, UPDATE_PAGE_PREFIX, sizeof UPDATE_PAGE_PREFIX - 1) &&
                    (!found || update_version_cmp(v, best) > 0)) {
                    best = v;
                    found = 1;
                    memcpy(out->tag, tag, sizeof tag);
                    memcpy(out->url, url, sizeof url);
                    out->prerelease = pre;
                }
            }
            if (--depth == 0) return found;   /* the end of the list */
            if (depth < 0) return -1;
        }
        i++;
    }
    return -1;   /* the list does not end */
}
