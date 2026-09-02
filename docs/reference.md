# Reference notes

> Standing notes rather than narrative: the title's own config, video modes, how names were recovered without a symbol table, and the demo disc.

## Why the screen is dark: nothing is drawn

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

## Reading the title's own config

`tools/decrypt_edat.py` decrypts SDATA containers with the published fixed keys,
which makes `tmxconfig.sdat` readable:

```xml
<root version="1.0" assetlabel="BCUS98106">
  <param key="RESOLUTION" fmt="int32-enum">1080/720/576/576(16:9)/480/480(16:9)</param>
  ... <usebots> <unlimitedweapons> <shotclock> ...
```

446 parameters and 14 file entries — useful for confirming what the title
expects, and independent of the runtime's own decryptor.

## Video modes

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

## Names, without a symbol table

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

## The demo disc

`BCET70046`, the PSN demo, was checked for a debug build. It is not one — same
39 sections, no `.symtab`, 14,361 OPD descriptors against retail's 14,372, the
same 34 libraries and 435 imports against 439. Nothing to recover from it.

It is still useful as a second target: near-identical code, 1.4 GB of assets
instead of 13 GB. Decrypting it needed the NPDRM path (free license, so the
published `NP_klic_free`), which `tools/decrypt_self.py` now handles.

---

*Part of the [Twisted Metal static recompilation](../README.md) working log.*
