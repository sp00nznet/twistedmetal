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
#include <string.h>
#include <stdlib.h>
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
static void cellVideoOutGetDeviceInfo(ppu_context* ctx);
static void cellNetCtlGetState(ppu_context* ctx);
static void cellSpursTaskAttribute2Initialize(ppu_context* ctx);
static void cellSpursCreateTask2(ppu_context* ctx);
static void cellSpursTaskGetContextSaveAreaSize(ppu_context* ctx);
static void probe_cellSpursSendSignal(ppu_context* ctx);

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

    ps3_hle_register_ctx(0x1E930EEFu, "cellVideoOutGetDeviceInfo", cellVideoOutGetDeviceInfo);
    ps3_hle_register_ctx(0x8B3EBA69u, "cellNetCtlGetState", cellNetCtlGetState);

    ps3_hle_register_ctx(0x8ADADF65u, "_cellSpursTaskAttribute2Initialize",
                         cellSpursTaskAttribute2Initialize);
    ps3_hle_register_ctx(0xE14CA62Du, "cellSpursCreateTask2", cellSpursCreateTask2);
    ps3_hle_register_ctx(0x9034E538u, "cellSpursTaskGetContextSaveAreaSize",
                         cellSpursTaskGetContextSaveAreaSize);
    ps3_hle_register_ctx(0xE0A6DBE4u, "_cellSpursSendSignal", probe_cellSpursSendSignal);
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
extern "C" uint64_t vm_read64(uint64_t a);

/* CellSpursTasksetAttribute2, 512 bytes:
 *   +0x00 revision  +0x04 name  +0x08 args  +0x10 priority[8]
 *   +0x18 max_contention  +0x1C enable_clear_ls  +0x20 task_name_buffer */
#define TSA2_SIZE      512
#define TSA2_ARGS      0x08
#define TSA2_PRIORITY  0x10
#define TSA2_MAXCONT   0x18

/* _cellSpursTasksetAttribute2Initialize(attr, revision) */
static void cellSpursTasksetAttribute2Initialize(ppu_context* ctx)
{
    const uint32_t attr = (uint32_t)ctx->gpr[3];
    const uint32_t revision = (uint32_t)ctx->gpr[4];
    if (!attr) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410901; return; }
    for (uint32_t o = 0; o < TSA2_SIZE; o += 4) vm_write32(attr + o, 0);
    vm_write32(attr + 0x00, revision);
    vm_write32(attr + TSA2_PRIORITY + 0, 0x01010101);   /* priority[0..7] = 1 */
    vm_write32(attr + TSA2_PRIORITY + 4, 0x01010101);
    vm_write32(attr + TSA2_MAXCONT, 8);
    fprintf(stderr, "[cellSpurs] TasksetAttribute2Initialize(attr=0x%08X, rev=%u)\n",
            attr, revision);
    ctx->gpr[3] = 0;
}

/* cellSpursCreateTaskset2(spurs, taskset, attr) */
static void cellSpursCreateTaskset2(ppu_context* ctx)
{
    const uint32_t spurs = (uint32_t)ctx->gpr[3], taskset = (uint32_t)ctx->gpr[4];
    const uint32_t attr = (uint32_t)ctx->gpr[5];
    const uint64_t args = attr ? vm_read64(attr + TSA2_ARGS) : 0;
    const uint32_t maxcont = attr ? vm_read32(attr + TSA2_MAXCONT) : 8;
    fprintf(stderr, "[cellSpurs] CreateTaskset2(spurs=0x%08X, taskset=0x%08X, attr=0x%08X) "
                    "args=0x%llX maxContention=%u -> v1 CreateTaskset\n",
            spurs, taskset, attr, (unsigned long long)args, maxcont);
    const int32_t rc = cellSpursCreateTaskset(
        (void*)(uintptr_t)spurs, (void*)(uintptr_t)taskset, args,
        attr ? (const void*)(uintptr_t)(attr + TSA2_PRIORITY) : nullptr, maxcont);
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

/* ---------------------------------------------------------------------------
 * The game's own logger, on the host.
 *
 * Twisted Metal routes its boot trace through one function, guest 0x0034ACAC,
 * as log(level, fmt, ...). Its output never reaches the console here, so the
 * whole boot is invisible past the handful of lines that go through printf
 * directly — and this binary kept 731 Class::method strings and 359 source
 * paths, so that trace is the best instrument available for finding where boot
 * gives up.
 *
 * ppu_register_function overrides a guest address with a host implementation,
 * so replace it: read the format string out of guest memory and render it with
 * the PPC64 argument registers. Enabled with TM_GAMELOG=1.
 * ------------------------------------------------------------------------- */

static const char* tm_gstr(uint32_t ea, char* buf, size_t cap)
{
    if (!ea) return "(null)";
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        uint32_t w = vm_read32((ea + i) & ~3u);
        char c = (char)((w >> ((3 - ((ea + i) & 3)) * 8)) & 0xFF);
        if (!c) break;
        buf[i] = c;
    }
    buf[i] = 0;
    return buf;
}

/* Render the guest's printf-style call. Only the conversions this title's log
 * strings actually use are handled; anything else prints its own spec back so
 * the line stays readable rather than silently losing an argument. */
static void tm_game_log(ppu_context* ctx)
{
    char fmt[512], sbuf[256];
    tm_gstr((uint32_t)ctx->gpr[4], fmt, sizeof fmt);

    int argi = 5;                       /* r5..r10 hold the varargs */
    fprintf(stderr, "[game] ");
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { fputc(*p, stderr); continue; }
        const char* spec = p++;
        while (*p && !strchr("diouxXeEfgGcspn%", *p)) p++;
        if (*p == '%') { fputc('%', stderr); continue; }
        uint64_t a = (argi <= 10) ? ctx->gpr[argi++] : 0;
        switch (*p) {
        case 's': fprintf(stderr, "%s", tm_gstr((uint32_t)a, sbuf, sizeof sbuf)); break;
        case 'p': fprintf(stderr, "0x%08X", (uint32_t)a); break;
        case 'c': fprintf(stderr, "%c", (char)a); break;
        case 'e': case 'E': case 'f': case 'g': case 'G':
            fprintf(stderr, "<float>"); break;   /* varargs floats are in f1.. */
        case 'd': case 'i': fprintf(stderr, "%d", (int32_t)a); break;
        case 'u': fprintf(stderr, "%u", (uint32_t)a); break;
        case 'o': fprintf(stderr, "%o", (uint32_t)a); break;
        case 'x': fprintf(stderr, "%x", (uint32_t)a); break;
        case 'X': fprintf(stderr, "%X", (uint32_t)a); break;
        default:  fwrite(spec, 1, (size_t)(p - spec + 1), stderr); break;
        }
    }
    fflush(stderr);
    ctx->gpr[3] = 0;
}

/* Replaces the lifted body, which tools/post_lift.py renames to _lifted. The
 * lifter emits direct C++ calls, so a rename is the only way to intercept one;
 * ppu_register_function redirects indirect dispatch only. */
void func_0034ACAC(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) on = getenv("TM_GAMELOG") ? 1 : 0;
    if (on) tm_game_log(ctx);
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * Title metadata from PARAM.SFO.
 *
 * ps3recomp's cellGame defaults to the placeholder id "BLES00000" and exposes
 * cellGame_init_from_paramsfo() for the harness to call -- but nothing calls
 * it, so the title reported itself as "Unknown Title" / BLES00000 and built its
 * game-data and patch-overlay paths under /dev_hdd0/game/BLES00000. Point it at
 * the disc's own PARAM.SFO so the id, title and versions are the real ones.
 * ------------------------------------------------------------------------- */
extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);
extern "C" const char* ppu_vfs_root;

extern "C" void tm_init_title_metadata(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/PS3_GAME/PARAM.SFO",
             ppu_vfs_root && *ppu_vfs_root ? ppu_vfs_root : ".");
    cellGame_init_from_paramsfo(path);
}

/* ---------------------------------------------------------------------------
 * cellVideoOutGetDeviceInfo (0x1E930EEF) — advertise refresh rates.
 *
 * The runtime fills the four available modes (480p/576p/720p/1080p) but writes
 * the struct with a memset plus three byte fields per mode, which leaves
 * CellVideoOutDisplayMode.refreshRates zero on every one. A title scanning the
 * mode table for one that supports the rate it wants finds none, and this game
 * then settles for `Configure: resId=4` — 480p — before failing its renderer
 * init at 1280x720.
 *
 * Layout (all big-endian in guest memory):
 *   CellVideoOutDeviceInfo  +0x00 portType  +0x01 colorSpace  +0x02 latency u16
 *                           +0x04 availableModeCount  +0x05 state
 *                           +0x06 rgbOutputRange  +0x07 reserved[5]
 *                           +0x0C availableModes[32]
 *   CellVideoOutDisplayMode +0x00 resolutionId +0x01 scanMode +0x02 conversion
 *                           +0x03 aspect  +0x04 reserved[2]  +0x06 refreshRates u16
 * ------------------------------------------------------------------------- */
extern "C" void vm_write8(uint64_t a, uint8_t v);
extern "C" void vm_write16(uint64_t a, uint16_t v);

#define VO_RES_1080   1
#define VO_RES_720    2
#define VO_RES_480    4
#define VO_RES_576    5
#define VO_SCAN_PROGRESSIVE 1
#define VO_ASPECT_16_9      2
/* 59.94Hz | 60Hz — what an NTSC HDMI display reports. The device advertises a
 * capability set; cellVideoOutGetState separately reports the mode in use. */
#define VO_RATES  (0x0001 | 0x0004)

static void cellVideoOutGetDeviceInfo(ppu_context* ctx)
{
    const uint32_t videoOut = (uint32_t)ctx->gpr[3];
    const uint32_t info = (uint32_t)ctx->gpr[5];
    if (!info)     { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8002B221; return; }
    if (videoOut)  { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8002B220; return; }

    for (uint32_t o = 0; o < 0x0C + 32 * 8; o += 4) vm_write32(info + o, 0);

    /* TM_VIDEO_MODES overrides the advertised set (comma-separated resolution
     * ids) so the title's mode negotiation can be probed without a rebuild.
     * A real display reports what it supports; this is that knob. */
    uint8_t modes[32] = { VO_RES_480, VO_RES_576, VO_RES_720, VO_RES_1080 };
    uint32_t n = 4;
    if (const char* env = getenv("TM_VIDEO_MODES")) {
        n = 0;
        for (const char* p = env; *p && n < 32; ) {
            modes[n++] = (uint8_t)strtoul(p, (char**)&p, 0);
            while (*p == ',' || *p == ' ') p++;
        }
        if (!n) { modes[0] = VO_RES_720; n = 1; }
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t m = info + 0x0C + i * 8;
        vm_write8(m + 0, modes[i]);
        vm_write8(m + 1, VO_SCAN_PROGRESSIVE);
        vm_write8(m + 2, 0);                 /* conversion: none */
        vm_write8(m + 3, VO_ASPECT_16_9);
        vm_write16(m + 6, VO_RATES);
    }

    vm_write8(info + 0x00, 1);   /* portType = HDMI */
    vm_write8(info + 0x01, 0);   /* colorSpace = RGB */
    vm_write16(info + 0x02, 0);  /* latency */
    vm_write8(info + 0x04, (uint8_t)n);
    vm_write8(info + 0x05, 2);   /* state = connected */
    vm_write8(info + 0x06, 1);   /* rgbOutputRange = full */

    fprintf(stderr, "[cellVideoOut] GetDeviceInfo -> %u modes, refreshRates=0x%04X each\n",
            n, VO_RATES);
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * Guest call tracing (TM_TRACE=1).
 *
 * tools/post_lift.py renames the lifted bodies of the functions in its TRACE
 * table to _lifted; these definitions take the original names, log the
 * arguments and the return value, and call through. It is the only way to see
 * inside a guest function — the lifter emits direct calls, so nothing else can
 * hook one — and it is how the renderer-init failure is being narrowed down.
 * ------------------------------------------------------------------------- */
/* Declared in ppu_recomp.h with C++ linkage, like every lifted function. */

static int tm_trace_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("TM_TRACE") ? 1 : 0;
    return on;
}

static void tm_trace(const char* name, void (*body)(ppu_context*), ppu_context* ctx)
{
    if (!tm_trace_on()) { body(ctx); return; }
    static int depth = 0;
    const uint32_t a3 = (uint32_t)ctx->gpr[3], a4 = (uint32_t)ctx->gpr[4],
                   a5 = (uint32_t)ctx->gpr[5];
    /* The lifter sets ctx->lr to the guest return address before every call,
     * so lr-4 names the exact guest instruction that called this. */
    fprintf(stderr, "[trace]%*s-> %s(0x%08X, 0x%08X, 0x%08X) from 0x%08X\n",
            depth * 2, "", name, a3, a4, a5, (uint32_t)ctx->lr - 4);
    depth++;
    body(ctx);
    depth--;
    fprintf(stderr, "[trace]%*s<- %s = 0x%08X (%d)\n", depth * 2, "", name,
            (uint32_t)ctx->gpr[3], (int32_t)ctx->gpr[3]);
    fflush(stderr);
}

/* FIOS Mutex ctor: r4 is the name, which is the whole point of watching it. */
void func_00779C18(ppu_context* ctx)
{
    char nm[64];
    if (tm_trace_on())
        fprintf(stderr, "[trace] Mutex::ctor(this=0x%08X, name='%s')\n",
                (uint32_t)ctx->gpr[3], tm_gstr((uint32_t)ctx->gpr[4], nm, sizeof nm));
    func_00779C18_lifted(ctx);
}
void func_0076CB00(ppu_context* ctx) { tm_trace("fiosWorkerSetup", func_0076CB00_lifted, ctx); }
void func_0076756C(ppu_context* ctx) { tm_trace("elemCtor?", func_0076756C_lifted, ctx); }

/* The FIOS base constructor: it stamps "FIOS obj ...." on every object the
 * engine builds, so tracing it enumerates exactly what does get constructed. */
void func_007556B4(ppu_context* ctx)
{
    if (tm_trace_on()) {
        char nm[64];
        const uint32_t t = (uint32_t)ctx->gpr[4];
        fprintf(stderr, "[trace] fiosBaseCtor(this=0x%08X, name=0x%08X '%s')\n",
                (uint32_t)ctx->gpr[3], t,
                (t >= 0x10000 && t < 0xF00000) ? tm_gstr(t, nm, sizeof nm) : "?");
    }
    func_007556B4_lifted(ctx);
}
void func_0076C534(ppu_context* ctx) { tm_trace("fiosSchedCtor", func_0076C534_lifted, ctx); }
void func_0076CDF4(ppu_context* ctx) { tm_trace("createSchedForMedia", func_0076CDF4_lifted, ctx); }
void func_0077A088(ppu_context* ctx) { tm_trace("Mutex::lock", func_0077A088_lifted, ctx); }
void func_00671698(ppu_context* ctx) { tm_trace("renderInit", func_00671698_lifted, ctx); }
void func_00670C10(ppu_context* ctx) { tm_trace("f_00670C10", func_00670C10_lifted, ctx); }
void func_00671560(ppu_context* ctx) { tm_trace("f_00671560", func_00671560_lifted, ctx); }
void func_006A9430(ppu_context* ctx) { tm_trace("f_006A9430", func_006A9430_lifted, ctx); }

/* ---------------------------------------------------------------------------
 * cellNetCtlGetState (0x8B3EBA69) — report a state instead of an error.
 *
 * ps3recomp deliberately FAILS this call when offline, because LittleBigPlanet
 * polls it forever waiting for IPObtained and only leaves the loop on ret < 0.
 * Twisted Metal does the opposite: it treats the error as "not ready yet" and
 * retries, so it spun here 704,666 times in a two-minute run and its frame loop
 * never completed a flip.
 *
 * Real hardware returns CELL_OK with state = Disconnected when there is simply
 * no connection; NOT_INITIALIZED is for a missing cellNetCtlInit, which this
 * title does call. Report the truth and let the game go offline.
 * ------------------------------------------------------------------------- */
#define CELL_NET_CTL_STATE_Disconnected 0

static void cellNetCtlGetState(ppu_context* ctx)
{
    const uint32_t state = (uint32_t)ctx->gpr[3];
    if (!state) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80130102; return; }
    vm_write32(state, CELL_NET_CTL_STATE_Disconnected);
    static long long n = 0;
    if (n++ < 3) fprintf(stderr, "[cellNetCtl] GetState() -> OK, Disconnected\n");
    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * cellSpurs "2" task API — the same gap as the taskset, one level down.
 *
 * The runtime implements the 7-argument v1 cellSpursCreateTask; this title
 * creates its tasks through cellSpursCreateTask2, which was unregistered and so
 * faked CELL_OK without creating anything. RPCS3 does not implement it either
 * (only a commented prototype), so the argument order comes from the game's own
 * call site at guest 0x00A1C2A0: r3=taskset, r4=&taskId, r5=elf, r6=argument,
 * r7=attribute — the SDK's
 *
 *   cellSpursCreateTask2(taskset, taskId, elf, argument, attribute)
 *
 * CellSpursTaskAttribute2 is 256 bytes: revision +0x00, sizeContext +0x04,
 * eaContext +0x08 (u64), lsPattern +0x10 (4 words), name +0x20. Unpack it onto
 * the v1 call, which takes context/size/lsPattern/argument as separate guest
 * EAs. A null attribute means the SDK defaults, i.e. no context and no pattern.
 * ------------------------------------------------------------------------- */
extern "C" int32_t cellSpursCreateTask(void* taskset, void* taskId, void* elf,
                                       void* context, uint32_t sizeContext,
                                       uint32_t lsPattern_ea, uint32_t argument_ea);

#define TA2_SIZE       256
#define TA2_SIZECTX    0x04
#define TA2_EACTX      0x08
#define TA2_LSPATTERN  0x10

static void cellSpursTaskAttribute2Initialize(ppu_context* ctx)
{
    const uint32_t attr = (uint32_t)ctx->gpr[3];
    const uint32_t revision = (uint32_t)ctx->gpr[4];
    if (!attr) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80410901; return; }
    for (uint32_t o = 0; o < TA2_SIZE; o += 4) vm_write32(attr + o, 0);
    vm_write32(attr + 0x00, revision);
    ctx->gpr[3] = 0;
}

static void cellSpursCreateTask2(ppu_context* ctx)
{
    const uint32_t taskset = (uint32_t)ctx->gpr[3], taskId = (uint32_t)ctx->gpr[4];
    const uint32_t elf = (uint32_t)ctx->gpr[5], argument = (uint32_t)ctx->gpr[6];
    const uint32_t attr = (uint32_t)ctx->gpr[7];

    const uint32_t sizeContext = attr ? vm_read32(attr + TA2_SIZECTX) : 0;
    const uint32_t eaContext   = attr ? (uint32_t)vm_read64(attr + TA2_EACTX) : 0;
    const uint32_t lsPattern   = attr ? attr + TA2_LSPATTERN : 0;

    fprintf(stderr, "[cellSpurs] CreateTask2(taskset=0x%08X, elf=0x%08X, arg=0x%08X, "
                    "attr=0x%08X ctx=0x%08X/%u) -> v1 CreateTask\n",
            taskset, elf, argument, attr, eaContext, sizeContext);

    const int32_t rc = cellSpursCreateTask((void*)(uintptr_t)taskset,
                                           (void*)(uintptr_t)taskId,
                                           (void*)(uintptr_t)elf,
                                           (void*)(uintptr_t)eaContext,
                                           sizeContext, lsPattern, argument);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* Size of the per-task context save area. The runtime keeps no save area, but
 * the title allocates from this, so a zero would give every task a null
 * context. One SPU local store is the SDK's maximum and always sufficient. */
static void cellSpursTaskGetContextSaveAreaSize(ppu_context* ctx)
{
    ctx->gpr[3] = 0x40000;
}

/* Probe: _cellSpursSendSignal (0xE0A6DBE4). The SPU task parks in WAIT_SIGNAL
 * (taskset syscall 2) waiting for this, while a PPU thread blocks in
 * cellSpursEventFlagWait for that task to set the flag. Logging it answers
 * whether the PPU side ever sends the signal at all. Forwards to the runtime. */
extern "C" int32_t _cellSpursSendSignal(void* taskset, uint32_t taskId);

static void probe_cellSpursSendSignal(ppu_context* ctx)
{
    const uint32_t ts = (uint32_t)ctx->gpr[3], tid = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[cellSpurs] SendSignal(taskset=0x%08X, task=%u)\n", ts, tid);
    ctx->gpr[3] = (uint64_t)(int64_t)_cellSpursSendSignal((void*)(uintptr_t)ts, tid);
}
