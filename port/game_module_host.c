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
#include "ps3emu/ps3_settings.h"

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

/* A file of the folder the launcher builds the DLL in, <the settings' folder>\game-code (gowa.py kit-build); ""
 * gives the folder. */
void game_module_built(wchar_t* out, size_t n, const wchar_t* name)
{
    wchar_t d[MAX_PATH + 64];
    if (!MultiByteToWideChar(CP_UTF8, 0, ps3_settings_path(), -1, d, MAX_PATH)) d[0] = 0;
    for (wchar_t* p = d; *p; p++) if (*p == L'/') *p = L'\\';
    wchar_t* slash = wcsrchr(d, L'\\');
    if (slash) *slash = 0;
    else wcscpy(d, L".");
    if (name[0]) swprintf(out, n, L"%ls\\game-code\\%ls", d, name);
    else swprintf(out, n, L"%ls\\game-code", d);
}

/* The game's DLL: beside the program (a build's own), else the one the launcher built. 1 beside, 2 built here,
 * 0 neither; path gets the one found, or where the launcher would build it. */
int game_module_where(wchar_t* path, size_t n)
{
    wchar_t p[MAX_PATH + 32];
    const DWORD len = GetModuleFileNameW(NULL, p, MAX_PATH);
    wchar_t* slash = len && len < MAX_PATH ? wcsrchr(p, L'\\') : NULL;
    if (slash) {
        wcscpy(slash + 1, L"ascension_game.dll");
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
            swprintf(path, n, L"%ls", p);
            return 1;
        }
    }
    game_module_built(path, n, L"ascension_game.dll");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ? 2 : 0;
}

/* A file of the Kit's folder, kit\ beside the program (gowa.py kit-tree). */
void game_module_kit(wchar_t* out, size_t n, const wchar_t* name)
{
    wchar_t p[MAX_PATH + 32];
    const DWORD len = GetModuleFileNameW(NULL, p, MAX_PATH);
    wchar_t* slash = len && len < MAX_PATH ? wcsrchr(p, L'\\') : NULL;
    if (slash) slash[1] = 0;
    else p[0] = 0;
    swprintf(out, n, L"%lskit\\%ls", p, name);
}

/* The first line of a small text file, without its line end; "" when there is none. */
static void first_line(const wchar_t* path, char* out, size_t n)
{
    out[0] = 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

/* What stands between the player and the game's code: 0 nothing (a DLL beside the program, or one the launcher built
 * for this program from this Kit), 1 not built yet, 2 built for another version of the program or the Kit, 3 no DLL
 * that will do, and no Kit to build one with. */
int game_module_state(void)
{
    wchar_t p[MAX_PATH + 64];
    const int where = game_module_where(p, sizeof p / sizeof p[0]);
    if (where == 1) return 0;
    char kit[96];
    game_module_kit(p, sizeof p / sizeof p[0], L"kit-id.txt");
    first_line(p, kit, sizeof kit);
    if (where == 0) return kit[0] ? 1 : 3;
    /* Built here: ascension_game.ini says for what (gowa.py kit-build). */
    int abi = -1;
    char built_kit[96] = "", line[200];
    game_module_built(p, sizeof p / sizeof p[0], L"ascension_game.ini");
    FILE* f = _wfopen(p, L"rb");
    const int said = f != NULL;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "abi = ", 6)) abi = atoi(line + 6);
            else if (!strncmp(line, "kit = ", 6)) snprintf(built_kit, sizeof built_kit, "%s", line + 6);
        }
        fclose(f);
    }
    if (!kit[0]) return abi == GAME_MODULE_ABI || !said ? 0 : 3;   /* nothing to build it again with: loading it tells */
    return abi == GAME_MODULE_ABI && !strcmp(kit, built_kit) ? 0 : 2;
}

/* Before anything looks for lifted code. */
int game_module_load(void)
{
    wchar_t path[MAX_PATH + 64];
    if (!game_module_where(path, sizeof path / sizeof path[0])) {
        fprintf(stderr, "[module] no ascension_game.dll beside the program or in %ls: the launcher builds it\n", path);
        return 0;
    }
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
