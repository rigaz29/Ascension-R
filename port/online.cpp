/*
 * Ascension-R -- the multiplayer's port side (docs/notes/2026-11-22-multiplayer-phase0.md and phase1.md).
 *
 * The game's own log. func_001C2784 is its printf-style log function: r3 a level (1 for errors), r4 the format, the
 * arguments from r5 on and past r10 in the caller's parameter save area (sp + 0x30 + 8 * n for argument n). The
 * release build keeps its 637 calls, most of them in the online code, and empties the function. With
 * GOWA_GAME_LOG=1 gowa_game_log (scripts/ppu_hooks.json, at its first instruction) prints each line as
 * "[game] <level> <text>": how every step of the online gate shows.
 *
 * The TSS. func_0035D684 decrypts PSN Title Small Storage in place and then wants a version 2 header; with
 * GOWA_GAME_LOG=1 gowa_tss_seen prints the size and the first bytes the decryption gave, which says at once whether
 * a TSS of ours went in right.
 *
 * SAM's SSL. SamStateStart::connect (func_0040A560) takes the SSL flag from byte 0x796 of the network settings,
 * which their constructor sets to 1 and nothing in the TSS changes; the matchmaker then speaks SSL of the game's own.
 * Ours does not, so gowa_sam_ssl clears the byte just before connect reads it. GOWA_SAM_SSL=1 leaves it.
 * That connect is also where the game goes online for multiplayer: the launcher's cheats go off there
 * (cheats.cpp, gowa_cheats_off), until the game is started again.
 *
 * The Online Pass: gowa_online_pass, below.
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include "port_compat.h"

extern "C" void gowa_cheats_off(const char* why);   /* cheats.cpp */

static int env_on(const char* name)
{
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

/* Whether [ea, ea + n) of guest memory can be read: committed and not guarded. */
static int guest_readable(uint32_t ea, uint32_t n)
{
    /* ppu_vm_size is 0 on Windows (the accessors do not check), so the host's view of the page decides. */
    if (!ea || !n || (uint64_t)ea + n > 0x100000000ull ||
        (ppu_vm_size && (uint64_t)ea + n > (uint64_t)ppu_vm_size)) return 0;
    return port_mem_mapped(vm_base + ea, n);
}

static uint32_t guest_u32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return _byteswap_ulong(v);
}

static uint64_t guest_u64(uint32_t ea)
{
    uint64_t v;
    memcpy(&v, vm_base + ea, 8);
    return _byteswap_uint64(v);
}

/* A guest C string into out (at most n - 1 characters), "(bad)" when it cannot be read. */
static void guest_str(uint32_t ea, char* out, size_t n)
{
    size_t k = 0;
    if (!ea) {
        snprintf(out, n, "(null)");
        return;
    }
    while (k + 1 < n) {
        const uint32_t a = ea + (uint32_t)k;
        if ((k == 0 || (a & 0xFFF) == 0) && !guest_readable(a, 1)) {
            if (k == 0) {
                snprintf(out, n, "(bad 0x%08X)", ea);
                return;
            }
            break;
        }
        const char c = (char)vm_base[a];
        if (!c) break;
        out[k++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    out[k] = '\0';
}

/* Argument k of the variable ones (0 = r5). */
static uint64_t log_arg(const ppu_context* ctx, unsigned k)
{
    if (k < 6) return ctx->gpr[5 + k];
    const uint32_t at = (uint32_t)ctx->gpr[1] + 0x30 + 8 * (k + 2);
    return guest_readable(at, 8) ? guest_u64(at) : 0;
}

/* printf over the guest's arguments: flags, width, precision and length as C has them; %s reads guest memory. */
static void guest_format(const ppu_context* ctx, const char* fmt, char* out, size_t n)
{
    size_t o = 0;
    unsigned k = 0;
    for (const char* p = fmt; *p && o + 1 < n;) {
        if (*p != '%') {
            out[o++] = *p++;
            continue;
        }
        if (p[1] == '%') {
            out[o++] = '%';
            p += 2;
            continue;
        }
        char spec[32];
        size_t s = 0;
        spec[s++] = *p++;
        while (*p && strchr("-+ #0", *p) && s < 8) spec[s++] = *p++;
        if (*p == '*') {
            s += (size_t)snprintf(spec + s, sizeof spec - s, "%d", (int)log_arg(ctx, k++));
            p++;
        } else {
            while (*p >= '0' && *p <= '9' && s < 16) spec[s++] = *p++;
        }
        if (*p == '.') {
            spec[s++] = *p++;
            if (*p == '*') {
                s += (size_t)snprintf(spec + s, sizeof spec - s, "%d", (int)log_arg(ctx, k++));
                p++;
            } else {
                while (*p >= '0' && *p <= '9' && s < 24) spec[s++] = *p++;
            }
        }
        int longs = 0, shorts = 0;
        while (*p && strchr("hlLqjzt", *p)) {
            if (*p == 'h') shorts++;
            else if (*p == 'l' || *p == 'L' || *p == 'q' || *p == 'j') longs++;
            p++;
        }
        const char conv = *p ? *p++ : 'd';
        char piece[512];
        const uint64_t v = log_arg(ctx, k++);
        switch (conv) {
        case 'd': case 'i': {
            int64_t x = longs >= 1 ? (int64_t)v : shorts >= 2 ? (int8_t)v : shorts ? (int16_t)v : (int32_t)v;
            spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = conv; spec[s] = '\0';
            snprintf(piece, sizeof piece, spec, (long long)x);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t x = longs >= 1 ? v : shorts >= 2 ? (uint8_t)v : shorts ? (uint16_t)v : (uint32_t)v;
            spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = conv; spec[s] = '\0';
            snprintf(piece, sizeof piece, spec, (unsigned long long)x);
            break;
        }
        case 'c':
            spec[s++] = 'c'; spec[s] = '\0';
            snprintf(piece, sizeof piece, spec, (int)(char)v);
            break;
        case 'p':
            snprintf(piece, sizeof piece, "0x%08X", (uint32_t)v);
            break;
        case 's': {
            char str[400];
            guest_str((uint32_t)v, str, sizeof str);
            spec[s++] = 's'; spec[s] = '\0';
            snprintf(piece, sizeof piece, spec, str);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d;
            memcpy(&d, &v, sizeof d);
            spec[s++] = conv; spec[s] = '\0';
            snprintf(piece, sizeof piece, spec, d);
            break;
        }
        default:
            snprintf(piece, sizeof piece, "%%%c", conv);
            break;
        }
        for (const char* q = piece; *q && o + 1 < n; q++) out[o++] = (*q == '\n' || *q == '\r') ? ' ' : *q;
    }
    out[o] = '\0';
}

static int game_log_on(void)
{
    static int on = -1;
    if (on < 0) on = env_on("GOWA_GAME_LOG");
    return on;
}

/* func_001C2784, the game's log: r3 the level, r4 the format. */
extern "C" void gowa_game_log(ppu_context* ctx, uint32_t)
{
    if (!game_log_on()) return;
    char fmt[512], line[1024];
    guest_str((uint32_t)ctx->gpr[4], fmt, sizeof fmt);
    guest_format(ctx, fmt, line, sizeof line);
    size_t len = strlen(line);
    while (len && line[len - 1] == ' ') line[--len] = '\0';
    printf("[game] %u %s\n", (unsigned)ctx->gpr[3], line);
}

/* func_0035D684 after a decryption that worked: r29 the TSS buffer, r31 its owner, the size at +0x10708. */
extern "C" void gowa_tss_seen(ppu_context* ctx, uint32_t)
{
    if (!game_log_on()) return;
    const uint32_t buf = (uint32_t)ctx->gpr[29];
    const uint32_t size = guest_readable((uint32_t)ctx->gpr[31] + 0x10708, 4) ? guest_u32((uint32_t)ctx->gpr[31] + 0x10708) : 0;
    char hex[3 * 32 + 1] = "";
    if (guest_readable(buf, 32))
        for (int i = 0; i < 32; i++) snprintf(hex + 3 * i, 4, "%02x ", vm_base[buf + (uint32_t)i]);
    printf("[online] TSS decrypted: %u bytes, version %u, blocks %u: %s\n", size,
           guest_readable(buf, 16) ? guest_u32(buf) : 0, guest_readable(buf, 16) ? guest_u32(buf + 12) : 0, hex);
}

/* func_002CA2A8 returning the mask of the add-on content the game found installed (r3, from +0x700 of the content
 * manager): bit N is /DLC/DLC<N>, a licensed INDEX.EDAT the game checks. Bit 0 is the Online Pass, which the online
 * gate wants ("No Online Pass", "An Online Pass is required to play God of War: Ascension Multiplayer"). Sony
 * dropped online passes for its own games in 2013 and its store is not reachable from here, so the port reports the
 * pass as there; GOWA_ONLINE_PASS=0 leaves the mask as found. No other bit is touched: paid add-ons stay as the
 * player has them. */
extern "C" void gowa_online_pass(ppu_context* ctx, uint32_t)
{
    static int on = -1;
    if (on < 0) {
        const char* v = getenv("GOWA_ONLINE_PASS");
        on = !(v && strcmp(v, "0") == 0);
    }
    if (on) ctx->gpr[3] |= 1;
}

/* func_0040A560 about to read the transport flags: r4 the network settings. Byte 0x796 is SSL, 0x797 the threaded
 * SSL transport on top of it; samc's func_0050CB14 builds plain TCP only with both clear (SSL off with 0x797 set is
 * its error 28), so both go. */
extern "C" void gowa_sam_ssl(ppu_context* ctx, uint32_t)
{
    gowa_cheats_off("the game went online for multiplayer");
    static int keep = -1;
    if (keep < 0) keep = env_on("GOWA_SAM_SSL");
    const uint32_t flags = (uint32_t)ctx->gpr[4] + 0x796;
    if (!keep && guest_readable(flags, 2) && (vm_base[flags] || vm_base[flags + 1])) {
        vm_base[flags] = 0;
        vm_base[flags + 1] = 0;
        printf("[online] SAM: plain TCP, SSL off (our matchmaker speaks none; GOWA_SAM_SSL=1 keeps it)\n");
    }
}
