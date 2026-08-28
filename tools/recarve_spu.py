#!/usr/bin/env python3
"""Re-carve the extracted SPU images using the runtime's own image extent.

extract_spu_images.py and spu_workload.c's spu_elf_image_size() disagree (the
dispatch miss reported 36564 bytes where the extracted file is 36512), so the
FNV-1a-64 fingerprints never line up. build_spu_workloads.img_size IS the
runtime's definition, so size each image with that, read straight from the
decrypted ELF, and rewrite the file.
"""
import glob, os, re, sys

sys.path.insert(0, os.environ.get('PS3RECOMP_TOOLS', '../ps3recomp/tools'))
from build_spu_workloads import img_size, fnv1a64 as fingerprint

ELF = 'input/EBOOT.ELF'
elf = open(ELF, 'rb').read()

for path in sorted(glob.glob('meta/spu/*.elf')):
    m = re.search(r'_at_([0-9A-Fa-f]{8})\.elf$', path)
    off = int(m.group(1), 16)   # the filename encodes a FILE offset, not a vaddr
    old = open(path, 'rb').read()
    # A generous slice; img_size walks the headers to find the real extent.
    blob = elf[off:off + 0x80000]
    n = img_size(blob)
    new = blob[:n]
    tag = 'same' if new == old else f'RESIZED {len(old)} -> {n}'
    open(path, 'wb').write(new)
    print(f'{os.path.basename(path):34} off={off:#x} size={n:6}  '
          f'fp=0x{fingerprint(new):016X}  {tag}')
