#!/usr/bin/env python3
"""Verify the userdata image a flash is about to write, before it is written.

`./unixnu build userdata` writes OS and SystemOS; scripts/append-data-window.py appends the 32 MiB
Data seed and rewrites the SHNGROOT manifest; scripts/make-clover-sparse.py wraps the result as an
all-RAW Android sparse file because fastboot cannot read 9.4 GB in one host read(). The image is
then flashed over the whole userdata partition, which is destructive, so the last chance to notice
a manifest that names the wrong window, a seed that was damaged after it was appended, or an image
that grew past the partition is on the host.

This reads the container the way a flasher would (header, then every RAW chunk) and checks:

  * the sparse header's expanded size, block size and chunk types;
  * SHNGROOT magic/version/digest, exactly OS + SystemOS + Data, and each entry's flags, alignment
    and extent inside both the image and the partition;
  * the Data seed's sha256 from the manifest against the bytes the image really carries;
  * the seed's own shape: an APFS container (NXSB) with a page-aligned block size;
  * `--crc` additionally recomputes the sparse header's CRC-32 over the expanded image.

Optionally `--apfs` extracts the seed to a temporary file and asks hdiutil for the volume UUID, so
the UUID the kernel will search for is proven against the bytes, not only against seed.json.

Usage:
    verify-userdata-window.py --image out/clover/userdata/userdata-root.sparse.img
    verify-userdata-window.py --image ... --crc
    verify-userdata-window.py --image ... --apfs
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

SPARSE_MAGIC = 0xED26FF3A
CHUNK_TYPE_RAW = 0xCAC1
HEADER = struct.Struct("<IHHHHIIII")
CHUNK = struct.Struct("<HHII")
FILE_HDR_SZ = 28
CHUNK_HDR_SZ = 12

MIB = 1 << 20
HEADER_BYTES = 4096
ENTRY_BYTES = 128
ENTRY_BASE = 24
BLOCK_BYTES = 512
# board.toml [userdata]: (last_lba - first_lba + 1) * lba_bytes.
PARTITION_BYTES = (0x747BFDE - 0xF10520 + 1) * BLOCK_BYTES
# The pinned seed of round159 (build/owner-data/seed.json).
DATA_BYTES = 32 * MIB
DATA_SHA256 = "ef00c083fc54453c4ff6062c2e6947ddbba63e96aabcfd27d2273a53e4d3918c"
DATA_UUID = "FC1A580E-2D67-48FC-B8CF-87422A319340"


def fail(message: str):
    print(f"verify-userdata-window: FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


class Container:
    """An all-RAW Android sparse file, read by expanded offset."""

    def __init__(self, path: pathlib.Path):
        self.path = path
        self.file = path.open("rb")
        raw = self.file.read(FILE_HDR_SZ)
        if len(raw) != FILE_HDR_SZ:
            fail(f"{path} is shorter than a sparse header")
        (self.magic, major, _minor, self.file_hdr_sz, self.chunk_hdr_sz, self.block_size,
         self.total_blocks, self.total_chunks, self.checksum) = HEADER.unpack(raw)
        if self.magic != SPARSE_MAGIC or major != 1:
            fail(f"not a v1 sparse image: magic=0x{self.magic:08x} major={major}")
        if self.file_hdr_sz != FILE_HDR_SZ or self.chunk_hdr_sz != CHUNK_HDR_SZ:
            fail(f"header sizes {self.file_hdr_sz}/{self.chunk_hdr_sz}, expected 28/12")
        if self.block_size != 4096:
            fail(f"block size {self.block_size}, expected 4096")
        self.expanded = self.total_blocks * self.block_size
        self.chunks = []  # (expanded offset, expanded bytes, file payload offset)
        file_offset = FILE_HDR_SZ
        expanded = 0
        for index in range(self.total_chunks):
            self.file.seek(file_offset)
            head = self.file.read(CHUNK_HDR_SZ)
            if len(head) != CHUNK_HDR_SZ:
                fail(f"chunk {index} header is cut short")
            ctype, _reserved, chunk_blocks, total_sz = CHUNK.unpack(head)
            if ctype != CHUNK_TYPE_RAW:
                fail(f"chunk {index} type 0x{ctype:04x}; this port's writer emits only RAW")
            payload = chunk_blocks * self.block_size
            if total_sz != CHUNK_HDR_SZ + payload:
                fail(f"chunk {index} total_sz {total_sz} for {chunk_blocks} blocks")
            self.chunks.append((expanded, payload, file_offset + CHUNK_HDR_SZ))
            expanded += payload
            file_offset += total_sz
        if expanded != self.expanded:
            fail(f"chunks expand to {expanded}, header says {self.expanded}")
        if self.path.stat().st_size != file_offset:
            fail(f"file is {self.path.stat().st_size} bytes, chunks end at {file_offset}")

    def read(self, offset: int, length: int) -> bytes:
        out = bytearray()
        end = offset + length
        if end > self.expanded:
            fail(f"read {offset:#x}+{length:#x} is past the expanded image")
        for expanded, payload, file_offset in self.chunks:
            if expanded + payload <= offset:
                continue
            if expanded >= end:
                break
            start = max(offset, expanded)
            stop = min(end, expanded + payload)
            self.file.seek(file_offset + start - expanded)
            part = self.file.read(stop - start)
            if len(part) != stop - start:
                fail(f"short read at expanded {start:#x}")
            out += part
        if len(out) != length:
            fail(f"gathered {len(out)} of {length} bytes at {offset:#x}")
        return bytes(out)

    def crc32(self) -> int:
        crc = 0
        for _expanded, payload, file_offset in self.chunks:
            self.file.seek(file_offset)
            left = payload
            while left:
                part = self.file.read(min(left, 8 * MIB))
                if not part:
                    fail("short read while hashing the expanded image")
                crc = zlib.crc32(part, crc)
                left -= len(part)
        return crc & 0xFFFFFFFF


def parse_manifest(header: bytes) -> tuple[int, list[dict]]:
    if header[:8] != b"SHNGROOT":
        fail("the expanded image does not start with SHNGROOT")
    version, count, image_bytes = struct.unpack_from("<IIQ", header, 8)
    if version != 1:
        fail(f"manifest version is {version}, not 1")
    if count != 3:
        fail(f"manifest count is {count}, expected OS + SystemOS + Data")
    if hashlib.sha256(header[:4064]).digest() != header[4064:4096]:
        fail("the manifest digest at byte 4064 does not match its own bytes")
    entries = []
    for index in range(count):
        raw = header[ENTRY_BASE + ENTRY_BYTES * index:ENTRY_BASE + ENTRY_BYTES * (index + 1)]
        name = raw[:32].split(b"\0", 1)[0].decode("ascii", "replace")
        offset, size = struct.unpack_from("<QQ", raw, 32)
        block, flags = struct.unpack_from("<II", raw, 48)
        entries.append({"name": name, "offset": offset, "size": size, "block": block,
                        "flags": flags, "sha256": raw[56:88]})
    return image_bytes, entries


def check_entries(image_bytes: int, entries: list[dict]) -> dict:
    found = {entry["name"]: entry for entry in entries}
    if sorted(found) != ["Data", "OS", "SystemOS"]:
        fail(f"manifest entries are {sorted(found)}, expected Data/OS/SystemOS")
    for name, entry in found.items():
        if entry["block"] != BLOCK_BYTES:
            fail(f"{name} block size is {entry['block']}, expected {BLOCK_BYTES}")
        want_flags = 0 if name == "Data" else 1
        if entry["flags"] != want_flags:
            fail(f"{name} flags is {entry['flags']}, expected {want_flags}")
        if entry["offset"] < HEADER_BYTES or entry["offset"] % BLOCK_BYTES:
            fail(f"{name} offset {entry['offset']:#x} is not a sector past the manifest")
        if not entry["size"] or entry["size"] % BLOCK_BYTES:
            fail(f"{name} size {entry['size']} is not a whole number of sectors")
        if entry["offset"] > image_bytes or entry["size"] > image_bytes - entry["offset"]:
            fail(f"{name} extent {entry['offset']:#x}+{entry['size']} escapes the image")
    data = found["Data"]
    if data["size"] != DATA_BYTES:
        fail(f"Data is {data['size']} bytes, the pinned seed is {DATA_BYTES}")
    if data["sha256"].hex() != DATA_SHA256:
        fail(f"Data manifest sha256 is {data['sha256'].hex()}, expected {DATA_SHA256}")
    if data["offset"] + data["size"] != image_bytes:
        fail("Data is not the last window: the backup GPT and card end must stay untouched")
    if image_bytes > PARTITION_BYTES:
        fail(f"image is {image_bytes} bytes, past the userdata partition ({PARTITION_BYTES})")
    return found


def check_seed(seed: bytes):
    if hashlib.sha256(seed).hexdigest() != DATA_SHA256:
        fail("the bytes at the Data offset do not hash to the pinned seed")
    if seed[32:36] != b"NXSB":
        fail(f"the Data seed is not an APFS container (block zero magic {seed[32:36]!r})")
    block_size = struct.unpack_from("<I", seed, 36)[0]
    if block_size < 4096 or block_size & (block_size - 1):
        fail(f"APFS container block size {block_size} is not a page-aligned power of two")


def apfs_uuid(seed: bytes) -> str:
    """Ask the host filesystem for the volume UUID of an extracted seed."""
    import plistlib

    with tempfile.TemporaryDirectory(prefix="clover-data-") as tmp:
        image = pathlib.Path(tmp) / "data.cdr"
        image.write_bytes(seed)
        attached = plistlib.loads(subprocess.run(
            ["hdiutil", "attach", "-readonly", "-nobrowse", "-plist", str(image)],
            check=True, capture_output=True).stdout)
        whole = attached["system-entities"][0]["dev-entry"]
        try:
            uuid = None
            for entity in attached["system-entities"]:
                device = entity.get("dev-entry")
                if not device or device == whole:
                    continue
                info = plistlib.loads(subprocess.run(
                    ["diskutil", "info", "-plist", device],
                    check=True, capture_output=True).stdout)
                if info.get("VolumeUUID"):
                    uuid = info["VolumeUUID"]
                    break
        finally:
            subprocess.run(["hdiutil", "detach", whole], check=False, capture_output=True)
    if uuid != DATA_UUID:
        fail(f"the seed's APFS volume UUID is {uuid}, expected {DATA_UUID}")
    return uuid


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", required=True, type=pathlib.Path,
                        help="the all-RAW sparse userdata image about to be flashed")
    parser.add_argument("--crc", action="store_true",
                        help="recompute the sparse header's CRC-32 over the whole image")
    parser.add_argument("--apfs", action="store_true",
                        help="mount the seed read-only and check its APFS volume UUID")
    parser.add_argument("--json", type=pathlib.Path, help="write the verified facts here")
    args = parser.parse_args()

    image = Container(args.image)
    if image.expanded > PARTITION_BYTES:
        fail(f"expanded image is {image.expanded} bytes, past the userdata partition")
    header = image.read(0, HEADER_BYTES)
    image_bytes, entries = parse_manifest(header)
    if image_bytes != image.expanded:
        fail(f"manifest image_bytes is {image_bytes}, the container expands to {image.expanded}")
    found = check_entries(image_bytes, entries)
    data = found["Data"]
    seed = image.read(data["offset"], data["size"])
    check_seed(seed)
    verified = {"image": str(args.image), "expanded_bytes": image.expanded,
                "manifest_sha256": hashlib.sha256(header).hexdigest(),
                "windows": [{"name": entry["name"], "offset": entry["offset"],
                             "size": entry["size"], "flags": entry["flags"],
                             "sha256": entry["sha256"].hex()} for entry in entries]}
    if args.crc:
        actual = image.crc32()
        if actual != image.checksum:
            fail(f"sparse CRC-32 is 0x{actual:08x}, header says 0x{image.checksum:08x}")
        verified["crc32"] = f"0x{actual:08x}"
        print(f"verify-userdata-window: crc32 0x{actual:08x} over {image.expanded} bytes")
    if args.apfs:
        verified["volume_uuid"] = apfs_uuid(seed)
        print(f"verify-userdata-window: APFS volume UUID {verified['volume_uuid']}")
    if args.json:
        args.json.write_text(json.dumps(verified, indent=2) + "\n")
    print(f"verify-userdata-window: PASS: {image.expanded} bytes, "
          f"{len(entries)} windows, Data {data['size']} bytes at {data['offset']:#x}, "
          f"seed sha256 {DATA_SHA256[:16]}...")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
