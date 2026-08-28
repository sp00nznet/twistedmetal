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

Early. The binary is not lifted yet — the disc image is still encrypted.

| Phase | State |
|---|---|
| Disc inventory (`PARAM.SFO`, region table) | **done** — `BCUS98106`, disc game, 1 game-side PRX |
| Disc decryption | **blocked** — needs the disc key (see below) |
| `EBOOT.BIN` → plain `EBOOT.ELF` | not started |
| Import / NID analysis | not started |
| Function boundary detection | not started |
| PPU lifting | not started |
| Build & link | not started |
| Boot | not started |
| Graphics (RSX → D3D12) | not started |
| Audio / input | not started |

### What the disc looks like

```
Twisted Metal (USA) (En,Fr,Es).iso   13.46 GB, UDF 2.50, "*SCEI BD/DVD Generator"
  PS3_DISC.SFB                        plain
  PS3_GAME/PARAM.SFO                  plain  — TITLE_ID BCUS98106, APP_VER 01.00,
                                               PS3_SYSTEM_VER 03.7300, CATEGORY DG
  PS3_GAME/USRDIR/EBOOT.BIN           encrypted, 16.3 MB
  PS3_GAME/USRDIR/prx/PsnEula.ppu.sprx encrypted, 8.6 KB  (the only game PRX)
  PS3_GAME/USRDIR/{audio,...}         encrypted, ~13 GB of assets
```

Sector 0 carries the PS3 region table, and this dump is a raw one:

| Sectors | | Size |
|---|---|---|
| `0x000000`–`0x00363f` | plain | 0.03 GB |
| `0x003640`–`0x62525f` | **AES-128-CBC encrypted** | 13.17 GB |
| `0x625260`–`0x64527f` | plain | 0.27 GB |

That is why `PARAM.SFO` reads fine and `EBOOT.BIN` comes out as noise — the
UDF metadata lives in the plain region, the payload does not.

### The blocker

Decrypting needs the 16-byte per-disc key. It cannot be derived from the image;
it comes off the physical disc (the BD ROM mark) or from a `.dkey` / IRD for
`BCUS98106`. Once you have it:

```bash
python tools/decrypt_iso.py "Twisted Metal (USA) (En,Fr,Es).iso" \
       --key <32 hex chars> -o twistedmetal.dec.iso

7z e twistedmetal.dec.iso -oinput PS3_GAME/USRDIR/EBOOT.BIN
xxd -l 4 input/EBOOT.BIN     # expect: SCE\0
```

`tools/decrypt_iso.py` also takes `--dkey` (the raw `d1` from a `.dkey` file),
deriving the disc key with `PS3_DISC_ERK` / `PS3_DISC_ERK_IV` from the
environment. No keys or key-derivation constants are stored in this repo.

`python tools/decrypt_iso.py --selftest` round-trips a synthetic image and
checks the region parser, so the tool is verifiable without a disc.

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
├── tools/
│   └── decrypt_iso.py      # raw PS3 disc image → plain image (bring your own key)
└── input/                  # your EBOOT + assets (gitignored)
```

Everything else — lifted C++, analysis JSON, extracted SPU images — is
generated from your own copy and stays out of the repo.

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
