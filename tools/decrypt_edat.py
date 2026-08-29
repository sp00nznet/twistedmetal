#!/usr/bin/env python3
"""Decrypt a PS3 SDATA (.sdat) container to plaintext.

Twisted Metal ships its configuration as tmxconfig.sdat, an NPD container with
SDAT_FLAG set — the self-decryptable variant that uses a fixed key and needs no
NPDRM licence. The chain is:

    crypt_key = npd.dev_hash XOR SDAT_KEY
    block_key = dev_hash[0:12] || be32(block_index)
    key       = AES-128-ECB-encrypt(crypt_key, block_key)
    if flags & 0x08 (ENCRYPTED_KEY): key = AES-128-CBC-decrypt(EDAT_KEY_0, 0, key)
    plaintext = AES-128-CBC-decrypt(key, npd.digest, ciphertext)

Flag 0x20 changes only the block layout: a 0x20-byte metadata record precedes
every block instead of one table before the data. Compression (flag 0x01) is not
handled.

The keys here are the published fixed SDATA constants, not per-title secrets —
the same ones ps3recomp's sdata_decrypt.h carries.

    python tools/decrypt_edat.py input/PS3_GAME/USRDIR/tmxconfig.sdat -o out.xml
"""

import argparse
import os
import struct
import sys

from Crypto.Cipher import AES

SDAT_KEY   = bytes.fromhex('0D655EF8E674A98AB8505CFA7D012933')
EDAT_KEY_0 = bytes.fromhex('BE959CA8308DEFA2E5E180C63712A9AE')
ZERO_IV    = bytes(16)


def decrypt(d):
    if len(d) < 0x100 or d[:3] != b'NPD':
        raise ValueError('not an NPD container')
    digest   = d[0x40:0x50]
    dev_hash = d[0x60:0x70]
    flags, block_size = struct.unpack_from('>II', d, 0x80)
    file_size, = struct.unpack_from('>Q', d, 0x88)

    if not flags & 0x01000000:
        raise ValueError(f'not SDATA (flags {flags:#010x}); needs an NPDRM licence')
    if flags & 0x01:
        raise ValueError('compressed EDAT is not supported')
    meta_0x20 = bool(flags & 0x20)
    enc_key = bool(flags & 0x08)

    crypt_key = bytes(a ^ b for a, b in zip(dev_hash, SDAT_KEY))
    total = (file_size + block_size - 1) // block_size
    out = bytearray()

    for bn in range(total):
        off = (0x100 + bn * (0x20 + block_size) + 0x20) if meta_0x20 \
              else (0x100 + total * 0x10 + bn * block_size)
        length = block_size
        if bn == total - 1 and file_size % block_size:
            length = file_size % block_size
        padded = (length + 15) & ~15

        b_key = dev_hash[:12] + struct.pack('>I', bn)
        key = AES.new(crypt_key, AES.MODE_ECB).encrypt(b_key)
        if enc_key:
            key = AES.new(EDAT_KEY_0, AES.MODE_CBC, ZERO_IV).decrypt(key)
        out += AES.new(key, AES.MODE_CBC, digest).decrypt(d[off:off + padded])[:length]

    return bytes(out[:file_size])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('sdat')
    ap.add_argument('-o', '--output', help='default: <input>.dec')
    args = ap.parse_args()

    plain = decrypt(open(args.sdat, 'rb').read())
    out = args.output or os.path.splitext(args.sdat)[0] + '.dec'
    open(out, 'wb').write(plain)
    print(f'{out}  {len(plain)} bytes, starts {plain[:16]!r}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
