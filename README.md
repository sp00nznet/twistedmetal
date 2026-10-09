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

Geometry from the title's own draw stream **rasterises**. It boots real game
code — CRT, memory, `cellGcmInit`, video-out, display buffers, RSX submission
with a D3D12 window open — brings up the engine's FIOS file-I/O scheduler,
installs its game data, initialises Sony's BoomRangBuss audio middleware, opens
186 asset files off the disc, decompresses the UI archive, and runs its
Scaleform front end. Rendering goes through
[caner's live NV4097 engine](https://github.com/canersaka/Yakuza-Dead-Souls-EX),
which this port drove upstream into ps3recomp along with the HDR surface formats
and the FIFO subchannel fix it needed
([ps3recomp#113](https://github.com/sp00nznet/ps3recomp/pull/113)).

Two things had to be right before anything appeared, and both were about *when*
rather than *what*: presenting on the guest's flip instead of a 60 Hz ticker,
and sampling the surface before the guest began clearing the next frame. Dumps
that read back as flat clear colours were being taken between frames.

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
| Boot | **reaches the match** — front end, menus, vehicle select, suburbs match with six opponents |
| Asset decompression | **works on the host** — 192 MB, ~1.8 ms per 64 KB block |
| Front end | **playable path** — legal, attract, menus and loading driven by a scripted pad |
| Graphics (RSX → D3D12) | **in-match HUD renders** — menus at full brightness; the 3D world and post-FX do not draw yet |
| SPURS jobs | **run** — all 15 JobQueue job binaries lifted, and the JOBCRT start-up fixed so they reach `main` |
| Audio / input | **audio initialises** — BoomRangBuss 1.0.33, banks load; input not started |

## Where it stops

The match runs, and its HUD draws, but the world does not. The chain behind
that, from the culling jobs that never ran through the Edge geometry ring to
the post-FX tasks that never start, is in
**[docs/in-match.md](docs/in-match.md)**. The live blocker is an unregistered
cellSpurs import (NID 0x7FDF4FEF) in the post-FX library init. Because of it
the post-FX tasks are never created, and the RSX waits forever on the frame
label they release.

This build links the runtime branch
[`ydkj-master-bringup`](https://github.com/sp00nznet/ps3recomp/tree/ydkj-master-bringup)
of ps3recomp, which carries every runtime fix the port needed.

## The working log

This port was figured out in the open, and the notes are worth more than a
summary. They live in [`docs/`](docs/):

- **[Static analysis](docs/analysis.md)** — what the binary is before any of it runs: imports, SPU images, the lift, HLE coverage.
- **[Boot bring-up](docs/bringup.md)** — the FIOS deadlock, SPURS, the lifter boundary bug that was the real blocker, and two filesystem bugs that went back upstream.
- **[Getting the first pixels](docs/first-pixels.md)** — Edge Zlib on the SPU, an RSX parked and never released, and the discovery that the draws had been rendering the whole time.
- **[The live engine, and the movie path](docs/live-engine-and-movies.md)** — porting caner's engine into this tree, the one fragment program that made everything render black, and the road to attract mode.
- **[Into the match](docs/in-match.md)** — the HUD in a running match, the SPU jobs that failed before `main`, the Edge geometry ring, and the post-FX frame label.
- **[Reference notes](docs/reference.md)** — the title's own config, video modes, recovering names without a symbol table, the demo disc.

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

## Building on Linux

The harness also builds and boots on Linux (checked on a Steam Deck,
SteamOS, GCC 15.2). Off Windows, CMake builds the runtime from
`PS3RECOMP_DIR` itself, so it needs SDL2 (plus Vulkan headers and glslang
for `-DPS3RECOMP_RSX_VULKAN=ON`), and a ps3recomp tree that has both
`ydkj-master-bringup` and master's POSIX fixes.

Without any game data, `twistedmetal_smoke` boots this harness on
ps3recomp's synthetic smoke title:

```bash
cmake -S . -B build-smoke -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DTM_BUILD_GAME=OFF -DTM_BUILD_SMOKE=ON
cmake --build build-smoke --target twistedmetal_smoke
./build-smoke/twistedmetal_smoke build-smoke/smoke/smoke.elf   # [smoke] PASS
```

The European disc, `BCES01010`, is a different binary from `BCUS98106`:
its TOC, code end and hooked functions sit at other addresses, which
`TM_TITLE` and `post_lift.py --title` select.

```bash
P=../ps3recomp/tools
python $P/find_functions.py input/EBOOT.ELF --output meta/functions.json
python tools/seed_gaps.py --code-hi 0xC7BD3C
python $P/ppu_loader.py input/EBOOT.ELF -o meta/
python $P/ppu_lifter.py input/EBOOT.ELF \
       --functions meta/functions.seeded.json --hle-stubs meta/EBOOT.imports.json \
       --toc 0xF21968 --code-end 0xC7BD3C --chunk-lines 150000 --output src/recomp -j 3
python tools/post_lift.py --title BCES01010
python $P/extract_spu_images.py input/EBOOT.ELF --output meta/spu
python $P/build_spu_workloads.py --images meta/spu --lifted src/spu_gen \
       --out src/gen/spu_workloads.c --register-fn tm_spu_register_all \
       --constructor --title twistedmetal
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DTM_TITLE=BCES01010 \
      -DTM_LINK_SPU_JOBS=OFF -DTM_RECOMP_COMPILE_OPTIONS=-O1
cmake --build build-linux -j2
PS3_VFS_ROOT=<writable dir holding PS3_GAME> ./build-linux/twistedmetal input/EBOOT.ELF
```

- GCC needs about 2 GB per 240k-line chunk at `-O1`. `--chunk-lines`
  (ps3recomp's `ppu_lifter.py`) keeps each chunk small enough for a 16 GB
  machine; without it, chunks are 600k lines.
- The VFS root must be writable: `/dev_hdd0` maps into it, and the title
  installs its game data there on first boot. A read-only disc tree can be
  symlinked in as `PS3_GAME`.
- `TM_GAMELOG=1` prints the game's own log, as on Windows.

On Linux the title boots on the null backend to its legal screens
(`UiLegal_1` through `UiLegal_havok`), with the UI archive inflated on the
host -- but not on every run yet: after the archive loader starts it often
stalls. Nothing is rendered: the live NV4097 engine is D3D12-only, and the
Vulkan backend is not wired in.

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
│   ├── smoke_stubs.cpp     # empty game hooks for twistedmetal_smoke
│   ├── compat/             # <dirent.h>/<unistd.h> Win32 shims
│   ├── gen/                # generated HLE NID table (committed)
│   ├── spu_gen/            # lifted SPU images, 19 MB (gitignored; regenerate)
│   └── recomp/             # lifted C++, 295 MB (gitignored; regenerate)
├── docs/                    # the working log, split by phase
│   ├── analysis.md          # what the binary is, before it runs
│   ├── bringup.md           # boot: FIOS, SPURS, the lifter boundary bug
│   ├── first-pixels.md      # Edge Zlib, the parked RSX, the first geometry
│   ├── live-engine-and-movies.md
│   ├── in-match.md          # the match, SPURS jobs, Edge ring, post-FX label
│   ├── reference.md         # config, video modes, names without symbols
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
