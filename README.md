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
FIOS file-I/O scheduler up, runs and joins its worker threads, and then shuts
down cleanly because SPURS task creation is rejected. Nothing is rendered yet.

| Phase | State |
|---|---|
| Disc inventory | **done** — `BCUS98106`, disc game, one game-side PRX |
| Disc decryption | **done** — `tools/decrypt_iso.py`, only needed for a raw dump |
| `EBOOT.BIN` → plain `EBOOT.ELF` | **done** — `tools/decrypt_self.py`, 16.3 MB ELF |
| Import / NID analysis | **done** — 439 imports, 34 libraries, 376 resolved (86%) |
| Function boundary detection | **done** — 31,032 functions, every `.opd` address verified |
| SPU image extraction | **done** — 11 embedded SPU ELFs, 1.17 MB |
| PPU lifting | **done** — 35,635 functions emitted, 4.47 M lines of C++ |
| Build & link | **done** — 106 MB x86-64 exe, clang-cl 21 + Ninja, 6 warnings |
| Boot | **runs to a clean exit** — FIOS + SPURS tasksets up; needs game data |
| Graphics (RSX → D3D12) | window opens, clears submitted, nothing drawn |
| Audio / input | not started |

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
at `+0x08`. Nothing notices while only the runtime touches the struct — but
this game touches it. Its FIOS condition-variable wrapper is 24 bytes:

```
+0x00 vtable   +0x08 name   +0x10 sys_lwcond_t
```

and `Cond::wait` (guest `0x0077A2E0`) validates with

```c
if (*(u32*)(this + 0x10) == 0) printf("wait for invalid cond '%s'
", name);
```

Against the 64-bit layout that word is the *high half* of the EA — always zero.
So every FIOS wait short-circuited instead of blocking, both media threads
spun, one held the scheduler's lwmutex the whole time, and the main thread
deadlocked behind it.

`src/hle_extra.cpp` overrides `sys_lwcond_create` to write the mutex EA into
both words: the first satisfies the guest ABI and the game's check, the second
doubles as the queue id and keeps the runtime's `(u32)vm_read64(lwcond + 0)`
truncating to the mutex, so its `sys_lwcond_wait` works unmodified. The proper
fix belongs upstream in ps3recomp's `ppu_sysprx.cpp`.

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

### Why it still exits

Not SPURS. The game's own boot trace stops before its first timing print
(`init: %.2f`, then `GameRT::startup`, `WorldLoader::loadUi`) and goes straight
to `Twisted app terminated! ReceivedExitGameRequest(False)` — terminated, with
no exit request received. `input/` holds only the EBOOT, so every asset open
under `/dev_bdvd/PS3_GAME/USRDIR` fails and the boot gives up. Staging the disc
tree there is the next step.

Two things also survive and are not yet understood: six
`attempt to lock invalid mutex '(null)'` pairs around FIOS scheduler teardown,
and one `bctr to NULL` per scheduler with the scheduler object in `r3`.

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
│   └── decrypt_self.py     # retail SELF -> plain ELF (bring your own key file)
├── src/
│   ├── boot_main.cpp       # ps3recomp boot harness, rebranded for this title
│   ├── hle_extra.cpp       # imports this title reaches that the runtime lacks
│   ├── compat/             # <dirent.h>/<unistd.h> Win32 shims
│   ├── gen/                # generated HLE NID table (committed)
│   └── recomp/             # lifted C++, 295 MB (gitignored; regenerate)
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
