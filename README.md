# Twisted Metal — Static Recompilation

> Turning *Twisted Metal* (2012, Eat Sleep Play / SCEA, `BCUS98106`) from a PS3
> disc binary into a native Windows executable — no emulator underneath.

The 2012 *Twisted Metal* was the series' last entry and its only PS3 outing:
David Jaffe and Scott Campbell's vehicular combat revival, built by Eat Sleep
Play, published by Sony, never ported anywhere else. When the servers went dark
the online half of the game went with them; the disc is the only copy that
still exists.

This project takes the disc's own `EBOOT.BIN`, disassembles every PowerPC
function, lifts them to C++, and links the result against
[ps3recomp](https://github.com/sp00nznet/ps3recomp) — clean-room HLE runtime
libraries that stand in for the PS3 operating system. Same approach as
[rubberducky](https://github.com/sp00nznet/rubberducky) and
[tokyojungle](https://github.com/sp00nznet/tokyojungle).

**You supply your own disc.** No game binary, asset, or key is committed here.

## Status

It builds and boots real game code — CRT, memory, `cellGcmInit`, video-out,
display buffers, RSX submission with a D3D12 window open — brings the engine's
FIOS file-I/O scheduler up, installs its game data, initialises Sony's
BoomRangBuss audio middleware, opens 186 asset files off the disc, and reaches
`WorldLoader::loadUi`, where it decompresses the UI archive and loads the shell
sound bank. The screen is still dark: the title holds a black fade while its
load bar is up, and the load has not finished.

| Phase | State |
|---|---|
| Disc inventory | **done** — `BCUS98106`, disc game, one game-side PRX |
| Disc decryption | **done** — `tools/decrypt_iso.py`, only needed for a raw dump |
| `EBOOT.BIN` → plain `EBOOT.ELF` | **done** — `tools/decrypt_self.py`, 16.3 MB ELF |
| Import / NID analysis | **done** — 439 imports, 34 libraries, 376 resolved (86%) |
| Function boundary detection | **done** — 31,032 functions, every `.opd` address verified |
| SPU image extraction | **done** — 11 embedded SPU ELFs, 1.17 MB |
| SPU lifting | **done** — all 11 lifted and registered; dispatch hits |
| PPU lifting | **done** — 35,635 functions emitted, 4.47 M lines of C++ |
| Build & link | **done** — 106 MB x86-64 exe, clang-cl 21 + Ninja, 6 warnings |
| Boot | **reaches the UI load** — 186 files opened, audio middleware up |
| Asset decompression | **works on the host** — 192 MB, ~1.8 ms per 64 KB block |
| Front end | **starts** — Scaleform UI loads, `UiLegal_1::onEnter` reached |
| Graphics (RSX → D3D12) | **presents** — clear reaches the swapchain; title draws a black fade |
| Audio / input | **audio initialises** — BoomRangBuss 1.0.33, banks load; input not started |

### The binary

```
EBOOT.BIN    16,309,120 bytes   SELF v2, key revision 0x16, self_type APP
                                auth_id 0x1010000001000003, firmware 3.73
EBOOT.ELF    16,306,688 bytes   ELF64 big-endian PowerPC64, ET_EXEC
                                entry 0xF030D0 -> OPD { func 0x348DE0, toc 0xF21930 }
                                8 program headers, 39 sections
```

Retail, so it is stripped — no `.symtab`, and the SELF does not carry
`.shstrtab`, so sections come out unnamed. Function recovery therefore leans on
the OPD table plus ps3recomp's prologue/leaf/extent heuristics:

```
opd scan          14,609 descriptor code addresses
prologue pass     15,658
leaf pass         22,791
seed pass         30,428
call-target pass  30,990
branch-target     31,032   <- final, every .opd address verified as a start
disassembled       3.87 M instructions across 1 executable segment
```

### Imports

439 imported functions across 34 libraries. The shape of the game is visible
straight from the list — a networked, SPURS-heavy, voice-chat-era retail title:

| Area | Libraries |
|---|---|
| Online / PSN | `sceNp` (47), `sys_net` (23), `sceNpTus` (19), `cellVoice` (15), `sceNpTrophy` (11), `sceNpCommerce2` (10), `cellNetCtl` (6), `cellHttp` (6), `cellSsl` (3), `sceNpUtil` (3), `sceNp2` (2), `cellHttpUtil` (2) |
| SPU | `cellSpurs` (46), `cellSpursJq` (16), `cellSync` (3) |
| Core | `sysPrxForUser` (43), `cellSysutil` (35), `sys_fs` (27), `sys_io` (12), `cellGame` (10), `cellSysmodule` (7) |
| Graphics | `cellGcmSys` (28), `cellSysutilAvconfExt` (1) |
| Audio / video | `cellFont` (16), `cellAudio` (11), `libvdec` (8), `cellAdec` (8), `cellMusicUtility` (7), `cellRtc` (6) |
| Misc | `cellUserInfo` (2), `cellL10n` (2), `cellPrx_PsnEula` (2), `cellFontFT` (1), `cellSysutilNpEula` (1) |

376 of the 439 NIDs resolve against ps3recomp's database. The 63 that do not are
concentrated in `sceNpTus` (10), `cellSpursJq` (10), `cellSysutil` (10) and
`sceNp` (6) — mostly online plumbing for servers that no longer exist, which is
the cheapest category to stub.

### SPU

Eleven SPU images are embedded in the PPU binary, 1.17 MB of SPU code in total,
sizes from 29 KB to 168 KB. `cellSpurs` + `cellSpursJq` in the import list says
they run as SPURS jobs rather than raw SPU threads. Identifying what each one
does — physics, particles, audio, culling — comes before any decision to HLE
them or run them through ps3recomp's SPU lifter.

### Lifting

```
30,828 functions lifted from the 31,032 detected
 4,807 mid-function tail-entry wrappers (5 passes to a fixed point)
35,635 functions emitted, 16,531 unique call targets
 4.47 M lines of C++, 295 MB, split across 8 translation units
     4 functions with no continuation at a mid-function entry -> halt
```

`--code-end 0xC79C6C` stops the lifter at the top of the import stub section,
so `.rodata` beyond it stays data instead of being disassembled as code.
`--toc 0xF21930` comes from the entry OPD.

### HLE coverage

Of the 439 imports, **231 (53%)** already have a handler in ps3recomp's runtime.
The gap is where the porting work is:

| Library | Have | Missing | Note |
|---|---|---|---|
| `sceNp` | 11 | 36 | dead servers — stub |
| `sysPrxForUser` | 12 | 31 | core CRT/threading — real work |
| `sys_net` | 2 | 21 | dead servers — stub |
| `sceNpTus` | 3 | 16 | dead servers — stub |
| `cellSpurs` | 31 | 15 | SPU job scheduling |
| `cellSpursJq` | 2 | 14 | SPU job queue |
| `sys_fs` | 14 | 13 | real work |
| `cellSysutil` | 24 | 11 | mixed |
| `cellGcmSys` | 25 | 3 | graphics, nearly there |

Everything else is one or two functions short. Roughly half the missing count
is online plumbing for servers that were shut down, which can return an error
and be done with.

### First boot

```
[ppu] loaded 2 PT_LOAD segments, entry OPD 0x00F030D0
[crt] sys_initialize_tls: block 0x0E000000, r13=0x0E007000
Available Main Ram at start of RTApp::initHardware 192(201326592)
IoMem: 0x11000000 167MB
[HLE] _cellGcmInitBody(cmdSize=0x10000, ioSize=0xA700000, ioAddr=0x11000000)
[cellVideoOut] Configure: resId=4 -> 720x480, pitch=5120
[cellGcmSys] SetDisplayBuffer(0/1) / SetTile / SetZcull / SetVBlankHandler
[rsx] backend init OK -- window open
FileIO initialized with data root = /dev_bdvd/PS3_GAME/USRDIR
[SYS] sys_ppu_thread_create name="fios mediathread 2..10"   (7 of them)
[cellSpurs] CreateTask REJECT no-init (taskset=0x11ECF180 elf=0x00D63C00)
Twisted app terminated! ReceivedExitGameRequest(False)
```

The game gets its own engine up: `RTApp::initHardware` runs, GCM is initialised
with a 167 MB IO region, the video mode is negotiated, display buffers, tiles
and zcull are registered, vblank and user handlers are installed, the D3D12
backend opens a window and receives real RSX commands, and FIOS mounts the disc
and its HDD cache. It then decides to quit, cleanly, through its own shutdown
path.

### The FIOS deadlock, and what it was

The first run hung with 2.1 million lines of the game's own diagnostic:

```
attempt to lock invalid mutex '(null)'
wait for invalid cond 'fios worker cond'      (forever)
```

Two false leads, both worth recording. `sys_ppu_thread_once` was unimplemented,
and an unimplemented import returns `CELL_OK` without running the initialiser
it was handed — that had to be fixed regardless (`src/hle_extra.cpp`), but it
was not the hang. The runtime's `bctr to NULL from func_000AE828+0x78DA` names
a host symbol 8 bytes long, so that offset is a symbolizer artefact, not a
guest address.

The real cause is a struct-layout mismatch. The PS3 ABI is

```c
struct sys_lwcond_t { sys_lwmutex_t *lwmutex; u32 lwcond_queue; };   /* 4 + 4 */
```

with a **32-bit** guest pointer, so the mutex EA occupies the first four bytes.
ps3recomp models it as a big-endian **64-bit** EA at `+0x00` with the queue id
at `+0x08`, which puts a zero word where the guest expects the pointer.
`src/hle_extra.cpp` overrides `sys_lwcond_create` to write the mutex EA into
both 32-bit words: the first satisfies the guest ABI, the second doubles as the
queue id and keeps the runtime's `(u32)vm_read64(lwcond + 0)` truncating to the
mutex, so its `sys_lwcond_wait` works unmodified.

A later store watch over a live FIOS condvar confirms the object directly — it
is 24 bytes, `sys_lwcond_t` at `+0x00`, a debug tag at `+0x08`, the name
pointer at `+0x10`:

```
0x40016400 <- 0x40001888   (our sys_lwcond_create: lwmutex EA, both words)
0x40016408 <- "FIOS obj "  (the Cond constructor's tag)
0x40016410 <- 0x00CF6A50   ('fios worker cond')
```

Two conds live at `0x40016400` and `0x40016418` — 0x18 apart, matching. So the
`+0x00` write is what the guest reads, and that is why the fix works. An earlier
draft of this file described the wrapper as `vtable / name / sys_lwcond_t at
+0x10`; that was inferred from the disassembly and the watch shows it is wrong.
The fix stands on the ABI and on the measurement; the wrapper description does
not.

Result: 2,186,074 log lines and a hang became 279 lines and
`sys_process_exit(code=0)`. FIOS now brings up seven media threads across three
scheduler generations, runs them, and joins them.

### SPURS

The next wall was `cellSpurs CreateTask REJECT no-init`. ps3recomp implements
the v1 taskset path — `cellSpursCreateTaskset` builds the real big-endian
`CellSpursTaskset` the SPU side reads and sets the flag `cellSpursCreateTask`
gates on — but this game uses the v2 API, which was not registered, so that
flag was never set. The v2 entry points differ only in carrying their options
in an attribute struct that the v1 implementation ignores anyway, so
`src/hle_extra.cpp` forwards them:

| NID | Function | Handling |
|---|---|---|
| `0xC2ACDF43` | `_cellSpursTasksetAttribute2Initialize` | set revision/sdkVersion |
| `0x4A6465E3` | `cellSpursCreateTaskset2` | forward to v1 `cellSpursCreateTaskset` |
| `0x9DCBCB5D` | `cellSpursAttributeEnableSystemWorkload` | acknowledge; workloads run on the host |
| `0x45FE2FCE` | `_sys_spu_printf_initialize` | acknowledge |

The taskset is now built and the first SPU task is accepted:

```
[cellSpurs] CreateTaskset2(spurs=0x11010080, taskset=0x11ECF180) -> v1 CreateTaskset
[cellSpurs] CreateTaskset() ea=0x11ECF180 spurs=0x11010080 (real BE layout)
[cellSpurs] CreateTask(id=0, entry=0x00D63C00, ctx=0x40083100)
[spu_workload] async dispatch MISS fp=0xCE95F52496B4AE31 size=36564
```

The dispatch miss is expected — no SPU image is lifted yet.

Five job-queue imports are still faked: `_cellSpursCreateJobQueue`
(`0xF244E799`), `cellSpursJobQueueAttributeSetMaxSizeJobDescriptor`
(`0x1686957E`), an undocumented `cellSpursJq` NID (`0x3D1294FC`),
`_cellSpursLFQueueInitialize` (`0x011EE38B`) and
`cellSpursLFQueueAttachLv2EventQueue` (`0x1656D49F`). The runtime has a
`cellSpursCreateJobQueue`, but the game calls the versioned underscore form
whose argument order has not been confirmed, so nothing is bridged on guesswork
yet.

### SPU images

`cellSpursCreateTask` hands SPURS a guest SPU image; the runtime fingerprints
it (FNV-1a-64) and looks for a lifted version in the workload registry. All
eleven images are now lifted and registered via
`ps3recomp/tools/build_spu_workloads.py`:

```
[spu_workload] dispatch HIT (async) fp=0xCE95F52496B4AE31 args=0x40083100 image=2 -> spawning thread
[taskset] built SpursTasksetContext image=2 taskset=0x11ECF180 task=0
[spu] SPURS taskset syscall num=2 (raw=0x2) image=2 link/r0=0x09668
```

That took a correction. `extract_spu_images.py` and the runtime's
`spu_elf_image_size()` disagree about an image's extent — the first dispatch
reported `size=36564` where the extracted file was 36,512 bytes — so the
fingerprints never matched and every dispatch missed. Two of the eleven images
were short; re-carving them at the runtime's own extent makes
`spu_0001_at_00D53C00` fingerprint to `0xCE95F52496B4AE31`, exactly what the
miss reported. `tools/recarve_spu.py` does this and is idempotent.

### The game's own log

The binary routes its boot trace through one function, guest `0x0034ACAC`,
`log(level, fmt, ...)`, whose output goes nowhere in this port — so most of the
boot was invisible. `tools/post_lift.py` renames the lifted body and
`src/hle_extra.cpp` implements it against host stderr, rendering the format
string with the PPC64 argument registers. `TM_GAMELOG=1` turns it on.

(`ppu_register_function` cannot do this: the lifter emits direct C++ calls, so
only indirect dispatch goes through the address table. Renaming the definition
is the way in — the same post-lift patch pattern rubberducky uses.)

It immediately paid for itself. The title was reporting:

```
[game] GameContent::getBootParameters() ~ title: Unknown Title
[game] GameContent::getBootParameters() ~ titleId: BLES00000
[game] GameContent::bootCheck() ~ cellGameDataCheck(patchData) OK [data exists]
```

ps3recomp's `cellGame` defaults to the placeholder id `BLES00000` and exposes
`cellGame_init_from_paramsfo()` for the harness to call — but nothing called
it, so the game built its game-data and patch-overlay paths under
`/dev_hdd0/game/BLES00000` and believed patch data was present. The harness now
points it at the disc's own `PARAM.SFO`:

```
[game]   title: Twisted Metal      titleId: BCUS98106
[game]   cellGameDataCheck(patchData) OK [data DOES NOT exist]
[game]   usrdirPath:[/dev_hdd0/game/BCUS98106/USRDIR]
```

### Why it exits: no file ever opens

`RTApp::initHardware` (guest `0x0064AD40`) completes its sequence and then calls

```
func_00671698(1, 0x500, 0x2D0)      // renderer init, 1280 x 720
```

which returns **0**. The failure branch stops the `RTApp::backgroundSwap`
thread, spins on `sys_timer_usleep(100)` until it acknowledges, and returns
failure; `main` then prints `Twisted app terminated!`. `tools/post_lift.py` can
wrap guest functions in a tracer (`TM_TRACE=1`), which shows it directly:

```
[trace]-> renderInit(0x00000001, 0x00000500, 0x000002D0)
[trace]  -> f_00670C10(0x00CD7BCC, ...)      <- "debugfont.fnt"
[trace]  <- f_00670C10 = 0 (0)
[trace]<- renderInit = 0 (0)
```

`0x00CD7BCC` is `"debugfont.fnt"`, and the neighbouring string is
`graphics/Font.cpp`. The file is present —
`PS3_GAME/USRDIR/globals/rt/fonts/debugfont.fnt` — so the load is failing, not
the data.

And it is failing because **no file is ever opened**. With the runtime's
filesystem tracing on, a whole boot logs zero opens, reads or seeks. The
renderer fails because its first font load gets nothing back.

### The actual blocker was a lifter boundary bug

FIOS allocated its worker objects, spawned threads on them and then locked an
unconstructed mutex and called through a null vtable. A census of the FIOS base
constructor (guest `0x007556B4`, which every FIOS object goes through) showed
**278** objects built over a boot — the scheduler complete with all eleven named
sub-objects, both worker condvars, both worker threads — and **not one** of them
a worker. A store watch confirmed it from the other side: the worker block is
memset at allocation and then receives only `+0x04` (its `Thread*`), `+0x08`
(its `Cond*`) and `+0x34`.

The cause turned out to be in the recompilation, not the HLE.
`find_functions` ended `func_0076C534` at `0x0076CAFC`, but the real function
runs to `0x0076CDF4` — 760 bytes, about 190 instructions, covered by no
detected function. The lifter emitted the branch targets inside that gap as
separate fragments, and every one of them ends in a trampoline **except**
`0x0076CB00`, whose loop falls off the end of the emitted function:

```c
        if (((ctx->cr >> 0) & 4)) goto loc_0076CB54;
}                                    /* <- no fall-through to 0x0076CBB4 */
```

So when that loop finished, control returned to the caller instead of
continuing into the rest of the FIOS scheduler constructor — which is where the
workers are built — and callee-saved registers were left holding loop values.
The caller then took a loop pointer for its object, locked a mutex that had
never been constructed, and called through a zero vtable. Every symptom traced
over the previous several passes was downstream of that one missing edge.

`tools/post_lift.py` restores it (`FALLTHROUGH`, idempotent):

| | before | after |
|---|---|---|
| `attempt to lock invalid mutex` | 6 | **0** |
| `bctr to NULL` | 3 | **0** |
| file opens | 0 | **real** |
| log lines | ~300 | 16,716 |

The game now reads its own data:

```
[fs] open '/dev_bdvd/PS3_GAME/USRDIR/globals/rt/fonts/debugfont.fnt' -> fd 3
[fs] read fd=3 nbytes=32768 -> 32768 (magic=464F4E54 "FONT")
[fs] open '/dev_bdvd/PS3_GAME/USRDIR/globals/rt/textures/specials.rtt' -> fd 3
GameContent::initConfigDocA(tmxconfig.sdat)
```

The proper fix belongs upstream in `find_functions`' boundary detection; the
post-lift edge keeps it in this repo for now. It is also worth auditing the
other 35,635 lifted functions for the same shape — a fragment whose last
statement is a conditional branch, with no terminator.

### Two filesystem bugs, in ps3recomp

With FIOS alive the title started reading, and immediately hit two runtime bugs.
Both fixes are in **`../ps3recomp`**, not this repo — they are runtime-level and
affect every title. They live there as working-tree edits; the same diff is
kept here as [`docs/ps3recomp-fixes.patch`](docs/ps3recomp-fixes.patch)
so this repo records what the build depends on, and so they can be reapplied or
upstreamed independently.

**Short reads past the first page.** `read fd=3 nbytes=32768 -> 0` with
`eof=0 err=1`, deterministic, on files hundreds of KB long. The guest VM is
`MEM_RESERVE`d with each page committed on first access by a vectored exception
handler — which covers CPU access from lifted code, but `fread` moves data
through the *kernel*, and a kernel write to a reserved page raises no user-mode
exception. The I/O just fails. So read 1 of a file worked (its destination had
been touched) and read 2 into a fresh page returned nothing, and the title
printed its own *"Short read … Possible reasons include disc eject"* and gave
up. `ppu_fs.cpp` now pre-faults the destination range read-then-write, in user
mode, before `fread`/`fwrite`. Short reads: **0**.

**`tmxconfig.sdat` would not decrypt.** `cellFsSdataOpen` reported
*"NPD decrypt FAILED (unsupported EDAT/needs license)"* and handed the caller a
success with no handle, so `GameContent::initConfigDocA` got nothing. The file
is flags `0x0100003C` — `SDAT_FLAG` is set, so it uses the fixed SDAT key and
needs no license at all. `sdata_decrypt.h` was bailing purely on flag `0x20`,
which changes only the block *layout*: a 0x20-byte metadata record precedes
every block instead of one table up front. Same crypto. Teaching it that one
offset was enough:

```
[fs] SdataOpen '/dev_bdvd/PS3_GAME/USRDIR/tmxconfig.sdat' -> fd 3
     (NPD decrypted, 0x54E4 bytes, magic '<?xm')
```

The config document is XML, and the title stopped exiting — it now runs past
config load and keeps going.

### 12% of the code was never lifted

The next wall was the same defect that caused the FIOS hang, measured properly.
`find_functions` detects 31,032 functions but accounts for only 88% of the
executable segment: **7,197 gaps totalling 1,573,492 bytes (12.09%)** lie
between them.

That is not cosmetic. A gap immediately after a function truncates it — which
stranded `func_0076C534`'s tail and left the FIOS workers unconstructed. And
code *inside* a gap is never lifted, so an indirect call into it lands on the
runtime's "unresolved indirect call" path and quietly returns. Seven such
targets in one uncovered 0x1A4-byte region at `0x009D38E4` spun the title
forever — 199,351 log lines per 200,000.

`tools/seed_gaps.py` closes the holes: it computes the gaps, emits a function
entry for each, and splits them at addresses harvested from a run log so
indirect targets become real entry points. **31,032 → 38,235 functions, 0 bytes
uncovered**, and re-lifting produced 47,120 emitted functions (from 35,635)
across 12 chunks. Unresolved indirect calls: **199,351 → 0**. The proper fix
belongs in `find_functions`' boundary detection upstream.

### Where it runs to now

The title boots, loads its data and renders:

```
[D3D12] adapter: NVIDIA GeForce RTX 5070 (VRAM 11943 MB)
[D3D12] Initialization complete (1280x720, 2 buffers, pipeline=ready)
[D3D12] bind_texture(unit=0, offset=0x2ACD800, fmt=0x86, 1024x512)
[RSX] DRAW_ARRAYS prim=... first=... count=...        x32
[fs] open ... x200          (fonts, .rtt textures, localisation, car icons)
[cellSaveData] dispatching funcStat OPD=0x00F002D0 (isNew=1)
[cellSpurs] CreateTaskset2 x2, CreateTask(id=0, entry=0x00D63C00)
[spu_workload] dispatch HIT (async) fp=0xCE95F52496B4AE31 -> spawning thread
```

One more title-local fix was needed to get here. ps3recomp deliberately *fails*
`cellNetCtlGetState` when offline, because LittleBigPlanet polls it forever
waiting for `IPObtained` and only leaves that loop on `ret < 0`. Twisted Metal
reads the error as "not ready yet" and retries — 704,666 times in two minutes.
Real hardware returns `CELL_OK` with `state = Disconnected` when there is simply
no connection, so `src/hle_extra.cpp` overrides the NID to say that. 704,666
calls became 4.

### Pixels: the path works, the content does not arrive

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

### What gates the content: Edge Zlib on the SPU

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

### Inflating on the host, and the buffer that was wrong twice

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

### The front end runs; the renderer never leaves the loading screen

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

### Why nothing draws: the RSX is parked and never released

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

### Getting the walker unstuck, and the first real geometry

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

The screen is still black. The scene renders into those offscreen targets and
the composite to the 720x480 display buffer never lands: no `NV3089` blit is
issued at all in this state, and `RTT_SAVERT` on the 1280x704 colour targets
comes back empty, so the geometry is going somewhere that ends up cleared. That
composite is the remaining step between this port and a picture.

### Finding a message when the cross-reference cannot

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

### Why the screen is dark: nothing is drawn

Worth stating plainly, because it is easy to blame the graphics path. It is not
the graphics path.

The title does not present by drawing into a backbuffer. It renders its scene
into a 1280x704 surface and composites with the RSX 2D engine:

```
[NV3089] 676x448 src=0xC1190000(pitch 10240) -> dst=0xC0010000(pitch 5120) at 22,16
```

`dst=0xC0010000` is local-memory offset `0x10000` — display buffer 0 — so the
finished, letterboxed SD frame is assembled in **guest memory**, scaled down
from the scene surface and inset by the overscan margin.

Both ends of that are empty. `RTT_SAVERT=1190000` dumps the scene render target
itself: 1280x704, **every pixel zero**. `TM_FBDUMP` dumps the guest display
buffer straight out of `vm_base`: 720x480, every pixel zero. The title issues 32
draws in a hundred seconds, all at startup, all the same 4-vertex QUAD, and
renders nothing else — because it is waiting on a loading screen.

So the compositor, the blit, the present and the draws all behave; there is
simply no image to show. The dark window is a symptom of the loader, not of the
renderer, and `CLEAR_RGB=0.2,0.4,0.8` settles it in one run: every dumped frame
comes back a uniform `(204,102,51)`, so the clear reaches the swapchain and only
geometry is missing. The one texture the title does bind, dumped out of local
memory with `TM_TEXDUMP=0x2ACD800,1024,512`, is two thin bands of noise on
black — an allocation nothing has filled in. Meanwhile `cellVideoOutSetGamma(0.00)`
every frame is the title holding a deliberate black fade while its load bar is
up.

### Reading the title's own config

`tools/decrypt_edat.py` decrypts SDATA containers with the published fixed keys,
which makes `tmxconfig.sdat` readable:

```xml
<root version="1.0" assetlabel="BCUS98106">
  <param key="RESOLUTION" fmt="int32-enum">1080/720/576/576(16:9)/480/480(16:9)</param>
  ... <usebots> <unlimitedweapons> <shotclock> ...
```

446 parameters and 14 file entries — useful for confirming what the title
expects, and independent of the runtime's own decryptor.

### Video modes

Checked, and not the cause. ps3recomp's `cellVideoOutGetDeviceInfo` advertises
four modes but leaves `CellVideoOutDisplayMode.refreshRates` zero on every one,
so a title scanning for a mode that supports its rate finds none;
`src/hle_extra.cpp` now writes the full big-endian struct with
`refreshRates = 0x0005` (59.94 | 60 Hz). `TM_VIDEO_MODES` overrides the
advertised set.

It changes nothing here: with only 720p advertised, with 1080p first, or with
the set reordered, the game still calls `cellVideoOutConfigure` with
`resId=4` (720x480). It is not choosing from the device table.

The ten unknown `cellSysutil` imports were also resolved, by computing NIDs from
RPCS3's function names: nine are `cellWebBrowser*` and one is
`cellOskDialogAddSupportLanguage` — the in-game browser and OSK, nothing on the
boot path.

### Names, without a symbol table

The binary is stripped, but it kept its assert and log strings — **731
`Class::method` names** and **359 source paths**. That is a partial symbol
table hiding in `.rodata`, and it names the engine outright:

| Evidence | Component |
|---|---|
| `hkaAnimation.inl`, `hkGsk.h`, `hkgpConvexHull*`, `hkgpMesh.h` | Havok physics + animation |
| `job/src/ppu/jobapi/jobarraycontainer.cpp`, `commandlistchecker.cpp` | Sony SPU job API (the 11 SPU images) |
| `audio_sys/boomrang/plugin_sdk/`, `dsp/`, `modules/dynamics/compressor.cpp` | Boomrang, Eat Sleep Play's own audio DSP |
| `AiChar::update`, `AiRagdollManager::initContainers`, `Boss1Truck::updateSuperCrushConstraint`, `Campaign::waitForPager` | the game itself |

Attributing each string back to the function that references it would name a
useful fraction of the 35,635 lifted functions. Not done yet.

### The demo disc

`BCET70046`, the PSN demo, was checked for a debug build. It is not one — same
39 sections, no `.symtab`, 14,361 OPD descriptors against retail's 14,372, the
same 34 libraries and 435 imports against 439. Nothing to recover from it.

It is still useful as a second target: near-identical code, 1.4 GB of assets
instead of 13 GB. Decrypting it needed the NPDRM path (free license, so the
published `NP_klic_free`), which `tools/decrypt_self.py` now handles.

## Reproducing the analysis

You need your own copy of the game and a scetool-format key file at `data/keys`
(gitignored, never committed).

```bash
# Only if your dump is a raw encrypted disc image; a decrypted rip skips this.
python tools/decrypt_iso.py "Twisted Metal (USA).iso" --key <32hex> -o tm.dec.iso
7z e tm.dec.iso -oinput PS3_GAME/USRDIR/EBOOT.BIN

# SELF -> ELF
python tools/decrypt_self.py input/EBOOT.BIN -o input/EBOOT.ELF

# Analysis (ps3recomp checked out at ../ps3recomp)
P=../ps3recomp/tools
python $P/elf_parser.py       input/EBOOT.ELF --imports > meta/imports.json
python $P/nid_database.py     --batch meta/nids.txt --json > meta/nids_resolved.json
python $P/find_functions.py   input/EBOOT.ELF --output meta/functions.json
python $P/extract_spu_images.py input/EBOOT.ELF --output meta/spu
python $P/ppu_loader.py       input/EBOOT.ELF -o meta/
python $P/gen_hle_nids.py     --all --out src/gen/ppu_hle_nids.cpp

# Lift (~2 min, 295 MB of C++)
python $P/ppu_lifter.py input/EBOOT.ELF \
       --functions meta/functions.json --hle-stubs meta/EBOOT.imports.json \
       --toc 0xF21930 --code-end 0xC79C6C --output src/recomp -j 16

# Build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
```

Both tools carry a `--selftest` that runs without any game data:
`decrypt_iso.py` round-trips a synthetic encrypted image through its region
parser; `decrypt_self.py` checks key selection and the CTR round-trip. The
SELF decryptor also validates its own output — the entry point must resolve to
an OPD descriptor pointing back into the image, which noise never does.

## Prerequisites

- Python 3.9+ with `pycryptodome`
- CMake 3.20+, Ninja
- LLVM/clang-cl 14+ inside a VS 2022 dev environment
- [ps3recomp](https://github.com/sp00nznet/ps3recomp) checked out at `../ps3recomp`
- 7-Zip (reads the UDF image)

## Project structure

```
twistedmetal/
├── README.md
├── LICENSE
├── CMakeLists.txt          # links the runtime + the boot harness
├── tools/
│   ├── decrypt_iso.py      # raw PS3 disc image -> plain image (bring your own key)
│   ├── decrypt_self.py     # retail SELF -> plain ELF (bring your own key file)
│   ├── recarve_spu.py      # size SPU images the way the runtime fingerprints them
│   ├── seed_gaps.py        # cover the 12% of code find_functions misses
│   ├── unpsarc.py          # PSARC v1.4 reader (how the archive layout was confirmed)
│   ├── symbolize.py        # watchdog host RVAs -> lifted guest function names
│   └── post_lift.py        # idempotent post-lift patches (loggers, traces, overrides)
├── src/
│   ├── boot_main.cpp       # ps3recomp boot harness, rebranded for this title
│   ├── hle_extra.cpp       # imports this title reaches that the runtime lacks
│   ├── tm_inflate.cpp      # self-contained RFC 1951 inflater for the SPU decompressor
│   ├── compat/             # <dirent.h>/<unistd.h> Win32 shims
│   ├── gen/                # generated HLE NID table (committed)
│   ├── spu_gen/            # lifted SPU images, 19 MB (gitignored; regenerate)
│   └── recomp/             # lifted C++, 295 MB (gitignored; regenerate)
├── docs/
│   └── ps3recomp-fixes.patch      # runtime fixes this build needs
├── data/keys               # your scetool key file (gitignored)
├── input/                  # your EBOOT + assets (gitignored)
└── meta/                   # analysis output, regenerated (gitignored)
```

Everything derived from the game — the plain ELF, lifted C++, analysis JSON,
extracted SPU images — is generated from your own copy and stays out of the repo.

## Related

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** — the PS3 HLE runtime this builds against
- **[rubberducky](https://github.com/sp00nznet/rubberducky)** — the first ps3recomp title to render its own scene
- **[tokyojungle](https://github.com/sp00nznet/tokyojungle)** — a retail PS3 title on the same pipeline

## Legal

No proprietary Sony code, game binaries, encryption keys, or copyrighted assets
are in this repository. It contains clean-room tooling only; everything derived
from the game is generated locally from a copy you supply. Screenshots of the
port's own output may appear under `docs/images/`.

Licensed under the [MIT License](LICENSE).
