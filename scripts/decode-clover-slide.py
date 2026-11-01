#!/usr/bin/env python3
"""Name the user-stack frames a boot captured, after recovering that boot's shared-cache slide.

usage: decode-clover-slide.py <flights/bootNN-rNNN.txt> [--cache DIR] [--slide HEX] [--tid HEX]

The runtime addresses in r266's `clover r266: pustk` rows carry this boot's shared-cache slide, and the
address table inside the extracted cache does not: its regions are UNSLID, starting at 0x180000000. Feeding
runtime addresses into that table is what made three rounds of host decoding worthless -- some frames land
in the 32 MB gaps the split cache leaves between its executable and data regions, and the rest get blamed on
whichever image happens to cover the wrong address.

The slide comes off the board, not out of a search. `clover r221: sbm2x` prints a runtime pc beside the eight
instruction words around it; those words are located by byte search in the extracted files, and each hit
gives the one unslid address those bytes live at, hence one candidate slide. A byte of the cache cannot be
mapped at two runtime addresses, so a candidate that is not page aligned (arm64 iOS maps 16 KiB pages) is
impossible and is thrown away. Whatever survives is the slide, and the tool refuses to name frames without
one.

Each frame then unslides into a cache vmaddr and is read out of the image that owns that address: the path
from LC_ID_DYLIB, the containing function from LC_FUNCTION_STARTS, and the symbol table from LC_SYMTAB.
Non-exported symbol names in this cache are the literal string <redacted> -- Apple leaves only exported
symbols named -- so what a frame reliably gives is its image and its offset, and those are printed as
themselves rather than dressed up as a function name.
"""

import argparse
import bisect
import glob
import mmap
import os
import re
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
CPU_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
LC_ID_DYLIB = 0x0D
LC_SYMTAB = 0x02
LC_FUNCTION_STARTS = 0x26
VM_PROT_EXECUTE = 0x4
PAGE_ALIGN = 0x4000
MH_FILE_TYPES = (2, 6)          # MH_EXECUTE, MH_DYLIB
N_STAB = 0xE0                   # mach-o: a set of these bits means a debug entry, not a symbol
N_SECT = 0x0E                   # mach-o: the type field for "defined in a section", not 0x01
MH_MAGIC_BYTES = b'\xcf\xfa\xed\xfe'
SYMTAB_READ_LIMIT = 1 << 26


def uleb(buf, i):
    value, shift = 0, 0
    while i < len(buf):
        byte = buf[i]
        i += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            break
        shift += 7
    return value, i


class CacheFile:
    """One extracted cache file: the regions it maps and the Mach-O headers it carries."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.handle = open(path, 'rb')
        self.blob = mmap.mmap(self.handle.fileno(), 0, access=mmap.ACCESS_READ)
        if self.blob[:7] != b'dyld_v1':
            raise ValueError('%s: not a dyld shared cache (%r)' % (path, bytes(self.blob[:16])))
        mapping_off, mapping_count = struct.unpack_from('<II', self.blob, 16)
        self.regions = []
        for i in range(mapping_count):
            addr, size, file_off, max_prot, init_prot = struct.unpack_from(
                '<QQQII', self.blob, mapping_off + i * 32)
            self.regions.append((addr, size, file_off, max_prot, init_prot))

    def unslid_of(self, file_off):
        """The cache vmaddr a byte of this file is mapped at, or None when nothing maps it."""
        for addr, size, base, max_prot, init_prot in self.regions:
            if base <= file_off < base + size:
                return addr + (file_off - base)
        return None

    def headers(self):
        out = []
        for addr, size, file_off, max_prot, init_prot in self.regions:
            if not init_prot & VM_PROT_EXECUTE:
                continue
            # Every header, not one per page: a cache packs its images on 32-byte boundaries, so the
            # page step used first missed most of them and left frames "in no image". memmem over the
            # region's own byte range finds all of them, and the load-command checks below reject the
            # data that happens to begin with the magic.
            rel = file_off
            limit = file_off + size
            while True:
                off = self.blob.find(MH_MAGIC_BYTES, rel, limit)
                if off < 0:
                    break
                rel = off + 4
                if off + 32 > len(self.blob):
                    break
                _, cputype, _, filetype, ncmds, sizeofcmds, _ = struct.unpack_from(
                    '<IIIIIII', self.blob, off)
                if (cputype != CPU_ARM64 or filetype not in MH_FILE_TYPES
                        or not 0 < ncmds < 512 or not 32 < sizeofcmds < 0x40000):
                    continue
                image = self._read_header(off, ncmds, sizeofcmds)
                if image is not None:
                    out.append(image)
        return out

    def _read_header(self, off, ncmds, sizeofcmds):
        p = off + 32
        end = p + sizeofcmds
        text = None
        path = None
        cmds = {}
        while p + 8 <= end:
            cmd, cmdsize = struct.unpack_from('<II', self.blob, p)
            if cmdsize < 8 or p + cmdsize > end:
                return None
            if cmd == LC_SEGMENT_64:
                seg = bytes(self.blob[p + 8:p + 24]).rstrip(b'\0')
                vm, vsz = struct.unpack_from('<QQ', self.blob, p + 24)
                if seg == b'__TEXT' and text is None:
                    text = (vm, vsz)
            elif cmd == LC_ID_DYLIB:
                start = p + struct.unpack_from('<I', self.blob, p + 8)[0]
                stop = self.blob.find(b'\0', start, p + cmdsize)
                if stop > 0:
                    path = bytes(self.blob[start:stop]).decode('utf-8', 'replace')
            elif cmd in (LC_SYMTAB, LC_FUNCTION_STARTS):
                if cmd in cmds:
                    return None
                cmds[cmd] = p
            p += cmdsize
        if text is None:
            return None
        vm, vsz = text
        # A header is only real if its own __TEXT belongs to the region it was found in: that single test
        # is what rejects the page of data that happened to start with the Mach-O magic.
        if not any(addr <= vm < addr + size for addr, size, _f, _mx, _it in self.regions):
            return None
        return (vm, vsz, path or '<unnamed>', off, dict(cmds), self)


def nearest(pairs, addr):
    if not pairs:
        return None
    i = bisect.bisect_right([v for v, _ in pairs], addr) - 1
    return pairs[i] if i >= 0 else None


class Namer:
    def __init__(self, files, linkedit):
        self.images = []
        for cf in files:
            self.images.extend(cf.headers())
        self.images.sort(key=lambda i: i[0])
        self.starts = [i[0] for i in self.images]
        self.linkedit = linkedit
        self._cache = {}

    def image_for(self, vmaddr):
        i = bisect.bisect_right(self.starts, vmaddr) - 1
        while i >= 0:
            vm, vsz, path, off, cmds, cf = self.images[i]
            if vm <= vmaddr < vm + vsz:
                return self.images[i]
            i -= 1
        return None

    def _linkedit_slice(self, off, size):
        for host in self.linkedit:
            if off + size <= len(host.blob):
                return host.blob, off
        return None, None

    def function_starts(self, image):
        key = (image[0], 'fs')
        if key in self._cache:
            return self._cache[key]
        vm, vsz, path, off, cmds, cf = image
        out = []
        if LC_FUNCTION_STARTS in cmds:
            foff, fsize = struct.unpack_from('<II', cf.blob, cmds[LC_FUNCTION_STARTS] + 8)
            blob, base = self._linkedit_slice(foff, min(fsize, SYMTAB_READ_LIMIT))
            if blob is not None:
                data = blob[base:base + fsize]
                addr, i = vm, 0
                while i < len(data):
                    delta, i = uleb(data, i)
                    if delta == 0:
                        break
                    addr += delta
                    if not vm <= addr < vm + vsz:
                        break
                    out.append((addr, '<function start>'))
        self._cache[key] = out
        return out

    def symbols(self, image):
        key = (image[0], 'sym')
        if key in self._cache:
            return self._cache[key]
        vm, vsz, path, off, cmds, cf = image
        out = []
        if LC_SYMTAB in cmds:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', cf.blob, cmds[LC_SYMTAB] + 8)
            blob, base = self._linkedit_slice(symoff, min(nsyms * 16, SYMTAB_READ_LIMIT))
            sbuf, sbase = self._linkedit_slice(stroff, min(strsize, SYMTAB_READ_LIMIT))
            if blob is not None and sbuf is not None:
                for i in range(nsyms):
                    n_strx, n_type, _sect, _desc, n_value = struct.unpack_from('<IBBHQ', blob, base + i * 16)
                    if n_type & N_STAB or not n_value or (n_type & 0x0E) != N_SECT:
                        continue
                    start = sbase + n_strx
                    stop = sbuf.find(b'\0', start, sbase + strsize)
                    if stop < 0:
                        continue
                    out.append((n_value, sbuf[start:stop].decode('utf-8', 'replace')))
                out.sort()
        self._cache[key] = out
        return out


def slide_candidates(files, pc, words):
    needle = b''.join(struct.pack('<I', w) for w in words)
    out = []
    for cf in files:
        start = 0
        while True:
            hit = cf.blob.find(needle, start)
            if hit < 0:
                break
            unslid = cf.unslid_of(hit)
            start = hit + 1
            if unslid is None or unslid > pc:
                continue
            out.append((pc - unslid, cf.name, hit, unslid))
    return out


def parse_probe_rows(path):
    for line in open(path, errors='replace'):
        m = re.search(r'clover r221: sbm2x t=(\d+) .*pc=([0-9a-f]+) lr=([0-9a-f]+) '
                      r'poff=([0-9a-f]+).* c=([0-9a-f.]+)', line)
        if m:
            yield {'t': int(m.group(1)), 'pc': int(m.group(2), 16), 'lr': int(m.group(3), 16),
                   'words': [int(w, 16) for w in m.group(5).split('.') if w]}


def parse_stack_rows(path):
    rows = {}
    for line in open(path, errors='replace'):
        m = re.search(r'clover r266: pustk t=(\d+) pid=(\d+) tid=([0-9a-f]+) cont=([0-9a-f]+) '
                      r'we=([0-9a-f]+) .*?lr=([0-9a-f]+) fn=(\d+)(.*)', line)
        if not m:
            continue
        frames = [int(x, 16) for x in m.group(8).split()]
        rows.setdefault((int(m.group(2)), int(m.group(3), 16)),
                        {'t': int(m.group(1)), 'cont': int(m.group(4), 16), 'we': int(m.group(5), 16),
                         'lr': int(m.group(6), 16), 'frames': frames})
        rows[(int(m.group(2)), int(m.group(3), 16))]['frames'] = frames
    return rows


def resolve(namer, slide, addr):
    unslid = addr - slide
    image = namer.image_for(unslid)
    if image is None:
        return unslid, None
    vm, vsz, path, off, cmds, cf = image
    bits = []
    fs = namer.function_starts(image)
    near = nearest(fs, unslid)
    if near:
        bits.append('function %#x +%#x' % (near[0], unslid - near[0]))
    sym = namer.symbols(image)
    n = nearest(sym, unslid)
    if n:
        bits.append('sym %s +%#x' % (n[1], unslid - n[0]))
    return unslid, '%s (+%#x of %#x) %s' % (path, unslid - vm, vsz, '  '.join(bits))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('flight')
    ap.add_argument('--cache', default=os.path.expanduser('~/clover-out/dyld-cache'))
    ap.add_argument('--slide', type=lambda v: int(v, 0))
    ap.add_argument('--tid', type=lambda v: int(v, 16))
    args = ap.parse_args()

    files = []
    for path in sorted(glob.glob(os.path.join(args.cache, 'dyld_shared_cache_arm64*'))):
        if not path.endswith('.symbols'):
            files.append(CacheFile(path))
    if not files:
        print('%s: no dyld_shared_cache_arm64* files' % args.cache, file=sys.stderr)
        return 2
    linkedit = [cf for cf in files if cf.name.endswith('.dyldlinkedit')]
    namer = Namer(files, linkedit)
    print('%d cache files, %d images named from their own headers' % (len(files), len(namer.images)))

    slide = args.slide
    if slide is None:
        rows = list(parse_probe_rows(args.flight))
        if not rows:
            print('no clover r221: sbm2x row to recover the slide from; pass --slide', file=sys.stderr)
            return 2
        agree, seen = {}, set()
        for row in rows:
            for cand, name, hit, unslid in slide_candidates(files, row['pc'], row['words']):
                # One report per distinct byte, not per beat: the same words sit in the same place every
                # time the probe fires, so the rows only repeat the first one.
                key = (cand, name, hit)
                if key in seen:
                    agree[cand].append(row['t']) if cand in agree else None
                    continue
                seen.add(key)
                if cand % PAGE_ALIGN == 0 and cand > 0:
                    agree.setdefault(cand, [row['t']])
                    print('  %s file %s -> unslid %s gives slide %#x, page aligned'
                          % (name, hex(hit), hex(unslid), cand))
                else:
                    print('  rejected: %s file %s -> unslid %s would need slide %#x, which is not page '
                          'aligned, so those bytes cannot be mapped at pc' % (name, hex(hit), hex(unslid), cand))
        if len(agree) > 1:
            print('  two page-aligned slides survive; %d rows agree on the first' % len(agree))
        if not agree:
            print('the probe bytes are in no mapped region on a page-aligned slide; refusing to name frames',
                  file=sys.stderr)
            return 1
        slide = max(agree.items(), key=lambda kv: len(kv[1]))[0]
        if len(agree) > 1:
            print('two page-aligned slides survive: %s -- taking the one most rows agree on'
                  % ', '.join(hex(s) for s in agree))
    print('slide %#x' % slide)

    stacks = parse_stack_rows(args.flight)
    if not stacks:
        print('no clover r266: pustk rows in this flight')
        return 0
    for (pid, tid), st in sorted(stacks.items()):
        if args.tid is not None and tid != args.tid:
            continue
        print('\n== pid=%d tid=%s t=%d cont=%#x we=%#x ==' % (
            pid, hex(tid), st['t'], st['cont'], st['we']))
        for label, addr in [('lr', st['lr'])] + [('frame%d' % i, f) for i, f in enumerate(st['frames'])]:
            unslid, named = resolve(namer, slide, addr)
            if named is None:
                print('  %-8s %#-12x -> %#-12x outside every image __TEXT' % (label, addr, unslid))
            else:
                print('  %-8s %#-12x -> %#-12x %s' % (label, addr, unslid, named))
    return 0


if __name__ == '__main__':
    sys.exit(main())
