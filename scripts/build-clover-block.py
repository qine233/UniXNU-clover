#!/usr/bin/env python3
"""Build the clover eMMC block device and re-audit everything it binds to.

drivers/CloverBlock publishes the card as an IOBlockStorageDevice so that XNU's own
IOBlockStorageDriver and IOMedia sit above drivers/CloverSdhci's proven PIO read. The
superclass lives in IOStorageFamily, which is loaded at runtime, so kernel.macho exports no
IOBlockStorageDevice symbol at all: this class implements every virtual slot it owns and the
two compiler-emitted base lifecycle symbols are aliased to IOService's here.

The audit is the point of this script. It compiles the driver with the same captured command
`unixnu build kernel` used for IOPlatformExpert.cpp (plus the staged storage headers), then:

  * checks the object is ARM64 subtype 0 for the iOS 17 kernel,
  * requires every one of the 25 iOS IOBlockStorageDevice virtuals plus getMetaClass and the
    destructor to appear in the vtable, so no inherited slot can name an unexported original,
  * resolves every vtable entry and every undefined symbol against kernel.macho's exports,
    with exactly two exceptions: the aliased base constructor/destructor and the three
    clover_emmc_block_* entry points that libCloverSdhci.a provides at kernel link time,
  * proves --remove-section=__DATA,__mod_init_func changes no instruction,
  * archives as build/clover-block/libCloverBlock.a.

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
OUT = ROOT / 'build/clover-block'
STORAGE_ABI = ROOT / 'build/storage-abi/include'
CLASS = 'CloverBlockDevice'

# Both symbols come from the compiler's view of IOBlockStorageDevice, whose real definitions
# are in IOStorageFamily and are not exported. The base constructor only forwards its
# metaclass and the base destructor is empty; init() is reproduced in the driver.
BASE_ALIASES = {
    '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': '__ZN9IOServiceC2EPK11OSMetaClass',
    '__ZN20IOBlockStorageDeviceD2Ev': '__ZN9IOServiceD2Ev',
}
# Resolved when the kernel links libCloverSdhci.a next to this archive, never by the kernel.
EMMC_IMPORTS = {
    '_clover_emmc_block_ready',
    '_clover_emmc_block_sectors',
    '_clover_emmc_block_window',
    '_clover_emmc_block_userdata_window',
    '_clover_emmc_block_read',
    '_clover_emmc_block_write',
    # Round171: the card's write cache is on, so the device's sync and idle call-outs commit it
    # through these two entry points instead of pretending the media is synchronous.
    '_clover_emmc_flush_cache',
    '_clover_emmc_cache_stats',
}
# Everything IOBlockStorageDevice owns on iOS: the four macOS-only deprecated virtuals are
# compiled out by TARGET_OS_OSX, and APPLE_KEXT_VTABLE_PADDING is 0, so the slot list is exactly
# these plus getMetaClass and the destructor.
REQUIRED = (
    'init', 'getProperty', 'setProperties',
    'doEjectMedia', 'doFormatMedia', 'doGetFormatCapacities', 'doSynchronizeCache',
    'getVendorString', 'getProductString', 'getRevisionString', 'getAdditionalDeviceInfoString',
    'reportBlockSize', 'reportEjectability', 'reportMaxValidBlock', 'reportMediaState',
    'reportRemovability', 'reportWriteProtection', 'getWriteCacheState', 'setWriteCacheState',
    'doAsyncReadWrite', 'requestIdle', 'doUnmap', 'doSetPriority', 'doSynchronize',
    'doGetProvisionStatus', 'getMetaClass',
)


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
require(STORAGE_ABI.exists(), f'{STORAGE_ABI} missing: the storage ABI overlay was not staged')
record = json.loads(record_path.read_text().strip().rstrip(','))
source = ROOT / 'drivers/CloverBlock/CloverBlock.cpp'
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
# The storage overlay must win over any tree header with the same name.
command = command[:1] + ['-I', STORAGE_ABI] + command[1:]
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-unnecessary-virtual-specifier']
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

unexpected = sorted(undefined - set(BASE_ALIASES) - EMMC_IMPORTS - set(native_names))
require(not unexpected, f'Unresolved imports outside the audited bindings: {unexpected}')
require(set(BASE_ALIASES) <= undefined, 'Compiler no longer emits the aliased base lifecycle calls')
require(EMMC_IMPORTS <= undefined, f'CloverSdhci imports changed: {sorted(undefined & EMMC_IMPORTS)}')
for alias, target in BASE_ALIASES.items():
    require(target in native_names, f'Alias target {target} is not exported by the reference kernel')

# ---------------------------------------------------------------- vtable
vtable = next((va for name, va, _ in symbols if name == f'__ZTV{len(CLASS)}{CLASS}'), None)
require(vtable is not None, f'Own vtable __ZTV{len(CLASS)}{CLASS} missing from the object')
section = next((s for segment in info['segments'] for s in segment['sections']
                if s['va'] <= vtable < s['va'] + s['size']), None)
require(section is not None, 'Vtable is not inside a section')
relocations = {}
at, rcount = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
for index in range(rcount):
    offset, bits = struct.unpack_from('<2I', raw, at + index * 8)
    if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
        relocations[section['va'] + offset] = symbols[bits & 0xffffff][0]

# Address point follows the offset-to-top and typeinfo words. Every slot is a relocation, so
# the first missing address is the end of the table.
entries = []
while vtable + 16 + 8 * len(entries) in relocations and len(entries) <= 512:
    entries.append(relocations[vtable + 16 + 8 * len(entries)])
require(len(entries) > 40, f'Implausible vtable length {len(entries)}')
require(all('20IOBlockStorageDevice' not in name for name in entries),
        'A vtable slot still names an IOBlockStorageDevice method instead of an override')
require(all(name in defined or name in native_names for name in entries),
        'A vtable entry resolves to neither this object nor an exported kernel symbol')

demangled = run([tool('llvm-cxxfilt')], input='\n'.join(entries), text=True).stdout.splitlines()
require(len(demangled) == len(entries), 'Demangler output does not line up with the vtable')
table = set(demangled)
missing = [name for name in REQUIRED if not any(f'{CLASS}::{name}(' in entry for entry in table)]
require(not missing, f'Overrides missing from the vtable: {missing}')
require(any(f'{CLASS}::~{CLASS}(' in entry for entry in table),
        'The class destructor is not in the vtable')

# ---------------------------------------------------------------- bind and archive
asm = OUT / 'base-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va}\n' for name, va in BASE_ALIASES.items()))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).stdout.strip()
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', asm,
     '-o', OUT / 'base-bindings.o'])
late = OUT / f'{CLASS}.late.o'
run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_CloverBlock.cpp', obj, late])


def text_section(blob):
    section = next(s for segment in macho(blob)['segments'] for s in segment['sections'] if s['name'] == '__text')
    return blob[section['fileoff']:section['fileoff'] + section['size']]


require(text_section(raw) == text_section(late.read_bytes()), 'Late registration cannot change instructions')
archive = OUT / 'libCloverBlock.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'base-bindings.o'])

report = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
          'reference': str(args.reference),
          'reference_sha256': hashlib.sha256(reference).hexdigest(),
          'base_aliases': BASE_ALIASES,
          'emmc_imports': sorted(EMMC_IMPORTS),
          'virtual_entries': len(entries),
          'required_overrides': list(REQUIRED),
          'runtime_qualified': False}
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'block: {archive} ({archive.stat().st_size} B)')
print(f"block: reference {args.reference.name} {report['reference_sha256'][:16]}…, "
      f'{len(entries)} vtable entries, {len(REQUIRED)} required overrides')
print('PASS: every slot is an override of a runtime class, every import is accounted for')
