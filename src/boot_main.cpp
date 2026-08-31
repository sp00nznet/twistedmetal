/*
 * ps3recomp - integrated PPU boot harness (first-boot attempt).
 *
 * Links the whole PPU runtime half into one executable and starts executing
 * the recompiled game's entry point:
 *
 *   lifted code (ppu_recomp.c) + loader (ppu_loader.cpp) + HLE bridge
 *   (ppu_hle.cpp + generated NID table) + HLE libs (cellGcmSys, rsx_commands)
 *
 * It loads the real EBOOT image, registers the lifted functions and the HLE
 * NID handlers, then dispatches the entry. Execution runs real game boot
 * code until it reaches a function outside the lifted subset (logged by the
 * unlifted stub), an unimplemented firmware import (logged by ps3_hle_call),
 * or an lv2 syscall (logged by lv2_syscall) -- telling us exactly what to
 * implement next.
 *
 * This proves the integration builds + runs; a full-image build additionally
 * needs the lifter to split output into multiple TUs (88 MB single-file
 * otherwise).
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
void     tm_hle_register_extra(void);   /* src/hle_extra.cpp */
void     tm_init_title_metadata(void);  /* src/hle_extra.cpp */
void     tm_fbdump_tick(void);          /* src/hle_extra.cpp */
void     tm_ef_kick_tick(void);         /* src/hle_extra.cpp */
void     tm_loaddone_tick(void);        /* src/hle_extra.cpp */
void     tm_fifowatch_tick(void);       /* src/hle_extra.cpp */
void     tm_memdump_tick(void);         /* src/hle_extra.cpp */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;   /* host dir that PS3 mount points map into */
/* Optional hook: load real system PRX modules (libsre = cellSpurs/cellSync) into
 * guest RAM and register their exports. Weak default is a no-op; a title that
 * links a lifted PRX defines a strong version. Called after the lifted function
 * table is registered and vm_base is live, before the game runs. */
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
}

#include <string.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
/* Last-chance crash reporter: vm_base accesses are bounds-guarded, so a real
 * access violation means a HOST pointer deref (e.g. a bad function pointer or a
 * runtime-struct walk). Print the faulting address and the RIP as a module
 * offset (RVA) so it can be symbolized with llvm-symbolizer against the PDB. */
extern "C" uint32_t    g_last_hle_nid;    /* ppu_hle.cpp breadcrumb */
extern "C" const char* g_last_hle_name;

extern "C" __declspec(thread) ppu_context* g_active_ctx;
static LONG WINAPI tm_crash_filter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[CRASH] code=0x%08lX rip=%p\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    fprintf(stderr, "[CRASH] last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "[CRASH] %s fault address 0x%llX\n",
                er->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)er->ExceptionInformation[1]);
    if (g_active_ctx) fprintf(stderr, "[CRASH] guest ctr=0x%08X lr=0x%08X r3=0x%08X\n",
          (uint32_t)g_active_ctx->ctr, (uint32_t)g_active_ctx->lr, (uint32_t)g_active_ctx->gpr[3]);
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=twistedmetal.exe 0x%llX)\n",
            (void*)mod, (unsigned long long)((char*)er->ExceptionAddress - (char*)mod),
            (unsigned long long)((char*)er->ExceptionAddress - (char*)mod));
    /* Host call stack (RVAs) so the lifted caller can be symbolized. */
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == mod)
            fprintf(stderr, "[CRASH]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifdef _WIN32
/* abort()/exit(3) reporter: the recompiled CRT (or a failed invariant) can call
 * abort() — Windows turns that into exit code 3 with no message. Capture a host
 * backtrace (RVAs) + the last HLE NID so the aborting caller can be symbolized. */
static void tm_abort_handler(int)
{
    fprintf(stderr, "\n[ABORT] SIGABRT raised; last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    void* frames[32];
    USHORT n = RtlCaptureStackBackTrace(0, 32, frames, NULL);
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&tm_abort_handler, &self);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == self)
            fprintf(stderr, "[ABORT]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    _exit(3);
}
#endif

/* Derive the VFS root (the dir containing PS3_GAME) from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf  -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];
static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }
    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip three trailing components: EBOOT.elf / USRDIR / PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
extern "C" uint32_t ppu_vm_size;   /* defined in ppu_loader.cpp (OOB guard) */
extern "C" void lv2_init_syscalls(void);   /* runtime/syscalls/lv2_register.c */

/* Guest-callback dispatch + RSX vblank/flip driver.
 *
 * g_ps3_guest_caller (defined NULL by libs/system/cellSysutil.c) is the hook the
 * HLE runtime uses to call back into recompiled code -- cellSysutil events and
 * the GCM vblank/flip handlers. ppu_guest_call (ppu_loader.cpp) does the OPD ->
 * dispatch. On real hardware the RSX fires a vblank interrupt ~60x/s that drives
 * the game's frame loop; with no RSX we synthesize it from a host timer thread
 * calling cellGcmTickVBlank()/TickFlip(), which invoke the registered handlers.
 * Without this the game inits, registers its handlers, and then waits forever
 * for a vblank that never comes. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;        /* libs/system/cellSysutil.c */
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);
/* Mark a vblank+flip tick pending WITHOUT running guest code -- the handlers are
 * delivered on the main guest thread (ppu_gcm_pump at HLE boundaries), serialized
 * with guest execution so the ticker thread never races it. */
extern "C" void cellGcm_request_tick(void);

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{ ppu_guest_call(opd, a0, a1, a2, a3); }

#ifdef _WIN32
/* RSX present backend (libs/video/rsx_d3d12_backend.c). Driven on the vblank
 * thread so the D3D12 device + window message pump live on one thread. */
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */

/* Live NV4097->D3D12 engine (libs/video/rsx_live_draw.c, from caner /
 * canersaka's Yakuza: Dead Souls port). Selected with RSX_LIVE_DRAW=1. It
 * binds a swap chain to a window the null backend opens and then owns
 * presentation, so the generic rsx_d3d12_backend is left out of the picture
 * entirely rather than run alongside it. */
extern "C" int   rsx_live_draw_enabled(void);
extern "C" int   rsx_live_draw_init(void* hwnd, uint32_t w, uint32_t h,
                                    const uint8_t* (*guest_ptr)(void*, uint32_t,
                                                                uint32_t, uint32_t),
                                    void* user);
extern "C" int   rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" int   rsx_null_backend_pump_messages(void);
extern "C" void* rsx_null_backend_get_hwnd(void);
extern "C" void  rsx_null_backend_suppress_present(int on);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t rsx_live_draw_get_frames(void);
extern "C" uint32_t rsx_live_draw_get_last_draws(void);
extern "C" void     rsx_live_draw_flush(void);
extern "C" void     rsx_live_draw_present(uint32_t buffer_id);

/* Resolve (location, offset) to host memory for the engine. location 0 is RSX
 * local VRAM, 1 is main/IO memory; the engine promises its callers the whole
 * min_bytes span is readable, so validate the interval, not just its start. */
static const uint8_t* tm_live_guest_ptr(void* user, uint32_t location,
                                        uint32_t offset, uint32_t min_bytes)
{
    (void)user;
    if (!vm_base) return nullptr;
    const uint32_t ea = cellGcmResolveLocated(location == 0, offset);
    if (!ea || ea == 0xFFFFFFFFu) return nullptr;
    if ((uint64_t)ea + min_bytes > 0x100000000ull) return nullptr;
    return (const uint8_t*)vm_base + ea;
}

static DWORD WINAPI vblank_ticker(LPVOID)
{
    /* Size the backend to the surface the GAME renders, not to the video mode.
     *
     * Twisted Metal draws into 1280x704 surfaces (its tiles and every RT the
     * backend reports are 1280x704) while configuring video-out as 720x480 and
     * compositing later. RT_DISPLAY_BY_SIZE, which is what rescues a title that
     * renders into a surface cellGcmSetDisplayBuffer never registered, only
     * treats a surface as the backbuffer when its clip EQUALS the backend size
     * -- so a backend opened at 1280x720 misses 1280x704 by sixteen rows, every
     * draw stays classified offscreen, render_frame() is never called and the
     * window shows nothing but the clear.
     *
     * TM_RSX_W / TM_RSX_H override it; the default matches this title. */
    uint32_t rsx_w = 1280, rsx_h = 704;
    if (const char* e = getenv("TM_RSX_W")) rsx_w = (uint32_t)strtoul(e, 0, 0);
    if (const char* e = getenv("TM_RSX_H")) rsx_h = (uint32_t)strtoul(e, 0, 0);
    /* RSX_LIVE_DRAW=1 selects the live NV4097->D3D12 engine: the null backend
     * opens the window, the engine binds its swap chain to that HWND and takes
     * over presentation. Otherwise the generic D3D12 backend runs as before. */
    const int live = rsx_live_draw_enabled();
    int rsx_ok = 0;
    if (live) {
        rsx_ok = (rsx_null_backend_init(rsx_w, rsx_h, "Twisted Metal (ps3recomp)") == 0);
        if (rsx_ok) {
            const int r = rsx_live_draw_init(rsx_null_backend_get_hwnd(), rsx_w, rsx_h,
                                             tm_live_guest_ptr, nullptr);
            if (r == 0) {
                rsx_null_backend_suppress_present(1);
                fprintf(stderr, "[rsx] live-draw engine up (D3D12); GDI present suppressed\n");
            } else {
                fprintf(stderr, "[rsx] live-draw init FAILED (%d) -- GDI present only\n", r);
            }
        }
    } else {
        rsx_ok = (rsx_d3d12_backend_init(rsx_w, rsx_h, "Twisted Metal (ps3recomp)") == 0);
    }
    fprintf(stderr, "[rsx] backend init %s\n", rsx_ok ? "OK -- window open" : "FAILED");
    /* The game's frame pacing (vblank/flip handlers -> display frame counter) must
     * advance at ~60Hz regardless of how long present() blocks. On a hidden/occluded
     * window DXGI Present throttles hard, which previously stalled these ticks and
     * paced the game's main loop to ~0.5fps. Drive the ticks off REAL elapsed time
     * and catch up in a bounded burst so present latency never slows the game. */
    ULONGLONG next_tick = GetTickCount64();
    for (;;) {
        Sleep(1);
        /* Drain the GCM FIFO as fast as we wake (~1ms), decoupled from the 60Hz
         * present/flip tick. The game's _jsGcmFifoFinish reference-wait spins on
         * control->ref expecting the RSX to advance it promptly; draining only at
         * 60Hz lets that wait time out (JSGcmFifo.cpp:142 ref-mismatch assert), so
         * keep control->get/ref tracking control->put with ~1ms latency. */
        if (rsx_ok) cellGcm_rsx_process_fifo();
        tm_fbdump_tick();   /* TM_FBDUMP=<secs>: snapshot the guest framebuffer */
        tm_ef_kick_tick();  /* TM_EF_KICK=<secs>: probe the stalled SPURS wait */
        tm_loaddone_tick(); /* TM_LOADDONE=<secs>: probe the load-complete byte */
        tm_fifowatch_tick();/* TM_FIFOWATCH=1: watch the RSX put/get/ref */
        tm_memdump_tick();  /* TM_MEMDUMP=<addr>[,n]: watch guest bytes */
        ULONGLONG now = GetTickCount64();
        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcm_request_tick();   /* no guest code here -- delivered on main thread */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += 16;                 /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -> resync */
        if (rsx_ok) {
            /* The live engine presents through its own swap chain on the null
             * backend's window, so pump that window's messages instead. */
            if ((live ? rsx_null_backend_pump_messages()
                      : rsx_d3d12_backend_pump_messages()) != 0) { rsx_ok = 0; }
            if (getenv("TM_PACETRACE")) {
                static ULONGLONG s_win=0; static int s_pf=0, s_pres=0; static ULONGLONG s_presms=0;
                s_pf += fired; s_pres++;
                ULONGLONG t0=GetTickCount64(); if (!live) rsx_d3d12_backend_present(); ULONGLONG t1=GetTickCount64();
                s_presms += (t1-t0);
                if (s_win==0) s_win=now;
                if (now - s_win >= 1000) {
                    fprintf(stderr,"[PACE] process_fifo=%d/s  present=%d/s  present_total=%llums/s (avg %llums)\n",
                            s_pf, s_pres, s_presms, s_pres? s_presms/s_pres:0);
                    s_pf=0; s_pres=0; s_presms=0; s_win=now;
                }
            } else {
                /* The live engine self-presents on the guest's flip method, so
                 * there is nothing to drive from here when it owns the window. */
                if (!live) rsx_d3d12_backend_present();
                else {
                    /* Upstream the engine self-presents when it sees the guest's
                     * flip method (0xE944, established from Yakuza). This title
                     * flips through a different path, so drive presentation on
                     * the same ~60Hz tick that used to drive the D3D12 backend. */
                    rsx_live_draw_present(0);
                    /* Is the engine actually receiving the stream? Frames only
                     * advance when it sees a flip; draws only when geometry
                     * reaches it. Both zero means the feed is not connected. */
                    static ULONGLONG last = 0;
                    const ULONGLONG now = GetTickCount64();
                    if (now - last >= 2000) {
                        last = now;
                        fprintf(stderr, "[live] frames=%u last_draws=%u\n",
                                rsx_live_draw_get_frames(),
                                rsx_live_draw_get_last_draws());
                        fflush(stderr);
                    }
                }
            }
        }
    }
    return 0;
}

extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;
#include <tlhelp32.h>
/* When the boot wedges, snapshot every other thread's instruction pointer as a
 * module RVA (symbolize with llvm-symbolizer) so a guest spin/wait is pinned to
 * an exact lifted function -- the HLE breadcrumb only covers HLE calls. */
/* Snapshot every other thread's RIP. For threads in the boot module (lifted
 * guest code) print the RVA (symbolizable) + a couple of stack-return RVAs;
 * for threads parked in a DLL (OS waits / FMOD) print the module name so they
 * are not mistaken for guest spins. Called twice so the caller can diff which
 * guest thread is genuinely parked (same RIP) vs. still progressing. */
static void dump_threads(const char* label, HMODULE self)
{
    fprintf(stderr, "[WATCHDOG] %s; last HLE call = 0x%08X (%s)\n",
            label, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    /* Module range from the PE headers, so the per-address in-module test below
     * is a range compare instead of a loader-lock-taking API call. */
    const uint64_t self_lo = (uint64_t)self;
    uint64_t self_hi = self_lo + 0x10000000ull;
    {   const IMAGE_DOS_HEADER* dh = (const IMAGE_DOS_HEADER*)self;
        if (dh->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS* nh =
                (const IMAGE_NT_HEADERS*)((const char*)self + dh->e_lfanew);
            if (nh->Signature == IMAGE_NT_SIGNATURE)
                self_hi = self_lo + nh->OptionalHeader.SizeOfImage;
        } }
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            /* NOTHING that can take a process-wide lock may run while the target
             * is SUSPENDED: fprintf takes the CRT stdio lock and
             * GetModuleHandleExA takes the loader lock, so if the suspended
             * thread happened to hold either, the watchdog deadlocks AND never
             * resumes it -- freezing the process it was meant to diagnose.
             * Capture raw values here, resume, then symbolize and print. */
            uint64_t rip = 0, rvas[20]; int nrva = 0; bool got = false;
            SuspendThread(th);
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            if ((got = !!GetThreadContext(th, &ctx))) {
                rip = ctx.Rip;
                /* Scan the suspended thread's stack for boot-module return
                 * addresses to reconstruct the lifted call chain — done for ALL
                 * threads (even when RIP is parked in ntdll inside a CriticalSection
                 * call), since that's exactly where the busy-spin's lwmutex churn
                 * lands the main thread. Map RVAs -> func_ names via symrva.py.
                 * (some false positives expected — these are stack-scan hits.)
                 * The in-module test is a plain range compare against the PE's
                 * SizeOfImage: no API call, so no loader lock. */
                uint64_t* sp = (uint64_t*)ctx.Rsp;
                /* Bound the scan to the committed stack region so we never read
                 * past the guard page (VirtualQuery gives this region's end;
                 * it takes no lock the suspended thread could be holding). */
                MEMORY_BASIC_INFORMATION mbi;
                uint64_t region_end = (uint64_t)sp + 0x8000;
                if (VirtualQuery((LPCVOID)sp, &mbi, sizeof mbi))
                    region_end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
                int maxk = (int)((region_end - (uint64_t)sp) / 8);
                if (maxk > 0x20000 / 8) maxk = 0x20000 / 8;
                for (int k = 0; k < maxk && nrva < 20; k++) {
                    uint64_t v = sp[k];
                    if (v >= self_lo && v < self_hi) rvas[nrva++] = v - self_lo;
                }
            }
            ResumeThread(th);
            CloseHandle(th);
            if (got) {
                if (rip >= self_lo && rip < self_hi) {
                    fprintf(stderr, "[WATCHDOG]   tid %5lu BOOT rip rva=0x%llX\n",
                            (unsigned long)te.th32ThreadID,
                            (unsigned long long)(rip - self_lo));
                } else {
                    HMODULE m = NULL; char path[MAX_PATH] = "?";
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)rip, &m);
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    fprintf(stderr, "[WATCHDOG]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                }
                for (int k = 0; k < nrva; k++)
                    fprintf(stderr, "[WATCHDOG]       tid %5lu ret rva=0x%llX\n",
                            (unsigned long)te.th32ThreadID, (unsigned long long)rvas[k]);
            }
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    fflush(stderr);
}

static DWORD WINAPI hang_watchdog(LPVOID)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hang_watchdog, &self);
    /* Keep sampling: the two-shot version could only ever see the first 15s, so
     * a wedge later in the boot (the cviewer scene load) was never captured.
     * TM_WATCHDOG_SECS=0 disables; default is a sample every 20s. */
    const char* e = getenv("TM_WATCHDOG_SECS");
    int period = e ? atoi(e) : 20;
    if (period <= 0) return 0;
    for (int n = 1; ; n++) {
        Sleep((DWORD)period * 1000);
        char label[32];
        snprintf(label, sizeof label, "%ds sample", n * period);
        dump_threads(label, self);
    }
}
#endif

/* The flat VM treats every address as valid RAM, so it must span every region
 * the PS3 memory map uses. The game's heap maps at 0x20000000+ and reaches
 * ~0x50000000, but sys_ppu_thread_create allocates thread stacks in the PS3
 * stack region at 0xD0000000-0xDFFFFFFF (vm.h: VM_STACK_BASE). Without covering
 * that, every spawned thread's stack access is OOB (reads 0 / writes dropped)
 * and the thread crashes. Size to include the stack region: ~3.75 GB, lazily
 * committed by the OS (only touched pages are backed). */
#define VM_SIZE    0x100010000ull /* full 32-bit guest space + 64K guard (top-edge reads), demand-committed */
#define STACK_TOP  0x0FF00000u   /* main-thread stack, below the 0x10000000 segment */

#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the full 4 GB guest space up front (no
 * commit cost) and commit each 64 KB page on first access. This makes EVERY
 * 32-bit guest offset valid -- a garbage guest pointer reads as zero instead of
 * crashing the process (essential now that the recompiled engine runs deep and
 * worker threads touch incomplete state). Out-of-arena faults fall through to
 * the crash reporter. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s <EBOOT.elf>\n", argv[0]); return 2; }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    timeBeginPeriod(1);   /* 1ms timer resolution: the default ~15.6ms granularity
                           * inflates every sub-15ms wait (the game's event polls,
                           * usleeps) and throttled the whole title. */
    SetUnhandledExceptionFilter(tm_crash_filter);
    AddVectoredExceptionHandler(0 /*last*/, [](EXCEPTION_POINTERS* ep)->LONG{
        if (ep->ExceptionRecord->ExceptionCode == 0xC00000FDu /*STACK_OVERFLOW*/) {
            fprintf(stderr,"\n[STACKOVERFLOW] infinite recursion detected; backtrace (RVAs):\n");
            HMODULE mod=GetModuleHandleA(0); void* fr[62]; USHORT n=RtlCaptureStackBackTrace(0,62,fr,0);
            for(USHORT i=0;i<n;i++) fprintf(stderr," %llX",(unsigned long long)((char*)fr[i]-(char*)mod));
            fprintf(stderr,"\n"); fflush(stderr); ExitProcess(7);
        }
        return EXCEPTION_CONTINUE_SEARCH; });
    { ULONG g=256*1024; SetThreadStackGuarantee(&g); }  /* reserve stack so the SO handler can run */
    signal(SIGABRT, tm_abort_handler);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: don't lose prints on kill */
#endif

    /* Flat VM: one host buffer, guest addr -> vm_base + addr. This maps the
     * FULL 32-bit guest space uniformly (page 0, the 0x60000000..0xD0000000
     * range, everything) -- which native-VA mapping can't on Windows, because
     * the OS reserves the low 64 KB and DLLs occupy parts of the mid range.
     * On real PS3 those addresses are RAM, and the game writes to them (its
     * null-object inits land on page 0); calloc backs them so the game runs.
     * HLE functions that take guest pointers must translate via vm_base /
     * vm_write* (which also byte-swap) -- a raw *guest_ptr would deref the host
     * buffer's offset incorrectly. */
#ifdef _WIN32
    /* Reserve the full 4 GB guest space; pages commit on first touch via the VEH. */
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;   /* full 32-bit space backed -> OOB guard unnecessary */
#else
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) { printf("load failed\n"); return 1; }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    fprintf(stderr,"[boot-dbg] before ppu_recomp_register\n"); fflush(stderr);
    ppu_recomp_register();   /* lifted function table -> address map */
    fprintf(stderr,"[boot-dbg] after ppu_recomp_register; before ps3_load_prx_modules\n"); fflush(stderr);
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    fprintf(stderr,"[boot-dbg] after prx; before ppu_hle_init\n"); fflush(stderr);
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    tm_hle_register_extra(); /* first registration wins the ctx lookup */
    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    tm_init_title_metadata();/* real TITLE_ID/TITLE from the disc PARAM.SFO */
    fprintf(stderr,"[boot-dbg] before lv2_init_syscalls\n"); fflush(stderr);
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */
    fprintf(stderr,"[boot-dbg] after lv2_init_syscalls\n"); fflush(stderr);

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
#ifdef _WIN32
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
#endif

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);
#ifdef _WIN32
    fprintf(stderr, "[boot] MAIN guest thread tid=%lu\n", (unsigned long)GetCurrentThreadId());
#endif
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return 0;
}
