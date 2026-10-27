#!/usr/bin/env python3
"""Make the prelinked AMFI exec hook allow unsigned processes.

The kernel already ignores code-signature failures (cs_enforcement_disable=1),
but the prelinked AMFI kext still has two deny branches in its exec hook:

  1. VA 0xfffffff004ab40e4  cbz w9, allow     (r163: CS_REQUIRE_LV)
  2. VA 0xfffffff004ab416c  tbz w0,#0, kill   (r164: no code signature)

Both sites are located by an exact byte signature instead of a hard-coded file
offset or VA: the prelink moves the whole kext when the kernel text grows, but
the kext's own instructions and relative branches do not change.

Usage:
    patch-clover-amfi.py SOURCE DEST
    patch-clover-amfi.py --check FILE

SOURCE is the kernel step's kernel.macho; DEST is the copy the payload step
takes through `--kernel`. --check exits 0 only when both sites already carry
the allow-path instruction, so the payload checkers can assert the result.
"""

import hashlib
import struct
import sys

# (signature that precedes the site, expected word, replacement, description)
SITES = (
    (bytes.fromhex("e00300f940c1fff00014259157000014b80f40f995570094080340b9090113120900092a"),
     0x34000229, 0x14000011,
     "exec hook CS_REQUIRE_LV check: cbz w9 -> b (r163)"),
    (bytes.fromhex("ff7f01a9e0630091e3430091e10316aae20315aaa9000094"),
     0x36000520, 0xD503201F,
     "exec hook signature check: tbz w0,#0 -> nop (r164)"),
)


def load_segments(data):
    magic, _, _, _, _, _, ncmds, _ = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit("not a 64-bit little-endian Mach-O")
    offset = 32
    segments = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmd == 0x19:  # LC_SEGMENT_64: segname[16] then vmaddr, vmsize, fileoff, filesize
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, offset + 24)
            segments.append((vmaddr, vmsize, fileoff, filesize))
        offset += cmdsize
    return segments


def file_to_va(segments, file_offset):
    for vmaddr, vmsize, fileoff, filesize in segments:
        if fileoff <= file_offset < fileoff + filesize:
            return vmaddr + (file_offset - fileoff)
    return None


def find_site(data, signature):
    first = data.find(signature)
    if first < 0:
        raise SystemExit("signature %s not found" % signature.hex())
    if data.find(signature, first + 1) >= 0:
        raise SystemExit("signature %s is not unique" % signature.hex())
    return first + len(signature)


def main(argv):
    if len(argv) == 3 and argv[1] == "--check":
        data = open(argv[2], "rb").read()
        segments = load_segments(data)
        bad = 0
        for signature, expected, replacement, description in SITES:
            site = find_site(data, signature)
            word = struct.unpack_from("<I", data, site)[0]
            va = file_to_va(segments, site)
            ok = word == replacement
            print("%s  file 0x%x VA %s  %s  %s" % (
                "ok  " if ok else "FAIL", site,
                "0x%x" % va if va is not None else "?", description,
                "0x%08x" % word))
            bad += 0 if ok else 1
        return 1 if bad else 0

    if len(argv) != 3:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: %s SOURCE DEST | --check FILE" % argv[0], file=sys.stderr)
        return 2

    source, dest = argv[1], argv[2]
    data = bytearray(open(source, "rb").read())
    segments = load_segments(data)
    for signature, expected, replacement, description in SITES:
        site = find_site(data, signature)
        word = struct.unpack_from("<I", data, site)[0]
        va = file_to_va(segments, site)
        if word == replacement:
            print("already patched  file 0x%x VA 0x%x  %s" % (site, va, description))
            continue
        if word != expected:
            raise SystemExit("file 0x%x holds 0x%08x, expected 0x%08x (%s)" % (
                site, word, expected, description))
        struct.pack_into("<I", data, site, replacement)
        print("patched          file 0x%x VA 0x%x  0x%08x -> 0x%08x  %s" % (
            site, va, expected, replacement, description))
    open(dest, "wb").write(data)
    print("wrote %s sha256 %s" % (dest, hashlib.sha256(data).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
