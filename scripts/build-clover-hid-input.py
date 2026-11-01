#!/usr/bin/env python3
"""Qualify the owned offline HID source against this board's own prelinked IOHIDDevice.

Round255. The board's desktop stop is backboardd waiting on a dispatch group that only a
ButtonResolver completion block leaves, and that block is only built for a process whose HID
service carries the keys the resolver filters on. UniXNU-sheng reaches the desktop with an owned
IOHIDDevice attached under IOHIDResource -- package/HidInputProbe.cpp -- and it subclasses the
original IOHIDDevice without IOHIDFamily's source, which is why upstream's
scripts/build-native-hid-input.py exists: it derives the family's C++ ABI out of a linked image
and refuses to compile the subclass unless every byte it depends on is the original one.

That script reads a sheng candidate prelink whose addresses are written into the file. This board
links a different image, so every address here is derived instead: the vtable by its structure,
each code site by its own bytes, the vptr store out of the constructor's own instruction pair.
The pinned upstream values are then asserted as well, because they are a claim about
IOHIDFamily-2031.120.4 and a rebuild that moves one of them has to fail here, not on the tablet.

What the host cannot prove is that the derived numbers describe a live object graph -- the
metaclass chain, metaCast, attach and start. Those are the probe's own stage counters, printed on
the card, and a wrong gMetaClass shows up there as a refusal rather than as a panic.
"""
import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', type=Path, default=None,
                    help='this board\'s linked kernel (default: <out>/clover/kernel/kernel.macho)')
args = parser.parse_args()
if args.kernel:
    kernel = args.kernel
else:
    candidates = [ROOT.parent / 'kernel/kernel.macho', Path.home() / 'UniXNU/out/clover/kernel/kernel.macho']
    kernel = next((c for c in candidates if c.is_file()), candidates[0])
# This runs as a kernel archive step, where the device repository's scripts, drivers and build
# directory have already been copied into the assembled tree and the XNU build recipes -- the
# compile flags every board driver is rebuilt against -- live there. Run straight out of the
# device repository and those recipes are not there, so find the tree instead of failing on a
# missing recipe.
TREE = ROOT if (ROOT / 'build/xnu/ios-obj').is_dir() else Path.home() / 'UniXNU/out/clover/tree'
OUT = TREE / 'build/native-hid-input'
OUT.mkdir(parents=True, exist_ok=True)

for kit in (TREE / 'kits/NativePrelink', ROOT / 'kits/NativePrelink',
            Path.home() / 'UniXNU/kits/NativePrelink'):
    if (kit / 'native_prelink.py').is_file():
        sys.path.insert(0, str(kit))
        break
from native_prelink import macho, backed_offset, exports


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=180, text=True, **kw)


def require(value, why):
    if not value:
        raise SystemExit(why)


data = kernel.read_bytes()
image = macho(data)
segments = image['segments']


def read(address, size):
    at = backed_offset(segments, address, size, len(data))
    require(at is not None, f'nothing backs {address:#x}')
    return data[at:at + size]


def va_of(file_offset):
    for segment in segments:
        if segment['fileoff'] <= file_offset < segment['fileoff'] + segment['size']:
            return segment['va'] + (file_offset - segment['fileoff'])
    raise SystemExit(f'no segment maps file offset {file_offset:#x}')


names = {name: address for address, listed in exports(data, image).items() for name in listed}
for needed in ('__ZTV9IOService', '___cxa_pure_virtual', '__ZN9IOServiceC2EPK11OSMetaClass',
               '__ZN9IOServiceD2Ev'):
    require(needed in names, f'the kernel exports no {needed}: nothing to qualify against')

# The vtable, found by structure: 207 non-null pointers, exactly one of them the pure-virtual
# thunk and that one at slot 180, the next 24 bytes still zero, and a first 168 slots differing
# from IOService's own table in exactly the 18 places where IOHIDDevice overrides it. The
# prelink drops every kext symbol table, so this is the only way in -- and the constraint is
# tight: a scan of the whole image below finds one location, not a shortlist.
io_base = struct.unpack('<168Q', read(names['__ZTV9IOService'] + 16, 168 * 8))
pure = names['___cxa_pure_virtual']
found = []
for segment in segments:
    if segment['size'] < 207 * 8 + 24 or not segment['fileoff']:
        continue
    blob = data[segment['fileoff']:segment['fileoff'] + segment['size']]
    for off in range(16, len(blob) - (207 * 8 + 24), 8):
        slots = struct.unpack_from('<207Q', blob, off)
        if slots[180] != pure or not all(slots) or blob[off + 207 * 8:off + 207 * 8 + 24] != bytes(24):
            continue
        changed = [i for i in range(168) if slots[i] != io_base[i]]
        if len(changed) != 18:
            continue
        found.append((segment['name'], segment['va'] + off, changed, slots))
require(len(found) == 1, f'expected one IOHIDDevice-shaped vtable, found {len(found)}: '
        + ', '.join(f'{n}@{a:#x}' for n, a, _, _ in found))
plane, VPTR, changed, methods = found[0]
require(VPTR == 0xfffffff005d045c0, f'the HID vtable moved: {VPTR:#x}, expected the qualified '
        'IOHIDFamily-2031.120.4 slot 0xfffffff005d045c0')
require(changed == [0, 1, 7, 12, 14, 21, 23, 76, 81, 82, 86, 87, 91, 92, 93, 101, 132, 140],
        f'original HID overrides differ from the qualified set: {changed}')
require(methods[181] == 0xfffffff004add0c4, f'slot 181 is {methods[181]:#x}, not the qualified '
        'handleReport -- the report forwarder below has to sit at +0x4c of it')

# The three code sites the ABI rests on, located by their own bytes rather than by a number.
PATTERNS = {
    'constructor': (bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6'),
                    0xfffffff004ad94f8),
    'start': (bytes.fromhex('680240f908a142f9e00313aae10314aa00013fd6680240f9603d003608d142f9'
                            'e1630091e00313aa00013fd6a02d0035e00f40f9'), 0xfffffff004ad9aec),
    'report-forwarder': (bytes.fromhex('680240f9081543f9e00313aae20316aae30315aae40314aa00013fd6'),
                         0xfffffff004add110),
}
sites = {}
for label, (pattern, pinned) in PATTERNS.items():
    hits = [va_of(match.start()) for match in re.finditer(re.escape(pattern), data)]
    require(len(hits) == 1, f'{label}: expected one copy of the original instruction sequence, '
            f'found {len(hits)} at {[hex(h) for h in hits]}')
    require(hits[0] == pinned, f'{label} moved to {hits[0]:#x} from the qualified {pinned:#x}')
    sites[label] = hits[0]
require(sites['report-forwarder'] - methods[181] == 0x4c, 'the report forwarder is no longer at '
        '+0x4c of the slot-181 entry it was qualified against')

# The constructor stores the vtable it was compiled against; read that store back out of its own
# adrp/add pair so the vtable above is proved to be the one this code uses, not just a shape.
body = read(sites['constructor'], 0x20)
adrp, add = struct.unpack_from('<2I', body, 12)
require((adrp & 0x9f000000) == 0x90000000 and (add & 0xffc00000) == 0x91000000,
        'the constructor no longer materialises its vptr with adrp/add')
imm = ((((adrp >> 5) & 0x7ffff) << 2) | ((adrp >> 29) & 3))
if imm & (1 << 20):
    imm -= 1 << 21
stored = (((sites['constructor'] + 12) >> 12 << 12) + (imm << 12)) + ((add >> 10) & 0xfff)
require(stored == VPTR, f'the constructor stores {stored:#x} but the vtable qualified here is {VPTR:#x}')
require(struct.unpack('<Q', read(0xfffffff005d00f10, 8))[0] == names['__ZN9IOServiceC2EPK11OSMetaClass'],
        'the constructor thunk no longer binds the actual native IOService constructor')
require(struct.unpack('<Q', read(0xfffffff005d00f18, 8))[0] == names['__ZN9IOServiceD2Ev'],
        'the destructor thunk no longer binds the actual native IOService destructor')

header = '''// Generated from the qualified original image; opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
for i in range(168, 207):
    if i == 171:
        header += ' virtual OSString *newTransportString() const;\n'
    elif i == 173:
        header += ' virtual OSString *newProductString() const;\n'
    elif i == 180:
        header += ' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
    elif i == 181:
        header += ' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
    else:
        header += f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n'
header += '};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");\n'
(OUT / 'OriginalHidDeviceABI.h').write_text(header)

print(run([sys.executable, TREE / 'scripts/build-arm-driver.py', 'NativeHidInput'],
          cwd=TREE), end='')
obj = OUT / 'NativeHidInput.o'
raw = obj.read_bytes()
compiled = macho(raw)
symbol_command = next(parameters for command, parameters, _ in compiled['commands'] if command == 2)
table, count, strings, _ = struct.unpack_from('<4I', raw, symbol_command + 8)
symbols = []
for index in range(count):
    string_index, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, table + index * 16)
    symbols.append((raw[strings + string_index:raw.index(b'\0', strings + string_index)].decode(), address, kind))
vtable = next(address for name, address, _ in symbols if name == '__ZTV14NativeHidInput')
relocations = {}
for segment in compiled['segments']:
    for section in segment['sections']:
        if not section['va'] <= vtable < section['va'] + section['size']:
            continue
        first, total = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
        for index in range(total):
            offset, bits = struct.unpack_from('<2I', raw, first + index * 8)
            if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                relocations[section['va'] + offset] = symbols[bits & 0xffffff][0]
built = [relocations.get(vtable + 16 + i * 8) for i in range(207)]
require(all(built) and vtable + 16 + 207 * 8 not in relocations, 'Compiled exact 207-slot vtable')
bindings = {'__ZN11IOHIDDeviceC2EPK11OSMetaClass': sites['constructor'],
            '__ZN11IOHIDDeviceD2Ev': sites['constructor'] + 0x20,
            '__ZN11IOHIDDevice10gMetaClassE': 0xfffffff006ae6fa0,
            '__ZTV11IOHIDDevice': VPTR - 16}
owned = []
for index, name in enumerate(built):
    if name.startswith(('__ZN14NativeHidInput', '__ZNK14NativeHidInput')):
        owned.append(index)
    elif '11IOHIDDevice' in name:
        bindings[name] = methods[index]
    else:
        require(index < 168 and names.get(name) == methods[index],
                f'slot {index} inherits {name} from neither the kernel nor the qualified HID table')
require(owned == [0, 1, 7, 14, 86, 87, 111, 171, 173, 180],
        f'the driver overrides {owned}, not the qualified [0, 1, 7, 14, 86, 87, 111, 171, 173, 180]')
# gMetaClass is filled by the family's own C++ static initializer at boot, so the slot is zero in
# any linked image and cannot be proved from the file -- that is why the probe reads the metaclass
# chain back on the tablet (native_hid_input_meta_status and the device's own metaCast) instead of
# this host pretending otherwise. What the host does check is that the driver imports exactly this
# name and nothing else HID-shaped, below.
require(any(segment['va'] <= 0xfffffff006ae6fa0 < segment['va'] + segment['size']
            and segment['name'] in ('__PLK_DATA_CONST', '__PRELINK_DATA', '__DATA', '__DATA_CONST',
                                    '__KLDDATA', '__LASTDATA_CONST')
            for segment in segments),
        'the qualified gMetaClass address is not inside any writable segment of this image')
undefined = {name for name, _, kind in symbols if kind == 1 and '11IOHIDDevice' in name}
require(undefined == set(bindings), f'driver imports {sorted(undefined)} but bindings are {sorted(bindings)}')

asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n' for name, address in sorted(bindings.items())))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).strip()
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', asm, '-o',
     OUT / 'hid-original-bindings.o'])
llvm = Path(run(['brew', '--prefix', 'llvm']).strip()) / 'bin'
late = OUT / 'NativeHidInput.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp', obj, late])


def text_of(blob, parsed):
    section = next(s for g in parsed['segments'] for s in g['sections'] if s['name'] == '__text')
    return blob[section['fileoff']:section['fileoff'] + section['size']]


require(text_of(raw, compiled) == text_of(late.read_bytes(), macho(late.read_bytes())),
        'owned code changed while deferring metaclass registration')
(OUT / 'abi-audit.json').write_text(json.dumps({
    'kernel': str(kernel), 'kernel_sha256': __import__('hashlib').sha256(data).hexdigest(),
    'hid_vtable_plane': plane, 'hid_vtable': VPTR, 'original_slots': 207, 'original_instance_size': 0xd0,
    'original_overrides': changed, 'sites': {k: hex(v) for k, v in sites.items()},
    'owned_slots': owned, 'bindings': {k: hex(v) for k, v in bindings.items()},
    'runtime_qualified': False,
    'source_sha256': __import__('hashlib').sha256((ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp').read_bytes()).hexdigest(),
}, indent=2) + '\n')
print('PASS: this board\'s own image carries the qualified IOHIDDevice ABI and the driver binds to nothing else')
