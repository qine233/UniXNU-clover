#!/usr/bin/env python3
"""Bind this board's owned concrete backend to the unchanged original Legacy lifecycle.

The clover narrowing of the reference port's build-gop-legacy-display.py. Three things differ:

  * the original code being bound is read out of this board's own shipped kernel
    (out/clover/kernel/kernel.macho), through the class identity that
    audit-clover-mobile-graphics-legacy.py derived statically from it, instead of out of a
    reference kernelcache that only another board has;
  * the driver sources are this device repository's vendored drivers/NativeMobileDisplay, so the
    archive the kernel links is the one this board's README describes - the builder refuses to run
    against the base module;
  * the board-local instrument is kits/clover/CloverLegacyDisplayProbe.cpp: no boot-progress
    channel (this board builds with CONFIG_XNU_BOOT_PROGRESS=n) and none of the other boards'
    peripheral probes, so the archive is three objects instead of thirty.

Everything the reference qualifies, this qualifies too, and against the same instructions: the
original family's executable text in this kernel is byte-identical to the image the reference
qualification was done against, so every pinned site below is re-read from this kernel and
compared byte for byte here.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]                 # the assembled build tree
OUT = ROOT / 'build/native-legacy-display'; OUT.mkdir(parents=True, exist_ok=True)
KERNEL = ROOT.parent / 'kernel/kernel.macho'               # what this board ships
ABI_JSON = ROOT / 'build/display-analysis/clover-mobile-graphics-legacy-abi.json'
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import macho, backed_offset, exports   # noqa: E402


def run(cmd, **kw): return subprocess.check_output([str(x) for x in cmd], timeout=600, **kw)
def require(v, why):
    if not v: raise SystemExit('CLOVER LEGACY DISPLAY BUILD FAILED: ' + why)
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()


require(KERNEL.exists(), 'build the kernel before its display archive: ' + str(KERNEL))
DEVICE = (ROOT / 'kits/clover').resolve().parent           # the device repository
DRIVER = (ROOT / 'drivers/NativeMobileDisplay').resolve()
require(DRIVER.is_relative_to(DEVICE.resolve()),
        'this archive is built from the device-local drivers/NativeMobileDisplay; run "unixnu tree" '
        'so the device module wins over the base one')

print(run(['python3', ROOT / 'scripts/audit-clover-mobile-graphics-legacy.py'], text=True), end='')
audit = json.loads(ABI_JSON.read_text())
require(audit['kernel_sha256'] == sha(KERNEL), 'the class audit describes this kernel')
pure = audit['abstract_slots']; methods = audit['methods']
own = audit['qualified_own_override_slots']
base = int(audit['family_text_base'], 16); family_bytes = audit['family_text_bytes']

# The opaque ABI header for the exact original class. Field positions in it are the ones this
# kernel's own getters were audited at (slot 249 for the dimensions, the queue words at +5472
# and +5480/-8488, the request link words at +2960/+2968).
header = '''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
// Generated for the clover board kernel: ''' + KERNEL.name + ''' sha256 ''' + sha(KERNEL) + '''
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 // Both fields are qualified by the complete original slot-249 getter.
 // This modifies this object's own mode state, never a vtable or executable.
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The real normal scanout backend removes its pending head before calling slot 295. Slot 312 is
 // a discard path (terminal state 8), not presentation. The queue is a tail queue (head at +5480,
 // tail pointer at +5488, each request's next at +2960 and previous-link at +2968): taking the head
 // moves the next request, if a client already queued one behind it, up to the head, so a wait on a
 // retire can never stay busy behind an un-taken queue. The original queue's reference moves to the
 // current-swap slot for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(168, 358):
    if i == 184: header += ' virtual bool isMainDisplay();\n'
    elif i == 188: header += ' virtual IOReturn getDotPitch(uint32_t *);\n'
    elif i == 213: header += ' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n'
    elif i == 249: header += ' virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n'
    elif i == 290: header += ' virtual void kickSwapQueue();\n'
    elif i == 296: header += ' virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
    elif i == 295: header += ' virtual void completeCurrentSwap();\n'
    elif i == 337: header += ' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n'
    elif i == 288: header += ' virtual bool startHardware();\n'
    elif i == 301: header += ' virtual bool getRefreshAttributes(uint64_t *,bool);\n'
    elif i in pure: header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else: header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header += '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
(OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)

print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True), end='')
obj = OUT / 'NativeLegacyDisplay.o'; raw = obj.read_bytes(); image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(nsyms):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, symoff + i * 16)
    name = raw[stroff + ix:raw.index(b'\0', stroff + ix)].decode()
    symbols.append((name, va, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
relocations = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]
compiled = [relocations.get(vtable + 16 + i * 8) for i in range(358)]
require(all(compiled) and vtable + 16 + 358 * 8 not in relocations, 'Exact owned 358-entry vtable')

# The original code being bound: this kernel's own image, qualified by the audit above.
blob = KERNEL.read_bytes(); kernel = macho(blob)
def read(va, n):
    at = backed_offset(kernel['segments'], va, n, len(blob)); return blob[at:at + n]
names = {n: v for v, group in exports(blob, kernel).items() for n in group}

legacy_ctor = base + 0x1A5CC; legacy_dtor = base + 0x1A618
bindings = {'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': legacy_ctor,
            '__ZN25IOMobileFramebufferLegacyD2Ev': legacy_dtor,
            '__ZN25IOMobileFramebufferLegacy10gMetaClassE': int(audit['meta_class_record'], 16),
            '__ZTV25IOMobileFramebufferLegacy': int(audit['legacy_vptr'], 16) - 16}
owned = []; native = []
for i, name in enumerate(compiled):
    expected = int(methods[i]['address'], 16)
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i); require(i in own or i in pure, 'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, 'No unresolved original abstract backend target'); bindings[name] = expected
        require(base <= expected < base + family_bytes, 'Original slot %d is inside the audited family text' % i)
    else:
        require(i < 168 and names.get(name) == expected, 'Actual inherited native base symbol ' + str(i))
        native.append(i)
require(owned == sorted(own + pure), 'Exact owned method set')
undefined = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
require(undefined == set(bindings), 'All and only actual original-class imports qualified')

# Every pinned original construction site, re-read from this kernel. A site name is reported when
# it does not match, so a kernel that moves a function fails loudly instead of binding a guess.
def site(name, va, size, expected):
    require(base <= va < base + family_bytes, name + ' is outside the audited family text extent')
    require(read(va, size) == expected, name + ' does not carry the qualified instructions')

# Exact original IOSurface getters, independently observed through the real original swap request.
# These aliases call original code; no object layout is substituted and no original method or
# vtable is patched. They live in the IOSurface module, not the family, so only the instruction
# encoding is checked here.
for name, address, field, wide in [
    ('descriptor', 0xfffffff004b9ead0, 48, True), ('width', 0xfffffff004b9ebbc, 88, True),
    ('height', 0xfffffff004b9ebc4, 96, True), ('stride', 0xfffffff004b9ebd4, 104, True),
    ('allocation', 0xfffffff004b9ebfc, 144, True), ('format', 0xfffffff004b9ebac, 128, False),
    ('planes', 0xfffffff004b9ebf4, 152, False)]:
    expected = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
    require(read(address, 8) == struct.pack('<2I', expected, 0xd65f03c0), 'Exact original surface getter ' + name)
    bindings['_native_legacy_surface_' + name] = address
# The original display-refresh notification sender (self, timestamp, continuous host time, frame
# interval): it sends the six-word vsync message (type 5) to every client entry queued at +5592,
# the tail queue that selector 72's registration (slot 208) fills at +5512 + type*16. The platform
# subclass that normally calls it on each panel vblank is not part of this port.
site('Original vsync sender prologue', 0xfffffff005796720, 16, bytes.fromhex('ff0302d1fd7b07a9fdc30191a0831ff8'))
site('Original vsync +5592 queue head', 0xfffffff005796830, 4, bytes.fromhex('08ed4af9'))
site('Original vsync six-word drop-async send', 0xfffffff00579685c, 16, bytes.fromhex('e2230091c3008052240080521684ff97'))
site('Original vsync message type 5', 0xfffffff005795fbc, 4, bytes.fromhex('0ab182d2'))
bindings['_native_legacy_vsync_notify'] = 0xfffffff005796720
# Slot 301 fills the six refresh words that setDisplayRefreshProperties publishes as
# IOMFBDisplayRefresh (minimum interval, its mach time, maximum interval, its mach time, step, its
# mach time; intervals in 32.32 seconds): the generic body fixes the minimum at 0x04444444 (1/60 s),
# and the render server paces its display timer by it.
require(int(methods[301]['address'], 16) == 0xfffffff00579ea5c, 'Original slot 301 address')
site('Original slot-301 1/60 s minimum', 0xfffffff00579ea68, 8, bytes.fromhex('888888d28888a0f2'))
site('Original slot-301 refresh word store', 0xfffffff00579ec54, 4, bytes.fromhex('08b544f9'))
# setDisplayRefreshProperties itself (self): fills the six words through slot 301 and publishes them
# as IOMFBDisplayRefresh. Nothing in the family calls it (the platform subclass does), so without it
# the render server falls back to its own 1/60 s heartbeat.
site('Original setDisplayRefreshProperties prologue', 0xfffffff00579ebb8, 16, bytes.fromhex('ff0302d1fd7b07a9fdc30191a0031ff8'))
site('Original setDisplayRefreshProperties slot-301 call', 0xfffffff00579ed54, 16, bytes.fromhex('086540f90173ff90218c0c9100013fd6'))
bindings['_native_legacy_set_refresh_properties'] = 0xfffffff00579ebb8
# Both protected destructors/constructors must follow the exact original chain (the audit re-checks
# the two branch targets; this binds the entries themselves).
def branch(pc):
    word = struct.unpack('<I', read(pc, 4))[0]; require(word & 0xfc000000 == 0x94000000, 'Actual protected base call')
    value = word & 0x3ffffff; return pc + 4 * (value - (1 << 26) if value & (1 << 25) else value)
require(branch(legacy_ctor + 0x30) == base + 0xF510 and branch(legacy_dtor + 0x18) == base + 0xF55C,
        'Original protected Legacy C2/D2 really call their original Service bases')
# Actual caller passes only self at slot 288 and tests bit zero of w0.
site('Original bool start_hardware() call and result check', 0xfffffff005787ef8, 16, bytes.fromhex('080040f9088144f900013fd6e0000037'))
getter = int(methods[249]['address'], 16)
for offset, field in [(0x30, 5808), (0x58, 5812)]:
    require(base <= getter + offset < base + family_bytes, 'Actual original dimensions getter inside the family text')
    require(struct.unpack('<I', read(getter + offset, 4))[0] == 0xb9400000 | ((field // 4) << 10) | (8 << 5) | 8,
            'Actual original width/height getter fields')
require(int(methods[295]['address'], 16) == 0xfffffff005795b88, 'Original slot 295 address')
site('Original slot-295 normal current completion', 0xfffffff005795b98, 16, bytes.fromhex('e00740f9280080520101001272d3ff97'))
site('Original completion calls optional report slot 337', 0xfffffff00578aa7c, 24,
     bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'))
site('Original isMainDisplay dispatch', 0xfffffff005777e9c, 16, bytes.fromhex('080040f908e142f900013fd6c0000036'))

asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(bindings.items())))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', asm, '-o', OUT / 'original-bindings.o'])

llvm = Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
late = OUT / 'NativeLegacyDisplay.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp', obj, late])
def code(data, info):
    sec = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[sec['fileoff']:sec['fileoff'] + sec['size']]
require(code(raw, image) == code(late.read_bytes(), macho(late.read_bytes())),
        'Only owned initializer section registration changes')

# The board instrument, compiled with the flags the XNU build recorded for the family it binds to.
r = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
               .read_text().strip().rstrip(','))
cmd = []; args = r['arguments']; index = 0
probe_source = ROOT / 'kits/clover/CloverLegacyDisplayProbe.cpp'; probe_obj = OUT / 'CloverLegacyDisplayProbe.o'
while index < len(args):
    value = args[index]
    if value == r['file']: cmd.append(str(probe_source))
    elif value == '-o': cmd += ['-o', str(probe_obj)]; index += 1
    elif value == '-Xclang' and args[index + 1].startswith('-W'): index += 1
    elif not value.startswith('-W'): cmd.append(value)
    index += 1
cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier',
        '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(OUT), '-I', str(ROOT / 'kits/clover')]
# A full kernel pass writes this record without -isysroot (it compiles with -nostdlibinc and
# BUILD_WERROR=0, so the host SDK default costs it nothing); with -Werror added here that default becomes
# incompatible-sysroot against the record's own arm64-apple-ios target. Pin the iOS SDK when the record
# did not. See scripts/build-arm-driver.py for the long form of this note.
if not any(str(x).startswith(('-isysroot', '--sysroot')) for x in cmd): cmd += ['-isysroot', sdk]
run(cmd, cwd=r['directory'])
require(probe_obj.exists(), 'the board instrument compiled')

archive = OUT / 'libNativeLegacyDisplay.late.a'; archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'original-bindings.o', probe_obj])

print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeGOPPresenter'], text=True), end='')
presenter = ROOT / 'build/native-gop-presenter'
presenter_late = presenter / 'NativeGOPPresenter.late.o'
run([llvm / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp',
     presenter / 'NativeGOPPresenter.o', presenter_late])
require(code((presenter / 'NativeGOPPresenter.o').read_bytes(), macho((presenter / 'NativeGOPPresenter.o').read_bytes())) ==
        code(presenter_late.read_bytes(), macho(presenter_late.read_bytes())),
        'Owned presenter instructions unchanged by late init staging')
run(['xcrun', 'ar', 'rcs', archive, presenter_late])

# The kernel force-loads this archive, so every object in it is pulled in: a name two of them define
# is a link error, and a name the board contract owns but nobody defines is one too. Check both here,
# where the message can name the objects, instead of 15 minutes later inside the kernel link.
def globals_of(path):
    data = path.read_bytes(); info = macho(data)
    start = next(p for c, p, _ in info['commands'] if c == 2)
    position, count, strings, _ = struct.unpack_from('<4I', data, start + 8)
    defined = set(); undefined = set()
    for index in range(count):
        string, kind, section, _, _ = struct.unpack_from('<IBBHQ', data, position + index * 16)
        if kind & 0xe0: continue                                   # symbolic debug entry
        if not kind & 1: continue                                  # file-local
        name = data[strings + string:data.index(b'\0', strings + string)].decode()
        # n_type low nibble: 0 undefined, 2 absolute (what .set emits), 0xe in a section.
        (undefined if kind & 0xe == 0 else defined).add(name)
    return defined, undefined
members = {'NativeLegacyDisplay.late': late, 'CloverLegacyDisplayProbe': probe_obj,
           'NativeGOPPresenter.late': presenter_late, 'original-bindings': OUT / 'original-bindings.o'}
owners = {}
for owner, path in members.items():
    for name in globals_of(path)[0]:
        require(name not in owners or name.startswith(('ltmp', 'l_')),
                'duplicate definition of ' + name + ' in ' + owner + ' and ' + owners.get(name, ''))
        owners[name] = owner
contract = {name for name in owners if name.startswith(('_native_legacy', '_native_gop'))}
for owner, path in members.items():
    for name in globals_of(path)[1]:
        if name.startswith(('_native_legacy', '_native_gop')):
            require(name in owners, name + ' (needed by ' + owner + ') is defined by no archive member')
require('_native_legacy_gpu_complete' in contract, 'the presenter hands frame completion back to the driver')

sources = [ROOT / 'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp', ROOT / 'drivers/NativeMobileDisplay/NativeLegacyRequest.h',
           ROOT / 'drivers/NativeMobileDisplay/NativeGOPPresenter.cpp', ROOT / 'drivers/NativeMobileDisplay/NativeGOPWire.h',
           probe_source, ROOT / 'scripts/build-clover-gop-legacy-display.py',
           ROOT / 'scripts/audit-clover-mobile-graphics-legacy.py']
# Three hashes, and only two of them are the identity of what gets linked. `ar` stamps every member
# with its mtime, so archive_sha256 changes on each run even when nothing else did (round180 measured
# this: two consecutive runs, four byte-identical members, two archive hashes). The linker reads object
# bytes, not those stamps, so compiled_object_sha256 and late_object_sha256 are what a rebuild must
# reproduce -- they are the numbers to compare across a round.
#
# kernel_sha256 has the same subtlety pointing the other way: the archive step runs before the kernel
# link, so the kernel this builder audits is the one the previous round shipped. Round180 answers that
# by re-running the audit and this builder against the freshly linked image: the family text is the same
# 225140 bytes at the same 0xfffffff00576c000, all 193 bindings land where they did, both objects come
# out byte-identical. An address that moved would have been a refusal here, not a wrong frame later.
(OUT / 'build-report.json').write_text(json.dumps(dict(
    kernel_sha256=sha(KERNEL), family_text_sha256=audit['family_text_sha256'],
    source_sha256={str(p.relative_to(ROOT)): sha(p) for p in sources},
    generated_abi_sha256=sha(OUT / 'OriginalLegacyFramebufferABI.h'),
    original_audit_sha256=sha(ABI_JSON),
    compiled_object_sha256=sha(obj), late_object_sha256=sha(late), archive_sha256=sha(archive),
    bindings={n: hex(v) for n, v in bindings.items()}, virtual_entries=358, owned_entries=owned,
    inherited_native_entries=native, guarded_backend_leaves=[i for i in pure if i not in (213, 337)],
    archive_board_symbols=sorted(contract),
    original_instructions_modified=False, driver_initialized=False, display_presented=False,
    device_observed=False), indent=2) + '\n')
print(f'Clover Legacy archive built: {archive}')
print(f'{len(bindings)} original bindings, {len(owned)} owned entries, {len(native)} inherited native entries; '
      'seven backend leaves still guarded, runtime lifecycle unqualified until a device frame reads back')
