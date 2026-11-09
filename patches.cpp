/*
 * Ascension-R -- switchable patches of the title
 *
 * scripts/title_patches.json lists them. `gowa.py lift` lifts each patch's
 * instruction edits in both forms behind the patch's guard (ppu_lifter
 * --insn-patches: if (guard) { new } else { original }) and writes the guards
 * and the data edits into recompiled/gowa_title_patches.h. Here, once the ELF
 * is in guest memory and before the title runs, each patch that is switched on
 * gets its data edits written and its guard set:
 *
 *   [patches] disable_mlaa = on       in ascension-r.ini (what a launcher row would write)
 *   --set patches.disable_mlaa=on     on the command line
 *   GOWA_PATCH_DISABLE_MLAA=1         in the environment
 *
 * Every patch is off unless switched on, or, with "default": true in the
 * catalog, on unless switched off (= off). A data edit whose bytes in guest
 * memory are not the ones the patch expects refuses the whole patch, guard
 * included: the patch was made for another build of the title. The lifted code
 * reads the guards, so they change only here, before the guest starts.
 */
#include "ps3emu/ps3_settings.h"
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" uint8_t vm_read8(uint64_t addr);
extern "C" void vm_write8(uint64_t addr, uint8_t val);

struct gowa_title_data {
    int patch;              /* index into k_title_patches; -1 ends the table */
    uint32_t addr;
    uint32_t n;
    uint8_t orig[16];
    uint8_t val[16];
};
struct gowa_title_patch {
    const char* id;         /* the [patches] key; NULL ends the table */
    const char* name;
    int* guard;
    int ppu_edits;          /* instructions lifted both ways: PPU ones, */
    int spu_edits;          /* and SPU ones in job modules */
    int def_on;             /* on unless switched off */
};

#define GOWA_TITLE_GUARD(g) extern "C" { int g = 0; }
#if __has_include("gowa_title_patches.h")
#include "gowa_title_patches.h"
#else
/* recompiled/ from a lift older than this file: no patches. `gowa.py lift`
 * writes the header. */
static const gowa_title_data k_title_data[] = { { -1, 0, 0, { 0 }, { 0 } } };
static const gowa_title_patch k_title_patches[] = { { NULL, NULL, NULL, 0, 0, 0 } };
#endif

static int patch_wanted(const gowa_title_patch* p)
{
    char env[96] = "GOWA_PATCH_";
    size_t k = 11;
    for (const char* c = p->id; *c && k + 1 < sizeof env; c++)
        env[k++] = (char)toupper((unsigned char)*c);
    env[k] = 0;
    return ps3_settings_bool("patches", p->id, env, p->def_on);
}

/* Whether a patch is switched on, before gowa_title_patches_apply has run. */
extern "C" int gowa_title_patch_wanted(const char* id)
{
    for (const gowa_title_patch* p = k_title_patches; p->id; p++)
        if (!strcmp(p->id, id)) return patch_wanted(p);
    return 0;
}

extern "C" void gowa_title_patches_apply(void)
{
    for (const gowa_title_patch* p = k_title_patches; p->id; p++) {
        if (!patch_wanted(p))
            continue;
        /* Frame generation and the upscaler take what moves from the blur's draws. With Disable
         * MLAA on the blur itself never reaches the picture; without it FSR 3 stays off (main.cpp). */
        if (!strcmp(p->id, "disable_motion_blur") && gowa_title_patch_wanted("disable_mlaa") &&
            (ps3_settings_ieq(ps3_settings_str("graphics", "frame_generation", "D3D12_FRAME_GENERATION", "off"), "on") ||
             ps3_settings_ieq(ps3_settings_str("graphics", "scaling", NULL, "bilinear"), "fsr3")) &&
            ps3_settings_bool("graphics", "frame_generation_objects", "D3D12_FG_OBJECTS", 1)) {
            fprintf(stderr, "[patches] %s: left off for FSR's motion\n", p->name);
            continue;
        }
        const int self = (int)(p - k_title_patches);
        int refused = 0;
        for (const gowa_title_data* d = k_title_data; d->patch >= 0 && !refused; d++)
            for (uint32_t i = 0; d->patch == self && i < d->n && !refused; i++) {
                const uint8_t held = vm_read8(d->addr + i);
                if (held != d->orig[i]) {
                    fprintf(stderr, "[patches] %s: refused, guest 0x%08X holds 0x%02X where the patch "
                            "expects 0x%02X (another build of the title?)\n",
                            p->name, d->addr + i, held, d->orig[i]);
                    refused = 1;
                }
            }
        if (refused)
            continue;
        unsigned bytes = 0;
        for (const gowa_title_data* d = k_title_data; d->patch >= 0; d++)
            for (uint32_t i = 0; d->patch == self && i < d->n; i++, bytes++)
                vm_write8(d->addr + i, d->val[i]);
        *p->guard = 1;
        fprintf(stderr, "[patches] %s: on (%d PPU and %d SPU instruction edit%s, %u data byte%s)\n",
                p->name, p->ppu_edits, p->spu_edits, p->ppu_edits + p->spu_edits == 1 ? "" : "s",
                bytes, bytes == 1 ? "" : "s");
    }
}

/* For the launcher: the patches this build has, in the catalog's order. */
extern "C" int gowa_title_patch_count(void)
{
    int n = 0;
    while (k_title_patches[n].id) n++;
    return n;
}
extern "C" const char* gowa_title_patch_id(int i)   { return k_title_patches[i].id; }
extern "C" const char* gowa_title_patch_name(int i) { return k_title_patches[i].name; }
extern "C" int gowa_title_patch_default(int i)      { return k_title_patches[i].def_on; }
