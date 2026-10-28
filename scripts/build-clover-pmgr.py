#!/usr/bin/env python3
"""Build the opt-in clover original-PMGR integration and re-audit its bindings.

The unchanged target AMFI waits for an `ApplePMGRNub` service (round82's panel: `"AMFI: No
PMGR?"`). The board has no Apple PMGR, so we publish the original nub over a *guarded*
provider: the reconstructed ApplePMGR ABI with every virtual slot except the pair AMFI itself
uses overridden to panic. Same shape as UniXNU-sheng's scripts/build-sheng-platform-power.py.

Two things differ from sheng:

  * sheng audits a reference candidate built by its own pipeline; every address it pins is
    re-checked here against `out/clover/kernel/kernel.macho`, the kernel this board actually
    links, so nothing is trusted on provenance alone.
  * the archive travels under `build/sheng-platform-power/libShengPlatformPower.a`, which is
    the path `scripts/build-xnu-4k-baseline.sh` force-loads when the tree is configured with
    `CONFIG_XNU_SHENG_PMGR=y`. The name is the base repository's, not a choice.

Run it as a `[kernel] archives` entry of board.toml, i.e. from the assembled build tree, so
the captured compile command of IOPlatformExpert.cpp is available.
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
OUT = ROOT / 'build/clover-pmgr'
ARCHIVE_DIR = ROOT / 'build/sheng-platform-power'
CLASS = 'CloverPlatformPower'

# Round82 verified these against out/clover/kernel/kernel.macho byte by byte; the checks below
# re-run against whatever reference is passed, so a layout change stops the build here instead
# of producing a kernel that dies on the panel.
PINS = {
    'constructor': 0xfffffff0049f003c,
    'destructor': 0xfffffff0049f019c,
    'gMetaClass': 0xfffffff006adf490,
    'nub_create': 0xfffffff004a22568,
    'nub_read': 0xfffffff004a24adc,
    'nub_write': 0xfffffff004a230ec,
    'base_vptr': 0xfffffff005ce26d8,
    'aon_string': 0xfffffff0042dde02,
    'amfi_site': 0xfffffff004aaeb40,
    'amfi_read_slot': 0xfffffff005cfac08,
    'amfi_write_slot': 0xfffffff005cfac10,
}
EXPECT_WORDS = {
    'constructor': (0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x91412808, 0x9116c114,
                    0x94010904, 0xd0009788, 0x911b6108, 0xf9000008, 0xb0ffc5a8, 0x91331508),
    'destructor': (0x140108b5, 0xd4200020, 0xd4200020, 0xf0010760),
    'nub_create': (0xa9bd57f6, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0103f4, 0xaa0003f5),
    'nub_read': (0xf9404400, 0xb4000080, 0xf9400008, 0xf9450101, 0xd61f0020, 0x12800000, 0xd65f03c0),
    'nub_write': (0xf9404400, 0xb4000080, 0xf9400008, 0xf9450502, 0xd61f0040, 0xd65f03c0),
    'amfi_site': (0x94006c46, 0x321e0015, 0xaa1403e0, 0xaa1503e1, 0x94006c45),
}
PROVIDER_SIZE, NUB_SIZE, SLOTS = 0x4b8a8, 0x740, 340


def run(command, **kwargs):
    kwargs.setdefault('capture_output', True)
    return subprocess.run([str(v) for v in command], check=True, timeout=600, **kwargs)


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
                    help='prelinked kernel this build must match, normally the previous round')
args = parser.parse_args()

sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import backed_offset, exports, macho

OUT.mkdir(parents=True, exist_ok=True)

# ---------------------------------------------------------------- original ABI
# Private fields stay opaque; only slots 320 and 321 keep an implementation, everything else
# becomes a pure virtual the board overrides with a panic.
header = '''// Generated exact storage/slot ABI; private fields stay opaque.
#pragma once
#include <IOKit/IOPMGR.h>
class ApplePMGR : public IOPMGR {
 OSDeclareAbstractStructors(ApplePMGR)
 unsigned char originalPrivate[%d-sizeof(IOPMGR)];
public:
''' % PROVIDER_SIZE
guards = ''
for slot in range(177, SLOTS):
    if slot == 320:
        header += ' virtual uint32_t readAon();\n'
    elif slot == 321:
        header += ' virtual void writeAon(uint64_t);\n'
    else:
        header += f' virtual void opaque{slot}() = 0;\n'
        guards += f' void opaque{slot}() override {{ unimplemented({slot}); }}\n'
header += '};\nstatic_assert(sizeof(ApplePMGR)==0x4b8a8,"Original provider storage");\n'
(OUT / 'OriginalPMGRABI.h').write_text(header)
(OUT / 'GuardedPMGRSlots.inc').write_text(guards)

# ---------------------------------------------------------------- compile
record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
require(record_path.exists(), f'{record_path} missing: run this from the assembled build tree')
record = json.loads(record_path.read_text().strip().rstrip(','))
source = ROOT / 'drivers/CloverPMGR/CloverPMGR.cpp'
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
            '-Wno-unnecessary-virtual-specifier', '-I', OUT, '-I', ROOT / '.cache/xnu-work/osfmk']
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

# ---------------------------------------------------------------- reference kernel
require(args.reference.exists(), f'{args.reference} missing: build the kernel once without this knob first')
reference = args.reference.read_bytes()
meta = macho(reference)


def read(va, size):
    return reference[backed_offset(meta['segments'], va, size, len(reference)):][:size]


def words(va, count):
    return struct.unpack('<' + 'I' * count, read(va, 4 * count))


def pointer(va):
    return struct.unpack('<Q', read(va, 8))[0]


def constructor_vptr(pc):
    """The original base constructor forms its vptr with ADRP/ADD and stores it at this+0."""
    first, second, store = struct.unpack('<3I', read(pc, 12))
    require(first & 0x9f00001f == 0x90000008 and second & 0xffc003ff == 0x91000108 and store == 0xf9000008,
            'Constructor must form x8 with ADRP/ADD and store it at this+0')
    imm = ((first >> 5) & 0x7ffff) << 2 | ((first >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~4095) + (imm << 12) + ((second >> 10) & 4095)


for name, expected in EXPECT_WORDS.items():
    got = words(PINS[name], len(expected))
    require(got == expected, f'Instruction contract differs at {name} ({PINS[name]:#x}): '
                             f'{[hex(v) for v in got]} != {[hex(v) for v in expected]}')
require(read(PINS['aon_string'], 13) == b'ApplePMGRNub\0', 'AMFI matching class differs')
require(constructor_vptr(PINS['constructor'] + 0x1c) == PINS['base_vptr'],
        'Base constructor forms a different vptr')
require(pointer(PINS['amfi_read_slot']) == PINS['nub_read'] and
        pointer(PINS['amfi_write_slot']) == PINS['nub_write'], 'AMFI dispatch imports differ')
aon_slots = struct.unpack('<2Q', read(PINS['base_vptr'] + 320 * 8, 16))
require(aon_slots == (0xfffffff004a20fbc, 0xfffffff004a20fc4), 'Provider AON slots differ')
require(words(aon_slots[0], 2) == (0x12800000, 0xd65f03c0),
        'The original AON read must return 0xffffffff')

native_names = {name: va for va, names in exports(reference, meta).items() for name in names}
require('__ZTV6IOPMGR' in native_names, 'IOPMGR vtable missing from the reference kernel')
iopmgr_vptr = native_names['__ZTV6IOPMGR']

# ---------------------------------------------------------------- own vtable
sym_at = next(at for cmd, at, _ in info['commands'] if cmd == 2)
at, count, strings, _ = struct.unpack_from('<4I', raw, sym_at + 8)
symbols = []
for index in range(count):
    ix, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, at + index * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), address, kind))
vtable = next((va for name, va, _ in symbols if name == f'__ZTV{len(CLASS)}{CLASS}'), None)
require(vtable is not None, f'Own vtable __ZTV{len(CLASS)}{CLASS} missing from the object')
relocations = {}
for segment in info['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            at, rcount = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
            for index in range(rcount):
                offset, bits = struct.unpack_from('<2I', raw, at + index * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[section['va'] + offset] = symbols[bits & 0xffffff][0]
entries = [relocations.get(vtable + 16 + slot * 8) for slot in range(SLOTS)]
require(all(entries) and vtable + 16 + SLOTS * 8 not in relocations,
        f'Exact {SLOTS}-entry provider table required')

bindings = {'__ZN9ApplePMGRC2EPK11OSMetaClass': PINS['constructor'],
            '__ZN9ApplePMGRD2Ev': PINS['destructor'],
            '__ZN9ApplePMGR10gMetaClassE': PINS['gMetaClass']}
for slot, name in enumerate(entries):
    if slot in (0, 1, 7) or 168 <= slot < SLOTS and slot not in (320, 321):
        require(CLASS in name, f'Own guard or lifecycle required at slot {slot} ({name})')
    elif slot in (320, 321):
        require('9ApplePMGR' in name, f'Only the actual original AON pair may be inherited at {slot}')
        bindings[name] = pointer(PINS['base_vptr'] + slot * 8)
    else:
        require(native_names.get(name) == pointer(iopmgr_vptr + 16 + slot * 8),
                f'Native IOPMGR/IOService entry differs at slot {slot} ({name})')
undefined = {name for name, _, kind in symbols if kind == 1}
require({n for n in undefined if 'ApplePMGR' in n} == set(bindings),
        f'All and only the audited original base imports, got {sorted(undefined)}')
aliases = {f'_clover_original_pmgr_nub_{kind}': PINS[f'nub_{kind}']
           for kind in ('create', 'read', 'write')}
require(set(aliases) <= undefined, f'Own nub helper calls missing, got {sorted(undefined)}')
bindings.update(aliases)

# ---------------------------------------------------------------- bind and archive
asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in bindings.items()))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).stdout.strip()
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', asm,
     '-o', OUT / 'original-bindings.o'])
late = OUT / f'{CLASS}.late.o'
run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_CloverPMGR.cpp', obj, late])


def text_section(blob):
    section = next(s for segment in macho(blob)['segments'] for s in segment['sections'] if s['name'] == '__text')
    return blob[section['fileoff']:section['fileoff'] + section['size']]


require(text_section(raw) == text_section(late.read_bytes()), 'Late registration cannot change instructions')
ARCHIVE_DIR.mkdir(parents=True, exist_ok=True)
archive = ARCHIVE_DIR / 'libShengPlatformPower.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'original-bindings.o'])

report = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'header_sha256': hashlib.sha256(header.encode()).hexdigest(),
          'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
          'reference': str(args.reference),
          'reference_sha256': hashlib.sha256(reference).hexdigest(),
          'bindings': {name: hex(va) for name, va in bindings.items()},
          'virtual_entries': SLOTS,
          'boot_arg': 'clover-pmgr=2', 'mode': 'guarded provider, original nub, loud failure',
          'original_executable_modified': False, 'runtime_qualified': False}
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'pmgr: {archive} ({archive.stat().st_size} B)')
print(f"pmgr: reference {args.reference.name} {report['reference_sha256'][:16]}…, "
      f'{SLOTS} virtual entries, {len(bindings)} original bindings')
print('PASS: original AON pair inherited, every other power slot panics, no AMFI byte modified')
