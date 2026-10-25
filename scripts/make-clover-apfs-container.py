#!/usr/bin/env python3
"""Build the APFS container round127 flashes into logfs.

The question this image exists to ask is whether Apple's own com.apple.filesystems.apfs can drive
the IOMedia that drivers/CloverBlock publishes: attach to it, read a container superblock through
it, and carry the read far enough to publish a volume. Nothing about that question needs a big
container, and logfs is exactly 16384 sectors, so the target here is the smallest shape apfs will
accept: one container, one volume, one marker file.

Two properties of the result matter to the board and are checked below:

  * the container starts at byte 0 of the image. The superblock is block 0 of the container, and
    apfs reads it at media offset 0 -- there is no partition map and no gap in front of it, because
    drivers/CloverBlock hands the whole 8 MiB window to IOKit as the media. Verified by locating
    `NXSB` at 0x20 inside the first 4096-byte block, the block size next to it, and the block
    count for the image size.
  * the image is exactly the partition's size. `fastboot flash logfs` takes a file no larger than
    the partition, and a container that thinks it owns 8 MiB must not be told it owns less.

The container is built with the host's own diskutil, which means the bytes are the same bytes
macOS writes for a real APFS media: the kext on the board is the same product, so this image is
what it expects to find rather than something this port hand-assembled.

Usage:
    python3.13 scripts/make-clover-apfs-container.py [--output build/clover-apfs]

Outputs:
    <output>/logfs.img        the raw container, 8388608 bytes, flash to logfs
    <output>/report.json      size, sha256, the superblock fields read back, the volume name
"""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

IMAGE_BYTES = 8 * 1024 * 1024
BLOCK_SIZE = 4096
VOLUME_NAME = "CLOVER127"
MARKER_NAME = "MARKER.txt"
MARKER_TEXT = "unixnu clover round127 apfs attach probe\n"
SUPERBLOCK_MAGIC_OFFSET = 0x20


def run(args, capture=True):
    result = subprocess.run(args, text=True, capture_output=capture)
    if result.returncode != 0:
        detail = (result.stdout or "") + (result.stderr or "") if capture else ""
        raise SystemExit(f"{' '.join(args)} failed: {detail.strip()}")
    return result.stdout or ""


def device_of(text, pattern):
    match = re.search(pattern, text)
    if not match:
        raise SystemExit(f"no {pattern!r} in: {text.strip()}")
    return match.group(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="build/clover-apfs")
    args = parser.parse_args()

    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    image = out / "logfs.img"
    scratch = out / "container-work.img"

    # A run that died midway can leave this file mounted; unlinking an attached image works, but
    # the fresh container below would then be built onto a stale device.
    subprocess.run(["hdiutil", "detach", str(scratch), "-quiet"], text=True, capture_output=True)
    scratch.unlink(missing_ok=True)
    # A blank image with no partition map and no filesystem: the media apfs sees is this file.
    run(["diskutil", "image", "create", "blank", "--size", str(IMAGE_BYTES),
         "--fs", "None", "--format", "RAW", str(scratch)])
    attached = device_of(run(["diskutil", "image", "attach", "--noMount", str(scratch)]),
                         r"(/dev/disk\d+)")

    container = None
    volume_mount = None
    try:
        made = run(["diskutil", "apfs", "createContainer", attached])
        container = device_of(made, r"Created new APFS Container (disk\d+)")
        run(["diskutil", "apfs", "addVolume", container, "APFS", VOLUME_NAME])
        # addVolume mounts the new volume in the standard place, which is how the marker gets in.
        volume_mount = pathlib.Path("/Volumes") / VOLUME_NAME
        if not volume_mount.is_dir():
            run(["diskutil", "mount", f"{container}s1"])
        (volume_mount / MARKER_NAME).write_text(MARKER_TEXT)
        run(["diskutil", "unmount", str(volume_mount)])
        # APFS writes its checkpoint when the volume unmounts; the container must be closed before
        # the file is read, so detach first and inspect the bytes afterwards.
        run(["hdiutil", "detach", attached, "-quiet"])
        attached = None
    finally:
        if attached:
            subprocess.run(["hdiutil", "detach", attached, "-quiet"], text=True,
                           capture_output=True)

    raw = scratch.read_bytes()
    if len(raw) != IMAGE_BYTES:
        raise SystemExit(f"container image is {len(raw)} bytes, expected {IMAGE_BYTES}")
    magic = raw[SUPERBLOCK_MAGIC_OFFSET:SUPERBLOCK_MAGIC_OFFSET + 4]
    if magic != b"NXSB":
        raise SystemExit(f"no APFS superblock at media offset 0x{SUPERBLOCK_MAGIC_OFFSET:x}: "
                         f"{magic!r}")
    block_size = int.from_bytes(raw[0x24:0x28], "little")
    block_count = int.from_bytes(raw[0x28:0x30], "little")
    if block_size != BLOCK_SIZE:
        raise SystemExit(f"container block size is {block_size}, expected {BLOCK_SIZE}")
    if block_count != IMAGE_BYTES // BLOCK_SIZE:
        raise SystemExit(f"container claims {block_count} blocks, image holds "
                         f"{IMAGE_BYTES // BLOCK_SIZE}")

    scratch.replace(image)
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    report = {
        "image": str(image),
        "bytes": IMAGE_BYTES,
        "sha256": digest,
        "container_block_size": block_size,
        "container_block_count": block_count,
        "volume_name": VOLUME_NAME,
        "marker": MARKER_NAME,
        "flash": f"fastboot flash logfs {image}",
    }
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
