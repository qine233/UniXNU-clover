#!/usr/bin/env python3
"""Replay XNU's code-signature structural validator on the host.

XNU validates every code signature blob twice over.  The first pass is purely
structural -- bsd/kern/ubc_subr.c's cs_validate_csblob() and the two helpers it
calls -- and it runs *before* any trust decision, which is why a binary whose
cdhash sits in the static trust cache can still die with

    unable to create a validated cs_blob object: 85
    proc NNNN: load code signature error 4 for file "logd"

EBADEXEC (85) is the only thing that pass can return, and it has ~25 return
sites.  On the device the kernel says only "85", so a round spent on that
message asks a question the host can answer for free: are these blobs actually
valid?

This script answers it by replaying the three functions line for line against
the bytes in a Mach-O's LC_CODE_SIGNATURE, and it names the ubc_subr.c line
that fires rather than the errno.  The line numbers are the ones in the tree
this board builds from; keep them in sync when the patch set moves that file.

Usage:
    scripts/check-cs-blob-structure.py /path/to/binary ...
    scripts/check-cs-blob-structure.py --selftest
    scripts/check-cs-blob-structure.py --dir /Volumes/OS --limit 200

--selftest is what makes a "these all pass" result worth reading: it feeds the
validator blobs that must fail, one per interesting rule, and asserts each one
reports the line it is supposed to.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

EBADEXEC = 85

# ---------------------------------------------------------------------------
# constants, from osfmk/kern/cs_blobs.h and bsd/kern/ubc_subr.c of this tree
# ---------------------------------------------------------------------------

CSMAGIC_CODEDIRECTORY = 0xFADE0C02
CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0
CSMAGIC_EMBEDDED_ENTITLEMENTS = 0xFADE7171
CSMAGIC_EMBEDDED_DER_ENTITLEMENTS = 0xFADE7172
CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT = 0xFADE8181

CS_SUPPORTSSCATTER = 0x20100
CS_SUPPORTSTEAMID = 0x20200
CS_SUPPORTSLINKAGE = 0x20600

CSSLOT_CODEDIRECTORY = 0
CSSLOT_ENTITLEMENTS = 5
CSSLOT_DER_ENTITLEMENTS = 7
CSSLOT_LAUNCH_CONSTRAINT_SELF = 8
CSSLOT_LAUNCH_CONSTRAINT_PARENT = 9
CSSLOT_LAUNCH_CONSTRAINT_RESPONSIBLE = 10
CSSLOT_LIBRARY_CONSTRAINT = 11
CSSLOT_ALTERNATE_CODEDIRECTORIES = 0x1000
CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT = CSSLOT_ALTERNATE_CODEDIRECTORIES + 5

CS_HASHTYPE_SHA1 = 1
CS_HASHTYPE_SHA256 = 2
CS_HASHTYPE_SHA256_TRUNCATED = 3
CS_HASHTYPE_SHA384 = 4

# cs_hash.cs_size / cs_digest_size, ubc_subr.c:170-205.  The truncated variant
# stores CS_SHA256_TRUNCATED_LEN (cs_blobs.h:140) but digests 32 bytes.
CS_HASH = {
    CS_HASHTYPE_SHA1: (20, 20),
    CS_HASHTYPE_SHA256: (32, 32),
    CS_HASHTYPE_SHA256_TRUNCATED: (20, 32),
    CS_HASHTYPE_SHA384: (48, 48),
}

# hashPriorities[], ubc_subr.c:232-238.  Two CodeDirectories with the same rank
# in one SuperBlob is rejected at 605.
CS_RANK = {
    CS_HASHTYPE_SHA1: 1,
    CS_HASHTYPE_SHA256_TRUNCATED: 2,
    CS_HASHTYPE_SHA256: 3,
    CS_HASHTYPE_SHA384: 4,
}

PAGE_SHIFT_4K = 12        # ubc_subr.c:130
PAGE_SHIFT = 12           # clover: arm64 4K pages (clover_defconfig)

# sizeof() of the four structures the validator measures itself against.  All
# four are __attribute__((aligned(1))), so these are plain sums of the fields
# declared before the first trailing dynamic content.
SIZEOF_GENERIC_BLOB = 8
SIZEOF_SUPER_BLOB = 12
SIZEOF_BLOB_INDEX = 8
SIZEOF_SCATTER = 24       # count(4) base(4) targetOffset(8) spare(8)
SIZEOF_CODE_DIRECTORY = 108

# CS_CodeDirectory field offsets.  codeLimit is a uint32, so the four one-byte
# fields land at 36/37/38/39 -- worth stating because reading them one byte
# early turns a valid 4K signature into "unsupported page shift: 32".
CD_MAGIC, CD_LENGTH, CD_VERSION, CD_FLAGS = 0, 4, 8, 12
CD_HASH_OFFSET, CD_IDENT_OFFSET = 16, 20
CD_N_SPECIAL, CD_N_SLOTS, CD_CODE_LIMIT = 24, 28, 32
CD_HASH_SIZE, CD_HASH_TYPE, CD_PLATFORM, CD_PAGE_SIZE = 36, 37, 38, 39
CD_SPARE2 = 40
CD_SCATTER_OFFSET = 44
CD_TEAM_OFFSET = 48
CD_SPARE3, CD_CODE_LIMIT64 = 52, 56
CD_EXEC_SEG_BASE, CD_EXEC_SEG_LIMIT, CD_EXEC_SEG_FLAGS = 64, 72, 80
CD_RUNTIME, CD_PRE_ENCRYPT_OFFSET = 88, 92
CD_LINKAGE_HASH_TYPE, CD_LINKAGE_APP_TYPE = 96, 97
CD_LINKAGE_APP_SUBTYPE, CD_LINKAGE_OFFSET, CD_LINKAGE_SIZE = 98, 100, 104

LC_CODE_SIGNATURE = 0x1D
CPU_TYPE_ARM64 = 0x0100000C

# the four Mach-O magics, both byte orders, thin and fat -- used only to skip
# scripts, plists and symlinks before opening anything
MACHO_MAGICS = (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",   # mach_o_64 LE / BE
                b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",   # mach_o 32 LE / BE
                b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",   # fat  LE / BE
                b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca")   # fat64 LE / BE


class Reject:
    """One EBADEXEC: the ubc_subr.c line that returned it and what it saw."""

    def __init__(self, line: int, detail: str) -> None:
        self.line = line
        self.detail = detail

    def __str__(self) -> str:
        return "EBADEXEC from ubc_subr.c:%d (%s)" % (self.line, self.detail)


def be32(buf: bytes, off: int) -> int:
    return struct.unpack_from(">I", buf, off)[0]


def be64(buf: bytes, off: int) -> int:
    return struct.unpack_from(">Q", buf, off)[0]


# ---------------------------------------------------------------------------
# cs_validate_blob() -- ubc_subr.c:490-497
# ---------------------------------------------------------------------------

def validate_blob(buf: bytes, off: int, length: int) -> Reject | None:
    if length < SIZEOF_GENERIC_BLOB:
        return Reject(494, "buffer holds %d bytes, less than CS_GenericBlob" % length)
    declared = be32(buf, off + 4)
    if length < declared:
        return Reject(494, "blob declares length %u, only %u bytes behind it" % (declared, length))
    return None


# ---------------------------------------------------------------------------
# cs_validate_codedirectory() -- ubc_subr.c:367-484
# ---------------------------------------------------------------------------

def validate_codedirectory(buf: bytes, off: int, length: int,
                           native4k: bool = True) -> Reject | None:
    if length < SIZEOF_CODE_DIRECTORY:
        return Reject(373, "cd length %u < sizeof(CS_CodeDirectory) %u"
                      % (length, SIZEOF_CODE_DIRECTORY))
    if be32(buf, off + CD_MAGIC) != CSMAGIC_CODEDIRECTORY:
        return Reject(376, "cd magic %08x" % be32(buf, off + CD_MAGIC))

    page_shift = buf[off + CD_PAGE_SIZE]
    allowed = page_shift in (PAGE_SHIFT_4K, PAGE_SHIFT)
    if not allowed and native4k and PAGE_SHIFT == 12 and page_shift == 14:
        # ubc_subr.c:379-381, only compiled in with -DXNU_NATIVE_CS_HASH_CACHE=1.
        allowed = True
    if not allowed:
        return Reject(383, "disallowing unsupported code signature page shift: %u" % page_shift)

    hash_type = buf[off + CD_HASH_TYPE]
    if hash_type not in CS_HASH:
        return Reject(388, "cs_find_md(%u) == NULL" % hash_type)
    cs_size = CS_HASH[hash_type][0]

    hash_size = buf[off + CD_HASH_SIZE]
    if hash_size != cs_size:
        return Reject(392, "cd->hashSize %u != cs_hash.cs_size %u" % (hash_size, cs_size))

    hash_offset = be32(buf, off + CD_HASH_OFFSET)
    if length < hash_offset:
        return Reject(396, "length %u < hashOffset %u" % (length, hash_offset))
    n_special = be32(buf, off + CD_N_SPECIAL)
    if hash_offset // cs_size < n_special:
        return Reject(401, "hashOffset/%u = %u < nSpecialSlots %u"
                      % (cs_size, hash_offset // cs_size, n_special))
    n_slots = be32(buf, off + CD_N_SLOTS)
    if (length - hash_offset) // cs_size < n_slots:
        return Reject(406, "(length %u - hashOffset %u)/%u = %u < nCodeSlots %u"
                      % (length, hash_offset, cs_size, (length - hash_offset) // cs_size, n_slots))

    version = be32(buf, off + CD_VERSION)
    scatter_offset = be32(buf, off + CD_SCATTER_OFFSET)
    if version >= CS_SUPPORTSSCATTER and scatter_offset != 0:
        if length < scatter_offset:
            return Reject(411, "length %u < scatterOffset %u" % (length, scatter_offset))
        n_pages = 0
        walk = off + scatter_offset
        while True:
            if walk + SIZEOF_SCATTER > off + length:
                return Reject(426, "scatter entry at +%u runs past the cd" % (walk - off))
            count = be32(buf, walk)
            if count == 0:
                break
            if (n_pages + count) & 0xFFFFFFFF < n_pages:
                return Reject(433, "scatter nPages overflow at +%u" % (walk - off))
            n_pages += count
            walk += SIZEOF_SCATTER

    ident_offset = be32(buf, off + CD_IDENT_OFFSET)
    if length < ident_offset:
        return Reject(449, "length %u < identOffset %u" % (length, ident_offset))
    if ident_offset != 0:
        room = length - ident_offset
        if buf.find(b"\x00", off + ident_offset, off + ident_offset + room) < 0:
            return Reject(456, "identifier has no NUL in the %u bytes after identOffset" % room)

    team_offset = be32(buf, off + CD_TEAM_OFFSET)
    if version >= CS_SUPPORTSTEAMID and team_offset != 0:
        if length < team_offset:
            return Reject(463, "length %u < teamOffset %u" % (length, team_offset))
        room = length - team_offset
        if buf.find(b"\x00", off + team_offset, off + team_offset + room) < 0:
            return Reject(468, "team id has no NUL in the %u bytes after teamOffset" % room)

    if version >= CS_SUPPORTSLINKAGE and buf[off + CD_LINKAGE_HASH_TYPE] != 0:
        link_offset = be32(buf, off + CD_LINKAGE_OFFSET)
        link_size = be32(buf, off + CD_LINKAGE_SIZE)
        if link_offset + link_size < link_offset or link_offset + link_size > length:
            return Reject(478, "linkage +%u+%u outside a cd of %u bytes"
                          % (link_offset, link_size, length))
    return None


# ---------------------------------------------------------------------------
# cs_validate_csblob() -- ubc_subr.c:516-718
# ---------------------------------------------------------------------------

def validate_csblob(buf: bytes, off: int, blob_size: int, native4k: bool = True,
                    trace: list[str] | None = None) -> Reject | None:
    error = validate_blob(buf, off, blob_size)
    if error:
        return error
    length = be32(buf, off + 4)
    magic = be32(buf, off)
    if trace is not None:
        trace.append("top blob magic=%08x declared length=%u (buffer %u)" % (magic, length, blob_size))

    if magic == CSMAGIC_EMBEDDED_SIGNATURE:
        if length < SIZEOF_SUPER_BLOB:
            return Reject(555, "superblob length %u < sizeof(CS_SuperBlob)" % length)
        count = be32(buf, off + 8)
        if (length - SIZEOF_SUPER_BLOB) // SIZEOF_BLOB_INDEX < count:
            return Reject(563, "(%u-12)/8 = %u < count %u"
                          % (length, (length - SIZEOF_SUPER_BLOB) // SIZEOF_BLOB_INDEX, count))

        have_cd = False
        best_rank = 0
        for index in range(count):
            slot_at = off + SIZEOF_SUPER_BLOB + index * SIZEOF_BLOB_INDEX
            slot_type = be32(buf, slot_at)
            slot_offset = be32(buf, slot_at + 4)
            if length < slot_offset:
                return Reject(572, "slot 0x%x offset %u > superblob length %u"
                              % (slot_type, slot_offset, length))
            sub_off = off + slot_offset
            error = validate_blob(buf, sub_off, length - slot_offset)
            if error:
                return Reject(error.line, error.detail + " [slot 0x%x]" % slot_type)
            sub_length = be32(buf, sub_off + 4)
            sub_magic = be32(buf, sub_off)
            if trace is not None:
                trace.append("  slot[%d] type=0x%x offset=%u magic=%08x length=%u"
                             % (index, slot_type, slot_offset, sub_magic, sub_length))

            is_cd = slot_type == CSSLOT_CODEDIRECTORY or (
                CSSLOT_ALTERNATE_CODEDIRECTORIES <= slot_type < CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT)
            if is_cd:
                error = validate_codedirectory(buf, sub_off, sub_length, native4k)
                if error:
                    return error
                hash_type = buf[sub_off + CD_HASH_TYPE]
                rank = CS_RANK.get(hash_type, 0)
                if trace is not None:
                    trace.append("    cd version=0x%x hashType=%u hashSize=%u pageSize=%u "
                                 "nSpecial=%u nSlots=%u hashOffset=%u identOffset=%u "
                                 "scatter=%u team=%u codeLimit=%u rank=%u"
                                 % (be32(buf, sub_off + CD_VERSION), hash_type,
                                    buf[sub_off + CD_HASH_SIZE], buf[sub_off + CD_PAGE_SIZE],
                                    be32(buf, sub_off + CD_N_SPECIAL),
                                    be32(buf, sub_off + CD_N_SLOTS),
                                    be32(buf, sub_off + CD_HASH_OFFSET),
                                    be32(buf, sub_off + CD_IDENT_OFFSET),
                                    be32(buf, sub_off + CD_SCATTER_OFFSET),
                                    be32(buf, sub_off + CD_TEAM_OFFSET),
                                    be32(buf, sub_off + CD_CODE_LIMIT), rank))
                if not have_cd or rank > best_rank:
                    have_cd = True
                    best_rank = rank
                elif rank == best_rank:
                    return Reject(605, "multiple hash=%d CodeDirectories in signature; rejecting"
                                  % hash_type)
            elif slot_type == CSSLOT_ENTITLEMENTS:
                if sub_magic != CSMAGIC_EMBEDDED_ENTITLEMENTS:
                    return Reject(619, "entitlements blob magic %08x" % sub_magic)
            elif slot_type == CSSLOT_DER_ENTITLEMENTS:
                if sub_magic != CSMAGIC_EMBEDDED_DER_ENTITLEMENTS:
                    return Reject(628, "DER entitlements blob magic %08x" % sub_magic)
            elif slot_type in (CSSLOT_LAUNCH_CONSTRAINT_SELF, CSSLOT_LAUNCH_CONSTRAINT_PARENT,
                               CSSLOT_LAUNCH_CONSTRAINT_RESPONSIBLE, CSSLOT_LIBRARY_CONSTRAINT):
                if sub_magic != CSMAGIC_EMBEDDED_LAUNCH_CONSTRAINT:
                    return Reject(637, "constraint slot 0x%x blob magic %08x" % (slot_type, sub_magic))

        if not have_cd:
            return Reject(714, "no CodeDirectory in the superblob")
        return None

    if magic == CSMAGIC_CODEDIRECTORY:
        return validate_codedirectory(buf, off, length, native4k)
    return Reject(710, "top magic %08x is neither an embedded signature nor a code directory" % magic)


# ---------------------------------------------------------------------------
# Mach-O: the range the kernel hands the validator
# ---------------------------------------------------------------------------

def cs_vector(data: bytes) -> tuple[int, int]:
    """(dataoff, datasize) of LC_CODE_SIGNATURE, as load_code_signature() sees it."""
    head = struct.unpack_from(">I", data, 0)[0]
    if head in (0xCAFEBABE, 0xCAFEBABF):
        for index in range(be32(data, 4)):
            cpu_type, _, offset, size, _ = struct.unpack_from(">IIIII", data, 8 + index * 20)
            if cpu_type == CPU_TYPE_ARM64:
                return cs_vector(data[offset:offset + size])
        raise ValueError("fat binary has no arm64 slice")

    if head in (0xFEEDFACE, 0xFEEDFACF):
        fmt = ">"
    elif head in (0xCEFAEDFE, 0xCFFAEDFE):
        fmt = "<"
    else:
        raise ValueError("not a Mach-O (magic %08x)" % head)

    mh_magic = struct.unpack_from(fmt + "I", data, 0)[0]
    ncmds = struct.unpack_from(fmt + "H", data, 16)[0]
    cursor = 32 if mh_magic in (0xFEEDFACF, 0xCFFAEDFE) else 28
    for _ in range(ncmds):
        cmd, cmd_size = struct.unpack_from(fmt + "II", data, cursor)
        if cmd == LC_CODE_SIGNATURE:
            data_off, data_size = struct.unpack_from(fmt + "II", data, cursor + 8)
            return (data_off, data_size)
        if cmd_size == 0:
            raise ValueError("zero-sized load command at %u" % cursor)
        cursor += cmd_size
    raise ValueError("no LC_CODE_SIGNATURE")


def check_file(path: pathlib.Path, native4k: bool, verbose: bool) -> Reject | None:
    data = path.read_bytes()
    try:
        (data_off, data_size) = cs_vector(data)
    except ValueError as exc:
        print("%-44s SKIP  %s" % (path.name, exc))
        return None
    trace: list[str] = [] if verbose else None
    error = validate_csblob(data, data_off, data_size, native4k, trace)
    if verbose:
        for line in trace:
            print("  " + line)
    if error:
        print("%-44s csvec off=%-8u size=%-8u %s" % (path.name, data_off, data_size, error))
    else:
        print("%-44s csvec off=%-8u size=%-8u PASSES cs_validate_csblob"
              % (path.name, data_off, data_size))
    return error


# ---------------------------------------------------------------------------
# selftest: the rules must actually fire
# ---------------------------------------------------------------------------

def build_cd(hash_type: int = CS_HASHTYPE_SHA256, page_shift: int = 12, n_slots: int = 2,
             version: int = 0x20400, ident: bytes = b"com.example.tool") -> bytes:
    """A CodeDirectory with a header, an identifier, special slots and code slots."""
    hash_size = CS_HASH[hash_type][0]
    header = 108
    ident_offset = header
    ident_padded = ident + b"\x00"
    hash_offset = ident_offset + len(ident_padded)
    hash_offset += (-hash_offset) % 16
    body = bytearray(hash_offset + (n_slots + 1) * hash_size)
    struct.pack_into(">IIIIIIII", body, 0, CSMAGIC_CODEDIRECTORY, len(body), version, 0,
                     hash_offset, ident_offset, 1, n_slots)
    struct.pack_into(">I", body, CD_CODE_LIMIT, 0x4000 * n_slots)
    body[CD_HASH_SIZE] = hash_size
    body[CD_HASH_TYPE] = hash_type
    body[CD_PLATFORM] = 0
    body[CD_PAGE_SIZE] = page_shift
    body[ident_offset:ident_offset + len(ident_padded)] = ident_padded
    for slot in range(hash_offset, len(body), hash_size):
        body[slot:slot + hash_size] = bytes([slot & 0xFF]) * hash_size
    return bytes(body)


def build_superblob(cds: list[tuple[int, bytes]], tail: bytes = b"") -> bytes:
    """A SuperBlob whose slot 0..n hold the given (type, blob) pairs."""
    count = len(cds)
    header = SIZEOF_SUPER_BLOB + count * SIZEOF_BLOB_INDEX
    offset = header
    indices = bytearray()
    payloads = bytearray()
    for slot_type, blob in cds:
        indices += struct.pack(">II", slot_type, offset)
        payloads += blob
        offset += len(blob)
    total = header + len(payloads) + len(tail)
    blob = bytearray(struct.pack(">III", CSMAGIC_EMBEDDED_SIGNATURE, total, count))
    blob += indices
    blob += payloads
    blob += tail
    return bytes(blob)


def selftest(native4k_default: bool = True) -> int:
    failures: list[str] = []
    replayed: list[str] = []

    def say(name: str, detail: str) -> None:
        replayed.append(name)
        print("  ok   %-38s %s" % (name, detail))

    def expect(name: str, want: int | None, buf: bytes, native4k: bool = native4k_default) -> None:
        error = validate_csblob(buf, 0, len(buf), native4k)
        got = error.line if error else None
        if got != want:
            failures.append("%s: expected %s, got %s"
                            % (name, "pass" if want is None else "ubc_subr.c:%d" % want,
                               "pass" if got is None else "ubc_subr.c:%d (%s)" % (got, error.detail)))
        else:
            say(name, "accepted" if want is None else "rejected at %d" % want)

    cd = build_cd()
    good = build_superblob([(CSSLOT_CODEDIRECTORY, cd)])
    expect("well-formed superblob", None, good)
    expect("raw code directory", None, cd)

    # 494: the buffer is shorter than the blob claims -- a truncated read.
    short = bytearray(good)
    struct.pack_into(">I", short, 4, len(good) + 16)
    expect("length field overruns buffer", 494, bytes(short))

    # 383: a 16K page shift is only allowed by the native 4K hash cache.
    cd14 = build_cd(page_shift=14)
    expect("16K shift with the exemption", None, build_superblob([(CSSLOT_CODEDIRECTORY, cd14)]))
    if validate_codedirectory(cd14, 0, len(cd14), native4k=False) is None:
        failures.append("16K shift without the exemption: expected ubc_subr.c:383, got pass")
    else:
        say("16K shift, no exemption", "rejected at 383")

    # 376 / 710: wrong magic.
    junk = build_superblob([(CSSLOT_CODEDIRECTORY, cd)])
    bad_magic = bytearray(junk)
    struct.pack_into(">I", bad_magic, 0, 0xFADE0C03)
    expect("unknown top magic", 710, bytes(bad_magic))

    # 392: hashSize disagrees with the hash type.
    cd_bad_size = bytearray(cd)
    cd_bad_size[CD_HASH_SIZE] = 20
    expect("hashSize != cs_size", 392, build_superblob([(CSSLOT_CODEDIRECTORY, bytes(cd_bad_size))]))

    # 388: an hash type cs_find_md() does not know.
    cd_bad_type = bytearray(cd)
    cd_bad_type[CD_HASH_TYPE] = 9
    cd_bad_type[CD_HASH_SIZE] = 32
    expect("unsupported hash type", 388, build_superblob([(CSSLOT_CODEDIRECTORY, bytes(cd_bad_type))]))

    # 406: more code slots than the buffer can hold.
    cd_many = bytearray(cd)
    struct.pack_into(">I", cd_many, CD_N_SLOTS, 1000)
    expect("nCodeSlots past the buffer", 406, build_superblob([(CSSLOT_CODEDIRECTORY, bytes(cd_many))]))

    # 401: special slots that do not fit before hashOffset.
    cd_special = bytearray(cd)
    struct.pack_into(">I", cd_special, CD_N_SPECIAL, 1000)
    expect("nSpecialSlots past hashOffset", 401, build_superblob([(CSSLOT_CODEDIRECTORY, bytes(cd_special))]))

    # 605: two SHA256 CodeDirectories -- same rank twice.
    expect("two same-rank CodeDirectories", 605,
           build_superblob([(CSSLOT_CODEDIRECTORY, cd),
                            (CSSLOT_ALTERNATE_CODEDIRECTORIES, cd)]))

    # 714: a superblob with no CodeDirectory at all.
    ent = struct.pack(">II", CSMAGIC_EMBEDDED_ENTITLEMENTS, 8)
    expect("superblob without a cd", 714, build_superblob([(CSSLOT_ENTITLEMENTS, ent)]))

    # 619: an entitlements slot that is not an entitlements blob.
    expect("entitlements slot, wrong magic", 619,
           build_superblob([(CSSLOT_CODEDIRECTORY, cd), (CSSLOT_ENTITLEMENTS, cd)]))

    # 572: a slot offset past the superblob's own length.
    two = bytearray(build_superblob([(CSSLOT_CODEDIRECTORY, cd), (CSSLOT_ENTITLEMENTS, ent)]))
    struct.pack_into(">I", two, SIZEOF_SUPER_BLOB + SIZEOF_BLOB_INDEX + 4, 100000)
    expect("slot offset past the end", 572, bytes(two))

    print()
    if failures:
        for line in failures:
            print("FAIL  " + line)
        return 1
    print("selftest: %d rules replayed as expected" % len(replayed))
    return 0


# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="*", type=pathlib.Path)
    parser.add_argument("--dir", type=pathlib.Path, help="check every Mach-O under a directory")
    parser.add_argument("--limit", type=int, default=0, help="stop after this many files")
    parser.add_argument("--trace", action="store_true", help="print every slot the validator walks")
    parser.add_argument("--no-hash-cache", action="store_true",
                        help="replay without -DXNU_NATIVE_CS_HASH_CACHE=1")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    native4k = not args.no_hash_cache

    if args.selftest:
        return selftest(native4k)

    targets = list(args.paths)
    if args.dir:
        targets += sorted(p for p in args.dir.rglob("*") if p.is_file() and not p.is_symlink())
    if not targets:
        parser.error("give a binary, --dir, or --selftest")

    checked = 0
    rejected = 0
    for path in targets:
        if not path.is_file():
            continue
        try:
            head = path.open("rb").read(4)
        except OSError:
            continue
        if head not in MACHO_MAGICS:
            continue
        error = check_file(path, native4k, args.trace)
        checked += 1
        rejected += 1 if error else 0
        if args.limit and checked >= args.limit:
            break
    print()
    print("%d Mach-O checked, %d rejected by cs_validate_csblob" % (checked, rejected))
    return 1 if rejected else 0


if __name__ == "__main__":
    sys.exit(main())
