/*
 * Ascension-R -- what the game's code needs from the program when it is a DLL of its own
 *
 * The Kit release (PORT_GAME_MODULE) builds the game's code into ascension_game.dll on the player's PC, against nothing:
 * no import library, no C runtime. The program fills one table with what the DLL calls (game_module_host.c), and the DLL
 * goes through it (game_module.c). The lists are every symbol the lifted code and the native routines take from outside
 * themselves; a new one is a link error of the DLL, and goes here.
 */
#pragma once
#include <stdint.h>

/* A DLL built for another number is refused. Change it when a list below or a structure they share changes.
 * 2: atol and calloc (native/spu_edge_memo.c). 3: the VRAM guards (port/vram_guard.cpp). */
#define GAME_MODULE_ABI 3

/* The program's functions. In the DLL each is one jump through the table. */
#define GAME_MODULE_FUNCS(X)                                                                                                  \
    X(gowa_cheat_damage) X(gowa_cheat_hit) X(gowa_cheat_jump) X(gowa_cheat_magic) X(gowa_cheat_orbs) X(gowa_cheat_rage)       \
    X(gowa_poll_wait) X(gowa_wide_aspect) X(gowa_wide_fov) X(gowa_wide_framing)                                               \
    X(lv2_syscall) X(ppu_stdcx64) X(ppu_stwcx32) X(ppu_timebase_now) X(ps3_hle_call) X(ps3_indirect_call)                     \
    X(rsx_d3d12_kept_publish) X(rsx_d3d12_register_vertex_ring) X(rsx_d3d12_vertex_pass_publish) X(rsx_d3d12_vertex_pass_set) \
    X(spu_begin_image) X(spu_drain_call) X(spu_halt) X(spu_ijmp_shadow) X(spu_img_restore) X(spu_indirect_branch)             \
    X(spu_overlay_register_pic_region) X(spu_overlay_register_verified_region) X(spu_rchcnt) X(spu_rdch)                      \
    X(spu_register_function) X(spu_stop) X(spu_workload_register_img) X(spu_wrch)                                             \
    X(vm_read8) X(vm_read16) X(vm_read32) X(vm_read64) X(vm_write8) X(vm_write16) X(vm_write32) X(vm_write64)                 \
    X(gowa_vram_alloc) X(gowa_vram_move) X(gowa_vram_texture)

/* The C runtime's, called by name. */
#define GAME_MODULE_CRT(X) X(atexit) X(fputc) X(fwrite) X(memcmp) X(memcpy) X(memmove) X(sqrt) X(sqrtf)

/* The C runtime's, called through an import. */
#define GAME_MODULE_IMPORTS(X)                                                                                                \
    X(__acrt_iob_func) X(__stdio_common_vfprintf) X(_aligned_malloc) X(abort) X(atoi) X(atol) X(calloc) X(clock) X(fflush)   \
    X(free) X(getenv) X(ldexp) X(log2f) X(malloc) X(strtoul)

/* The program's data that is settled before the guest runs: the DLL keeps a copy, so reading it costs what it did. */
typedef struct { uint8_t* vb; const unsigned char* coh; } GameModuleMm;
#define GAME_MODULE_DATA(X)                                                                                                   \
    X(uint8_t*, vm_base) X(GameModuleMm, g_ppu_mm) X(volatile int, g_spu_direct) X(volatile int, g_spu_ijmp_local)            \
    X(int, g_gowa_patch_disable_depth_of_field) X(int, g_gowa_patch_disable_mesh_trimming) X(int, g_gowa_patch_disable_mlaa)  \
    X(int, g_gowa_patch_disable_motion_blur) X(int, g_gowa_patch_disable_shadows)

typedef struct GameModuleHost {
    uint32_t abi, size;
#define X(n) void* f_##n;
    GAME_MODULE_FUNCS(X) GAME_MODULE_CRT(X)
#undef X
#define X(n) void* i_##n;
    GAME_MODULE_IMPORTS(X)
#undef X
#define X(t, n) t* d_##n;
    GAME_MODULE_DATA(X)
#undef X
    uint8_t* stack_reset_hint;   /* written while the guest runs, so the DLL reads the program's own */
    uint64_t tls_index;          /* where the program's thread-local block is, and in it the SPU's pending transfer */
    uint64_t tramp_offset;
} GameModuleHost;

typedef struct GameModuleOut {
    const void* functions;       /* the lifted function table */
    uint64_t function_count;
    void* resolver;              /* spu_indirect_branch as the DLL names it */
    void (*sync)(const GameModuleHost*);
    unsigned long tls_index;     /* the loader's, for the DLL's own thread-locals */
    unsigned long thread_id;
} GameModuleOut;

typedef int (*GameModuleInit)(const GameModuleHost*, GameModuleOut*);
