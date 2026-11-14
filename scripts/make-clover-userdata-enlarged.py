#!/usr/bin/env python3
"""Assemble a userdata image whose OS container has room for a Data volume.

Round128's image copied the IPSW's OS disk byte for byte. That disk's APFS
container is exactly the size of its partition (5.66 GiB) and holds one System
volume, so a boot has 214 free blocks and every launchd boot task that writes
dies on APFS's threshold_blocks=100 -- the card says so itself:

    fs_tx_enter_internal:27549: disk5s1 launchd: ENOSPC: ttype INSERT
    num_blocks 163 free_blocks 214 ... threshold_blocks 100

This script wraps an enlarged OS window into the SHNGROOT layout. The window is
prepared on the host, because growing an APFS container is diskutil's job:

    # 1. extract the OS window (6,077,580,800 B) from the round128 image
    dd if=round128-userdata-root.img of=os.img bs=4096 skip=256 count=1483784
    # 2. extend the window file to the target size and rewrite its GPT end
    python3 make-clover-userdata-enlarged.py --patch-gpt os.img 24000000000
    # 3. attach and grow the container, then give it the Data volume iOS expects
    hdiutil attach -nomount os.img
    diskutil apfs resizeContainer diskXsY 24g      # explicit size, not 0
    diskutil apfs addVolume diskX APFS Data -role D
    diskutil unmount /Volumes/Data
    hdiutil detach diskX
    # 4. trim the file back to the container's end and rewrite its GPT again
    python3 make-clover-userdata-enlarged.py --patch-gpt os.img 0
    # 5. wrap it behind the manifest, keeping SystemOS byte for byte
    python3 make-clover-userdata-enlarged.py --original round128-userdata-root.img \\
        --window os.img --output round420-userdata-root.img --verify

The manifest is the version-1 SHNGROOT layout drivers/CloverBlock reads:
"SHNGROOT", u32 version, u32 count, u64 image_bytes, then 128-byte entries
{char name[32], u64 offset, u64 size, u32 block_size, u32 flags, u8 sha256[32],
40 reserved}; byte 4064 is the SHA-256 of bytes 0..4063. Windows are 1 MiB
aligned and flagged read-only (block_size 512, flags 1).
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

MIB = 1 << 20
CHUNK = 8 << 20
SYSTEMOS_NAME = "SystemOS"


def align(n):
    return (n + MIB - 1) // MIB * MIB


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                return h.digest()
            h.update(b)


def find_window(image, name):
    """Return (offset, size) of a named window from an existing image."""
    with open(image, "rb") as f:
        man = f.read(4096)
    if man[:8] != b"SHNGROOT":
        raise SystemExit(f"{image}: no SHNGROOT manifest")
    _ver, count, _total = struct.unpack_from("<IIQ", man, 8)
    for i in range(count):
        base = 24 + i * 128
        entry = man[base:base + 32].split(b"\0")[0].decode()
        if entry == name:
            offset, size = struct.unpack_from("<QQ", man, base + 32)
            return offset, size
    raise SystemExit(f"{image}: no window named {name}")


def write_manifest(f, entries, total):
    man = bytearray(4096)
    man[0:8] = b"SHNGROOT"
    struct.pack_into("<IIQ", man, 8, 1, len(entries), total)
    for i, (name, offset, size, digest) in enumerate(entries):
        base = 24 + i * 128
        raw = name.encode()
        if len(raw) >= 32:
            raise SystemExit(f"window name {name!r} is 32 bytes or longer")
        man[base:base + len(raw)] = raw
        struct.pack_into("<QQII", man, base + 32, offset, size, 512, 1)
        man[base + 56:base + 88] = digest
    man[4064:4096] = hashlib.sha256(bytes(man[:4064])).digest()
    f.seek(0)
    f.write(man)


def copy_sparse(src_path, src_offset, dst, dst_offset, size):
    with open(src_path, "rb") as src:
        src.seek(src_offset)
        dst.seek(dst_offset)
        done = 0
        while done < size:
            want = min(CHUNK, size - done)
            b = src.read(want)
            if len(b) != want:
                raise SystemExit(f"short read at {src_offset + done}")
            if b.count(0) == want:
                dst.seek(want, 1)
            else:
                dst.write(b)
            done += want


def assemble(original, window, output, verify):
    sys_off_src, sys_size = find_window(original, SYSTEMOS_NAME)
    os_size = os.path.getsize(window)
    sys_off = align(MIB + os_size)
    total = sys_off + sys_size
    os_digest = sha256_file(window)
    with open(original, "rb") as f:
        f.seek(sys_off_src)
        h = hashlib.sha256()
        left = sys_size
        while left:
            b = f.read(min(CHUNK, left))
            h.update(b)
            left -= len(b)
        sys_digest = h.digest()
    with open(output, "wb") as dst:
        dst.truncate(total)
        write_manifest(dst, [("OS", MIB, os_size, os_digest),
                             (SYSTEMOS_NAME, sys_off, sys_size, sys_digest)], total)
        copy_sparse(window, 0, dst, MIB, os_size)
        copy_sparse(original, sys_off_src, dst, sys_off, sys_size)
    print(f"{output}: {total} bytes ({total / 2**30:.2f} GiB)")
    report = {"image_bytes": total,
              "OS": {"offset": MIB, "size": os_size, "sha256": os_digest.hex()},
              SYSTEMOS_NAME: {"offset": sys_off, "size": sys_size,
                              "sha256": sys_digest.hex()}}
    with open(os.path.splitext(output)[0] + ".json", "w") as f:
        json.dump(report, f, indent=1)
    if not verify:
        return 0
    with open(output, "rb") as f:
        man = f.read(4096)
        if hashlib.sha256(man[:4064]).digest() != man[4064:4096]:
            raise SystemExit("manifest self-hash does not verify")
        for i in range(2):
            base = 24 + i * 128
            name = man[base:base + 32].split(b"\0")[0].decode()
            offset, size, block_size, flags = struct.unpack_from("<QQII", man, base + 32)
            want = man[base + 56:base + 88]
            if block_size != 512 or flags != 1:
                raise SystemExit(f"{name}: block_size={block_size} flags={flags}")
            f.seek(offset)
            h = hashlib.sha256()
            left = size
            while left:
                b = f.read(min(CHUNK, left))
                h.update(b)
                left -= len(b)
            print(f"{name}: offset {offset} size {size} sha256 {h.hexdigest()[:12]} "
                  f"{'MATCH' if h.digest() == want else 'MISMATCH'}")
            if h.digest() != want:
                raise SystemExit(f"{name}: hash mismatch")
    return 0


def patch_gpt(path, target):
    """Rewrite the GPT so the file's last LBA is target, or the partition end if 0.

    Used after the window file is extended (backup GPT still sits at the old end)
    and again after the container is resized (trim the file back to its end).
    """
    size = os.path.getsize(path)
    last = size // 512 - 1
    with open(path, "r+b") as f:
        f.seek(512)
        h = bytearray(f.read(512))
        if h[:8] != b"EFI PART":
            raise SystemExit(f"{path}: no primary GPT")
        pe_lba, count, esz = struct.unpack_from("<QII", h, 72)
        f.seek(pe_lba * 512)
        entries = bytearray(f.read(count * esz))
        start = end = None
        for i in range(count):
            off = i * esz
            if entries[off:off + 16] == b"\0" * 16:
                continue
            start, end = struct.unpack_from("<QQ", entries, off + 32)
            break
        if start is None:
            raise SystemExit(f"{path}: no partition entry")
        if target:
            new_last = target // 512 - 1
            if new_last <= end:
                raise SystemExit(f"{path}: target {target} does not extend the partition")
            new_end = new_last - 33
            struct.pack_into("<QQ", entries, off + 32, start, new_end)
            f.truncate(target)
            size = target
        else:
            new_last = end + 33
            f.truncate((new_last + 1) * 512)
            size = (new_last + 1) * 512
        # primary header: alternate LBA, last usable LBA, entries CRC
        struct.pack_into("<Q", h, 32, new_last)
        struct.pack_into("<Q", h, 48, new_end if target else end)
        struct.pack_into("<I", h, 88, zlib.crc32(entries) & 0xFFFFFFFF)
        struct.pack_into("<I", h, 16, 0)
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF)
        f.seek(512)
        f.write(h)
        f.seek(pe_lba * 512)
        f.write(entries)
        f.seek((new_last - 32) * 512)
        f.write(entries)
        backup = bytearray(h)
        struct.pack_into("<Q", backup, 24, new_last)
        struct.pack_into("<Q", backup, 32, 1)
        struct.pack_into("<Q", backup, 72, new_last - 32)
        struct.pack_into("<I", backup, 16, 0)
        struct.pack_into("<I", backup, 16,
                         zlib.crc32(bytes(backup[:92])) & 0xFFFFFFFF)
        f.seek(new_last * 512)
        f.write(backup)
    print(f"{path}: {size} bytes, last LBA {new_last}, partition "
          f"{start}..{(new_end if target else end)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--original", help="round128 image that carries SystemOS")
    ap.add_argument("--window", help="enlarged OS window file")
    ap.add_argument("--output", help="image to write")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--patch-gpt", nargs=2, metavar=("FILE", "SIZE"),
                    help="rewrite FILE's GPT for SIZE bytes (0 = trim to partition end)")
    args = ap.parse_args()
    if args.patch_gpt:
        patch_gpt(args.patch_gpt[0], int(args.patch_gpt[1], 0))
        return 0
    if not (args.original and args.window and args.output):
        ap.error("--original, --window and --output are required")
    return assemble(args.original, args.window, args.output, args.verify)


if __name__ == "__main__":
    sys.exit(main())
