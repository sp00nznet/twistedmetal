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
| Boot | **runs the front end** — 186 files opened, audio middleware up, Scaleform UI loaded |
| Asset decompression | **works on the host** — 192 MB, ~1.8 ms per 64 KB block |
| Front end | **starts** — Scaleform UI loads, `UiLegal_1::onEnter` reached |
| Graphics (RSX → D3D12) | **rasterises** — the title's own geometry draws through the live NV4097 engine, presented on the guest's flip |
| Audio / input | **audio initialises** — BoomRangBuss 1.0.33, banks load; input not started |

## Where it stops

Attract mode and the intro video are the live frontier — the title's `st_intro.avi`
decodes through an H.264 Media Foundation transform, and the menu's textures are
confirmed good. What is on screen is still short of a playable front end.

## The working log

This port was figured out in the open, and the notes are worth more than a
summary. They live in [`docs/`](docs/):

- **[Static analysis](docs/analysis.md)** — what the binary is before any of it runs: imports, SPU images, the lift, HLE coverage.
- **[Boot bring-up](docs/bringup.md)** — the FIOS deadlock, SPURS, the lifter boundary bug that was the real blocker, and two filesystem bugs that went back upstream.
- **[Getting the first pixels](docs/first-pixels.md)** — Edge Zlib on the SPU, an RSX parked and never released, and the discovery that the draws had been rendering the whole time.
- **[The live engine, and the movie path](docs/live-engine-and-movies.md)** — porting caner's engine into this tree, the one fragment program that made everything render black, and the road to attract mode.
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
├── docs/                    # the working log, split by phase
│   ├── analysis.md          # what the binary is, before it runs
│   ├── bringup.md           # boot: FIOS, SPURS, the lifter boundary bug
│   ├── first-pixels.md      # Edge Zlib, the parked RSX, the first geometry
│   ├── live-engine-and-movies.md
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
