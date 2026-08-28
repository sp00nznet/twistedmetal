/*
 * Imports Twisted Metal actually reaches that ps3recomp does not implement.
 *
 * An unregistered NID returns CELL_OK with untouched out-params, which is
 * fine until the missing call is the one that *runs other code*. That is
 * exactly what stalls this title: the engine's FIOS file scheduler builds
 * its mutexes and condition variables inside a sys_ppu_thread_once
 * initialiser. With thread_once faked, the initialiser never runs, the
 * scheduler's lwmutex/lwcond structs stay zeroed, and the game's own
 * diagnostics start printing:
 *
 *     attempt to lock invalid mutex '(null)'
 *     wait for invalid cond 'fios worker cond'      (forever)
 *
 * So thread_once has to be real. The other two here are cheap and were
 * being faked next to it.
 */
#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <mutex>

extern "C" {
void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
uint32_t vm_read32(uint64_t a);
void     vm_write32(uint64_t a, uint32_t v);
uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
int32_t  cellGcmMapEaIoAddress(uint32_t ea, uint32_t io, uint32_t size);
}

/* sysPrxForUser 0xA3E3BE68 — sys_ppu_thread_once(once_ctrl, init_opd).
 *
 * `once_ctrl` points at one guest word: 0 = not run yet, 1 = done. Run the
 * initialiser exactly once, on the calling thread, then flip the word.
 *
 * ponytail: one global lock rather than a per-control-word CAS. Correct for
 * any number of threads and initialisers, and it serialises them — if some
 * title's two initialisers ever need to run concurrently, make it a
 * per-address lock. */
static void sys_ppu_thread_once(ppu_context* ctx)
{
    const uint32_t ctrl = (uint32_t)ctx->gpr[3];
    const uint32_t opd  = (uint32_t)ctx->gpr[4];
    static std::mutex once_lock;

    bool run = false;
    {
        std::lock_guard<std::mutex> g(once_lock);
        if (ctrl && vm_read32(ctrl) == 0) {
            vm_write32(ctrl, 1);
            run = true;
        }
    }
    if (run && opd) {
        fprintf(stderr, "[sysprx] sys_ppu_thread_once: running init opd=0x%08X\n", opd);
        ppu_guest_call(opd, 0, 0, 0, 0, 0, 0, 0, 0);
    }
    ctx->gpr[3] = 0;   /* CELL_OK */
}

/* sysPrxForUser 0x42B23552 — sys_prx_register_library(lib).
 * Registers a static library's export table with the loader. Nothing here
 * resolves exports through the guest loader, so acknowledging is enough. */
static void sys_prx_register_library(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

/* cellGcmSys 0x626E8518 — cellGcmMapEaIoAddressWithFlags(ea, io, size, flags).
 * Same mapping as cellGcmMapEaIoAddress; the flags select RSX page attributes
 * the software command processor does not model. */
static void cellGcmMapEaIoAddressWithFlags(ppu_context* ctx)
{
    const uint32_t ea = (uint32_t)ctx->gpr[3], io = (uint32_t)ctx->gpr[4];
    const uint32_t size = (uint32_t)ctx->gpr[5], flags = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[cellGcmSys] MapEaIoAddressWithFlags(ea=0x%08X, io=0x%08X, "
                    "size=0x%X, flags=0x%X)\n", ea, io, size, flags);
    ctx->gpr[3] = (uint64_t)(int64_t)cellGcmMapEaIoAddress(ea, io, size);
}

static void sys_lwcond_create(ppu_context* ctx);
static void cellSpursTasksetAttribute2Initialize(ppu_context* ctx);
static void cellSpursCreateTaskset2(ppu_context* ctx);
static void cellSpursAttributeEnableSystemWorkload(ppu_context* ctx);
static void sys_spu_printf_initialize(ppu_context* ctx);

extern "C" void tm_hle_register_extra(void)
{
    ps3_hle_register_ctx(0xA3E3BE68u, "sys_ppu_thread_once",      sys_ppu_thread_once);
    ps3_hle_register_ctx(0x42B23552u, "sys_prx_register_library", sys_prx_register_library);
    ps3_hle_register_ctx(0x626E8518u, "cellGcmMapEaIoAddressWithFlags",
                         cellGcmMapEaIoAddressWithFlags);
    ps3_hle_register_ctx(0xDA0EB71Au, "sys_lwcond_create", sys_lwcond_create);

    ps3_hle_register_ctx(0xC2ACDF43u, "_cellSpursTasksetAttribute2Initialize",
                         cellSpursTasksetAttribute2Initialize);
    ps3_hle_register_ctx(0x4A6465E3u, "cellSpursCreateTaskset2", cellSpursCreateTaskset2);
    ps3_hle_register_ctx(0x9DCBCB5Du, "cellSpursAttributeEnableSystemWorkload",
                         cellSpursAttributeEnableSystemWorkload);
    ps3_hle_register_ctx(0x45FE2FCEu, "_sys_spu_printf_initialize", sys_spu_printf_initialize);
}

/* ---------------------------------------------------------------------------
 * sys_lwcond_create (sysPrxForUser 0xDA0EB71A) — fix the struct layout.
 *
 * The PS3 ABI is
 *     struct sys_lwcond_t { sys_lwmutex_t *lwmutex; u32 lwcond_queue; }
 * with a 32-bit guest pointer, so the mutex EA is the FIRST 4 bytes.
 * ps3recomp models it as a big-endian 64-bit EA at +0x00 with the queue id at
 * +0x08. Nothing notices while only the runtime touches the struct — but this
 * game does touch it. Its FIOS wrapper is 24 bytes:
 *
 *     +0x00 vtable   +0x08 name   +0x10 sys_lwcond_t
 *
 * and Cond::wait (guest 0x0077A2E0) validates with
 * `if (*(u32*)(this + 0x10) == 0) print("wait for invalid cond '%s'")`.
 * Against the be64 layout that word is the high half of the EA: always zero.
 * So every FIOS wait short-circuits, the two media threads spin instead of
 * blocking, one of them keeps the scheduler lwmutex, and the main thread
 * deadlocks behind it. That is the whole hang.
 *
 * Write the mutex EA into BOTH words. The first satisfies the guest ABI and
 * the game's check; the second doubles as the queue id and makes the runtime's
 * `(u32)vm_read64(lwcond + 0)` in sys_lwcond_wait still truncate to the mutex,
 * so its wait keeps working unmodified.
 *
 * ponytail: the real fix is the layout in ps3recomp's ppu_sysprx.cpp, which
 * would also need its sys_lwcond_wait to read +0x00 as 32-bit. Doing it here
 * keeps the change inside this repo; upstream it when the runtime is touched.
 * ------------------------------------------------------------------------- */
static void sys_lwcond_create(ppu_context* ctx)
{
    const uint32_t lwcond  = (uint32_t)ctx->gpr[3];
    const uint32_t lwmutex = (uint32_t)ctx->gpr[4];
    vm_write32(lwcond + 0x00, lwmutex);   /* guest ABI: 32-bit lwmutex pointer */
    vm_write32(lwcond + 0x04, lwmutex);   /* lwcond_queue; keeps the be64 read valid */
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * cellSpurs "2" taskset API.
 *
 * The runtime implements the v1 taskset path (cellSpursCreateTaskset), which
 * builds the real big-endian CellSpursTaskset the lifted SPU side reads and
 * sets the init flag cellSpursCreateTask gates on. This game uses the v2 API
 * instead, so that flag was never set and every task creation came back
 *
 *     [cellSpurs] CreateTask REJECT no-init (taskset=... elf=...)
 *
 * The v2 entry points differ from v1 only in carrying their options in an
 * attribute struct rather than as arguments, and the runtime's v1 ignores
 * those options anyway — so forward to it.
 * ------------------------------------------------------------------------- */
extern "C" int32_t cellSpursCreateTaskset(void* spurs, void* taskset, uint64_t args,
                                          const void* priority, uint32_t maxContention);

/* _cellSpursTasksetAttribute2Initialize(attr, revision) */
static void cellSpursTasksetAttribute2Initialize(ppu_context* ctx)
{
    const uint32_t attr = (uint32_t)ctx->gpr[3];
    const uint32_t revision = (uint32_t)ctx->gpr[4];
    if (!attr) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410901; return; }
    /* Only revision/sdkVersion are read back, and CreateTaskset2 below ignores
     * the rest — so set those two rather than guessing the struct's length. */
    vm_write32(attr + 0x00, revision ? revision : 1);
    vm_write32(attr + 0x04, 0);
    fprintf(stderr, "[cellSpurs] TasksetAttribute2Initialize(attr=0x%08X, rev=%u)\n",
            attr, revision);
    ctx->gpr[3] = 0;
}

/* cellSpursCreateTaskset2(spurs, taskset, attr) */
static void cellSpursCreateTaskset2(ppu_context* ctx)
{
    const uint32_t spurs = (uint32_t)ctx->gpr[3], taskset = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[cellSpurs] CreateTaskset2(spurs=0x%08X, taskset=0x%08X) "
                    "-> v1 CreateTaskset\n", spurs, taskset);
    const int32_t rc = cellSpursCreateTaskset((void*)(uintptr_t)spurs,
                                              (void*)(uintptr_t)taskset,
                                              0, nullptr, 0);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* cellSpursAttributeEnableSystemWorkload(attr, priority[8], maxSpu, isPreemptible[8]).
 * Reserves an SPU for the system workload. The runtime schedules workloads on
 * the host, so there is nothing to reserve — acknowledging is the whole job. */
static void cellSpursAttributeEnableSystemWorkload(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

/* _sys_spu_printf_initialize(agent, ...): registers the SPU printf relay. SPU
 * printf output has nowhere to arrive from here; succeed so the CRT continues. */
static void sys_spu_printf_initialize(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}
