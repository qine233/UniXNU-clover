#!/usr/bin/env python3
"""Build the clover accelerator shell and re-audit everything it binds to.

drivers/CloverAccel publishes the IOAcceleratorES service Metal's discovery looks for
(IOServiceMatching("IOAcceleratorES") -> MetalPluginName -> the plugin bundle). Its base
classes, IOService and IOUserClient, are kernel-resident, so unlike CloverBlock this archive
needs no runtime superclass pointer and no alias for a constructor the kernel does not export:
the object's imports are checked against kernel.macho one by one.

What it does keep from the other drivers is the late registration. The OSDefineMetaClass*
macros put the class' registration in __DATA,__mod_init_func, which OSRuntimeInitializeCPP
walks at libkern init before any archive-provided class is meant to exist; clover_accel_run()
calls the same initializer itself once the storage hand-off has finished, so the class is
registered exactly once and at a point where IOService is certainly alive. This script strips
the section, globalizes the initializer, proves the instructions did not change, and archives
as build/clover-accel/libCloverAccel.a.

Run it as a `[kernel] archives` entry of board.toml, i.e. from the assembled build tree.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/clover-accel'
CLASS = 'IOAcceleratorES'
INIT = '__GLOBAL__sub_I_CloverAccel.cpp'
# IOService is compiled into the kernel, so its constructor, allocator and the platform getter
# are all exported; anything undefined beyond these is a mistake this build stops on.
LATE_IMPORTS = set()


def run(command, **kwargs):
    kwargs.setdefault('capture_output', True)
    try:
        return subprocess.run([str(v) for v in command], check=True, timeout=600, **kwargs)
    except subprocess.CalledProcessError as error:
        sys.stderr.write((error.stderr or b'').decode() if isinstance(error.stderr, bytes) else (error.stderr or ''))
        raise


def tool(name):
    """PATH first, then the keg-only Homebrew LLVM location `unixnu build kernel` puts on PATH."""
    found = shutil.which(name)
    if found:
        return found
    for directory in ('/opt/homebrew/opt/llvm/bin', '/usr/local/opt/llvm/bin'):
        if (Path(directory) / name).exists():
            return str(Path(directory) / name)
    raise SystemExit(f'{name} not found on PATH (brew install llvm)')


def require(ok, message):
    if not ok:
        raise SystemExit(message)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference', type=Path, default=ROOT.parent / 'kernel/kernel.macho',
                    help='prelinked kernel this build must match')
args = parser.parse_args()

sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import exports, macho

OUT.mkdir(parents=True, exist_ok=True)

# ---------------------------------------------------------------- compile
record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
require(record_path.exists(), f'{record_path} missing: run this from the assembled build tree')
record = json.loads(record_path.read_text().strip().rstrip(','))
source = ROOT / 'drivers/CloverAccel/CloverAccel.cpp'
require(source.exists(), f'{source} missing')
obj = OUT / f'{CLASS}.o'
command, arguments, i = [], record['arguments'], 0
while i < len(arguments):
    arg = arguments[i]
    if arg == record['file']:
        command.append(source)
    elif arg == '-o':
        command += ['-o', obj]
        i += 1
    elif arg == '-Xclang' and arguments[i + 1].startswith('-W'):
        i += 1
    elif not arg.startswith('-W'):
        command.append(arg)
    i += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-unnecessary-virtual-specifier']
# A full kernel pass writes this record without -isysroot (it compiles with -nostdlibinc and
# BUILD_WERROR=0, so the host SDK default costs it nothing); with -Werror added here that default becomes
# incompatible-sysroot against the record's own arm64-apple-ios target. Pin the iOS SDK when the record
# did not. See scripts/build-arm-driver.py for the long form of this note.
if not any(str(arg).startswith(('-isysroot', '--sysroot')) for arg in command):
    command += ['-isysroot', run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).stdout.strip()]
run(command, cwd=record['directory'])
raw = obj.read_bytes()
info = macho(raw)
require(info['kind'] == 1, 'Require an actual ARM64 subtype-0 Mach-O object')
build_version = [struct.unpack_from('<2I', raw, at + 8) for cmd, at, _ in info['commands'] if cmd == 0x32]
require(build_version == [(2, 0x110000)], 'Require the iOS 17 kernel build target')

# ---------------------------------------------------------------- symbols and reference
require(args.reference.exists(), f'{args.reference} missing: build the kernel once without this knob first')
reference = args.reference.read_bytes()
meta = macho(reference)
native_names = {name: va for va, names in exports(reference, meta).items() for name in names}

sym_at = next(at for cmd, at, _ in info['commands'] if cmd == 2)
at, count, strings, _ = struct.unpack_from('<4I', raw, sym_at + 8)
symbols = []
for index in range(count):
    ix, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, at + index * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), address, kind))
defined = {name for name, _, kind in symbols if kind & 0x0e == 0x0e}
undefined = {name for name, _, kind in symbols if kind & 0x0e == 0x00 and kind & 0x01}

unexpected = sorted(undefined - LATE_IMPORTS - set(native_names))
require(not unexpected, f'Unresolved imports outside the audited bindings: {unexpected}')
require(LATE_IMPORTS <= undefined, f'The late imports disappeared: {sorted(LATE_IMPORTS - undefined)}')

# Both classes the file defines have to still be in the object's own symbol table, and the
# initializer the run calls by name has to exist.
for name in ('__ZTV15IOAcceleratorES', '__ZTV19AGXDeviceUserClient', INIT, '_clover_accel_run'):
    require(name in defined, f'{name} missing from the object')

# ---------------------------------------------------------------- late registration
late = OUT / f'{CLASS}.late.o'
run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=' + INIT, obj, late])


def text_section(blob):
    section = next(s for segment in macho(blob)['segments'] for s in segment['sections'] if s['name'] == '__text')
    return blob[section['fileoff']:section['fileoff'] + section['size']]


late_raw = late.read_bytes()
require(text_section(raw) == text_section(late_raw), 'Late registration cannot change instructions')
late_defined = set()
info = macho(late_raw)
sym_at = next(at for cmd, at, _ in info['commands'] if cmd == 2)
at, count, strings, _ = struct.unpack_from('<4I', late_raw, sym_at + 8)
for index in range(count):
    ix, kind, _, _, address = struct.unpack_from('<IBBHQ', late_raw, at + index * 16)
    name = late_raw[strings + ix:late_raw.index(b'\0', strings + ix)].decode()
    if kind & 0x0e == 0x0e:
        late_defined.add(name)
require(INIT in late_defined, f'{INIT} is not global after the strip')

archive = OUT / 'libCloverAccel.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late])

report = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
          'reference': str(args.reference),
          'reference_sha256': hashlib.sha256(reference).hexdigest(),
          'late_imports': sorted(LATE_IMPORTS),
          'initializer': INIT}
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'accel: {archive} ({archive.stat().st_size} B)')
print(f"accel: reference {args.reference.name} {report['reference_sha256'][:16]}…, "
      f'{len(undefined)} imports all exported, {INIT} late')
print('PASS: the shell binds only to kernel-resident IOUserClient/IOService')
