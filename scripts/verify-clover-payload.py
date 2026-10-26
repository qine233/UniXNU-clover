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
import json
import pathlib
import subprocess
import sys

XNU_PAYLOAD = pathlib.Path("/Users/zhouzhou/UniXNU/out/clover/payload/payload.img")
LOCAL_ROOT = pathlib.Path("/Users/zhouzhou/UniXNU-clover/build/clover-root/launchd.macho")
LOCAL_CACHE = pathlib.Path("/Users/zhouzhou/UniXNU-clover/build/clover-root/trustcache.bundle")
LOCAL_KERNEL = pathlib.Path("/Users/zhouzhou/UniXNU/out/clover/kernel/kernel.macho")
# The assembled build tree: scripts/build-clover-pmgr.py records the original-PMGR addresses it
# audited here, and kits/NativePrelink (a symlink into the UniXNU base) parses Mach-O for it.
TREE = pathlib.Path("/Users/zhouzhou/UniXNU/out/clover/tree")
PMGR_REPORT = TREE / "build/clover-pmgr/build-report.json"

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


def check_pmgr(kernel: bytes) -> bool:
    """The round83 provider binds the original ApplePMGR by absolute address, so the addresses
    written into build/clover-pmgr/original-bindings.S have to still name those symbols in the
    kernel this round actually linked. A layout shift would otherwise ship a power manager that
    dispatches into the wrong function, and the only sign on the panel would be round82's
    `AMFI: No PMGR?` again -- indistinguishable from never having published a nub."""
    label = "kernel.macho: original-PMGR bindings alive"
    if not PMGR_REPORT.exists():
        print(f"{label:42} SKIPPED (no {PMGR_REPORT})")
        return False
    report = json.loads(PMGR_REPORT.read_text())
    # Our own helper aliases are absolute symbols this object defines for the link; they are not
    # exported by the kernel and cannot be looked up in it.
    originals = {name: int(value, 16) for name, value in report["bindings"].items()
                 if not name.startswith("_clover_")}
    sys.path.insert(0, str(TREE / "kits/NativePrelink"))
    from native_prelink import exports, macho  # noqa: PLC0415 - needs the tree on sys.path
    names = {name: va for va, found in exports(kernel, macho(kernel)).items() for name in found}
    drifted = sorted(name for name, va in originals.items() if names.get(name) != va)
    linked = b"CloverPMGR: failed at stage" in kernel
    ok = not drifted and linked
    print(f"{label:42} {'OK' if ok else 'FAIL'} "
          f"({len(originals)} bindings, {len(drifted)} moved"
          f"{', ' + ', '.join(drifted) if drifted else ''}; provider text "
          f"{'present' if linked else 'absent'})")
    return ok


def check_sdhci(kernel: bytes) -> bool:
    """drivers/CloverSdhci is linked as a plain archive, not with -force_load (see
    configs/clover_defconfig), so nothing but a reference keeps it: the link would silently drop
    the whole eMMC instrument if the call in sheng_pmgr_run() ever went away. These eight facts
    prove the member is in this image -- the boot argument it parses, the dump each mode builds,
    mode 4's "no pwr" verdict (an address taken in a ternary, so never folded), the panic every
    failure path takes, and the four symbols this image must export (run, block read, block
    write, panic append). The CMD13
    report strings used to
    be checked here, but the panic hook forced the whole driver to -mgeneral-regs-only, and
    without SIMD clang now folds those two twelve-byte literals into immediate stores: their
    characters are in the text (see cmd13Probe) but the rodata strings are gone."""
    label = "kernel.macho: CloverSdhci linked"
    found = {name: name.encode() in kernel
             for name in ("clover-emmc", "look-only dump of c0c4000",
                          "no pwr", "CloverSdhci: failed at stage",
                          "_clover_emmc_run", "_clover_emmc_block_read",
                          "_clover_emmc_block_write", "_clover_emmc_panic_append")}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}" for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_block(kernel: bytes) -> bool:
    """drivers/CloverBlock is the other plain archive in the link, kept alive only because
    CloverSdhci's mode 21 tail names clover_block_run(). Round128's probe is the same shape one
    partition over: the boot argument that selects the root image, the panic line that carries the
    depth, the container-scheme class name it walks the registry for, both window accessors
    CloverSdhci has to have linked in for the geometry to exist at all, and the two magics it reads
    (the SHNGROOT manifest and the GPT header the window turns out to carry). Round129 adds the
    second half of the same line: the volume class whose media it mounts, the filesystem name handed
    to vfs_rootmountalloc() and the path it walks once the mount is up. Round130 adds the one rung
    that needed a struct field -- the exported vnode_setmountedon() it links against. Round131 adds no
    import and no symbol: it works straight off measured offsets, so the only way to see that it asks
    its question is the flag and vfsops slots on its panic line. Round132 adds neither an import nor an
    offset: it reads four registry strings off the volume node and the container above it, so its
    evidence is the identity keys in the string table and the seven volume fields on the same line. Any
    of them going missing means the round silently asks nothing -- the failure shape check-clover.py
    refuses to let the archives have."""
    label = "kernel.macho: CloverBlock linked"
    found = {name: name.encode() in kernel
             for name in ("clover-apfs", "CloverBlock r132: apfs",
                          "fl=0x%x fc=0x%x fk=0x%x", "op=0x%x:0x%x mm=0x%x:0x%x",
                          "vt=%u un=%u vs=0x%x vo=0x%x vu=0x%x vc=0x%x", "FullName", "Sealed",
                          "AppleAPFSContainerScheme", "AppleAPFSVolume",
                          "SystemVersion.plist", "_clover_emmc_block_window",
                          "_clover_emmc_block_userdata_window", "SHNGROOT", "EFI PART",
                          "_clover_block_run", "_vnode_setmountedon")}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}" for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


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
    if kernel is not None:
        ok &= check_pmgr(kernel)
        ok &= check_sdhci(kernel)
        ok &= check_block(kernel)
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="*", type=pathlib.Path, default=[XNU_PAYLOAD])
    args = ap.parse_args()
    return min(1, sum(report(image) != 0 for image in args.image))


if __name__ == "__main__":
    sys.exit(main())
