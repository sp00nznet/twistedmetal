# The live engine, and the movie path

> Porting caner's live NV4097 to D3D12 engine into this tree, the one fragment program that made everything render black, and the road to attract mode and H.264 video.

## What was actually blocking the boot: network init

Every graphics conclusion above was drawn while the title was wedged, and it was
not wedged on graphics. After `UiLegal_4` the boot entered `func_00282730` and
stayed there, logging

```
[game] Initializing network hardware, 1 unsuccessful attempts.
[game] Initializing network hardware, 2 unsuccessful attempts.
...
[game] Initializing network hardware, 150 unsuccessful attempts.
```

until the process was killed. The loop polls `cellNetCtlGetState` and leaves
only when the state reads `IPObtained` (3):

```
loc_002828F8:  r27++ ; GetState(&state)
               if (ret >= 0) goto check
               log("...unsuccessful attempts") ; *(r31+0x18) = r30   /* r30 is 0 */
check:         if (state == 3) goto done                             /* the only exit */
               if (r27 > 15) log(...)
               sleep(r27 >= 120 ? 0.5s : 5s) ; goto loc_002828F8
```

Neither an error return nor a truthful `Disconnected` breaks it — the error path
stores `r30`, which is zero, so the loop condition is unchanged and it retries.
ps3recomp's default is to *fail* the call when offline, which is right for
LittleBigPlanet (whose connect job exits on `ret < 0`) and wrong here; this repo
had additionally overridden it to return `CELL_OK` with `Disconnected`, which is
what real hardware does and also wrong here. Reporting `IPObtained` is what lets
the loop complete. `TM_NET_STATE` overrides it for testing the other two.

With that one value changed the state machine walks all the way through:

```
UiLegal_HealthWarning -> UiLegal_1 -> _2 -> _3 -> _4 -> UiNetShutdown -> MainMenu
```

and settles into a steady frame loop in `MainMenu` (`onEnter` at `0x0047EC50`).
Draw volume went from a handful of quads to **101,208 draws**, and the render
target they go to changed with it:

```
100816 draws  rt=0x00AB0000   tex0 = 0x0AB55580 / 0x0AC55580 / 0x0ACAAB80
   302 draws  rt=0x01190000
    68 draws  rt=0x01BE0000   tex0 = 0x02ACD800
```

Two things follow. The first is that the black DXT1 texture at local memory
`0x02ACD800`, which the previous section treats as *the* blocker, accounts for
68 of 101,208 draws. It is a minor element, not the menu. The second is that the
menu's real textures live at `0x0AB55580` and friends — **main memory**, not RSX
local memory — so the archive's local-memory writes were never going to be where
the menu's artwork came from.

`0x02ACD800` is still unexplained but is no longer interesting: the first
local-memory write of the archive lands at `0xC2B0D800` block 0 and marches
contiguously upward, so the texture sitting exactly `0x40000` below it is a
separate, earlier allocation and nothing is being dropped from the stream.

Render target `0x00AB0000` (1280x704) still reads back all black at frame 600,
which is around the moment the menu is entered. Whether it stays black once the
menu has settled is not yet measured: the frame counter only reaches ~600 in a
560-second run, because the flip wait now dominates (7.9 million polls for 3,382
completions, roughly 8 fps).

`MoviePlayer::openFile` remains at zero calls after ten minutes sitting in
`MainMenu`, so no attract movie starts on its own.

## Where the movie path actually begins

Tracing back from the goal rather than forward from the boot narrows it to a
handful of facts, all static and all checkable:

- `MoviePlayer::openFile` (`0x006AC648`) has exactly **one** caller in the whole
  binary: `playMovieFile` at `0x001DB604`.
- `playMovieFile` has **no** static callers. Its function descriptor
  (`0x00F010B0`) appears exactly once in the image, at `0x00CE78E0` — a slot in
  a table of descriptors, so it is only ever reached through that table.
- The only code that materialises `0x00CE78E0` is `func_001DB4FC`, immediately
  above `playMovieFile`, which looks like the constructor that installs the
  table on an object.
- `func_001DB4FC` also has no static callers, and a trace on it records **zero
  calls** in a five-minute run that reaches `MainMenu`.

So nothing that can play a movie is ever constructed. This is upstream of every
graphics question: it is not that the intro plays and shows nothing, it is that
the object which would open `USRDIR/movies/*.avi` does not exist yet.

The `.avi` names are all present in the binary, in three separate tables:

```
0x00C7DD34  ste_mgi mge_dfi df_end c1 ep_1 c2 ep_2 unlock st_mid mg_mid df_mid
0x00C897B4  ep_1 ep_2 credits c2
0x00CB2878  st_intro st_mid st_end mg_intro mg_mid mg_end df_intro df_mid df_end
```

alongside `c:/TMX/Build/packages/RT_BCUS98106/Game/attractModeScript.cpp`, so
the attract path exists in this build. Ten minutes sitting in `MainMenu` does
not start it, which is consistent with the menu never becoming interactive:
render target `0x00AB0000` takes 100,816 draws and still reads back black, and
injected pad input (`YDKJ_INJECT_PAD`) produces no state transition.

That ordering is the useful part. The next thing to find is what calls
`func_001DB4FC`, and since it is reached neither statically nor through any
descriptor in the image, the candidates are a caller the lifter never emitted
(12% of the code was not lifted — see above) or a computed call from the script
system that owns `attractModeScript.cpp`.

## Reaching the intro video

```
[fs] open '/dev_bdvd/PS3_GAME/USRDIR/movies/st_intro.avi' -> fd 52
Avi   AVC Video Stream 0 w: 1280 h: 720 start: 0 end: 28282500
Avi Subs Stream 1
Avi Audio Stream 2 freq: 48000 channels: 6 format: AC3
Avi Audio Stream 3 freq: 48000 channels: 6 format: AC3
Avi Audio Stream 4 freq: 48000 channels: 6 format: AC3
[cellVdec] StartSeq(handle=0)
[SYS] sys_ppu_thread_create tid=26 name="videoStream"
[SYS] sys_ppu_thread_create tid=27 name="audioStream"
[SYS] sys_ppu_thread_create tid=28 name="subtitleStream"
[SYS] sys_ppu_thread_create tid=30 name="VideoDisplayPS3"
[SYS] sys_ppu_thread_create tid=31 name="MoviePlayerRingBuffer"
[SYS] sys_ppu_thread_create tid=32 name="Demuxer"
[SYS] sys_ppu_thread_create tid=33 name="MoviePlayerSourceFile"
```

The title opens the campaign intro cinematic, parses its AVI container, and
brings up the entire movie player: demuxer, ring buffer, source-file reader and
the video/audio/subtitle stream threads. `MoviePlayer::openFile` had been at zero
calls through the whole investigation above; the object that owns it had never
even been constructed.

There is no picture, and that is expected rather than mysterious:
`cellVdecStartSeq` returns `0x80610101` and `cellAdecStartSeq` returns
`0x80610201` because ps3recomp's `cellVdec`/`cellAdec` accept access units and
decode nothing. Decoding H.264 and AC3 is the next piece of work, and it is a
self-contained one.

Two fixes were needed to get here, on top of the network unblock.

**`sys_ppu_thread_exit` returned to the guest.** On hardware it never returns.
ps3recomp recorded the exit status, signalled the joiners, and returned
`CELL_OK`, with a comment saying the thread proc would handle termination after
return — but the guest calls it from deep inside its thread entry, so execution
simply carried on past it. One Twisted Metal thread exits from inside a loop and
so called exit **1,435 times in a single run**, staying alive the whole time.
`ppu_host_thread_proc` now arms a `setjmp` and the syscall `longjmp`s out to it;
1,442 exit calls became 16, with 8 clean unwinds.
`PS3_NO_THREAD_EXIT_UNWIND=1` restores the old behaviour.

**The shell sound bank will not unload.** `WorldLoader::setup` tears the menu
down before the cinema plays, and `func_00606CE4` blocks in the BRB bank unload
at `func_009B2574` — named by the string it is handed, `"shell"`. That waits on
the BRB mixer, which runs as the SPURS job workload `Wws_Job/BrBDblBufWrkld`
whose PM is not lifted, so it never completes. A movie does not need the shell
bank torn down, so `TM_SKIP_BANKUNLOAD=1` returns success without doing it. That
is a workaround, not a fix: the real answer is to run the BRB mixer job.

The full sequence, all of it title-local knobs rather than code changes:

```
TM_FORCESTATE=00CEF810,100   enter UiMoviesMenu
TM_FORCECALL=0048266C,120    fire its select handler -> onSelect(st_intro.avi)
TM_LOADDONE=140              release the load-complete byte the cinema re-clears
TM_SKIP_BANKUNLOAD=1         skip the BRB unload that deadlocks the cinema load
```

## Porting the live NV4097→D3D12 engine

The generic `rsx_d3d12_backend.c` is, in its author's own words, a placeholder
shader path. Rubber Ducky renders through it because it asks very little of it.
Getting a Scaleform UI on screen means bringing across the engine that does
render — caner's (canersaka) live path from
[Yakuza-Dead-Souls-EX](https://github.com/canersaka/Yakuza-Dead-Souls-EX),
MIT, with this project as copyright holder and game-independent fixes
explicitly prepared for upstream.

Ported into `libs/video/`:

```
rsx_dispatch.c/.h        NV4097 method dispatcher + register model (clean-room,
                         documented against envytools rnndb nv30-40_3d, Mesa
                         nv30 and psdevwiki)
rsx_live_draw.c/.h       the live engine: PSO cache, vertex/index decode,
                         primitive restart, RT-as-texture, texture decode and
                         remap, per-draw constants, full raster state, fences
rsx_vertex_compact.c/.h  vertex stream compaction
rsx_restart_cuts.h
```

Its shader translators are newer than this tree's, so `rsx_fp_decompiler`,
`rsx_vp_decompiler` and `rsx_vertex_formats.h` were taken with them. Two
functions this tree adds and his does not — `rsx_fp_extract_consts` and
`rsx_fp_code_hash`, both used by the old backend — are appended to the new
decompiler rather than lost, and `rsx_fp_decompile`'s signature change (third
parameter is now `SET_SHADER_CONTROL`, not an exports-32 flag) is absorbed at
the one call site.

Three adaptations were needed to make it title-agnostic:

- **`rsx_live_draw_config.c`** supplies the Yakuza runner's state the engine
  reads — a config snapshot and three A010 debug flags — all zero, so every
  Yakuza-specific path is off.
- **Opt-in.** Upstream defaults the engine ON because that runner has no other
  renderer. Here it must be asked for: `RSX_LIVE_DRAW=1` (his `YZ_RSX_DRAW`
  still works).
- **Presentation.** Upstream self-presents when it sees flip method `0xE944`,
  which his README notes was established empirically from Yakuza. This title
  flips through a different path, so presentation is driven from the same ~60Hz
  tick that used to drive the D3D12 backend.

`rsx_null_backend` gained `get_hwnd` and `suppress_present` (ported from his
fork) so the engine can bind a swap chain to the window the null backend opens
and silence the GDI blit underneath it.

### Where it stands

The engine is up, fed and presenting:

```
[rsx] live-draw engine up (D3D12); GDI present suppressed
[live] frames=2696 last_draws=0
[RSX] CLEAR_SURFACE mask=0xF0 color=0x00000000
[live-draw] frame 3 ... clears[guest=1 ...] groups[seen=0 exec=0 ...]
```

Frames advance, the method stream reaches it, and it processes the guest's
clears. **No draw groups form** (`groups[seen=0]`), so nothing is drawn yet. Its
own telemetry localises that precisely: over 150 seconds the feed carries
~16,375 `VERTEX_BEGIN_END` (0x1808) but only **4** `DRAW_ARRAYS` (0x1814) and no
index batches, so the begin/end pairs are arriving without the batch method that
would turn them into geometry. That is the next thing to chase, and the engine's
`[live-draw]` counters — packets, groups, drop reasons, clears, textures — are
the right instrument for it.

Two other things it reports that will need wiring: it falls back with *"flip 0
has no registered/rendered scanout"* because `rsx_live_draw_set_display_buffer`
is never called from this tree's `cellGcmSetDisplayBuffer`, and its target reads
`clip=0,0+720x480` — the video-out mode rather than the 1280x704 surfaces the
title actually renders into.

### Getting it to draw

The engine draws. Its own counters, after the menu is up:

```
packets[seen=191783 queued=191783]
groups[seen=73209 exec=73096 empty=18872 drop{fetch=0 degen=0 prim=112 pso=0 ring=0 surface=1}]
clears[guest=3135 badsurf=0]  textures[cached=22/1024 decodefail=6]
```

Three things had to be found to get here.

**The title draws indexed.** The first measurement said 16,375 `VERTEX_BEGIN_END`
and 4 `DRAW_ARRAYS`, which read as "no geometry ever reaches it". That run simply
ended before the menu: counting the whole boot shows `DRAW_INDEX_ARRAY` (0x1824)
is the dominant kick — 10,498 and climbing against 245 `DRAW_ARRAYS` — and
indexed draws only start once the UI comes up. `RSX_LIVE_FEED_DBG=1` prints the
three counts for any run, live engine or not.

**Presentation had to be driven.** Upstream self-presents on flip method
`0xE944`; this title flips elsewhere, so nothing ever presented and frames stayed
at zero. Driving `rsx_live_draw_present()` from the same 60Hz tick that used to
drive the old backend fixed that.

**Display buffers had to be registered.** The engine reported *"flip 0 has no
registered/rendered scanout"* and fell back to whatever surface was current,
because `cellGcmSetDisplayBuffer` only ever told the old backend.
`rsx_live_draw_set_display_buffer` is now called alongside it, and the engine
picks up both buffers:

```
[live-draw] display buffer 0 = loc0:0x00010000 pitch=5120 720x480
[live-draw] display buffer 1 = loc0:0x00556000 pitch=5120 720x480
```

The engine also honours the guest's clears with the guest's own colours —
dumping its tracked surfaces mid-run gives `0x00AB0000` filled `(253,98,98)` and
`0x01BE0000` filled `(16,164,0)`, both 1280x704, both exactly what the title
cleared them to. `rsx_live_draw_debug_dump_surface` writes PPM, not BMP.

**Where it stops.** `empty=12187` of `13480` groups. The engine forms a draw
group per `VERTEX_BEGIN_END` pair and most of them carry no geometry, which
matches the raw method counts — far more begin/end pairs than batch methods. So
the surfaces clear correctly and a minority of real draws execute, but the bulk
of the title's geometry is not arriving as the engine expects it. `drop{fetch=0}`
says this is not vertex fetch failing; the groups are empty before that.

That is the next thing to chase, and it is a good place to be: the question has
moved from "why is everything black" to "why do most begin/end pairs carry no
batch", which the engine's own counters can answer.

### Visible geometry

```
[live] pre-present dump (last_draws=535)
surf_00AB0000.ppm   1280x704  distinct=2  non-dark=49.93%
                              (255,255,255) x112490   (0,0,0) x112790
```

A large white polygon covering the lower half of the surface with a clean
slanted edge. Not a clear — a clear is uniform, and every earlier dump was
`distinct=1`. Geometry from the title's own draw stream is rasterising with a
working vertex transform.

Two things were wrong, and both were about *when*, not *what*.

**Presenting on the wrong clock.** Presentation was being driven from the 60Hz
ticker, independent of the guest. `cellGcm_take_flip_pending_synced()` is true
only once the drain has consumed everything up to `put` — i.e. the FIFO holds
exactly one completed frame — so presenting on that instead took
`last_draws` from **0 to 910**. Before this, every presented frame was one the
guest had not finished.

**Sampling on the wrong clock too.** Every surface dump was taken after the
present, by which point the guest had begun the next frame and cleared. That is
why surfaces kept reading back as flat clear colours — `0x00AB0000` as
`(253,98,98)`, `0x01BE0000` as `(16,164,0)` — which looked like "the clears work
and nothing draws" when it was really "you are looking between frames". Dumping
*before* the present, on a frame carrying real geometry, shows the geometry.

Getting the trigger right took several attempts worth recording, because each
failure looked like a black surface rather than a missed sample: a wall-clock
deadline lands after the guest has stopped flipping; a flip counter catches
frames holding a single draw. What works is "dump the first presented frame that
carried at least N draws" — `TM_LIVE_DUMP=250`.

The per-draw CSV (`YZ_RSX_DRAW_CSV=draws.csv`) says the pipeline state is sound:
**16,937 draws, every one `outcome=execute`**, 16,498 of them into `0x00AB0000`,
colour mask `0x01010101`, viewport 1280x704, vertex counts of 66/30/48/21/57 —
no drops for fetch, degenerate primitives, PSO or ring exhaustion.

What is still wrong is shading: the geometry is flat white rather than textured
UI art. That is the next thing — the geometry, the transform, the surfaces and
the frame timing are now all doing what they should.

### Shading: one fix, and where it still falls short

**Texture memory was resolving to the wrong place.** The engine asks its host to
translate `(location, offset)`, and `location` comes straight from the RSX DMA
context selector, so it is authoritative. The callback was answering with
`cellGcmResolveOffset()`, which *prefers VRAM for any page the guest ever derived
from a local EA* — a heuristic that is right for a title keeping its data there
and wrong for this one. Main-memory textures were landing in local memory:

```
before   loc=1 off=0x0F784780  ->  ea=0xCF784780      (local VRAM)
after    loc=1 off=0x08BA4800  ->  ea=0x19BA4800      (main, via the IO table)
```

The callback now honours `location`: local goes to `localAddress + offset`, main
goes through `cellGcmResolveIO()` — the IO-table-first resolver written for
exactly this case — and only falls back when the page is unmapped. The
1024x1024 B8 font atlas at `0x0AB55580` now binds and refreshes every frame:

```
[tex-refresh] n=1 frame=912 unit-src=0:0x0AB55580 fmt=0xA1 1024x1024
```

**What is still flat.** Geometry rasterises but comes out untextured white on the
frames sampled so far. The engine falls back to a 1x1 white texture whenever a
texture is unavailable, which is exactly that colour, and its cache reports a
handful of decode failures on formats it does not implement:

```
[texture-cache] decode failed src=0:0x01BE0000 fmt=0xFA 1280x704 pitch=10240
[texture-cache] decode failed src=0:0x022D0000 fmt=0xF2 1472x1472 pitch=3072
[texture-cache] decode failed src=1:0x00FDF280 fmt=0xBF 1024x1
```

`0xFA` masks to `0x9A` — `W16Z16Y16X16` half-float — and `0x01BE0000` is one of
the title's own render targets being sampled as a texture. So the UI composites
through half-float render targets, and that is the format the decoder does not
cover. `0xF2` and `0xBF` are likewise outside the supported set
(`B8, A1R5G5B5, A4R4G4B4, R5G6B5, A8R8G8B8, DXT1/23/45, G8B8, DEPTH24_D8`).

Sampling is also frame-dependent: one dump at 535 draws showed the polygon,
another at 1350 showed nothing, so which frame a single readback lands on
matters as much as what the renderer does. `TM_LIVE_DUMP=<n>` now samples up to
five qualifying frames rather than one.

## What caner's Yakuza fork says about the renderer

[canersaka/Yakuza-Dead-Souls-EX](https://github.com/canersaka/Yakuza-Dead-Souls-EX)
is a fork of ps3recomp carrying a playable Yakuza: Dead Souls port. It is MIT
licensed and its copyright line is this project's own, and its README states
that game-independent fixes are prepared for submission upstream — so reading
and adopting from it is clean, with credit.

Two things from it matter here.

**The subchannel bug is confirmed, and it is not title-specific.**
`rsx_live_draw.c` masks the subchannel out of every method before dispatch:

```c
const u32 canonical = method & 0x1FFCu;   /* sub = (method >> 13) & 7 */
```

with the comment that *"if an SPU-built command list binds NV4097 on a different
subchannel, feeding the raw 0x2xxx-shifted method into the canonical dispatcher
silently stores the state in the wrong register bank."* That is exactly the
defect found here independently: the subchannel is a **binding slot, not an
engine selector**. Our `gcm_2d_method` only ever claims subchannels 2..7, so
everything Twisted Metal issued on subchannel 1 — `SET_SHADER_PROGRAM` included
— was dropped, leaving one stale fragment program bound for every draw in the
game.

That is now the default rather than an opt-in flag: subchannels the 2D path does
not claim are treated as 3D. `GCM_SUBCH1_2D=1` restores the old split. Distinct
fragment programs seen in a boot go from **1 to 14**.

**The bigger point: this backend is not the one that renders.** His README is
explicit —

> *"The newer live engine is separate from the older generic
> `rsx_d3d12_backend.c`, which still contains a placeholder shader path."*

He wrote a separate live NV4097-to-D3D12 engine to get Yakuza rendering:

```
rsx_live_draw.c       8351 lines
rsx_dispatch.c         516     NV4097 method dispatcher (clean-room, documented
rsx_vertex_compact.c   351     against envytools rnndb / Mesa nv30 / psdevwiki)
```

with per-shader PSO caching, guest vertex/index decode, primitive restart and
strip/fan expansion, render-target-as-texture sampling, texture decode and
remap, per-draw vertex constants, full blend/depth/stencil/cull/viewport/scissor
state, and GPU reference fences. None of those files exist in this tree.

That reframes the graphics work here. Rubber Ducky renders through
`rsx_d3d12_backend.c` because it asks very little of it — and the hardcoded
subchannel map in `cellGcmSys.c` is, by its own comment, the duck's layout. A UI
as involved as this title's is past what that backend does. The realistic route
to a picture is to bring the live engine across rather than keep repairing the
generic one; the shader translator underneath it is shared and already works,
which is why the fragment constants recovered here look sane.

## The subchannel map was tuned for Rubber Ducky

`libs/video/cellGcmSys.c` says so directly, in the NV309E state it keeps:

> *"Rubber Ducky binds it to the same subchannel this file previously …"*

The FIFO drain sends subchannel 0 to the 3D engine and **everything else** to
`gcm_2d_method`, whose branches are `2/3` (NV3062), `4/5` (NV308A/NV309E) and
`6/7` (NV3089). That mapping is one title's layout, and its own comment warns
the binding is libgcm-version-specific and that `SET_OBJECT` binds are not
tracked. Twisted Metal drives NV4097 on subchannel **1**, so every 3D method it
issued there — including `SET_SHADER_PROGRAM` — was handed to the 2D handler and
dropped. That is why one constant-black program was the only shader the renderer
ever saw.

With `GCM_SUBCH1_3D=1` the title's real shaders arrive, and so do their
constants:

```
[FPK] tex0=0x01870000 fp=0x00FDDC82 k0=(0.996094 0.00389099 1.51992e-05 0)
[FPK] tex0=0x00F6E680 fp=0x00FDBE82 k0=(0 0 0 1)
[FPK] tex0=0x00AB0000 fp=0x00FD5D02 k0=(0.0625 0 0 0)  k1=(255 0 0 0)
[FPK] tex0=0x01870000 fp=0x00FDB882 k0=(2 0 0 0)       k1=(0.5 0 0 0)
[FPK] tex0=0x01F50000 fp=0x00FDD682 k0=(-3 -0 0 0)     k1=(0 -0 3 0)
```

Plausible scale/bias constants, and draws now bind a variety of textures
including render targets sampled as textures — a real UI compositing pipeline
rather than one stale program.

The principled fix is to read the subchannel-to-engine map from the FIFO instead
of assuming it. A first attempt at that (`GCM_OBJDBG=1`, logging method 0 as
`SET_OBJECT`) does not work: the handles it prints are `0x44340000`,
`0x00040310`, `0x00000001` — not object handles, so method 0 is not decoding as
`SET_OBJECT` and the FIFO header walk needs work before that route is usable.

## What the profiler says about the hang

`PERF=1` across the moment the UI appears:

```
[PERF] 59.81 fps | tex 0 calls   | render_frame 0.29s (88%) | guest 0.04s (12%)
[PERF]  1.27 fps | tex 775 calls | render_frame 6.49s (41%) | pso 764 calls, 1.12s
                 | guest 9.32s (59%)
```

The frame cost explodes by two orders of magnitude when the UI starts, with 775
texture uploads and 764 pipeline-state creations in one interval. But uploads
are **not** the cost — `tex` is 0.01s for those 775 calls — and capping them
(`TEX_BUDGET`, added and left off by default) does not stop the TDR. PSO
creation is 1.12s of CPU. The GPU-side cost is inside `render_frame`, still
unattributed.

## Is anything actually visible?

No. Nothing has been shown on screen, and a run still presents a black window.
The decoded intro frame elsewhere in this file came out of the decoder before it
reached the RSX — it proves H.264 decoding, and nothing about display.

Two corrections to earlier claims in this file.

**Some readbacks measured the wrong surface.** At 40 seconds the draws target
render target `0x01BE0000`; the dumps taken at that point were of `0x00AB0000`,
which receives only *clears* then. "The render target reads back black" was
therefore partly a statement about a surface nothing was drawing to.
`0x00AB0000` is the right target later, at the menu — the two are not
interchangeable and were treated as if they were.

**There is now intermittent non-black output, and it has not been proven to be
a draw.** With `GCM_SUBCH1_3D=1`, readbacks of `0x01BE0000` around 55-58 seconds
twice came back with exactly 25.00% of the surface pure white — and 25% of
1280x704 is precisely the 640x352 draw viewport. The same readback without the
fix is uniformly black. But it is a transient: repeated captures at 50, 52, 55
and 58 seconds mostly return black, and the one control that would settle it —
the same moment with `FP_OFF=1` to remove the draws — did not reproduce the
white in its own baseline, so it decided nothing. A clear is scissored to the
viewport too, so viewport-shaped white is not by itself proof of a draw.
Forcing the clear colour green (`CLEAR_RGB=0,0.25,0`) left the white white, but
also left the surrounding clear black rather than green, so that override was
not reaching this surface and the test is not conclusive either.

What can be said: before the subchannel fix every draw ran a one-instruction
program whose constant was sixteen zero bytes, so black was the arithmetically
correct output. After it the title's real shaders reach the renderer, and
non-black pixels appear in exactly the right rectangle at least sometimes.
Whether they are draws, and why they do not persist, needs the GPU hang fixed
first — the device is removed between 40 and 68 seconds, which is both why the
menu can never be sampled and why every attempt here is a race against a
closing window.

## Why everything renders black: one fragment program

The menu draws 100,816 textured quads with a live glyph atlas into a render
target that reads back black. The reason turns out to be short.

Every draw in the run used the **same** fragment program, `0x00EC2B02`. Dumping
its ucode from guest memory and decoding the first instruction by hand:

```
1E 81 01 40  00 02 1C 9C  C8 00 00 01  C8 00 00 01
w0 = 0x01401E81   FP_END set        -- a one-instruction program
w1 = 0x1C9C0002   reg type 2 = CONST -- its operand is an inline constant
```

One instruction, output a constant — which is exactly what the decompiler
emitted (`h[0] = fp_k[0].xxxx`). And the constant that follows it in the ucode
is sixteen zero bytes. So every draw in the game output `(0,0,0,0)`. The screen
was black because the shader said black.

The title does not really have one fragment program. `NV4097_SET_SHADER_PROGRAM`
(method `0x08E4`) was **being thrown away**, because it arrives on RSX
subchannel 1 and the FIFO drain routes everything with `subch != 0` to the 2D
engine handler, which silently discards what it does not recognise. That is the
defect recorded earlier in this file from a `GCM2D_TRACE` histogram — subchannel
1 carrying NV4097 methods — and it was dismissed on a test that measured the
wrong surface at a point in the boot where the title had not even reached its
menu. The test was worthless; the finding was right.

`GCM_SUBCH1_3D=1` routes subchannel 1 to the 3D engine as well. With it:

```
[FPK] distinct fp #1 = 0x00EC2B02      <- the only one without the fix
[FPK] distinct fp #2 = 0x00FDDC82
[FPK] distinct fp #3 = 0x08BA4782
...  ten and counting
```

The renderer is now being given the shaders the title actually programmed,
instead of one stale default. This is the single biggest correctness fix to the
graphics path so far, and it was found by asking a question that should have
been asked much earlier: not "why is the surface black" but "what colour does
the shader compute".

The proper fix is to track `SET_OBJECT` binds so the subchannel-to-engine map is
read from the FIFO rather than assumed. `GCM_SUBCH1_3D` is the heuristic version
of that and is what every run should now use.

### The remaining blocker is a GPU hang

The device is removed with `DXGI_ERROR_DEVICE_HUNG` — a TDR — and it now has a
window: a readback of the menu render target succeeds at **40 seconds** and
fails at **68 seconds**, so the hang lands while the UI first renders in
earnest. It is not draw volume (`DRAW_LIMIT=32` changes nothing), not a shader
loop (every `for` in the dumped VP and FP HLSL is `[unroll]` with a fixed trip
count), and the D3D12 debug layer reports no validation error before the
removal. Until it is fixed, no readback of the menu can be taken after the
menu exists, which is why there is still no screenshot of it here.

### Narrowing the GPU hang

The TDR is what now stops everything, so it is worth recording exactly what it
is and is not. One measurement note first: `887A0005` in a log is **not** a
reliable count of device removals — it is the code the *PSO creation* failure
reports once the device is already gone, so a configuration that builds no
guest PSOs shows zero occurrences whether or not the device died. The signal to
grep for is `DEVICE_HUNG` / `RemoveDevice` from `D3D12_IQ=1`.

With that:

| configuration | `DEVICE_HUNG` |
|---|---|
| default | 1 |
| `FP_OFF=1` (guest-FP draws skipped entirely) | **0** |
| `VP_BYPASS=1` | 1 |
| `DRAW_LIMIT=4` | **0** |
| `DRAW_LIMIT=8` / `16` / `32` / `64` | 1 |
| `FP_KILL=<addr>` for six different programs | 1 each |
| `DRAW_SKIP_TEX=2ACD800` | 1 |

So it is cumulative work in the guest-FP draw path, not one shader and not one
texture. Every `for` in the dumped VP and FP HLSL is `[unroll]` with a fixed
trip count, no program decodes to an implausible size (1-75 instructions), and
the debug layer reports no validation error before the removal. A readback of
the menu render target succeeds at 40 seconds and fails at 68, so it lands while
the UI first renders in earnest.

That last point is also why there is still no screenshot of the menu in this
file: every readback taken after the menu exists fails, because
`CreateCommittedResource` for the readback buffer returns failure once the
device is gone.

## Reaching attract mode

```
[attract] world ctor #1: factory 0x00F06F98 -> 0x00EFD8A8
[trace]   -> attractScript(0x014BE734, ...) from 0x003A8780
[trace]   <- attractScript = 0x15B5C560
[game] GeomPageMgr::init(UI World Main, 21474236)
[game] VehicleMgr::loadNewVehicle() called, requesting dbId=910, for pi=0
[game] setReservedSlotId() time=27250 player 0 addr=1667b00 setting reserved slot ID=255
[game] VehicleMgr::loadNewVehicle() called, requesting dbId=901, for pi=1
...                                                          pi=2 .. pi=10
[game] GeomPageMgr::init(vehicleMainMemstack, ...)
[game] GeomPageMgr::init(fodderCarMainMemstack, ...)
[game] GeomPageMgr::init(semiTrailerMainMemstack, ...)
```

`AttractModeScript::create` runs, its world is constructed, and the script
populates it with a full AI roster — eleven players, vehicle database ids 901 to
919 — and brings up the vehicle, traffic-car and semi-trailer geometry pools.
`VehicleMgr::loadNewVehicle` appears **44 times** here and **zero times in every
other run in this repo's history**, including the nine-minute menu idle and the
forced `WorldLoader::loadGame` runs. Nothing in the UI path loads a vehicle;
this is the game world.

### How

Three things had to line up, and the first two are recorded in the sections
above: the network unblock to get out of the legal screens, and the discovery
that `WorldLoader::loadGame` takes a **script factory function pointer** rather
than a world name.

The third is where the factory is consumed. `loadGame` only *stores* it, at
`+0x3F8` of the world object; `func_0020C7F8` later reads that slot and passes
it as the **seventh argument (r9)** to `func_003A82C8`, the world constructor.
Forcing `loadGame` with the attract descriptor does not work — by the time it
runs, the world already in flight was constructed from a factory captured
earlier, which is why `func_000120C4` stayed at zero calls through all of that.

Substituting at the point of use does work. `TM_ATTRACT=<n>` replaces `r9` with
`0x00EFD8A8` — the `.opd` descriptor of `AttractModeScript::create` — on the
nth world construction:

```
TM_ATTRACT=1 FP_OFF=1 GCM_SUBCH1_3D=1 FLOW_CONDKICK=1 \
TM_SKIP_BANKUNLOAD=1 TM_LOADDONE=140 ./build/twistedmetal.exe input/EBOOT.ELF
```

`FP_OFF=1` is there because the GPU TDR otherwise removes the device around 40-68
seconds and the frame loop then crawls; with the guest-FP draws skipped the run
is stable long enough to get here. So this reaches attract mode's *simulation*
— world, script, vehicles, players — while the picture still waits on the
renderer work described above.

## The attract world: how it is selected, and how far it now gets

Attract mode is not a UI state and not a data script. It is a **world script**,
and the machinery around it is now mapped.

`func_000120C4` is `AttractModeScript::create()` — it allocates a 0x1D28 object
through the allocator that takes a file and line (`attractModeScript.cpp:45`),
constructs it, stores vtable `0x00CE3360` at offset 0, and sets a script-type
global at `0x0119C508` to 2. `func_002B8F38` has the identical shape and is the
factory the title normally uses, so this is a family: `CampaignScript`,
`CinematicScript`, `VideoScript`, `GameModeScript`, `UiScript` and the rest.

`WorldLoader::loadGame` takes **the factory itself** as its first argument. The
`0x00F01FC8` the game passes is not a world name — it is a PPC64 function
descriptor in `.opd` whose entry is `func_002B8F38`. So the attract world is
the same call with the descriptor of `func_000120C4`, at `0x00EFD8A8`.

`TM_FORCEWORLD=<seconds>[,game|,direct|,attract]` performs the transition the
movie itself makes when it ends. `func_001DB6B8` — the movie object's update —
finishes with

```
if (*(u8*)0x00F3810C) func_0010EC08(*(u32*)0x00F4A248);   /* back to the UI */
else                  func_0010EB0C(*(u32*)0x00F4A248);   /* load the world  */
```

and `func_0010EB0C` is the only caller of `WorldLoader::loadGame`. With
`,direct` and `,attract` that call now runs, and **`WorldLoader::loadGame`
completes for the first time** — it had been at zero calls through every
investigation in this file:

```
[forceworld] WorldLoader::loadGame(0x00EFD8A8) = AttractModeScript::create
[game] 191878 ms WorldLoader::loadGame
[game] CommonBank::Unload() : Unloading "shell"...
[game] CommonBank::Load() : Loading bank "menu"... 91328 bytes
```

Two dead ends recorded so the next attempt does not repeat them. Going through
the dispatcher (`,game`) does not reach `loadGame`: `func_0010EB0C` switches on
a level type at `mgr+0x5454` and only 2, 3 or 4 fall through to it — the
comparisons are signed and against 6, 4, 1 and 8, so 1 returns — and the menu
holds none of those, while `func_003C2AD4` runs before the value is read. And
the gate at `0x01541648` reads `0xFFFFFFFF`, which is negative, so the
first-branch shortcut is never taken either.

### Where it stops

`loadGame` only *stores* the factory, at `+0x3F8` of the world object. The
script is instantiated later: `func_0020C7F8` reads that slot and passes it as
the seventh argument to `func_003A82C8`, the world constructor. In a forced run
`func_0020C7F8` executes 28,666 times and `func_003A82C8` twice, but
`func_000120C4` is never entered — so the world constructed is not the one whose
factory was just stored. The remaining step is to make the world construction
run against the stored factory rather than the one already in flight.

## Getting past the intro, and where attract mode actually lives

Three findings, and together they say the intro is not what stands in the way.

**`st_intro.avi` is 565 MB.** The AVI header reports `end: 28282500`, which read
as microseconds is 28 seconds — it is not. The file is half a gigabyte, and at
the seven frames a second this port sustains it cannot be watched to its end.
`TM_FSEOF=<fd>,<bytes>` truncates a descriptor so the demuxer sees EOF early;
the decode stops where told, but the player does not treat a short read as
end-of-movie. `cellVdecEndSeq` is never called and the cinema keeps polling the
movie object (`func_001DB6B8`, which stays 0), so the title waits for a movie
that will not finish.

**There is no attract state to reach.** The binary holds 61 `Ui*` class names
and 93 UI-state vtables, and none of them is an attract or title state:

```
UiLegal_HealthWarning -> UiLegal_1..4 -> UiNetShutdown -> UiMainMenu
```

`UiMainMenu` *is* the startup screen on this build. Attract mode is not a state
at all — it is `attractModeScript.cpp`, a script, and the binary has a `UiScript`
class to run it. It would be started from the main menu on an idle timer.

**Which means attract needs the menu, and the menu needs the renderer.** Idling
nine minutes in `UiMainMenu` with the decoder working produces no attract, no
`WorldLoader::loadCinema` and no `MoviePlayer::openFile`. That is consistent
with everything else: the menu draws 100,816 quads with a live glyph atlas into
a render target that reads back black, and it takes no pad input.

The GPU side is worth stating precisely, because it is now the common blocker:

- The device is removed with `DXGI_ERROR_DEVICE_HUNG` — a TDR — in **every**
  long run, including menu-only runs that never touch the movie path.
- It is not draw volume: `DRAW_LIMIT=32` changes nothing, still 16 removals.
- It is not a shader loop: every `for` in the dumped VP and FP HLSL is
  `[unroll]` with a fixed trip count.
- It lands 26-88% of the way through a run, so the black menu is *not*
  downstream of it — the menu is already black long before.

So the order of work is: fix the D3D12 backend, and the menu, the input, the
attract script and a watchable intro all follow from it. The video decoder is
done and is not the thing holding this up.

## Decoding it: H.264 on the Media Foundation MFT

The intro decodes. A frame pulled straight out of the decoder is the real
thing — the Calypso Industries tape recorder on gravel under a storm sky,
correct colours, correct geometry, legible text.

ps3recomp's `cellVdec` accepts access units and decodes nothing, so the work was
a decoder plus the four things standing between the title and it.

**The `Ex` entry points.** ps3recomp registers `cellVdecOpen`, `DecodeAu`,
`GetPicture` and friends; the title calls the *Ex* variants, which nothing
registered. `cellVdecQueryAttrEx` therefore returned an unresolved-NID error,
the game read a memory requirement of zero — `Allocating 0 k for Vdec` — and
never opened the decoder, after which `cellVdecStartSeq` failed with
`CELL_VDEC_ERROR_ARG`. The NIDs came out of the import table rather than a
database: stubs are laid out per module and sorted by NID, so the unregistered
entries sitting *inside* the cellVdec and cellAdec runs are the missing ones.

```
0xC982A84A cellVdecQueryAttrEx    0x0053E2D8 cellVdecOpenEx
0x7E4A4A49 cellAdecQueryAttrEx    0x8B5551A4 cellAdecOpenEx
```

**Handle zero.** With the decoder open the title still never asked it to decode
anything. It stores the vdec handle at `+0xC0` of its video-stream object and
treats zero as "no decoder": `func_0079A634` returned status 155 without ever
calling `cellVdecDecodeAu`, so the AVI streamed, the demuxer ran, audio flowed,
and the video decoder sat idle. Slot 0 is a perfectly legal index and a useless
handle. Handles are now `0x0DEC0000 + slot`, mapped back on entry.

**The audio handshake.** The player waits for every stream to report ready
before it starts, so with AC3 unimplemented it printed `MoviePlayer SyncLoaded
timed out!` forever. `cellAdec` now accepts AUs and answers `GetPcmItem`/`GetPcm`
with silence. There is no audio; there is a video.

**Two misdeclared APIs.** ps3recomp gives `cellVdecGetPicture` the signature of
`cellVdecGetPicItem` — the real one is `(handle, const CellVdecPicFormat*, u8*
outBuff)` — and its `CellVdecPicItem` is an invented layout rather than the PS3
one. Both are re-implemented in `src/vdec_hle.cpp` against the real ABI.

Decoding uses the stock **Microsoft H264 Video Decoder MFT**, an OS component,
so there is no new dependency — `mfplat`/`mfuuid` ship with Windows. The title
feeds Annex-B access units (`00 00 00 01 41 ...`), the MFT returns NV12, and
`cellVdecGetPicture` converts to whatever the title asks for. It asks for
RGBA32 at 1280x720 and takes delivery straight into RSX local memory.

### What actually happens on screen

The pipeline is complete and every stage was measured:

```
cellFsRead  32 KB at a time, position advancing   (TM_FSREADS=52)
DecodeAu=111 PICOUT=111 GetPicItem=111 GetPicture=111   -- balanced, no drops
frame -> local memory 0x0DFCD880
4891 draws bind tex0=0x0DFCD880 into RT 0x01BE0000
[NV3089] 1024x352 src=0xC1BE0000 -> dst=0xC0026A80    -- composited to the display buffer
```

So the title decodes the cinematic, uploads it, draws it and composites it. It
sustains about 7 frames a second for roughly fifteen seconds — a little over a
hundred frames of the intro — and then the **GPU device is removed**:

```
[D3D12-IQ] RemoveDevice: DXGI_ERROR_DEVICE_HUNG -- the TDR mechanism has been triggered
[D3D12] wait_for_gpu STUCK 2s: want 25758 got 25756
PSO FAIL (fp=0x08BA4702, 0x887A0005)
```

after which every D3D call fails and the frame loop crawls. That TDR is **not
the movie's doing** — it happens just the same in menu-only runs with the movie
path never touched (`887A0005` appears 6-16 times in runs with zero
`GetPicture` calls). It is the same D3D12 backend that renders the menu black,
and it is the next thing to fix. The readback paths that would have confirmed
the picture on screen fail for the same reason: `CreateCommittedResource` for a
7 MB readback buffer returns failure because the device is already gone.

## Driving the title to the intro: `UiMoviesMenu::onSelect(st_intro.avi)`

The menu cannot be operated — it renders black and takes no pad input — so the
movie path was exercised by driving the state machine directly. Two knobs do it,
both issued from inside `UiState::update` so the transition happens on the UI
thread with a live stack:

```
TM_FORCESTATE=<vtable hex>[,seconds]   enter the state built on that vtable
TM_FORCECALL=<guest addr>[,seconds]    then call that function on the state object
```

Finding the target was a matter of asking which UI states reach
`WorldLoader::loadCinema`. Two do, and their vtables name them:

```
vtable 0x00CEF810  onEnter 0x004822E8  slot7 0x0048266C -> loadCinema
vtable 0x00CF1420  onEnter 0x004D30D8  slot7 0x004D44E0 -> loadCinema
```

`TM_FORCESTATE=00CEF810` lands on a live state object at `0x180CF7D0`, and the
strings it immediately looks up say exactly what it is:

```
strTable(0x2B18) = 'Sweet Tooth'
strTable(0x2B19) = 'Mr. Grimm'
strTable(0x2B1A) = 'Dollface'
strTable(0x0CB4) = 'PREV/NEXT'
strTable(0x0C80) = ' SELECT'
strTable(0x0C94) = ' BACK'
```

Then `TM_FORCECALL=0048266C` runs its select handler, and the title says:

```
[game] UiMoviesMenu::onSelect(st_intro.avi)
[game] 170101 ms WorldLoader::loadCinema
[game] CommonBank::Unload() : Unloading "shell"...
```

**It names the intro cinematic and loads the cinema world.** That is the movie
path running for the first time — `WorldLoader::loadCinema` had been at zero
calls through every previous investigation.

One gate had to be released to get that far: the load-complete byte at
`0x0190FC89`, which the cinema path re-clears, so `TM_LOADDONE` now writes it on
every tick rather than once.

### Where it stops

`WorldLoader::setup` is entered and never returns. The chain, each step
confirmed by trace rather than by reading:

```
WorldLoader::setup 0x0020C698
  -> 0x003C24B4
    -> 0x003A8A20
      -> 0x003A87C4
        -> virtual call on *(this+0x144) slot 5  =  0x004BDA30
          -> 0x006107E8
            -> 0x00606F78
              -> 0x00606CE4      <-- never returns
```

`func_00606CE4` gets as far as `0x0060E500` (returns 0) and then blocks. It does
not reach any of its FIOS callees (`0x0076B66C`, `0x00361750`) — those are traced
and never fire. The main thread reports no hot-read spin, so it is blocked in a
synchronisation primitive rather than polling, while three FIOS worker threads
spin at `0x007714FC` on flags at `0x4000186C` / `0x4003263C` / `0x40043C6C`
waiting for work that is never posted. Those spins do not occur in a normal run.

That shape — a loader blocked on a wait while its workers idle — is the same
class of defect as the FIOS scheduler constructor bug fixed earlier, where
`find_functions` truncated `func_0076C534` and the workers were never built.

## The menu's textures are fine (and how that was nearly missed)

The menu draws sample a 1024x1024 B8 atlas at raw offset `0x0AB55580`. Read as a
main-memory address that buffer is **entirely zero** — 0 of 4096 bytes — which
looks exactly like "the artwork never arrived" and matches a black render
target. It is the wrong address. The same raw offset resolved as RSX *local*
memory (`0xCAB55580`) holds real data, and `TEX_SAVE=1` shows Scaleform filling
it as the menu builds:

```
[TEXB8] off=0xCAB55580 1024x1024 nz=348/61680  min=0 max=255
[TEXB8] off=0xCAB55580 1024x1024 nz=2274/61680 min=0 max=255
[TEXB8] off=0xCAB55580 1024x1024 nz=6737/61680 min=0 max=255
```

That is a glyph atlas being rasterised. The backend already resolves this one
correctly through the format's location bits, so the texture path is **not** the
bug — but the near-miss is worth recording, because "the texture reads as zero"
was about to become the fourth wrong conclusion in this file. A raw RSX offset
means nothing without its location.

Four textures in this title *are* mis-resolved, and a probe finds them:
`TEX_PROBE=1` resolves the offset both ways when the chosen one samples empty
and the other does not.

```
[TEX_PROBE] 0x01190000: local empty, using main (586 non-zero)
[TEX_PROBE] 0x01191400: local empty, using main (586 non-zero)
[TEX_PROBE] 0x01F50000: local empty, using main (259 non-zero)
[TEX_PROBE] 0x01F51400: local empty, using main (269 non-zero)
```

These are the mirror image of the case `TEX_RESOLVE_AUTO` was written for: the
guest tags them local and built them in main memory.

So at the menu the assets are present, the atlas is live, the geometry is
submitted (100,816 draws) and render target `0x00AB0000` still reads back black.
That puts the remaining fault in the vertex/fragment path — which is exactly
where [the earlier investigation](#the-draws-were-rendering-the-whole-time) left
it, with the difference that it can now be reproduced on the surface that
actually matters instead of on a 68-draw side surface.

Three instruments were added along the way and are worth keeping:

| Knob | What it does |
|---|---|
| `TM_MEMDUMP=<hex ea>[,n]` | prints `n` guest bytes once a second with a non-zero count |
| `RTT_SAVERT_AFTER=<secs>` | triggers the RT dump on wall time, not a frame number |
| `TEX_PROBE=1` | picks the texture resolution that actually has content |

## Three instruments that were lying

Worth recording, because each one produced a wrong conclusion that survived for
a long time.

**Strings read as ASCII.** The title stores localised text as UTF-16BE. `tm_gstr`
read it as ASCII, stopped on the leading zero byte, and reported every string as
empty — which made `Movie::c('', '')` look like a movie player being handed a
nameless file. It is really `Movie::c("EXIT GAME NETWORK", "Disconnecting from
the game network ...")`, a dialog, and the function is not a movie player at all.

**`%^` parsed as a conversion.** The Ui state machine formats
`"!@#$%^&* %s::onEnter() triggered at [%f]"`. The log renderer scanned forward
from `%` to the next conversion character, walked over `^&* `, and consumed the
class name as the argument — so every transition printed as a bare number and no
state could be identified by name. Only accept a real flag/width/length run
after `%`; anything else is literal.

**Vtable slots read as code addresses.** They are PPC64 function descriptors.
Dereferencing them is what turned `vtable=0x00CEF690 [ 00F06528 ... ]` into
`[ 0047F8F8 ... 0047EC50 ]` and identified the final state as `MainMenu`.

With those three fixed, `func_004AA780` — labelled `IntroMovie::onEnter` in
`post_lift.py` and treated as the intro path in everything above — turns out to
be `UiNetShutdown`. There is no evidence the intro movie state was ever entered.

## Finding a message when the cross-reference cannot

Almost none of this was reachable by reading the lifted C++. String addresses in
this binary are formed with `lis`/`addi` off a base register held across many
uses, so grepping for a string's low half-word finds nothing, and the two
messages that mattered most — the rwlock warning and `Failed to load` — never
matched.

What worked was finding the *logger*. `func_0034ACAC` was already known;
`func_00980B20` is a second one with the same `log(level, fmt, ...)` shape, and
everything FIOS, the ArchiveLoader and the WorldLoader say goes through it. One
override turned a silent boot into a running commentary, including the exact
failure and the archive it was reading. `TM_GAMELOG=2` additionally prefixes
each line with `lr-4`, the guest address that logged it, which locates any
message in a stripped binary without a cross-reference at all.

---

*Part of the [Twisted Metal static recompilation](../README.md) working log.*
