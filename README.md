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

That was not enough, and it took a long time to notice: the title's *other*
network loop needs the state to actually read `IPObtained`, and a truthful
`Disconnected` leaves it spinning just as surely as an error does. The boot sat
there behind the legal screens for the whole of the graphics investigation
below. See [What was actually blocking the boot](#what-was-actually-blocking-the-boot-network-init)
— with `IPObtained` reported the state machine reaches `MainMenu` and the draw
count goes from a handful of quads to 101,208.

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

### The draws were rendering the whole time

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

### What was actually blocking the boot: network init

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

### Where the movie path actually begins

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

### Reaching the intro video

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

### Driving the title to the intro: `UiMoviesMenu::onSelect(st_intro.avi)`

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

#### Where it stops

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

### The menu's textures are fine (and how that was nearly missed)

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

### Three instruments that were lying

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
