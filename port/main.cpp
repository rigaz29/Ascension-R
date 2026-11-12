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
#include "report.h"               /* what a session leaves behind */
#include "game_find.h"            /* game_file_needed: the files the game cannot start without */
#include "loading_screen.h"       /* the precompile gate: the vertex shaders first, on a screen of their own */
#include "precompile_music.h"     /* its music: --music-preview */
#include "ps3emu/ps3_settings.h"  /* ascension-r.ini: the player's settings and this machine's paths */

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
void     ppu_mm_fix(const unsigned char* coh);      /* PPU_MM_LEAN */
const unsigned char* spu_coh_bits_publish(void);
void     cellGame_init_from_paramsfo(const char* sfo_path);  /* libs/system/cellGame.c */
int      spu_job_replay_main(const char* dir);           /* runtime/spu/spu_jobcap.c */

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
unsigned cellGcm_flip_merged_count(void);     /* flips never presented */
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
extern "C" int  rsx_d3d12_backend_generating(void);
extern "C" double rsx_d3d12_backend_refresh_hz(void);
extern "C" void rsx_d3d12_backend_camera_register(int reg, int prev_reg, int prev_attr);
extern "C" void rsx_d3d12_backend_fsr3_refuse(const char* why);
extern "C" int  gowa_title_patch_wanted(const char* id);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define rsx_backend_record_ahead rsx_d3d12_backend_record_ahead
#  define rsx_backend_generating rsx_d3d12_backend_generating
#  define rsx_backend_refresh_hz rsx_d3d12_backend_refresh_hz
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
#  define rsx_backend_generating() 0
#  define rsx_backend_refresh_hz() 0.0
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

#ifdef _WIN32
/* The game ended by another process when the player closes it, or when it cannot start (game_open_failed), not by
 * itself. On a laptop whose display hangs off the other GPU, a session with frame generation on that ended itself
 * (TerminateProcess on its own process, from the window's thread or from any other) left the next session's GPU
 * stopped in a present at the boot logos: 4 of 8 starts. Ended from outside it was 0 of 8, and 0 of 8 again with the
 * window's close handled first; with this, 0 of 14 (docs/notes/2026-11-11-fg-freeze-exit.md). A copy of the exe
 * started for this alone (--end-process, at the top of main) ends this one; when it cannot, this one ends itself as
 * before. */
static void end_by_another_process(UINT code)
{
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t cmd[MAX_PATH + 48];
        swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" --end-process %lu %u", exe,
                 (unsigned long)GetCurrentProcessId(), code);
        STARTUPINFOW si;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, 5000);   /* it ends this process before it ends itself */
            CloseHandle(pi.hProcess);
        }
    }
    TerminateProcess(GetCurrentProcess(), code);
}

/* --end-process <pid> <code>: end_by_another_process's other half. */
static int end_process_cli(const char* pid, const char* code)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)strtoul(pid, NULL, 10));
    if (!h) return 1;
    TerminateProcess(h, (UINT)strtoul(code, NULL, 10));
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
    return 0;
}
#endif

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
static volatile LONG g_vblanks = 0;   /* vblank beats ticked, for GOWA_THREADS */

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

extern "C" void (*g_gcm_walker_hook)(void);
static void present_guest_frame(void)
{
    rsx_backend_present();
    /* a probe's slice for the present's own work (poll_wait.cpp, RSX_SLOW) */
    if (g_gcm_walker_hook) g_gcm_walker_hook();
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

/* Microseconds on the same counter, for the vblank beat. */
static ULONGLONG clock_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)(c.QuadPart / freq.QuadPart) * 1000000ull +
           (ULONGLONG)(c.QuadPart % freq.QuadPart) * 1000000ull / (ULONGLONG)freq.QuadPart;
#else
    return GetTickCount64() * 1000ull;
#endif
}

static DWORD WINAPI frame_clock(LPVOID)
{
    ps3_thread_name("frame clock");   /* vblank ticks, the RSX drain and presents */
    /* GOWA_FRAME_CLOCK_PRIO=<-2..2>: this thread's priority, THREAD_PRIORITY_*
     * (0 normal, the default; 1 above normal; 2 highest). It shares the CPU's
     * six cores with six SPU workers and the main thread. */
    if (const char* e = getenv("GOWA_FRAME_CLOCK_PRIO"))
        SetThreadPriority(GetCurrentThread(), atoi(e));
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "Ascension-R";

#if defined(_WIN32)
    /* The title's view-projection: c264-c267 in 85 of the 102 vertex programs of a level,
     * the rest being full-screen and HUD programs. Its motion blur's draws of what moves
     * have last frame's in c335-c338 and a vertex's last position in attribute 7. */
    rsx_d3d12_backend_camera_register(264, 335, 7);
    /* MLAA pipelines the frame: at its final composite the depth is the next frame's. */
    if (!gowa_title_patch_wanted("disable_mlaa")) rsx_d3d12_backend_fsr3_refuse("MLAA is on");
#endif
    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");
#ifdef _WIN32
    gowa_loading_screen_close();   /* the precompile screen, if there was one: the game's window is up */
#endif

    unsigned  last_flip = 0;
    /* Vblank beats at the PS3's 59.94 Hz (60000/1001), on the microsecond
     * counter, as in wave/main.cpp: `+= 16` on the millisecond clock beat at
     * 62.5 Hz. The title registers a vblank handler and flips in VSYNC mode;
     * whatever it timed by vblanks ran 4.2% fast. */
    ULONGLONG beat0 = clock_us();
    ULONGLONG beats = 0;
    /* The title waits on its flip handler before building the next frame, so
     * every millisecond between the drain reaching a flip and the flip
     * completing is a millisecond it sits idle. Completing it only on the
     * next 16 ms beat cost ~8 ms a frame in God of War HD's gameplay. Complete
     * it when the drain reaches it, but no sooner than 16 ms after the previous
     * one, so a fast frame still waits for its vblank. GOWA_FLIP_ON_BEAT=1
     * restores beat-only completion. GOWA_FLIP_MIN_MS changes the 16 ms; 0, with
     * D3D12_VSYNC=0, lets the title run past 60 fps, for measuring it. */
    const bool flip_on_reach = getenv("GOWA_FLIP_ON_BEAT") == nullptr;
    ULONGLONG flip_min_us = 16000;
    /* [display] frame_cap = 20 or 30 (the launcher's "Frame rate cap"): a flip
     * completes and is presented no sooner than ~1/cap s after the one before,
     * on the vblank beat as on the drain's own path, so with VSync on every
     * frame stays on screen for the same number of refreshes (three or two at
     * 60 Hz) instead of a mix of two, three and four -- an even pace below the
     * title's 30 rather than a judder. Off, the beat completes and presents a
     * flip whenever the drain has reached one, as before. GOWA_FLIP_MIN_MS
     * overrides the interval. */
    bool frame_cap = false;
    {
        const char* cap = ps3_settings_str("display", "frame_cap", "GOWA_FRAME_CAP", "off");
        double fps = cap ? atof(cap) : 0.0;
        /* Frame generation shows two frames for each flip, so with no cap of its own it is held
         * at what fills the display: half the refresh rate. Past the 60 a PS3 flips at, half of
         * that again, or of a third, so a frame still stays on screen for a whole number of
         * refreshes. A flip lands on the pass after its time, ~0.8 ms late: at 60 Hz 32.5 ms is
         * 59.7 frames on the display, 33.4 ms 58.1. */
        const bool generating = rsx_ok && rsx_backend_generating();
        if (generating && !(fps >= 10 && fps <= 60)) {
            double hz = atof(ps3_settings_str("graphics", "frame_generation_refresh", "D3D12_FG_REFRESH", "0"));
            if (hz <= 0.0) hz = rsx_backend_refresh_hz();
            if (hz <= 0.0) hz = 60.0;
            int n = 2;
            while (hz / n > 60.5) n += 2;
            fps = hz / n;
            fprintf(stderr, "[gowa] frame generation: a %.2f Hz display, %.2f flips a second\n", hz, fps);
            /* At half the refresh VSync holds that pace by itself, through the swap chain's queue:
             * the title gets ~10 ms ahead of the display, and that takes up what a slow frame
             * loses. A minimum as long as a frame throws it away: the gate went from 28.4 flips a
             * second to 30.0, and from 93% of frames a refresh apart to 99.5. Only the 60 of the
             * vblanks stays. */
            if (n == 2 && ps3_settings_bool("display", "vsync", "D3D12_VSYNC", 1)) fps = 60.0;
        }
        if (fps >= 10 && fps <= 60.5) {
            flip_min_us = (ULONGLONG)(1000000.0 / fps - (generating ? 833 : 2000));
            frame_cap = true;
            fprintf(stderr, "[gowa] frame cap %.2f fps: a flip no sooner than %.2f ms after the last\n",
                    fps, (double)flip_min_us / 1000.0);
        }
    }
    if (const char* e = getenv("GOWA_FLIP_MIN_MS")) flip_min_us = (ULONGLONG)atoi(e) * 1000;
    if (const char* e = getenv("GOWA_FLIP_MIN_US")) flip_min_us = (ULONGLONG)atoi(e);
    unsigned drain_poll_ms = 1;
    if (const char* e = getenv("GOWA_DRAIN_POLL_MS")) drain_poll_ms = (unsigned)atoi(e);
    /* The window's messages every 8 ms, not on every pass: 8% of this thread was in
     * PeekMessageA with an empty queue, and the drain, which writes the labels the
     * title waits on, does not run meanwhile. The pads are read by polling, so
     * nothing a player does waits for it. GOWA_PUMP_MS overrides, 0 every pass. */
    ULONGLONG pump_ms = 8;
    if (const char* e = getenv("GOWA_PUMP_MS")) pump_ms = (ULONGLONG)atoi(e);
    ULONGLONG last_pump_ms = 0;
    ULONGLONG last_flip_us = 0;
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
        const ULONGLONG now_us = clock_us();

        int fired = 0;
        while ((long long)(now_us - (beat0 + beats * 1001000ull / 60ull)) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            /* frame_cap: not before its interval since the last flip. */
            const bool cap_ok = !frame_cap || clock_us() - last_flip_us >= flip_min_us;
            if (cap_ok) cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && (held || cellGcm_take_flip_pending())) {
                if (cap_ok) {
                    held = 0;
                    present_guest_frame();
                    last_flip = cellGcm_flip_request_count();
                    last_flip_us = now_us;
                } else {
                    held = 1;
                }
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. Not past a
             * flip frame_cap holds (only it leaves one held here): the drain
             * would reach the next flip and present the two as one. */
            if (rsx_ok && !held) cellGcm_rsx_process_fifo();
            beats++;
            InterlockedIncrement(&g_vblanks);
            fired++;
        }
        if (fired >= 240) { beat0 = now_us; beats = 0; }   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok && !flip_on_reach) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
                last_flip_us = now_us;
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
            if (held && clock_us() - last_flip_us >= flip_min_us) {
                held = 0;
                last_flip_us = clock_us();
                cellGcmTickFlip();
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
        }
        /* Record the frame's draws so far while the drain has caught up
         * (D3D12_RECORD_AHEAD), instead of all of them at the frame's end,
         * when the title is waiting for the drain to reach its flip. */
        if (rsx_ok) rsx_backend_record_ahead();
        const bool pump = now - last_pump_ms >= pump_ms;
        if (pump) last_pump_ms = now;
        if (rsx_ok) {
            if (pump && rsx_backend_pump() != 0) {
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
                end_by_another_process(0);
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
    /* Top down, so above 4 GB on every PC. The runtime tells a guest address from a host pointer by its value in
     * places (cellSaveData, cellRtc: below 4 GB is the guest's), and Windows left to itself put this below 4 GB on
     * some PCs, where a host pointer into the guest's memory read as a guest address: issue #10's saves went into the
     * save folder itself, with no folder of their own. */
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
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

/* GOWA_WWS_WATCH=<s>: the ticket rows of the job lists of workloads 2-17, General6
 * to RsxSync (BatchJobList, RenJobSet, RenderJobs, MeshProcJobs and EDGE0-2
 * among them) -- polled every few microseconds
 * on a thread of its own (it keeps one host core busy). Every s seconds, per
 * list: jobs published and taken a second; how much of the time the workload
 * was running, idle with its list empty (waiting for whoever publishes), and
 * idle with entries pending (a wait entry, or the scheduler not there yet);
 * and what ran each time a job was published -- each workload that was
 * running, or "none" (a PPU thread). */
extern "C" unsigned spurs_pm_running_mask(void);
extern "C" void ps3_ft_watch_word(uint32_t ea);
static DWORD WINAPI wws_watch(LPVOID arg)
{
    ps3_thread_name("wws watch");
    const unsigned every = (unsigned)(uintptr_t)arg;
    /* GOWA_WWS_TRACE=1: only hand the rows to PS3_FTRACE (PS3_FT_LINE), once a
     * second as the lists appear; no polling, so the trace is not disturbed. */
    const bool trace_only = getenv("GOWA_WWS_TRACE") != nullptr;
    const uint32_t sp = 0x7001AD00;
    enum { W0 = 2, W1 = 17, NW = W1 - W0 + 1, NL = 4 };
    struct Row {
        uint32_t ea, last;
        double run, empty, pend, pend_n;
        unsigned pub, took, by[33];
        int pending;
    } rows[NW][NL];
    memset(rows, 0, sizeof rows);
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    auto now_us = [&]() { QueryPerformanceCounter(&c); return (double)c.QuadPart * 1e6 / (double)f.QuadPart; };
    double t_last = now_us(), t_report = t_last, t_resolve = 0;
    for (;;) {
        const double t = now_us(), dt = t - t_last;
        t_last = t;
        if (t - t_resolve > 1e6) {   /* the lists appear when the level loads */
            t_resolve = t;
            if (pipe_r8(sp + 0x74) & 0x40)
                for (uint32_t w = W0; w <= W1; w++) {
                    const uint32_t hi = w >= 16;
                    const uint32_t info = sp + (hi ? 0x1000 + 0x20 * (w - 16) : 0xB00 + 0x20 * w);
                    const uint32_t data = pipe_r32(info + 0xC);
                    for (uint32_t l = 0; l < NL; l++) {
                        Row& r = rows[w - W0][l];
                        const uint32_t ea = data && pipe_r32(data + 0x30 + 4 * l) ? data + 0x40 + 0x10 * l : 0;
                        if (ea != r.ea) { r.ea = ea; r.last = ea ? pipe_r32(ea) : 0;
                                          if (ea) ps3_ft_watch_word(ea); }
                    }
                }
        }
        if (trace_only) { Sleep(1000); continue; }
        const unsigned run = spurs_pm_running_mask();
        for (uint32_t w = W0; w <= W1; w++)
            for (uint32_t l = 0; l < NL; l++) {
                Row& r = rows[w - W0][l];
                if (!r.ea) continue;
                const uint32_t row = pipe_r32(r.ea);
                const unsigned pub = row >> 16, cons = row & 0xFFFF;
                const unsigned dpub = (pub - (r.last >> 16)) & 0xFFFF, dcons = (cons - (r.last & 0xFFFF)) & 0xFFFF;
                if (dpub && dpub < 0x8000) {
                    r.pub += dpub;
                    const unsigned others = run & ~(1u << w);
                    if (!others) r.by[32] += dpub;
                    for (unsigned b = 0; b < 32; b++)
                        if (others >> b & 1) r.by[b] += dpub;
                }
                if (dcons && dcons < 0x8000) r.took += dcons;
                r.last = row;
                if (run >> w & 1) { r.run += dt; r.pending = 0; }
                else if (pub == cons) { r.empty += dt; r.pending = 0; }
                else { r.pend += dt; if (!r.pending) r.pend_n++; r.pending = 1; }
            }
        if (t - t_report >= every * 1e6) {
            const double win = t - t_report;
            t_report = t;
            for (uint32_t w = W0; w <= W1; w++)
                for (uint32_t l = 0; l < NL; l++) {
                    Row& r = rows[w - W0][l];
                    if (!r.ea || (!r.pub && !r.took)) continue;
                    char by[256]; int o = 0;
                    for (int k = 0; k < 4; k++) {   /* the four commonest */
                        int best = -1;
                        for (int b = 0; b < 33; b++)
                            if (r.by[b] && (best < 0 || r.by[b] > r.by[best])) best = b;
                        if (best < 0) break;
                        const double share = 100.0 * r.by[best] / r.pub;
                        if (best == 32) o += snprintf(by + o, sizeof by - o, " none %.0f%%", share);
                        else o += snprintf(by + o, sizeof by - o, " w%d %.0f%%", best, share);
                        r.by[best] = 0;
                    }
                    fprintf(stderr, "[wws] w%-2u %-12s L%u: %5.0f published/s %5.0f taken/s; running %3.0f%%, idle empty %3.0f%%, "
                            "idle pending %3.0f%% (%.0f/s, %.2f ms each); published while running:%s\n",
                            w, s_pipe_wkl[w], l, r.pub * 1e6 / win, r.took * 1e6 / win, 100 * r.run / win,
                            100 * r.empty / win, 100 * r.pend / win, r.pend_n * 1e6 / win,
                            r.pend_n ? r.pend / 1000.0 / r.pend_n : 0.0, by);
                    memset(r.by, 0, sizeof r.by);
                    r.run = r.empty = r.pend = r.pend_n = 0; r.pub = r.took = 0;
                }
            fflush(stderr);
        }
        for (int i = 0; i < 64; i++) YieldProcessor();
    }
    return 0;
}

/* GCM_PARK_DUMP_MS: the job lists the frame waits on, once, when the RSX has
 * stood at a park that long -- the port's half of the runtime's park dump,
 * which does not know these lists. Per list: its ticket row (published and
 * consumed counts) and the first words of the list. */
extern "C" void (*gcm_park_dump_hook)(void);
/* GCM_PARK_DUMP_MS's SPU PUT trace: the SPURS workload the writing worker runs
 * (-1 off a policy-module worker: an SPU thread or task). */
extern "C" int (*gcm_spu_put_who_hook)(void);
extern "C" int spurs_pm_current_wid(unsigned long tid);
static int port_spu_put_who(void) { return spurs_pm_current_wid(GetCurrentThreadId()); }
extern "C" void ppu_threads_dump(const char* why);   /* runtime/syscalls/sys_ppu_thread.c */
extern "C" void spu_taskset_rounds_dump(void);         /* runtime/spu/spu_workload.c */
extern "C" void spu_sig_ring_dump(int last);           /* runtime/spu/spu_workload.c */
extern "C" void gcm_sem_ring_dump(void);               /* libs/video/cellGcmSys.c */
extern "C" void spurs_debug_dump(const char* why);     /* libs/spurs/cellSpurs.c */
extern "C" void spurs_signal_workload(uint32_t spurs_ea, uint32_t wid);   /* libs/spurs/cellSpurs.c */
extern "C" int spurs_workload_unkicked(uint32_t spurs_ea, uint32_t wid);
extern "C" void (*gcm_park_stall_hook)(uint32_t park_io, unsigned long long stood_ms);

/* GCM_PARK_STALL_MS's hook (title_environment sets it): an EDGE job list
 * (wids 14-16) whose next entry is a wait already met -- its halfword is 0 --
 * on a workload that nothing runs and nothing has kicked, is signalled.
 *
 * The title kicks an EDGE list when the MeshProc batch counter at 0xA0B724
 * reaches 0; the counter names the list ((count << 16) | (wid << 8) | flags),
 * and the list's first entry waits on it. The main thread re-arms that same
 * counter for the next frame -- the next list -- as soon as the batch is done.
 * When it did so before the kicked list's policy module had looked at its
 * wait, the wait read the re-armed count, the module went idle, and the next
 * zero kicked the next list: the first was never kicked again, and the RSX
 * stood at its first hole for good (the menu-start freeze, about one run in
 * fifteen: the opening movie's frames are trivial, so the main thread set up
 * two frames back to back). On the real machine the main thread takes a frame's
 * time between the two. The wait being met now is the very condition the
 * module waits for, so the signal only lets it see that. */
static void wws_rekick(uint32_t park_io, unsigned long long stood_ms)
{
    const uint32_t sp = 0x7001AD00;
    if (!(pipe_r8(sp + 0x74) & 0x40)) return;
    for (uint32_t w = 14; w <= 16; w++) {
        const uint32_t info = sp + (w >= 16 ? 0x1000 + 0x20 * (w - 16) : 0xB00 + 0x20 * w);
        const uint32_t data = pipe_r32(info + 0xC);
        for (uint32_t l = 0; data && l < 4; l++) {
            const uint32_t list = pipe_r32(data + 0x30 + 4 * l);
            if (!list) continue;
            const uint32_t done = pipe_r32(data + 0x40 + 0x10 * l) & 0xFFFF;
            if (done >= 0x400) continue;
            const uint32_t cmd = pipe_r32(list + 8 * done), arg = pipe_r32(list + 8 * done + 4);
            if ((cmd & 0xFF) != 2 && (cmd & 0xFF) != 5) continue;
            if (arg < 0x10000 || arg >= 0xF0000000u) continue;
            const uint32_t word = pipe_r32(arg & ~3u);
            if (((arg & 2) ? (word & 0xFFFF) : (word >> 16)) != 0) continue;
            if (spurs_workload_unkicked(sp, w) != 1) continue;
            spurs_signal_workload(sp, w);
            static int n = 0;
            if (n++ < 16)
                fprintf(stderr, "[wws-rekick] RSX at park 0x%08X for %llu ms; w%u %s L%u entry %u waits on "
                                "0x%08X, which is met, and nothing runs it: signalled\n",
                        park_io, stood_ms, w, s_pipe_wkl[w], l, done, arg);
        }
    }
}
/* The job descriptors (cmd 1 entries) of the job lists of workloads 12-17:
 * each distinct one once, its first 0xB0 bytes, and the gate word 0x9B504C. */
static void wws_jobs_dump(const char* why)
{
    const uint32_t sp = 0x7001AD00;
    if (!(pipe_r8(sp + 0x74) & 0x40)) return;
    fprintf(stderr, "[jobs-dump] %s: gate 0x9B504C = %08X; frame label [0x%08X + f(0x%X)] base words:",
            why, pipe_r32(0x9B504C), pipe_r32(0xA23514), pipe_r32(0xA23518));
    /* The label the main thread waits for at the top of its frame (func_003A4854:
     * [base + 79 * idx] == 1, then cleared), and its neighbours. */
    { const uint32_t base = pipe_r32(0xA23514), idx = pipe_r32(0xA23518), at = base + 79u * idx;
      if (base >= 0x10000 && base < 0xF0000000u)
          for (uint32_t a = (at & ~15u) - 0x10; a < (at & ~15u) + 0x20; a += 4)
              fprintf(stderr, " %08X:%08X%s", a, pipe_r32(a), a == at ? "*" : ""); }
    fprintf(stderr, "\n");
    uint32_t seen[64]; int nseen = 0;
    for (uint32_t w = 12; w <= 17; w++) {
        const uint32_t info = sp + (w >= 16 ? 0x1000 + 0x20 * (w - 16) : 0xB00 + 0x20 * w);
        const uint32_t data = pipe_r32(info + 0xC);
        for (uint32_t l = 0; data && l < 4; l++) {
            const uint32_t list = pipe_r32(data + 0x30 + 4 * l);
            if (!list) continue;
            for (uint32_t e = 0; e < 32; e++) {
                const uint32_t cmd = pipe_r32(list + 8 * e), arg = pipe_r32(list + 8 * e + 4);
                if ((cmd & 0xFF) == 4 || (!cmd && !arg)) break;
                if ((cmd & 0xFF) != 1 || arg < 0x10000 || arg >= 0xF0000000u) continue;
                int dup = 0;
                for (int k = 0; k < nseen; k++) dup |= seen[k] == arg;
                if (dup || nseen >= 64) continue;
                seen[nseen++] = arg;
                fprintf(stderr, "[jobs-dump] w%u L%u [%u] job 0x%08X size 0x%X:", w, l, e, arg, cmd >> 16);
                for (uint32_t o = 0; o < 0xB0 && o < (cmd >> 16); o += 4) fprintf(stderr, " %08X", pipe_r32(arg + o));
                fprintf(stderr, "\n");
            }
        }
    }
    fflush(stderr);
}

static void wws_rows_dump(void)
{
    const uint32_t sp = 0x7001AD00;
    if (!(pipe_r8(sp + 0x74) & 0x40)) { fprintf(stderr, "[park-dump] job lists not set up yet\n"); return; }
    /* Workloads 12-17, and any other whose policy module is RsxSync's (the
     * same job-list layout). */
    const uint32_t wws_pm = pipe_r32(sp + 0x1020 + 4);
    fprintf(stderr, "[park-dump] policy module/data per workload:");
    for (uint32_t w = 0; w <= 17; w++) {
        const uint32_t info = sp + (w >= 16 ? 0x1000 + 0x20 * (w - 16) : 0xB00 + 0x20 * w);
        fprintf(stderr, " w%u %X/%X", w, pipe_r32(info + 4), pipe_r32(info + 0xC));
    }
    fprintf(stderr, "\n");
    for (uint32_t w = 0; w <= 17; w++) {
        const uint32_t info = sp + (w >= 16 ? 0x1000 + 0x20 * (w - 16) : 0xB00 + 0x20 * w);
        if (w < 12 && pipe_r32(info + 4) != wws_pm) continue;
        const uint32_t data = pipe_r32(info + 0xC);
        for (uint32_t l = 0; data && l < 4; l++) {
            const uint32_t list = pipe_r32(data + 0x30 + 4 * l);
            if (!list) continue;
            const uint32_t row = data + 0x40 + 0x10 * l;
            const uint32_t t = pipe_r32(row);
            /* What the main thread's wait for a list (func_0049D688) waits for:
             * every policy-module slot's count (row halfwords 1-7, 0xFFFF for
             * an unused slot) up to the first empty entry at or after the
             * published count. */
            uint32_t end = t >> 16, lo = 0xFFFF;
            while (end < 0x400 && (pipe_r32(list + 8 * end) & 0xFFFF)) end++;
            for (uint32_t h = 2; h < 16; h += 2) lo = pipe_r16(row + h) < lo ? pipe_r16(row + h) : lo;
            char inc[64] = "";
            if (lo < end) snprintf(inc, sizeof inc, " (INCOMPLETE: entries to %u, slots done to %u)", end, lo);
            fprintf(stderr, "[park-dump] w%-2u %-12s L%u: data 0x%08X published %u consumed %u%s%s; row %08X %08X %08X %08X; "
                            "list 0x%08X: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                    w, s_pipe_wkl[w], l, data, t >> 16, t & 0xFFFF, (t >> 16) != (t & 0xFFFF) ? " (pending)" : "", inc,
                    t, pipe_r32(row + 4), pipe_r32(row + 8), pipe_r32(row + 12), list,
                    pipe_r32(list), pipe_r32(list + 4), pipe_r32(list + 8), pipe_r32(list + 12),
                    pipe_r32(list + 16), pipe_r32(list + 20), pipe_r32(list + 24), pipe_r32(list + 28));
            /* Every entry up to the first end (cmd 4) or 32: {cmd, arg}, 1 a job, 3 a nop,
             * 2 and 5 a wait on the halfword at arg (shown as it reads now). */
            fprintf(stderr, "[park-list] w%u L%u:", w, l);
            for (uint32_t e = 0; e < 32; e++) {
                const uint32_t cmd = pipe_r32(list + 8 * e), arg = pipe_r32(list + 8 * e + 4);
                const uint32_t ty = cmd & 0xFF;
                if (ty == 2 || ty == 5) {
                    const uint32_t wd = pipe_r32(arg & ~3u);
                    fprintf(stderr, " [%u]wait%u(%08X)=%04X", e, ty, arg, (arg & 2) ? wd & 0xFFFF : wd >> 16);
                } else if (ty == 1) fprintf(stderr, " [%u]job(%08X,%X)", e, arg, cmd >> 16);
                else fprintf(stderr, " [%u]%08X:%08X", e, cmd, arg);
                if (ty == 4 || (!cmd && !arg)) break;
            }
            fprintf(stderr, "\n");
        }
    }
    /* The main thread's batch counters ((count << 16) | (wid << 8) | flags, waited to
     * zero per frame slot) and the MLAA control block (batch, label EA, counter EA,
     * buffer), this build's addresses (docs/notes/2026-10-27-frame-pipeline.md). */
    fprintf(stderr, "[park-dump] batch counters 0xA0B6C0:");
    for (uint32_t a = 0xA0B6C0; a < 0xA0B780; a += 4) fprintf(stderr, "%s%08X", (a & 0xF) ? " " : " | ", pipe_r32(a));
    fprintf(stderr, "\n[park-dump] MLAA block 0xA1FDB0:");
    for (uint32_t a = 0xA1FDB0; a < 0xA1FDE0; a += 4) fprintf(stderr, " %08X", pipe_r32(a));
    const uint32_t lab = pipe_r32(0xA1FDB4), cnt = pipe_r32(0xA1FDB8);
    if (lab >= 0x10000 && lab < 0xF0000000u) fprintf(stderr, "; label now %08X", pipe_r32(lab));
    if (cnt >= 0x10000 && cnt < 0xF0000000u) fprintf(stderr, "; counter now %08X", pipe_r32(cnt));
    fprintf(stderr, "\n");
    /* The MLAA barriers (count at +0, waiter mask at +0x10; 0xA1FE00, 0xA1FE80). */
    for (uint32_t b = 0xA1FE00; b <= 0xA1FE80; b += 0x80) {
        fprintf(stderr, "[park-dump] barrier 0x%08X:", b);
        for (uint32_t a = b; a < b + 0x20; a += 4) fprintf(stderr, " %08X", pipe_r32(a));
        fprintf(stderr, "\n");
    }
    wws_jobs_dump("park");
    /* What each guest thread is waiting in, the signal-driven tasks, and the
     * SPU_SIG_TRACE ring (SPU_SIG_RING=1). */
    ppu_threads_dump("park");
    spu_taskset_rounds_dump();
    spu_sig_ring_dump(8192);
}

/* SPU_SIG_RING_DUMP_S=<s>[,<s>...]: what a park dump shows (the job lists,
 * counters, guest threads, SPURS slots, the SPU_SIG_TRACE ring with
 * SPU_SIG_RING=1) at these seconds after start, for a run that stalls without
 * a park, or does not stall at all. */
static DWORD WINAPI sig_ring_dumper(LPVOID arg)
{
    const char* e = (const char*)arg;
    const ULONGLONG t0 = GetTickCount64();
    while (e && *e) {
        const double s = atof(e);
        const ULONGLONG at = t0 + (ULONGLONG)(s * 1000.0);
        while (GetTickCount64() < at) Sleep(50);
        fprintf(stderr, "[sig-ring] dump at %.1f s\n", s);
        spurs_debug_dump("timed");
        wws_rows_dump();
        gcm_sem_ring_dump();
        while (*e && *e != ',') e++;
        if (*e == ',') e++;
    }
    return 0;
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
    unsigned merged_prev = cellGcm_flip_merged_count();
    LONG pres_prev = g_frames_presented;
    LONG vbl_prev = g_vblanks;
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
        const unsigned merged = cellGcm_flip_merged_count();
        const LONG pres = g_frames_presented;
        const LONG vbl = g_vblanks;
        const double gone = proc - proc_prev > live_delta ? (double)(proc - proc_prev - live_delta) : 0.0;
        fprintf(stderr, "[threads] t=%us flips=%.2f/s presents=%.2f/s skipped=%.2f/s vblanks=%.2f/s "
                        "process=%.0f%% gone=%.0f%% threads=%d started=%d ended=%d\n",
                t, (flips - flips_prev) * 1e7 / win, (pres - pres_prev) * 1e7 / win,
                (merged - merged_prev) * 1e7 / win, (vbl - vbl_prev) * 1e7 / win,
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
        wall_prev = wall; proc_prev = proc; flips_prev = flips; merged_prev = merged; pres_prev = pres; vbl_prev = vbl;
        fflush(stderr);
    }
    return 0;
}
#endif

/* ---------------------------------------------------------------------------
 * Settings and the title's environment
 * -----------------------------------------------------------------------*/
/* ascension-r.ini beside the executable for a build (a settings file is already there) or a copy
 * the player marked with portable.txt, else in %LOCALAPPDATA%\Ascension-R. The logs, the saves and
 * the caches go where it is. */
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
    snprintf(out, n, "%sascension-r.ini", exe);
#ifdef _WIN32
    char old_name[1100], marker[1100];
    snprintf(old_name, sizeof old_name, "%sgowa.ini", exe);
    snprintf(marker, sizeof marker, "%sportable.txt", exe);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES || GetFileAttributesA(old_name) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) {
        /* The name before the rename: copied once, and the old file is left as it is. */
        CopyFileA(old_name, out, TRUE);
        return;
    }
    const char* local = getenv("LOCALAPPDATA");
    if (local && *local) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s\\Ascension-R", local);
        CreateDirectoryA(dir, NULL);
        snprintf(out, n, "%s\\ascension-r.ini", dir);
    }
#endif
}

#ifdef _WIN32
#include <io.h>
/* ---------------------------------------------------------------------------
 * The session's log
 *
 * Played from the launcher, the game's output went to its console and went
 * with it: a session that ended by itself left nothing to read. When stdout
 * is a console (not a file or a pipe a script reads), stdout and stderr now go
 * to <the settings' folder>\logs\session-<date>-<time>.log, the five newest
 * kept, and the console says where. How the session ended is in it: "[rsx]
 * window closed", "[sysPrxForUser] sys_process_exit", "[quit]", or a crash's
 * code, address and stack ([crash], crash_report).
 *
 * Beside it, session-<date>-<time>.pad records what the game read from the
 * pad (cellPad_record_to in cellPad.c), so a scene reached in play can be
 * reached again by PAD_REPLAY.
 *
 * logs/missed_code.txt keeps, over the sessions, the code that ran without a
 * lift of it (ps3_missed_code_to in runtime/diag/missed_code.c); `gowa.py
 * lift` reads it. PS3_MISSED_CODE=<file> records to that file instead, in any
 * run, and =0 not at all.
 * -----------------------------------------------------------------------*/
extern "C" void cellPad_record_to(const char* path);
extern "C" void ps3_missed_code_to(const char* path);

static char s_log_path[1100];

static int log_name_cmp(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

/* The four newest files in dir matching pat kept, so five with this session's;
 * their names sort by time. */
static void session_prune(const char* dir, const char* pat_name)
{
    static char names[256][64];
    int n = 0;
    char pat[1200];
    snprintf(pat, sizeof pat, "%s\\%s", dir, pat_name);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (n < 256 && strlen(fd.cFileName) < 64) snprintf(names[n++], 64, "%s", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(names, (size_t)n, sizeof names[0], log_name_cmp);
    for (int i = 0; i + 4 < n; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s\\%s", dir, names[i]);
        DeleteFileA(p);
    }
}

static void session_log(const char* settings_path)
{
    const DWORD ft = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    if (ft == FILE_TYPE_DISK || ft == FILE_TYPE_PIPE) return;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", settings_path);
    char* sl = strrchr(dir, '\\');
    char* fs = strrchr(dir, '/');
    if (fs && (!sl || fs > sl)) sl = fs;
    if (sl) sl[1] = 0; else dir[0] = 0;
    strncat(dir, "logs", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    session_prune(dir, "session-*.log");
    session_prune(dir, "session-*.pad");
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(s_log_path, sizeof s_log_path, "%s\\session-%04u%02u%02u-%02u%02u%02u.log", dir, st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (!freopen(s_log_path, "w", stdout)) { s_log_path[0] = 0; return; }
    _dup2(_fileno(stdout), _fileno(stderr));   /* one file, one position */
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    char pad[1100];
    snprintf(pad, sizeof pad, "%s", s_log_path);
    const size_t pl = strlen(pad);
    if (pl > 4) {
        memcpy(pad + pl - 4, ".pad", 4);
        cellPad_record_to(pad);
    }
    if (!getenv("PS3_MISSED_CODE")) {
        char missed[1200];
        snprintf(missed, sizeof missed, "%s\\missed_code.txt", dir);
        ps3_missed_code_to(missed);
    }
    HANDLE con = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (con != INVALID_HANDLE_VALUE) {
        char m[1300];
        const int k = snprintf(m, sizeof m, "This session's log: %s\r\n", s_log_path);
        DWORD w = 0;
        WriteFile(con, m, (DWORD)k, &w, NULL);
        CloseHandle(con);
    }
}

/* An exception nothing handled: its code, address and stack into the log,
 * then on as before (the runtime's filter flushes the log, Windows reports). */
static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_crash;
static LONG WINAPI crash_report(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    char w[400];
    gowa_where(er->ExceptionAddress, w, sizeof w);
    fprintf(stderr, "[crash] exception 0x%08lX at %s, thread %lu", (unsigned long)er->ExceptionCode, w,
            (unsigned long)GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, ", %s 0x%llX", er->ExceptionInformation[0] == 1 ? "writing" :
                                       er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                (unsigned long long)er->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    CONTEXT c = *ep->ContextRecord;
    for (int i = 0; i < 32 && c.Rip; i++) {
        gowa_where((const void*)c.Rip, w, sizeof w);
        fprintf(stderr, "[crash]   #%d %s\n", i, w);
        DWORD64 img = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &img, NULL);
        if (!fe) break;   /* a frame without unwind data: the walk stops there */
        PVOID hd = NULL;
        DWORD64 est = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c.Rip, fe, &c, &hd, &est, NULL);
    }
    fflush(stderr);
    return s_prev_crash ? s_prev_crash(ep) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

/* launcher.cpp: this port's defaults, and the settings window. */
extern "C" void gowa_port_defaults(void);
extern "C" int  gowa_launcher(void);   /* 1 = play, 0 = quit */
extern "C" int  gowa_prepare_cli(const char* game, const char* rpcs3, const char* rap, const char* settings);   /* game_prepare.cpp */
/* patches.cpp: the title patches switched on in [patches], applied once the
 * ELF is in guest memory and before the title runs. */
extern "C" void gowa_title_patches_apply(void);
#ifdef PORT_GAME_MODULE
extern "C" int game_module_load(void);        /* game_module_host.c */
extern "C" void game_module_sync(void);
extern "C" int game_module_state(void);
#endif
extern "C" void gowa_cheats_apply(void);
extern "C" void ps3_pad_tune(double stick_full, int flick_hold);
/* precompile.cpp: the game's vertex programs into the shader cache, without the game. */
extern "C" int gowa_precompile(void);

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
    /* Stop the drain at every flip. With the walker 64 KB or more behind the
     * title (the runtime's default), it went on past a flip: a frame clock
     * thread that fell behind then presented two to six frames as one, 150-700
     * ms apart, and a batch of several frames could fill and lose its last flip,
     * showing a display buffer mid-frame (full-screen colour flashes). The
     * title waits for its flips, so it never runs far ahead. ABBA, three runs
     * each, first fight: flips 20.67 -> 20.72, presents 19.96 -> 20.72, gaps of
     * 100 ms or more 18-31 -> 1 a run, the longest 248-717 -> 105-152 ms
     * (docs/notes/2026-11-02-fight-stutter.md). */
    env_default("GCM_FIFO_CATCHUP_BYTES", "0x1000000");
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
    /* An EDGE job list the title kicked before it had looked at the counter
     * its first entry waits on could miss its turn and never be kicked again;
     * the RSX then stood at its first hole for good (the menu-start freeze).
     * Once a park has stood 100 ms, wws_rekick signals such a list. */
    env_default("GCM_PARK_STALL_MS", "100");
    /* No SPURS_MAXCONT default any more. It set the EDGE lists (14-16) to three
     * SPUs because every workload arrived with a maximum contention of 1: the
     * HLE dropped _cellSpursWorkloadAttributeInitialize's ninth argument. The
     * title's own maxima (5, 6 or 8; docs/notes/2026-10-31-rpcs3-vs-port.md)
     * now reach the instance, and SPURS_MAXCONT stays an override for tests. */
    /* The title mixes its audio in SPURS workload 7, and a block that
     * workload has not filled when cellAudio takes it plays as a short gap.
     * Its worker at above-normal priority, with the paced mixer: 27-35 gaps
     * per 5 s in the first level -> 4-6, silent blocks 10-11% -> 8-10%,
     * flips/s unchanged (docs/notes/2026-10-28-rendering-accuracy-plan.md, 3.4).
     * SPURS_BOOST_WID=none leaves every worker at normal priority. */
    env_default("SPURS_BOOST_WID", "7");
    /* [system] steady_audio (the launcher's "Steady audio", on by default): workload 7, the
     * title's audio, has a host worker of its own, as SPU 5, the only SPU the title lets it
     * run on (SPURS_LANE=7:5). SPU 5's own worker takes what the title's table gives it
     * otherwise. In the parasites' fight a kick of it waited up to 8-16 ms for SPU 5 while
     * other workloads' jobs held it -- past the ~16 ms of blocks the title keeps ahead, so the
     * blocks played as gaps (docs/notes/2026-11-03-parasite-stutter.md). Keeping SPU 5 for it
     * (SPURS_RESERVE=7:5, lent to the short jobs with SPURS_LEND) ended that, but left half an
     * SPU idle with the other five full: at the gate 20.67 flips/s against 22.98 with a worker
     * of its own, in the scripted fight 20.35 against 21.26, the ring as far ahead (performance
     * plan, 2.25). SPURS_RESERVE set by hand takes its place. */
    if (ps3_settings_bool("system", "steady_audio", "GOWA_STEADY_AUDIO", 1) && !getenv("SPURS_RESERVE"))
        env_default("SPURS_LANE", "7:5");
    /* [system] speakers (the launcher's "Speakers"): the output's channels. The title's port is 7.1
     * (PortOpen nChannel=8), filled as its own Sound Mode says: two channels on Stereo, the default;
     * auto follows the device's own layout, and Windows folds a layout it lacks down. raw_audio
     * ("Bypass sound effects"): the stream skips the APOs where the device has a raw path. The
     * runtime's switches: libs/audio/cellAudio.c (docs/notes/2026-11-11-audio-output.md). */
    env_default("PS3_AUDIO_CHANNELS", ps3_settings_str("system", "speakers", "PS3_AUDIO_CHANNELS", "auto"));
    env_default("PS3_AUDIO_RAW", ps3_settings_bool("system", "raw_audio", "PS3_AUDIO_RAW", 0) ? "1" : "0");
    /* Each display buffer its own target, the flipped one presented: the title
     * draws its video into one buffer, tonemaps into the next and composites
     * its post chain into a third within one frame, and with a single
     * backbuffer each overwrote the frame on screen. */
    env_default("RSX_DISPLAY_RT", "1");
    /* A new render target starts empty rather than with what guest memory
     * holds there, as RPCS3's reference setup for the title has it (Read
     * Color Buffers off): the first loading frames composite the scene target
     * before any scene is drawn into it, and ~1 MB of old data in that memory
     * showed as a band of noise. */
    env_default("RSX_RT_READ", "0");
    /* SPU single precision on the host's own float ops: SPU code runs with
     * MXCSR at round toward zero with FTZ and DAZ, which is how the SPU
     * rounds, so fa/fm/fma need no exact emulation (bit-identical results,
     * runtime/spu/tests/test_spu_float_exact.c). That emulation was ~70% of
     * the menu's limiting workload (docs/notes/2026-10-28-l7-analysis.md).
     * SPU_FLOAT_RZ=0 goes back to it. */
#ifdef PORT_SPU_FLOAT_RZ_ONLY
    /* Not a choice in this build: the lifted SPU code carries only the
     * round-toward-zero forms (CMakeLists.txt, PORT_SPU_FLOAT_RZ_ONLY), which
     * are right only in that mode. */
    if (const char* e = getenv("SPU_FLOAT_RZ"))
        if (strcmp(e, "1") != 0)
            fprintf(stderr, "[gowa] SPU_FLOAT_RZ=%s ignored: this build's SPU code needs SPU_FLOAT_RZ=1\n", e);
#ifdef _WIN32
    _putenv_s("SPU_FLOAT_RZ", "1");
#else
    setenv("SPU_FLOAT_RZ", "1", 1);
#endif
#else
    env_default("SPU_FLOAT_RZ", "1");
#endif
    /* Every MFC channel access looks up its context's DMA engine: a shared
     * lock and a walk over every context's engine, and the policy modules
     * keep one context per workload and SPU. That was ~10% of the SPU
     * workers' time in the first level. The runtime's per-thread memo of the
     * last context hits for a whole run (docs/notes/2026-10-28-level-after-l7.md).
     * SPU_MFC_LOOKUP_CACHE=0 goes back to the locked walk. */
    env_default("SPU_MFC_LOOKUP_CACHE", "1");
    /* The job modules' computed jumps back to an entry of their own function
     * as gotos (lift --ijmp-local): the audio worker's branch-free loops had
     * spent ~18% of its time in the SPU dispatcher, and it went from 0.69 to
     * 0.56 of a core. Off at first: the frame clock thread then fell behind
     * more often. With the drain stopping at every flip and that thread at the
     * highest priority, ABBA, three runs each: 20.70 -> 21.33 flips/s (+3.0%),
     * every frame presented (docs/notes/2026-10-29-performance-plan.md, 2.17
     * and 2.18).
     * SPU_IJMP_LOCAL=0 turns it off. */
    env_default("SPU_IJMP_LOCAL", "1");
    /* The frame clock thread at THREAD_PRIORITY_HIGHEST: it walks the RSX
     * commands, records the draws and presents, on six cores shared with six
     * SPU workers and the main thread. ABBA, two runs each, first fight: flips
     * 20.85 (normal), 21.10 (above normal), 21.23 (highest); the longest present
     * gaps 157-161 -> 105-115 ms; the audio ring's gaps unchanged, 0.2 per 5 s
     * (docs/notes/2026-11-02-fight-stutter.md). GOWA_FRAME_CLOCK_PRIO=0: normal. */
    env_default("GOWA_FRAME_CLOCK_PRIO", "2");
    /* Keep the newest copies of a save before it is written over
     * ([system] save_backups, the launcher's "Save backups"). */
    env_default("PS3_SAVEDATA_BACKUPS", ps3_settings_str("system", "save_backups", NULL, "5"));
    env_default("VTX_RESIDENT_MB", "256");      /* EDGE's kept results in GPU memory (rsx_kept.h) */
    /* Record a frame's draws into the command list as the drain captures them,
     * in time it otherwise spends idle, rather than all at the frame's end. */
    env_default("D3D12_RECORD_AHEAD", "1");
    /* The runtime logs every lv2 wait and SPURS poll whenever stderr is not a
     * terminal; the diagnostics have their own switches. */
    env_default("PS3_VERBOSE", "0");
    /* An HDD (PSN) title. The executable is GOWA.SELF, which the title's own
     * EBOOT.BIN launcher starts from /dev_hdd0/game/<id>/USRDIR; argv[0] is that
     * path, not the disc path the runtime defaults to. */
    /* The runtime halts an SPU that repeats one transfer for 30 s, as wedged. The job policy
     * modules poll for work that way, and building pipelines on a first start can hold the game
     * up for longer, so the halt stopped idle workers and the next load hung. */
    env_default("SPU_DMA_REPEAT_SECS", "604800");
    env_default("PS3_TITLE", "NPUA80918");
    env_default("PS3_BOOT_PATH", "/dev_hdd0/game/NPUA80918/USRDIR/GOWA.SELF");
    /* This machine's paths, written into [paths] by `gowa.py build`. */
    env_default("PS3_VFS_ROOT", ps3_settings_str("paths", "vfs", "PS3_VFS_ROOT", NULL));
    env_default("PS3_HDD0_ROOT", ps3_settings_str("paths", "hdd0", "PS3_HDD0_ROOT", NULL));
    /* A mod folder, laid out like the PS3's paths (dev_hdd0\game\NPUA80918\USRDIR\...): a file
     * there replaces the game's or adds one. Empty (the default): none. */
    env_default("PS3_MODS_ROOT", ps3_settings_str("paths", "mods", "PS3_MODS_ROOT", NULL));
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

#if defined(PORT_SPU_PGO_GEN)
/* PORT_SPU_PGO=gen: the lifted SPU code is instrumented. Its profile goes to LLVM_PROFILE_FILE
 * every 15 s: the bench closes the game with WM_CLOSE and kills it 3 s later, so the write at
 * exit cannot be counted on. */
extern "C" int __llvm_profile_write_file(void);
static DWORD WINAPI pgo_writer(void*)
{
    for (;;) {
        Sleep(15000);
        __llvm_profile_write_file();
    }
}
#endif

#ifdef _WIN32
/* ---------------------------------------------------------------------------
 * A file the game cannot start without, and it could not be opened
 *
 * The title opens USRDIR's three lists of archives and the archives at its
 * start (game_files.inc). Without one it went on and stopped by itself a few
 * seconds later, a black window and nothing said, or, without the updates',
 * waited on its first loading screen (issue #2). The launcher does not start
 * a game whose folder lacks them (game_probe), but a script, the launcher
 * turned off, or a file gone or locked since can still get here: the log, and
 * a message unless a script started the game, say which file, and it stops.
 * -----------------------------------------------------------------------*/
extern "C" void (*g_ps3_open_failed_hook)(const char* guest_path, const char* host_path);   /* ppu_fs.cpp */
static int s_open_failed_says;

static void game_open_failed(const char* gpath, const char* hpath)
{
    static const char root[] = "/dev_hdd0/game/NPUA80918/";
    const size_t k = sizeof root - 1;
    if (_strnicmp(gpath, root, k) || !game_file_needed(gpath + k)) return;
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) Sleep(INFINITE);   /* another thread says it, and ends the game */
    char host[1100];
    snprintf(host, sizeof host, "%s", hpath);
    for (char* c = host; *c; c++)
        if (*c == '/') *c = '\\';
    fprintf(stderr, "[quit] the game's folder has no %s, or it could not be read: %s\n", gpath + k, host);
    fflush(stdout);
    fflush(stderr);
    if (s_open_failed_says) {
        wchar_t m[1600];
        swprintf(m, sizeof m / sizeof m[0],
                 L"The game cannot start: its folder has no %hs, or it could not be read.\n\n%hs\n\n%ls",
                 gpath + k, host,
                 game_file_update(gpath + k)
                     ? L"That file comes with one of the game's updates. Install the updates 1.04 to 1.12 in RPCS3, "
                       L"in that order (1.12's own is the EBOOT alone), and pick the game's folder in the launcher's "
                       L"System tab."
                     : L"It needs the whole game, as RPCS3 or the game's PKG installs it. Pick that folder in the "
                       L"launcher's System tab.");
        MessageBoxW(NULL, m, L"Ascension-R", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }
    /* Ended as when the window closes, by another process: guest threads are still running and may hold locks an exit
     * needs, and with frame generation on its swap chain may be up already. */
    end_by_another_process(3);
}
#endif

int main(int argc, char** argv)
{
    /* gowa [--settings <file>] [--set <section.key=value>]... [--no-launcher] [<PPU ELF>]
     * The ELF defaults to [paths] elf in the settings. --no-launcher is for
     * scripts: the settings window (launcher.cpp) is not shown.
     * --precompile-shaders: compile the game's vertex programs into the shader
     * cache and quit, the game not run (precompile.cpp; the precompile gate
     * runs it, loading_screen.cpp).
     * --report [--with-saves]: make a problem report of the last sessions and quit (report.cpp;
     * the launcher's Report a problem).
     * --end-process <pid> <code>: end that process and quit (end_by_another_process). */
#ifdef _WIN32
    if (argc == 4 && !strcmp(argv[1], "--end-process")) return end_process_cli(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "--loading-preview")) return gowa_loading_preview(argv[2]);
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--music-preview"))
        return gowa_music_preview(argv[2], argc == 4 ? atof(argv[3]) : 100.0);
#endif
    const char* elf = NULL;
    const char* settings = NULL;
    char settings_buf[1100];
    const char* sets[32];
    int nsets = 0, no_launcher = 0, precompile = 0, report = 0, report_saves = 0;
    const char* prepare = NULL, *prep_rpcs3 = NULL, *prep_rap = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--settings") && i + 1 < argc)            settings = argv[++i];
        else if (!strcmp(argv[i], "--set") && i + 1 < argc)            { if (nsets < 32) sets[nsets++] = argv[++i]; else ++i; }
        else if (!strcmp(argv[i], "--no-launcher"))                    no_launcher = 1;
        else if (!strcmp(argv[i], "--precompile-shaders"))             precompile = 1;
        else if (!strcmp(argv[i], "--report"))                         report = 1;
        else if (!strcmp(argv[i], "--with-saves"))                     report_saves = 1;
        else if (!strcmp(argv[i], "--prepare-game") && i + 1 < argc)   prepare = argv[++i];
        else if (!strcmp(argv[i], "--rpcs3") && i + 1 < argc)          prep_rpcs3 = argv[++i];
        else if (!strcmp(argv[i], "--rap") && i + 1 < argc)            prep_rap = argv[++i];
        else if (!elf)                                                 elf = argv[i];
    }
    if (!settings) { default_settings_path(settings_buf, sizeof settings_buf, argv[0]); settings = settings_buf; }
    if (prepare) return gowa_prepare_cli(prepare, prep_rpcs3, prep_rap, settings);
#ifdef _WIN32
    if (!precompile && !report) {
        session_log(settings);
        s_prev_crash = SetUnhandledExceptionFilter(crash_report);
    }
#endif
    { const char* m = getenv("PS3_MISSED_CODE");
      if (!precompile && m && *m && strcmp(m, "0")) ps3_missed_code_to(m); }
#if defined(PORT_SPU_PGO_GEN)
    CreateThread(NULL, 0, pgo_writer, NULL, 0, NULL);
#endif
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
    if (report) {
        char made[1200];
        if (gowa_report_create(report_saves, made, sizeof made)) {
            printf("[report] could not be made\n");
            return 1;
        }
        printf("[report] %s\n", made);
        return 0;
    }
#ifdef _WIN32
    if (!precompile && s_log_path[0]) gowa_report_session_start(s_log_path);
#endif
    if (precompile) {
        title_environment();   /* the environment the game runs in: the same shader cache keys */
        return gowa_precompile();
    }
#ifdef _WIN32
    s_open_failed_says = !no_launcher;
    g_ps3_open_failed_hook = game_open_failed;
#endif
    /* The settings window, unless a script asked for none, or the player
     * turned it off and is not holding Shift. */
    int launcher = !no_launcher && ps3_settings_bool("launcher", "show", NULL, 1);
#ifdef _WIN32
    if (!no_launcher && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) launcher = 1;
    /* No game where the settings say: the launcher looks for it, whatever the setting. */
    if (!no_launcher && !elf) {
        const char* e = ps3_settings_str("paths", "elf", NULL, NULL);
        if (!e || !*e || GetFileAttributesA(e) == INVALID_FILE_ATTRIBUTES) launcher = 1;
    }
#ifdef PORT_GAME_MODULE
    /* The game's code not built here yet, or built for another version: the launcher builds it. */
    if (!no_launcher && game_module_state() != 0) launcher = 1;
#endif
#endif
    if (launcher && !gowa_launcher()) return 0;
    title_environment();
#ifdef PORT_GAME_MODULE
    if (!game_module_load()) return 1;
#endif
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
    ppu_mm_fix(spu_coh_bits_publish());

    uint32_t entry = ppu_load_elf(elf);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", elf);
        return 1;
    }
    gowa_title_patches_apply();   /* [patches]: the ones switched on (patches.cpp) */
    gowa_cheats_apply();          /* [cheats]: cheats.cpp */
#ifdef PORT_GAME_MODULE
    game_module_sync();
#endif
    /* The evade smooths the right stick over its reads and wants ~0.87: a flick at 30 frames a
     * second is two reads at most. The whole of it, cellPad.c. */
    ps3_pad_tune(0.85, 4);

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

    /* SPU_JOB_REPLAY=<dir>: run the SPU jobs an SPU_JOB_CAPTURE run recorded,
     * time them and compare their output, instead of the game. The ELF is
     * loaded (the jobs' code regions) and the lifted SPU code registered; no
     * PPU thread, no GPU. */
    if (const char* rd = getenv("SPU_JOB_REPLAY"))
        return spu_job_replay_main(rd);

#ifdef _WIN32
    /* The vertex shaders built for this version before anything of the game's runs, on a screen of their own
     * where its window is about to be (loading_screen.cpp). The frame clock takes the screen down once the
     * game's window is up. */
    gowa_precompile_gate();
#endif
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
    if (const char* ww = getenv("GOWA_WWS_WATCH"))
        if (atoi(ww) > 0) CreateThread(NULL, 0, wws_watch, (LPVOID)(uintptr_t)atoi(ww), 0, NULL);
    gcm_park_dump_hook = wws_rows_dump;
    gcm_park_stall_hook = wws_rekick;
    gcm_spu_put_who_hook = port_spu_put_who;
    if (const char* rd = getenv("SPU_SIG_RING_DUMP_S"))
        CreateThread(NULL, 0, sig_ring_dumper, (LPVOID)rd, 0, NULL);
    if (getenv("GOWA_WWS_TRACE") && !getenv("GOWA_WWS_WATCH"))
        CreateThread(NULL, 0, wws_watch, (LPVOID)(uintptr_t)10, 0, NULL);
    { extern void ps3_sampler_start(void); ps3_sampler_start(); }   /* PS3_SAMPLE=<ms> */
    gowa_freeze_watch_start();

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
