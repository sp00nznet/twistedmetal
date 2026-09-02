# Getting the first pixels

> Why nothing drew for so long: Edge Zlib on the SPU, an RSX that was parked and never released, the walker, and the discovery that the draws had been rendering the whole time.

## Pixels: the path works, the content does not arrive

The window was black. It is now presenting, and the reason it looked dead was a
size mismatch in this repo's own harness.

The backend classifies a surface as the backbuffer if `cellGcmSetDisplayBuffer`
registered its offset, or — with `RT_DISPLAY_BY_SIZE=1` — if its clip rectangle
equals the backend's size. This title renders into **1280x704** surfaces while
configuring video-out as 720x480 and compositing later, so with the harness
opening the backend at 1280x**720** nothing ever matched:

```
[D3D12] offscreen RT 0: off=0x1870000 1280x704 (render-to-texture)
[D3D12] offscreen RT 1: off=0xAB0000  1280x704 (render-to-texture)
...                          x4, every render target
```

Every draw was classified offscreen, `has_display` stayed 0, and `render_frame()`
— which submits all recorded geometry — was never called. `src/boot_main.cpp`
now sizes the backend to the surface the game actually renders (`TM_RSX_W` /
`TM_RSX_H` override it). Offscreen classifications: **4 → 0**.

With that, geometry does reach the screen. Isolating the one object the title
draws (`DRAW_KEEP_TEX=2ACD800 DRAW_KEEP_NOCLEAR=1`) and dumping presents gives a
frame that is uniformly filled rather than empty:

```
frame_000.bmp: 1280x704   100% of pixels lit   single colour rgb(0,0,25)
frame_002.bmp: 1280x704     0% lit
```

So the quad rasterizes, blends and presents — a flat dark-blue fill, one draw,
no image detail — and then the game stops drawing entirely. It issues **20
draws in a hundred seconds**, all at startup, all the same 4-vertex QUAD with
the same 1024x512 texture bound.

That is what a stalled loading screen looks like. The render path is not what is
keeping the screen dark; the title has nothing further to draw.

## What gates the content: Edge Zlib on the SPU

The stalled SPU task identifies itself. Its image carries

```
SPUNAME  PS3_Release/edgezlib_inflate_task
EDGE ZLIB ERROR: edgeZlibInflateRawData failed (%d)
```

and the PPU binary names the taskset it belongs to, `edgeDecompressorTaskset`.
It is Sony's **Edge Zlib** decompressor — the thing that inflates the title's
packed assets. It parks in `WAIT_SIGNAL` having run 0 ms, which is *correct*
behaviour for an idle worker, and a PPU thread blocks in
`cellSpursEventFlagWait` for it.

Work **is** submitted — an earlier draft of this file said it was not, and that
was wrong. Decoding `func_0099790C`, the PPU-side wait, shows it takes a
*request object* whose busy flag at `+0x28` the SPU clears on completion; the
title queues a request and then blocks on that flag. Signalling the parked task
by hand (`spu_taskset_signal_task`) does wake it — the sleep messages stop — but
it still never clears the flag, so the lifted SPU image is not the way in.

The submission path is the **job queue**, not the lock-free queue. Logging every
faked import actually reached at run time settles it:

```
x12  0x3D1294FC  cellSpursJobQueuePortInitialize
x6   0xF244E799  _cellSpursCreateJobQueue
x6   0x80A0264C  cellSpursJobQueuePortTrySync
x6   0x1686957E  cellSpursJobQueueAttributeSetMaxSizeJobDescriptor
x5   0x634B1502  cellSpursJobQueuePortSync
x1   0x011EE38B  _cellSpursLFQueueInitialize
```

(The unnamed `cellSpursJq` NIDs were resolved by computing NIDs from RPCS3's
function names.) The title creates six job queues and twelve ports and syncs
them — and `_cellSpursJobQueuePortPushBody` is **never called**, so it is
blocked before it ever submits a job. `_cellSpursLFQueuePushBody` is never
called either; an earlier draft of this file blamed it, wrongly.

`cellSpursJq` is 63 `UNIMPLEMENTED_FUNC` stubs in RPCS3 and absent from
ps3recomp, so this is a reverse-engineering project rather than a bridge — and
it is the one thing between this port and content.

## Inflating on the host, and the buffer that was wrong twice

ps3recomp's porting guide recommends doing SPU decompression on the host, and
the requests make that easy: the captured 63-byte stream starts `78 DA` and
`zlib.decompress` returns exactly the 132 bytes the descriptor asks for. It is
ordinary RFC 1950 zlib.

ps3recomp links no zlib and Windows exposes no system one, so `src/tm_inflate.cpp`
is a self-contained RFC 1951 inflater — stored, fixed and dynamic Huffman, about
200 lines. `tm_inflate_selftest()` checks it against that captured request, so
the decoder has a runnable check that fails if the logic breaks.

Overriding `func_0099790C` to service the request and clear the busy flag took
the title from **2 files opened to 186**: fonts, localisation, every `.rtt`
texture, every level `.psarc`, the audio banks and `ui.psarc`. That single
inflate is the archive's `contents.dat`.

It then stopped again, and the reason was a wrong field. The request is:

```
+00: 016ACD28   +0C: 122A0331 (src)   +10: 0000003F (63)
+14: 122A02EC   +18: 00000084 (132)   +24: 00000084   +28: 00000001 (busy)
```

`+0x00` looks like the destination and it is not. The source at `+0x0C` sits
`0x45` bytes *inside* the `+0x14` buffer, because **Edge decompresses in
place**: it DMAs the compressed file into local store and writes the inflated
result back over the same main-memory buffer. Writing to `+0x00` left the title
parsing the still-compressed bytes, which it did without complaining — the
20-byte name field of each `contents.dat` entry read as empty, so it built the
member path with `"%s/%s"` and an empty second half and opened the archive's
mount root:

```
ArchiveLoader (ui//ui.psarc)... Failed to load ui//ui/!
```

Two dead ends came before the right answer. Extracting `ui.psarc` to the loose
directory the path names turned the FIOS error from `0x80010709` into
`0x80010012` and made things *worse* — `loc_0036028C` compares against
`0x80010709` explicitly, so "not found" is a case the title handles and
"is a directory" is not. Putting an empty file there instead let the open
succeed and printed the third member path as mojibake — and that mojibake was
the compressed stream, which is what finally named the bug.

Inflating into `+0x14` instead takes the title through `TweakFile::Object::Read():
Success (ui/ui.ltn)`, the patch-path setup, and `CommonBank::Load() : Loading
bank "shell"... 669872 bytes`. `tools/unpsarc.py` — the PSARC v1.4 reader written
during the wrong turn — stays, because it is how the archive's real layout was
confirmed against what the title was reading.

## The front end runs; the renderer never leaves the loading screen

With the trace readable, the legal sequence runs in full:

```
-> UiLegal_Health::onEnter    -> UiLegal_1::onEnter
-> UiLegal_1::update (x3)     -> UiLegal_2::onEnter
-> UiLegal_3::onEnter         -> UiLegal_4::onEnter
```

That is the sequence the intro movie sits on the far side of, and the state
machine advances through it on its own thread. What does not happen is any
drawing. `PERF=1` is unambiguous:

```
[PERF] 561.43 fps | tex 0.00s (0%, 0 calls) | vtx 0.00s (0%, 0k verts)
       | render_frame 0.00s (3%) | guest 0.03s (97%)
```

Zero vertices, for the whole run. The backend is presenting empty frames as fast
as it can while the guest does all the work — so the dark window is now the
guest not submitting, not the backend dropping anything. `renderInit(1, 1280,
720)` returns 1, and every `bctr to NULL` indirect-dispatch failure that used to
fill the log is gone: they were downstream of the garbage decompressed data.

The reason is that the boot sequence never leaves its load bar. `func_0064C23C`
(`updateLoadBar`) spins until the byte at `0x0190FC89` goes non-zero, and while
it does, the only thing rendering is the load bar. The byte's two writers,
`func_0064C410` and `func_0010E57C`, are never reached — both are called
indirectly, and indirect dispatch demonstrably works now, so the title simply
has not decided the load is done. Setting the byte by hand (`TM_LOADDONE=<secs>`)
does release the thread that spins on it, but `updateLoadBar` still does not
return, so that byte is a symptom rather than the gate.

Two measurements are worth recording because both contradicted a guess:

- **The decompressor is not the slow part.** 1.8 ms per 64 KB block, 0.46 ms of
  it actually inflating — about five seconds for the whole 192 MB archive.
- **The flip wait is not the gate either.** The renderer's frame wait
  (`func_006755D0`) polls `cellGcmGetFlipStatus` up to 25,000 times at 40 us
  before giving up, and a raw count looks damning — 2.28 M polls, 98.4% of them
  `WAITING`. Per completion it is 64 polls, about 2.5 ms a frame, which is fine.
  The one-off 8,890-poll run (356 ms) is the archive load starving the ticker.

Audio is a known casualty rather than a mystery: BRB's mixer is the SPURS job
workload `Wws_Job/BrBDblBufWrkld`, whose PM is not lifted, so its primary output
never comes up and `brb_StartSession()` times out after ten seconds. The title
carries on without it.

## Why nothing draws: the RSX is parked and never released

`PERF=1` says zero vertices, so the next question is whether the title is
submitting at all. `TM_FIFOWATCH=1` prints the RSX control register once a
second — `put`, `get`, `ref` — read straight out of guest memory at ps3recomp's
fixed control-register EA:

```
[fifo] put=0x0F7050D4 get=0x0F700940 ref=0x00000000
       STALLED, words at get: 2F700940 0010C400 02C00400 00022800 00040194 FEED0000
```

`put` moves; `get` does not. The title **is** submitting — the drain is not
following. `GCM_RECDBG=1` shows why. The FIFO starts out healthy, ping-ponging
between two 6 MB segments of the 12 MB ring the title maps at IO `0x0F100000`:

```
[JMP] 0F100374 -> 0F700100 (put=0F700248)
[JMP] 0F700248 -> 0F100200 (put=0F100348)
```

and then degenerates into

```
[JMP] 0F700940 -> 0F700940 (put=0F7050D4)
```

`0x2F700940` is a JUMP whose target is its own address — the "park the RSX
here" idiom. A title writes it at the write head so the GPU stops if it catches
up, and overwrites it when the next segment is appended. This one never
overwrites it: `put` moves on into the other half and the park is left standing,
mid-segment, with perfectly good commands sitting right behind it
(`0x00040194` is a method write, `FEED0000` a DMA context handle).

The drain used to *take* that jump, re-reading the same word forever — **38.8
million times in one run**. `docs/ps3recomp-fixes.patch` stops that pass instead
(the next one re-reads the word, so a jump that does get patched is still
followed).

The second half of the patch is the reason the title never releases the park.
Its GCM imports name its sync mechanism: `_cellGcmSetFlipCommandWithWaitLabel`
and `cellGcmGetLabelAddress` — it waits on RSX **labels**. And the FIFO's
"unknown method" log is the NV406E semaphore trio:

```
[RSX] unknown method 0x0064 = 0x00000400   SEMAPHORE_OFFSET
[RSX] unknown method 0x0068 = 0x00000000   SEMAPHORE_ACQUIRE
[RSX] unknown method 0x006C = 0x00000001   SEMAPHORE_RELEASE
```

All three were falling through as no-ops, so the label the title waits on never
moved. `GCM_REFLOG=1` confirms the other half of the same story: **zero fences
drained in a whole run**, `ref` pinned at 0, so `_jsGcmFifoFinish` times out
every frame — 25,000 polls at 40 us — and the title never appends its next
segment.

The patch implements RELEASE (write the label) and OFFSET, and puts blocking
ACQUIRE behind `GCM_SEMA_ACQUIRE=1`: made unconditional it is what the hardware
does, but it desynced the parser here, leaving `get` sitting in the middle of
vertex floats. Off by default, RELEASE alone is the half that can unblock a
waiting guest.

Measured, honestly: `render_frame` goes from **0% of frame time to 95%** and the
frame rate from 6.8 to 32 fps, so the backend is finally doing work every frame
— and the screen is still black, with zero vertices.

`GCM_RECDBG=1` now also records the last words the walker consumed and prints
that decode chain when a branch goes bad, which answers where it loses sync.
The answer is: it doesn't. The chain is correctly aligned for hundreds of
commands — real NV4097 state, surface, viewport, transform — right up to

```
io=0F101F68 w=00040064  method=0x0064 count=1   SEMAPHORE_OFFSET
io=0F101F70 w=0004006C  method=0x006C count=1   SEMAPHORE_RELEASE
io=0F101F78 w=40000000  <- float 2.0f
```

`0x0F101F78` is exactly where the healthy trace had `JUMP -> 0F700100`. Every
segment jump sits at a `0xF78` block boundary, and this one has been
**overwritten with vertex data**. The title reused the block while the RSX was
one command short of the jump out of it — a FIFO overrun, not a parser bug.

That closes the loop with the timeout: `_jsGcmFifoFinish` waits, the wait fails,
the title proceeds anyway after its 25,000 polls and writes over commands the
walker has not reached. The semaphore trace shows the handshake it is waiting on:

```
[SEMA] m=0x64 v=0x00000010   OFFSET label 1
[SEMA] m=0x68 v=0x00000000   ACQUIRE  wait for label 1 == 0
[SEMA] m=0x64 v=0x00000400   OFFSET label 64
[SEMA] m=0x6C v=0x00000001   RELEASE  label 64 = 1
```

So the next step is that handshake, not the command parser.

One hypothesis was tested and disproved, which is worth recording so nobody
spends the afternoon on it again. ps3recomp completes a flip on the 60 Hz beat
whether or not the walker has reached the flip command, so a title that infers
"the GPU is done with that block" from flip status would recycle too early —
which is exactly the overrun above. Gating flip completion on `get == put`
changes nothing: the desync still happens, still at a block-end jump, still with
zero vertices. The title is not inferring from flip status, so that patch was
taken back out rather than left as a knob that does nothing.

## Getting the walker unstuck, and the first real geometry

The overrun above leaves the walker pointing at whatever it mis-decoded, and
nothing brings it back — `get` freezes for the rest of the run. Three recoveries
in `docs/ps3recomp-fixes.patch` fix that, in increasing order of generality:

- a branch whose target has no IO mapping **resyncs to `put`** instead of
  stopping, the same trade the unmapped-`get` path already makes;
- a park (`JUMP` to its own address) whose block the title has since abandoned —
  `put` is elsewhere — **follows the write head**, because the title only ever
  patches a park when it reuses that block;
- and a **watchdog**: if `get` has not moved across several passes while `put` is
  somewhere else, resync. The first two handle stalls we recognise; this one
  catches the rest, and it is what makes the result repeatable. Without it the
  same boot rendered about one run in five.

With those, the FIFO stays live — `get == put`, `ref` publishing, and the
semaphore label the title waits on finally moving:

```
[fifo] put=0x0F105694 get=0x0F105694 ref=0x80000000  labels: [64]=1
```

and the renderer receives real work for the first time:

```
[PERF] tex 6469 calls | vtx 660k verts | pso 4362 calls | render_frame ...
```

660,000 vertices per 20 frames, 6,469 texture binds, 4,362 pipeline states, and
seven offscreen render targets including the half-res post-processing chain —
against zero for all of them before. The title is drawing its main menu.

The screen is still black, and the composite is now located exactly.
`GCM2D_TRACE=1` shows the title issuing a full NV3089 scaled blit on
**subchannel 6** — which the backend does accept — with every method present and
the trigger fired:

```
subch=6 0x0400 IMAGE_IN_SIZE   = 0x01600400   (1024 x 352)
subch=6 0x0404 IMAGE_IN_FORMAT = 0x00022800   (pitch 10240)
subch=6 0x0408 IMAGE_IN_OFFSET = 0x01BE0000   (offscreen RT 3)
subch=6 0x0318/0x031C          = 0x00100000   (1:1 scale)
subch=6 0x0300 FORMAT          = 0x00000003
```

And the blit does run — `NV3089_DBG` catches it:

```
[NV3089] 1024x352 fmt=0x3 src=0xC1BE0000(pitch 10240) -> dst=0xC0026A80(pitch 5120) at 0,0
```

`0xC0026A80` is inside display buffer 0 (base `0x10000`), at the letterbox
inset. So the title's final image is assembled correctly — into **guest
memory** — and that is the whole problem. `nv3089_blit()` is a CPU copy; it
never touches D3D12. Meanwhile the backend presents its own swapchain, and it
only runs the draw pass when a recorded draw targets the display surface.
`VP_SUBMIT=30` shows that never happens:

```
[PRESENTGATE] records=4 onscreen=0 offscreen=1 clears=3
              has_display=0 -> render_frame=SKIPPED
```

Every batch, all run. This title composites *entirely* through the 2D engine, so
`has_display` is 0 forever and the swapchain shows only the clear. Nothing is
broken in the parser, the walker, the shaders or the draws — the frame is
finished, in the wrong memory.

So the backend needs to be able to show a frame it did not draw.
`GCM_GUEST_FB=<off>,<w>,<h>,<pitch>` in `docs/ps3recomp-fixes.patch` adds that:
it maps the guest display buffer, swaps BGRA to RGBA into an upload heap and
`CopyTextureRegion`s it onto the backbuffer at the end of the pass, and forces
`has_display` so the pass runs at all. `GCM_GUEST_FB=0x10000,720,480,5120`
wires up correctly — but there is still nothing to show, because **the composite
itself only runs occasionally**. A whole run often logs no `[NV3089]` line at
all, and both display buffers read back black.

And that is not the reason either. `[NV3089]` is gated behind `NV3089_DBG`, so
its absence from a log meant nothing — a mistake of mine that cost a couple of
rounds. With it on, the composite runs every frame, in two chunks that together
span the full 1280:

```
[NV3089] 1024x352 fmt=0x3 src=0xC1BE0000(pitch 10240) -> dst=0xC0026A80(pitch 5120)
[NV3089]  256x352 fmt=0x3 src=0xC1BE1000(pitch 10240) -> dst=0xC0027A80(pitch 5120)
```

which finally names the real problem. `src=0xC1BE0000` is offscreen RT 3 **in
guest memory**, and the pitch says what it is: 10240 over 1280 pixels is 8 bytes
each — RGBA16F. But the 3D scene is rendered by D3D12 into GPU textures; nothing
ever writes those bytes in guest memory. So the composite faithfully copies
zeros, every frame, and always will.

That is the architectural mismatch underneath all of it: **the scene lives on
the GPU and the title's compositor reads it from guest RAM.** Not a desync, not
a dropped command, not a format — the two halves of the frame path are in
different memories. Closing it means teaching `nv3089_blit` that when its source
matches a known offscreen D3D12 target it should do a GPU-side composite to the
backbuffer instead of a CPU copy — with a format conversion, since the source is
half-float and the backbuffer is 8-bit, so a fullscreen draw rather than a
`CopyTextureRegion`.

`GCM_GUEST_FB` above is the other half of that and already works; it just has
nothing to present until the source is real. Retargeting the scene pass straight
at the backbuffer (`RT_DISPLAY_BY_SIZE=1` at 1280x704) is not a shortcut either:
four surfaces share those dimensions, so the render-to-texture chain collapses
and the screen stays black.

So the composite was built. `GCM_COMPOSITE_RT=1` makes `nv3089_blit` hand the
job to the GPU when its destination lands inside a registered display buffer and
its source is a surface the backend holds as a texture — a `CopyTextureRegion`
from that render target onto the backbuffer, with `has_display` forced so the
pass runs. `GCM_COMPOSITE_RT=<hex>` forces a particular source, and
`RT_FORCE_RGBA8=1` allocates offscreen targets as 8-bit, because the title's
real composite source is `dxgi=10` — `R16G16B16A16_FLOAT`, exactly as the
10240-byte pitch over 1280 pixels predicted — and a same-format copy cannot
read it otherwise.

All of that works. It finds the right target, in the right format, and copies
1280x704 onto the backbuffer every frame. **And every render target is empty.**
Not the composite source, not the scene MRT target, not the post-process chain:

```
GCM_COMPOSITE_RT=0xAB0000  -> [COMPOSITE] RT 0x00AB0000 1280x704 -> backbuffer, 0.00%
GCM_COMPOSITE_RT=0x1BE0000 -> [COMPOSITE] RT 0x01BE0000 1280x704 -> backbuffer, 0.00%
```

while `PERF` in the same runs reports 660k vertices, 6469 texture binds and 4362
pipeline states with zero cache misses.

Two refinements ruled out the obvious explanations. `off_rt_find()` matches on
offset alone and slots share offsets at different sizes, so looking the source
up by address can pick a stale one — `GCM_COMPOSITE_RT=2` instead composites the
slot **the draw loop itself bound**, which is where geometry provably went. It
picks `0x1BE0000`, and with `RT_FORCE_RGBA8=1` making that copyable the
composite runs every frame onto the backbuffer. Still nothing.

Then the shading path was removed from the question entirely:

```
RT_FORCE_RGBA8=1 GCM_COMPOSITE_RT=2 DEPTH_OFF=1 CULL_OFF=1 NO_ALPHATEST=1 FP_FORCE=1
    -> 0.00%
RT_FORCE_RGBA8=1 GCM_COMPOSITE_RT=2 VP_BYPASS=1 DEPTH_OFF=1 CULL_OFF=1
    -> 0.00%
```

Depth off, culling off, alpha test off, fragment shader forced to solid magenta,
vertex program bypassed — and not one pixel. Meanwhile the vertex shaders
decompile and cache normally (`[VP] per-draw VS cached`, 3-8 instructions each,
the right shape for UI quads). So this is not depth, culling, blending, the
fragment program or the composite: the geometry is not rasterising at all, which
points at the transform or at the draws never reaching the GPU.

Chasing that produced the sharpest statement of the problem, and one correction
of my own: `VTX_POS` masks components past the attribute's `size`, so a
2-component position printed as `(-1, 1, 0, 0)` and looked like a `w=0`
degenerate vertex. It is not. Printing all four shows the fetch is perfect:

```
[VTXPOS] a0 off=0x80FDF220 stride=16 size=2 type=2 -> (-1,  1, 0, 1)
[VTXPOS] a0 ...                                    -> (-1, -1, 0, 1)
[VTXPOS] a0 ...                                    -> ( 1, -1, 0, 1)
[VTXPOS] a0 ...                                    -> ( 1,  1, 0, 1)
```

A fullscreen quad in NDC, `w=1`, exactly as it should be — the constant vertex
attribute register defaults to `(0,0,0,1)` and the fetch honours it.

So the input is right and the output is nothing. Everything between them checks
out too. A per-batch histogram (`VP_SUBMIT`) says the whole batch is real
geometry aimed at one surface, and all of it is flagged for execution:

```
[PRESENTGATE] targets: 0x00AB0000 x57
[PRESENTGATE] is_vp=1289 not_vp=0
[VPPASS] records=1328 is_vp=1328 clears=39 any=1 vpso=... rootsig=...
```

`is_vp` matters because the execution loop skips anything without it; nothing is
being skipped. The VP pass runs with a valid PSO and root signature. And
`RTT_SAVERT` on `0x00AB0000` — the surface every one of those draws targets —
reads back **0.000%** at frame 1560, as does `0x01BE0000`. (`RTT_SAVERT_SKIP`
was added for this: the built-in trigger fires at frame 60, minutes before this
title reaches its menu.)

Correct vertices, correct target, valid pipeline, draws executed, surface empty.
The obvious pipeline-state culprits were then ruled out one at a time, each
against that same surface rather than through the composite:

```
CMASK_FORCE=1                                     -> 0.000%
DEPTH_OFF=1 CULL_OFF=1 CMASK_FORCE=1 NO_ALPHATEST=1 -> 0.000%
```

Not the colour write mask, not depth, not culling, not alpha test. And then the
control that sharpens it into something worth handing over:

```
CLEAR_RGB=0.2,0.4,0.8   -> 0.000%, every pixel (0,0,0)
```

The guest issues **39 clears per batch** into that surface, and recolouring them
changes nothing. So it is not that the draws write nothing — **nothing reaches
that render target at all**, clears included. That moves the suspect off
pipeline state and onto the render-target binding itself: the RTV descriptor the
offscreen path binds, or the slot bookkeeping behind `off_rt_find` /
`off_rt_rtv` / `off_rt_transition`. The same control also shows the backbuffer
readback is sound — `CLEAR_RGB` comes back as a uniform `(204,102,51)` there —
so the instrument is trustworthy and the offscreen target genuinely is not being
written.

One more check closes the loop: the D3D12 debug layer is genuinely on
(`D3D12_DBG=1` prints "Debug layer enabled") and reports **nothing**. So every
barrier, descriptor and PSO/RTV format pairing is valid by D3D12's own
validation, while the target it all points at stays empty.

That combination — valid bindings, executed draws, unwritten target — is past
what inference can settle from logs.

The last thing worth doing without a capture was to stop trusting the layers
above the draw call and print what is actually handed to the GPU. `DRAWARGS=<N>`
does that:

```
[DRAWARGS] verts=6 start=0 rt=3 rtv=2000503706544 vp=1280x704
```

Six vertices -- a quad expanded to two triangles -- from vertex 0, into slot 3's
render-target view, with a full 1280x704 viewport. Every argument is sound. The
vertex-buffer stride agrees with the buffer view on both sides (256 bytes), and
`rsx_vtx_pos_dbg` prints the *destination* slot, so the NDC quad quoted earlier
is what landed in the buffer rather than what was read out of guest memory.

Correct vertices, stride, count, viewport and RTV; clean validation; draw
issued; target empty. Every layer this side of the GPU said the frame should be
there.

Except the validation was not clean — it was being swallowed. `D3D12_DBG=1`
enables the debug layer and prints nothing; `D3D12_IQ=1` drains the **info
queue** after submit, and that is a different thing entirely. It carries two
real findings:

```
[sev=1 id=921] ID3D12CommandList::Close: An ID3D12Resource object ... was
               deleted prior to closing the command list.
[sev=2 id=679] CreateGraphicsPipelineState: The Pixel Shader expects a Render
               Target View bound to slot 1/2/3, but ... none will be bound.
               ... writes of an unbound Render Target View are discarded.
```

The first is a genuine use-after-free: `off_rt_get()` releases an offscreen
render target the moment its dimensions change — and we see exactly that,
slot 4 going 1280x704 -> 640x352 — while the frame in flight still references
it. Every draw recorded against that resource lands nowhere and the replacement
reads back empty. `docs/ps3recomp-fixes.patch` waits for the GPU before both
release sites; recreations are rare, so the stall costs nothing. It removes the
error on the paths it covers, though a run can still surface a handful from
release sites not yet found.

The second says the fragment programs write four colour outputs while only one
RTV is ever bound, so slots 1-3 are discarded. Creating the MRT targets in the
render-to-texture pre-pass does not fix it — `off_rt_find()` still returns -1
for them, so the draw records are not carrying MRT offsets in the first place.
That was tried and reverted rather than left in.

Neither makes the picture appear — but chasing the second one exposed a flaw in
how every "is the target empty" test above was read. **`0.000% nonzero` cannot
tell an untouched surface from one written black.** The dumped fragment shader
settles part of it (`PSOut _po; _po.c0 = h[0];` — it does write `SV_Target0`),
and it also shows why `FP_FORCE` never did anything: it patches `return r[0];`,
which these shaders do not contain.

`RT_CLEARDBG=1` removes the ambiguity by clearing offscreen targets to magenta
instead of the guest colour:

```
RT_CLEARDBG=1 RTT_SAVERT=AB0000 -> 1280x704, 66.667% nonzero, uniform (255,0,255)
```

So the surface **is** reachable and writable, the clears land on it, and the
readback is sound. And the draws change **not one pixel** of it. That is the
sharpest statement of the problem yet, and it is much narrower than "nothing
renders": on the same RTV, in the same command list, `ClearRenderTargetView`
works and `DrawInstanced` does not. Whatever is wrong is in what separates
them — the PSO, the root signature, the vertex buffer view, the depth state —
and not in the surface, the binding, the FIFO or the composite.

Worth noting for whoever picks this up: `DEPTH_OFF`, `CULL_OFF`, `NO_ALPHATEST`
and `FP_IDCOLOR` all act on the guest-FP pipeline, while these draws go through
the single shared VP pipeline (`[VPPASS] vpso=...` is one pointer for the whole
batch). Those knobs were never touching the draws in question, so the earlier
"not depth, not culling, not the shader" conclusions do not hold and want
redoing against the VP path.

Redoing them against the VP path got two results worth passing on.

**The guest vertex program writes a negative w.** The decompiled VS ends

```
o[0].w = vp_c[467].x;
Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);
```

and `VP_MVP` prints the constant bank as the shader sees it:

```
[VPMVP] slot=3 lastNZ=c467  c260=(0 0 0 0)  c467=(-1 1 0 0)
```

So `w = -1`. Every vertex is behind the camera and the clipper discards all of
it — clears untouched, exactly the symptom. Worth noting too that `c467`'s value
`(-1, 1, 0, 0)` mirrors the quad's first vertex `(-1, 1, 0, 1)`, which may mean
vertex data is landing in the constant bank (a `transform_constant_load`
indexing problem) rather than the program legitimately asking for `w = -1`.

**But forcing a valid position does not help either.** `VP_BYPASS=1` rewrites
the output to `float4(v[0].xy * 0.5, 0.5, 1.0)` — `w = 1`, unambiguously on
screen — and it does patch this per-draw path (the anchor matches the dumped
HLSL). The target still comes back uniform magenta. By the backend's own note on
that switch, "if it stays blank, the fault is upstream of the shader -- the
attribute binding itself".

`LOAD_DBG=1` sharpens the first one considerably. Across a whole run the title
issues exactly **two** `SET_TRANSFORM_CONSTANT_LOAD` indices:

```
100  1
100  467
```

So `c467 = (-1, 1, 0, 0)` is genuinely what the game wrote, and the decompiled
shader takes lane `.x` of it for `w`:

```
{ float4 _v = (float4)((vp_c[467]).xxxx);  o[0].w = _v.w; }
```

Lane `.y` of that same constant is `1` — the value a sane `w` would have. That
makes a **source-swizzle decode bug in the vertex-program decompiler** the most
likely reading: the ucode probably says `.y` (or `.w` of a differently-loaded
register) and the decompiler emits `.xxxx`. Worth checking against the raw VP
ucode before believing it, but the shape of the evidence fits, and it is a
defect that would silently blank any title whose program does this.

Both of those were then tested and neither is the blocker. `VP_W_ONE=1` keeps
the guest transform and forces only the clip-space `w` to 1 — the exact fix the
swizzle theory predicts — and the target is unchanged. `VP_BYPASS=1` replaces
the position outright, same result.

And one more assumption of mine was wrong, which invalidated the earlier magenta
control: **the clears and the draws target different surfaces.** `DRAWARGS` now
prints the bound slot's offset, and the draws go to `0x01BE0000` while the
guest's clears go to `0xAB0000`. Every "clears land but draws don't" reading
above was comparing two different render targets.

`RT_CLEARDBG=2` fixes that by magenta-clearing the surface **the draws
themselves bind, at the moment they bind it**:

```
RT_CLEARDBG=2 RTT_SAVERT=1BE0000 -> 1280x704, distinct colours: 1, all (255,0,255)
```

So on the draws' own target, with the RTV proven good by the clear that lands on
it through the very same handle, the draws that follow change **not one pixel**.
That is the cleanest statement this investigation can produce without GPU-side
visibility: `ClearRenderTargetView` and `DrawInstanced`, same RTV, same command
list, one works and one does nothing.

One caveat on that evidence, and the last hypothesis it suggested:
`ClearRenderTargetView` writes through the descriptor and never consults
`OMSetRenderTargets`, so the clear proves the *descriptor* is good, not that the
binding is live when the draw runs. `RT_REBIND=1` re-issues
`OMSetRenderTargets` immediately before every draw rather than only on a target
change — and the surface is still uniformly magenta. So the binding is not it
either.

## The draws were rendering the whole time

All of the above is wrong about the most important thing, and the mistake was
mine twice over.

`VP_TESTTRI=1` injects a triangle of my own through the game's exact pipeline --
same PSO, same root signature, same vertex buffer, same RTV -- written at vertex
1000 so it cannot collide with guest data. **It rendered.** So the GPU reads
`vp_vb`, the pipeline works, and draws do reach render targets.

That forced a re-read of every "uniform magenta" result, and both were artefacts
of how I sampled them:

- `DRAWARGS` was printing the *retarget* viewport, not the per-draw one. The
  guest draws set their own: `dvp=0,0 640x352` -- the top-left quadrant of a
  1280x704 target.
- The colour histograms sampled the first 300,000 pixels of the BMP, which in
  bottom-up order is the **bottom** of the image. The draws land at the top.
  I was counting the one region they never touch.

Analysing the whole surface instead:

```
whole image      distinct=2  (255,0,255) x168960   (0,0,0) x56320
top-left 640x352 distinct=1  (0,0,0) x112640
```

The quadrant the draws target is entirely black while the rest stays magenta --
exactly the shape of geometry that rasterised. And `FP_IDCOLOR=1` fills that
same quadrant with a flat `(240,177,172)`.

**So the renderer works end to end.** Geometry rasterises, the fragment shader
executes, and forcing a colour makes it visible. The real shader outputs black
because its *input* is black:

```
[EMPTYTEX] fp=0xFDDC82 unit=0 raw=0x01870000 res=0xC1870000 1280x704 fmt=0xE5
[EMPTYSCAN] 0xC1800000..+1MB nonzero 0/1027
```

and following that to the end names the actual input. `DRAWARGS` now prints the
texture each draw samples and whether it resolves to an offscreen target:

```
[DRAWARGS] verts=6 start=0 rt=3 rec=0x01BE0000 bound=0x01BE0000
           tex0=0x02ACD800 texrt=-1 dvp=0,0 640x352
```

`texrt=-1` means it is not a render target at all -- it is an ordinary texture
at local-memory offset `0x2ACD800`, uploaded from guest memory. And that is the
same texture dumped near the top of this file with `TM_TEXDUMP`: **8.64%
non-zero, two thin bands of noise on black.** A shader sampling a near-empty
texture outputs near-black, which is exactly what lands.

So the chain is complete and it is not a graphics-pipeline fault at all:

1. the draws rasterise correctly into their 640x352 viewport;
2. the fragment program samples the texture at local memory `0x2ACD800`;
3. that texture is essentially empty;
4. so the UI renders black, and the composite faithfully carries black to the
   display buffer.

And that texture is **not** empty any more. The 8.64% reading above predates the
asset-pipeline fix; re-dumping it now gives **87.05% non-zero**. The data is
there.

`TEXFMTDBG=1` names what it is:

```
[TEXFMT] unit=0 fmt=0x86 1024x512 mips=1 cube=0 off=0x2ACD800
```

`0x86` is `CELL_GCM_TEXTURE_COMPRESSED_DXT1`, and the backend maps it to
`DXGI_FORMAT_BC1_UNORM` with a correct 4-bits-per-texel size. So every link is
sound -- populated source, known format, correct mapping -- and the sample still
comes back black.

Except the texture is not populated either, and "87% non-zero" was one more
measurement error of mine: it counts non-zero **bytes**, not decoded pixels.
`RTT_DUMP=1` prints the bytes the upload actually reads:

```
[TEXUP] off=0xC2ACD800 fmt=0x86  row0: 01 00 00 00 55 55 55 55
                                row240: 01 00 00 00 55 55 55 55
                                   mid: 01 00 00 00 55 55 55 55
```

Identical at the first row, row 240 and the middle. As DXT1 that block is
`color0 = 0x0001`, `color1 = 0x0000` — both black in RGB565 — with indices
`0x55555555`, every texel selecting `color1`. **The whole texture is a uniform
black DXT1 image**, and non-zero bytes throughout, which is why every
byte-counting check called it populated.

So the pipeline was right all along and so was the sampler: the UI draws a
texture that really is black, because the surface at local memory `0x2ACD800`
holds a formatted placeholder rather than the game's artwork. `ui.vram` — all
132 MB of it — inflates into local memory at `0xC321D800` and above, about
7.7 MB past this address, so whatever fills `0x2ACD800` arrives by another route
and has not arrived.

Tracking every local-memory destination the decompressor writes narrows that to
a number:

```
[edge] local-memory writes: 1792 blocks, 0xC2B0D800..0xC9B0D800
```

The archive lands in `0xC2B0D800` and up. The texture the UI samples is at
`0xC2ACD800` — **exactly 0x40000, 256 KB, below the first byte the loader ever
writes.** Nothing touches it, which is why it still holds a formatted black
placeholder while everything around it is real artwork.

Parsing the archive rules out the tidy explanation: `ui.vram` has **no
uncompressed blocks** — all 2021 of them are deflated, so every one passes
through the decompressor, and the observed span matches exactly (1792 blocks x
64 KB = 117 MB of the 132 MB total, still loading at the time of the print).

So the texture at `0x2ACD800` is not the head of `ui.vram` at all. It is a
*separate* VRAM allocation sitting 256 KB below it, and nothing this port
currently does writes there. A 1024x512 DXT1 surface cannot be rendered into, so
it is a loaded texture — which means it should arrive by a copy from main memory
(the `ui.ngp` side of the archive) into local memory, and that copy is what has
not happened.

That is the question to pick up: **which transfer fills local memory
`0x2ACD800`, and is that transfer path implemented?**

`GCM2D_TRACE=1` turns up a separate, concrete defect while answering it. The
title uses subchannels 1, 3, 4, 5, 6 and 7, and the drain routes every method
with `subch != 0` to `gcm_2d_method()`, which handles only 2/3 (NV3062), 4/5
(NV308A/NV309E) and 6/7 (NV3089). **Subchannel 1 is dropped**, and its methods
are not 2D at all:

```
0x0F4C  0x151C  0x1A80 0x1A84 ... 0x1AC0  0x1F78
```

`0x1A00 + i*0x20` is `NV4097_SET_TEXTURE_OFFSET(i)`, so `0x1A80`-`0x1AC0` is
texture-unit 4-6 setup — 3D methods on subchannel 1. ps3recomp assumes the 3D
object is always bound to subchannel 0; this title also drives it through
subchannel 1, and every one of those methods is silently discarded. That is
worth fixing on its own: it is state the renderer never sees, and it is
invisible because the 2D path swallows it without logging.

Whether it is *this* bug that leaves `0x2ACD800` unwritten is not yet
established — the texture the UI samples is unit 0, which would come through
subchannel 0 — but it is the first concrete defect on the asset-arrival side and
the obvious place to start.

Everything downstream of this is now known to work: FIFO walk, fences,
semaphores, rasteriser, sampler, composite and present.

Everything below this heading was chasing the wrong layer.

The list that was eliminated is still sound as far as it goes, each verified
rather than assumed: FIFO walk and fences;
vertex fetch and the buffer's contents, allocation and mapping; input layout,
stride and buffer view; `is_vp` gating; draw arguments; PSO and root signature;
RTV creation, indexing and per-draw rebinding; viewport and scissor; colour
mask, depth, cull and alpha test; clip-space `w`; the whole position; and D3D12
validation via the info queue. Everything reports correct and the draws write
nothing. What remains needs to watch the GPU execute one of them.

Those two together are the handoff: the negative `w` is a real defect that would
stop rendering on its own and wants fixing regardless, and something upstream of
the vertex shader is *also* wrong, because correcting the position is not
sufficient. `D3D12_IQ=1`, `RT_CLEARDBG=1` and `VP_MVP` are the instruments that
got this far; they are the ones to keep using. The
next tool is a frame capture: PIX or RenderDoc on a single frame will show in
seconds whether the draws reach that render target, which descriptor is actually
bound and what the output merger does with the result. Everything above narrows
where to look in that capture; the following are known-good and can be skipped:
the FIFO walk, the vertex fetch, `is_vp` gating, the colour mask, depth, cull,
alpha test, the RTV heap size and indexing, and the backbuffer readback.

That is a piece of emulator-correctness work rather than another fix, and it is
what stands between this port and a picture. The picture is in turn what the
front end needs before its menu will take input: injected pad input
(`YDKJ_INJECT_PAD`) does not move the menu today, and the movie chain — the
ArchiveLoader's movies path, the attract script, the `c1.avi`/`ep_1.avi` id
table, `MoviePlayer::openFile` — is confirmed at zero calls.

---

*Part of the [Twisted Metal static recompilation](../README.md) working log.*
