#!/usr/bin/env python3
"""Symbolize a dyld shared cache trap address read from the board.

The user-breakpoint probe prints `foff = VME_OFFSET(entry) + (pc - vme_start)`.
For the iOS 17 split cache the shared region is mapped with vme_start
0x180000000 and file offset 0, so foff is the offset of the trapping pc inside
the *shared region*, not a file offset:  the address in the cache's own unslid
layout is 0x180000000 + foff.

The probe also brings back the eight instructions around the trap.  This tool
uses those words to find the one place in the extracted cache that carries
them, names the image, the function and the nearest export, and (when the
offsets resolve) the source string the fatal path was about to print.

Refusing to guess is part of the contract: if the eight words do not appear,
or the function and export data cannot be read, the tool exits non-zero
instead of naming something plausible.

Usage:
  decode-clover-cache-offset.py --cache /path/to/ios/System/Library/Caches/com.apple.dyld \\
      --foff 0x3a17b2ec --code aa1503e0.9423e402.94023d17.9423e307.d4200000.d4200020.f02436e8.f942a100
"""

import argparse
import glob
import mmap
import os
import struct
import sys


SHARED_REGION_START = 0x180000000
MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
LC_ID_DYLIB = 0x0D
LC_DYLD_INFO_ONLY = 0x80000022
LC_FUNCTION_STARTS = 0x26
LC_DYLD_EXPORTS_TRIE = 0x80000033
LC_SYMTAB = 0x02
VM_PROT_EXECUTE = 4


def is_break(word):
    return (word & 0xFFE0001F) == 0xD4200000


def uleb(buf, i):
    value = 0
    shift = 0
    while i < len(buf):
        byte = buf[i]
        i += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            break
    return value, i


class CacheFile:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.file = open(path, 'rb')
        self.blob = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        if not self.blob[:7] == b'dyld_v1':
            raise ValueError(f'{path}: not a dyld cache (magic {self.blob[:16]!r})')
        self.uuid = bytes(self.blob[88:104])
        mapping_off, mapping_count = struct.unpack_from('<II', self.blob, 16)
        self.mappings = []
        for i in range(mapping_count):
            addr, size, file_off, max_prot, init_prot = struct.unpack_from(
                '<QQQII', self.blob, mapping_off + i * 32)
            self.mappings.append((addr, size, file_off, init_prot, max_prot))
        self.images = None

    def read_at(self, offset, size):
        return bytes(self.blob[offset:offset + size])

    def image_for_vmaddr(self, vmaddr):
        for image in self.parse_images():
            if image.text_vmaddr <= vmaddr < image.text_vmaddr + image.text_size:
                return image
        return None

    def parse_images(self):
        """Find every Mach-O header in an executable mapping.

        The image array and imagesText are the documented way in, but their
        header layout differs between cache format versions.  Scanning the
        executable mappings for MH_MAGIC_64 with sane load commands yields
        the same set without pinning a layout.
        """
        if self.images is not None:
            return self.images
        images = []
        for addr, size, file_off, init_prot, _ in self.mappings:
            if not init_prot & VM_PROT_EXECUTE:
                continue
            rel = 0
            while rel + 32 <= size:
                off = file_off + rel
                if off + 4 > len(self.blob):
                    break
                if struct.unpack_from('<I', self.blob, off)[0] == MH_MAGIC_64:
                    _, cputype, _, filetype, ncmds, sizeofcmds, _ = struct.unpack_from(
                        '<IIIIIII', self.blob, off)
                    if (cputype == CPU_TYPE_ARM64 and filetype in (2, 6)
                            and 0 < ncmds < 4096 and 32 < sizeofcmds < (1 << 24)):
                        image = MachOImage(self, off)
                        if image.text_vmaddr:
                            images.append(image)
                rel += 0x1000
        images.sort(key=lambda i: i.text_vmaddr)
        self.images = images
        return images


class MachOImage:
    def __init__(self, cache, file_off):
        self.cache = cache
        self.file_off = file_off
        blob = cache.blob
        _, _, _, _, ncmds, sizeofcmds, _ = struct.unpack_from('<IIIIIII', blob, file_off)
        self.path = None
        self.text_vmaddr = None
        self.text_size = 0
        self.function_starts = None
        self.export_data = None
        p = file_off + 32
        end = p + sizeofcmds
        for _ in range(ncmds):
            if p + 8 > end:
                break
            cmd, cmdsize = struct.unpack_from('<II', blob, p)
            if cmdsize < 8 or p + cmdsize > end:
                break
            if cmd == LC_SEGMENT_64:
                segname = bytes(blob[p + 8:p + 24]).rstrip(b'\0').decode('ascii', 'replace')
                vmaddr, vmsize = struct.unpack_from('<QQ', blob, p + 24)
                if segname == '__TEXT':
                    self.text_vmaddr = vmaddr
                    self.text_size = vmsize
            elif cmd == LC_ID_DYLIB:
                name_off = struct.unpack_from('<I', blob, p + 8)[0]
                start = p + name_off
                stop = blob.find(b'\0', start, p + cmdsize)
                self.path = bytes(blob[start:stop]).decode('utf-8', 'replace')
            elif cmd == LC_FUNCTION_STARTS:
                self.function_starts = struct.unpack_from('<II', blob, p + 8)
            elif cmd in (LC_DYLD_EXPORTS_TRIE, LC_DYLD_INFO_ONLY):
                if cmd == LC_DYLD_INFO_ONLY:
                    self.export_data = struct.unpack_from('<II', blob, p + 40)
                else:
                    self.export_data = struct.unpack_from('<II', blob, p + 8)
            p += cmdsize
        self.path = self.path or '<unnamed>'


class Linkedit:
    """The split cache keeps every image's linkedit payload in a subcache
    whose load-command offsets are plain file offsets into that file."""

    def __init__(self, paths):
        self.files = []
        for path in paths:
            f = open(path, 'rb')
            self.files.append(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))

    def function_starts(self, image):
        if not image.function_starts:
            return []
        off, size = image.function_starts
        for blob in self.files:
            if off + size > len(blob):
                continue
            data = blob[off:off + size]
            addr = image.text_vmaddr
            starts = []
            i = 0
            sane = True
            while i < len(data):
                delta, i = uleb(data, i)
                if delta == 0:
                    break
                addr += delta
                if not image.text_vmaddr <= addr < image.text_vmaddr + image.text_size:
                    sane = False
                    break
                starts.append(addr)
            if sane and starts:
                return starts
        return []

    def exports(self, image):
        if not image.export_data:
            return []
        off, size = image.export_data
        for blob in self.files:
            if off + size > len(blob):
                continue
            data = blob[off:off + size]
            found = []
            try:
                self._walk_exports(data, 0, image.text_vmaddr, '', found, 0)
            except (IndexError, RecursionError):
                continue
            if found:
                found.sort()
                return found
        return []

    def _walk_exports(self, data, node, text, prefix, found, depth):
        if depth > 64:
            return
        i = node
        terminal, i = uleb(data, i)
        if terminal:
            stop = i + terminal
            flags, j = uleb(data, i)
            if not flags & 0x08:
                address, _ = uleb(data, j)
                found.append((text + address, prefix))
            i = stop
        if i >= len(data):
            return
        children = data[i]
        i += 1
        for _ in range(children):
            if i >= len(data):
                return
            stop = data.find(b'\0', i)
            if stop < 0:
                return
            label = data[i:stop].decode('utf-8', 'replace')
            i = stop + 1
            child, i = uleb(data, i)
            self._walk_exports(data, child, text, prefix + label, found, depth + 1)


def parse_code(text):
    return [int(part, 16) for part in text.replace('.', ' ').replace(',', ' ').split()]


def locate(cache_paths, code, brk_imm):
    """Return (cache, hit_file_offset, pc_offset_in_window) for the code words."""
    pattern = struct.pack(f'<{len(code)}I', *code)
    hits = []
    for path in cache_paths:
        cache = CacheFile(path)
        start = 0
        while True:
            hit = cache.blob.find(pattern, start)
            if hit < 0:
                break
            for index, word in enumerate(code):
                if is_break(word) and (brk_imm is None or (word >> 5) & 0xFFFF == brk_imm):
                    hits.append((cache, hit, index))
            start = hit + 1
    return hits


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cache', required=True, help='directory holding dyld_shared_cache_arm64*')
    parser.add_argument('--foff', required=True, help='region offset printed by the probe')
    parser.add_argument('--code', required=True, help='eight code words, dot separated')
    parser.add_argument('--brk', type=lambda v: int(v, 0), default=None,
                        help='brk immediate printed by the probe, e.g. 0x0')
    args = parser.parse_args()

    foff = int(args.foff, 16)
    code = parse_code(args.code)
    if len(code) != 8:
        print('code must be eight words', file=sys.stderr)
        return 2
    region_pc = SHARED_REGION_START + foff

    paths = sorted(glob.glob(os.path.join(args.cache, 'dyld_shared_cache*')))
    paths = [p for p in paths if not p.endswith('.symbols')]
    if not paths:
        print(f'{args.cache}: no dyld_shared_cache* files', file=sys.stderr)
        return 2

    linkedit = Linkedit([p for p in paths if p.endswith('.dyldlinkedit')])
    hits = locate(paths, code, args.brk)
    if not hits:
        print('no cache file carries the given code words', file=sys.stderr)
        return 1

    print(f'region pc {region_pc:#x} (shared start {SHARED_REGION_START:#x} + foff {foff:#x})')
    print(f'{len(hits)} place(s) carry the eight words:')
    for cache, hit, index in hits:
        pc_in_file = hit + index * 4
        # Which mapping holds this file offset?
        for addr, size, file_off, init_prot, max_prot in cache.mappings:
            if file_off <= pc_in_file < file_off + size:
                vmaddr = addr + (pc_in_file - file_off)
                print(f'  {cache.name}: file {pc_in_file:#x} mapping {addr:#x}+{size:#x} '
                      f'prot {init_prot:#x} -> cache vmaddr {vmaddr:#x}')
                image = cache.image_for_vmaddr(vmaddr)
                if image is None:
                    print('    no image __TEXT covers this address')
                    break
                print(f'    image {image.path}')
                print(f'    text {image.text_vmaddr:#x}+{image.text_size:#x} '
                      f'offset {vmaddr - image.text_vmaddr:#x}')
                starts = linkedit.function_starts(image)
                before = [s for s in starts if s <= vmaddr]
                if before:
                    print(f'    function start {before[-1]:#x} '
                          f'(+{vmaddr - before[-1]:#x} into it)')
                later = [s for s in starts if s > vmaddr]
                if later:
                    print(f'    next start {later[0]:#x} ({later[0] - vmaddr:#x} after)')
                exports = linkedit.exports(image)
                named = [e for e in exports if e[0] <= vmaddr]
                if named:
                    print(f'    nearest export {named[-1][1]} at {named[-1][0]:#x} '
                          f'(+{vmaddr - named[-1][0]:#x})')
                break
    return 0


if __name__ == '__main__':
    sys.exit(main())
