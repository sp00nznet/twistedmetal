# Boot bring-up

> From the first boot to a title that runs its own code end to end — the FIOS deadlock, SPURS, the lifter boundary bug that was the real blocker, and two filesystem bugs that went back upstream.

## First boot

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

## The FIOS deadlock, and what it was

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

## SPURS

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

## SPU images

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

## The game's own log

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

## Why it exits: no file ever opens

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

## The actual blocker was a lifter boundary bug

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

## Two filesystem bugs, in ps3recomp

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

## 12% of the code was never lifted

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

## Where it runs to now

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

---

*Part of the [Twisted Metal static recompilation](../README.md) working log.*
