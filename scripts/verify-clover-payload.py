#!/usr/bin/env python3.13
"""Byte-verify what a clover payload volume actually carries.

Every round rebuilds the payload, and the FAT cluster chains move whenever the
root image changes size, so comparing whole images across rounds says nothing.
What matters is per file: the kernel and the ADT slots that must not change, and
the init program plus its trust cache, which must be byte-identical to what
scripts/make-clover-root.py just signed.

This reads each file out of the image with mtools (`mdir`/`mcopy`, brew install
mtools) and hashes it, so one command both checks the round and produces the
ledger numbers that go into README.md.

Usage: verify-clover-payload.py [image ...]     # default: the built payload
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys

XNU_PAYLOAD = pathlib.Path("/Users/zhouzhou/UniXNU/out/clover/payload/payload.img")
LOCAL_ROOT = pathlib.Path("/Users/zhouzhou/UniXNU-clover/build/clover-root/launchd.macho")
LOCAL_CACHE = pathlib.Path("/Users/zhouzhou/UniXNU-clover/build/clover-root/trustcache.bundle")
LOCAL_KERNEL = pathlib.Path("/Users/zhouzhou/UniXNU/out/clover/kernel/kernel.macho")

PAYLOADS = "Payloads"
FILES = ["unixnu.toml", "kernel.macho", "scratch-data.img", "kernel.macho.adt",
         "kernel.macho.trustcache"]


def mcopy_out(image: pathlib.Path, path: str) -> bytes:
    """Read one file out of the FAT image on stdout."""
    return subprocess.run(
        ["mcopy", "-i", str(image), f"::{path}", "-"],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout


def mdir(image: pathlib.Path, path: str) -> str:
    return subprocess.run(
        ["mdir", "-i", str(image), f"::{path}"],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout


def locate(image: pathlib.Path, name: str) -> str:
    """The in-volume path of `name`, found by looking at both directories."""
    for candidate in (f"EFI/UniXNU/{name}", f"EFI/UniXNU/{PAYLOADS}/{name}"):
        parent = candidate.rsplit("/", 1)[0]
        listing = mdir(image, parent)
        short = candidate.rsplit("/", 1)[1]
        if short in listing or short.split(".")[0][:6] in listing:
            return candidate
    raise KeyError(name)


def report(image: pathlib.Path) -> int:
    print(f"== {image}")
    ok = True
    blobs: dict[str, bytes] = {}
    for name in FILES:
        try:
            blobs[name] = mcopy_out(image, locate(image, name))
        except Exception as exc:  # noqa: BLE001 - report every gap, not just the first
            print(f"{name:34} MISSING ({exc})")
            ok = False
    for name, blob in blobs.items():
        print(f"{name:34} {len(blob):>9} bytes  sha256 {hashlib.sha256(blob).hexdigest()}")

    checks = [
        ("scratch-data.img == signed root", blobs.get("scratch-data.img"), LOCAL_ROOT),
        ("trustcache == published bundle", blobs.get("kernel.macho.trustcache"), LOCAL_CACHE),
    ]
    for label, inside, disk in checks:
        if inside is None or not disk.exists():
            print(f"{label:42} SKIPPED")
            ok = False
            continue
        same = inside == disk.read_bytes()
        ok &= same
        print(f"{label:42} {'OK' if same else 'DIFFERS'}")

    # The kernel slot cannot be a byte-equality check: `unixnu build payload`
    # writes the entry probes from board.toml (entry_probe_reset_*) into the
    # copy it places on the volume, so the in-volume kernel is the locally built
    # kernel plus a few dozen patched words. What proves the round is that the
    # patch stayed local: same length, and the differing bytes confined to small
    # runs. A whole-file rewrite here would mean the kernel itself changed under
    # the payload build, which is a different round's business.
    kernel = blobs.get("kernel.macho")
    label = "kernel.macho: probe patch localized"
    if kernel is None or not LOCAL_KERNEL.exists():
        print(f"{label:42} SKIPPED")
        ok = False
    else:
        disk = LOCAL_KERNEL.read_bytes()
        if len(kernel) != len(disk):
            print(f"{label:42} FAIL (length {len(kernel)} vs {len(disk)})")
            ok = False
        else:
            offs = [i for i in range(len(kernel)) if kernel[i] != disk[i]]
            runs = 1 + sum(1 for a, b in zip(offs, offs[1:]) if b - a > 64) if offs else 0
            localized = len(offs) <= 20000 and runs <= 64
            print(f"{label:42} {'OK' if localized else 'FAIL'} "
                  f"({len(offs)} bytes in {runs} runs, "
                  f"sha256 {hashlib.sha256(kernel).hexdigest()})")
            ok &= localized
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="*", type=pathlib.Path, default=[XNU_PAYLOAD])
    args = ap.parse_args()
    return min(1, sum(report(image) != 0 for image in args.image))


if __name__ == "__main__":
    sys.exit(main())
