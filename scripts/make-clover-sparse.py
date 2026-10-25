#!/usr/bin/env python3.13
"""Wrap a raw partition image in an Android sparse container, byte for byte.

Two reasons this exists, and the second one is why every chunk is RAW:

macOS caps a single read() at 2147483647 bytes (measured: a 9.4 GB request
returns 2 GiB minus one), and fastboot's raw-image path reads the whole image in
one call, so any image over 2 GiB fails on the host before a byte reaches the
board -- `fastboot: error: Failed reading from userdata`. The sparse path does
not: libsparse hands fastboot one chunk at a time, so the same bytes flash.

fastboot's own `-S SIZE` reaches the same code, but it builds the container by
scanning for runs of zero and turning them into DONT_CARE chunks, which are
*skipped* on the device. On a card that still holds Android's /data, "skipped"
means the container's zero blocks stay stale ext4 bytes. The userdata image this
repo builds is Apple's own disk bytes, and a claim of "unchanged" is only worth
what the reader can verify, so this writer emits only CHUNK_TYPE_RAW: nothing is
skipped, nothing is inferred, and the expanded image is the input image.

Format: system/core/libsparse/sparse_format.h, v1.0 as AOSP writes it -- 28-byte
file header, 12-byte chunk headers, blocks of --block-size (4096 default, the
partition sector size times 8), and a header image_checksum of the standard
CRC-32 over the expanded data. No per-chunk CRC32 chunks: those are only read to
be verified, and a wrong one fails the flash mid-partition, which is the one
outcome worse than no flash at all.

Usage:
    make-clover-sparse.py IMAGE.img IMAGE.sparse.img   # write the container
    make-clover-sparse.py --verify IMAGE.img IMAGE.sparse.img
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import struct
import sys
import zlib

SPARSE_MAGIC = 0xED26FF3A
CHUNK_TYPE_RAW = 0xCAC1
HEADER = struct.Struct("<IHHHHIIII")
CHUNK = struct.Struct("<HHII")
FILE_HDR_SZ = 28
CHUNK_HDR_SZ = 12


def expand_size(path: pathlib.Path) -> int:
    return path.stat().st_size


def write_sparse(image: pathlib.Path, out: pathlib.Path, block_size: int, blocks_per_chunk: int) -> int:
    """Stream `image` into `out` as all-RAW chunks; return the chunk count."""
    size = expand_size(image)
    if size % block_size:
        raise SystemExit(f"{image} is not a multiple of {block_size}; a short tail would be zero-padded")
    total_blks = size // block_size
    chunk_bytes = block_size * blocks_per_chunk
    chunks = -(-total_blks // blocks_per_chunk)

    crc = 0
    written_blks = 0
    count = 0
    with image.open("rb") as src, out.open("wb") as dst:
        # The header's checksum field is filled after the fact: the CRC is over
        # the expanded data, which is only known at the end.
        dst.write(HEADER.pack(SPARSE_MAGIC, 1, 0, FILE_HDR_SZ, CHUNK_HDR_SZ, block_size,
                              total_blks, chunks, 0))
        while written_blks < total_blks:
            want = min(chunk_bytes, (total_blks - written_blks) * block_size)
            data = src.read(want)
            if len(data) != want:
                raise SystemExit(f"short read at block {written_blks}: {len(data)} of {want}")
            crc = zlib.crc32(data, crc)
            dst.write(CHUNK.pack(CHUNK_TYPE_RAW, 0, want // block_size, CHUNK_HDR_SZ + want))
            dst.write(data)
            written_blks += want // block_size
            count += 1
        dst.seek(HEADER.size - 4)
        dst.write(struct.pack("<I", crc & 0xFFFFFFFF))
    return count


def verify(image: pathlib.Path, out: pathlib.Path, block_size: int) -> bool:
    """Re-read the container the way a flasher would and hash every region."""
    size = expand_size(image)
    ok = True
    with out.open("rb") as src:
        magic, major, minor, file_hdr_sz, chunk_hdr_sz, blk_sz, total_blks, total_chunks, checksum = (
            HEADER.unpack(src.read(FILE_HDR_SZ)))
        problems = []
        if magic != SPARSE_MAGIC or major != 1:
            problems.append(f"magic=0x{magic:08x} major={major}")
        if file_hdr_sz != FILE_HDR_SZ or chunk_hdr_sz != CHUNK_HDR_SZ or blk_sz != block_size:
            problems.append(f"hdr sizes {file_hdr_sz}/{chunk_hdr_sz}, blk {blk_sz}")
        if total_blks * blk_sz != size:
            problems.append(f"expanded {total_blks * blk_sz} != image {size}")
        for p in problems:
            print(f"FAIL header: {p}")
        ok = not problems
        seen = blocks = 0
        # Compare against the image in blocks, hashing rather than buffering so a
        # 9 GB container checks in memory it can actually hold.
        with image.open("rb") as raw:
            while seen < total_chunks:
                ctype, _reserved, chunk_sz, total_sz = CHUNK.unpack(src.read(CHUNK_HDR_SZ))
                if ctype != CHUNK_TYPE_RAW:
                    print(f"FAIL chunk {seen}: type 0x{ctype:04x}, this writer emits only RAW")
                    return False
                if total_sz != CHUNK_HDR_SZ + chunk_sz * blk_sz:
                    print(f"FAIL chunk {seen}: total_sz {total_sz} for {chunk_sz} blocks")
                    return False
                payload = src.read(chunk_sz * blk_sz)
                expect = raw.read(chunk_sz * blk_sz)
                if hashlib.sha256(payload).digest() != hashlib.sha256(expect).digest():
                    print(f"FAIL chunk {seen} at block {blocks}: bytes differ from the image")
                    ok = False
                blocks += chunk_sz
                seen += 1
        if blocks != total_blks:
            print(f"FAIL: chunks cover {blocks} blocks, header says {total_blks}")
            ok = False
    print(f"{'OK' if ok else 'FAIL'} verify: {seen} RAW chunks, {blocks} blocks, "
          f"crc32 0x{checksum:08x}")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", type=pathlib.Path)
    ap.add_argument("sparse", type=pathlib.Path)
    ap.add_argument("--block-size", type=int, default=4096)
    ap.add_argument("--chunk-blocks", type=int, default=8192, help="blocks per RAW chunk (32 MiB default)")
    ap.add_argument("--verify", action="store_true", help="re-read the container and compare")
    ap.add_argument("--force", action="store_true", help="overwrite an existing container")
    args = ap.parse_args()

    if args.verify:
        return 0 if verify(args.image, args.sparse, args.block_size) else 1
    if args.sparse.exists() and not args.force:
        raise SystemExit(f"{args.sparse} exists; pass --force to rewrite it")
    count = write_sparse(args.image, args.sparse, args.block_size, args.chunk_blocks)
    print(f"{args.sparse}: {count} RAW chunks, "
          f"{expand_size(args.sparse)} bytes wrapping {expand_size(args.image)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
