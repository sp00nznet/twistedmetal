# Into the match, and the SPU jobs that never ran

Working log for 2026-10-01 to 10-03: from the front end into a running match,
and the chase for the 3D world.

## Reaching the match

The match (suburbs, six opponents) now runs hundreds of frames, and the HUD
renders: minimap, "Opponents: 6", timer, weapons. Getting there took a string
of runtime fixes, all in the ps3recomp `ydkj-master-bringup` branch:

- **Scaled resolve.** The title composites with uneven NV3089 blits; a
  draw-based scaled blit replaced the integer-only path, so the menus and logo
  appear.
- **Havok hangs.** Several separate causes:
  - an MFC slot leak per job;
  - SPU floats need the Cell's extended range (exponent 255 is an ordinary
    binade, no NaN or Inf, denormals flush to zero);
  - PPU reservations must break on any SPU write to the same 128-byte line;
  - a real `cellSpursCreateTaskset2` (0x2900 bytes, size at +0x1890, two event
    flags at +0x1898/+0x189C, per-task exit codes at +0x1980);
  - SPU-created tasks have to actually start.
- **The IO map collision.** `gcm_io_alloc` auto-mapped over the title's own
  fixed IO map at 0x0F100000, so the FIFO walker read every in-game frame from
  the wrong memory. It now skips pages the title already mapped.
- **Park-wait.** The title parks the RSX on a JUMP-to-self and patches it
  later. The walker now gives it 100 ms before resyncing to `put`.

## Why there is no world

World draws are culled: every object's visibility byte stays 0. The cull runs
as a SPURS JobQueue job (`cellSpursJq`), and every JobQueue import was a stub.
With an HLE for push (NID 0x90E392CF) and sync (0x9396BE1D) and the 15 job
binaries lifted, the jobs dispatched and returned, but still did nothing.

**Every job was failing before `main`.** All 15 binaries are linked against
the "JOBCRT Ver13" crt (the WWS/Edge one). That crt does two things our job
runner did not expect:

1. It treats the low 28 bits of `CellSpursJobContext2+0x14` as an LS pointer
   to the job manager's kernel block. When that is 0 it returns 0x80410A11 and
   STOPs before `main`. We passed 0, as the SDK header calls it padding.
2. It reads the SPURS kernel context with absolute loads: `spuNum` at 0x1C8,
   the trace buffer at 0x210, a trace flag at 0x175. The job is loaded at LS 0,
   so those loads returned its own instruction words.

The runner now hands over a zeroed kernel block and overlays the kernel's view
of 0x170, 0x1C0 and 0x210. Jobs now run their work and return normally.

## The Edge geometry ring

With jobs running, the 68 KB geometry job (image 16) became the bottleneck. It
allocates 32 KB segments from a ring at EA 0x1F900000, header at 0x01930680.
It waits for the RSX to free segments by polling a per-SPU label: 0x80 + spuNum,
at 0x20000800 and up.

- Every job believed it was SPU 0, so four concurrent workers fought over one
  ring. Each JobQueue worker now claims its own spuNum, and the ring stalls are
  gone.
- The SPU repeated-DMA limiter halted a job after 256 identical polls, which
  threw away geometry. It now backs off for up to 2 s before halting.
- JobQueue jobs now run on four host workers (`TM_JQ_WORKERS`), not on the
  pushing PPU thread. On that thread a 1.6 s image job blocked the game.

## Frame pacing and post-FX: where it stops

The RSX waits on an NV406E ACQUIRE of label 0x42 equal to the frame number. The
walker only blocks there with `GCM_SEMA_ACQUIRE=1`; otherwise it runs past.
Label 0x42 is written by one PPU thread, **SpuPostFX** (`func_00657B84`), after
the SPU post-processing for that frame completes.

1. SpuPostFX waits on GCM notify slot 0. `cellGcmGetNotifyDataAddress` returned
   a host pointer, so its "pending" flags went nowhere and nothing ever cleared
   them. The notify area now lives in IO page 0xF1 (IO 0x0F100000 + index ×
   0x40), and `NV4097_NOTIFY` writes `{timestamp, 0}` there. SpuPostFX now
   wakes.
2. It then waits for word +0x28 of the post-FX object at 0x01928F60. The post-FX
   SPURS tasks are supposed to set it, but they are never created. The library
   init (`func_00C3DB98`) creates its Taskset2 and calls unregistered cellSpurs
   NID **0x7FDF4FEF** as (taskset, 128-byte-aligned object, 6). That looks like
   a barrier or semaphore initializer. It never reaches its `cellSpursCreateTask2`
   loop.

That is the next step: identify 0x7FDF4FEF and 0x9FCB567B, and get the post-FX
tasks created. Without them the RSX waits forever with acquire enabled, and
without acquire it skips every frame's post pass. The missing post pass is also
the likely cause of the dim (~11 % brightness) in-match image.

## Diagnostic knobs added

- `SPU_JOBPROF=<s>`: host time per job image.
- `SPU_PCSAMPLE=<s>`: PC and LR of each live SPU context.
- `SPURS_JOB_LSWORD=<img>:<lsa>`: an LS word after a job runs.
- `TM_PEEK=<ea>`: log 32 bytes on change.
- `TM_MAINSTACK=<s>`: main and SpuPostFX guest back-chains.
- `GCM_SEMA_ACQUIRE`, `GCM_METHOD_HIST`, `GCM_WORD_STATS`, `EVT_RECV_KICK`.

## Running it

Run one instance at a time, launched from PowerShell
(`Start-Process` + `WaitForExit` + `Stop-Process`). Git Bash's `timeout` often
fails to kill the process on Windows, and two parallel instances saturate a
12-core machine.
