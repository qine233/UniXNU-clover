#!/usr/bin/env python3
"""Build the r430 V2 gate program: the userspace JIT probe.

This is deliberately a separate program from scripts/clover-el0-init.S. That
file has not been pid 1 since r137 removed rd=md0, so a probe added there never
runs. XNU's DEVELOPMENT-only offline-air-init hook execs
/private/var/tmp/air-native as the bootstrapper instead; this builder produces
exactly that file, signed ad hoc with 4 KiB pages so its cdhash can be added to
the owner trust module.

Output (default build/clover-jit):
  air-native       the signed Mach-O, to be copied into the Data volume
  air-native.txt   size, sha256 and CDHash, for the round's notes
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'scripts/clover-jit-probe.S'
IDENTIFIER = 'org.unixnu.clover-jit-probe'
IOS_MIN = '17.0'
PAGE_SIZE = 4096


def run(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        sys.stderr.write(''.join(f'+ {word}\n' for word in command))
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f'command failed with {result.returncode}')
    return result.stdout + result.stderr


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT / 'build/clover-jit')
    parser.add_argument('--sdk', help='iPhoneOS SDK path (default: xcrun --sdk iphoneos)')
    args = parser.parse_args()

    sdk = args.sdk or run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).strip()
    if not Path(sdk).is_dir():
        raise SystemExit(f'not an SDK: {sdk}')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    obj = out / 'clover-jit-probe.o'
    run(['xcrun', 'clang', '-target', f'arm64-apple-ios{IOS_MIN}', '-isysroot', sdk,
         '-c', str(SOURCE), '-o', str(obj)])
    linked = out / 'air-native.linked'
    run(['xcrun', 'ld', '-arch', 'arm64', '-static', '-e', '_start',
         '-platform_version', 'ios', IOS_MIN, IOS_MIN, str(obj), '-o', str(linked)])
    versioned = out / 'air-native.versioned'
    run(['xcrun', 'vtool', '-set-build-version', 'ios', IOS_MIN, IOS_MIN,
         '-replace', '-output', str(versioned), str(linked)])
    signed = out / 'air-native'
    signed.write_bytes(versioned.read_bytes())
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
    data = signed.read_bytes()
    if data[:4] != b'\xcf\xfa\xed\xfe':
        raise SystemExit('the probe is not a 64-bit Mach-O')
    report = (f'air-native {len(data)} bytes sha256 {hashlib.sha256(data).hexdigest()} '
              f'cdhash {match[1]}\n')
    (out / 'air-native.txt').write_text(report)
    print(report, end='')


if __name__ == '__main__':
    main()
