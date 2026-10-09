/*
 * TM_GSTACKS=<seconds>: every that many seconds, print the guest call chain of
 * every live PPU thread the title created. When the boot stalls, the last
 * sample names the function each thread is parked in.
 *
 * The main thread runs on ppu_run's own context, which the thread table does
 * not hold, so it is not covered here. The Windows build samples host threads
 * instead (hang_watchdog in boot_main.cpp).
 *
 * Its own translation unit: sys_ppu_thread.h brings the runtime's ppu_context,
 * which cannot share a file with the lifter's ppu_recomp.h.
 */
#include "sys_ppu_thread.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern "C" void ppu_dump_guest_stack(ppu_context* ctx, const char* tag);

static void* tm_gstacks_thread(void* arg)
{
    const unsigned period = (unsigned)(uintptr_t)arg;
    for (unsigned n = 1;; n++) {
        sleep(period);
        fprintf(stderr, "[gstacks] %us sample\n", n * period);
        for (int i = 1; i < PPU_THREAD_MAX; i++) {
            ppu_thread_info* t = &g_ppu_threads[i];
            if (t->state != PPU_THREAD_STATE_RUNNING) continue;
            char tag[96];
            snprintf(tag, sizeof tag, "tid=%llu '%s'",
                     (unsigned long long)t->ctx.thread_id, t->name);
            ppu_dump_guest_stack(&t->ctx, tag);
        }
        fflush(stderr);
    }
    return nullptr;
}

extern "C" void tm_gstacks_start(void)
{
    const char* e = getenv("TM_GSTACKS");
    const int period = e ? atoi(e) : 0;
    if (period <= 0) return;
    pthread_t th;
    if (pthread_create(&th, nullptr, tm_gstacks_thread, (void*)(uintptr_t)period) == 0)
        pthread_detach(th);
}
