#!/usr/bin/env python3
"""Build the init program and the trust cache of the clover md0/mockfs root.

The round this feeds: the kernel has no block driver on this tablet (eMMC needs
an SDHCI driver XNU does not have), so IOFindBSDRoot sits in its
"Waiting on <IOMedia UUID ...>" loop forever. XNU's other root path needs no
driver at all: boot-arg rd=md0 makes IOFindBSDRoot take the physical range
DarwinBoot publishes at /chosen/memory-map/RAMDisk, hand it to mdevadd, and
return md0 as the root device. The filesystem then comes from mockfs, which is
registered in this kernel's vfsconf table (vfc_mountroot = _mockfs_mountroot)
and turns the root block device into "/", "/dev" and "/sbin/launchd" -- where
the launchd IS the device, reinterpreted as one Mach-O.

So the payload's initrd is this script's launchd.macho, and the only thing it
has to satisfy is XNU's exec of /sbin/launchd. Two requirements follow:

  * it must be static. There is no dyld, no shared cache and no /usr/lib on a
    mockfs root, so anything dynamic cannot be loaded. This mirrors the
    upstream QEMU fixture, scripts/qemu-entry/native-user.S.
  * its CodeDirectory must be trusted. AMFI is live (the boot already passed
    it), so exec of a binary whose cdhash is not in the boot trust cache is
    refused. codesign --sign - gives an ad-hoc signature whose cdhash this
    script reads back and packs into an Apple trust cache bundle, the same
    format upstream uses in scripts/build-native-user-fixture.py:

        bundle  = <count=1, type=8>            (two u32, little endian)
        module  = <version=2, 16-byte uuid, n> + n entries
        entry   = <20-byte cdhash> + <type=2> + 3 padding bytes

    The uuid is uuid5(NAMESPACE_URL, "<identifier>:<cdhash hex>"), which is how
    the kernel identifies a module in a trust cache.

Outputs, under --output (default build/clover-root):
  launchd.macho      the signed init program; the payload passes it as
                     --data-ramdisk, so it becomes Payloads/scratch-data.img and
                     the initrd of the boot entry
  trustcache.bundle  the one-entry trust cache; the payload passes it as
                     --trust-cache
  report.json        sizes, sha256s, the cdhash, and the board.toml line that
                     pins the bundle

The build is reproducible: an ad-hoc signature carries no timestamp, so the
same source and the same SDK produce the same bytes and the same cdhash.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'scripts/clover-el0-init.S'
IDENTIFIER = 'org.unixnu.clover-init'
IOS_MIN = '17.0'          # matches the kernel's iOS 17 (21H461) target
PAGE_SIZE = 4096          # the signing block size, and this port's page size


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f'{command[0]} failed:\n{result.stdout}{result.stderr}')
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/clover-root')
    parser.add_argument('--sdk', help='iPhoneOS SDK path (default: xcrun --sdk iphoneos)')
    args = parser.parse_args()

    sdk = args.sdk or run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).strip()
    if not Path(sdk).is_dir():
        raise SystemExit(f'not an SDK: {sdk}')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    object_file = out / 'clover-el0-init.o'
    run(['xcrun', 'clang', '-target', f'arm64-apple-ios{IOS_MIN}', '-isysroot', sdk,
         '-c', str(SOURCE), '-o', str(object_file)])
    linked = out / 'launchd-static.macho'
    run(['xcrun', 'ld', '-arch', 'arm64', '-static', '-e', '_start',
         '-platform_version', 'ios', IOS_MIN, IOS_MIN, str(object_file), '-o', str(linked)])
    signed = out / 'launchd.macho'
    # vtool, not just the linker flag: the upstream fixture does the same, and a
    # mach-o whose build version comes only from -platform_version has been seen
    # to carry the wrong minos in the LC_BUILD_VERSION that AMFI reads.
    run(['xcrun', 'vtool', '-set-build-version', 'ios', IOS_MIN, IOS_MIN,
         '-replace', '-output', str(signed), str(linked)])
    run(['codesign', '--force', '--sign', '-', '--pagesize', str(PAGE_SIZE),
         '--identifier', IDENTIFIER, str(signed)])
    run(['codesign', '--verify', '--strict', str(signed)])
    details = run(['codesign', '--display', '--verbose=4', str(signed)])
    match = re.search(r'^CDHash=([0-9a-f]{40})$', details, re.M)
    if not match:
        raise SystemExit('codesign did not report a CDHash')
    page = re.search(r'^Page size=(\d+)$', details, re.M)
    if not page or int(page[1]) != PAGE_SIZE:
        raise SystemExit(f'CodeDirectory page size is {page and page[1]}, not {PAGE_SIZE}')
    cdhash = bytes.fromhex(match[1])

    module_id = uuid.uuid5(uuid.NAMESPACE_URL, f'{IDENTIFIER}:{cdhash.hex()}')
    module = struct.pack('<I16sI', 2, module_id.bytes, 1) + cdhash + bytes([2, 0, 0, 0])
    bundle = struct.pack('<2I', 1, 8) + module
    (out / 'trustcache.bundle').write_bytes(bundle)

    image = signed.read_bytes()
    if image[:4] not in (b'\xcf\xfa\xed\xfe',):
        raise SystemExit('the init program is not a 64-bit Mach-O')
    report = {
        'purpose': 'init program of the md0/mockfs root; exits, so the kernel reports initproc exited',
        'source': str(SOURCE.relative_to(ROOT)),
        'launchd_sha256': hashlib.sha256(image).hexdigest(),
        'launchd_bytes': len(image),
        'cdhash': cdhash.hex(),
        'trust_module_uuid': str(module_id),
        'own_bundle_sha256': hashlib.sha256(bundle).hexdigest(),
        'own_bundle_bytes': len(bundle),
        'signing_page_size': PAGE_SIZE,
        'ios_min_version': IOS_MIN,
        'apple_trust_chain_verified': False,
    }
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'launchd.macho {len(image)} bytes sha256 {report["launchd_sha256"][:16]}… '
          f'cdhash {cdhash.hex()}')
    print(f'own trust module bundle {len(bundle)} bytes sha256 {report["own_bundle_sha256"][:16]}…')
    print('boot trust cache: python3 scripts/make-clover-trust-bundle.py')
    return 0


if __name__ == '__main__':
    sys.exit(main())
