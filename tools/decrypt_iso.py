#!/usr/bin/env python3
"""Decrypt a raw PS3 disc image (redump-style ISO).

A PS3 Blu-ray keeps its UDF metadata and the first few files (PS3_DISC.SFB,
PARAM.SFO) in plain sectors and AES-128-CBC encrypts everything else --
EBOOT.BIN included. Sector 0 holds the region table that says which is which,
so an encrypted image still *lists* fine in 7-Zip while every extracted file
past the first region is garbage.

No keys ship with this repo. Supply the disc key yourself:

    --key  <32 hex>   the derived disc key (what PS3Dec calls the "dec" key)
    --dkey <32 hex>   raw d1 from a .dkey file; derived using PS3_DISC_ERK and
                      PS3_DISC_ERK_IV from the environment

Usage:
    python tools/decrypt_iso.py game.iso --key <32hex> -o game.dec.iso
    python tools/decrypt_iso.py --selftest
"""

import argparse
import os
import struct
import sys

from Crypto.Cipher import AES

SECTOR = 2048


def parse_regions(sector0):
    """Return [(first_sector, last_sector, encrypted), ...] from the region table.

    Layout: u32 count of plain regions, u32 reserved, then 2*count u32 sector
    boundaries. Regions alternate plain/encrypted; the encrypted ones sit
    strictly between two plain boundaries.
    """
    count = struct.unpack('>I', sector0[0:4])[0]
    if not 1 <= count <= 64:
        raise ValueError(f'implausible plain-region count {count}; not a raw PS3 disc image?')
    bounds = list(struct.unpack(f'>{count * 2}I', sector0[8:8 + count * 8]))
    regions = []
    for i in range(len(bounds) - 1):
        encrypted = i % 2 == 1
        first = bounds[i] + 1 if encrypted else bounds[i]
        last = bounds[i + 1] - 1 if encrypted else bounds[i + 1]
        if last >= first:
            regions.append((first, last, encrypted))
    return regions


def derive_key(d1, erk, erk_iv):
    """PS3Dec's derivation: the disc key is d1 encrypted under the fixed ERK."""
    return AES.new(erk, AES.MODE_CBC, erk_iv).encrypt(d1)


def decrypt_sector(key, lba, data):
    iv = struct.pack('>QQ', 0, lba)
    return AES.new(key, AES.MODE_CBC, iv).decrypt(data)


def decrypt_image(src, dst, key, log=None):
    total = os.path.getsize(src) // SECTOR
    with open(src, 'rb') as fi, open(dst, 'wb') as fo:
        regions = parse_regions(fi.read(SECTOR))
        fi.seek(0)
        for first, last, encrypted in regions:
            last = min(last, total - 1)
            if last < first:
                continue
            if log:
                log(f'{"decrypt" if encrypted else "copy   "} sectors '
                    f'{first:#x}..{last:#x} ({(last - first + 1) * SECTOR / 1e9:.2f} GB)')
            fi.seek(first * SECTOR)
            fo.seek(first * SECTOR)
            lba = first
            while lba <= last:
                # 4096 sectors at a time keeps the read syscalls cheap; CBC is
                # still per-sector because the IV is the sector number.
                chunk = fi.read(min(4096, last - lba + 1) * SECTOR)
                if not chunk:
                    break
                if encrypted:
                    chunk = b''.join(
                        decrypt_sector(key, lba + i, chunk[i * SECTOR:(i + 1) * SECTOR])
                        for i in range(len(chunk) // SECTOR))
                fo.write(chunk)
                lba += len(chunk) // SECTOR
                if log and encrypted and lba % (4096 * 128) < 4096:
                    log(f'  {(lba - first) / (last - first + 1):.0%}')
    return regions


def selftest():
    """Round-trip a synthetic 8-sector image through the region parser + decryptor."""
    key = bytes(range(16))
    bounds = [0, 1, 6, 7]  # plain 0-1, encrypted 2-5, plain 6-7
    sector0 = struct.pack('>II', 2, 0) + struct.pack('>4I', *bounds)
    sector0 += b'\0' * (SECTOR - len(sector0))

    plain = [sector0] + [bytes([i]) * SECTOR for i in range(1, 8)]
    assert parse_regions(sector0) == [(0, 1, False), (2, 5, True), (6, 7, False)]

    enc = list(plain)
    for lba in range(2, 6):
        enc[lba] = AES.new(key, AES.MODE_CBC, struct.pack('>QQ', 0, lba)).encrypt(plain[lba])

    src, dst = '_selftest.iso', '_selftest.dec.iso'
    try:
        open(src, 'wb').write(b''.join(enc))
        decrypt_image(src, dst, key)
        assert open(dst, 'rb').read() == b''.join(plain), 'round-trip mismatch'
    finally:
        for f in (src, dst):
            if os.path.exists(f):
                os.remove(f)
    print('selftest ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('iso', nargs='?', help='raw (encrypted) PS3 disc image')
    ap.add_argument('-o', '--output', help='default: <iso>.dec.iso')
    ap.add_argument('--key', help='derived disc key, 32 hex chars')
    ap.add_argument('--dkey', help='raw d1 from a .dkey file, 32 hex chars')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return 0
    if not args.iso:
        ap.error('an iso is required')

    if args.key:
        key = bytes.fromhex(args.key)
    elif args.dkey:
        erk, erk_iv = os.environ.get('PS3_DISC_ERK'), os.environ.get('PS3_DISC_ERK_IV')
        if not (erk and erk_iv):
            ap.error('--dkey needs PS3_DISC_ERK and PS3_DISC_ERK_IV in the environment')
        key = derive_key(bytes.fromhex(args.dkey), bytes.fromhex(erk), bytes.fromhex(erk_iv))
    else:
        ap.error('one of --key or --dkey is required')
    if len(key) != 16:
        ap.error('disc key must be 16 bytes (32 hex chars)')

    out = args.output or os.path.splitext(args.iso)[0] + '.dec.iso'
    decrypt_image(args.iso, out, key, log=lambda m: print(m, file=sys.stderr, flush=True))
    print(out)

    # A correct key makes the UDF metadata in the encrypted region readable;
    # the cheapest end-to-end proof is that EBOOT.BIN now starts with "SCE\0".
    print('now check: 7z e -so <out> PS3_GAME/USRDIR/EBOOT.BIN | head -c4  ->  SCE\\0',
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
