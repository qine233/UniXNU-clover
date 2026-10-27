#!/usr/bin/env python3
"""Append the round159 Data window to the clover userdata image.

`./unixnu build userdata` writes the manifest the device reads (SHNGROOT, 4096 bytes) followed by the
OS and SystemOS disk images, and it writes every entry with flags=1 (read-only). The Data volume is
the one window the owner mount attaches read-write, so it cannot be produced by that packer: this
script appends the 32 MiB APFS seed built by scripts/prepare-owner-data-fixture.py after the last
window, adds it as entry 3 with flags=0, rewrites count/image_bytes, and re-signs the manifest digest
at byte 4064. It then reads the result back the way CloverBlock does and refuses to report success
unless every field matches.

Run it on the image out/clover/userdata/userdata-root.img, then convert that image to the all-RAW
sparse file the flash step uses (scripts/make-clover-sparse.py). Nothing here touches a device.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys

MIB = 1 << 20
HEADER_BYTES = 4096
ENTRY_BYTES = 128
ENTRY_BASE = 24
BLOCK_BYTES = 512
NAME = "Data"
# The seed pinned by build/owner-data/seed.json for round159.
SEED_BYTES = 32 * MIB
SEED_SHA256 = "ef00c083fc54453c4ff6062c2e6947ddbba63e96aabcfd27d2273a53e4d3918c"
SEED_UUID = "FC1A580E-2D67-48FC-B8CF-87422A319340"
# (last_lba - first_lba + 1) * lba_bytes of board.toml, round158's numbers.
MAX_BYTES = (0x747BFDE - 0xF10520 + 1) * 512


def fail(message: str):
    print(f"append-data-window: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_header(header: bytes) -> tuple[int, list[dict]]:
    if header[:8] != b"SHNGROOT":
        fail("the image does not start with an SHNGROOT manifest")
    version, count, image_bytes = struct.unpack_from("<IIQ", header, 8)
    if version != 1:
        fail(f"manifest version is {version}, not 1")
    if not 1 <= count <= 4:
        fail(f"manifest count is {count}, outside 1..4")
    entries = []
    for index in range(count):
        raw = header[ENTRY_BASE + ENTRY_BYTES * index:ENTRY_BASE + ENTRY_BYTES * (index + 1)]
        name = raw[:32].split(b"\0", 1)[0].decode("ascii", "replace")
        offset, size = struct.unpack_from("<QQ", raw, 32)
        block, flags = struct.unpack_from("<II", raw, 48)
        entries.append({"name": name, "offset": offset, "size": size, "block": block,
                        "flags": flags, "sha256": raw[56:88], "raw": raw})
    if hashlib.sha256(header[:4064]).digest() != header[4064:4096]:
        fail("the existing manifest digest at byte 4064 does not match its own bytes")
    return image_bytes, entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=pathlib.Path,
                        help="the userdata image to extend in place")
    parser.add_argument("--seed", required=True, type=pathlib.Path,
                        help="the 32 MiB APFS Data seed (owner-data-32.cdr)")
    parser.add_argument("--seed-json", type=pathlib.Path,
                        help="seed.json; when given, its UUID/size/sha256 must match")
    parser.add_argument("--report", type=pathlib.Path,
                        help="userdata-root.json to update")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    seed = args.seed.read_bytes()
    digest = hashlib.sha256(seed).digest()
    if len(seed) != SEED_BYTES:
        fail(f"seed is {len(seed)} bytes, the pinned Data window is {SEED_BYTES}")
    if digest.hex() != SEED_SHA256:
        fail(f"seed sha256 is {digest.hex()}, the pinned value is {SEED_SHA256}")
    if args.seed_json:
        spec = json.loads(args.seed_json.read_text())
        for key, want in (("bytes", SEED_BYTES), ("sha256", SEED_SHA256), ("volume_uuid", SEED_UUID)):
            if spec.get(key) != want:
                fail(f"{args.seed_json} says {key}={spec.get(key)!r}, expected {want!r}")

    with args.image.open("rb") as image:
        header = image.read(HEADER_BYTES)
    if len(header) != HEADER_BYTES:
        fail("the image is shorter than one manifest block")
    image_bytes, entries = parse_header(header)
    size = args.image.stat().st_size
    names = [entry["name"] for entry in entries]
    for needed in ("OS", "SystemOS"):
        if needed not in names:
            fail(f"the manifest has no {needed} entry; this is not round158's image")
    for entry in entries:
        if entry["name"] == NAME:
            if (entry["offset"] == size - SEED_BYTES and entry["size"] == SEED_BYTES and
                    entry["flags"] == 0 and entry["sha256"] == digest and image_bytes == size):
                # The manifest can agree with itself while the appended bytes were damaged after
                # the fact; the window is only already there if the bytes still hash to the seed.
                with args.image.open("rb") as image:
                    image.seek(entry["offset"])
                    if hashlib.sha256(image.read(SEED_BYTES)).digest() != digest:
                        fail("the manifest says Data holds this seed, but the bytes do not")
                print(f"append-data-window: {args.image} already carries Data at "
                      f"{entry['offset']:#x} ({SEED_BYTES} bytes)")
                return 0
            fail("the manifest already has a Data entry that is not this seed")
    if image_bytes != size:
        fail(f"image_bytes says {image_bytes}, the file is {size} bytes")
    if size % MIB:
        fail(f"the image ends at {size:#x}, not on a 1 MiB boundary")
    if size + SEED_BYTES > MAX_BYTES:
        fail(f"the Data window would end at {size + SEED_BYTES}, past the userdata partition "
             f"({MAX_BYTES} bytes)")
    if len(entries) >= 4:
        fail("the manifest has no free entry slot")

    new_bytes = size + SEED_BYTES
    new_header = bytearray(HEADER_BYTES)
    new_header[:ENTRY_BASE] = header[:ENTRY_BASE]
    for index, entry in enumerate(entries):
        new_header[ENTRY_BASE + ENTRY_BYTES * index:ENTRY_BASE + ENTRY_BYTES * (index + 1)] = entry["raw"]
    struct.pack_into("<32sQQII32s40s", new_header, ENTRY_BASE + ENTRY_BYTES * len(entries),
                     NAME.encode(), size, SEED_BYTES, BLOCK_BYTES, 0, digest, bytes(40))
    struct.pack_into("<IIQ", new_header, 8, 1, len(entries) + 1, new_bytes)
    new_header[4064:4096] = hashlib.sha256(bytes(new_header[:4064])).digest()

    print(f"append-data-window: {args.image}: {size} -> {new_bytes} bytes, "
          f"{NAME} at {size:#x} sha256 {digest.hex()}")
    if args.dry_run:
        print("append-data-window: dry run, nothing written")
        return 0

    with args.image.open("r+b") as image:
        image.seek(size)
        image.write(seed)
        image.truncate(new_bytes)
        image.seek(0)
        image.write(new_header)

    # Read the result back the way drivers/CloverBlock parses it, not the buffers above.
    with args.image.open("rb") as image:
        reread = image.read(HEADER_BYTES)
        declared, result = parse_header(reread)
        image.seek(size)
        readback = image.read(SEED_BYTES)
    if declared != new_bytes:
        fail(f"read-back image_bytes is {declared}, expected {new_bytes}")
    if [entry["name"] for entry in result] != names + [NAME]:
        fail(f"read-back entries are {[entry['name'] for entry in result]}")
    data_entry = result[-1]
    if (data_entry["offset"] != size or data_entry["size"] != SEED_BYTES or
            data_entry["block"] != BLOCK_BYTES or data_entry["flags"] != 0 or
            data_entry["sha256"] != digest):
        fail("read-back Data entry does not carry the seed's offset/size/flags/digest")
    if hashlib.sha256(readback).digest() != digest:
        fail("the bytes at the Data offset are not the seed")
    if args.image.stat().st_size != new_bytes:
        fail("the image size does not match the manifest")

    if args.report:
        report = json.loads(args.report.read_text())
        digest_image = hashlib.sha256()
        with args.image.open("rb") as image:
            for chunk in iter(lambda: image.read(8 * MIB), b""):
                digest_image.update(chunk)
        report["image_bytes"] = new_bytes
        report["image_sha256"] = digest_image.hexdigest()
        report["manifest_sha256"] = hashlib.sha256(bytes(new_header)).hexdigest()
        report["windows"] = [{"name": entry["name"], "offset": entry["offset"],
                              "size": entry["size"],
                              "sha256": bytes(entry["sha256"]).hex()} for entry in result]
        report["flash_command"] = f'fastboot flash userdata {args.image}'
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(f"append-data-window: rewrote {args.report}")

    print("append-data-window: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
