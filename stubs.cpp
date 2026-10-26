/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ppu_context.h"

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 *
 * This one does, for a narrow reason (the same as in the God of War HD port).
 * FIOS hands Edge Zlib inflate requests to an SPU task through an ANY2ANY
 * SPURS lock-free queue, and the SPU end of that queue is Sony's own code. The PPU end (_cellSpursLFQueuePushBody and the
 * GetPushPointer2/CompletePushPointer2 pair behind it) isn't documented, and
 * RPCS3 leaves it as a TODO. So the queue initializer and PushBody run as the
 * lifted firmware code (`scripts/gowa.py lle`); only those two exports are
 * registered, and every other cellSpurs call stays HLE.
 * -----------------------------------------------------------------------*/

#ifdef GOWA_LIBSRE_LLE
#include "prx_loader.h"
#include "gowa_lle.h"

extern "C" {
extern const prx_func_entry libsre_function_table[];
extern const uint64_t       libsre_function_table_count;
void     ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
void     ps3_hle_call(uint32_t nid, ppu_context* ctx);
void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
void     ps3_indirect_call(ppu_context* ctx);
uint8_t  vm_read8(uint64_t addr);
uint32_t vm_read32(uint64_t addr);
void     vm_write8(uint64_t addr, uint8_t v);
void     vm_write16(uint64_t addr, uint16_t v);
void     vm_write32(uint64_t addr, uint32_t v);
void     vm_write64(uint64_t addr, uint64_t v);
uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);
int64_t  sys_event_port_create(ppu_context* ctx);
int64_t  sys_event_port_connect_local(ppu_context* ctx);
int32_t  cellSpursAttachLv2EventQueue(void* spurs, uint32_t queue, uint8_t* port, int32_t isDynamic);
int32_t  cellSpursCreateTasksetWithAttribute(void* spurs, void* taskset, const void* attr);
void     spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
}

/* SPURS instance -> tasksets, recorded as the title creates them. The firmware
 * resolves a SPURS-owned queue's waiter to its taskset by workload id through
 * the SPURS workload table; the HLE instance creates every taskset as workload
 * 0 and doesn't keep that table, so the port keeps its own. */
static int lle_trace(void);

struct taskset_rec { uint32_t spurs, taskset; };
static taskset_rec s_tasksets[32];
static int         s_taskset_n = 0;

static void hle_create_taskset_with_attribute(ppu_context* ctx)
{
    uint32_t spurs = (uint32_t)ctx->gpr[3], taskset = (uint32_t)ctx->gpr[4];
    int32_t rc = cellSpursCreateTasksetWithAttribute((void*)(uintptr_t)spurs,
                                                     (void*)(uintptr_t)taskset,
                                                     (const void*)(uintptr_t)ctx->gpr[5]);
    if (rc == 0 && s_taskset_n < 32) s_tasksets[s_taskset_n++] = { spurs, taskset };
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static uint32_t taskset_of(uint32_t spurs, uint32_t wid)
{
    uint32_t found = 0;
    int matches = 0;
    for (int i = 0; i < s_taskset_n; i++) {
        if (s_tasksets[i].spurs != spurs) continue;
        if (vm_read32(s_tasksets[i].taskset + 0x74) != wid) continue;   /* taskset->wid */
        if (!found) found = s_tasksets[i].taskset;
        matches++;
    }
    static int warned = 0;
    if (matches > 1 && warned++ < 4)
        fprintf(stderr, "[lfqueue] %d tasksets of spurs 0x%08X have wid %u; signalling 0x%08X\n",
                matches, spurs, wid, found);
    return found;
}

static void register_lifted(uint32_t guest_addr, void (*host)(void*))
{
    ppu_register_function(guest_addr, (void (*)(ppu_context*))host);
}

/* CompletePushPointer2 wakes the task parked on the queue through a function
 * pointer to libsre's _cellSpursSendSignal. The SPU tasks are run by the HLE
 * task model, which parks and wakes them itself, so the signal goes to the HLE
 * handler rather than to the firmware's SPURS kernel bookkeeping. */
static void send_signal_bridge(ppu_context* ctx)
{
    ps3_hle_call(0xE0A6DBE4u /* _cellSpursSendSignal */, ctx);
}

/* libsre's LFQueue signal hook (code 0x280126C0 in this relocation), which
 * CompletePushPointer[2] calls through a function pointer with the queue's
 * eaSignal and the waiter word the SPU task left in the queue:
 *
 *   eaSignal & 0xF == 1  (queue owned by a SPURS instance):
 *       taskset = the workload's taskset, workload id = waiter >> 8
 *   otherwise            (queue owned by a taskset):
 *       taskset = eaSignal
 *   task id = waiter & 0xFF
 *
 * The firmware then calls its own _cellSpursSendSignal directly, which works
 * on SPURS kernel state the HLE instance doesn't keep. Here the signal goes to
 * the HLE task model instead, which is what parked the task. */
static void lfqueue_send_signal(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3], waiter = (uint32_t)ctx->gpr[4];
    uint32_t task = waiter & 0xFF;
    uint32_t taskset = (ea & 0xF) == 1 ? taskset_of(ea & ~0xFu, (waiter >> 8) & 0xFFFFFF) : ea;
    if (lle_trace())
        fprintf(stderr, "[lfqueue] signal ea=0x%08X waiter=0x%08X -> taskset 0x%08X task %u\n",
                ea, waiter, taskset, task);
    if (!taskset) {
        fprintf(stderr, "[lfqueue] signal ea=0x%08X waiter=0x%08X: no taskset\n", ea, waiter);
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410902;
        return;
    }
    spu_taskset_signal_task(taskset, task);
    ctx->gpr[3] = 0;
}

/* Run a lifted libsre export on the calling guest thread, the way the runtime
 * calls a PRX export: libsre's TOC in r2, its code in CTR, and r1 dropped below
 * the caller's frame so the callee's prologue can't overwrite the caller's TOC
 * save slot. ps3_hle_call restores the caller's r2 when this handler returns. */
static void call_libsre(ppu_context* ctx, uint32_t opd)
{
    uint64_t sp = ctx->gpr[1];
    ctx->gpr[2] = vm_read32(opd + 4);
    ctx->ctr    = vm_read32(opd);
    ctx->gpr[1] = (sp - 0x1000) & ~0xFull;
    ps3_indirect_call(ctx);
    ctx->gpr[1] = sp;
}

static uint32_t libsre_opd(uint32_t nid)
{
    for (uint32_t i = 0; i < gowa_libsre_lle_count; i++)
        if (gowa_libsre_lle[i].nid == nid) return gowa_libsre_lle[i].opd;
    return 0;
}

static void dump_queue_line(const char* tag, uint32_t q)
{
    fprintf(stderr, "[lfqueue] %s q=0x%08X", tag, q);
    for (uint32_t o = 0; o < 0x80; o += 4) {
        if (o % 16 == 0) fprintf(stderr, "\n  +%02X:", o);
        fprintf(stderr, " %08X", vm_read32(q + o));
    }
    fprintf(stderr, "\n");
}

static int lle_trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("GOWA_LLE_TRACE") ? 1 : 0;
    return t;
}

/* _cellSpursLFQueueInitialize(owner, queue, buffer, size, depth, direction) */
static void lle_lfqueue_initialize(ppu_context* ctx)
{
    uint32_t q = (uint32_t)ctx->gpr[4];
    uint32_t args[6];
    for (int i = 0; i < 6; i++) args[i] = (uint32_t)ctx->gpr[3 + i];
    call_libsre(ctx, libsre_opd(0x011EE38Bu));
    uint32_t rc = (uint32_t)ctx->gpr[3];
    fprintf(stderr, "[lfqueue] init owner=0x%08X q=0x%08X buf=0x%08X size=%u depth=%u dir=%u -> 0x%08X\n",
            args[0], args[1], args[2], args[3], args[4], args[5], rc);
    if (rc || lle_trace()) dump_queue_line("after init", q);
}

/* cellSpursLFQueueAttachLv2EventQueue(queue)
 *
 * The firmware's PushBody refuses with CELL_SPURS_TASK_ERROR_STAT while the
 * queue's port bytes at +0x20 are still 0xFF, and those are filled in here. The
 * firmware version creates an lv2 queue, attaches it to the SPURS instance on a
 * dynamic port (so an SPU task can send_event to it) and records both in the
 * queue line; ANY2ANY gets two of each plus a local PPU event port per queue.
 * This does the same with the runtime's own queue, port and SPURS attach
 * primitives, because the firmware's attach walks SPURS internals that the HLE
 * instance doesn't model. The layout written matches the firmware's:
 *
 *   other:   +0x78 u64 queue id, +0x20 u32 port
 *   ANY2ANY: +0x78/+0x7C queue ids, +0x20/+0x21 ports, +0x28 PPU port 0,
 *            +0x30/+0x50 the high/low halves of PPU port 1
 */
static void hle_lfqueue_attach(ppu_context* ctx)
{
    uint32_t q = (uint32_t)ctx->gpr[3];
    if (!q)        { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410911; return; }
    if (q & 0x7F)  { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410910; return; }

    uint32_t dir = vm_read32(q + 0x24);
    bool any2any = dir == 3;
    if (any2any ? (vm_read8(q + 0x20) != 0xFF || vm_read8(q + 0x21) != 0xFF)
                : vm_read32(q + 0x20) != 0xFFFFFFFFu) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8041090F;   /* already attached */
        return;
    }

    /* eaSignal (+0x70) is a taskset, or the SPURS instance with a low tag bit. */
    uint32_t owner = vm_read32(q + 0x74);
    uint32_t spurs = (owner & 0xF) ? (owner & ~0xFu) : vm_read32(owner + 0x64);

    /* Guest scratch for the out-parameters, well below the caller's frame. */
    uint32_t scratch = ((uint32_t)ctx->gpr[1] - 0x2000u) & ~0xFu;

    int n = any2any ? 2 : 1;
    uint32_t eq[2] = { 0, 0 }, ppu_port[2] = { 0, 0 };
    uint8_t  spu_port[2] = { 0, 0 };
    for (int i = 0; i < n; i++) {
        eq[i] = sys_event_queue_create_direct(0, any2any ? 15 : 1);
        if (!eq[i]) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410904; return; }

        vm_write8(scratch, 0);
        int32_t rc = cellSpursAttachLv2EventQueue((void*)(uintptr_t)spurs, eq[i],
                                                  (uint8_t*)(uintptr_t)scratch, 1);
        if (rc) { ctx->gpr[3] = (uint64_t)(int64_t)(rc | 0x80410900); return; }
        spu_port[i] = vm_read8(scratch);

        if (any2any) {
            ppu_context c;
            memset(&c, 0, sizeof c);
            c.gpr[3] = scratch + 4; c.gpr[4] = 1 /* SYS_EVENT_PORT_LOCAL */; c.gpr[5] = 0;
            sys_event_port_create(&c);
            ppu_port[i] = vm_read32(scratch + 4);
            memset(&c, 0, sizeof c);
            c.gpr[3] = ppu_port[i]; c.gpr[4] = eq[i];
            sys_event_port_connect_local(&c);
        }
    }

    if (any2any) {
        vm_write32(q + 0x78, eq[0]);
        vm_write32(q + 0x7C, eq[1]);
        vm_write8(q + 0x20, spu_port[0]);
        vm_write8(q + 0x21, spu_port[1]);
        vm_write32(q + 0x28, ppu_port[0]);
        vm_write16(q + 0x30, (uint16_t)(ppu_port[1] >> 16));
        vm_write16(q + 0x50, (uint16_t)ppu_port[1]);
    } else {
        vm_write64(q + 0x78, eq[0]);
        vm_write32(q + 0x20, spu_port[0]);
    }
    fprintf(stderr, "[lfqueue] attach q=0x%08X dir=%u spurs=0x%08X queues=%u/%u spu ports=0x%02X/0x%02X\n",
            q, dir, spurs, eq[0], eq[1], spu_port[0], spu_port[1]);
    if (lle_trace()) dump_queue_line("after attach", q);
    ctx->gpr[3] = 0;
}

/* _cellSpursLFQueuePushBody(queue, entry, isBlocking) */
static void lle_lfqueue_push_body(ppu_context* ctx)
{
    uint32_t q = (uint32_t)ctx->gpr[3], entry = (uint32_t)ctx->gpr[4];
    uint32_t blocking = (uint32_t)ctx->gpr[5];
    if (lle_trace()) dump_queue_line("before push", q);
    call_libsre(ctx, libsre_opd(0x8A85674Du));
    uint32_t rc = (uint32_t)ctx->gpr[3];
    static int failures = 0;
    if (lle_trace() || (rc && failures++ < 8)) {
        fprintf(stderr, "[lfqueue] push q=0x%08X entry=0x%08X blocking=%u -> 0x%08X\n",
                q, entry, blocking, rc);
        dump_queue_line("after push", q);
    }
}
#endif

extern "C" void ps3_load_prx_modules(void)
{
#ifdef GOWA_LIBSRE_LLE
    const char* path = getenv("GOWA_LIBSRE_IMAGE");
    if (!path || !*path) path = "firmware/libsre/libsre_image.bin";

    uint32_t size = 0;
    uint8_t* image = prx_image_load_file(path, &size);
    if (!image) {
        fprintf(stderr, "[gowa] libsre image not found at %s; SPURS queues stay HLE\n", path);
        return;
    }

    prx_module m;
    memset(&m, 0, sizeof m);
    m.name         = "libsre";
    m.base         = gowa_libsre_base;
    m.image        = image;
    m.image_size   = size;
    m.funcs        = libsre_function_table;
    m.func_count   = libsre_function_table_count;
    /* No exports: the runtime would route every one of them ahead of the HLE
     * handlers. The few that should run as firmware code get handlers below. */
    m.exports      = nullptr;
    m.export_count = 0;

    prx_load_result r = prx_load_module(&m, register_lifted);
    free(image);
    if (!r.ok) {
        fprintf(stderr, "[gowa] libsre load failed; SPURS queues stay HLE\n");
        return;
    }
    ppu_register_function(gowa_libsre_send_signal_code, send_signal_bridge);
    ppu_register_function(gowa_libsre_lfqueue_signal_code, lfqueue_send_signal);
    ps3_hle_register_ctx(0xC10931CBu, "cellSpursCreateTasksetWithAttribute(port)",
                         hle_create_taskset_with_attribute);
    ps3_hle_register_ctx(0x011EE38Bu, "_cellSpursLFQueueInitialize(libsre)", lle_lfqueue_initialize);
    ps3_hle_register_ctx(0x8A85674Du, "_cellSpursLFQueuePushBody(libsre)", lle_lfqueue_push_body);
    ps3_hle_register_ctx(0x1656D49Fu, "cellSpursLFQueueAttachLv2EventQueue(port)", hle_lfqueue_attach);
    printf("[gowa] libsre at 0x%08X: %u functions; LFQueue init/push run as firmware code\n",
           r.base, r.funcs_registered);
#endif
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */
