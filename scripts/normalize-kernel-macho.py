#!/usr/bin/env python3
"""Make a prelinked kernel a pure function of its source by rewriting the three
fields that a rebuild cannot reproduce, then asserting nothing else moves.

Two builds of the same tree, measured on the real artifacts -- round176's shipped
prelink kernel against a rebuild of the identical tree: 52,043,776 bytes each,
exactly 136 of them different, in three kinds:

  * 50 x 2 bytes -- nlist entries of type 0x66 (N_SO, the object-path debug
    symbol) whose n_value is the mtime of the .o the linker read.  Only the
    objects a rebuild actually re-makes move: the twelve board archives, and the
    XNU files a board patch rewrites.  Everything else kept its stamp, which is
    why 277,620 symbols produced 50 diffs rather than 277,620.
  * 16 bytes -- LC_UUID, a random UUID the linker generates per link.
  * 22 bytes -- <key>_PrelinkKCID</key><data>, the kernel-collection id, which
    emit_prelink.py computes as sha256(native + original)[:16].  That is a
    deterministic function; it moves only because the UUID and the stamps inside
    the hashed bytes move.

Upstream already knows this triple: scripts/check-prelink-rebuild-equivalence.py
zeroes exactly these three and then compares, and concludes "retain executed
image".  That answers "did a rebuild change behaviour" and leaves the image
unreproducible.  This closes the other half: it rewrites the three into values
derived from the rest of the file, so a rebuild of a tree produces the bytes the
tree produces and cmp says nothing.

The recipe, in the order that makes it self-consistent:

  1. preimage = the file with every N_SO n_value, the LC_UUID payload and the
     KCID base64 string replaced by zeroes.
  2. digest   = sha256(preimage).
  3. LC_UUID  = digest[0:16], with the version nibble set to 3 and the variant
     bits to 10 so the field keeps the shape ld writes -- a UUID consumer that
     rejects our UUID is not one we can hand a normalised kernel to.
  4. KCID     = base64(digest[16:32]) -- 22 characters and "==", the same width
     as the string that was there, which is what lets this run without moving a
     single payload byte.

Nothing in the boot contract reads these three.  Kernelcollection trust is a
cdhash over the executable segments (__TEXT_EXEC, __PLK_TEXT_EXEC); the ADT does
not look at Mach-O identity fields; the device never parses its own symbol table
-- the stabs exist for a host that has the object files, which is the one thing
a rebuild makes untrue.  A normalised file is its own normalisation, so --check
on the shipped image is the whole gate.

Usage:
  normalize-kernel-macho.py --selftest
  normalize-kernel-macho.py --check KERNEL          # the shipped image is normalised
  normalize-kernel-macho.py KERNEL                  # rewrite in place
  normalize-kernel-macho.py --output OUT KERNEL     # write a copy instead
  normalize-kernel-macho.py --prove A B             # two builds of one tree converge
"""

import argparse
import base64
import hashlib
import re
import struct
import sys
from pathlib import Path

MAGIC_64 = 0xFeedFACF
CPU_ARM64 = 0x100000C

LC_SYMTAB = 0x02
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B

N_SO = 0x66           # object-path stab; its n_value is the object file's mtime
NLIST_SIZE = 16       # struct nlist, 64-bit
UUID_WIDTH = 16
STAMP_WIDTH = 8
KCID_TEXT_WIDTH = 22  # base64 of 16 bytes: 22 characters plus "=="
KCID_PATTERN = re.compile(rb'<key>_PrelinkKCID</key><data>([A-Za-z0-9+/]{22}==)</data>')


class Field:
    """One byte range that a rebuild cannot reproduce, and the kind of thing it is."""

    def __init__(self, kind, start, end):
        self.kind = kind
        self.start = start
        self.end = end

    def width(self):
        return self.end - self.start


def collect_fields(data):
    """Return the non-reproducible fields, or raise if the file is not shaped like
    a prelinked kernel: a kernel whose identity metadata moved shape would
    normalise into something the boot contract never saw, so refuse it."""
    data = bytes(data)
    if len(data) < 32:
        raise ValueError('truncated Mach-O')
    magic, cputype = struct.unpack_from('<II', data, 0)
    if magic != MAGIC_64 or cputype != CPU_ARM64:
        raise ValueError('not an ARM64 Mach-O (magic %08x cputype %08x)' % (magic, cputype))
    ncmds = struct.unpack_from('<I', data, 16)[0]
    fields = []
    uuids = 0
    at = 32
    for _ in range(ncmds):
        if at + 8 > len(data):
            raise ValueError('load command table runs past the file')
        command, size = struct.unpack_from('<II', data, at)
        if size < 8 or at + size > len(data):
            raise ValueError('malformed load command at 0x%x' % at)
        kind = command & 0x7F
        if kind == LC_UUID:
            if size != 24:
                raise ValueError('LC_UUID at 0x%x has size %d' % (at, size))
            fields.append(Field('uuid', at + 8, at + 8 + UUID_WIDTH))
            uuids += 1
        elif kind == LC_SYMTAB:
            symoff, nsyms = struct.unpack_from('<2I', data, at + 8)
            if symoff + nsyms * NLIST_SIZE > len(data):
                raise ValueError('symbol table at 0x%x (%d entries) runs past the file'
                                 % (symoff, nsyms))
            for n in range(nsyms):
                item = symoff + n * NLIST_SIZE
                if data[item + 4] == N_SO:
                    fields.append(Field('stamp', item + 8, item + 8 + STAMP_WIDTH))
        elif kind == LC_SEGMENT_64:
            name = data[at + 8:at + 24].rstrip(b'\0')
            if name == b'__PRELINK_INFO':
                offset, length = struct.unpack_from('<QQ', data, at + 40)
                if offset + length > len(data):
                    raise ValueError('__PRELINK_INFO at 0x%x length 0x%x runs past the file'
                                     % (offset, length))
                for match in KCID_PATTERN.finditer(data, offset, offset + length):
                    lo, hi = match.span(1)
                    fields.append(Field('kcid', lo, hi))
        at += size
    if uuids != 1:
        raise ValueError('expected exactly one LC_UUID, found %d' % uuids)
    if not any(f.kind == 'stamp' for f in fields):
        raise ValueError('no object-path debug symbols: this is not the unstripped kernel '
                         'the board links')
    kcids = sum(1 for f in fields if f.kind == 'kcid')
    if kcids != 1:
        raise ValueError('expected exactly one _PrelinkKCID, found %d' % kcids)
    return fields, data


def zeroed(data):
    """The preimage: data with every build-varying field blanked to zeroes."""
    fields, data = collect_fields(data)
    preimage = bytearray(data)
    for field in fields:
        preimage[field.start:field.end] = bytes(field.width())
    return fields, bytes(preimage)


def derived_uuid(digest):
    uuid = bytearray(digest[:UUID_WIDTH])
    uuid[6] = (uuid[6] & 0x0F) | 0x30        # version 3, the shape ld writes
    uuid[8] = (uuid[8] & 0x3F) | 0x80        # RFC 4122 variant
    return bytes(uuid)


def normalise(data):
    """Rewrite the three build-varying fields as functions of the rest of the file.

    Returns (bytes, counts); the length never changes."""
    fields, preimage = zeroed(data)
    digest = hashlib.sha256(preimage).digest()
    out = bytearray(preimage)
    counts = {'stamp': 0, 'uuid': 0, 'kcid': 0}
    for field in fields:
        if field.kind == 'uuid':
            out[field.start:field.end] = derived_uuid(digest)
        elif field.kind == 'kcid':
            encoded = base64.b64encode(digest[UUID_WIDTH:UUID_WIDTH * 2])
            if len(encoded) != field.width():
                raise ValueError('_PrelinkKCID width changed (%d -> %d)'
                                 % (field.width(), len(encoded)))
            out[field.start:field.end] = encoded
        else:
            if field.width() != STAMP_WIDTH:
                raise ValueError('object-path stamp field width changed at 0x%x' % field.start)
            out[field.start:field.end] = bytes(STAMP_WIDTH)
        counts[field.kind] += 1
    if len(out) != len(preimage):
        raise ValueError('normalisation moved the file length')
    return bytes(out), counts


def is_normalised(data):
    """True when the file is its own normalisation: rerunning changes nothing."""
    out, _counts = normalise(data)
    return out == bytes(data)


def symbol_table(data):
    """(symoff, nsyms) from the file's LC_SYMTAB, for tests that need an address."""
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at = 32
    for _ in range(ncmds):
        command, size = struct.unpack_from('<II', data, at)
        if (command & 0x7F) == LC_SYMTAB:
            return struct.unpack_from('<2I', data, at + 8)
        at += size
    raise ValueError('no LC_SYMTAB')


# ---------------------------------------------------------------------------
# selftest.  The assertion worth having is that two builds of the same content
# converge, and that needs two files differing only in these fields -- which a
# synthetic Mach-O gives me here instead of in half an hour of linking.

def build_synthetic(uuid_bytes, stamps, kcid_text, stab=N_SO, body=b'kernel bytes\n' * 8):
    """A Mach-O with one LC_UUID, one symbol table carrying object-path stabs, and
    a __PRELINK_INFO segment whose payload holds a _PrelinkKCID."""
    commands = bytearray()
    commands += struct.pack('<II16sQQQQIIII', LC_SEGMENT_64, 72, b'__TEXT' + bytes(10),
                            0x10000000, 0x1000, 0, 0, 7, 5, 0, 0)
    commands += struct.pack('<II', LC_UUID, 24) + uuid_bytes
    commands += struct.pack('<II', LC_SYMTAB, 24) + bytes(16)
    commands += struct.pack('<II16sQQQQIIII', LC_SEGMENT_64, 72, b'__PRELINK_INFO' + bytes(2),
                            0x11000000, 0x1000, 0, 0, 1, 1, 0, 0)
    header = struct.pack('<7I', MAGIC_64, CPU_ARM64, 0, 2, 4, len(commands), 0) + bytes(4)

    symoff = len(header) + len(commands) + len(body)
    symtab = bytearray()
    for n, stamp in enumerate(stamps):
        symtab += struct.pack('<IBBHQ', n * 15, stab, 0, 1, stamp)
    symtab += struct.pack('<IBBHQ', 0, 0x0F, 1, 0, 0x4142434445464748)   # a symbol to leave alone
    info = b'<plist><dict><key>_PrelinkKCID</key><data>' + kcid_text + b'</data></dict></plist>'
    infooff = symoff + len(symtab)
    strings = b''.join(b'/build/path%02d.o\0' % n for n in range(len(stamps)))
    stroff = infooff + len(info)
    struct.pack_into('<4I', commands, 72 + 24 + 8, symoff, len(stamps) + 1, stroff, len(strings))
    # segment_command_64: cmd/cmdsize/segname/vmaddr/vmsize/fileoff/filesize -> fileoff at +40
    struct.pack_into('<2Q', commands, 72 + 24 + 24 + 40, infooff, len(info))
    return header + commands + body + bytes(symtab) + info + strings


def selftest():
    failures = []
    passed = 0

    def check(name, ok, detail=''):
        nonlocal passed
        if ok:
            passed += 1
            print('  ok   %-44s %s' % (name, detail))
        else:
            failures.append(name)
            print('  FAIL %-44s %s' % (name, detail))

    # Two builds of one source: identical content, different stamps, UUID, KCID.
    a = build_synthetic(bytes(range(16)), [1700000000, 1700000001], b'A' * KCID_TEXT_WIDTH + b'==')
    b = build_synthetic(bytes(range(100, 116)), [1700009999, 1700010000],
                        b'B' * KCID_TEXT_WIDTH + b'==')
    apart = sum(1 for x, y in zip(a, b) if x != y)
    check('two synthetic builds start out different', a != b, '%d bytes apart' % apart)
    na, ca = normalise(a)
    nb, cb = normalise(b)
    check('normalising the two synthetic builds converges', na == nb,
          'stamps=%d uuid=%d kcid=%d' % (ca['stamp'], ca['uuid'], ca['kcid']))
    check('the normalised file is its own normalisation', is_normalised(na))
    check('normalisation preserves the length', len(na) == len(a), '%d bytes' % len(a))
    check('counted every object stamp', ca['stamp'] == 2 and cb['stamp'] == 2, str(ca['stamp']))
    moved = sum(1 for x, y in zip(a, na) if x != y)
    fields, preimage = zeroed(a)
    covered = set()
    for field in fields:
        covered.update(range(field.start, field.end))
    outside = [i for i in range(len(a)) if a[i] != na[i] and i not in covered]
    check('nothing outside the derived fields moved', not outside,
          '%d bytes differ, %d bytes of %d fields eligible' % (moved, len(covered), len(fields)))
    check('the stamps are blanked and the identity fields are filled',
          all(na[f.start:f.end] == bytes(f.width()) for f in fields if f.kind == 'stamp')
          and all(na[f.start:f.end] != bytes(f.width()) for f in fields if f.kind != 'stamp'))
    symoff, nsyms = symbol_table(a)
    last = symoff + (nsyms - 1) * NLIST_SIZE
    check('a symbol that is not an object-path stab kept its value',
          na[last:last + 16] == a[last:last + 16], 'type 0x0f at 0x%x' % last)
    digest = hashlib.sha256(preimage).digest()
    uuid_at = 32 + 72 + 8
    check('the derived UUID is the masked head of sha256(zeroed file)',
          na[uuid_at:uuid_at + UUID_WIDTH] == derived_uuid(digest), na[uuid_at:uuid_at + 16].hex())
    check('the derived UUID is version 3', (na[uuid_at + 6] >> 4) == 3)
    check('the derived UUID carries the RFC 4122 variant', (na[uuid_at + 8] & 0xC0) == 0x80)
    match = KCID_PATTERN.search(na)
    check('the derived KCID is the tail of the same digest',
          match is not None and match.group(1) == base64.b64encode(digest[16:32]),
          match.group(1).decode() if match else 'absent')

    for name, bad in (
            ('refuse a non-Mach-O', b'not a kernel at all, not a kernel at all'),
            ('refuse a kernel without an LC_UUID',
             build_synthetic(bytes(16), [1], b'C' * KCID_TEXT_WIDTH + b'==').replace(
                 struct.pack('<II', LC_UUID, 24), struct.pack('<II', 0x32, 24))),
            ('refuse a kernel without object stamps',
             build_synthetic(bytes(16), [1], b'D' * KCID_TEXT_WIDTH + b'==', stab=0x0F)),
            ('refuse a truncated kernel',
             build_synthetic(bytes(16), [1], b'E' * KCID_TEXT_WIDTH + b'==')[:120]),
    ):
        try:
            normalise(bad)
            check(name, False, 'accepted it')
        except ValueError as error:
            check(name, True, str(error)[:44])

    check('--check refuses a build that was not normalised', not is_normalised(a))

    print()
    if failures:
        for name in failures:
            print('FAIL  ' + name)
        return 1
    print('selftest: %d rules replayed as expected' % passed)
    return 0


# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('kernel', nargs='?', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--check', action='store_true',
                        help='exit non-zero unless the kernel is already normalised')
    parser.add_argument('--prove', nargs=2, type=Path, metavar=('A', 'B'),
                        help='assert two builds of the same tree converge once normalised')
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    if args.prove:
        norm = []
        for path in args.prove:
            data = path.read_bytes()
            out, counts = normalise(data)
            norm.append(out)
            print('%s: %d bytes, %d build-varying (stamps=%d uuid=%d kcid=%d), sha256 %s'
                  % (path.name, len(data), sum(1 for x, y in zip(data, out) if x != y),
                     counts['stamp'], counts['uuid'], counts['kcid'],
                     hashlib.sha256(data).hexdigest()))
        print('normalized sha256 %s' % hashlib.sha256(norm[0]).hexdigest())
        if norm[0] != norm[1]:
            print('PROVE FAIL: the two builds still differ after normalisation', file=sys.stderr)
            return 1
        print('PROVE PASS: both builds of this tree are one image of %d bytes' % len(norm[0]))
        return 0

    if not args.kernel:
        parser.error('give a kernel, --prove A B, or --selftest')
    data = args.kernel.read_bytes()
    out, counts = normalise(data)
    if args.check:
        if out != data:
            moved = sum(1 for x, y in zip(data, out) if x != y)
            print('NOT NORMALISED: %d bytes of %s are still build-varying '
                  '(stamps=%d uuid=%d kcid=%d)'
                  % (moved, args.kernel, counts['stamp'], counts['uuid'], counts['kcid']),
                  file=sys.stderr)
            return 1
        print('normalised: %s (%d object stamps, 1 LC_UUID, 1 _PrelinkKCID; sha256 %s)'
              % (args.kernel, counts['stamp'], hashlib.sha256(data).hexdigest()))
        return 0
    target = args.output or args.kernel
    if args.output is None and out == data:
        print('%s is already normalised; leaving it alone' % args.kernel)
        return 0
    target.write_bytes(out)
    print('%s: %d object stamps zeroed, LC_UUID and _PrelinkKCID derived from the file (sha256 %s)'
          % (target, counts['stamp'], hashlib.sha256(out).hexdigest()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
