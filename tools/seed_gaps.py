#!/usr/bin/env python3
"""Cover the holes find_functions leaves in the executable segment.

find_functions detects 31,032 functions in this binary but only accounts for
88% of the code region: 7,197 gaps totalling 1.57 MB sit between the functions
it found. Both boot blockers traced so far came out of those gaps.

  * A gap immediately after a function truncates it. func_0076C534 was detected
    as 0x0076C534..0x0076CAFC while the real function runs to 0x0076CDF4, so the
    lifter emitted the rest as disconnected fragments and one of them fell off
    the end of its C function instead of continuing -- the FIOS scheduler's
    workers were never constructed.
  * Code inside a gap is never lifted at all, so an indirect call into it hits
    the runtime's "unresolved indirect call" path and returns without doing
    anything. Seven such targets at 0x009D38E4..0x009D3A64 spun forever.

This emits an augmented function list that covers every gap, optionally split
at addresses harvested from a run log, so the lifter emits real entry points for
them. Feed the result back to ppu_lifter with --functions.

    python tools/seed_gaps.py --targets meta/unresolved_targets.txt \\
        --out meta/functions.seeded.json
"""

import argparse
import json
import re
import sys

# The executable span: .init through the end of .sceStub.text. Past this is
# .rodata, which must not be lifted as code (it is also what --code-end pins).
CODE_LO, CODE_HI = 0x10200, 0xC79C6C


def load_targets(paths):
    """Addresses to force into their own function entry, from files of hex
    literals or from raw run logs containing 'indirect call -> 0xADDR'."""
    out = set()
    for p in paths:
        text = open(p, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'0x([0-9A-Fa-f]{6,8})', text):
            a = int(m.group(1), 16)
            if CODE_LO <= a < CODE_HI and a % 4 == 0:
                out.add(a)
    return out


def gaps_of(funcs):
    spans = sorted((s, e) for s, e in funcs if e > CODE_LO and s < CODE_HI)
    out, cur = [], CODE_LO
    for s, e in spans:
        if s > cur:
            out.append((cur, s))
        cur = max(cur, e)
    if cur < CODE_HI:
        out.append((cur, CODE_HI))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--functions', default='meta/functions.json')
    ap.add_argument('--targets', action='append', default=[],
                    help='file of hex addresses, or a run log (repeatable)')
    ap.add_argument('--out', default='meta/functions.seeded.json')
    args = ap.parse_args()

    orig = json.load(open(args.functions))
    funcs = [(int(f['start'], 16), int(f['end'], 16)) for f in orig]
    known = {s for s, _ in funcs}

    gaps = gaps_of(funcs)
    targets = load_targets(args.targets)

    added = []
    for lo, hi in gaps:
        # Split the gap at every harvested target inside it, so each becomes a
        # real entry point rather than an address in the middle of one blob.
        cuts = sorted({lo} | {t for t in targets if lo < t < hi})
        for i, s in enumerate(cuts):
            e = cuts[i + 1] if i + 1 < len(cuts) else hi
            if e > s and s not in known:
                added.append({'start': f'0x{s:08X}', 'end': f'0x{e:08X}'})

    hit = sum(1 for t in targets if any(int(a['start'], 16) == t for a in added))
    merged = sorted(orig + added, key=lambda f: int(f['start'], 16))
    json.dump(merged, open(args.out, 'w'), indent=1)

    covered = sum(hi - lo for lo, hi in gaps)
    print(f'{len(orig):,} functions in, {len(gaps):,} gaps covering {covered:,} bytes')
    print(f'{len(targets):,} target(s) supplied, {hit:,} promoted to their own entry')
    print(f'wrote {args.out}: {len(merged):,} functions')
    return 0


if __name__ == '__main__':
    sys.exit(main())
