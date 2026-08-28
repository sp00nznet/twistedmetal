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

The binary is decrypted and fully analysed. Lifting has not started.

| Phase | State |
|---|---|
| Disc inventory | **done** — `BCUS98106`, disc game, one game-side PRX |
| Disc decryption | **done** — `tools/decrypt_iso.py`, only needed for a raw dump |
| `EBOOT.BIN` → plain `EBOOT.ELF` | **done** — `tools/decrypt_self.py`, 16.3 MB ELF |
| Import / NID analysis | **done** — 439 imports, 34 libraries, 376 resolved (86%) |
| Function boundary detection | **done** — 31,032 functions, every `.opd` address verified |
| SPU image extraction | **done** — 11 embedded SPU ELFs, 1.17 MB |
| PPU lifting | not started |
| Build & link | not started |
| Boot | not started |
| Graphics (RSX → D3D12) | not started |
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
├── tools/
│   ├── decrypt_iso.py      # raw PS3 disc image -> plain image (bring your own key)
│   └── decrypt_self.py     # retail SELF -> plain ELF (bring your own key file)
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
