# Static analysis

> What the binary is, before any of it runs: imports, SPU images, the lift, and how much of the HLE surface the title actually reaches.

## The binary

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

## Imports

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

## SPU

Eleven SPU images are embedded in the PPU binary, 1.17 MB of SPU code in total,
sizes from 29 KB to 168 KB. `cellSpurs` + `cellSpursJq` in the import list says
they run as SPURS jobs rather than raw SPU threads. Identifying what each one
does — physics, particles, audio, culling — comes before any decision to HLE
them or run them through ps3recomp's SPU lifter.

## Lifting

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

## HLE coverage

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

---

*Part of the [Twisted Metal static recompilation](../README.md) working log.*
