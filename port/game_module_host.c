/*
 * Ascension-R -- the program's side of the game's DLL (PORT_GAME_MODULE, the Kit release; game_module.h)
 */
#include <windows.h>
#include <intrin.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "game_module.h"

#define DECLARE_FUNC(n) void n(void);
GAME_MODULE_FUNCS(DECLARE_FUNC)
#define DECLARE_DATA(t, n) extern t n;
GAME_MODULE_DATA(DECLARE_DATA)

extern unsigned long _tls_index;
extern __declspec(thread) void (*g_spu_trampoline_fn)(void*);
extern uint8_t g_spu_stack_reset_hint[1024];
extern void* g_spu_resolver_alias;   /* spu_drain.c */
void spu_direct_init(void);

/* The lifted function table the runtime reads, filled from the DLL's. Only the pages it fills are ever touched. */
typedef struct { uint64_t addr; void* func; const char* name; } GameFunction;
#define GAME_MAX_FUNCTIONS (1u << 17)
GameFunction function_table[GAME_MAX_FUNCTIONS + 1];
uint64_t function_table_count;

static GameModuleHost s_api;
static GameModuleOut s_module;

static void host_fill(void)
{
    s_api.abi = GAME_MODULE_ABI;
    s_api.size = sizeof s_api;
#define SET_FUNC(n) s_api.f_##n = (void*)n;
    GAME_MODULE_FUNCS(SET_FUNC)
    GAME_MODULE_CRT(SET_FUNC)
#define SET_IMPORT(n) s_api.i_##n = (void*)n;
    GAME_MODULE_IMPORTS(SET_IMPORT)
#define SET_DATA(t, n) s_api.d_##n = &n;
    GAME_MODULE_DATA(SET_DATA)
    s_api.stack_reset_hint = g_spu_stack_reset_hint;
    /* gs:0x58 is the thread's array of thread-local blocks, one per module. */
    s_api.tls_index = _tls_index;
    s_api.tramp_offset = (uint64_t)((char*)&g_spu_trampoline_fn - ((char**)__readgsqword(0x58))[_tls_index]);
}

/* Before anything looks for lifted code. */
int game_module_load(void)
{
    wchar_t path[MAX_PATH + 32];
    const DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = n ? wcsrchr(path, L'\\') : NULL;
    if (!slash) return 0;
    wcscpy(slash + 1, L"ascension_game.dll");
    HMODULE dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        fprintf(stderr, "[module] could not load %ls: error %lu\n", path, GetLastError());
        return 0;
    }
    const GameModuleInit init = (GameModuleInit)(void*)GetProcAddress(dll, "ascension_game_init");
    host_fill();
    if (!init || !init(&s_api, &s_module)) {
        fprintf(stderr, "[module] %ls was built for another version of this program\n", path);
        return 0;
    }
    if (s_module.tls_index == _tls_index) {
        fprintf(stderr, "[module] %ls has no thread-local block of its own\n", path);
        return 0;
    }
    if (s_module.function_count > GAME_MAX_FUNCTIONS) {
        fprintf(stderr, "[module] %llu lifted functions: the table holds %u\n", (unsigned long long)s_module.function_count, GAME_MAX_FUNCTIONS);
        return 0;
    }
    memcpy(function_table, s_module.functions, (size_t)(s_module.function_count + 1) * sizeof function_table[0]);
    function_table_count = s_module.function_count;
    g_spu_resolver_alias = s_module.resolver;
    printf("[module] %ls: %llu lifted functions\n", path, (unsigned long long)function_table_count);
    return 1;
}

/* Once the data in the list is settled, before the guest runs. */
void game_module_sync(void)
{
    spu_direct_init();
    s_module.sync(&s_api);
}
