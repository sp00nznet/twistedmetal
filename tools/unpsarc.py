#!/usr/bin/env python3
"""Extract a PlayStation archive (.psarc) next to itself as a loose directory.

Twisted Metal's ArchiveLoader reads an archive's contents.dat out of the .psarc
and then opens the archive's members through FIOS at the *mounted* path --
`ui//ui/ui.ngp` and friends. That mount is served by FIOS's dearchiver, which
this port does not get far enough to set up, so every member open comes back
0x80010709 and the load bar never completes:

    ArchiveLoader (ui//ui.psarc)... Failed to load ui//ui/!

Extracting the archive to the directory the game asks for lets those opens hit
the media layer directly, which works. Nothing here is game-specific: PSARC
v1.4 is a TOC of MD5-named entries plus a table of zlib block sizes, and entry
0 is the newline-separated name list for the rest.

    python tools/unpsarc.py input/PS3_GAME/USRDIR/ui/ui.psarc

writes input/PS3_GAME/USRDIR/ui/ui/{contents.dat,ui.ngp,ui.ptr,ui.vram}.
Extracted game data is derived from the disc: it stays out of the repository.
"""
import os
import struct
import sys
import zlib


def read_toc(f):
    magic, ver, comp, toclen, entsz, nfiles, blocksz, flags = struct.unpack('>4sI4sIIIII', f.read(32))
    if magic != b'PSAR':
        raise SystemExit('not a PSARC: %r' % magic)
    if comp != b'zlib':
        raise SystemExit('unsupported compression %r' % comp)
    entries = []
    for _ in range(nfiles):
        e = f.read(entsz)
        entries.append((struct.unpack('>I', e[16:20])[0],          # first block
                        int.from_bytes(e[20:25], 'big'),            # uncompressed size
                        int.from_bytes(e[25:30], 'big')))           # offset
    width = 2 if blocksz <= 0x10000 else 3
    raw = f.read(toclen - 32 - nfiles * entsz)
    blocks = [int.from_bytes(raw[i:i + width], 'big') for i in range(0, len(raw), width)]
    return entries, blocks, blocksz


def extract(f, entry, blocks, blocksz, out):
    first, usize, off = entry
    f.seek(off)
    written, b = 0, first
    while written < usize:
        n = blocks[b]
        b += 1
        chunk = f.read(n if n else blocksz)
        # A stored size of 0 means a whole uncompressed block; otherwise the
        # block is deflated -- except when it did not compress, in which case
        # it is stored raw at its own size. The zlib header tells them apart.
        if n and len(chunk) >= 2 and chunk[0] & 0x0F == 8 and ((chunk[0] << 8) | chunk[1]) % 31 == 0:
            chunk = zlib.decompress(chunk)
        out.write(chunk)
        written += len(chunk)
    return written


def main():
    if len(sys.argv) != 2:
        print('usage: unpsarc.py <archive.psarc>', file=sys.stderr)
        return 2
    path = sys.argv[1]
    outdir = os.path.splitext(path)[0]
    os.makedirs(outdir, exist_ok=True)

    with open(path, 'rb') as f:
        entries, blocks, blocksz = read_toc(f)
        names = ['contents.dat']          # replaced by entry 0's own list
        buf = bytearray()

        class Sink:
            def write(self, b): buf.extend(b)
        extract(f, entries[0], blocks, blocksz, Sink())
        names = bytes(buf).decode('ascii').split('\n')

        for name, entry in zip(names, entries[1:]):
            dst = os.path.join(outdir, name)
            os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
            with open(dst, 'wb') as o:
                n = extract(f, entry, blocks, blocksz, o)
            print('%-20s %12d bytes' % (name, n))
    print('-> %s' % outdir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
