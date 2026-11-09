/*
 * Ascension-R -- the PPU end of an ANY2ANY SPURS lock-free queue
 *
 * FIOS hands Edge Zlib's inflate requests to an SPU task through one of these queues. The SPU end is
 * the game's own SPU code, which works on the queue's cache line in guest memory with GETLLAR and
 * PUTLLC, so the bytes and the order of the updates here are the interface.
 *
 * The line, big-endian:
 *
 *   +0x00  u64 pop word   h1 next to retire, h2 done bitmap, h3 wait lanes, h4 tail
 *   +0x08  u64 push word  h5 next to retire, h6 done bitmap, h7 wait lanes, h8 tail
 *   +0x10  entry size     +0x14 depth         +0x18 u64 buffer, bit 0 set
 *   +0x20  SPU port for pops, +0x21 SPU port for pushes
 *   +0x24  direction      +0x28 PPU port 0    +0x2C state: 0, 1 while being set up, 2 ready
 *   +0x30  high half of PPU port 1, then +0x32 fifteen u16 slots of tasks waiting to pop
 *   +0x50  low half of PPU port 1,  then +0x52 fifteen u16 slots of tasks waiting to push
 *   +0x70  eaSignal: the owning taskset, or the SPURS instance with a low tag bit
 *   +0x78  event queue id for pops, +0x7C for pushes
 *
 * Positions run from 0 to 2*depth, so an empty queue and a full one differ. A wait lane packs three
 * 5-bit indices that run from 0 to 29 over the fifteen slots: bits 0-4 the end of the list, bits
 * 10-14 the next waiter to wake, bits 5-9 the last waiter that has gone.
 *
 * A push takes a position by moving h8, copies the entry in, then sets its bit in h6 and retires
 * every leading set bit by moving h5. Retiring is when a waiter can be woken. A PPU thread that
 * finds no room writes 0xFFFF in a slot, joins the lane and sleeps on the event queue.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps3emu/ps3types.h"
#include "ppu_context.h"

extern "C" {
void     ps3_hle_call(uint32_t nid, ppu_context* ctx);
void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
void     lv2_syscall(ppu_context* ctx);
int      ppu_stwcx32(uint64_t ea, uint32_t expected, uint32_t val);
int      ppu_stdcx64(uint64_t ea, uint64_t expected, uint64_t val);
uint8_t  vm_read8(uint64_t addr);
uint16_t vm_read16(uint64_t addr);
uint32_t vm_read32(uint64_t addr);
uint64_t vm_read64(uint64_t addr);
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
void     gowa_lfqueue_register(void);
}

enum : uint32_t {
    SYNC_EBUSY  = 0x80410101u,
    SYNC_EINVAL = 0x80410102u,
    SYNC_EPERM  = 0x80410109u,
    SYNC_ESTAT  = 0x8041010Fu,
    SYNC_EALIGN = 0x80410110u,
    SYNC_ENULL  = 0x80410111u,
    TASK_EINVAL = 0x80410902u,
    TASK_EPERM  = 0x80410904u,
    TASK_ESTAT  = 0x8041090Fu,
    TASK_EALIGN = 0x80410910u,
    TASK_ENULL  = 0x80410911u,
    TASK_EFAULT = 0x80410914u,
    ANY2ANY     = 3,
    SLOT_PPU    = 0xFFFF,
};

static inline void set_result(ppu_context* c, uint32_t v) { c->gpr[3] = (uint64_t)(int64_t)(int32_t)v; }
static inline void fence(void) { __atomic_thread_fence(__ATOMIC_ACQ_REL); }

/* The guest's lwarx/ldarx and stwcx./stdcx.: the same reservation the lifted code keeps, so a
 * commit by an SPU or another thread breaks it the same way. */
static inline uint64_t ll64(ppu_context* c, uint32_t ea)
{
    const uint64_t v = vm_read64(ea);
    c->reserve_addr = ea;
    c->reserve_value = v;
    return v;
}

static inline uint32_t ll32(ppu_context* c, uint32_t ea)
{
    const uint32_t v = vm_read32(ea);
    c->reserve_addr = ea;
    c->reserve_value = v;
    return v;
}

static inline bool sc64(ppu_context* c, uint32_t ea, uint64_t v)
{
    const bool ok = c->reserve_addr == ea && ppu_stdcx64(ea, c->reserve_value, v);
    c->reserve_addr = 0;
    return ok;
}

static inline bool sc32(ppu_context* c, uint32_t ea, uint32_t v)
{
    const bool ok = c->reserve_addr == ea && ppu_stwcx32(ea, (uint32_t)c->reserve_value, v);
    c->reserve_addr = 0;
    return ok;
}

/* a - b on a ring of two depths. */
static inline int32_t ring(int32_t a, int32_t b, int32_t depth) { return a < b ? a - b + 2 * depth : a - b; }
/* a - b on the ring of 30 that the lanes' indices run on. */
static inline int32_t lane_gap(int32_t a, int32_t b) { return a < b ? a - b + 30 : a - b; }
static inline int32_t lane_next(int32_t v) { return v == 29 ? 0 : v + 1; }
static inline int32_t lane_slot(int32_t v) { return v > 14 ? v - 15 : v; }

static inline uint64_t set_u16(uint64_t word, int shift, uint32_t v)
{
    return (word & ~(0xFFFFull << shift)) | ((uint64_t)(v & 0xFFFF) << shift);
}

/* SPURS instance -> tasksets, recorded as the game creates them. A queue owned by a SPURS instance
 * names its waiter by workload id; the runtime creates every taskset as workload 0 and keeps no
 * such table. */
struct taskset_rec { uint32_t spurs, taskset; };
static taskset_rec s_tasksets[32];
static int         s_taskset_n = 0;

static void create_taskset_with_attribute(ppu_context* c)
{
    const uint32_t spurs = (uint32_t)c->gpr[3], taskset = (uint32_t)c->gpr[4];
    const int32_t rc = cellSpursCreateTasksetWithAttribute((void*)(uintptr_t)spurs, (void*)(uintptr_t)taskset,
                                                           (const void*)(uintptr_t)c->gpr[5]);
    if (rc == 0 && s_taskset_n < 32) s_tasksets[s_taskset_n++] = { spurs, taskset };
    c->gpr[3] = (uint64_t)(int64_t)rc;
}

static uint32_t taskset_of(uint32_t spurs, uint32_t wid)
{
    uint32_t found = 0;
    int matches = 0;
    for (int i = 0; i < s_taskset_n; i++) {
        if (s_tasksets[i].spurs != spurs || vm_read32(s_tasksets[i].taskset + 0x74) != wid) continue;
        if (!found) found = s_tasksets[i].taskset;
        matches++;
    }
    static int warned = 0;
    if (matches > 1 && warned++ < 4)
        fprintf(stderr, "[lfqueue] %d tasksets of spurs 0x%08X have wid %u; signalling 0x%08X\n", matches, spurs, wid, found);
    return found;
}

/* Wakes the SPU task whose word was left in a slot. */
static uint32_t signal_task(uint32_t ea_signal, uint32_t waiter)
{
    const uint32_t taskset = (ea_signal & 0xF) == 1 ? taskset_of(ea_signal & ~0xFu, (waiter >> 8) & 0xFFFFFF) : ea_signal;
    if (!taskset) {
        fprintf(stderr, "[lfqueue] signal ea=0x%08X waiter=0x%08X: no taskset\n", ea_signal, waiter);
        return TASK_EINVAL;
    }
    spu_taskset_signal_task(taskset, waiter & 0xFF);
    return 0;
}

/* cellSpursLFQueueAttachLv2EventQueue(queue): an event queue on a dynamic SPU port for each side,
 * and a local PPU port connected to it. */
static void attach_event_queues(ppu_context* c)
{
    const uint32_t q = (uint32_t)c->gpr[3];
    if (!q)       { set_result(c, TASK_ENULL);  return; }
    if (q & 0x7F) { set_result(c, TASK_EALIGN); return; }
    const uint32_t dir = vm_read32(q + 0x24);
    const bool any2any = dir == ANY2ANY;
    if (any2any ? (vm_read8(q + 0x20) != 0xFF || vm_read8(q + 0x21) != 0xFF) : vm_read32(q + 0x20) != 0xFFFFFFFFu) {
        set_result(c, TASK_ESTAT);
        return;
    }
    const uint32_t owner = vm_read32(q + 0x74);
    const uint32_t spurs = (owner & 0xF) ? (owner & ~0xFu) : vm_read32(owner + 0x64);
    const uint32_t scratch = ((uint32_t)c->gpr[1] - 0x2000u) & ~0xFu;   /* out-parameters, below the caller's frame */
    const int n = any2any ? 2 : 1;
    uint32_t eq[2] = { 0, 0 }, ppu_port[2] = { 0, 0 };
    uint8_t  spu_port[2] = { 0, 0 };
    for (int i = 0; i < n; i++) {
        eq[i] = sys_event_queue_create_direct(0, any2any ? 15 : 1);
        if (!eq[i]) { set_result(c, TASK_EPERM); return; }
        vm_write8(scratch, 0);
        const int32_t rc = cellSpursAttachLv2EventQueue((void*)(uintptr_t)spurs, eq[i], (uint8_t*)(uintptr_t)scratch, 1);
        if (rc) { set_result(c, (uint32_t)rc | 0x80410900u); return; }
        spu_port[i] = vm_read8(scratch);
        if (any2any) {
            ppu_context t;
            memset(&t, 0, sizeof t);
            t.gpr[3] = scratch + 4; t.gpr[4] = 1 /* SYS_EVENT_PORT_LOCAL */; t.gpr[5] = 0;
            sys_event_port_create(&t);
            ppu_port[i] = vm_read32(scratch + 4);
            memset(&t, 0, sizeof t);
            t.gpr[3] = ppu_port[i]; t.gpr[4] = eq[i];
            sys_event_port_connect_local(&t);
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
    c->gpr[3] = 0;
}

static void write_line(uint32_t q, uint32_t buffer, uint32_t size, uint32_t depth, uint32_t dir, uint32_t owner)
{
    vm_write16(q + 0x00, 0);
    vm_write32(q + 0x10, size);
    vm_write16(q + 0x02, 0);
    vm_write32(q + 0x14, depth);
    vm_write16(q + 0x06, 0);
    vm_write64(q + 0x18, buffer);
    vm_write16(q + 0x08, 0);
    vm_write32(q + 0x24, dir);
    vm_write16(q + 0x0A, 0);
    vm_write16(q + 0x0E, 0);
    vm_write16(q + 0x30, 0);
    for (uint32_t i = 0; i < 15; i++) vm_write16(q + 0x32 + 2 * i, 0);
    vm_write16(q + 0x50, 0);
    for (uint32_t i = 0; i < 15; i++) vm_write16(q + 0x52 + 2 * i, 0);
    vm_write64(q + 0x70, owner);
    if (dir == ANY2ANY) {
        vm_write64(q + 0x18, vm_read64(q + 0x18) | 1);
        vm_write16(q + 0x04, 0);
        vm_write8(q + 0x21, 0xFF);
        vm_write16(q + 0x50, 0xFFFF);
        vm_write16(q + 0x0C, 0);
        vm_write8(q + 0x20, 0xFF);
        vm_write32(q + 0x28, 0xFFFFFFFFu);
        vm_write16(q + 0x30, 0xFFFF);
        vm_write32(q + 0x78, 0);
        vm_write32(q + 0x7C, 0);
    } else {
        vm_write32(q + 0x28, 0);
        vm_write64(q + 0x78, 0);
        vm_write32(q + 0x20, 0xFFFFFFFFu);
    }
}

static uint32_t init_line(ppu_context* c, uint32_t q, uint32_t buffer, uint32_t size, uint32_t depth, uint32_t dir, uint32_t owner)
{
    if (!q) return SYNC_ENULL;
    if (size) {
        if (!buffer) return SYNC_ENULL;
        if (size > 0x4000 || (size & 0xF)) return SYNC_EINVAL;
    }
    if (!depth || (depth & 0xFFFF8000u) || dir > 3) return SYNC_EINVAL;
    if ((q & 0x7F) || (buffer & 0xF)) return SYNC_EALIGN;

    uint32_t next;
    do {
        const uint32_t state = ll32(c, q + 0x2C);
        if (state == 0) {
            for (uint32_t i = 0; i < 16; i++)
                if (vm_read64(q + i * 8)) { c->reserve_addr = 0; return SYNC_ESTAT; }
            next = 1;
        } else {
            if (state != 2) { c->reserve_addr = 0; return SYNC_ESTAT; }
            next = state;
        }
    } while (!sc32(c, q + 0x2C, next));
    fence();
    if (next == 2) {
        if (vm_read32(q + 0x10) != size || vm_read32(q + 0x14) != depth || vm_read64(q + 0x18) != buffer ||
            vm_read64(q + 0x70) != owner || vm_read32(q + 0x24) != dir) return SYNC_EINVAL;
    } else {
        write_line(q, buffer, size, depth, dir, owner);
        fence();
        vm_write32(q + 0x2C, 2);
    }
    fence();
    return 0;
}

/* _cellSpursLFQueueInitialize(owner, queue, buffer, size, depth, direction) */
static void lfqueue_initialize(ppu_context* c)
{
    const uint32_t owner = (uint32_t)c->gpr[3], q = (uint32_t)c->gpr[4], buffer = (uint32_t)c->gpr[5];
    const uint32_t size = (uint32_t)c->gpr[6], depth = (uint32_t)c->gpr[7], dir = (uint32_t)c->gpr[8];
    uint32_t rc;
    if ((owner & 0xF) == 0 && vm_read32(owner + 0x74) > 0x1F) rc = TASK_EINVAL;   /* not a taskset's wid */
    else {
        rc = init_line(c, q, buffer, size, depth, dir, owner);
        if ((int32_t)rc < 0) rc = (rc & 0xFF) | 0x80410900u;
    }
    fprintf(stderr, "[lfqueue] init owner=0x%08X q=0x%08X buf=0x%08X size=%u depth=%u dir=%u -> 0x%08X\n",
            owner, q, buffer, size, depth, dir, rc);
    set_result(c, rc);
}

/* Sleeps until a pop or a retire signals this queue's push side. */
static void wait_for_room(ppu_context* c, uint32_t q)
{
    c->gpr[3] = vm_read32(q + 0x7C);
    c->gpr[4] = ((uint32_t)c->gpr[1] - 0x200u) & ~0xFu;
    c->gpr[5] = 0;
    c->gpr[11] = 0x82;   /* sys_event_queue_receive */
    lv2_syscall(c);
    if ((int32_t)c->gpr[3] != 0) {
        static int warned = 0;
        if (warned++ < 8) fprintf(stderr, "[lfqueue] waiting on q=0x%08X: 0x%08X\n", q, (uint32_t)c->gpr[3]);
    }
}

/* Takes the next position to push at. A push word and, when the thread has to join the wait lane,
 * the slot's word are updated: the slot first, under its own reservation, then the push word, as
 * long as neither word has moved under it. */
static uint32_t get_push_position(ppu_context* c, uint32_t q, int32_t* position, bool blocking)
{
    const int32_t depth = (int32_t)vm_read32(q + 0x14), span = depth * 2;
    const uint32_t push = q + 8;
    bool woken = false, slow = false;
    uint64_t seen = 0, word = 0, pop_seen = 0, next = 0;
    int32_t slot = 0, h8 = 0;
    uint32_t slot_ea = 0, slot_word = 0, rc = 0;
    bool blocked = false, join = false;

top:
    if (!slow) goto attempt;
reload:
    seen = vm_read64(push);
    slot = lane_slot((int32_t)((seen >> 16) & 31));
attempt:
    if (!slow) {
        word = ll64(c, push);
        seen = word;
        slot = lane_slot((int32_t)((word >> 16) & 31));
        slot_ea = (q + 0x52 + 2 * (uint32_t)slot) & ~3u;
        slot_word = vm_read32(slot_ea);
    } else {
        slot_ea = (q + 0x52 + 2 * (uint32_t)slot) & ~3u;
        const uint32_t w = ll32(c, slot_ea);
        fence();
        if (seen != vm_read64(push)) goto reload;
        slot_word = w;
        word = seen;
    }
    {
        pop_seen = vm_read64(q);
        h8 = (int16_t)word;
        const int32_t used = ring(h8, (int32_t)(pop_seen >> 48), depth);
        const int32_t open = ring(h8, (int32_t)(word >> 48), depth);
        const uint32_t lanes = (uint32_t)(word >> 16) & 0xFFFF;
        const int32_t end = lanes & 31, gone = (lanes >> 5) & 31, wake = (lanes >> 10) & 31;
        const int32_t waiting = lane_gap(end, wake), signalled = lane_gap(wake, gone);

        bool full, over;
        if (!woken) {
            full = used + signalled >= depth;
            over = open + signalled > 15;
        } else {
            full = used >= depth;
            over = open > 15;
        }
        blocked = full || over;
        if (waiting != 0 && !woken) blocked = true;      /* someone is ahead of this thread */
        const bool list_full = waiting == 15 || signalled == 15;

        rc = 0;
        join = false;
        uint32_t new_lanes = lanes;
        if (woken) {
            if (!blocked) new_lanes = (lanes & ~(31u << 5)) | ((uint32_t)lane_next(gone) << 5);
        } else if (blocked) {
            rc = SYNC_EBUSY;
            if (!list_full && blocking) {
                new_lanes = (lanes & ~31u) | (uint32_t)lane_next(end);
                slot_word |= ((q + 0x52 + 2 * (uint32_t)slot) & 3) ? 0x0000FFFFu : 0xFFFF0000u;
                join = true;
            }
        }
        const int32_t tail = h8 + 1 >= span ? 0 : h8 + 1;
        next = set_u16(word, 16, new_lanes);
        next = set_u16(next, 0, (uint32_t)((!blocked && rc == 0) ? tail : h8));
    }
    if (!slow) {
        if (join) { slow = true; goto reload; }
        if (!sc64(c, push, next)) goto attempt;
    } else {
        if (!sc32(c, slot_ea, slot_word)) goto attempt;
        for (;;) {
            const uint64_t now = ll64(c, push);
            if (now != seen) goto reload;
            fence();
            if (vm_read64(q) != pop_seen) goto reload;
            if (join && vm_read16(q + 0x52 + 2 * (uint32_t)slot) != SLOT_PPU) goto reload;
            if (blocking && vm_read8(q + 0x21) == 0xFF) { c->reserve_addr = 0; return SYNC_ESTAT; }
            if (sc64(c, push, next)) break;
        }
    }
    *position = h8;
    if (join) {
        wait_for_room(c, q);
    } else {
        if (!blocked || rc) return rc;
    }
    woken = true;
    goto top;
}

static uint32_t send_signals(ppu_context* c, uint32_t q, const uint32_t* signals, int n)
{
    for (int i = 0; i < n; i++) {
        const uint32_t v = signals[i];
        if ((v & 0xFF00) == 0xFF00) {
            const uint32_t port = (v & 0xFF) == 1 ? vm_read32(q + 0x28)
                                                  : ((uint32_t)vm_read16(q + 0x30) << 16) | vm_read16(q + 0x50);
            if (port == 0xFFFFFFFFu) return SYNC_EPERM;
            c->gpr[3] = port;
            c->gpr[4] = c->gpr[5] = c->gpr[6] = 0;
            c->gpr[11] = 0x8A;   /* sys_event_port_send */
            lv2_syscall(c);
        } else {
            const uint32_t rc = signal_task(vm_read32(q + 0x74), v);
            if (rc) return rc;
        }
    }
    return 0;
}

/* Marks a position written, retires the leading written positions and wakes one waiter per retired
 * position on each side. Waking a popper updates the pop word too: that word is committed first,
 * then the push word, as long as the push word has not moved. */
static uint32_t complete_push(ppu_context* c, uint32_t q, int32_t position)
{
    const int32_t depth = (int32_t)vm_read32(q + 0x14), span = depth * 2;
    const uint32_t push = q + 8;
    bool slow = false;
    int total = 0;
    uint32_t signals[64];
    uint64_t pop, word, seen;
    uint32_t pop_lanes;

start:
    if (!slow) {
        word = ll64(c, push);
        fence();
        pop = vm_read64(q);
    } else {
        pop = ll64(c, q);
        fence();
        word = vm_read64(push);
    }
    seen = word;
    {
        const int32_t h5 = (int32_t)(word >> 48), h1 = (int32_t)(pop >> 48);
        const uint32_t done = (uint32_t)(word >> 32) & 0xFFFF;
        pop_lanes = (uint32_t)(pop >> 16) & 0xFFFF;
        uint32_t push_lanes = (uint32_t)(word >> 16) & 0xFFFF;
        const int32_t offset = ring(position, h5, depth);
        const int32_t pop_wake = (pop_lanes >> 10) & 31, pop_gone = (pop_lanes >> 5) & 31;
        const int32_t push_wake = (push_lanes >> 10) & 31, push_gone = (push_lanes >> 5) & 31;
        int32_t pops = ring((int16_t)pop, h1, depth) + lane_gap(pop_wake, pop_gone);
        int32_t pushes = ring((int16_t)word, h1, depth) + lane_gap(push_wake, push_gone);
        const int32_t shift = 15 - offset;
        const uint32_t bitmap = done | ((shift & 0x20) ? 0u : 1u << (shift & 31));
        const uint32_t clear = ~(bitmap | 0xFFFF0000u);
        const int32_t retired = (clear ? __builtin_clz(clear) : 32) - 16;
        int32_t head = h5 + retired;
        if (head >= span) head -= span;
        int32_t pop_waiters = pops <= 15 ? lane_gap(pop_lanes & 31, pop_wake) : 0;
        int32_t push_waiters = depth > pushes ? lane_gap(push_lanes & 31, push_wake) : 0;

        word = set_u16(word, 48, (uint32_t)head);
        word = set_u16(word, 32, retired >= 32 ? 0u : bitmap << retired);

        int count = 0;
        for (int32_t left = retired - 1; left >= 0; left--) {
            if (total <= left && pop_waiters != 0 && pops <= 15 && total + count < 64) {
                const int32_t wake = (pop_lanes >> 10) & 31;
                const uint32_t w = vm_read16(q + 0x32 + 2 * (uint32_t)lane_slot(wake));
                signals[total + count] = w == SLOT_PPU ? 0xFF01u : w;
                if (!slow) { slow = true; goto start; }
                pop_lanes = (pop_lanes & ~(31u << 10)) | ((uint32_t)lane_next(wake) << 10);
                count++;
                pops++;
                pop_waiters--;
            }
            if (push_waiters != 0 && pushes < depth && total + count < 64) {
                const int32_t wake = (push_lanes >> 10) & 31;
                const uint32_t w = vm_read16(q + 0x52 + 2 * (uint32_t)lane_slot(wake));
                signals[total + count] = w == SLOT_PPU ? 0xFF02u : w;
                push_lanes = (push_lanes & ~(31u << 10)) | ((uint32_t)lane_next(wake) << 10);
                count++;
                pushes++;
                push_waiters--;
            }
        }
        word = set_u16(word, 16, push_lanes);
        pop = set_u16(pop, 16, pop_lanes);
        if (!(slow ? sc64(c, q, pop) : sc64(c, push, word))) goto start;
        total += count;
    }
    if (slow) {
        for (;;) {
            const uint64_t now = ll64(c, push);
            if (now != seen) goto start;
            fence();
            if (vm_read16(q + 4) != pop_lanes) goto start;
            if (sc64(c, push, word)) break;
        }
    }
    return send_signals(c, q, signals, total);
}

/* _cellSpursLFQueuePushBody(queue, entry, isBlocking) */
static void lfqueue_push_body(ppu_context* c)
{
    const uint32_t q = (uint32_t)c->gpr[3], entry = (uint32_t)c->gpr[4];
    const bool blocking = (uint32_t)c->gpr[5] != 0;
    if (q & 0x7F)      { set_result(c, TASK_EALIGN); return; }
    if (!q || !entry)  { set_result(c, TASK_ENULL);  return; }
    if (vm_read32(q + 0x24) != ANY2ANY) {
        static int warned = 0;
        if (warned++ < 4) fprintf(stderr, "[lfqueue] q=0x%08X: direction %u is not implemented\n", q, vm_read32(q + 0x24));
        set_result(c, TASK_EINVAL);
        return;
    }
    int32_t position = 0;
    uint32_t rc = get_push_position(c, q, &position, blocking);
    if (rc) { set_result(c, (rc & 0xFF) | 0x80410900u); return; }

    const int32_t depth = (int32_t)vm_read32(q + 0x14);
    const uint32_t size = vm_read32(q + 0x10);
    const uint32_t buffer = (uint32_t)vm_read64(q + 0x18) & ~1u;
    c->gpr[3] = buffer + size * (uint32_t)(position >= depth ? position - depth : position);
    c->gpr[4] = entry;
    c->gpr[5] = size;
    ps3_hle_call(0x6BF66EA7u /* _sys_memcpy */, c);

    rc = complete_push(c, q, position);
    if (rc == TASK_EINVAL || rc == TASK_ESTAT) rc = TASK_EFAULT;
    else if ((int32_t)rc < 0) rc = (rc & 0xFF) | 0x80410900u;
    set_result(c, rc);
}

extern "C" void gowa_lfqueue_register(void)
{
    ps3_hle_register_ctx(0xC10931CBu, "cellSpursCreateTasksetWithAttribute(port)", create_taskset_with_attribute);
    ps3_hle_register_ctx(0x011EE38Bu, "_cellSpursLFQueueInitialize(port)", lfqueue_initialize);
    ps3_hle_register_ctx(0x8A85674Du, "_cellSpursLFQueuePushBody(port)", lfqueue_push_body);
    ps3_hle_register_ctx(0x1656D49Fu, "cellSpursLFQueueAttachLv2EventQueue(port)", attach_event_queues);
    fprintf(stderr, "[gowa] SPURS queues: the port's own code, no firmware\n");
}
