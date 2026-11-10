/*
 * Ascension-R -- the DLL side of the game's code (PORT_GAME_MODULE, the Kit release; game_module.h)
 *
 * Built with no C runtime and no entry point. ascension_game_init takes the program's table, then runs the constructors
 * the lifted SPU code registers itself from, which the C runtime would have run before main in one exe.
 */
#include "game_module.h"

__asm__(".att_syntax prefix\n");

/* One jump through the table, under the name the lifted code calls. */
#define THUNK(n)                                                                                                             \
    void* g_h_##n;                                                                                                            \
    __asm__(".text\n.balign 16\n.globl " #n "\n.def " #n "; .scl 2; .type 32; .endef\n" #n ":\n\tjmpq *g_h_" #n "(%rip)\n");
GAME_MODULE_FUNCS(THUNK)
GAME_MODULE_CRT(THUNK)

#define IMPORT(n) void* __imp_##n;
GAME_MODULE_IMPORTS(IMPORT)

#define DATA(t, n) t n;
GAME_MODULE_DATA(DATA)

/* Read by SPU_DIRECT, and the pending transfer's place: spu_context.h, PS3_GAME_MODULE. */
uint8_t* g_spu_stack_reset_hint;
uint64_t g_module_tls_index, g_module_tramp_off;

/* What the compiler takes for granted. */
int _fltused = 0x9875;
__asm__(".text\n.balign 16\n.globl __chkstk\n.def __chkstk; .scl 2; .type 32; .endef\n__chkstk:\n"
        "\tpushq %rcx\n\tpushq %rax\n\tleaq 24(%rsp), %rcx\n"
        ".Lchkstk_page:\n\tcmpq $0x1000, %rax\n\tjb .Lchkstk_last\n\tsubq $0x1000, %rcx\n\ttestb $0, (%rcx)\n\tsubq $0x1000, %rax\n\tjmp .Lchkstk_page\n"
        ".Lchkstk_last:\n\tsubq %rax, %rcx\n\ttestb $0, (%rcx)\n\tpopq %rax\n\tpopq %rcx\n\tretq\n");

/* The native routines have thread-locals of their own. The directory the C runtime would have brought. */
typedef struct { uint64_t start, end, index, callbacks; uint32_t zero, flags; } GameTlsDirectory;
typedef void (*GameTlsCallback)(void*, unsigned long, void*);
unsigned long _tls_index;
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLZ", read)
#pragma section(".rdata$T", read)
__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;
__declspec(allocate(".CRT$XLA")) GameTlsCallback __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) GameTlsCallback __xl_z = 0;
__declspec(allocate(".rdata$T")) const GameTlsDirectory _tls_used = {
    (uint64_t)&_tls_start, (uint64_t)&_tls_end, (uint64_t)&_tls_index, (uint64_t)(&__xl_a + 1), 0, 0 };

/* The bounds of the constructors, which the linker sorts between these two. */
typedef void (*GameCtor)(void);
#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCZ", read)
__declspec(allocate(".CRT$XCA")) static GameCtor s_ctors_begin = 0;
__declspec(allocate(".CRT$XCZ")) static GameCtor s_ctors_end = 0;

extern const char function_table[];
extern const uint64_t function_table_count;
void spu_indirect_branch(void*);

/* After the program has settled them: before any of the game's code runs. */
static void module_sync(const GameModuleHost* h)
{
#define SYNC(t, n) n = *h->d_##n;
    GAME_MODULE_DATA(SYNC)
}

/* Windows gives a DLL's thread-locals their index only if the DLL imports something: with no import at all they read
 * and write the program's block, index 0. This is the one import, for that. */
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);

__declspec(dllexport) int ascension_game_init(const GameModuleHost* h, GameModuleOut* out)
{
    out->thread_id = GetCurrentThreadId();
    out->tls_index = _tls_index;
    if (h->abi != GAME_MODULE_ABI || h->size != sizeof *h) return 0;
#define SET_FUNC(n) g_h_##n = h->f_##n;
    GAME_MODULE_FUNCS(SET_FUNC)
    GAME_MODULE_CRT(SET_FUNC)
#define SET_IMPORT(n) __imp_##n = h->i_##n;
    GAME_MODULE_IMPORTS(SET_IMPORT)
    g_spu_stack_reset_hint = h->stack_reset_hint;
    g_module_tls_index = h->tls_index;
    g_module_tramp_off = h->tramp_offset;
    for (GameCtor* c = &s_ctors_begin; c < &s_ctors_end; c++)
        if (*c) (*c)();
    out->functions = function_table;
    out->function_count = function_table_count;
    out->resolver = (void*)spu_indirect_branch;
    out->sync = module_sync;
    return 1;
}
