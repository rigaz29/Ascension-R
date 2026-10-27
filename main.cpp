/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"
#include "ps3emu/thread_name.h"   /* ps3_thread_name: per-thread CPU tables and debuggers */
#include "ps3emu/ps3_ftrace.h"    /* ps3_ft_dump: write a PS3_FTRACE trace on quitting */
#include "ps3emu/ps3_settings.h"  /* gowa.ini: the player's settings and this machine's paths */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#include <tlhelp32.h>   /* GOWA_THREADS: the process's thread list */
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
void     cellGame_init_from_paramsfo(const char* sfo_path);  /* libs/system/cellGame.c */

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
void     cellGcm_fifo_kick_wait(unsigned ms); /* idle wait, woken by a kick */
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" void rsx_d3d12_backend_record_ahead(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define rsx_backend_record_ahead rsx_d3d12_backend_record_ahead
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif
#if !defined(_WIN32)
#  define rsx_backend_record_ahead() ((void)0)
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

/* Milliseconds for the frame clock. GetTickCount64 advances in steps of the
 * default ~15.6 ms timer tick, whatever timeBeginPeriod asks for, so "16 ms
 * since the last flip" read 15 for a 20 ms frame about one time in four and the
 * flip then waited for the next step: the title sat 10-13 ms on its flip
 * semaphore after the drain had reached the flip. The shim's GetTickCount64
 * off Windows already has fine resolution. */
static ULONGLONG clock_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)(c.QuadPart / (freq.QuadPart / 1000));
#else
    return GetTickCount64();
#endif
}

static DWORD WINAPI frame_clock(LPVOID)
{
    ps3_thread_name("frame clock");   /* vblank ticks, the RSX drain and presents */
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    ULONGLONG next_tick = clock_ms();
    /* The title waits on its flip handler before building the next frame, so
     * every millisecond between the drain reaching a flip and the flip
     * completing is a millisecond it sits idle. Completing it only on the
     * next 16 ms beat cost ~8 ms a frame in God of War HD's gameplay. Complete
     * it when the drain reaches it, but no sooner than 16 ms after the previous
     * one, so a fast frame still waits for its vblank. GOWA_FLIP_ON_BEAT=1
     * restores beat-only completion. GOWA_FLIP_MIN_MS changes the 16 ms; 0, with
     * D3D12_VSYNC=0, lets the title run past 60 fps, for measuring it. */
    const bool flip_on_reach = getenv("GOWA_FLIP_ON_BEAT") == nullptr;
    ULONGLONG flip_min_ms = 16;
    if (const char* e = getenv("GOWA_FLIP_MIN_MS")) flip_min_ms = (ULONGLONG)atoi(e);
    unsigned drain_poll_ms = 1;
    if (const char* e = getenv("GOWA_DRAIN_POLL_MS")) drain_poll_ms = (unsigned)atoi(e);
    ULONGLONG last_flip_ms = 0;
    /* A flip the drain reached and the ticker took, still waiting for its
     * 16 ms. Nothing drains while it waits, so the frame presented when it is
     * released is still exactly that frame. */
    int held = 0;

    for (;;) {
        /* The title publishes a frame's commands at its flip and then waits
         * for the drain. Nothing kicks this wait when `put` moves, so every
         * millisecond of it is a millisecond the frame starts late: at 4 ms
         * the drain began 2-4 ms after each flip request. Check every 1 ms
         * (timeBeginPeriod(1) is set); an idle pass costs a few lock-free
         * reads. GOWA_DRAIN_POLL_MS overrides. */
        cellGcm_fifo_kick_wait(drain_poll_ms);
        ULONGLONG now = clock_ms();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && (held || cellGcm_take_flip_pending())) {
                held = 0;
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
                last_flip_ms = now;
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok && !flip_on_reach) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
                last_flip_ms = now;
            }
            cellGcm_rsx_process_fifo();
        } else if (rsx_ok) {
            /* The drain may have reached a flip, here or on the beat above:
             * hold it, and drain no further while it is held. Once 16 ms have
             * passed since the previous flip, complete it, then present that
             * frame. Completing first lets the title start its next frame
             * while the backend renders and presents (Present waits for the
             * monitor's vblank).
             *
             * A flip reached less than 16 ms after the previous one used to be
             * taken on the next pass by the present-only path above, without
             * completing it, and the title then waited for the next 16 ms beat
             * to complete it: 10-15 ms on its flip semaphore in about one frame
             * in six of God of War HD's gameplay. */
            if (!held) held = cellGcm_take_flip_pending();
            if (!held) {
                cellGcm_rsx_process_fifo();
                held = cellGcm_take_flip_pending();
            }
            if (held && clock_ms() - last_flip_ms >= flip_min_ms) {
                held = 0;
                last_flip_ms = clock_ms();
                cellGcmTickFlip();
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
        }
        /* Record the frame's draws so far while the drain has caught up
         * (D3D12_RECORD_AHEAD), instead of all of them at the frame's end,
         * when the title is waiting for the drain to reach its flip. */
        if (rsx_ok) rsx_backend_record_ahead();
        if (rsx_ok) {
            if (rsx_backend_pump() != 0) {
                /* The window was closed, which is how a player quits. Nothing
                 * else stops the title, which went on running with no window.
                 * Terminate rather than exit: guest threads are still running
                 * and may hold locks the exit path needs. */
                fprintf(stderr, "[rsx] window closed, exiting\n");
                /* A PS3_FTRACE trace is written at exit, which terminating
                 * skips; a hand-played session ends here. */
                if (g_ps3_ft.buf) ps3_ft_dump();
                fflush(stdout);
                fflush(stderr);
#ifdef _WIN32
                TerminateProcess(GetCurrentProcess(), 0);
#else
                _Exit(0);
#endif
                rsx_ok = 0;
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (!held && fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. */
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* ---------------------------------------------------------------------------
 * Hang watchdog
 * -----------------------------------------------------------------------*/
/* GOWA_WATCHDOG="10,30,50": at each of those seconds after start, print which
 * HLE call every guest thread is inside and each thread's last direct-call lr.
 * A thread parked in sys_lwcond_wait or a semaphore shows up here and nowhere
 * else, since those waits don't log. */
extern "C" const char* g_hle_inflight[64];
extern "C" void ppu_report_guest_lrs(void);
extern "C" uint32_t vm_read32(uint64_t a);

/* GOWA_PEEK="ea[,ea...]": with the watchdog, also dump 0x80 bytes at each
 * address (hex), e.g. a queue or lock line a thread is spinning on. */
static void peek_diag(const char* list)
{
    for (const char* p = list; *p; ) {
        uint32_t ea = (uint32_t)strtoul(p, NULL, 16) & ~0xFu;
        fprintf(stderr, "[WATCHDOG]   peek 0x%08X:", ea);
        for (uint32_t o = 0; o < 0x80; o += 4) {
            if (o % 16 == 0) fprintf(stderr, "\n[WATCHDOG]     +%02X:", o);
            fprintf(stderr, " %08X", vm_read32(ea + o));
        }
        fprintf(stderr, "\n");
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}

static DWORD WINAPI hang_watchdog(LPVOID arg)
{
    const char* spec = (const char*)arg;
    int elapsed = 0;
    for (const char* p = spec; *p; ) {
        int at = atoi(p);
        if (at > elapsed) { Sleep((DWORD)(at - elapsed) * 1000); elapsed = at; }
        fprintf(stderr, "[WATCHDOG] %ds\n", elapsed);
        for (unsigned t = 0; t < 64; t++)
            if (g_hle_inflight[t])
                fprintf(stderr, "[WATCHDOG]   guest-tid %-3u inside %s\n", t, g_hle_inflight[t]);
        ppu_report_guest_lrs();
        if (const char* pk = getenv("GOWA_PEEK")) peek_diag(pk);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame pipeline probe
 * -----------------------------------------------------------------------*/
/* GOWA_PIPE=<seconds>: every so many seconds, the state of the chain that
 * produces a frame, in one block (docs/ROADMAP.md, section 2):
 *   frame    the main thread's frame slot and the per-slot batch counters it
 *            waits on, (count << 16) | (wid << 8) | flags;
 *   rsx      put/get/ref, the words around get, the post-process labels;
 *   mlaa     the post-process tasks' control block and taskset bitsets;
 *   reports  the RsxSync report slots (+0xC halfword: 0 = released);
 *   spurs    the workload flag, and per Wws_Job workload its readyCount,
 *            contention, state, signal, current list and ticket rows.
 * The addresses are this build's (PSN 1.12): static data, or heap blocks the
 * title allocates at the same place every boot. GOWA_PIPE_SPURS=<ea>
 * overrides the SPURS instance. */
static uint32_t pipe_r32(uint32_t ea) { return vm_read32(ea); }
static uint32_t pipe_r16(uint32_t ea) { return (vm_read32(ea & ~3u) >> ((ea & 2) ? 0 : 16)) & 0xFFFF; }
static uint32_t pipe_r8(uint32_t ea)  { return (vm_read32(ea & ~3u) >> (24 - 8 * (ea & 3))) & 0xFF; }

static const char* const s_pipe_wkl[18] = {
    "CollisL", "HighestPrio6", "General6", "SysMisc", "ModelServer", "Network",
    "BatchJobList", "BrBDblBufWrkld", "RenJobSet", "RenJobSet", "RenJobSet", "RenJobSet",
    "RenderJobs", "MeshProcJobs", "EDGE0", "EDGE1", "EDGE2", "RsxSync",
};

static void pipe_report(unsigned t)
{
    const uint32_t frame = pipe_r32(0xA0C13C), slot = (frame + 2) % 3;
    fprintf(stderr, "[pipe] t=%us frame=%u slot=%u slot-flag=%u counters %08X %08X %08X flips=%u presented=%ld\n",
            t, frame, slot, pipe_r8(0xA0C12B + slot),
            pipe_r32(0xA0B740), pipe_r32(0xA0B744), pipe_r32(0xA0B748),
            cellGcm_flip_request_count(), (long)g_frames_presented);

    /* The command ring: the title maps IO 0 at EA 0x70000000 and keeps that
     * mapping linear (MapEaIoAddress, then MapMainMemory right after it). */
    const uint32_t put = pipe_r32(0x20002000), get = pipe_r32(0x20002004);
    fprintf(stderr, "[pipe] rsx put=%08X get=%08X ref=%08X label510=%08X label520=%08X @get:",
            put, get, pipe_r32(0x20002008), pipe_r32(0x20000510), pipe_r32(0x20000520));
    if (get < 0x7200000u)
        for (int k = -4; k < 6; k++)
            fprintf(stderr, k ? " %08X" : " [%08X]", pipe_r32(0x70000000u + get + (uint32_t)(k * 4)));
    fprintf(stderr, "\n");

    const uint32_t ctl = 0xA1FDB0, counter = pipe_r32(ctl + 8);
    const uint32_t ts = pipe_r32(0xA1FE00 + 0x34);   /* the post-process barrier's taskset */
    fprintf(stderr, "[pipe] mlaa batch=%u label=%08X counter=%08X(%08X) buffer=%08X taskset=%08X",
            pipe_r32(ctl), pipe_r32(ctl + 4), counter, counter ? pipe_r32(counter) : 0,
            pipe_r32(ctl + 12), ts);
    if (ts) {
        static const char* const bs[6] = { "run", "ready", "pend", "enabled", "signalled", "waiting" };
        for (int b = 0; b < 6; b++)   /* tasks 0-7 are the top byte */
            fprintf(stderr, " %s=%02X", bs[b], pipe_r8(ts + 0x10u * (uint32_t)b));
    }
    fprintf(stderr, "\n[pipe] reports");
    for (uint32_t i = 1; i < 8; i++)
        fprintf(stderr, " ED%X0:%u", i, pipe_r16(0x7001ED00 + 16 * i + 0xC));
    fprintf(stderr, "\n");

    uint32_t sp = 0x7001AD00;
    if (const char* e = getenv("GOWA_PIPE_SPURS")) sp = (uint32_t)strtoul(e, NULL, 16);
    if (!(pipe_r8(sp + 0x74) & 0x40)) { fprintf(stderr, "[pipe] spurs 0x%08X: not a CellSpurs2\n", sp); return; }
    const uint32_t enabled = pipe_r32(sp + 0xB0);
    fprintf(stderr, "[pipe] spurs %08X flag=%08X receiver=%u enabled=%08X signal1=%04X signal2=%04X\n",
            sp, pipe_r32(sp + 0x6C), pipe_r8(sp + 0x77), enabled, pipe_r16(sp + 0x70), pipe_r16(sp + 0x78));
    for (uint32_t w = 0; w < 18; w++) {
        if (!(enabled & (0x80000000u >> w))) continue;
        const uint32_t hi = w >= 16;
        const uint32_t ready = pipe_r8(sp + (hi ? 0x10 + (w - 16) : w));
        const uint32_t cont = (pipe_r8(sp + 0x20 + (w & 15)) >> (hi ? 4 : 0)) & 0xF;
        const uint32_t state = pipe_r8(sp + (hi ? 0xC0 + (w - 16) : 0x80 + w));
        const uint32_t sig = (pipe_r16(sp + (hi ? 0x78 : 0x70)) >> (15 - (w & 15))) & 1;
        const uint32_t info = sp + (hi ? 0x1000 + 0x20 * (w - 16) : 0xB00 + 0x20 * w);
        const uint32_t data = pipe_r32(info + 0xC);
        const uint32_t list = pipe_r32(data + 8);
        fprintf(stderr, "[pipe]   w%-2u %-14s ready=%u cont=%u state=%u sig=%u data=%08X list=%u",
                w, s_pipe_wkl[w], ready, cont, state, sig, data, list);
        /* Ticket rows (published << 16 | consumed) and the per-list words of
         * each list that has one (a four-list workload has them all). */
        for (uint32_t l = 0; l < 4; l++)
            if (pipe_r32(data + 0x30 + 4 * l))
                fprintf(stderr, " L%u{row=%08X a=%X b=%X}", l, pipe_r32(data + 0x40 + 0x10 * l),
                        pipe_r32(data + 0x10 + 4 * l), pipe_r32(data + 0x20 + 4 * l));
        fprintf(stderr, "\n");
    }
}

static DWORD WINAPI pipe_probe(LPVOID arg)
{
    unsigned every = (unsigned)(uintptr_t)arg, t = 0;
    for (;;) {
        Sleep(every * 1000u);
        t += every;
        pipe_report(t);
        fflush(stderr);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Per-thread CPU table
 * -----------------------------------------------------------------------*/
/* GOWA_THREADS=<seconds>: every so many seconds, how busy each host thread
 * was (percent of one core over the window), by the name the runtime gives it
 * ("ppu main", "spurs kernel", "spu job", "frame clock", ...), with the flips
 * and presents of the window and the threads that started and ended in it.
 * CPU of threads that both started and ended inside the window is the
 * process total less the live threads ("gone"). This is the first number to
 * read before changing anything for speed: which side of the frame is busy. */
#ifdef _WIN32
static DWORD WINAPI thread_table(LPVOID arg)
{
    const unsigned every = (unsigned)(uintptr_t)arg;
    typedef HRESULT (WINAPI *get_desc_fn)(HANDLE, PWSTR*);
    const get_desc_fn get_desc = (get_desc_fn)(void*)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    enum { MAXT = 1024 };
    static DWORD tid_prev[MAXT], tid_now[MAXT];
    static ULONGLONG cpu_prev[MAXT], cpu_now[MAXT];
    static char name_now[MAXT][48];
    int n_prev = 0;
    const DWORD pid = GetCurrentProcessId();
    auto ft64 = [](const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    auto process_cpu = [&]() {
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        return ft64(k) + ft64(u);
    };
    ULONGLONG wall_prev = GetTickCount64(), proc_prev = process_cpu();
    unsigned flips_prev = cellGcm_flip_request_count();
    LONG pres_prev = g_frames_presented;
    unsigned t = 0;
    for (;;) {
        Sleep(every * 1000u);
        t += every;
        int n = 0;
        ULONGLONG live_delta = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te; te.dwSize = sizeof te;
        int started = 0;
        for (BOOL ok = Thread32First(snap, &te); ok && n < MAXT; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c, e, k, u;
            if (!GetThreadTimes(h, &c, &e, &k, &u)) { CloseHandle(h); continue; }
            tid_now[n] = te.th32ThreadID;
            cpu_now[n] = ft64(k) + ft64(u);
            snprintf(name_now[n], sizeof name_now[n], "tid %lu", (unsigned long)te.th32ThreadID);
            PWSTR w = NULL;
            if (get_desc && SUCCEEDED(get_desc(h, &w)) && w && w[0]) {
                int i = 0;
                for (; w[i] && i < 47; i++) name_now[n][i] = (char)(w[i] < 128 ? w[i] : '?');
                name_now[n][i] = 0;
            }
            if (w) LocalFree(w);
            CloseHandle(h);
            ULONGLONG before = 0; int seen = 0;
            for (int j = 0; j < n_prev; j++) if (tid_prev[j] == tid_now[n]) { before = cpu_prev[j]; seen = 1; break; }
            if (!seen) started++;
            live_delta += cpu_now[n] - before;
            n++;
        }
        CloseHandle(snap);
        int ended = 0;
        for (int j = 0; j < n_prev; j++) {
            int alive = 0;
            for (int i = 0; i < n; i++) if (tid_now[i] == tid_prev[j]) { alive = 1; break; }
            ended += !alive;
        }
        const ULONGLONG wall = GetTickCount64(), proc = process_cpu();
        const double win = (double)(wall - wall_prev) * 1e4;   /* 100 ns units */
        const unsigned flips = cellGcm_flip_request_count();
        const LONG pres = g_frames_presented;
        const double gone = proc - proc_prev > live_delta ? (double)(proc - proc_prev - live_delta) : 0.0;
        fprintf(stderr, "[threads] t=%us flips=%.2f/s presents=%.2f/s process=%.0f%% gone=%.0f%% "
                        "threads=%d started=%d ended=%d\n",
                t, (flips - flips_prev) * 1e7 / win, (pres - pres_prev) * 1e7 / win,
                (proc - proc_prev) * 100.0 / win, gone * 100.0 / win, n, started, ended);
        /* Group by name: "spu job" x12 is one line; numbered helpers too. */
        static char gname[MAXT][48]; static double gcpu[MAXT]; static int gcount[MAXT];
        int g = 0;
        for (int i = 0; i < n; i++) {
            ULONGLONG before = 0;
            for (int j = 0; j < n_prev; j++) if (tid_prev[j] == tid_now[i]) { before = cpu_prev[j]; break; }
            const double pct = (cpu_now[i] - before) * 100.0 / win;
            char key[48]; snprintf(key, sizeof key, "%s", name_now[i]);
            if (!strncmp(key, "tid ", 4)) snprintf(key, sizeof key, "(unnamed)");
            for (char* q = key; *q; q++) if (*q >= '0' && *q <= '9' && !strncmp(key, "shader build", 12)) *q = '#';
            int k = 0;
            for (; k < g; k++) if (!strcmp(gname[k], key)) break;
            if (k == g) { snprintf(gname[g], sizeof gname[g], "%s", key); gcpu[g] = 0; gcount[g] = 0; g++; }
            gcpu[k] += pct; gcount[k]++;
        }
        for (int shown = 0; shown < 14; shown++) {
            int best = -1;
            for (int k = 0; k < g; k++) if (gcount[k] > 0 && (best < 0 || gcpu[k] > gcpu[best])) best = k;
            if (best < 0 || gcpu[best] < 0.5) break;
            fprintf(stderr, "[threads]   %6.1f%%  %-28s x%d\n", gcpu[best], gname[best], gcount[best]);
            gcount[best] = -gcount[best];
        }
        memcpy(tid_prev, tid_now, sizeof(DWORD) * n);
        memcpy(cpu_prev, cpu_now, sizeof(ULONGLONG) * n);
        n_prev = n;
        wall_prev = wall; proc_prev = proc; flips_prev = flips; pres_prev = pres;
        fflush(stderr);
    }
    return 0;
}
#endif

/* ---------------------------------------------------------------------------
 * Settings and the title's environment
 * -----------------------------------------------------------------------*/
/* gowa.ini next to the executable. */
static void default_settings_path(char* out, size_t n, const char* argv0)
{
#ifdef _WIN32
    char exe[1024];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!len || len >= sizeof exe) snprintf(exe, sizeof exe, "%s", argv0);
#else
    char exe[1024];
    snprintf(exe, sizeof exe, "%s", argv0);
#endif
    char* slash = strrchr(exe, '/');
    char* bslash = strrchr(exe, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) slash[1] = 0; else exe[0] = 0;
    snprintf(out, n, "%sgowa.ini", exe);
}

/* launcher.cpp: this port's defaults, and the settings window. */
extern "C" void gowa_port_defaults(void);
extern "C" int  gowa_launcher(void);   /* 1 = play, 0 = quit */

/* A variable the title needs, unless the environment already has one. */
static void env_default(const char* name, const char* value)
{
    if (!value || !*value || getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

/* What the title needs from its environment, so the executable also starts
 * without scripts/gowa.py. Anything already set wins, so a test can still
 * change each of these.
 *
 * The runtime switches below come from the God of War HD port, where each was
 * measured; they describe how libgcm and lv2 behave rather than that title, so
 * they are the starting point here. Check each when this title shows a problem
 * near it. */
static void title_environment(void)
{
    /* Fire the flip handler when the flip completes, as hardware does, not when
     * it is requested: a title that paces itself on the handler otherwise never
     * waits, and can write frames into its command ring faster than the RSX
     * reads them. */
    env_default("GCM_FLIPCB_ONTICK", "1");
    /* The D3D12 backend swaps any display-sized texture for a copy of the
     * previous frame, a reflection trick for another title; a full-screen
     * splash or loading texture then draws the previous frame instead. */
    env_default("SCREEN_AS_TEX", "0");
    /* A self-JUMP park in the command ring may stand for longer than the
     * runtime's 50 ms grace while a title loads, and following `put` past it
     * skips the commands it is waiting for. */
    env_default("GCM_PARK_GRACE_MS", "10000");
    /* Hand the title its command ring in 32 KB segments, as libgcm does: each
     * full segment publishes `put`, so the drain starts on a frame while the
     * title is still writing it. */
    env_default("GCM_FIFO_SEGMENT", "0x8000");
    /* The command ring waits (SEMAPHORE_ACQUIRE) on a label the SPU post-process
     * tasks write when a frame's batch is done. Without the wait the RSX runs
     * ahead of them; both other ports of this title turn it on. */
    env_default("GCM_SEMA_ACQUIRE", "1");
    /* The title keeps `put` fixed and drives the ring with self-JUMP parks, so
     * the walker's "circling" rule (several full passes without reaching put,
     * then jump to put) throws away the rest of a frame. */
    env_default("GCM_FIFO_CIRCLE_PASSES", "0");
    /* The walker takes no time between two GET_REPORTs, and RsxSync turns each
     * report into a post-process batch and its task signals: batch N+1 came
     * while the lifted tasks were still on batch N, and its signal ended their
     * barrier waits instead. A report waits while those tasks are in a round. */
    env_default("GCM_REPORT_SPU_HOLD_MS", "3000");
    /* The title gives every SPURS workload a maximum contention of 1, and the
     * three EDGE job lists (14-16) then ran one after another on SPU 0 while
     * four SPUs idled: 85% of SPU 0, the whole of a level frame's critical
     * path. Three SPUs each: menu 2.0 -> 3.6 flips/s, first level 1-3 -> 2.4-4.4
     * (docs/PERF.md). Ref A runs its EDGE lists the same way. To be checked
     * again once the level's 3D scene draws: geometry order is the risk.
     * SPURS_MAXCONT=14=1,15=1,16=1 goes back to the title's value. */
    env_default("SPURS_MAXCONT", "14=3,15=3,16=3");
    /* Each display buffer its own target, the flipped one presented: the title
     * draws its video into one buffer, tonemaps into the next and composites
     * its post chain into a third within one frame, and with a single
     * backbuffer each overwrote the frame on screen. */
    env_default("RSX_DISPLAY_RT", "1");
    /* Keep the newest copies of a save before it is written over
     * ([system] save_backups, the launcher's "Save backups"). */
    env_default("PS3_SAVEDATA_BACKUPS", ps3_settings_str("system", "save_backups", NULL, "5"));
    /* Record a frame's draws into the command list as the drain captures them,
     * in time it otherwise spends idle, rather than all at the frame's end. */
    env_default("D3D12_RECORD_AHEAD", "1");
    /* The runtime logs every lv2 wait and SPURS poll whenever stderr is not a
     * terminal; the diagnostics have their own switches. */
    env_default("PS3_VERBOSE", "0");
    /* An HDD (PSN) title. The executable is GOWA.SELF, which the title's own
     * EBOOT.BIN launcher starts from /dev_hdd0/game/<id>/USRDIR; argv[0] is that
     * path, not the disc path the runtime defaults to. */
    env_default("PS3_TITLE", "NPUA80918");
    env_default("PS3_BOOT_PATH", "/dev_hdd0/game/NPUA80918/USRDIR/GOWA.SELF");
    /* This machine's paths, written into [paths] by `gowa.py build`. */
    env_default("PS3_VFS_ROOT", ps3_settings_str("paths", "vfs", "PS3_VFS_ROOT", NULL));
    env_default("PS3_HDD0_ROOT", ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL));
    env_default("GOWA_LIBSRE_IMAGE", ps3_settings_str("paths", "libsre", "GOWA_LIBSRE_IMAGE", NULL));
    /* FFmpeg's DLLs are loaded when the first movie plays, from PATH. */
    const char* ff = ps3_settings_str("paths", "ffmpeg", NULL, NULL);
    if (ff && *ff) {
        const char* path = getenv("PATH");
        size_t n = strlen(ff) + (path ? strlen(path) : 0) + 2;
        char* v = (char*)malloc(n);
        if (v) {
#ifdef _WIN32
            snprintf(v, n, "%s;%s", ff, path ? path : "");
            _putenv_s("PATH", v);
#else
            snprintf(v, n, "%s:%s", ff, path ? path : "");
            setenv("PATH", v, 1);
#endif
            free(v);
        }
    }
}

int main(int argc, char** argv)
{
    /* gowa [--settings <file>] [--set <section.key=value>]... [--no-launcher] [<PPU ELF>]
     * The ELF defaults to [paths] elf in the settings. --no-launcher is for
     * scripts: the settings window (launcher.cpp) is not shown. */
    const char* elf = NULL;
    const char* settings = NULL;
    char settings_buf[1100];
    const char* sets[32];
    int nsets = 0, no_launcher = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--settings") && i + 1 < argc)            settings = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc)            { if (nsets < 32) sets[nsets++] = argv[++i]; else ++i; }
        else if (!strcmp(argv[i], "--no-launcher"))                    no_launcher = 1;
        else if (!elf)                                                 elf = argv[i];
    }
    if (!settings) { default_settings_path(settings_buf, sizeof settings_buf, argv[0]); settings = settings_buf; }
    const int src = ps3_settings_load(settings);
    gowa_port_defaults();
    printf("[settings] %s: %s\n", settings,
           src == 0 ? "loaded" : src == 1 ? "no file, defaults" : "could not be read, defaults");
    for (int i = 0; i < nsets; i++) {
        char sk[256];
        snprintf(sk, sizeof sk, "%s", sets[i]);
        char* dot = strchr(sk, '.');
        char* eq = strchr(sk, '=');
        if (!dot || !eq || dot > eq) { printf("[settings] --set %s: expected section.key=value\n", sets[i]); continue; }
        *dot = 0; *eq = 0;
        ps3_settings_override(sk, dot + 1, eq + 1);
    }
    /* The settings window, unless a script asked for none, or the player
     * turned it off and is not holding Shift. */
    int launcher = !no_launcher && ps3_settings_bool("launcher", "show", NULL, 1);
#ifdef _WIN32
    if (!no_launcher && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) launcher = 1;
#endif
    if (launcher && !gowa_launcher()) return 0;
    title_environment();
    if (!elf) elf = ps3_settings_str("paths", "elf", NULL, NULL);
    if (!elf || !*elf) {
        printf("usage: %s [--settings <file>] [--set <section.key=value>] [<PPU ELF>]\n"
               "No ELF given, and %s has no [paths] elf: run `scripts/gowa.py build` first.\n",
               argv[0], settings);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(elf);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", elf);
        return 1;
    }

    derive_vfs_root(elf);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Title id and CATEGORY come from PARAM.SFO. Without it cellGame keeps its
     * placeholder BLES00000 and reports a disc game, and a PSN title (HG) then
     * looks for its data under a disc layout that isn't there. */
    {
        char sfo[1100];
        snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    if (const char* wd = getenv("GOWA_WATCHDOG"))
        CreateThread(NULL, 0, hang_watchdog, (LPVOID)wd, 0, NULL);
#ifdef _WIN32
    if (const char* tt = getenv("GOWA_THREADS"))
        if (atoi(tt) > 0) CreateThread(NULL, 0, thread_table, (LPVOID)(uintptr_t)atoi(tt), 0, NULL);
#endif
    if (const char* pp = getenv("GOWA_PIPE"))
        if (atoi(pp) > 0) CreateThread(NULL, 0, pipe_probe, (LPVOID)(uintptr_t)atoi(pp), 0, NULL);
    { extern void ps3_sampler_start(void); ps3_sampler_start(); }   /* PS3_SAMPLE=<ms> */

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
