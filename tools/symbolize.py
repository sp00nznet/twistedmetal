#!/usr/bin/env python3
"""Turn the watchdog's host RVAs into lifted guest function names.

When the boot wedges, the harness snapshots every other thread's return
addresses as module RVAs. Those are only useful once mapped back to the lifted
function they land in, which the linker map makes exact — and unlike the
runtime's own nearest-symbol guess (which resolves against a function table
whose host order does not match guest order, and so reports nonsense like
`func_000AE828+0x78DA` for an 8-byte function), this reads the real layout.

    build/twistedmetal.exe ... 2>&1 | tee run.log
    python tools/symbolize.py run.log
"""

import bisect
import re
import sys


def load_map(path):
    """(rva, name) sorted by rva, from a link.exe /MAP file.

    Lines look like:  0001:0004b2c0  func_00010200  0000000140160cc0 f  chunk.obj
    The fourth column is the loaded VA; the RVA is that minus the image base,
    which the map states as 'Preferred load address is <hex>'.
    """
    base = None
    syms = []
    for line in open(path, encoding='utf-8', errors='replace'):
        if base is None:
            m = re.search(r'Preferred load address is ([0-9A-Fa-f]+)', line)
            if m:
                base = int(m.group(1), 16)
            continue
        m = re.match(r'\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s', line)
        if m:
            syms.append((int(m.group(2), 16) - base, m.group(1)))
    syms.sort()
    return syms


def main():
    if len(sys.argv) < 2:
        print('usage: symbolize.py <run.log> [mapfile]', file=sys.stderr)
        return 2
    syms = load_map(sys.argv[2] if len(sys.argv) > 2 else 'build/twistedmetal.map')
    if not syms:
        print('no symbols parsed from the map', file=sys.stderr)
        return 1
    addrs = [a for a, _ in syms]
    print(f'{len(syms):,} symbols')

    seen = {}
    for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
        m = re.search(r'tid (\d+) ret rva=0x([0-9A-Fa-f]+)', line)
        if not m:
            continue
        tid, rva = m.group(1), int(m.group(2), 16)
        i = bisect.bisect_right(addrs, rva) - 1
        if i < 0:
            continue
        name, off = syms[i][1], rva - addrs[i]
        # A huge offset means the RVA is past that symbol's real extent.
        seen.setdefault(tid, []).append(f'{name}+0x{off:X}' if off else name)

    for tid, frames in seen.items():
        print(f'\ntid {tid}:')
        for f in frames[:24]:
            print(f'    {f}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
