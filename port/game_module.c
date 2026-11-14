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

#ifdef GOWA_PGO_GEN
/* The training build of "Optimise for this PC" (gowa.py kit-build --pgo): the lifted code instrumented
 * (-fprofile-instr-generate), with clang's profile runtime (clang_rt.profile-x86_64.lib) linked in. That runtime is
 * built against a C runtime. What it takes from one comes from ucrtbase.dll and kernel32.dll through the import
 * libraries the build makes (kit\ucrtbase_pgo.def, kit\kernel32_pgo.def), from the program's table for what this
 * file takes from there already, and from here. */
unsigned long long __security_cookie = 0x00002B992DDFA232ull;
void __security_check_cookie(unsigned long long cookie) { (void)cookie; }
/* What it calls by name and this file has only as __imp_ pointers, and the POSIX names ucrtbase has with a _. */
#define PLAIN(n) __asm__(".text\n.balign 16\n.globl " #n "\n.def " #n "; .scl 2; .type 32; .endef\n" #n ":\n\tjmpq *__imp_" #n "(%rip)\n");
PLAIN(malloc) PLAIN(free) PLAIN(calloc) PLAIN(getenv) PLAIN(atoi) PLAIN(fflush) PLAIN(__acrt_iob_func) PLAIN(__stdio_common_vfprintf)
#define ALIAS(n, to) __asm__(".text\n.balign 16\n.globl " #n "\n.def " #n "; .scl 2; .type 32; .endef\n" #n ":\n\tjmp " #to "\n");
ALIAS(strdup, _strdup) ALIAS(fileno, _fileno) ALIAS(fdopen, _fdopen) ALIAS(getpid, _getpid) ALIAS(open, _open)
__declspec(dllimport) void* __stdcall CreateThread(void*, unsigned long long, unsigned long(__stdcall*)(void*), void*, unsigned long,
                                                   unsigned long*);
__declspec(dllimport) void __stdcall Sleep(unsigned long);
__declspec(dllimport) int __stdcall MoveFileExA(const char*, const char*, unsigned long);
char* getenv(const char*);
unsigned long long strlen(const char*);
void* memcpy(void*, const void*, unsigned long long);
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char*);
/* The counters go to LLVM_PROFILE_FILE every 15 s, and the training run ends killed once one written after its play
 * is there. The runtime truncates its file once and appends every write after it, which llvm-profdata merge adds
 * up: each goes to <file>.new instead, moved over the file after it (main.cpp's pgo_writer has the same). */
static char s_pgo_raw[1024], s_pgo_new[1024 + 4];
static unsigned long __stdcall pgo_writer(void* arg)
{
    (void)arg;
    for (;;) {
        Sleep(15000);
        if (__llvm_profile_write_file() == 0)
            MoveFileExA(s_pgo_new, s_pgo_raw, 1);   /* MOVEFILE_REPLACE_EXISTING */
    }
}
static void pgo_writer_start(void)
{
    const char* raw = getenv("LLVM_PROFILE_FILE");
    const unsigned long long n = raw ? strlen(raw) : 0;
    if (!n || n >= sizeof s_pgo_raw) return;
    memcpy(s_pgo_raw, raw, n + 1);
    memcpy(s_pgo_new, raw, n);
    memcpy(s_pgo_new + n, ".new", 5);
    __llvm_profile_set_filename(s_pgo_new);
    CreateThread(0, 0, pgo_writer, 0, 0, 0);
}
#endif

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
#ifdef GOWA_PGO_GEN
    pgo_writer_start();
#endif
    out->functions = function_table;
    out->function_count = function_table_count;
    out->resolver = (void*)spu_indirect_branch;
    out->sync = module_sync;
    return 1;
}
