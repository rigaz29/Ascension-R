/*
 * GowARecomp -- the launcher's cheats: [cheats] infinite_health, one_hit_kill.
 *
 * [cheats] max_orbs, infinite_magic and infinite_jumps too: the meters of red orbs (tPowerUpMeter),
 * magic (tMagicMeterPlayer) and items (tItemMeter), and the flag of the jump in the air.
 *
 * A creature keeps its health at +0x214 and its maximum at +0x218, and all its damage goes through
 * func_00087440 (r3 the creature, f1 the damage, negative to heal). What one creature does to
 * another comes from func_00156680 and func_00154460, both with the attacker in r31 at that call.
 * scripts/ppu_hooks.json has the lift call the functions here before those instructions.
 *
 * What a creature is shows in its controller, at +0x1304: goPlayer for Kratos, goAI for enemies,
 * goPassiveAI and goNeoIO for the level's props, which a script may need alive (health 10000 and
 * 1 in the first fight).
 */
#include "ppu_recomp.h"
#include "ps3emu/ps3_settings.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint32_t GO_PLAYER = 0x006925D0, GO_PASSIVE_AI = 0x0069EA78, GO_NEO_IO = 0x0069E920;
static const uint32_t CREATURE_HEALTH = 0x214, CREATURE_MAX_HEALTH = 0x218, CREATURE_CONTROLLER = 0x1304;

static int s_infinite_health, s_one_hit_kill, s_max_orbs, s_infinite_magic, s_infinite_jumps;

/* Objects live on the title's heap. Anything else in an attacker's register is not a creature. */
static int on_heap(uint64_t addr)
{
    return addr >= 0x70000000u && addr < 0x80000000u;
}

/* The vtable of a creature's controller, 0 if it has none. */
static uint32_t controller_class(uint64_t creature)
{
    if (!on_heap(creature)) return 0;
    const uint32_t controller = vm_read32(creature + CREATURE_CONTROLLER);
    return on_heap(controller) ? vm_read32(controller) : 0;
}

static double read_float(uint64_t addr)
{
    const uint32_t u = vm_read32(addr);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

extern "C" void gowa_cheats_apply(void)
{
    s_infinite_health = ps3_settings_bool("cheats", "infinite_health", "GOWA_CHEAT_INFINITE_HEALTH", 0);
    s_one_hit_kill = ps3_settings_bool("cheats", "one_hit_kill", "GOWA_CHEAT_ONE_HIT_KILL", 0);
    s_max_orbs = ps3_settings_bool("cheats", "max_orbs", "GOWA_CHEAT_MAX_ORBS", 0);
    s_infinite_magic = ps3_settings_bool("cheats", "infinite_magic", "GOWA_CHEAT_INFINITE_MAGIC", 0);
    s_infinite_jumps = ps3_settings_bool("cheats", "infinite_jumps", "GOWA_CHEAT_INFINITE_JUMPS", 0);
    if (s_infinite_health) fprintf(stderr, "[cheats] infinite health\n");
    if (s_one_hit_kill) fprintf(stderr, "[cheats] one hit kill\n");
    if (s_max_orbs) fprintf(stderr, "[cheats] max red orbs\n");
    if (s_infinite_magic) fprintf(stderr, "[cheats] infinite magic and items\n");
    if (s_infinite_jumps) fprintf(stderr, "[cheats] infinite double jumps\n");
}

extern "C" void gowa_cheat_damage(ppu_context* ctx, uint32_t)
{
    if (s_infinite_health && ctx->fpr[1] > 0.0 && controller_class(ctx->gpr[3]) == GO_PLAYER) ctx->fpr[1] = 0.0;
}

extern "C" void gowa_cheat_hit(ppu_context* ctx, uint32_t)
{
    const uint64_t victim = ctx->gpr[3];
    if (!s_one_hit_kill || ctx->fpr[1] <= 0.0 || controller_class(ctx->gpr[31]) != GO_PLAYER) return;
    const uint32_t cls = controller_class(victim);
    if (cls && cls != GO_PLAYER && cls != GO_PASSIVE_AI && cls != GO_NEO_IO)
        ctx->fpr[1] = read_float(victim + CREATURE_HEALTH) + read_float(victim + CREATURE_MAX_HEALTH) + 1.0;
}

/* A meter setting its value, with the value in f31 and in f1 the maximum it is about to be clamped
 * to: the maximum goes in. Every set fills it, the one at a level's start included. */
extern "C" void gowa_cheat_orbs(ppu_context* ctx, uint32_t)
{
    if (s_max_orbs) ctx->fpr[31] = ctx->fpr[1];
}

extern "C" void gowa_cheat_magic(ppu_context* ctx, uint32_t)
{
    if (s_infinite_magic) ctx->fpr[31] = ctx->fpr[1];
}

/* The test of the jump in the air already had: the flags word read into a register, its bit 0x4
 * cleared there for Kratos, so the game never sees it set. The word itself is left alone. */
extern "C" void gowa_cheat_jump(ppu_context* ctx, uint32_t addr)
{
    if (!s_infinite_jumps) return;
    const int flags = addr == 0x000B62DCu ? 4 : 3;
    const int creature = addr == 0x00270F6Cu ? 23 : 31;
    if (controller_class(ctx->gpr[creature]) != GO_PLAYER) return;
    ctx->gpr[flags] &= ~(uint64_t)4u;
}
