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
#include <thread>
#include <chrono>

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
static uint64_t g_edge_bytes;    /* total inflated, for progress */
static uint32_t g_edge_dst;      /* where the inflated contents.dat landed */
static uint32_t g_tasksets[8];    /* every taskset the title creates */
static int      g_ntasksets;
static void cellSpursCreateTaskset2(ppu_context* ctx);
static void cellSpursAttributeEnableSystemWorkload(ppu_context* ctx);
static void sys_spu_printf_initialize(ppu_context* ctx);
static void cellVideoOutGetDeviceInfo(ppu_context* ctx);
static void cellNetCtlGetState(ppu_context* ctx);
static void cellSpursTaskAttribute2Initialize(ppu_context* ctx);
static void cellSpursCreateTask2(ppu_context* ctx);
static void cellSpursTaskGetContextSaveAreaSize(ppu_context* ctx);
static void probe_cellSpursSendSignal(ppu_context* ctx);
static void probe_cellGcmGetFlipStatus(ppu_context* ctx);
static void probe_EventFlagWait(ppu_context* ctx);

extern "C" void tm_vdec_register(void);   /* src/vdec_hle.cpp */

extern "C" void tm_hle_register_extra(void)
{
    tm_vdec_register();
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
    ps3_hle_register_ctx(0x72A577CEu, "cellGcmGetFlipStatus", probe_cellGcmGetFlipStatus);
    ps3_hle_register_ctx(0x373523D4u, "cellSpursEventFlagWait", probe_EventFlagWait);
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
    if (g_ntasksets < (int)(sizeof g_tasksets / sizeof *g_tasksets))
        g_tasksets[g_ntasksets++] = taskset;
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
    /* The title's localised text is UTF-16BE, so an ASCII read stops on the
     * leading zero byte and reports every string as empty. Detect that and
     * transcode the Latin-1 range, which covers every name we look at. */
    if ((vm_read32(ea & ~3u) >> ((3 - (ea & 3)) * 8) & 0xFF) == 0) {
        for (; i + 1 < cap; i++) {
            const uint32_t a = ea + i * 2;
            const uint32_t hi = (vm_read32(a & ~3u) >> ((3 - (a & 3)) * 8)) & 0xFF;
            const uint32_t lo = (vm_read32((a + 1) & ~3u) >> ((3 - ((a + 1) & 3)) * 8)) & 0xFF;
            if (!hi && !lo) break;
            buf[i] = hi ? '?' : (char)lo;
        }
        buf[i] = 0;
        return buf;
    }
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
    int fargi = 1;                      /* f1..f13 hold varargs floats */
    /* TM_GAMELOG=2 prefixes each line with the guest address that logged it,
     * which is the only practical way to locate a message in a stripped
     * binary whose string references the static cross-reference misses. */
    if (getenv("TM_GAMELOG") && atoi(getenv("TM_GAMELOG")) > 1)
        fprintf(stderr, "[game @%08X] ", (uint32_t)ctx->lr - 4);
    else
        fprintf(stderr, "[game] ");
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { fputc(*p, stderr); continue; }
        /* The Ui state machine's format strings start "!@#$%^&* %s::onEnter",
         * so a scan that skips forward to the next conversion character walks
         * straight over "^&* " and consumes the class name as the argument to
         * a spec that was never there. Only accept a run of real printf flags,
         * width, precision and length modifiers; anything else is literal. */
        const char* spec = p;
        const char* q = p + 1;
        while (*q && strchr("-+ #0123456789.hlLqjzt'", *q)) q++;
        if (*q == '%') { fputc('%', stderr); p = q; continue; }
        if (!*q || !strchr("diouxXeEfgGcspn", *q)) { fputc('%', stderr); continue; }
        p = q;
        uint64_t a = (argi <= 10) ? ctx->gpr[argi++] : 0;
        switch (*p) {
        case 's': fprintf(stderr, "%s", tm_gstr((uint32_t)a, sbuf, sizeof sbuf)); break;
        case 'p': fprintf(stderr, "0x%08X", (uint32_t)a); break;
        case 'c': fprintf(stderr, "%c", (char)a); break;
        case 'e': case 'E': case 'f': case 'g': case 'G':
            /* A varargs float consumes its GPR slot *and* is passed in f1..f13,
             * so the argi bump above is right and the value comes from the FPR.
             * The Ui state machine timestamps every transition with %f, which is
             * how long each legal screen has been up. */
            fprintf(stderr, "%g", fargi <= 13 ? ctx->fpr[fargi++] : 0.0); break;
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

/* The title's other logger, same log(level, fmt, ...) shape. Everything the
 * FIOS layer, the ArchiveLoader and the WorldLoader say goes here rather than
 * through 0x0034ACAC -- including the "Failed to load %s!" that ends the boot. */
void func_00980B20_lifted(ppu_context* ctx);

void func_00980B20(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) on = getenv("TM_GAMELOG") ? 1 : 0;
    if (on) tm_game_log(ctx);
    func_00980B20_lifted(ctx);
}

/* And a third, log(channel, fmt, ...), used by the Ui state machine -- the
 * "!@#$%^&* UiLegal_N::onEnter() triggered at [%f]" line every legal screen
 * prints comes through here, timestamp and all. */
void func_004740EC_lifted(ppu_context* ctx);

void func_004740EC(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) on = getenv("TM_GAMELOG") ? 1 : 0;
    if (on) tm_game_log(ctx);
    func_004740EC_lifted(ctx);
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

/* TM_TRACE=2 adds the FIOS mutex/ctor chatter, which is thousands of lines per
 * second and buries the handful of calls that say where the boot actually is. */
static int tm_trace_verbose(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("TM_TRACE"); v = e ? atoi(e) : 0; }
    return v >= 2;
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
    if (tm_trace_verbose())
        fprintf(stderr, "[trace] Mutex::ctor(this=0x%08X, name='%s')\n",
                (uint32_t)ctx->gpr[3], tm_gstr((uint32_t)ctx->gpr[4], nm, sizeof nm));
    func_00779C18_lifted(ctx);
}
void func_0076CB00(ppu_context* ctx) { tm_trace("fiosWorkerSetup", func_0076CB00_lifted, ctx); }
void func_0076756C(ppu_context* ctx)
{ if (tm_trace_verbose()) tm_trace("elemCtor?", func_0076756C_lifted, ctx); else func_0076756C_lifted(ctx); }

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
void func_0077A088(ppu_context* ctx)
{ if (tm_trace_verbose()) tm_trace("Mutex::lock", func_0077A088_lifted, ctx); else func_0077A088_lifted(ctx); }
void func_00671698(ppu_context* ctx) { tm_trace("renderInit", func_00671698_lifted, ctx); }

/* Front-end path: does the title reach the UI load, the legal screens and the
 * intro movie? Each name comes from the format string the function logs. */
void func_0020C26C_lifted(ppu_context* ctx);
void func_0020C33C_lifted(ppu_context* ctx);
void func_0020C478_lifted(ppu_context* ctx);
void func_0035FBFC_lifted(ppu_context* ctx);
void func_0035FD84_lifted(ppu_context* ctx);
void func_003609AC_lifted(ppu_context* ctx);
void func_00474110_lifted(ppu_context* ctx);
void func_006AC648_lifted(ppu_context* ctx);
void func_0020C26C(ppu_context* ctx) { tm_trace("WorldLoader::loadGame", func_0020C26C_lifted, ctx); }
void func_0020C33C(ppu_context* ctx) { tm_trace("WorldLoader::loadUi", func_0020C33C_lifted, ctx); }
void func_0020C478(ppu_context* ctx) { tm_trace("WorldLoader::loadCinema", func_0020C478_lifted, ctx); }
void func_0035FBFC(ppu_context* ctx) { tm_trace("ArchiveLoader::waitIO", func_0035FBFC_lifted, ctx); }
void func_0035FD84(ppu_context* ctx) { tm_trace("ArchiveLoader::load", func_0035FD84_lifted, ctx); }
void func_003609AC(ppu_context* ctx) { tm_trace("ArchiveLoader::thread", func_003609AC_lifted, ctx); }
void func_00474110(ppu_context* ctx) { tm_trace("UiLegal_1::onEnter", func_00474110_lifted, ctx); }
/* The rest of the legal-screen sequence. UiLegal_1 enters; whether its
 * update() advances to 2/3/4 is what says if the front end is running or
 * just sitting there, and the intro movie is on the far side of it. */
void func_00474198_lifted(ppu_context* ctx);
void func_0047447C_lifted(ppu_context* ctx);
void func_00474710_lifted(ppu_context* ctx);
void func_00474A68_lifted(ppu_context* ctx);
void func_00475150_lifted(ppu_context* ctx);
void func_00475450_lifted(ppu_context* ctx);
/* update() runs every frame; only its first calls are informative. */
void func_00474198(ppu_context* ctx)
{
    static int n = 0;
    if (tm_trace_on() && (n++ < 3 || tm_trace_verbose()))
        tm_trace("UiLegal_1::update", func_00474198_lifted, ctx);
    else
        func_00474198_lifted(ctx);
}
void func_0047447C(ppu_context* ctx) { tm_trace("UiLegal_2::onEnter", func_0047447C_lifted, ctx); }
void func_00474710(ppu_context* ctx) { tm_trace("UiLegal_3::onEnter", func_00474710_lifted, ctx); }
void func_00474A68(ppu_context* ctx) { tm_trace("UiLegal_4::onEnter", func_00474A68_lifted, ctx); }
void func_00475150(ppu_context* ctx) { tm_trace("UiLegal_Health::onEnter", func_00475150_lifted, ctx); }
void func_00475450(ppu_context* ctx) { tm_trace("UiLegal_ESRB::onEnter", func_00475450_lifted, ctx); }
/* The Ui state machine itself: which state it enters, how often it ticks,
 * and the timer each screen sets. Legal_4 enters and never leaves, and
 * these say whether the driver stops calling it or the transition never
 * fires. update() runs per frame, so it is verbose-only. */
void func_005BA908_lifted(ppu_context* ctx);
void func_005BB358_lifted(ppu_context* ctx);
void func_005CAEA0_lifted(ppu_context* ctx);
/* r4 is the state being entered. Most Ui classes are not in the trace table, so
 * print the object's vtable and its first slots: matching a slot against a known
 * onEnter (UiLegal_3's is 0x00474710) names the slot, and the same slot then
 * names every other state the machine walks into -- including whatever it does
 * instead of the intro movie. */
/* The UI state machine pointer, captured from UiState::enter(machine, new,
 * prev). tm_force_state() needs it to drive a transition itself. */
static uint32_t g_ui_machine = 0;
void func_005BA908(ppu_context* ctx)
{
    g_ui_machine = (uint32_t)ctx->gpr[3];
    if (tm_trace_on()) {
        const uint32_t st = (uint32_t)ctx->gpr[4];
        const uint32_t vt = st ? vm_read32(st) : 0;
        fprintf(stderr, "[trace] UiState::enter state=0x%08X vtable=0x%08X [", st, vt);
        /* The vtable holds PPC64 function descriptors, so the slot value is
         * not the code address -- dereference it to get the entry point, which
         * is what names the state against the lifted functions. */
        for (uint32_t i = 0; i < 10 && vt; i++) {
            const uint32_t d = vm_read32(vt + i * 4);
            fprintf(stderr, " %08X", d ? vm_read32(d) : 0);
        }
        fprintf(stderr, " ]\n");
    }
    tm_trace("UiState::enter", func_005BA908_lifted, ctx);
}
/* TM_FORCESTATE=<vtable hex>[,seconds] drives the state machine into the state
 * built on that vtable. Two of them reach WorldLoader::loadCinema and so the
 * campaign intro movies: 0x00CEF810 (onEnter 0x004822E8) and 0x00CF1420
 * (onEnter 0x004D30D8). The menu cannot be operated yet -- it renders black and
 * takes no pad input -- so this is the only way to exercise the movie path.
 * The transition is issued from inside UiState::update so it happens on the UI
 * thread with a live stack, and that update is skipped for the one frame. */
static uint32_t tm_find_state(uint32_t vt)
{
    for (uint32_t a = 0x17E00000u; a < 0x18400000u; a += 4)
        if (vm_read32(a) == vt) return a;
    return 0;
}

/* TM_FORCESTATE=list prints every UI state object that actually exists. Only
 * some of the 93 vtables in the image are ever instantiated, so forcing one by
 * vtable finds nothing unless the title built it. A UI state is recognisable
 * without knowing its class: vtable slot 4 always resolves to 0x005CB92C. */
static void tm_list_states(void)
{
    int n = 0;
    for (uint32_t a = 0x17E00000u; a < 0x18400000u; a += 4) {
        const uint32_t vt = vm_read32(a);
        if (vt < 0x00CE0000u || vt >= 0x00CF2000u || (vt & 3)) continue;
        const uint32_t d4 = vm_read32(vt + 16);
        if (d4 < 0x00E00000u || d4 >= 0x00F80000u) continue;
        if (vm_read32(d4) != 0x005CB92Cu) continue;
        const uint32_t d3 = vm_read32(vt + 12), d0 = vm_read32(vt + 0);
        fprintf(stderr, "[states] obj=0x%08X vtable=0x%08X onEnter=0x%06X update=0x%06X\n",
                a, vt, d3 ? vm_read32(d3) : 0, d0 ? vm_read32(d0) : 0);
        n++;
    }
    fprintf(stderr, "[states] %d live UI state objects\n", n);
    fflush(stderr);
}

extern "C" void ps3_indirect_call(ppu_context* ctx);
static uint32_t g_forced_state = 0;

static bool tm_force_state(ppu_context* ctx)
{
    static int armed = -1;
    static uint32_t vt = 0, secs = 150;
    static int done = 0;
    if (armed < 0) {
        const char* e = getenv("TM_FORCESTATE");
        armed = e ? 1 : 0;
        if (e) { vt = (uint32_t)strtoul(e, nullptr, 16);
                 const char* c = strchr(e, 44);
                 if (c) secs = (uint32_t)strtoul(c + 1, nullptr, 0); }
    }
    static clock_t t0 = 0;
    if (armed && !t0) t0 = clock();
    const uint32_t el = t0 ? (uint32_t)((clock() - t0) / CLOCKS_PER_SEC) : 0;

    if (armed && !done && g_ui_machine && el >= secs) {
        done = 1;
        { const char* e = getenv("TM_FORCESTATE");
          if (e && strcmp(e, "list") == 0) { tm_list_states(); return false; } }
        const uint32_t st = tm_find_state(vt);
        g_forced_state = st;
        fprintf(stderr, "[forcestate] vtable 0x%08X -> state 0x%08X (machine 0x%08X)\n",
                vt, st, g_ui_machine);
        fflush(stderr);
        if (st) {
            ctx->gpr[3] = g_ui_machine;
            ctx->gpr[4] = st;
            ctx->gpr[5] = 0;
            func_005BA908_lifted(ctx);
            return true;
        }
    }

    /* TM_FORCECALL=<guest address>[,seconds] then invokes that function on the
     * state object -- the campaign select screen stores the chosen character in
     * its own fields, so its "select" handler (0x0048266C) needs nothing but
     * `this` to start the campaign and, with it, the intro cinematic. */
    static int carmed = -1;
    static uint32_t cfn = 0, csecs = 0;
    static int cdone = 0;
    if (carmed < 0) {
        const char* e = getenv("TM_FORCECALL");
        carmed = e ? 1 : 0;
        if (e) { cfn = (uint32_t)strtoul(e, nullptr, 16);
                 const char* c = strchr(e, 44);
                 csecs = c ? (uint32_t)strtoul(c + 1, nullptr, 0) : secs + 10; }
    }
    if (carmed && !cdone && g_forced_state && el >= csecs) {
        cdone = 1;
        fprintf(stderr, "[forcecall] 0x%08X(this=0x%08X)\n", cfn, g_forced_state);
        fflush(stderr);
        ctx->gpr[3] = g_forced_state;
        ctx->gpr[4] = 0;
        ctx->ctr = cfn;
        ps3_indirect_call(ctx);
        return true;
    }
    return false;
}

void func_005BB358(ppu_context* ctx)
{ if (tm_force_state(ctx)) return;
  static int n = 0;
  if (tm_trace_on() && (n++ < 4 || tm_trace_verbose())) tm_trace("UiState::update", func_005BB358_lifted, ctx);
  else func_005BB358_lifted(ctx); }
void func_005CAEA0(ppu_context* ctx) { tm_trace("UiState::setTimer", func_005CAEA0_lifted, ctx); }
/* The state the machine enters straight after UiLegal_4 and leaves again for
 * the main menu. It calls into the 0x006Axxxx MoviePlayer, which is where
 * the intro video lives -- so this is where the movie is meant to start
 * and where it gives up. update() is per frame, so it is capped. */
void func_004AA780_lifted(ppu_context* ctx);
void func_004AAA98_lifted(ppu_context* ctx);
void func_006AD8E8_lifted(ppu_context* ctx);
void func_006ADA38_lifted(ppu_context* ctx);
void func_004B6270_lifted(ppu_context* ctx);
void func_004B6580_lifted(ppu_context* ctx);
/* IntroMovie::onEnter guards the movie on three reads before it will start one:
 *
 *     p = *(u32*)0x00F42288;        if (!p) skip
 *     q = *(u32*)(p + 0x3AB8);      if (!q) skip
 *     b = *(u8 *)0x0120AC25;        if (!b) skip
 *
 * It takes the skip branch and falls straight through to the main menu, so
 * printing all three says which one is zero. */
void func_004AA780(ppu_context* ctx)
{
    const uint32_t p = vm_read32(0x00F42288u);
    const uint32_t q = p ? vm_read32(p + 0x3AB8) : 0;
    const uint32_t b = vm_read8(0x0120AC25u);
    fprintf(stderr, "[intro] guards: [0x00F42288]=0x%08X  +0x3AB8=0x%08X  "
                    "[0x0120AC25]=%u  -> %s\n", p, q, b,
            (p && q && b) ? "plays" : "SKIPPED");
    fflush(stderr);
    tm_trace("IntroMovie::onEnter", func_004AA780_lifted, ctx);
}
void func_004AAA98(ppu_context* ctx)
{ static int n = 0;
  if (tm_trace_on() && (n++ < 6 || tm_trace_verbose())) tm_trace("IntroMovie::update", func_004AAA98_lifted, ctx);
  else func_004AAA98_lifted(ctx); }
void func_006AD8E8(ppu_context* ctx) { tm_trace("MoviePlayer::a", func_006AD8E8_lifted, ctx); }
void func_006ADA38(ppu_context* ctx) { tm_trace("MoviePlayer::b", func_006ADA38_lifted, ctx); }
void func_004AA5D8_lifted(ppu_context* ctx);
/* Called from IntroMovie::update once its timer expires, as (7, 0x00CBECA0).
 * If that pointer is a movie name this is the call that should start one. */
void func_004AA5D8(ppu_context* ctx)
{
    if (tm_trace_on()) {
        char b[128];
        fprintf(stderr, "[trace] introTail(%d, 0x%08X '%s')\n", (int)ctx->gpr[3],
                (uint32_t)ctx->gpr[4], tm_gstr((uint32_t)ctx->gpr[4], b, sizeof b));
        fflush(stderr);
    }
    func_004AA5D8_lifted(ctx);
}
void func_0034F060_lifted(ppu_context* ctx);
/* stringTable::get(table, id). IntroMovie::onEnter asks it for ids 0x400 and
 * 0x401 and hands both to Movie::c, which is what would name the file to play.
 * Both come back empty, so print the id, the returned pointer and its first
 * bytes to see whether the table is unpopulated or the lookup misses. */
void func_0034F060(ppu_context* ctx)
{
    const uint32_t tbl = (uint32_t)ctx->gpr[3], id = (uint32_t)ctx->gpr[4];
    func_0034F060_lifted(ctx);
    if (tm_trace_on()) {
        const uint32_t r = (uint32_t)ctx->gpr[3];
        char sb[64]; char hex[64] = {0};
        for (int i = 0; i < 8 && r; i++) {
            const uint32_t w = vm_read32((r + i) & ~3u);
            snprintf(hex + i * 3, 4, "%02X ",
                     (unsigned)((w >> ((3 - ((r + i) & 3)) * 8)) & 0xFF));
        }
        fprintf(stderr, "[trace] strTable(0x%08X, 0x%X) = 0x%08X '%s' [%s]\n",
                tbl, id, r, tm_gstr(r, sb, sizeof sb), hex);
        fflush(stderr);
    }
}

/* The last call IntroMovie::onEnter makes, with two heap pointers that look
 * like strings -- if this is "play this movie", they name it. */
void func_004B6270(ppu_context* ctx)
{
    if (tm_trace_on()) {
        char a[128], b[128];
        fprintf(stderr, "[trace] Movie::c('%s', '%s') from 0x%08X\n",
                tm_gstr((uint32_t)ctx->gpr[4], a, sizeof a),
                tm_gstr((uint32_t)ctx->gpr[5], b, sizeof b), (uint32_t)ctx->lr - 4);
    }
    tm_trace("Movie::c", func_004B6270_lifted, ctx);
}
void func_004B6580(ppu_context* ctx) { tm_trace("Movie::d", func_004B6580_lifted, ctx); }



void func_006AC648(ppu_context* ctx) { tm_trace("MoviePlayer::openFile", func_006AC648_lifted, ctx); }
/* The chain that would actually play an intro: the ArchiveLoader building the
 * movies path from "%smovies", the attract script, the id->filename table
 * holding c1.avi/ep_1.avi/..., and the one caller of MoviePlayer::openFile.
 * Whichever of these never runs is where the intro is lost. */
void func_0036255C_lifted(ppu_context* ctx);
void func_000120C4_lifted(ppu_context* ctx);
void func_00059F6C_lifted(ppu_context* ctx);
void func_001DB604_lifted(ppu_context* ctx);
void func_0036255C(ppu_context* ctx) { tm_trace("ArchiveLoader::moviesPath", func_0036255C_lifted, ctx); }
void func_000120C4(ppu_context* ctx) { tm_trace("attractScript", func_000120C4_lifted, ctx); }
void func_00059F6C(ppu_context* ctx) { tm_trace("movieNameForId", func_00059F6C_lifted, ctx); }
void func_001DB604(ppu_context* ctx) { tm_trace("playMovieFile", func_001DB604_lifted, ctx); }
void func_00799B5C_lifted(ppu_context* ctx);
void func_00799B5C(ppu_context* ctx) { tm_trace("vid::00799B5C", func_00799B5C_lifted, ctx); }
void func_0079A634_lifted(ppu_context* ctx);
/* The movie video stream. It returns 155 without decoding whenever
 * *(this+0xC0) is zero, so print the object to see what is missing. */
void func_0079A634(ppu_context* ctx)
{
    static int n = 0;
    if (n < 6) {
        const uint32_t o = (uint32_t)ctx->gpr[3];
        fprintf(stderr, "[vid] stream=0x%08X +0xC0=0x%08X +0xC4=0x%08X +0xCC=0x%08X"
                        " +0xB8=0x%08X +0xBC=0x%08X arg=0x%08X\n",
                o, vm_read32(o + 0xC0), vm_read32(o + 0xC4), vm_read32(o + 0xCC),
                vm_read32(o + 0xB8), vm_read32(o + 0xBC), (uint32_t)ctx->gpr[4]);
        fflush(stderr); n++;
    }
    tm_trace("vid::0079A634", func_0079A634_lifted, ctx);
}
void func_00799EF0_lifted(ppu_context* ctx);
void func_00799EF0(ppu_context* ctx) { tm_trace("vid::00799EF0", func_00799EF0_lifted, ctx); }
void func_0079A1D8_lifted(ppu_context* ctx);
void func_0079A1D8(ppu_context* ctx) { tm_trace("vid::0079A1D8", func_0079A1D8_lifted, ctx); }
void func_009B2574_lifted(ppu_context* ctx);
/* The cinema load hangs here. func_00606CE4 calls this at 0x00606ED4 right
 * after naming the bank "shell", so this is the BRB audio bank unload, and it
 * waits on a mixer that runs as a SPURS job whose PM was never lifted. A movie
 * does not need the shell bank torn down, so TM_SKIP_BANKUNLOAD=1 returns
 * success without doing it, to see what the load does next. */
void func_009B2574(ppu_context* ctx)
{
    static int skip = -1;
    if (skip < 0) skip = getenv("TM_SKIP_BANKUNLOAD") ? 1 : 0;
    if (skip) {
        static int n = 0;
        if (n++ < 4) { fprintf(stderr, "[skip] bank unload 0x009B2574(0x%08X)\n",
                               (uint32_t)ctx->gpr[3]); fflush(stderr); }
        ctx->gpr[3] = 0;
        return;
    }
    tm_trace("cw::009B2574", func_009B2574_lifted, ctx);
}
void func_009B2784_lifted(ppu_context* ctx);
void func_009B2784(ppu_context* ctx) { tm_trace("cw::009B2784", func_009B2784_lifted, ctx); }
void func_00606F78_lifted(ppu_context* ctx);
void func_00606F78(ppu_context* ctx) { tm_trace("cw::00606F78", func_00606F78_lifted, ctx); }
void func_00606CE4_lifted(ppu_context* ctx);
void func_00606CE4(ppu_context* ctx) { tm_trace("cw::00606CE4", func_00606CE4_lifted, ctx); }
void func_0036168C_lifted(ppu_context* ctx);
void func_0036168C(ppu_context* ctx) { tm_trace("cw::0036168C", func_0036168C_lifted, ctx); }
void func_00361750_lifted(ppu_context* ctx);
void func_00361750(ppu_context* ctx) { tm_trace("cw::00361750", func_00361750_lifted, ctx); }
void func_005F8FF4_lifted(ppu_context* ctx);
void func_005F8FF4(ppu_context* ctx) { tm_trace("cw::005F8FF4", func_005F8FF4_lifted, ctx); }
void func_00606B6C_lifted(ppu_context* ctx);
void func_00606B6C(ppu_context* ctx) { tm_trace("cw::00606B6C", func_00606B6C_lifted, ctx); }
void func_00607670_lifted(ppu_context* ctx);
void func_00607670(ppu_context* ctx) { tm_trace("cw::00607670", func_00607670_lifted, ctx); }
void func_0060E500_lifted(ppu_context* ctx);
void func_0060E500(ppu_context* ctx) { tm_trace("cw::0060E500", func_0060E500_lifted, ctx); }
void func_00617730_lifted(ppu_context* ctx);
void func_00617730(ppu_context* ctx) { tm_trace("cw::00617730", func_00617730_lifted, ctx); }
void func_007675D0_lifted(ppu_context* ctx);
void func_007675D0(ppu_context* ctx) { tm_trace("cw::007675D0", func_007675D0_lifted, ctx); }
void func_0076B66C_lifted(ppu_context* ctx);
void func_0076B66C(ppu_context* ctx) { tm_trace("cw::0076B66C", func_0076B66C_lifted, ctx); }
void func_004BDA30_lifted(ppu_context* ctx);
void func_004BDA30(ppu_context* ctx) { tm_trace("cw::004BDA30", func_004BDA30_lifted, ctx); }
void func_00681630_lifted(ppu_context* ctx);
void func_00681630(ppu_context* ctx) { tm_trace("cw::00681630", func_00681630_lifted, ctx); }
void func_005A0AA0_lifted(ppu_context* ctx);
void func_005A0AA0(ppu_context* ctx) { tm_trace("cw::005A0AA0", func_005A0AA0_lifted, ctx); }
void func_005A9034_lifted(ppu_context* ctx);
void func_005A9034(ppu_context* ctx) { tm_trace("cw::005A9034", func_005A9034_lifted, ctx); }
void func_005A92A0_lifted(ppu_context* ctx);
void func_005A92A0(ppu_context* ctx) { tm_trace("cw::005A92A0", func_005A92A0_lifted, ctx); }
void func_006A1FC8_lifted(ppu_context* ctx);
void func_006A1FC8(ppu_context* ctx) { tm_trace("cw::006A1FC8", func_006A1FC8_lifted, ctx); }
void func_001111EC_lifted(ppu_context* ctx);
void func_001111EC(ppu_context* ctx) { tm_trace("cw::001111EC", func_001111EC_lifted, ctx); }
void func_003C00FC_lifted(ppu_context* ctx);
void func_003C00FC(ppu_context* ctx) { tm_trace("cw::003C00FC", func_003C00FC_lifted, ctx); }
void func_006107E8_lifted(ppu_context* ctx);
void func_006107E8(ppu_context* ctx) { tm_trace("cw::006107E8", func_006107E8_lifted, ctx); }
void func_005B1D64_lifted(ppu_context* ctx);
void func_005B1D64(ppu_context* ctx) { tm_trace("cw::005B1D64", func_005B1D64_lifted, ctx); }
void func_000CBC3C_lifted(ppu_context* ctx);
void func_000CBC3C(ppu_context* ctx) { tm_trace("cw::000CBC3C", func_000CBC3C_lifted, ctx); }
void func_003499E0_lifted(ppu_context* ctx);
void func_003499E0(ppu_context* ctx) { tm_trace("cw::003499E0", func_003499E0_lifted, ctx); }
void func_003A87C4_lifted(ppu_context* ctx);
/* The cinema world setup hangs here, at the virtual call on *(this+0x144)
 * slot 5. Resolve and print that target so the hang has a guest address. */
void func_003A87C4(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const uint32_t obj  = self ? vm_read32(self + 0x144) : 0;
    const uint32_t vt   = obj ? vm_read32(obj) : 0;
    const uint32_t desc = vt ? vm_read32(vt + 0x14) : 0;
    fprintf(stderr, "[cinema] this=0x%08X *(+0x144)=0x%08X vtable=0x%08X"
                    " slot5=0x%08X -> 0x%08X\n",
            self, obj, vt, desc, desc ? vm_read32(desc) : 0);
    fflush(stderr);
    tm_trace("cin::003A87C4", func_003A87C4_lifted, ctx);
}
void func_003A9050_lifted(ppu_context* ctx);
void func_003A9050(ppu_context* ctx) { tm_trace("cin::003A9050", func_003A9050_lifted, ctx); }
void func_003AA034_lifted(ppu_context* ctx);
void func_003AA034(ppu_context* ctx) { tm_trace("cin::003AA034", func_003AA034_lifted, ctx); }
void func_003BE230_lifted(ppu_context* ctx);
void func_003BE230(ppu_context* ctx) { tm_trace("cin::003BE230", func_003BE230_lifted, ctx); }
void func_003BE810_lifted(ppu_context* ctx);
void func_003BE810(ppu_context* ctx) { tm_trace("cin::003BE810", func_003BE810_lifted, ctx); }
void func_004DDCE0_lifted(ppu_context* ctx);
void func_004DDCE0(ppu_context* ctx) { tm_trace("cin::004DDCE0", func_004DDCE0_lifted, ctx); }
void func_004DDD04_lifted(ppu_context* ctx);
void func_004DDD04(ppu_context* ctx) { tm_trace("cin::004DDD04", func_004DDD04_lifted, ctx); }
void func_004DDFD0_lifted(ppu_context* ctx);
void func_004DDFD0(ppu_context* ctx) { tm_trace("cin::004DDFD0", func_004DDFD0_lifted, ctx); }
void func_00667DF8_lifted(ppu_context* ctx);
void func_00667DF8(ppu_context* ctx) { tm_trace("cin::00667DF8", func_00667DF8_lifted, ctx); }
void func_00669F8C_lifted(ppu_context* ctx);
void func_00669F8C(ppu_context* ctx) { tm_trace("cin::00669F8C", func_00669F8C_lifted, ctx); }
void func_003C24B4_lifted(ppu_context* ctx);
void func_003C24B4(ppu_context* ctx) { tm_trace("setup::003C24B4", func_003C24B4_lifted, ctx); }
void func_003A8A20_lifted(ppu_context* ctx);
void func_003A8A20(ppu_context* ctx) { tm_trace("setup::003A8A20", func_003A8A20_lifted, ctx); }
void func_004DCB4C_lifted(ppu_context* ctx);
void func_004DCB4C(ppu_context* ctx) { tm_trace("setup::004DCB4C", func_004DCB4C_lifted, ctx); }
void func_003A78C8_lifted(ppu_context* ctx);
void func_003A78C8(ppu_context* ctx) { tm_trace("setup::003A78C8", func_003A78C8_lifted, ctx); }
void func_003622B0_lifted(ppu_context* ctx);
void func_003622B0(ppu_context* ctx) { tm_trace("setup::003622B0", func_003622B0_lifted, ctx); }
void func_0005A028_lifted(ppu_context* ctx);
void func_0005A028(ppu_context* ctx) { tm_trace("avi::0005A028", func_0005A028_lifted, ctx); }
void func_0005A82C_lifted(ppu_context* ctx);
void func_0005A82C(ppu_context* ctx) { tm_trace("avi::0005A82C", func_0005A82C_lifted, ctx); }
void func_003D40E0_lifted(ppu_context* ctx);
void func_003D40E0(ppu_context* ctx) { tm_trace("avi::003D40E0", func_003D40E0_lifted, ctx); }
void func_003D7BE0_lifted(ppu_context* ctx);
void func_003D7BE0(ppu_context* ctx) { tm_trace("avi::003D7BE0", func_003D7BE0_lifted, ctx); }
void func_003F1AF0_lifted(ppu_context* ctx);
void func_003F1AF0(ppu_context* ctx) { tm_trace("avi::003F1AF0", func_003F1AF0_lifted, ctx); }
void func_003F33D0_lifted(ppu_context* ctx);
void func_003F33D0(ppu_context* ctx) { tm_trace("avi::003F33D0", func_003F33D0_lifted, ctx); }
void func_0066DD48_lifted(ppu_context* ctx);
void func_0066DD48(ppu_context* ctx) { tm_trace("avi::0066DD48", func_0066DD48_lifted, ctx); }
void func_002997B8_lifted(ppu_context* ctx);
void func_002997B8(ppu_context* ctx) { tm_trace("avi::002997B8", func_002997B8_lifted, ctx); }
void func_0056EF3C_lifted(ppu_context* ctx);
void func_0056EF3C(ppu_context* ctx) { tm_trace("avi::0056EF3C", func_0056EF3C_lifted, ctx); }
void func_0056F7B0_lifted(ppu_context* ctx);
void func_0056F7B0(ppu_context* ctx) { tm_trace("avi::0056F7B0", func_0056F7B0_lifted, ctx); }
void func_00692924_lifted(ppu_context* ctx);
void func_00692924(ppu_context* ctx) { tm_trace("avi::00692924", func_00692924_lifted, ctx); }
void func_00693600_lifted(ppu_context* ctx);
void func_00693600(ppu_context* ctx) { tm_trace("avi::00693600", func_00693600_lifted, ctx); }
void func_00693AD0_lifted(ppu_context* ctx);
void func_00693AD0(ppu_context* ctx) { tm_trace("avi::00693AD0", func_00693AD0_lifted, ctx); }
void func_00081D88_lifted(ppu_context* ctx);
void func_00081D88(ppu_context* ctx) { tm_trace("avi::00081D88", func_00081D88_lifted, ctx); }
void func_001DB6B8_lifted(ppu_context* ctx);
void func_001DB6B8(ppu_context* ctx) { tm_trace("avi::001DB6B8", func_001DB6B8_lifted, ctx); }
void func_001DB4FC_lifted(ppu_context* ctx);
/* The constructor that installs vtable 0x00CE78E0, whose slot 0 is
 * playMovieFile. playMovieFile has no static callers -- it is only ever
 * reached through this vtable -- so whether this ctor runs says whether the
 * object that can play a movie is ever built at all. */
void func_001DB4FC(ppu_context* ctx) { tm_trace("movieObj::ctor", func_001DB4FC_lifted, ctx); }

void func_0014BCA8_lifted(ppu_context* ctx);
void func_0020C698_lifted(ppu_context* ctx);
void func_0014BCA8(ppu_context* ctx) { tm_trace("boot::seq", func_0014BCA8_lifted, ctx); }
void func_0020C698(ppu_context* ctx) { tm_trace("WorldLoader::setup", func_0020C698_lifted, ctx); }
void func_00360B90_lifted(ppu_context* ctx);
void func_00360C3C_lifted(ppu_context* ctx);
void func_00360E84_lifted(ppu_context* ctx);
void func_00360EBC_lifted(ppu_context* ctx);
void func_00360F68_lifted(ppu_context* ctx);
void func_00360F88_lifted(ppu_context* ctx);
void func_0064BA08_lifted(ppu_context* ctx);
void func_0036204C_lifted(ppu_context* ctx);
void func_00360B90(ppu_context* ctx) { tm_trace("AL::b90", func_00360B90_lifted, ctx); }
/* ArchiveLoader's open-and-read helper. r4 is the path it is after, and
 * naming the one call that returns 0 is the whole point of watching it. */
void func_00360C3C(ppu_context* ctx)
{
    char nm[192];
    if (tm_trace_on()) {
        const uint32_t a3 = (uint32_t)ctx->gpr[3];
        fprintf(stderr, "[trace] AL::c3c path=%s  arg0=0x%08X [",
                tm_gstr((uint32_t)ctx->gpr[4], nm, sizeof nm), a3);
        for (uint32_t o = 0; o < 0x20; o += 4) fprintf(stderr, " %08X", vm_read32(a3 + o));
        fprintf(stderr, " ]\n");
        /* Is the contents.dat we inflated still where the title parses it? */
        if (g_edge_dst) {
            fprintf(stderr, "[trace]   contents.dat @0x%08X:", g_edge_dst);
            for (uint32_t o = 0; o < 0x30; o += 4) fprintf(stderr, " %08X", vm_read32(g_edge_dst + o));
            fprintf(stderr, "\n");
        }
    }
    tm_trace("AL::c3c", func_00360C3C_lifted, ctx);
}
void func_00360E84(ppu_context* ctx) { tm_trace("AL::e84", func_00360E84_lifted, ctx); }
void func_00360EBC(ppu_context* ctx) { tm_trace("AL::ebc", func_00360EBC_lifted, ctx); }
void func_00360F68(ppu_context* ctx) { tm_trace("AL::f68", func_00360F68_lifted, ctx); }
void func_00360F88(ppu_context* ctx) { tm_trace("AL::f88", func_00360F88_lifted, ctx); }
void func_0064BA08(ppu_context* ctx)
{ if (tm_trace_verbose()) tm_trace("updateLoadBar::frame", func_0064BA08_lifted, ctx); else func_0064BA08_lifted(ctx); }

/* The two writers of the load-complete byte at 0x0190FC89. The boot sequence
 * and the loading-screen thread both spin until one of them runs, so knowing
 * whether either is ever reached separates "the load is unfinished" from "the
 * load finished and nobody said so". */
void func_0064C410_lifted(ppu_context* ctx);
void func_0010E57C_lifted(ppu_context* ctx);
void func_0064C410(ppu_context* ctx) { tm_trace("loadBar::finish", func_0064C410_lifted, ctx); }
void func_0010E57C(ppu_context* ctx) { tm_trace("setLoadDone", func_0010E57C_lifted, ctx); }
void func_0036204C(ppu_context* ctx) { tm_trace("AL::204c", func_0036204C_lifted, ctx); }



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
#define CELL_NET_CTL_STATE_IPObtained   3

/* Reporting Disconnected truthfully is not enough: the title's network-init
 * loop at 0x00282730 leaves only when the state reads IPObtained. Neither a
 * Disconnected state nor an error return breaks it -- both land in the same
 * retry, which is where the boot sat logging "Initializing network hardware,
 * N unsuccessful attempts" forever, making the legal screens the last thing
 * it ever showed. Report IPObtained so the loop completes and the boot goes
 * on; TM_NET_STATE overrides it for testing the other paths. */
static void cellNetCtlGetState(ppu_context* ctx)
{
    const uint32_t state = (uint32_t)ctx->gpr[3];
    if (!state) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x80130102; return; }
    static int want = -1;
    if (want < 0) { const char* e = getenv("TM_NET_STATE");
                    want = e ? atoi(e) : CELL_NET_CTL_STATE_IPObtained; }
    vm_write32(state, (uint32_t)want);
    static long long n = 0;
    if (n++ < 3) fprintf(stderr, "[cellNetCtl] GetState() -> OK, state=%d\n", want);
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


/* Probe: cellGcmGetFlipStatus (0x72A577CE).
 *
 * The renderer's frame wait (func_006755D0) polls this up to 25,000 times with
 * a 40 us sleep between -- a second per frame if the flip never completes --
 * and then gives up and draws anyway. Counting the polls per completion says
 * whether flips are arriving at 60 Hz or not at all, which is the difference
 * between a slow port and a renderer that never gets a frame out. */
extern "C" uint32_t cellGcmGetFlipStatus(void);

static void probe_cellGcmGetFlipStatus(ppu_context* ctx)
{
    const uint32_t st = cellGcmGetFlipStatus();
    ctx->gpr[3] = (uint64_t)st;

    static long long polls = 0, waits = 0, done = 0, worst = 0, run = 0;
    polls++;
    if (st == 0) { done++; if (run > worst) worst = run; run = 0; }
    else         { waits++; run++; }
    if ((polls % 20000) == 0)
        fprintf(stderr, "[flip] %lld polls: %lld DONE, %lld WAITING, "
                        "longest wait run %lld\n", polls, done, waits, worst);
}

static void probe_cellSpursSendSignal(ppu_context* ctx)
{
    const uint32_t ts = (uint32_t)ctx->gpr[3], tid = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[cellSpurs] SendSignal(taskset=0x%08X, task=%u)\n", ts, tid);
    ctx->gpr[3] = (uint64_t)(int64_t)_cellSpursSendSignal((void*)(uintptr_t)ts, tid);
}

/* ---------------------------------------------------------------------------
 * TM_FBDUMP=<seconds> — write the guest display buffer to a BMP.
 *
 * This title does not present by drawing into the backbuffer. It renders its
 * scene into a 1280x704 surface and then composites with the RSX 2D engine:
 *
 *   [NV3089] 676x448 src=0xC1190000 -> dst=0xC0010000 at 22,16
 *
 * dst 0xC0010000 is local-memory offset 0x10000 — display buffer 0, the one
 * cellGcmSetDisplayBuffer registered — so the finished, letterboxed frame is
 * assembled in GUEST MEMORY. The D3D12 backend presents its own backbuffer and
 * never reads that, which is why the window stays dark however well the draws
 * translate. Dumping it is the direct way to see what the title actually
 * produced.
 * ------------------------------------------------------------------------- */
extern "C" uint8_t* vm_base;

static void tm_write_bmp(const char* path, const uint8_t* argb, uint32_t w, uint32_t h,
                         uint32_t pitch)
{
    const uint32_t rowsz = ((w * 3 + 3) / 4) * 4;
    const uint32_t dataz = rowsz * h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t*)(hdr + 2) = 54 + dataz;
    *(uint32_t*)(hdr + 10) = 54;
    *(uint32_t*)(hdr + 14) = 40;
    *(int32_t*)(hdr + 18) = (int32_t)w;
    *(int32_t*)(hdr + 22) = (int32_t)h;         /* positive: bottom-up */
    *(uint16_t*)(hdr + 26) = 1;
    *(uint16_t*)(hdr + 28) = 24;
    *(uint32_t*)(hdr + 34) = dataz;

    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t* row = (uint8_t*)calloc(1, rowsz);
    for (int y = (int)h - 1; y >= 0; y--) {
        const uint8_t* src = argb + (size_t)y * pitch;
        for (uint32_t x = 0; x < w; x++) {
            /* Guest stores big-endian A8R8G8B8: bytes are A,R,G,B. */
            row[x * 3 + 0] = src[x * 4 + 3];   /* B */
            row[x * 3 + 1] = src[x * 4 + 2];   /* G */
            row[x * 3 + 2] = src[x * 4 + 1];   /* R */
        }
        fwrite(row, 1, rowsz, f);
    }
    free(row);
    fclose(f);
}

extern "C" void tm_fbdump_tick(void)
{
    static int secs = -1;
    if (secs < 0) { const char* e = getenv("TM_FBDUMP"); secs = e ? atoi(e) : 0; }
    if (!secs || !vm_base) return;

    static unsigned long long t0 = 0, next = 0;
    const unsigned long long now = (unsigned long long)time(NULL);
    if (!t0) { t0 = now; next = now + (unsigned)secs; return; }
    if (now < next) return;
    next = now + (unsigned)secs;

    /* Local memory base 0xC0000000; display buffer 0 at offset 0x10000,
     * 720x480 with the 5120-byte pitch cellGcmSetDisplayBuffer reported. */
    /* TM_TEXDUMP=<offset>[,w,h] aims the dump at any local-memory surface
     * -- the bound texture, say -- instead of the display buffer. */
    uint32_t off = 0x00010000u, w = 720, h = 480, pitch = 5120;
    if (const char* t = getenv("TM_TEXDUMP")) {
        off = (uint32_t)strtoul(t, 0, 0);
        if (const char* c = strchr(t, ',')) {
            w = (uint32_t)strtoul(c + 1, 0, 0);
            const char* c2 = strchr(c + 1, ',');
            h = c2 ? (uint32_t)strtoul(c2 + 1, 0, 0) : w;
            pitch = w * 4;
        }
    }

    static int n = 0;
    char path[64];
    snprintf(path, sizeof path, "fb_%03d.bmp", n++);
    tm_write_bmp(path, vm_base + 0xC0000000u + off, w, h, pitch);
    fprintf(stderr, "[fbdump] wrote %s from local memory 0x%08X %ux%u\n",
            path, off, w, h);
}

/* TM_EF_KICK=<secs> — diagnostic: set SPURS event flag bit 0 after N seconds.
 *
 * A PPU thread blocks in cellSpursEventFlagWait on flag 0x400EDA00 pattern
 * 0x0001 while the Edge Zlib task idles in WAIT_SIGNAL. If that wait is the
 * decompressor announcing readiness, satisfying it should let the loader go on
 * and submit its first job — which is the question this answers. It is a probe,
 * not a fix: nothing here decompresses anything. */
extern "C" int32_t cellSpursEventFlagSet(void* eventFlag, uint16_t bits);

/* TM_LOADDONE=<secs> -- probe: set the load-complete byte at 0x0190FC89.
 *
 * The boot sequence sits in updateLoadBar (func_0064C23C) until that byte goes
 * non-zero, and while it does, the only thing rendering is the load bar -- so
 * the front end can run its legal-screen state machine (it does, all the way to
 * UiLegal_4) without a single vertex reaching the backend. Its two writers,
 * func_0064C410 and func_0010E57C, are never reached. Setting it by hand says
 * whether that byte is really the gate or just a symptom. A probe, not a fix.
 */
/* TM_FIFOWATCH=1 -- print the RSX control register once a second.
 *
 * put/get/ref is the whole question when nothing draws: if `put` is not moving,
 * the title is not submitting commands and the backend is blameless; if `put`
 * runs ahead of `get`, the drain is behind. The register lives at a fixed guest
 * EA (ps3recomp's VM_HLE_INJECT_BASE + 0x2000) so the recompiled code can reach
 * it through vm_base, which means we can read it the same way. */
#define GCM_CONTROL_EA 0x20002000u

/* TM_MEMDUMP=<hex guest address>[,<count>] prints that many bytes once a
 * second. The menu draws sample textures at main-memory addresses such as
 * 0x0AB55580; if those bytes are zero the geometry is fine and the artwork
 * simply never arrived, which is a different bug from the renderer dropping
 * the draws. */
extern "C" void tm_memdump_tick(void)
{
    static int on = -1;
    static uint32_t addr = 0, count = 32;
    if (on < 0) {
        const char* e = getenv("TM_MEMDUMP");
        on = e ? 1 : 0;
        if (e) { addr = (uint32_t)strtoul(e, nullptr, 16);
                 const char* c = strchr(e, 44);
                 if (c) count = (uint32_t)strtoul(c + 1, nullptr, 0); }
    }
    if (!on || !addr) return;
    static int tick = 0;
    if ((tick++ % 60) != 0) return;
    uint32_t nz = 0;
    fprintf(stderr, "[memdump] 0x%08X:", addr);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t w = vm_read32((addr + i) & ~3u);
        const uint32_t b = (w >> ((3 - ((addr + i) & 3)) * 8)) & 0xFF;
        if (b) nz++;
        if (i < 32) fprintf(stderr, " %02X", (unsigned)b);
    }
    fprintf(stderr, "   (%u/%u non-zero)\n", nz, count);
    fflush(stderr);
}

extern "C" void tm_fifowatch_tick(void)
{
    static int on = -1;
    if (on < 0) on = getenv("TM_FIFOWATCH") ? 1 : 0;
    if (!on || !vm_base) return;
    static unsigned long long next = 0;
    const unsigned long long now = (unsigned long long)time(NULL);
    if (now < next) return;
    next = now + 1;
    const uint32_t put = vm_read32(GCM_CONTROL_EA + 0);
    const uint32_t get = vm_read32(GCM_CONTROL_EA + 4);
    const uint32_t ref = vm_read32(GCM_CONTROL_EA + 8);
    fprintf(stderr, "[fifo] put=0x%08X get=0x%08X ref=0x%08X", put, get, ref);

    /* When get stops moving while put runs ahead, the drain has stalled on a
     * command it will not step over. Print it: this title maps every IO offset
     * at a constant +0x11000000 (cellGcmInit ioAddr, and both MapEaIoAddress
     * calls agree), so the word at `get` is directly readable. */
    static uint32_t last_get = 0xFFFFFFFFu;
    if (get == last_get && get != put) {
        const uint32_t ea = get + 0x11000000u;
        fprintf(stderr, "  STALLED, words at get:");
        for (int i = 0; i < 6; i++) fprintf(stderr, " %08X", vm_read32(ea + i * 4));
    }
    last_get = get;
    /* The title syncs on RSX labels (cellGcmGetLabelAddress). ps3recomp puts
     * them at a fixed guest base, 16 bytes apart, so the ones the FIFO's
     * SEMAPHORE_RELEASE writes are readable here. If these never move, the
     * title's wait can never succeed however the FIFO behaves. */
    fprintf(stderr, "  labels:");
    for (int i = 0; i < 4; i++) fprintf(stderr, " %u", vm_read32(0x20000000u + i * 0x10u));
    fprintf(stderr, " [64]=%u", vm_read32(0x20000000u + 64 * 0x10u));
    fprintf(stderr, "\n");
    fflush(stderr);
}

extern "C" void tm_loaddone_tick(void)
{
    static int secs = -1;
    if (secs < 0) { const char* e = getenv("TM_LOADDONE"); secs = e ? atoi(e) : 0; }
    if (!secs || !vm_base) return;
    static unsigned long long t0 = 0;
    const unsigned long long now = (unsigned long long)time(NULL);
    if (!t0) { t0 = now; return; }
    if (now - t0 < (unsigned)secs) return;
    static int done = 0;
    /* Keep writing it. The cinema load path re-clears this byte, so a
     * one-shot poke released the spin once and it stalled again. */
    const int first = !done;
    done = 1;
    vm_write8(0x0190FC89u, 1);
    if (first) fprintf(stderr, "[probe] set load-complete byte 0x0190FC89 = 1 after %d s\n", secs);
    fflush(stderr);
}

extern "C" void tm_ef_kick_tick(void)
{
    static int secs = -1;
    if (secs < 0) { const char* e = getenv("TM_EF_KICK"); secs = e ? atoi(e) : 0; }
    if (!secs) return;
    static unsigned long long t0 = 0;
    const unsigned long long now = (unsigned long long)time(NULL);
    if (!t0) { t0 = now; return; }
    if (now - t0 < (unsigned)secs) return;
    static int done = 0;
    if (done) return;
    done = 1;
    const uint32_t flag = 0x400EDA00u;
    /* A PPU-side Set on an SPU2PPU flag is recorded as "owed bits" for the SPU
     * to collect rather than applied, so events stays 0 and the waiter keeps
     * blocking. The waiter polls the guest events word directly, so write it. */
    vm_write16(flag + 0x00 /* EF_EVENTS */, 0x0001);
    fprintf(stderr, "[TM_EF_KICK] wrote events=0x0001 to flag 0x%08X\n", flag);
}

/* Probe: cellSpursEventFlagWait (0x373523D4). Logs the guest return address so
 * the waiting function can be identified, then forwards to the runtime. */
extern "C" int32_t cellSpursEventFlagWait(void* eventFlag, void* bits, uint32_t mode);

static void probe_EventFlagWait(ppu_context* ctx)
{
    const uint32_t ea = (uint32_t)ctx->gpr[3], b = (uint32_t)ctx->gpr[4];
    const uint32_t mode = (uint32_t)ctx->gpr[5];
    static int n = 0;
    if (n++ < 6)
        fprintf(stderr, "[probe] EventFlagWait(flag=0x%08X, bits=0x%08X, mode=%u) from guest 0x%08X\n",
                ea, b, mode, (uint32_t)ctx->lr - 4);
    ctx->gpr[3] = (uint64_t)(int64_t)cellSpursEventFlagWait(
        (void*)(uintptr_t)ea, (void*)(uintptr_t)b, mode);
}

/* ---------------------------------------------------------------------------
 * The Edge decompressor's completion wait, guest 0x0099790C.
 *
 * Decoded from the lifted code: wait(this, request, ...) computes its event-flag
 * bit as `1 << ((request - (this + 0x2C04)) / sizeof(request))`, then loops
 * cellSpursEventFlagWait while `*(u32*)(request + 0x28)` is non-zero. So
 * `request` is a submitted decompression request and `+0x28` is its busy flag,
 * which the SPU task clears on completion.
 *
 * That means work IS queued and the wait is legitimate — the SPU side simply
 * never runs it. Dumping the request is the way to learn its layout, which is
 * what an HLE inflate would need. TM_REQDUMP=1.
 * ------------------------------------------------------------------------- */
#define REQ_DST     0x00
#define REQ_SRC     0x0C
#define REQ_SRCLEN  0x10
#define REQ_DSTLEN  0x18
#define REQ_BUF     0x14
#define REQ_DSTLEN2 0x24
#define REQ_BUSY    0x28

extern "C" int tm_inflate(uint8_t* out, uint32_t out_cap, const uint8_t* src, uint32_t src_len);

/* The lifted Edge SPU task parks in WAIT_SIGNAL the instant it starts and
 * nothing in the runtime ever wakes it -- the title never calls
 * _cellSpursSendSignal, so whatever it does kick the task with, we do not model
 * it. This is the one point where the title is provably blocked on a queued
 * request, so kick the task here and give it a moment to answer. If it does
 * not, fall back to inflating on the host. */
extern "C" void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
extern "C" int tm_inflate_selftest(void);

void func_0099790C_lifted(ppu_context* ctx);

void func_0099790C(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const uint32_t req  = (uint32_t)ctx->gpr[4];

    static int on = -1;
    if (on < 0) { const char* e = getenv("TM_EDGE_HLE"); on = e ? atoi(e) : 1; }

    { static long long dumped = 0;
      if (getenv("TM_REQDUMP") && req && dumped++ < 4) {
          fprintf(stderr, "[edge] request 0x%08X raw:' + B + 'n", req);
          for (uint32_t o = 0; o < 0x40; o += 0x10)
              fprintf(stderr, "    +%02X: %08X %08X %08X %08X' + B + 'n", o,
                      vm_read32(req + o), vm_read32(req + o + 4),
                      vm_read32(req + o + 8), vm_read32(req + o + 12));
      } }

    { static long long calls = 0;
      if (getenv("TM_REQDUMP") && calls++ < 40)
          fprintf(stderr, "[edge] enter #%lld req=0x%08X busy=%u dst=0x%08X src=0x%08X "
                          "srclen=%u dstlen=%u/%u\n", calls, req,
                  req ? vm_read32(req + REQ_BUSY) : 0,
                  req ? vm_read32(req + REQ_DST) : 0,
                  req ? vm_read32(req + REQ_SRC) : 0,
                  req ? vm_read32(req + REQ_SRCLEN) : 0,
                  req ? vm_read32(req + REQ_DSTLEN) : 0,
                  req ? vm_read32(req + REQ_DSTLEN2) : 0); }

    if (req && getenv("TM_SPU_KICK")) {
        const char* ms = getenv("TM_SPU_KICK_MS");
        const int spins = (ms ? atoi(ms) : 200) / 5;
        /* Signalling a taskset with nothing parked is a no-op, so poke them all
         * rather than guess which one owns the Edge task. */
        for (int i = 0; i < g_ntasksets; i++)
            for (uint32_t t = 0; t < 4; t++)
                spu_taskset_signal_task(g_tasksets[i], t);
        for (int i = 0; i < spins && vm_read32(req + REQ_BUSY); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!vm_read32(req + REQ_BUSY)) {
            static int said = 0;
            if (said++ < 4) fprintf(stderr, "[edge] SPU task serviced request 0x%08X\n", req);
            ctx->gpr[3] = 0;
            return;
        }
    }

    if (on && req) {
        const uint32_t dst    = vm_read32(req + REQ_DST);
        const uint32_t src    = vm_read32(req + REQ_SRC);
        const uint32_t srclen = vm_read32(req + REQ_SRCLEN);
        uint32_t dstlen       = vm_read32(req + REQ_DSTLEN);
        if (!dstlen) dstlen   = vm_read32(req + REQ_DSTLEN2);

        /* Edge decompresses IN PLACE: the request's +0x14 buffer holds the
         * compressed file (the +0x0C source points 0x45 bytes into it) and the
         * title parses the inflated result back out of that same buffer, not
         * out of +0x00. Writing to +0x00 left it parsing the still-compressed
         * bytes, which is why every member name came out empty or garbage.
         * Inflate to the host first so the source survives being overwritten. */
        const uint32_t inplace = vm_read32(req + REQ_BUF);
        if (src && srclen && dstlen && vm_base && (inplace || dst)) {
            /* Attribute the load time: how much of the wall clock between two
             * requests is actually spent inflating, and how much is the title
             * getting back here. A full archive is ~2900 blocks, so a hundred
             * milliseconds of round trip per block is ten minutes of loading. */
            using clk = std::chrono::steady_clock;
            static clk::time_point last;
            static double gap_ms = 0, work_ms = 0;
            const clk::time_point t0 = clk::now();
            if (last.time_since_epoch().count())
                gap_ms += std::chrono::duration<double, std::milli>(t0 - last).count();

            uint8_t* tmp = (uint8_t*)malloc(dstlen);
            int n = tmp ? tm_inflate(tmp, dstlen, vm_base + src, srclen) : -1;
            work_ms += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            last = clk::now();
            { static long long m = 0;
              if (++m % 512 == 0)
                  fprintf(stderr, "[edge] %lld blocks: %.1f ms/block round trip, "
                                  "%.2f ms/block inflating (%.0f%%)\n",
                          m, gap_ms / m, work_ms / m, 100.0 * work_ms / (gap_ms ? gap_ms : 1)); }
            if (n >= 0) {
                if (inplace) memcpy(vm_base + inplace, tmp, (size_t)n);
                else         memcpy(vm_base + dst,     tmp, (size_t)n);
            }
            free(tmp);
            static long long done = 0;
            if (n >= 0) {
                g_edge_dst = inplace ? inplace : dst;
                vm_write32(req + REQ_BUSY, 0);   /* the wait loop polls this */
                g_edge_bytes += (uint64_t)n;
                /* Track where the archive lands in RSX local memory. The UI
                 * samples a texture at local offset 0x2ACD800; knowing the
                 * inflated range says whether the loader ever writes there. */
                { const uint32_t d = inplace ? inplace : dst;
                  static uint32_t lo = 0xFFFFFFFFu, hi = 0;
                  static long long nloc = 0;
                  /* Same for main memory: the menu samples textures at
                   * 0x0AB55580 and friends, and those bytes read back as all
                   * zero, so record whether the archive writes anywhere near
                   * them. */
                  if (d < 0xC0000000u) {
                      static uint32_t mlo = 0xFFFFFFFFu, mhi = 0;
                      static long long nmain = 0;
                      if (d < mlo) mlo = d;
                      if (d + (uint32_t)n > mhi) mhi = d + (uint32_t)n;
                      if ((++nmain % 256) == 0)
                          fprintf(stderr, "[edge] main-memory writes: %lld blocks, "
                                          "0x%08X..0x%08X\n", nmain, mlo, mhi);
                  }
                  if (d >= 0xC0000000u) {
                      /* The UI texture sits 0x40000 below the lowest address
                       * the archive has been seen to write, so print the first
                       * destinations: if the stream is meant to start there and
                       * the opening blocks go missing, this shows it. */
                      if (nloc < 8)
                          fprintf(stderr, "[edge] local write #%lld -> 0x%08X + %d\n",
                                  nloc, d, n);
                      if (d < lo) lo = d;
                      if (d + (uint32_t)n > hi) hi = d + (uint32_t)n;
                      if ((++nloc % 256) == 0)
                          fprintf(stderr, "[edge] local-memory writes: %lld blocks, "
                                          "0x%08X..0x%08X\n", nloc, lo, hi);
                  } }
                /* A full archive load is thousands of 64K blocks; report
                 * progress periodically rather than capping and going quiet. */
                if (done++ < 4 || (done % 512) == 0)
                    fprintf(stderr, "[edge] inflated request 0x%08X: %u -> %d bytes "
                                    "(src 0x%08X -> dst 0x%08X) [#%lld, %llu MB total]\n",
                            req, srclen, n, src, inplace ? inplace : dst,
                            done, (unsigned long long)(g_edge_bytes >> 20));
                /* Fall through to the lifted body rather than returning: with the
                 * busy flag already clear it skips the event-flag wait but still
                 * runs the post-processing after it, which the caller depends on. */
                func_0099790C_lifted(ctx);
                return;
            }
            static int warned = 0;
            if (warned++ < 4)
                fprintf(stderr, "[edge] inflate FAILED for request 0x%08X "
                                "(src 0x%08X len %u -> dst 0x%08X cap %u); waiting instead\n",
                        req, src, srclen, dst, dstlen);
        }
    }
    (void)self;
    func_0099790C_lifted(ctx);
}
