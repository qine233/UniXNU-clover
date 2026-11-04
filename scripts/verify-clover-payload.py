#!/usr/bin/env python3.13
"""Byte-verify what a clover payload volume actually carries.

Every round rebuilds the payload, and the FAT cluster chains move whenever the
root image changes size, so comparing whole images across rounds says nothing.
What matters is per file: the kernel and the ADT slots that must not change, and
the init program plus its trust cache, which must be byte-identical to what
scripts/make-clover-root.py just signed.

This reads each file out of the image with mtools (`mdir`/`mcopy`, brew install
mtools) and hashes it, so one command both checks the round and produces the
ledger numbers that go into README.md.

Usage: verify-clover-payload.py [image ...]     # default: the built payload
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import struct
import subprocess
import sys
import tomllib

XNU_PAYLOAD = pathlib.Path("/Users/zhouzhou/UniXNU/out/clover/payload/payload.img")
LOCAL_ROOT = pathlib.Path("/Users/zhouzhou/UniXNU-clover/build/clover-root/launchd.macho")
LOCAL_CACHE = pathlib.Path("/Users/zhouzhou/UniXNU-clover/build/clover-trust/trustcache.bundle")
LOCAL_KERNEL = pathlib.Path("/Users/zhouzhou/UniXNU/out/clover/kernel/kernel.macho")
# The assembled build tree: scripts/build-clover-pmgr.py records the original-PMGR addresses it
# audited here, and kits/NativePrelink (a symlink into the UniXNU base) parses Mach-O for it.
TREE = pathlib.Path("/Users/zhouzhou/UniXNU/out/clover/tree")
PMGR_REPORT = TREE / "build/clover-pmgr/build-report.json"
# Round177: the tool that turns the three fields a link cannot reproduce into functions of the rest of
# the kernel, so two builds of one tree are one file. Imported by path, not copied, so the checker and
# the round flow can never drift apart.
NORMALIZER = pathlib.Path(__file__).with_name("normalize-kernel-macho.py")

PAYLOADS = "Payloads"
FILES = ["unixnu.toml", "kernel.macho", "scratch-data.img", "kernel.macho.adt",
         "kernel.macho.trustcache"]


def mcopy_out(image: pathlib.Path, path: str) -> bytes:
    """Read one file out of the FAT image on stdout."""
    return subprocess.run(
        ["mcopy", "-i", str(image), f"::{path}", "-"],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout


def mdir(image: pathlib.Path, path: str) -> str:
    return subprocess.run(
        ["mdir", "-i", str(image), f"::{path}"],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout


def locate(image: pathlib.Path, name: str) -> str:
    """The in-volume path of `name`, found by looking at both directories."""
    for candidate in (f"EFI/UniXNU/{name}", f"EFI/UniXNU/{PAYLOADS}/{name}"):
        parent = candidate.rsplit("/", 1)[0]
        listing = mdir(image, parent)
        short = candidate.rsplit("/", 1)[1]
        if short in listing or short.split(".")[0][:6] in listing:
            return candidate
    raise KeyError(name)


def check_pmgr(kernel: bytes) -> bool:
    """The round83 provider binds the original ApplePMGR by absolute address, so the addresses
    written into build/clover-pmgr/original-bindings.S have to still name those symbols in the
    kernel this round actually linked. A layout shift would otherwise ship a power manager that
    dispatches into the wrong function, and the only sign on the panel would be round82's
    `AMFI: No PMGR?` again -- indistinguishable from never having published a nub."""
    label = "kernel.macho: original-PMGR bindings alive"
    if not PMGR_REPORT.exists():
        print(f"{label:42} SKIPPED (no {PMGR_REPORT})")
        return False
    report = json.loads(PMGR_REPORT.read_text())
    # Our own helper aliases are absolute symbols this object defines for the link; they are not
    # exported by the kernel and cannot be looked up in it.
    originals = {name: int(value, 16) for name, value in report["bindings"].items()
                 if not name.startswith("_clover_")}
    sys.path.insert(0, str(TREE / "kits/NativePrelink"))
    from native_prelink import exports, macho  # noqa: PLC0415 - needs the tree on sys.path
    names = {name: va for va, found in exports(kernel, macho(kernel)).items() for name in found}
    drifted = sorted(name for name, va in originals.items() if names.get(name) != va)
    linked = b"CloverPMGR: failed at stage" in kernel
    ok = not drifted and linked
    print(f"{label:42} {'OK' if ok else 'FAIL'} "
          f"({len(originals)} bindings, {len(drifted)} moved"
          f"{', ' + ', '.join(drifted) if drifted else ''}; provider text "
          f"{'present' if linked else 'absent'})")
    return ok


def exec_text_ranges(blob: bytes) -> list[tuple[int, int]]:
    """File ranges of the executable text segments in a Mach-O kernel.

    Two independent `build kernel` runs do not produce identical files: emit_prelink mints a fresh
    prelink UUID, which lands in LC_UUID, in __PRELINK_INFO, and in a few __LINKEDIT records. The
    entry-probe patch, by contrast, only writes code, and it lands in __TEXT_EXEC and
    __PLK_TEXT_EXEC (measured against the archived pre-patch kernel: 6838 bytes, 35 runs, nothing
    outside those two). Keeping the byte comparison inside them is what makes "the patch stayed
    local" a statement about code rather than about a UUID the next build is free to change."""
    if len(blob) < 32 or struct.unpack_from("<I", blob, 0)[0] != 0xFEEDFACF:
        return []
    ncmds = struct.unpack_from("<I", blob, 16)[0]
    offset, ranges = 32, []
    for _ in range(ncmds):
        command, size = struct.unpack_from("<II", blob, offset)
        if command == 0x19 and size >= 72:
            name, _va, _vs, fileoff, filesize = struct.unpack_from("<16sQQQQ", blob, offset + 8)
            if name.rstrip(b"\0") in (b"__TEXT_EXEC", b"__PLK_TEXT_EXEC"):
                ranges.append((fileoff, fileoff + filesize))
        offset += size
    return ranges


def check_sdhci(kernel: bytes) -> bool:
    """drivers/CloverSdhci is linked as a plain archive, not with -force_load (see
    configs/clover_defconfig), so nothing but a reference keeps it: the link would silently drop
    the whole eMMC instrument if the call in sheng_pmgr_run() ever went away. These eight facts
    prove the member is in this image -- the boot argument it parses, the dump each mode builds,
    mode 4's "no pwr" verdict (an address taken in a ternary, so never folded), the panic every
    failure path takes, and the four symbols this image must export (run, block read, block
    write, panic append). The CMD13
    report strings used to
    be checked here, but the panic hook forced the whole driver to -mgeneral-regs-only, and
    without SIMD clang now folds those two twelve-byte literals into immediate stores: their
    characters are in the text (see cmd13Probe) but the rodata strings are gone."""
    label = "kernel.macho: CloverSdhci linked"
    found = {name: name.encode() in kernel
             for name in ("clover-emmc", "look-only dump of c0c4000",
                          "no pwr", "CloverSdhci: failed at stage",
                          "CloverSdhci r159: link hs=%d/%x bw=%d/%x clk=%x hc=%x hdr=%x/%x ok=%d",
                          "CloverSdhci r159: multi %s fail lba=%u blocks=%u st=0x%x w=%u/%u (once)",
                          "_clover_emmc_run", "_clover_emmc_block_read",
                          "_clover_emmc_block_write", "_clover_emmc_panic_append")}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}" for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_block(kernel: bytes) -> bool:
    """drivers/CloverBlock is the other plain archive in the link, kept alive only because
    CloverSdhci's mode 21 tail names clover_block_run(). Round128's probe is the same shape one
    partition over: the boot argument that selects the root image, the panic line that carries the
    depth, the container-scheme class name it walks the registry for, both window accessors
    CloverSdhci has to have linked in for the geometry to exist at all, and the two magics it reads
    (the SHNGROOT manifest and the GPT header the window turns out to carry). Round129 adds the
    second half of the same line: the volume class whose media it mounts, the filesystem name handed
    to vfs_rootmountalloc() and the path it walks once the mount is up. Round130 adds the one rung
    that needed a struct field -- the exported vnode_setmountedon() it links against. Round131 adds no
    import and no symbol: it works straight off measured offsets, so the only way to see that it asks
    its question is the flag and vfsops slots on its panic line. Round132 adds neither an import nor an
    offset: it reads four registry strings off the volume node and the container above it, so its
    evidence is the identity keys in the string table and the seven volume fields on the same line.
    Round133's path-open probe is gone: a non-null devvp makes that loader arm unreachable. Round134
    reuses vn_rdwr after a failed mount to read the same devvp, so its identity and dr/db/dx/dy fields
    must be in the line. Round135 adds the two fields that say which layer made that 13: the same read
    again behind IO_NOAUTH (na=) and one raw IOMedia::read of the volume's own media (ir=/ia=) beside its
    three public state answers (mo=/mc=/mw=). Any of them going missing means the round silently asks
    nothing -- the failure shape check-clover.py refuses to let the archives have. Round136 corrects that
    request to min(media size, 512) and adds the media's own geometry (ms=/bs=/mb=), the request it really
    made (ml=/md=), and the media the mount's own dev_t resolves to (xm=/xe=/xs=/xo=/xw=/xr=/xa=/wo=).
    Round137 adds the boot path beside it: `CloverBlock r137: apfs` is the probe's own panic line and
    stays because clover-apfs=2 is still compiled in. Round143 moves the boot line to the two windows:
    the OS volume and the SystemOS cryptex volume are published one after the other, each named by its
    own `CloverBlock r143: os|system vu=%s bsd=%s %u:%u p=%u` line, and `windows os=%u+%u system=%u+%u
    fires=%u` is the one line that proves both windows came from the manifest. The two owner exports
    are what kern_exec.o's offline-owner patch links against; without them the archive member would
    not be pulled in at all and the mount would have no volume to ask for."""
    label = "kernel.macho: CloverBlock linked"
    found = {name: name.encode() in kernel
             for name in ("clover-apfs", "CloverBlock r137: apfs",
                          "CloverBlock r145: os vu=%s bsd=%s %u:%u p=%u",
                          "CloverBlock r145: system vu=%s bsd=%s %u:%u p=%u",
                          "CloverBlock r159: boot os=%u+%u system=%u+%u data=%u+%u fires=%u deferred=1",
                          "CloverBlock r159: data vu=%s bsd=%s %u:%u p=%u",
                          "CloverBlock r159: seed data %s dev=%d bytes=%llu off=%llu",
                          "CloverBlock r159: manifest %s vu=%s off=%llu size=%llu",
                          "CloverBlock r145: owner system %s dev=%d",
                          "fl=0x%x fc=0x%x fk=0x%x", "op=0x%x:0x%x mm=0x%x:0x%x",
                          "vt=%u un=%u vs=0x%x vo=0x%x vu=0x%x vc=0x%x",
                          "dr=%u db=%u dx=0x%x dy=0x%x na=%u ir=0x%x ia=%u "
                          "mo=%u mc=%u mw=%u ms=0x%x:0x%x bs=%u mb=0x%x:0x%x "
                          "ml=%u md=0x%x xm=%u xe=%u xs=0x%x:0x%x xo=%u xw=%u "
                          "xr=0x%x xa=%u wo=%u r=%u w=%u",
                          "FullName", "Sealed",
                          "AppleAPFSContainerScheme", "AppleAPFSVolume",
                          "SystemVersion.plist", "_clover_emmc_block_window",
                          "_clover_emmc_block_userdata_window",
                          # Round316: the eight signature bytes are no longer a searchable string. The
                          # walk moved into BlockCore.h and compares them byte by byte, so the optimizer
                          # turns them into immediates and drops the table -- the decode is in the image,
                          # the ASCII is not. The nine refusal tags replaced it as the marker, because
                          # they are what the walk actually leaves behind and each one is a distinct
                          # finding the frame has to be able to name.
                          "root-magic", "root-count", "system-bounds", "data-geometry",
                          "datawin", "root-code", "EFI PART",
                          "_clover_block_run", "_vnode_setmountedon", "_vn_rdwr",
                          "_native_verified_block_root_authorized",
                          "_native_verified_block_volume_device",
                          "_native_verified_block_manifest_sha384",
                          "_native_ram_block_volume_seed",
                          "_native_ram_block_gpu_addon",
                          "_vfs_context_kernel", "_PE_parse_boot_argn")}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}" for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_owner_mount(kernel: bytes) -> bool:
    """Round143 compiles sheng's offline SystemOS union mount back in. The base tree already carries
    offline-owner-systemos-mount.patch, but scripts/build-xnu-4k-baseline.sh passes
    -DXNU_OWNER_CACHE_MOUNT=0 (and STARTUP/UNION_SYSTEMOS) to kern_exec.o, so the block only exists
    if the board patch won. The panic line is unique to that block and names the failure the round
    is asking about; `commpage_update_dyld_flags` is the call the dyld-flag arm makes, and the
    device-path buffer is the one piece of state the mount hands to the photograph."""
    label = "kernel.macho: owner SystemOS mount compiled in"
    found = {name: name.encode() in kernel
             for name in ("offline owner SystemOS mount failed: %d",
                          "offline scratch Data mount failed: %d",
                          "_commpage_update_dyld_flags",
                          "native_owner_cache_device_path",
                          "native_owner_cache_native_union",
                          "native_ram_block_volume_seed")}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}" for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_owner_root_trust(kernel: bytes) -> bool:
    """Round150 removes `rd=md0s39`, so bsd_init.c's FSIOC_KERNEL_ROOTAUTH block is reached and
    must accept the root on owner grounds instead of skipping the check. The base tree carries
    offline-owner-root-binding-audit.patch, but scripts/build-xnu-4k-baseline.sh leaves
    XNU_OWNER_ROOT_AUDIT/TRUST undefined unless NativeVirtioBlock is on, so only the board patch can
    put the block back. Its evidence is the three globals the audit writes and the checkpoint the
    audited arm calls; all three are referenced by that arm and cannot be dropped by the linker.
    The panic text is asserted too: if the audit were compiled out, the boot would reach it."""
    label = "kernel.macho: owner root trust compiled in"
    found = {name: name.encode() in kernel
             for name in ("native_owner_root_checkpoint",
                          "native_owner_root_accepted",
                          "native_owner_root_selected_device",
                          "rootvp not authenticated after mounting")}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}" for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_mac_probe(kernel: bytes) -> bool:
    """Round141's probe lives in security/mac_vfs.c, which the link can keep without any symbol
    naming it: the hooks still dispatch through the same vtables and only two printf() calls were
    added inside those loops. So the only proof the probe is in this image is its two format
    strings. Each one carries the policy name and the hook's return, which is the whole answer the
    round asks for; losing either means the panic frame would come back with no attribution and the
    round would silently ask nothing."""
    label = "kernel.macho: MAC exec probe linked"
    found = {name: name.encode() in kernel
             for name in ("clover-mac: label-update %s (%s) -> %d disjoint=%d\n",
                          "clover-mac: label-check %s (%s) -> %d\n")}
    ok = all(found.values())
    detail = ", ".join(f"{name.split(':')[1].strip()} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


AMFI_SITES = (
    ("r163 cs_require_lv",
     bytes.fromhex("e00300f940c1fff00014259157000014b80f40f995570094080340b9090113120900092a"),
     0x14000011),
    ("r164 signature",
     bytes.fromhex("ff7f01a9e0630091e3430091e10316aae20315aaa9000094"),
     0xD503201F),
)


def check_amfi_allow(kernel: bytes) -> bool:
    """Rounds163-164 replace the two deny branches of AMFI's exec hook in the prelinked
    kext. Neither branch has a symbol the link keeps, so the only evidence is the
    instruction after each exact signature: the payload image must carry the allow-path
    word, not the stock cbz/tbz. A missing signature means the payload was built from a
    kernel this patcher never touched, which is exactly the r163 frame's kill loop."""
    label = "kernel.macho: AMFI exec hook allows"
    found = {}
    for name, signature, want in AMFI_SITES:
        at = kernel.find(signature)
        unique = at >= 0 and kernel.find(signature, at + 1) < 0
        word = struct.unpack_from("<I", kernel, at + len(signature))[0] if unique else None
        found[name] = unique and word == want
    ok = all(found.values())
    detail = ", ".join(f"{name} {'allow-path' if yes else 'stock'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


# Round164's source concession has no string and no symbol naming it, so the image's evidence is
# the three instruction groups the patch adds: the branch that recognises a 16K-signed blob on a
# 4K kernel, the store that marks it skipped, and the early accept inside cs_validate_hash. Each
# group has to stay unique in the 52 MiB prelink. Losing any one means the payload was built from a
# tree without zzy-clover-dyld-cache-skip.patch, which is exactly the tens-of-minutes preflight walk.
#
# Three of the twenty bytes here are wildcarded, and each for a reason that is not taste: they encode
# an address, not a decision. `b.cond` carries its target as a 19-bit word offset, and `adrp` carries a
# 21-bit page difference -- both change when unrelated code in another file grows and moves the target
# a page. That is what happened in round180: the display archive is ~30 KB of new kernel text, the
# ADRP byte for the cs_validate_hash guard went 0x74 to 0x54, and an exact-byte fingerprint reported a
# missing patch that is in the source and in the image. Every byte that names a register, a struct
# offset or a tested bit stays pinned, so what is being asserted is still the instruction, not an
# address: load w11 from the blob flags, materialise the guard's page (page number free), add #0x120,
# load the counter, and test bit 0 of w11 with the branch that accepts the faulted subrange.
NATIVE4K_SKIP = (
    ("prepare branch", rb"\x1f\x39\x00\x71\x20\x09\x4c\x7a...(\x54)"),
    ("skipped store", rb"\x28\x00\x80\x52\x28\x40\x03\x39"),
    # Round200 grew the text by a page, and the adrp's whole 21-bit page difference moved with it
    # (0xd0ffd154 -> 0xb0ffd154): all four bytes of that word are address, so all four are free.
    ("hash accept", rb"\x0b\x40\x43\x39....\x94\x82\x04\x91\x8a\x02\x40\xb9\xcb\x01\x00\x36"),
)


def check_dyld_cache_skip(kernel: bytes) -> bool:
    """Round164 stops deriving 4K page hashes for the 16K-signed dyld caches. The compiler may
    move the three groups within their functions, and the linker may move their targets between
    pages, but the instructions themselves survive; requiring all three means both halves are
    present -- skipping the derivation without accepting the faulted subranges would reject every
    page instead of accepting it."""
    label = "kernel.macho: dyld cache hash skip"
    found = {name: len(re.findall(pattern, kernel)) == 1 for name, pattern in NATIVE4K_SKIP}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


# Round165's driver change leaves two strings in the kernel: the one-shot A/B result and the line
# that says the batch is not trusted. Round167 keeps the A/B row as evidence and adds the three-way
# agreement. Round168 left the giving-up lines and the long guard wait. Round169 adds the two
# strings that only exist if a failed read stops the card and a blocked read names its stage, so a
# payload built from an older CloverSdhci is caught here rather than in a boot frame. Round171 adds
# the row that says a batch was judged on its word count with a refused stop, which is the shape only
# the new completion criterion can produce.
READ_INTEGRITY = (
    "CloverSdhci r167: readcheck lba=",
    "CloverSdhci r167: single-block reads for this boot",
    "CloverSdhci r167: batch reads trusted for this boot",
    "CloverSdhci r168: block guard waited %llu ms",
    "CloverSdhci r169: card busy %llu ms",
    "CloverSdhci r169: read %s lba=%u blocks=%u done=%u st=%08x",
    "CloverSdhci r171: batch stop refused lba=",
)

# Round167's write half counted every write only after the card said ready. Round168 widened that
# wait and reports the card's own programming time, plus the give-up line; Round169 raises
# settlement to thirty seconds. Round171 changes the other half: the cache is switched *on* at
# bring-up, committed by a flush that switches it off and back (the two lines here are the commit and
# its failure reading), and the block device says which writes it refuses to leave in the cache.
WRITE_INTEGRITY = (
    "CloverSdhci r171: write cache size=",
    "CloverSdhci r171: flush failed flushed=",
    "CloverBlock r171: first commit blocks=",
    "CloverSdhci r167: write not ready lba=",
    "CloverSdhci r169: write settle took %llu ms polls=%u",
    "CloverSdhci r168: write gave up lba=%u blocks=%u done=%u",
)

# Round172's read path is a second transport, not a tweak: ADMA2 descriptors the host fetches over
# the bus, a bounce buffer this driver allocated, and one comparison against the PIO path that
# decides for the whole boot. Each of these lines only exists if that code is compiled in, and the
# first two are the pair the frame needs -- "dma up" says the path came at all, "dmacheck" is the
# verdict. Reads staying on PIO is also a real result, so its line is here too.
DMA_INTEGRITY = (
    "CloverSdhci r172: dma up data=",
    "CloverSdhci r172: dmacheck lba=",
    "CloverSdhci r172: ADMA2 reads trusted for this boot",
    "CloverSdhci r172: reads stay on the PIO path for this boot",
    "CloverSdhci r172: first dma read lba=",
    # Round293 splits the refusal: the same row used to say "deadline" whether the card held the data line past
    # this request's budget or this driver's own spin limit ran out first. Boot 79 refused four reads and one
    # write without naming either, so the row's tail -- exit name plus ms/budget/polls -- is now pinned here too.
    "CloverSdhci r172: dma %s lba=%u blocks=%u st=%08x err8=%02x hctl=%02x ms=%llu budget=%llu polls=%u\n",
)

# Round173 gives the write side the same second transport, and the same kind of verdict line: the gate
# rewrites a run inside logfs with the bytes it read from it and only trusts ADMA when the partition
# reads back as it was found. "wrcheck" is the number; the two verdict lines are both real results, so
# both are required -- a payload that can only print one of them cannot have compiled the gate.
WRITE_DMA_INTEGRITY = (
    "CloverSdhci r173: wrcheck lba=",
    "CloverSdhci r173: ADMA2 writes trusted for this boot",
    "CloverSdhci r173: writes stay on the PIO path for this boot",
    "CloverSdhci r173: first dma write lba=",
    "CloverSdhci r173: dma write %s lba=%u blocks=%u st=%08x err8=%02x hctl=%02x ms=%llu budget=%llu polls=%u\n",
)

# Round166's kernel half prints the first words of every code-signature blob the exec path loads,
# which is how the next frame tells a wrong read from a validation gate. The string is the only
# evidence that the patch reached the prelink.
EXEC_SIG_DUMP = "clover r166: sig n=%u mo=%lld off=%u size=%u w=%08x/%08x/%08x/%08x"


# Round174 moves the reading off the panel: the driver's own verdict lines are written to the card as
# kind 21 records, so a dd of logfs sector 64 upwards answers the round even if the screen scrolled
# twelve lines ago. The armed line is the only string that round added, and it is the one the host
# needs -- a dump with no verdict records has to say whether the ring was ever armed or never flushed.
# Round175 arms a second thing on the same card: the tee that drains the kernel's own log buffer, which
# is how the probes printed by XNU (shared_region, exec/exit, APFS, the loader's signature error) reach
# the dump instead of the panel. Round176 gives that text its own ring -- kind 22 at sector 192, 512
# slots -- because sharing 64 slots with the driver's verdicts meant a busy daemon could evict a reading
# before the card saw it. Each armed line is checked for the same reason a dump needs it: no records in
# a ring has to say whether that ring was ever armed, and after round176 the two answers are separate.
VERDICT_RING = ("CloverSdhci r174: verdict ring armed lba=",
                "CloverSdhci r176: kernel ring armed lba=",
                "CloverSdhci r175: tee armed bufc=")


# Round194 caps the r187/r189 per-call probe lines at 48 in the kernel-ring tee. The r193 dump needed
# that: 2021 of the ring's 512 surviving records were those two kinds, so the ring only covered 101 s of
# a 313 s boot and the part after the boot started working was what got evicted. Round195 showed the
# flood had only moved -- r153, r155 and r170's dyld-cache opens took 491 of 512 -- so each chatty class
# now has its own budget and its own first-drop line. A payload built from a driver without the cap
# still shows every other string this verifier looks for, so the cap gets its own needle; the line
# names the class and the number so a reader of the ring knows what 32 means.
TEE_PROBE_CAP = "CloverSdhci r195: %s probe lines capped at %u"


def check_tee_probe_cap(kernel: bytes) -> bool:
    label = "kernel.macho: tee probe cap linked"
    ok = TEE_PROBE_CAP.encode() in kernel
    print(f"{label:42} {'OK' if ok else 'FAIL'} (probe cap line {'present' if ok else 'ABSENT'})")
    return ok


def check_read_integrity(kernel: bytes) -> bool:
    """Every block read drains the FIFO first and resets the data path on failure; the batch is
    trusted only behind a three-way agreement and is now judged on delivered words rather than an end
    bit this host never raises; every counted write is confirmed ready by the card, the card's write
    cache is on and is committed by an explicit flush; the ADMA2 read path exists as a second
    transport whose own verdict line is compiled in; and the ADMA2 write path exists behind a gate
    that rewrites a logfs run with its own bytes. All facts compile to the strings above;
    requiring them keeps a payload from shipping the old read or write path while the menu still
    claims this round."""
    label = "kernel.macho: eMMC read/write integrity"
    found = {name: name.encode() in kernel
             for name in READ_INTEGRITY + WRITE_INTEGRITY + DMA_INTEGRITY + WRITE_DMA_INTEGRITY}
    ok = all(found.values())
    detail = ", ".join(f"{name.split(':')[1].strip()} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


# Round178 changes who owns the block host, not what crosses it, and that is exactly the kind of change
# a payload can silently drop: the menu would still say r178 while the driver kept the guard for a whole
# request. The one string the round adds names both halves of the reading -- the cuts this request made
# and the waits the boot accumulated -- so its presence in the shipped image is the assertion.
GUARD_HANDOFF = ("CloverSdhci r178: %s handoff lba=%u blocks=%u done=%u cuts=%u waits=%u worst=%llu ms",)


def check_guard_handoff(kernel: bytes) -> bool:
    """Round178 releases the block guard between chunks of one request, so a page fault at the other
    volume is queued behind a chunk rather than behind a 102-second transaction flush. The round's only
    new string is the hand-off report, and it is compiled in on both paths -- a payload without it is
    the round171 driver wearing a round178 label."""
    label = "kernel.macho: block guard hand-off"
    missing = [name for name in GUARD_HANDOFF if name.encode() not in kernel]
    ok = not missing
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({'present' if ok else missing[0]})")
    return ok


def check_verdict_ring(kernel: bytes) -> bool:
    """Round174 writes every CloverSdhci verdict line to logfs as a record of its own, round175 feeds
    the kernel's own log lines in, and round176 gives that second producer its own ring so the first one
    cannot be evicted by it. Without those armed strings in the image, a host-side dd that finds no
    records cannot tell an unarmed ring from a ring that armed and never got a request to finish."""
    label = "kernel.macho: verdict rings and log tee linked"
    found = {name: name.encode() in kernel for name in VERDICT_RING}
    ok = all(found.values())
    detail = ", ".join(f"{name.split(':')[1].strip()} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


# Round180 is the first round whose display stack is real code rather than a stub, and the payload is
# the only place that claim can be checked: the defconfig says the link force-loads
# build/native-legacy-display/libNativeLegacyDisplay.late.a, but a stale archive, a stripped probe or a
# kernel built before the switch flip would all still leave the menu reading "r180". Each string below
# exists in exactly one of the two halves this round adds -- drivers/NativeMobileDisplay (the original
# Legacy framebuffer class, rebuilt against this kernel) and package/CloverLegacyDisplayProbe.cpp (the
# registration and construction probe) -- and none of them appears in the shipped image today, so their
# presence is the evidence that both halves crossed the link. The names are pairs because the reading
# has to be legible in the verifier's own line: a format string is not something to print as a label.
LEGACY_DISPLAY = (("legacy boot args", "offline-legacy-transfer"),
                  ("cpu-present gate", "offline-legacy-cpu-present"),
                  ("gpu hand-off gate", "sheng-fb-gpu"),
                  ("probe round stamp", "clover r180:"),
                  ("console reading", "probe: console %ux%u row=%u depth=%u display=%llu rotate=%u scale=%u base=%llx length=%llx"),
                  ("IOSurface root reading", "late: IOSurface load=%d root found=%u"),
                  # Round202 starts the display, so the four-step line and the driver's own hardware
                  # gate have to be in the image too: without them phase 4 is a boot argument that
                  # selects code the payload does not carry.
                  ("attach reading", "late: dt=%u probe=%u attached=%u started=%u score=%d"),
                  ("gop gate", "offline-legacy-gop"))


# Round181's instrument is the other half of the board's input story, and it is linked the same way
# the two block drivers are -- a plain archive path in CONFIG_XNU_EXTRA_LD_ARCHIVES, kept alive only
# because CloverPMGR calls clover_input_run(). A payload built before the archive was added, or after
# the boot arg was dropped, still prints the current menu title, so the strings below are the
# evidence. Each one is a line the driver can only emit by driving the bus: the divisor word is what it
# writes to I2C_CLK_CTL, the declared-geometry line is the device tree's own numbers, and the two
# candidate readings are the smallest real read each upstream driver asks.
CLOVER_INPUT = (("input round stamp", "clover r181:"),
                ("400 kHz divisor", "bring-up ok fifo out=%u in=%u clk=%08x"),
                ("declared geometry", "declared qup=%llx size=%u dt_size=%u gcc=%llx+%u intid=%u"),
                ("goodix candidate", "goodix gt9xx @5d"),
                ("focaltech candidate", "edt/focaltech @38"),
                ("no-answer reading", "no address ACKed; rails unproven, sda=%08x scl=%08x status=%08x op=%08x"),
                # Round318 turns the masked bus= column's blind spot into a reading: the raw
                # I2C_STATUS and ERROR_FLAGS words are printed once per segment with the layer's
                # decode beside them, and the verdict class the failure branches key on. These two
                # format strings are the whole instrument -- if the rewire dropped a field, the
                # payload would still boot and the row would still print.
                ("r318 raw segment row",
                 "clover r318: xfer %s class=%s status=%08x err=%08x vend=%08x res=%08x "
                 "sda=%u scl=%u op=%08x t=%ums"),
                ("r318 core row",
                 "clover r318: core %s class=%s flags=%08x err=%08x vend=%08x res=%08x "
                 "iovr=%u ouvr=%u iur=%u oovr=%u"))


def check_clover_input(kernel: bytes) -> bool:
    """Round181 puts a QUP v2 I2C instrument in the kernel. It is a plain archive, so nothing about
    the link fails when it is absent -- the bus simply never gets driven, on a board whose only
    output is a reading."""
    label = "kernel.macho: touch instrument linked"
    found = {name: needle.encode() in kernel for name, needle in CLOVER_INPUT}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


# Round182 is linked the same way as round181 -- a plain archive, pulled in only because CloverPMGR
# references clover_power_run() -- and the same silence-on-absence applies. The strings below are the
# lines that can only be emitted by a driver that walked the APID map and put a read command on the
# arbiter: the declared five-window geometry comes out of PowerRegs.h bound to the DTB, the scan line
# is the map walk's summary, the owner line is 排期 4's second gate, and the enable line is the reason
# round181's `rails unproven` was written.
# Round313 rewrites what these strings say. The map-walk line gained `dec=` and `layout=` because the
# word it prints is the one round183 could not settle, and the ownership line's boolean `irq_owner=`
# became `irq_ee=` plus `irq_src=` because the two bands the version word selects derive that answer
# from different places. The new r313 strings are the ones only a band-aware driver can emit: the four
# renderings of the version word, the band the driver chose, the floor plan it printed as arithmetic,
# the owner/ACC word read at its group address and the sentence that says the access words live in a
# different window under this band. A payload carrying round182's stamps but not these would boot
# claiming a floor plan it never selected.
CLOVER_POWER = (("power round stamp", "clover r182:"),
                ("band round stamp", "clover r313:"),
                ("declared windows", "declared core=%llx+%u chnls=%llx+%llu obsrvr=%llx+%llu"),
                ("map walk", "scan %s ppid=0x%03x rc=%d(%s) ver=%08x dec=%u layout=%s words=%u "
                 "mapped=%u span=%u"),
                ("version legs", "arbiter_ver hex=%08x dec=%u nib=%x-%x-%x-%x-%x-%x-%x-%x again=%08x "
                 "stable=%u"),
                ("band verdict", "band=%s floor=%08x next=%08x"),
                ("positive control", "main_apids=%u (mask %x) differs=%u"),
                ("floor plan", "layout=%s map=0x%x+%u x%u entries=%u obs=ee*0x%x+apid*0x%x+%u"),
                ("ownership gate", "write-owner is EE %u, this EE is %u: reads ok, writes denied"),
                ("irq provenance", "apid=%d owner=%u irq_ee=%u irq_src=%s irq_apid=%d"),
                ("group indexing", "apid=%u group=%u intr+0x%x owner_acc=%08x"),
                ("window honesty", "acc words are %s offsets on %s; no chnls page mapped"),
                ("identity refusal", "type is not PMIC_TYPE_VALUE; identity NOT established"),
                ("burst question", "burst no, singles yes: sequential reads unproven on this arbiter"),
                ("enable reading", "enable=%02x state=%s follow=%02x"))


def check_clover_power(kernel: bytes) -> bool:
    """Round182's arbiter report is the only answer this board has to "which EE owns the rail the
    touch panel needs", so a payload without the archive boots claiming a power round it never runs."""
    label = "kernel.macho: pmic arbiter instrument linked"
    found = {name: needle.encode() in kernel for name, needle in CLOVER_POWER}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


# Round183 is linked the same plain-archive way as the two rounds above, and its strings are the ones
# only a walker over a real SMEM heap can emit: the header line is the four words whose agreement makes
# every later offset believable, the ptable line is the `$TOC` magic read as one little-endian word (the
# byte-order form of that same magic is the bug the host model exists to catch), the part line is a table
# entry confirmed against the partition header it points at, the item line is an SMP2P candidate judged
# by "$SMP", version, sixteen entries and both pids, and the image line is the presence claim 排期 5 is
# chosen from. The verdict line is the last one because it is the reading this round exists to produce.
CLOVER_DSP = (("dsp round stamp", "clover r183:"),
              ("heap header", "smem initialized=%08x free=%u available=%u reserved=%08x sbl=%08x pc=%08x/%08x"),
              ("global toc walk", "global toc scanned=%u allocated=%u highest=%u"),
              ("partition table", "ptable at %08x magic=$TOC version=%u entries=%u info=%08x num_items=%u capacity=%u"),
              ("partition row check", "part[%u] hosts=%u:%u off=%u size=%u cacheline=%u hdr=%08x ok=%d hosts_ok=%d rc=%d (%s)"),
              ("doorbell word", "doorbell %llx+04 = %08x"),
              ("smp2p identity", "item=%u path=%s data=%llx phys=%llx size=%u hdr magic=%08x ver=%u pids=%u:%u total=%u valid=%u flags=%08x"),
              ("image presence", "image %s %llx+%u pages=%u nonzero=%u first=%u elf_at=%s head0=%08x"),
              ("the fork", "verdict adsp_edge=%s adsp_loaded=%s (mode 3 done)"))


def check_clover_dsp(kernel: bytes) -> bool:
    """Round183 answers whether 排期 5 is a protocol job or a bring-up job, and only the archive's own
    strings prove the walk crossed the link: a payload built without it boots the r183 menu and reads
    nothing, which on this round looks exactly like an empty heap."""
    label = "kernel.macho: smem and adsp instrument linked"
    found = {name: needle.encode() in kernel for name, needle in CLOVER_DSP}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


# Round184 puts the deadline back, and a deadline is only worth having if what happened before it
# survives the reset it causes. The panel cannot be the reading any more: round183's photograph has the
# kernel's own version banner on its first row and the DSP instrument's `part(3)` on its last, so the
# screen filled and every line after it was drawn nowhere. The two RAM rings are memory-only until a
# driver tail flushes them, and a boot that panics never reaches another tail. So the panic sink drains
# them, and the drain's own line is the proof it is in the image -- a payload without it boots the r184
# menu, panics, resets and leaves a card that says nothing about the panic it just had.
CLOVER_PANIC_DRAIN = (("panic drain line", "CloverSdhci r184: panic drain wrote=%u dropped=%u "
                                           "tee_passes=%u\n"),)


def check_panic_drain(kernel: bytes) -> bool:
    """Round184's reading is the card, and the card only gets the reading if the sink drains the
    rings; this string is the one thing in the image that says the drain was linked."""
    label = "kernel.macho: panic ring drain linked"
    found = {name: needle.encode() in kernel for name, needle in CLOVER_PANIC_DRAIN}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


# Round185 arms the rings from the GPT entry walk. The arming round174..176 relied on sat inside the
# mode-20 report block while the shipped boot has been mode 21 since round137, and it also waited for a
# record read back out of a partition that had never been written -- so a normal boot could not arm
# anything, which is the plainest reading of the round184 card coming back byte-identical to the one
# before it. These two lines are the image's own statement of which happened, armed or refused with the
# entry it looked for, and they are what lets an empty dump still be read as a finding.
CLOVER_LOG_WINDOW = (("log window armed", "CloverSdhci r185: log window armed from GPT first=%u "
                                          "sectors=%u report=%u driver=%u kernel=%u\n"),
                     ("log window refusal", "CloverSdhci r185: no logdump entry at %u..%u found=%u "
                                            "first=%u last=%u, rings stay disarmed\n"))


def check_log_window(kernel: bytes) -> bool:
    """Round185's reading only exists if a mode-21 boot armed the rings; this is the string that says
    the arming moved into the entry walk and was linked with it."""
    label = "kernel.macho: logdump rings armed from GPT"
    found = {name: needle.encode() in kernel for name, needle in CLOVER_LOG_WINDOW}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_spawn_why(kernel: bytes) -> bool:
    """Round186 asks which path launchd's posix_spawn() failed to resolve. The image has to carry both
    ENOENT producers' lines: the program-path walk (with the component that stopped, the mount behind
    the deepest prefix that resolved, and whether a retry fails the same way) and the dylinker lookup.
    A payload with only one of them cannot tell "the executable is not there" from "dyld is not
    there" -- and launchd panics on either with the same five words."""
    label = "kernel.macho: spawn ENOENT probe linked"
    found = {"spawn-why": (b"clover r186: spawn-why err=%d n=%d path=%s stop=%s good=%s "
                           b"fs=%s on=%s mflags=%lx retry=%d\n") in kernel,
             "loader-why": b"clover r186: loader-why n=%d err=%d path=%s pid=%d\n" in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_sys_why(kernel: bytes) -> bool:
    """Round187 asks where usermanagerd and keybagd stop inside the kernel. The image has to carry the
    entry line for each half of the syscall surface -- mach traps, which sleh.c routes before BSD ever
    sees them, and unix calls -- plus the failure line that names the errno a call left behind. All
    three are filtered to the two boot-task names, so a missing one means a whole half of the surface
    would be silent on the next card read."""
    label = "kernel.macho: syscall-in probe linked"
    found = {"unix-in": (b"clover r187: unix-in n=%d pid=%d tid=%lld comm=%s code=%d a0=%lx a1=%lx\n")
             in kernel,
             "unix-fail": (b"clover r187: unix-fail n=%d pid=%d tid=%lld comm=%s code=%d err=%d "
                           b"a0=%lx\n") in kernel,
             "mach-in": (b"clover r187: mach-in n=%d pid=%d tid=%lld comm=%s trap=%d a0=%lx a1=%lx\n")
             in kernel,
             "comm-filter": b"usermanagerd" in kernel and b"keybagd" in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_sys_why2(kernel: bytes) -> bool:
    """Round188 reads what the two stuck tasks were carrying. r187's frame ends keybagd with a write to
    the console, a close and a denied reboot(0) -- its own fatal path, whose text names the failure --
    and usermanagerd with write_nocancel(1, ...) and nothing after it. So the image has to carry the
    buffer text and the return of every write, and the path behind every failing open/stat/access; a
    missing string means the next card read would again show the calls without their contents."""
    label = "kernel.macho: write/path contents probe linked"
    found = {"unix-write": (b"clover r188: unix-write n=%d pid=%d tid=%lld comm=%s code=%d fd=%d "
                            b"len=%lu txt=%s\n") in kernel,
             "unix-path": (b"clover r188: unix-path n=%d pid=%d tid=%lld comm=%s code=%d err=%d "
                           b"path=%s\n") in kernel,
             "unix-wrote": (b"clover r188: unix-wrote n=%d pid=%d tid=%lld comm=%s code=%d fd=%d "
                            b"ret=%lld err=%d\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_sys_why3(kernel: bytes) -> bool:
    """Round189 closes the two defects the r188 frame exposed. Every open_dprotected_np path came out
    empty because the probe read x[1] -- that call's flags, 0x701 -- where the path is x[0], and the
    entry budget ended at 67.190 s, which left the 13 s before usermanagerd's last write unread. The
    image has to carry the successful-open line with its returned fd, the mach-trap return line, and
    the wider budgets behind them; a missing string means the next card read would again show calls
    without their targets or their returns."""
    label = "kernel.macho: open/mach-return probe linked"
    found = {"unix-open": (b"clover r189: unix-open n=%d pid=%d tid=%lld comm=%s code=%d fd=%lld "
                           b"path=%s\n") in kernel,
             "mach-out": (b"clover r189: mach-out n=%d pid=%d tid=%lld comm=%s trap=%d ret=%d\n")
             in kernel,
             "kept-r188": b"clover r188: unix-path n=%d pid=%d tid=%lld comm=%s code=%d err=%d "
                          b"path=%s\n" in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_sys_why4(kernel: bytes) -> bool:
    """Round190 brackets the call the r189 frame ends on. fcntl(3, F_BARRIERFSYNC) enters at
    112.438859 and the ring says nothing for the next 187 s; an entry line alone cannot say whether
    the call returned, so the image has to carry the exit line for the handful of storage calls, and
    the driver's cache commit has to carry its own bracket -- written to the card before the first
    CMD6, so a commit that hangs still leaves the fact that it started."""
    label = "kernel.macho: barrier bracket linked"
    found = {"unix-out": (b"clover r190: unix-out n=%d pid=%d tid=%lld comm=%s code=%d ret=%lld "
                          b"err=%d\n") in kernel,
             "flush-in": (b"CloverSdhci r190: flush-in flushes=%u on=%u size=%u wait=%llu ms\n")
             in kernel,
             "flush-out": (b"CloverSdhci r190: flush-out flushed=%d back=%d rsp=%08x st=%08x "
                           b"polls=%u rsp2=%08x st2=%08x polls2=%u ms=%llu flushes=%u\n") in kernel,
             "sync-in": b"CloverBlock r190: sync-in pending=%llu\n" in kernel,
             "kept-r189": b"clover r189: unix-open n=%d pid=%d tid=%lld comm=%s code=%d fd=%lld "
                          b"path=%s\n" in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_flush_stages(kernel: bytes) -> bool:
    """Round191 splits the r190 bracket. The frame stops at flush-in, so no step of the commit has
    ever produced a reading: five stage records, each drained to the card before the step that
    follows it, are what let a frame name the step that hung. The r190 pair stays -- it is the outer
    bracket this round subdivides."""
    label = "kernel.macho: cache commit stages linked"
    found = {name: needle in kernel for name, needle in (
        ("flush-go", b"CloverSdhci r191: flush-go cf=%u bound=%u ps=%08x inh=%u\n"),
        ("flush-c1", b"CloverSdhci r191: flush-c1 ok=%d st=%08x rsp=%08x spins=%u ms=%llu\n"),
        ("flush-w1", b"CloverSdhci r191: flush-w1 ready=%d to=%d busy=%llu ms polls=%u rsp=%08x "
                     b"st=%08x smp=%u mk=%u ps=%08x ms=%llu\n"),
        ("flush-c2", b"CloverSdhci r191: flush-c2 ok=%d st=%08x rsp=%08x spins=%u ms=%llu\n"),
        ("flush-w2", b"CloverSdhci r191: flush-w2 ready=%d to=%d busy=%llu ms polls=%u rsp=%08x "
                     b"st=%08x smp=%u mk=%u ps=%08x ms=%llu\n"),
        ("kept-r190", b"CloverSdhci r190: flush-in flushes=%u on=%u size=%u wait=%llu ms\n"))}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_cache_ownership(kernel: bytes) -> bool:
    """Round192 hands the write cache back to the card. The r191 frame's last record is flush-go and
    nothing follows it, including the r184 panic drain: every in-commit record leaves through a block
    write, and a card holding DAT0 for the commit refuses those too. So the image has to carry the
    line that says the commit was skipped on purpose, and it has to keep the r171 line -- that one is
    what reports ctrl=0/on=0 in the next frame."""
    label = "kernel.macho: cache ownership reading linked"
    found = {"skipped": (b"CloverSdhci r192: commit skipped size=%u on=%u (once)\n") in kernel,
             "kept-r171": (b"CloverSdhci r171: write cache size=%u ctrl=%u on=%d rsp=%08x st=%08x "
                           b"cf=%u\n") in kernel,
             "kept-r191": b"CloverSdhci r191: flush-go cf=%u bound=%u ps=%08x inh=%u\n" in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_critical_exit_probe(kernel: bytes) -> bool:
    """Round193 reads why a crash became critical. The r192 frame ends on the third watchdogd
    SIGTRAP and the panic behind it, so the image has to carry the line that names the
    crash-behavior bits and deadline of every critical exit, and it has to keep the exit line
    that pairs the death with its pid and wait status in the same frame -- round309 renamed that
    line's token when it fixed the seconds column, so the needle tracks the token, not the round
    that first wrote it. Round211
    adds the kernel side of the same kill: the sender's own backtrace and, for every receiving
    thread, the saved user lr/sp plus the kernel frames behind it."""
    label = "kernel.macho: critical exit reading linked"
    found = {"crit": (b"clover r193: crit pid=%d comm=%s rv=0x%x sig=%d fatal=%d sigcrash=%d "
                      b"crashbits=0x%x spawnfail=%d deadline=%llu now=%llu\n") in kernel,
             "killsig": (b"clover r207: killsig pid=%d comm=%s ppid=%d sig=%d spid=%d scomm=%s "
                         b"ns=%d code=0x%llx cs=0x%x ra=%016llx anchor=%016llx\n") in kernel,
             "killupc": (b"clover r208: sender upc=%016llx uinsn=%08x spid=%d scomm=%s\n")
                        in kernel,
             "skst": (b"clover r211: skst n=%u f0=%016llx f1=%016llx f2=%016llx f3=%016llx "
                      b"f4=%016llx f5=%016llx\n") in kernel,
             "sbthr": (b"clover r211: sbthr n=%u pid=%d mt=%d tid=0x%llx pc=%016llx lr=%016llx "
                       b"sp=%016llx cpsr=%08x state=%d kin=%u wait=%016llx kstk=%016llx\n")
                      in kernel,
             "sbthk": (b"clover r211: sbthk n=%u ktop=%016llx kfp=%016llx klr=%016llx "
                       b"kf=%016llx,%016llx,%016llx,%016llx,%016llx,%016llx\n") in kernel,
             "kept-exit": (b"clover r309: exit pid=%d ppid=%d t=%llus rv=0x%x ns=%d code=0x%llx "
                           b"comm=%s\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_panic_tail_read(kernel: bytes) -> bool:
    """Round209 reads the log buffer from the panic sink. The r208 frame's kernel ring ends at
    167.198 s and gains nothing before the deadline, because the tee only runs on block I/O tails;
    the sink is the one writer left, so the image has to carry the backwards scan and its status
    row -- `unseen` is what separates a stopped kernel from a stopped card path. It has to keep the
    r184 drain line, which is the record that the drain ran at all. Round211 adds `append_rc`: the
    status row is the only place where the sink's own verdict on the panic sentence survives.
    Round212 adds the two halves that make a silent drain attributable: the enter/exit pair and the
    boot id in every row, plus the probe ring the kernel's kill-point lines are relayed into.
    Round301 adds the two rows that make a refused append still readable: the sink's sentence, said
    from the RAM copy in parts, and the status line that says why there are no parts."""
    label = "kernel.macho: panic-time log tail linked"
    found = {"tail": (b"CloverSdhci r212: panic tail lines=%u unseen=%u bufx=%d tee=%u boot=%u "
                      b"cursor=%u size=%u lap=%u refused=%u pend=%u drop=%u guard=%u last=%ums "
                      b"cache=%u append_rc=%u\n") in kernel,
             "refused": (b"CloverSdhci r212: panic tail refused reason=%u magic=0x%x size=%d "
                         b"bufx=%d tee=%u boot=%u\n") in kernel,
             "enter": (b"CloverSdhci r212: drain enter boot=%u first=%u panic_first=%u "
                       b"verdict=%u tee=%u append_rc=%u probe_seen=%u\n") in kernel,
             # Round308 moves this row's token and splits its last column in two. The number that used to
             # sit under probe_pend= is the driver tee ring's backlog, so the relay's own queue -- sealed
             # minus consumed, the pair the flush reports beside a write -- had no column at all, and boot
             # 97 read that 0 as a relay with nothing pending.
             "exit": (b"CloverSdhci r308: drain exit boot=%u wrote=%u dropped=%u tee_passes=%u "
                      b"probe=%u probe_drop=%u ring_pend=%u probe_pend=%u\n") in kernel,
             "exit-generation": (b"CloverSdhci r212: drain exit") not in kernel,
             "kept-r184": (b"CloverSdhci r184: panic drain wrote=%u dropped=%u tee_passes=%u\n")
                          in kernel,
             # Round301's whole point is that the sentence survives a refused report append, and a
             # driver that lost the copy would still boot, still drain, and still print every r212
             # row above -- so both strings are pinned here, complete, for the standing reason: a cut
             # format string leaves every prefix needle green.
             "panic-text": (b"CloverSdhci r301: panic text rc=%u chars=%u part=%u %s\n") in kernel,
             "panic-copy": (b"CloverSdhci r301: panic copy rc=%u chars=%u said=%u\n") in kernel,
             # round302: the driver's own account of the blind sentence write. The record itself is
             # binary words, so this row is the only thing in the image that names the attempt.
             "panic-blind-row": (b"CloverSdhci r302: blind ok=%u chars=%u total=%u slot=%u\n") in kernel,
             # round309: the drain's own entry counter, said on the driver's tee path. The binary drain-in
             # sector is written from the first statement of the sink's drain, before its gates -- so a boot
             # with no sector and no row here is a boot whose sink never reached the drain, and a boot with
             # the row and no sector is the sink refusing its own write. Either leg alone would be a hole.
             "drain-legs": (b"CloverSdhci r309: drain legs in=%u boot=%u host=%x probe=%u\n") in kernel,
             # round310: what the sink found the card doing at its own door, before it asked the card for
             # anything. Boot 102's four post-marker absences could only be read as "the card refused", and
             # the word that would have settled it -- the stage and the live ticket the sink walked in on --
             # was in no record. This row, and the same two words inside the marker, are that reading.
             "quiesce-row": (b"CloverSdhci r310: quiesce %s stage=%u inhibit=%x owner=%x ok=%u "
                             b"probe=%u/%u\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_boot_record(kernel: bytes) -> bool:
    """Round212's counter is the one reading that survives a card carrying two boots: every
    two-block record names its boot, so a drain line in a reused slot can be attributed."""
    label = "kernel.macho: r212 boot counter armed"
    found = {"boot-id": (b"CloverSdhci r212: boot id=%u record=%u probe slots=%u text=%u\n")
                        in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_host_ticket(kernel: bytes) -> bool:
    """Round213 replaces the IOLock with a ticket word. The image has to carry the arming line,
    the bounded waiter's report, and the watchdog's own report: without all three a dump cannot
    tell 'the guard was never armed', 'somebody waited out the bound' and 'the beat recovered a
    stuck holder' apart, and those are the three outcomes this round is decided by."""
    label = "kernel.macho: r213 host ticket armed"
    found = {"armed": (b"CloverSdhci r213: host ticket armed, wait=%llums stuck=%llums\n")
                      in kernel,
             "waited": (b"CloverSdhci r213: guard wait gave up after %llu ms owner=%u stage=%u\n")
                       in kernel,
             "stuck": (b"CloverSdhci r213: guard stuck stage=%u dir=%u lba=%u blocks=%u pid=%u "
                       b"held=%llu ms\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_card_reinit(kernel: bytes) -> bool:
    """Round214 leaves the panic path's trip counters in misc's second block. Round215 moved the
    report line into the request-context recovery (check_card_recover), so all that remains here is
    the marker: without it a dump cannot say whether the card ever failed, only that it stopped."""
    label = "kernel.macho: r214 trip counter marker"
    found = {"marker": b"clover-stuck=" in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_card_recover(kernel: bytes) -> bool:
    """Round215 lets the request bring the card back itself instead of waiting for the watchdog.
    The image has to carry the ladder's report line and the silence verdict that arms it: r214's
    frame ended at 408 s with the card quiet and nothing left to say whether the request gave up
    on a busy card or on one that had stopped answering."""
    label = "kernel.macho: r215 card recover armed"
    found = {"line": (b"CloverSdhci r215: card recover why=%s d=%u n=%u stop=%u rst=%u pwr=%u "
                      b"clk=%u clkst=%u t=%u cmd0=%u cmd1=%u ready=%u cmd2=%u cmd3=%u cmd7=%u "
                      b"hs=%u bw=%u ok=%u\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def _m68_personality(kernel: bytes) -> bool:
    """True when the shipped prelink carries AppleM68Buttons' own personality dict.

    A window is taken around each occurrence of the bundle identifier because the plist is stored
    as one blob: the identifier, IONameMatch=buttons and IOProviderClass=IOPlatformDevice are
    kilobytes apart, not adjacent, and the first probe of this contract got that wrong and read a
    linked-in kext as a stripped personality."""
    needle = b"com.apple.driver.AppleM68Buttons"
    window = 4096
    at = kernel.find(needle)
    while at != -1:
        chunk = kernel[max(0, at - window):at + window]
        if (b"IOKitPersonalities" in chunk and b"IONameMatch" in chunk
                and b"IOProviderClass" in chunk and b"IOPlatformDevice" in chunk):
            return True
        at = kernel.find(needle, at + 1)
    return False
def check_sb_sample(kernel: bytes) -> bool:
    """Round217 samples the living process instead of reading the corpse. The image has to carry
    the try-locked walk and the sampler line: r216's frame showed the kill-time walk waiting 39 s
    on the dying task's lock and then reading a state that had already been released, so the next
    frame is decided by lines printed before the kill."""
    label = "kernel.macho: r217 sb sample linked"
    found = {"sbs": (b"clover r217: sbs t=%llu pid=%d n=%u c=%u%s%s\n") in kernel,
             "busy": (b"clover r217: sbthr busy=1 pid=%d budget=%u\n") in kernel,
             "killsrc": (b"clover r218: killsrc upc=%016llx ulr=%016llx code=") in kernel,
             "killustk": (b"clover r218: killustk n=%u sp=%016llx fp=%016llx") in kernel,
             "sbpc": (b"clover r218: sbpc t=%llu tid=0x%llx x16=%llx n=%u") in kernel,
             "sbm2": (b"clover r221: sbm2 t=%llu tid=0x%llx op=%llx b=%x sz=%x rem=%x loc=%x") in kernel,
             "sbm2x": (b"clover r221: sbm2x t=%llu tid=0x%llx pc=%llx") in kernel,
             "sbmsg": (b"clover r221: sbmsg t=%llu tid=0x%llx") in kernel,
             "sbuf": (b"clover r222: sbuf t=%llu") in kernel,
             "sbhex": (b"clover r222: sbhex t=%llu") in kernel,
             "sbport": (b"clover %s: sbport t=%llu %s nm=%x o=%llx cnt=%u ql=%u seq=%u") in kernel,
             "peer": (b"clover r224: peer t=%llu pid=%d n=%u sc=%d act=%d b=%u") in kernel,
             "p1": (b"clover r225: p1s t=%llu pid=%d n=%u c=%u%s%s\n") in kernel,
             "p1m2": (b"clover r225: p1m2 t=%llu tid=0x%llx op=%llx b=%x sz=%x") in kernel,
             "p1uf": (b"clover r225: p1uf t=%llu tid=0x%llx") in kernel,
             "p1hex": (b"clover r225: p1hex t=%llu tid=0x%llx") in kernel,
             "kreg": (b"clover r226: kreg%u %016llx.%016llx.%016llx.%016llx.") in kernel,
             "kreg4": (b"clover r226: kreg4 fp=%016llx lr=%016llx sp=%016llx pc=%016llx") in kernel,
             "kraw": (b"clover r226: kraw%u %016llx.%016llx.%016llx.%016llx.") in kernel,
             "sbgone": (b"clover r226: sbgone t=%llu pid=%d\n") in kernel,
             "rcvsum": (b"clover r228: rcvsum t=%llu n=%u tot=%u\n") in kernel,
             "rcv": (b"clover r228: rcv%u t=%llu pid=%u gen=%u tid=%llx k=%u p=%llx o=%llx") in kernel,
             "rcvb": (b"clover r228: rcvb%u n=%u") in kernel,
             "rcdump": (b"clover r228: rcdump t=%llu n=%u tot=%u\n") in kernel,
             "rce": (b"clover r228: rce t=%llu pid=%u gen=%u tid=%llx k=%u") in kernel,
             "sbtask": (b"clover r228: sbtask gen=%u pid=%d t=%llu\n") in kernel,
             "bb": (b"clover r229: bb t=%llu pid=%d n=%u sc=%d act=%d f=%u s=%u hops=%u"
                    b" wait=%u d=%llu%s\n") in kernel,
             "bbgone": (b"clover r229: bb t=%llu pid=%d f=%u gone\n") in kernel,
             # Round264 renamed two fields of this row: x6 is rcv_size, not a send right, and x3's low
             # half is the remote port, not a receive timeout.
             "bbhop": (b"clover r231: bbhop t=%llu pid=%d tid=0x%llx op=%llx rcv=%x"
                       b" sz=%x remote=%x local=%x id=%x to=%llx rno=%s lo=%s\n") in kernel,
             "bbwait": (b"clover r231: bbwait t=%llu pid=%d tid=0x%llx st=%x"
                        b" cont=%llx pc=%llx x0=%llx x1=%llx x2=%llx x3=%llx"
                        b" u=%u run=0x%llx\n") in kernel,
             "bbset": (b"clover r234: bbset%s t=%llu %s rn=%s nm=%x o=%llx n=%u"
                       b" all=%u m=%s\n") in kernel,
             "sbown": (b"clover r234: sbown t=%llu %s nm=%x own=%s\n") in kernel,
             "bbhopo": (b"clover r234: bbhopo t=%llu pid=%d tid=0x%llx rem=%x ro=%s\n") in kernel,
             "beat": (b"clover r234: beat t=%llu pid=%d rn=%x rep=%x\n") in kernel,
             "bblock": (b"clover r234: bblock t=%llu tid=0x%llx op=%llx a=%llx st=%x"
                        b" w=%08x own=%x ow=0x%llx ost=%x\n") in kernel,
             "bblock-err": (b"clover r234: bblock t=%llu tid=0x%llx op=%llx a=%llx"
                            b" st=%x rd=err\n") in kernel,
             "bbustk": (b"clover r236: bbustk t=%llu pid=%d tid=0x%llx sp=%llx"
                        b" fp=%llx lr=%llx fn=%u%s\n") in kernel,
             "ulock": (b"clover r238: ulock t=%llu pid=%d a=%llx w=%u b=%u k=%u"
                       b" s=%u wr=%d sw=%d op=%x st=%llu age=%llu cw=%x\n") in kernel,
             "ulockc": (b"clover r238: ulockc t=%llu seen=%u filt=%u stored=%u"
                        b" over=%u open=%u\n") in kernel,
             "bbobj": (b"clover r237: bbobj t=%llu pid=%d"
                       b" tid=0x%llx a=%llx base=%llx isa=%llx"
                       b" val=%llx w=%x\n") in kernel,
             "bbgrp": (b"clover r239: bbgrp t=%llu pid=%d tid=0x%llx base=%llx"
                       b" b=%x g=%x cnt=%u wait=%d notif=%d nh=%llx nt=%llx\n") in kernel,
             "bbchain": (b"clover r239: bbchain t=%llu pid=%d tid=0x%llx fi=%u fn=%u"
                         b" blk=%llx inv=%llx isa=%llx ctl=%llx sisa=%llx def=%llx"
                         b" disa=%llx grp=%llx\n") in kernel,
             "bbnochain": (b"clover r239: bbnochain t=%llu pid=%d tid=0x%llx"
                           b" base=%llx fn=%u\n") in kernel,
             "census": (b"clover r243: census t=%llu\n") in kernel,
             "census-nodes": (b"clover r243: node %s\n") in kernel,
             "census-hbt": (b"clover r243: hbt len=%u u32=%u\n") in kernel,
             "census-buttons": (b"clover r243: path buttons=%s\n") in kernel,
             "census-model": (b"clover r243: model=%.16s\n") in kernel,
             "census-svc": (b"clover r243: svc total=%d stopped=%d button-family=%d\n") in kernel,
             "census-plane": (b"clover r243: plane byname=%s buttons byname=%s\n") in kernel,
             # Round250. The census proves the kernel published the right answer; these two prove the
             # image can say whether userland ever asked for it. Both branches have to be in the
             # binary: the row that names a caller, and the verdict that no caller existed.
             "mgq-asks": (b"clover r252: asks path=%llu key=%llu notes=%u whos=%u\n") in kernel,
             "mgq-none": (b"clover r252: no user client ever named a button path or key\n") in kernel,
             "mgq-row": (b"clover r252: row %u/%u %s\n") in kernel,
             "mgq-who": (b"clover r252: who pid=%d comm=%.20s path=%u key=%u\n") in kernel,
             # Round254 reads the same three probes backwards, and keeps them because they are the
             # proof of the round that removed the root. Round253 linked Apple's button driver, the
             # personality matched /J120AP/buttons, and handleStart panicked
             # "failed to instantiate function-button_hold" at t=12.744 s, ending the boot before any
             # instrument ran. The driver builds function-button_<name> for every name in its own
             # table, and a synthesized ADT node has none of them, so the class, the format string and
             # the personality must all be OUT of the shipped image: their presence here would mean a
             # board that dies at thirteen seconds instead of the one that reaches the desktop.
             "m68-class": b"AppleM68Buttons" not in kernel,
             "m68-name": b"function-button_%s" not in kernel,
             # The personality is checked in its absence for the same reason it was checked in its
             # presence: it is the only thing that makes IOPlatformExpert attach a driver to the node
             # we publish, so if the dict is back in the image the boot is back at thirteen seconds.
             "m68-personality": not _m68_personality(kernel),
             # Round254's miss list is one row per distinct name with a count, and the live line is
             # said once per name. All three have to be in the binary: without the first the frame
             # cannot say how many absences there were, without the second the names are unreadable,
             # and without the third there is no record that the tally was ever taken at all.
             "mgq-miss": (b"clover r252: misses total=%u distinct=%u\n") in kernel,
             "mgq-missrow": (b"clover r252: miss %u/%u t=%llus %c pid=%d comm=%.20s count=%u "
                             b"missing=%s\n") in kernel,
             "mgq-missnew": (b"clover r252: miss new t=%llu %c pid=%d comm=%.20s missing=%s\n") in kernel,
             # The sixteen-slot ring this round replaces must not survive in the image, or the frame
             # would report a shown count out of a table that no longer exists.
             "mgq-miss-old": (b"clover r252: misses total=%u shown=%u\n") not in kernel,
             # Round255. The owned input device is only in this image if the archive the kernel
             # force-loads carries it, and the only proof from the file is the strings its two
             # objects print: the probe's stage verdict and refusals, and the product strings the
             # driver publishes through the original IOHIDDevice slots it overrides.
             "hid-stage": (b"clover r255: stage=%d load=%d matching=%d meta=%d resource=%u "
                           b"sizes=%u,%u,%u,%u,%u device=%llx transfer=%u presses=%u sinks=%u,%u\n") in kernel,
             "hid-size": (b"clover r255: IOHIDDevice size=%u not 208, refusing\n") in kernel,
             "hid-load": (b"clover r255: IOHIDFamily load=%d, no device\n") in kernel,
             # Round256's beat: the early lines are the ones the ring loses (boot 43 printed its verdict
             # at t=25 s and kept rows from t=1208 s), so the state is said again every 120 s of the ARM
             # counter, and the gate is a clock rather than a call count because the driver calls the
             # checkpoint ten times a second while a press is held.
             "hid-beat": (b"clover r255: beat stage=%d service=%llx%s published=%u seen=%u presses=%u "
                          b"asks=%u touch=%u reports=%u sinks=%u,%u transfer=%u\n") in kernel,
             # Round259: the beat now says whether the device put a finger in the collection it
             # published, and the bus instrument that could say whether a real one exists is back in
             # the same image -- both are only true of this payload if these bytes are in it.
             "hid-touch-line": (b"touch=%u reports=%u ") in kernel,
             # Round260: the beat carries the touch bus verdict as state, because boot 47 proved the
             # burst it replaced never reaches the card (spoken t=12 s, ring window from t=1194 s).
             "hid-bus-row": (b"clover r260: bus mode=%llu map=%llx hw=%llx io=%llx op=%llx err=%llx "
                             b"status=%llx state=%llx clk=%llx ack=%llx id=%llx,%llx xfers=%llu "
                             b"timeout=%llu nack=%llu verdict=%lld\n") in kernel,
             # Round319: the beat also re-says the r318 raw words the r260 line's 16 slots do not carry
             # -- raw I2C_STATUS / ERROR_FLAGS and SDA/SCL's own bits -- from clover_input_last_xfer,
             # because those rows print once at driver start and the tee's ring laps over them before
             # readback. The literal is pinned whole: a format string cut in half leaves prefix needles
             # green while the row on the card says nothing.
             "hid-r319-row": (b"clover r319: last status=%llx err=%llx sda=%llu scl=%llu vend=%llx "
                              b"res=%llx\n") in kernel,
             # Round262: the unfiltered thread census. r261's host forensics named the wait -- backboardd
             # asleep on a dispatch group inside a call into libVFXCore -- but the sampler's wait rows are
             # filtered to threads that are waiting AND not in the trap, so they cannot show a second
             # sleeper on the same word or an idle worker that never got the block. Both literals are
             # pinned whole, because a format string cut in half still leaves every prefix needle green.
             # Round262's census row, then Round263's wider one: the deadline separates an untimed idle
             # wait from a timed one, and the send right's owner names the service an answer is owed by --
             # the four-slot hop capture was a cap, not a count, so this is where that stops. Both
             # literals are pinned whole (r252's lesson: an eaten format string leaves prefix needles green).
             # Round264: the census row re-decoded the way mach_msg2_trap unpacks its own arguments
             # (remote port in x3's low half, x6 is rcv_size), the remote's owner as peer=, and the
             # receive port's queue depth and send-right count -- plus the hop row, whose two labels
             # this round retires because they called a port a timeout and a size a port name.
             "bb-census-row": (b"clover r262: census t=%llu pid=%d threads=%u shown=%u"
                               b" hidden=%u wait=%u run=%u rx=%u tm=%u\n") in kernel,
             "bb-census-th": (b"clover r262: th tid=%llx st=%x we=%llx cont=%llx"
                              b" pc=%llx dl=%llx m2=%llx rcv=%x remote=%x local=%x to=%llx"
                              b" sz=%x mc=%u sr=%x rmc=%u rsr=%x peer=%s\n") in kernel,
             # Round265: boot 51 resolved the one outstanding send right to pid 140, so the shipped kernel
             # has to walk that task as well (a header per fire plus its rows) and must not lose r264's row
             # in the process. A payload without the peer rows names a debtor and then reads nothing about
             # it, which is the reading this round exists to get.
             "bb-peer-header": (b"clover r265: peer t=%llu pid=%d comm=%s%s threads=%u shown=%u"
                                b" hidden=%u wait=%u run=%u rx=%u tm=%u wl=%u\n") in kernel,
             # Round273: r267 collapsed the workloop owner into one number and boot 59 could not tell
             # which of the kernel's three branches produced it. The row is the object: kq_state,
             # kq_count, kqwl_dynamicid and the whole kqwl_request. wl= on the header above is the
             # positive control -- boot 60 found the debtor on mach ports instead, so an absent row
             # has to stay readable as "no waiter" rather than "no instrument".
             "bb-peer-workloop": (b"clover r273: wl t=%llu pid=%d tid=%llx kn=%llx kq=%llx wlo=%u"
                                  b" ks=%x kc=%u kqpid=%d dyn=%llx tr=%x tc=%u tt=%llx ow=%llx\n") in kernel,
             # Round274: the peer task's first mappings, so the four frames of the bound servicer
             # that belong to no shared-cache image can be turned into functions of the binary we
             # already hold. Pinned whole for the same reason as every other row: a cut literal
             # still leaves the prefix needles green.
             "bb-peer-map": (b"clover r274: map t=%llu pid=%d ne=%u shown=%u e0=%llx-%llx"
                             b" e1=%llx-%llx e2=%llx-%llx e3=%llx-%llx\n") in kernel,
             # Round295: boot 82's chain ended on pid 187 and the kernel cannot say which service that is,
             # because p_comm is 17 bytes and com.apple.migration and com.apple.migrationpluginwrapper share
             # their first fifteen. The row carries both name sources and the walk's own verdict, so a chain
             # that stopped is a reading rather than a missing row -- which is only true if the whole literal
             # shipped, hence the pinned merge rather than a prefix.
             # Round296 rewrites that literal rather than adding to it: boot 83 ended both leaves on no_send
             # with thid=/rcv=/to= all zero, because the capture rule demanded a non-zero x3 and a thread whose
             # x3 is zero is inside the trap waiting to receive. cap= names which thread the three columns came
             # from and qmc=/qsr= are that port's own queue, so "the reply landed and never woke anybody" and
             # "no reply came" stop printing alike. The r295 literal is gone on purpose; the checker below asks
             # for the r296 one.
             # Round303 appends qerr= to that literal: boot 84's qmc=0 qsr=0 was indistinguishable from "the name
             # never became an object", because the helper left both at their init when its translate failed. Now
             # the queue read carries its own return code, so a 0 only reads as a finding when qerr=0 says the
             # translate succeeded. The literal moves with it: a cut needle stays green even when the row is wrong.
             "bb-send-chain": (b"clover r296: hop=%u t=%llu pid=%d comm=%s exec=%s ex=%s"
                               b" threads=%u wait=%u run=%u trap=%u cap=%s thid=%llx rcv=%x remote=%x"
                               b" to=%llx rmc=%u rsr=%x qmc=%u qsr=%u qerr=%d peer=%s end=%s\n") in kernel,
             # Round297 rides beside that row rather than inside it. Boot 84 read qmc=0 qsr=0 on the leaf's own
             # receive name, and the sentence "nothing is queued on the port this thread waits on" is only worth
             # anything if the request the previous hop placed was addressed to that port -- which no reader of
             # two per-task port indices can say. prev=/rvobj=/robj= are ipc_object pointers, the one value that
             # survives crossing address spaces, and link= is the kernel's own comparison of two of them. The
             # literal is pinned whole for the standing reason: a format string cut in half leaves every prefix
             # needle green, and this row is the whole answer of the round.
             # Round298 rewrites the identity row round297 shipped, in the same three columns: boot 86 read
            # link=unknown at the leaf because its rcv=d03 never became an object, and the helper that failed
            # is the same one that produced boot 84's qmc=0 -- both leave their out-parameters at zero when
            # ipc_object_translate() fails, so the row has to carry the codes. tname=/tspace= are the receiver's
            # own name and space for the port this hop wrote from (ip_receiver_name/ip_receiver, written by
            # ipc_port_init() at osfmk/ipc/ipc_port.c:846-847), and space= is this task's own, which is the
            # first comparison of two port names that lives inside one address space. Pinned whole: a cut
            # literal would still leave the r297 prefix needles green.
            # Round303 appends vb_r=/vb_v= to that literal: the leaf's verr=17 was KERN_INVALID_RIGHT, which the
            # translate returns only when the entry exists but holds neither a receive nor a send right -- a port
            # set, a send-once right or a dead name, three readings that all printed as the same rerr/verr. When
            # the translate fails the helper now asks ipc_right_lookup_read() and hands back the entry's own type
            # bits, so the shape behind the failure is named instead of inferred.
            "bb-chain-link": (b"clover r298: hop=%u t=%llu prev=%lx rvobj=%lx verr=%d vname=%x robj=%lx"
                              b" rerr=%d otype=%d tname=%x tspace=%lx space=%lx link=%s tlink=%s"
                              b" vb_r=%x vb_v=%x\n") in kernel,
             # Round275 measured the precondition of the only proposed fix and Round276 acted on it, so three
             # rows are pinned: the class lookup (what r275 read as 0/0 on boot 62, and what has to read 1 the
             # moment this round works -- the needle checking itself), the publish's own state (stage 3 with a
             # non-zero service is "started", and stage 0 is distinguishable from "capped" only because the row
             # exists), and the service census. r275's own svc/walked literals are gone on purpose: they matched
             # a case-sensitive "KeyStore" while the driver names its service "Native keystore state service",
             # so a successful publish would have kept reporting zero -- the replaced shapes are the second and
             # third rows here, under r276, with the name folded and the class asked beside it.
             "bb-aks-class": (b"clover r275: aks class=%d userclient_class=%d\n") in kernel,
             "bb-aks-publish": (b"clover r276: aks stage=%d status=%x service=%llx refused=%u opens=%u calls=%u "
                                b"sel7=%u sel14=%u sel17=%u sel35=%u sls=%u\n") in kernel,
             "bb-aks-svc": (b"clover r276: ks svc %.24s class=%.24s hit=%s%s\n") in kernel,
             "bb-aks-walked": (b"clover r276: ks walked=%d stopped=%d ks_names=%d ks_classes=%d rows=%d\n") in kernel,
             # The driver itself, not just the instrument that reports it: the name it gives the service is the
             # string the census has to match, so if it is absent the kernel was linked without the vendored
             # object and every r276 row above would still print happily.
             "bb-aks-service": b"Native keystore state service" in kernel,
             # Round277: the census rows that ride the driver's probe relay instead of the kernel log.
             # Boot 63 carried none of the tee rows at all, so these literals are what prove the round
             # shipped an instrument that can still be read when the tee cannot keep up.
             "bb-aks-relay": (b"clover r277: enter secs=%llu calls=%d\n") in kernel,
             "bb-aks-relay-aks": (b"clover r277: aks stage=%d status=%x service=%llx class=%d opens=%u calls=%u\n") in kernel,
             "bb-aks-relay-walk": (b"clover r277: walking visited=%d names=%d classes=%d\n") in kernel,
             "bb-aks-relay-end": (b"clover r277: svc-walk end visited=%d stopped=%d classes=%d\n") in kernel,
             # Round279. Boot 66 settled the bisect -- classes registered, service withheld, and the board
             # lived to its deadline with the whole census printed -- so what is left to learn is what a
             # client does with the published service. These five literals are that reading, and they are
             # pinned as whole format strings rather than as a prefix count because boot 63's census died
             # six tenths of a second before its own gate: an instrument that only speaks from the census
             # cannot testify about a boot that ends before it, and a payload that lost one of these rows
             # would report "nobody asked" for a keystore that nobody shipped.
             # Round281 replaces round280's one late table with a repeating schedule, because boot 68 stopped
             # at 1324 s before the table and a reading with one speaking time only describes boots that reach
             # it. Whole literals are pinned: the state row, the watchdog row and its no-map arm are three
             # separate readings, and a boot that ships two of them leaves the third to be inferred.
             "aks-say-state": (b"clover r281: aks t=%u fire=%u stage=%d status=%x service=%llx owner=%s "
                          b"opens=%u calls=%u refused=%u other=%u\n") in kernel,
             "aks-say-wdt": (b"clover r281: wdt t=%u en=%x armed=%u intmask=%u sts=%x bark=%u bite=%u "
                          b"bark_ms=%u bite_ms=%u req=%u pet=%u why=%s\n") in kernel,
             "aks-say-wdt-nomap": (b"clover r281: wdt t=%u nomap why=ctrl\n") in kernel,
             # Round324's two hands, both read out of the kernel image rather than from source: the arm
             # line (package/CloverKeyStoreProbe.cpp) is the positive control for a boot that reached the
             # arming window, and the bbwhy line (osfmk/kern/task.c) is the per-beat copy of who backboardd
             # blocks on -- the reading boot 109's hops=2 promised and its burst-only child dropped. Finding
             # the string proves the format survived to the image; it is not by itself a card reading.
             "r324-wdt-arm": (b"clover r324: wdt-arm t=%u bite_ms=%u at=%u\n") in kernel,
             "r324-bbwhy": (b"clover r324: bbwhy t=%llu pid=%d fired=%u hn=%u rem=%x set=%u "
                            b"state=%s owner=%s\n") in kernel,
             # Round325 keeps r324's beat copy and adds the one boot 110 proved necessary: the same answer
             # sealed from inside the fire that walks the hops, with every hop's owner instead of hop 0 alone.
             # Boot 110 latched at t=603 and ended near 612 while its next beat was due at 667, so the beat
             # cadence is measurably slower than the event; the relay row is the leg that arrives in the window.
             "r325-bbnow": (b"clover r325: bbnow t=%llu pid=%d fired=%u hn=%u rem=%x set=%u state=%s "
                            b"o0=%s o1=%s o2=%s o3=%s\n") in kernel,

             "aks-say-sel": (b"clover r281: aks sel%u n=%u rc=%x\n") in kernel,
             "aks-say-open": (b"clover r281: open %u pid=%d comm=%.16s\n") in kernel,
             "aks-say-call": (b"clover r281: call %u pid=%d comm=%.16s sel=%u rc=%x\n") in kernel,
             "aks-say-rows": (b"clover r281: rows open=%u/%u call=%u/%u dropped=%u\n") in kernel,
             # Both earlier shapes have to be gone: they would spend ring space on readings the ring drops anyway.
             "aks-say-r280-gone": b"clover r280: " not in kernel,
             "aks-say-r279-gone": b"clover r279: " not in kernel,
             # Round286 takes the GPU line from read-only to power-on: GDSC cx then gx, the AHB/CFG
             # votes, the gfx3d branch enable, then a read-back of the GDSCRs and a gated chipid read.
             # The literals are pinned as whole strings because the refusal arms are readings too: a
             # payload that carries the gcc row but not the skip arm would leave "the AHB vote was off"
             # indistinguishable from "the string was never linked", and a payload missing the chipid
             # row or its skipped arm would leave the round's acceptance to be inferred. The map and
             # no-timer arms say why a row that should be there is not; r282's rows have to be gone
             # because this round writes what that one only read.
             "gpu-row-gcc": (b"clover r286: gpu t=%u fire=%u gcc gpu_ahb=%u gpu_bimc=%u bimc=%u "
                             b"ctl_sdhc1_ahb=%u ctl_sdhc1_apps=%u raw ahb=%08x ctl=%08x\n") in kernel,
             "gpu-row-gcc-post": (b"clover r286: gpu t=%u gcc post gpu_ahb=%u gpu_bimc=%u bimc=%u "
                                  b"raw ahb=%08x bimc_gfx=%08x bimc=%08x\n") in kernel,
             "gpu-row-skip": (b"clover r286: gpu t=%u gpucc skipped ahb=%u raw=%08x (GPUCC access needs the "
                              b"gcc_gpu_cfg_ahb vote)\n") in kernel,
             "gpu-row-gpucc-pre": (b"clover r286: gpu t=%u gpucc pre cx=%08x cxhw=%08x cx_on=%u "
                                   b"cx_collapse=%u gx=%08x gx_on=%u gx_collapse=%u gfx3d_en=%u cxo_en=%u "
                                   b"rcg=%08x dom=%08x sw=%08x\n") in kernel,
             "gpu-row-gpucc-post": (b"clover r286: gpu t=%u gpucc post cx=%08x cxhw=%08x cx_on=%u "
                                    b"cx_collapse=%u gx=%08x gx_on=%u gx_collapse=%u gfx3d_en=%u cxo_en=%u "
                                    b"rcg=%08x dom=%08x sw=%08x\n") in kernel,
             "gpu-row-gdsc": (b"clover r286: gpu t=%u gdsc cx_on=%u gx_on=%u gfx3d_en=%u cx=%08x cxhw=%08x "
                              b"gx=%08x\n") in kernel,
             "gpu-row-chipid": b"clover r286: gpu t=%u chipid=%08x expect=%08x ok=%u\n" in kernel,
             "gpu-row-chipid-skip": (b"clover r286: gpu t=%u chipid skipped cx_on=%u gx_on=%u "
                                     b"gfx3d_en=%u gate=%s\n") in kernel,
             "gpu-row-coremapfail": b"clover r286: gpu t=%u core map failed phys=%llx\n" in kernel,
             "gpu-row-mapfail": b"clover r286: gpu t=%u gpucc map failed phys=%llx\n" in kernel,
             "gpu-mapfail": b"clover r286: gpu map failed phys=%llx size=%x\n" in kernel,
             "gpu-notimer-wl": b"clover r286: gpu t=%u notimer (resource service work loop unavailable)\n" in kernel,
             "gpu-notimer-src": b"clover r286: gpu t=%u notimer (timer source refused)\n" in kernel,
             "gpu-r282-gone": b"clover r282: " not in kernel,
             # Round283's answer crosses the reset in IMEM, but the two lines that read the record are
             # still strings in this image. Both are pinned because they are exclusive branches: a
             # payload carrying only the named branch could not tell "no reset hook fired" from "the
             # record was never read", and the unnamed line is the reading that says exactly that.
             "reset-record-named": (b"clover r283: last reset kind=%llu pid=%llu aux=%llx pc=%llx "
                                    b"cnt=%llu beat=%llu beatcnt=%llu beatmagic=%llx text=%s\n") in kernel,
             "reset-record-unnamed": (b"clover r283: last reset unnamed magic=%llx beat=%llu "
                                      b"beatcnt=%llu beatmagic=%llx\n") in kernel,
             # Round289: boot 74 and boot 75 both ended in `Kernel data abort.` and both were read as a pc plus
             # an lr that resolves to nothing, because the 480-byte panic record stops before esr, far, fp and
             # sp -- and the trace lives only in these rows. A payload without them would meet the next fatal
             # with exactly the reading that could not name the last two.
             "abort-state-row": (b"clover r289: abt pid=%d tid=0x%llx pc=%016llx lr=%016llx fp=%016llx"
                                 b" sp=%016llx esr=%08x far=%016llx cpsr=%08x kstack=%016llx..%016llx\n") in kernel,
             "abort-frames-row": (b"clover r289: bt n=%u from=%u f=") in kernel,
             "abort-frames-empty": (b"clover r289: bt n=0 from=0 f=\n") in kernel,
             # Round290: every GPU row this board has ever printed named the enable request (BIT(0)) and
             # never the halt state (BIT(31), mainline clk-branch.h:59 with BRANCH_HALT's 1 = halted), so
             # boot 74's `gfx3d_en=1` said only that the branch was asked. This is the row that says
             # whether the clock runs; without it the round asks a question the image cannot answer.
             "gpu-row-branches": (b"clover r290: gpu t=%u branches %s gfx3d=%u/%u cxo=%u/%u rbcpr=%u/%u "
                                  b"rbbm=%u/%u en/halt rcg3d=%08x rcg_rbcpr=%08x rcg_rbbm=%08x\n") in kernel,
             # Round291: boot 77 left gfx3d halted with gx_collapse=0 and gx_on=0 -- collapse released,
             # supply absent -- and the two halted branches' RCG words carry no sourced name. What is left
             # to know is whether the corner the gx GDSC powers against was ever raised, which is the LDO
             # word, the driver's own uV decode, and its own settling bits. Without this row the round asks
             # about a rail the image cannot read.
             "gpu-row-ldo": (b"clover r291: gpu t=%u ldo %s vref=%08x val=%u uv=%u ctrl=%08x byp=%u pdn=%u "
                             b"cpr_byp=%u sts=%08x ready=%u settled=%u cx_on=%u gx_on=%u\n") in kernel,
             # Round294's census: the whole LDO block, in one row. Pinned as the compiler merges the two
             # segments, which is the shape the shipped image carries -- the source splits it across lines.
             "gpu-row-ldo-census": (b"clover r294: gpu t=%u ldo words %s w00=%08x w04=%08x w08=%08x "
                                    b"w0c=%08x w10=%08x w14=%08x w18=%08x w1c=%08x w20=%08x w24=%08x "
                                    b"w28=%08x w2c=%08x w30=%08x cx_on=%u gx_on=%u\n") in kernel,
             # Round317's verdict: the same thirteen words counted against the vendor's init list by the
             # layer that list now lives in. Pinned together with the two CString tables it renders, because
             # the row is the interpretation and the tables are its vocabulary -- an image that linked the
             # row but dropped a name would print `shape=unknown-shape`, which is a build accident wearing
             # the costume of a reading. `no-gfx3d` is the chipid gate table, appended to the r286 refusal
             # row in the same round.
             "gpu-row-ldo-verdict": (b"clover r317: gpu t=%u ldo verdict %s shape=%s named=%u/%u "
                                      b"comparable=%u agree=%u differ=%u nz_zero=%u/%u no_expect=%u "
                                      b"outside=%u dead=%u ctrl_pdn=%u ctrl_unnamed=%08x vsettle=%u "
                                      b"map_ok=%u control=%u\n") in kernel,
             "gpu-ldo-shape-names": (b"no-words" in kernel and b"unnamed-mapping" in kernel
                                      and b"dead-page" in kernel and b"list-unwritten-blind" in kernel
                                      and b"list-never-written" in kernel and b"partial" in kernel
                                      and b"as-expected" in kernel),
             "gpu-chipid-gate-names": (b"no-cx" in kernel and b"no-gx" in kernel
                                       and b"no-gfx3d" in kernel),
             # Round311's MEM_ACC row, in all three shapes. They are pinned together because the row's
             # reading *is* which shape it took: the refused-range line and the answered-page line are the
             # two halves of "does this board's L1 corner select answer at all", and a driver that lost one
             # of them would still boot, still power the GDSCs and still print every GPU row above.
             "gpu-row-memacc-map": (b"clover r311: gpu t=%u fire=%u mem_acc map=0 phys=%llx size=%x\n")
                                    in kernel,
             "gpu-row-memacc-gpucc": (b"clover r311: gpu t=%u fire=%u mem_acc map=1 answered=%u "
                                      b"raw=%08x l1=%u corner=%u gpucc=1 cx_on=%u gx_on=%u\n") in kernel,
             "gpu-row-memacc-nogpucc": (b"clover r311: gpu t=%u fire=%u mem_acc map=1 answered=%u "
                                        b"raw=%08x l1=%u corner=%u gpucc=0\n") in kernel,
             "bb-peer-row": (b"clover r265: pth tid=%llx st=%x cont=%llx we=%llx pc=%llx"
                             b" dl=%llx rcv=%x remote=%x to=%llx mc=%u sr=%x rmc=%u rsr=%x"
                             b" peer=%s\n") in kernel,
             # Round266: boot 52 found the debtor asleep on five EVFILT_WORKLOOP waits and a semaphore, which
             # are not port waits -- the wait-event word is their only identity, and one user stack per
             # distinct continuation is what names the caller. A payload missing these re-asks the question
             # boot 52 already answered and cannot act on it.
             "bb-peer-stack": (b"clover r266: pustk t=%llu pid=%d tid=%llx cont=%llx"
                               b" we=%llx sp=%llx fp=%llx lr=%llx fn=%u%s\n") in kernel,
             # Round267: boot 53 showed the debtor's five workloop waits are five separate notes, so what is
             # left to know is who owns each one -- which is what thread->block_hint and the kernel's own
             # kdp_*_find_owner answer. Both censuses print the same row, so the string constant appears once
             # in the image; a payload without it names waits by their addresses again.
             "bb-wait-owner": (b"clover r267: own t=%llu pid=%d tid=%llx hint=%x ak=%u"
                               b" owner=%llx ctx=%llx\n") in kernel,
             # Round268: the kernel names a semaphore's creator, not its signaller, so the shipped kernel has
             # to read the object itself -- waiters queued and send rights left -- or the deepest wait in the
             # chain stays unattributable. One row per semaphore waiter, in both censuses, so the constant is
             # present once. Round269 appends port= to it, because the sender-right scan below needs the object
             # address on the frame to be checkable at all.
             "bb-sema-rights": (b"clover r268: sem t=%llu pid=%d tid=%llx count=%d"
                                b" srights=%u otype=%x receiver=%s port=%llx\n") in kernel,
             # Round269: boot 55 landed on count=-1 srights=1, the middle branch -- somebody can signal and has
             # not. Only the process list knows who, through the per-space reverse hash that holds pure send
             # rights. Without this row the round is a riddle again.
             "bb-sema-holder": (b"clover r269: srhost t=%llu pid=%d tid=%llx port=%llx hit=%llu"
                                b" name=%llx type=%llx scanned=%u probed=%u\n") in kernel,
             # Round270: boot 56 named the holder as the debtor's own task and the whole task is asleep with
             # nothing runnable, which is a wake-up that never lands. The driver already tallies IPIs sent,
             # IPIs received and per-core SGIs; this row is the only new thing, and a frame without it cannot
             # tell "the kernel never asked" from "the IPI went out and nobody arrived".
             "bb-ipi": (b"clover r270: ipi t=%llu sent=%llu recv=%llu tick=%llu"
                        b" sgi=%u,%u,%u,%u,%u,%u,%u,%u tm=%u,%u,%u,%u,%u,%u,%u,%u\n") in kernel,
             # Round271: boot 57 read sent=0 recv=0 and a timer tick on core 0 only, all of it from the CPU
             # driver's counters -- which cannot say whether a core was ever registered to receive one.
             # This row is XNU's own bookkeeping (real_ncpus, CpuDataEntries, StartedState, cpu_running,
             # cpu_stat.timer_cnt), and without it "eight waiters, one core" stays an inference from
             # interrupts rather than a reading of the scheduler's machine set.
             "bb-cpu": (b"clover r271: cpu t=%llu real=%u max=%u reg=%u start=%u run=%u"
                        b" s=%u%u%u%u%u%u%u%u r=%u%u%u%u%u%u%u%u"
                        b" tm=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n") in kernel,
             # Round299 adds the row beside that one rather than widening it: r271 is the kernel's answer to
             # "how many cpus does this board have", and boot 87 read real=1 max=8 reg=1 start=0 on both fires,
             # which is the question without the mechanism. The mechanism lives in the CPU driver -- how far
             # each core got, why it stopped, what the firmware answered, which address it was given and which
             # affinity the core reported -- so this row prints those six, pinned whole for the standing reason:
             # a cut format string leaves every prefix needle green, and this is the row the round ships.
             "bb-cpu-smp": (b"clover r299: smp t=%llu req=%u con=%u run=%u"
                            b" st=%u%u%u%u%u%u%u%u fl=%u%u%u%u%u%u%u%u"
                            b" rc=%d,%d,%d,%d,%d,%d,%d,%d"
                            b" want=%x,%x,%x,%x,%x,%x,%x,%x seen=%x,%x,%x,%x,%x,%x,%x,%x"
                            b" entry=%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx\n") in kernel,
             # Round300 pays the debt boot 88 opened: its ring stopped at the r299 row and the r262 thread rows
             # and r296/r298 chain rows were simply not there -- three exempt classes, no capped-at notice --
             # so nothing on the card separated a board that stopped mid-fire from a fire that emitted fewer
             # rows. A fire now numbers itself, states what it planned, and closes with what went out; both
             # rows are pinned whole, because a cut format string leaves every prefix needle green.
             "bb-census-fire": (b"clover r300: fire t=%llu seq=%llu pid=%d th=%u hidden=%u\n") in kernel,
             "bb-census-done": (b"clover r300: done t=%llu seq=%llu th=%u chain=%u\n") in kernel,
             # Round303 is the receiving task's own entry census -- the row the chain could not reach from its
             # own side. tname= was read under the port's lock in the receiver's space, so rnm= is exactly that
             # task's name for the port the sender wrote to (boot 87's 0x3207/0x3507/0x2903), and rentr=/rvbits=
             # answer whether the entry still holds a right and what kind, while rwait=/rwtid= ask the receiver's
             # own threads whether any waits on that name. Pinned whole for the standing reason.
             "bb-receiver-census": (b"clover r303: hop=%u t=%llu rpid=%d rnm=%x rentr=%d"
                                    b" rvbits=%x rwait=%u rwtid=%llx rbusy=%d\n") in kernel,
             # Round304 relays the fire's tail rows -- thread rows, chain pair, receiving row, and the fire's own
             # opening and closing rows -- into round212's driver RAM ring as well as to the console, because boot
             # 93 put the census head on the card and nothing behind it: those rows ride the tee, the tee rides a
             # block tail, and that boot ended before the next one. Two readings are pinned here. The relay helper
             # has to be in the shipped image (it is static, so its name lives in the symbol table and nowhere
             # else), and the driver has to say what it moved, because rows sitting in BSS with nothing reporting
             # seen/next/pend are the same silence this round replaces -- a number that is absent is the reading
             # boot 88, 90, 91 and 93 each had to argue about. The one-copy check is the other half: the relay is
             # the same buffer printf() prints, so a format string appearing twice would mean the row was rewritten
             # rather than copied, which would cut boots 50 through 93 out of the comparison.
             "census-relay": b"clover_census_row" in kernel,
             "census-relay-onecopy": all(kernel.count(marker) == 1 for marker in (
                 b"clover r262: th tid=", b"clover r300: fire t=", b"clover r300: done t=",
                 b"clover r296: hop=", b"clover r298: hop=", b"clover r303: hop=")),
             "probe-beat-flush": (b"CloverSdhci r307: probe why=%s calls=%u wrote=%u seen=%u next=%u pend=%u"
                                  b" flushed=%u skipped=%u stalled=%u refused=%u dropped=%u"
                                  b" starve=%u/%u/%u/%u pio=%u/%x/%u@%u ch=%u"
                                  b" blocked=%u/%u/%u empty=%u cut=%u\n") in kernel,
             # Round306. Two more shapes, because boot 95 showed that a relay with nothing pending is
             # silent in exactly the same way as a relay that was refused, and a consumer that stops at
             # its own guard never reaches the line above at all. Round307 renames both with one column
             # more (`calls=`) and moves each budget to its speaker, because boot 96 spent all four
             # `nothing` slots on why=tail and left the drain -- the consumer at the scene -- unsaid.
             "probe-silent": (b"CloverSdhci r307: probe silent why=%s guard=%s calls=%u seen=%u next=%u"
                              b" flushed=%u blocked=%u/%u/%u empty=%u cut=%u\n") in kernel,
             "probe-report-generation": (b"CloverSdhci r306: probe") not in kernel,
             "census-step": (b"clover r306: step t=%llu seq=%llu pid=%d th=%u hidden=%u\n") in kernel,
             # Round307. The relay-only copy of "the walk reached the counters". The check is that the row
             # is in the image and that its console twin is not: r306's step row is the pipe under audit,
             # so a second printf of this token in the same kernel would mean both copies ride it again
             # and the boot would be back to one absence standing for two facts.
             "census-pre": (b"clover r307: pre t=%llu seq=%llu pid=%d th=%u hidden=%u\n") in kernel,
             "census-pre-relay-only": kernel.count(b"clover r307: pre t=%llu") == 1,
             # Round308. Boot 97 is the shape: the census head, r270, r271 and r299 all reached the card and
             # then the boot said nothing, with the relay reporting seen=0 -- never asked for a row. That
             # brackets the stop inside a window with no statement that can block, so the two rows here seal
             # on the relay around the r299 printf and carry the r271 row's own core counts. Their console
             # twins are deliberately absent: the pair times one printf, and a copy that rides the pipe under
             # trial times nothing.
             "relay-bracket": all(needle in kernel for needle in (
                 b"clover r308: smp-pre t=%llu seq=%llu pid=%d th=%u hidden=%u"
                 b" reg=%u start=%u run=%u\n",
                 b"clover r308: smp-post t=%llu seq=%llu pid=%d th=%u hidden=%u"
                 b" reg=%u start=%u run=%u\n")),
             "relay-bracket-onceliteral": all(kernel.count(b"clover r308: smp-%s" % side) == 1
                                              for side in (b"pre", b"post")),
             # The beat thread's own accounting row, which is what makes an absent bracket pair a reading
             # rather than a hole: it runs whether or not a target ever latched, and it is the only row on
             # the card that says how many cpus XNU thinks it has on a boot whose tee went blind.
             "beat-accounting": (
                 b"clover r308: beat t=%llu target=%d ent=%u fired=%u same=%u cap=%u gone=%u"
                 b" notask=%u busy=%u reg=%u start=%u run=%u\n") in kernel,
             # Round309 moves three more statements off the kernel log and onto the relay, each one answering
             # a question the eleven-boot census absence could not: whether the gate ever opened (latch),
             # whether the burst was ever entered (cen-in) and where it left (cen-out with a named why). Boot
             # 101 is the shape that made them necessary -- the latch condition held at t=1418 in the very
             # row that latches, and the boot ended before the next beat row. Their console twins stay out of
             # the source (check-clover.py pins that), and one literal each is all the image can show.
             "gate-and-burst": all(needle in kernel for needle in (
                 b"clover r309: latch t=%llu pid=%d name=%x gen=%u\n",
                 b"clover r309: cen-in t=%llu pid=%d ent=%u\n",
                 b"clover r309: cen-out t=%llu pid=%d why=%s fired=%u\n")),
             "gate-and-burst-onceliteral": all(
                 kernel.count(b"clover r309: " + row) == 1
                 for row in (b"latch", b"cen-in", b"cen-out")),
             "bb-census-hop": (b"clover r231: bbhop t=%llu pid=%d tid=0x%llx op=%llx rcv=%x"
                               b" sz=%x remote=%x local=%x id=%x to=%llx rno=%s lo=%s\n") in kernel,
             "input-armed": (b"clover-input") in kernel,
             # Round257: the gate that made boot 44 say published=0 must be gone from the shipped image,
             # and the flag it tested has to still be readable as a value.
             "hid-gate": (b"device-has-no-built-in") not in kernel,
             "hid-flag": (b" deviceBuiltIn=%d") in kernel,
             "hid-early": (b"clover r255: device=%llx%s\n") in kernel,
             # The single-line-per-second print round255 shipped with is gone by design; leaving it in
             # the image would mean the beat was added without capping the early path, which is the one
             # thing boot 43 proved cannot be read back.
             "hid-spam": (b"clover r255: service=%llx%s\n") not in kernel,
             # The driver reaches for two producers through weak imports, and a static kernel link has
             # no undefined-weak: the first r255 build died on exactly these two names. The board's
             # answer is a pair of counters, printed in the stage line above and in the beat.
             "hid-name": (b"Offline HID input probe") in kernel,
             # Round258: the two things sheng has that this board never published. The digitizer
             # collection is a report-descriptor byte string, but DigitizerCollectionDispatch and
             # kern.maxvnodes are property names that only exist if the shipped image says so.
             "hid-digitizer": (b"DigitizerCollectionDispatch") in kernel,
             "adt-maxvnodes": (b"kern.maxvnodes") in kernel,
             "hid-resource": (b"IOHIDResource") in kernel,
             "tee-filter": (b"CloverSdhci r229: tee filter fl=%u nag=%u exit kept=%u "
                           b"dropped=%u comms=%u\n") in kernel,
             "tee-sync": (b"CloverSdhci r230: tee sync-in=%u sync-out=%u dropped=%u\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_flight_region(kernel: bytes) -> bool:
    """Round210 leaves a record before the stop instead of after it. The r209 frame showed the
    kernel alive to the deadline and the card refusing the sink, so the image has to carry the
    entry writer, the arming line that says where the entries went, and a beat entry the timer can
    call without waiting on the block guard. The beat's TryLock is the contract: a beat that
    blocked would be indistinguishable from a stopped card."""
    label = "kernel.macho: r210 flight region linked"
    found = {"armed": (b"CloverSdhci r210: flight armed entry=%u slots=%u beat=%u slots=%u "
                       b"window=%u..%u\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_signal_desc_probe(kernel: bytes) -> bool:
    """Round196 reads what an aborting process said. The r195 frame has usermanagerd answering
    SIGABRT (namespace 2, code 6) at three-second intervals for the rest of the boot, and the
    reason buffer carries the abort message -- but only the critical path prints it, and these
    processes never take that path. The image has to carry the line that prints it from
    proc_prepareexit instead, and it has to keep r193's critical-exit reading beside it."""
    label = "kernel.macho: signal description probe linked"
    found = {"sig": (b"clover r196: sig pid=%d comm=%s sig=%d n=%u desc=%s\n") in kernel,
             "kept-r193": (b"clover r193: crit pid=%d comm=%s rv=0x%x sig=%d fatal=%d sigcrash=%d "
                           b"crashbits=0x%x spawnfail=%d deadline=%llu now=%llu\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_user_breakpoint_probe(kernel: bytes) -> bool:
    """Round199 reads where a user SIGTRAP came from. The r198 frame has SpringBoard dying of
    namespace 2, code 5 (OS_REASON_SIGNAL, SIGTRAP) 0.35-0.66 s after every exec, with no reason
    payload and no line about the instruction. Round200 adds the map side: the entry that owns the
    pc, its segment bounds, the file offset and the instructions leading into the brk, because the
    trapping address lives in the dyld shared cache and cannot be symbolized on the host. Round203
    adds the failed assert's inputs -- x4-x7, x19-x28, the stack words past the eighth argument and
    the object x0 points at -- for traps inside that cache only. The image has to carry all three
    lines, and it has to keep the exit line that pairs the pid with its death -- round309 renamed
    that token along with its clock, so the needle follows the rename."""
    label = "kernel.macho: user breakpoint reading linked"
    found = {"ubrk": (b"clover r199: ubrk pid=%d comm=%s n=%u pc=0x%llx lr=0x%llx sp=0x%llx "
                      b"cpsr=0x%x esr=0x%x ec=%u brk=0x%x\n") in kernel,
             "ubrk2": (b"clover r200: ubrk2 pid=%d x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx "
                       b"x8=0x%llx x16=0x%llx x17=0x%llx map=%d start=0x%llx end=0x%llx "
                       b"off=0x%llx foff=0x%llx code=") in kernel,
             "ubrk3": (b"clover r203: ubrk3 pid=%d x4=%016llx x5=%016llx x6=%016llx "
                       b"x7=%016llx\n") in kernel,
             "ubrk7": (b"clover r204: ubrk7 pid=%d o=%016llx w0=%016llx w1=%016llx "
                       b"w2=%016llx w3=%016llx") in kernel,
             "ubrk8": (b"clover r206: ubrk%d pid=%d fp=%016llx w0=%016llx w1=%016llx "
                       b"w2=%016llx w3=%016llx w4=%016llx w5=%016llx w6=%016llx "
                       b"w7=%016llx w8=%016llx w9=%016llx w10=%016llx w11=%016llx\n") in kernel,
             "kept-exit": (b"clover r309: exit pid=%d ppid=%d t=%llus rv=0x%x ns=%d code=0x%llx "
                           b"comm=%s\n") in kernel}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_legacy_display(kernel: bytes) -> bool:
    """Round180 replaces the display stub with the original Legacy framebuffer class plus a probe that
    registers, constructs and unwinds it at phase 1. Nothing in the payload proves the class was linked
    except its own strings, so a payload that ships without them boots the round179 display stack while
    the menu, the boot args and this verifier all say otherwise."""
    label = "kernel.macho: legacy display archive linked"
    found = {name: needle.encode() in kernel for name, needle in LEGACY_DISPLAY}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_exec_sig_dump(kernel: bytes) -> bool:
    """Round166 prints the header words of the first eight code-signature blobs the loader reads.
    A payload without the string cannot answer whether the exec path was handed the right bytes."""
    label = "kernel.macho: exec signature dump"
    present = EXEC_SIG_DUMP.encode() in kernel
    print(f"{label:42} {'OK' if present else 'FAIL'} "
          f"({'present' if present else 'ABSENT'})")
    return present


def check_kernel_normalized(kernel: bytes) -> bool:
    """Round177 (schedule item 8): a rebuild of this tree is one image, not a near-miss.

    Three fields cannot survive a link: the LC_UUID ld mints per link, the mtime a rebuild writes into
    every object-path debug symbol's n_value, and the _PrelinkKCID that hashes the first two into
    submission. normalize-kernel-macho.py rewrites all three as functions of the rest of the file, and
    two real builds of the identical tree converge on sha256 86273887... The payload's copy is that
    kernel plus the entry probes this build writes into executable text -- a step the rewrite never
    touches -- so what is checkable on the shipped artifact is that the stamps are blank and that the
    file still parses as the prelinked kernel the normalizer accepts rather than a stranger it refuses.
    A payload built from an unnormalised kernel would be a file no rebuild can name."""
    label = "kernel.macho: build-varying fields normalized"
    if not NORMALIZER.is_file():
        print(f"{label:42} SKIPPED (normalize-kernel-macho.py absent)")
        return False
    spec = importlib.util.spec_from_file_location("normalize_kernel_macho", NORMALIZER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    try:
        fields, _rest = module.collect_fields(kernel)
    except ValueError as error:
        print(f"{label:42} FAIL (refused: {error})")
        return False
    stamps = [field for field in fields if field.kind == "stamp"]
    blank = sum(1 for field in stamps if kernel[field.start:field.end] == bytes(field.width()))
    uuids = sum(1 for field in fields if field.kind == "uuid")
    kcids = sum(1 for field in fields if field.kind == "kcid")
    ok = bool(stamps) and blank == len(stamps) and uuids == 1 and kcids == 1
    print(f"{label:42} {'OK' if ok else 'FAIL'} "
          f"({blank}/{len(stamps)} object stamps blank, {uuids} LC_UUID, {kcids} KCID)")
    return ok


def check_root_mount_probe(kernel: bytes) -> bool:
    """Round144 asks why r143 left bsd_init() with an empty mountlist although vfs_mountroot()
    returned 0. The probe leaves no symbol of its own -- vfs_subr.c records which return-0 branch
    ran in six volatiles and bsd_init.c reads them back -- so the image's only evidence is the two
    lines it can print: the retry that recovered, and the panic that carries both snapshots. Losing
    either means a boot that faults at the same MNT_ROOTFS write with no numbers, exactly the frame
    this round exists to replace. Round145 adds the per-call history (caller phase, tag, device,
    list head) and the mount-list add/remove counts that one snapshot could not carry."""
    label = "kernel.macho: root mount probe linked"
    found = {name: name.encode() in kernel
             for name in ("clover r145: mountlist empty after vfs_mountroot()==0"
                          " p1 tag=%d mr=%lx first=%lx n=%d inshut=%d dev=%d"
                          " p2 err=%d tag=%d first=%lx n=%d",
                          "clover r145: root mount recovered on retry p1tag=%d p2err=%d"
                          " first=%p\n",
                          "clover r145 hist calls=%d add=%d del=%d now ph=%d tag=%d dev=%d"
                          " n=%d first=%lx\n",
                          "clover r145 call %d ph=%d tag=%d dev=%d n=%d first=%lx\n")}
    ok = all(found.values())
    detail = ", ".join(f"{(name.split(':', 1)[1] if ':' in name else name).strip()[:28]}"
                       f" {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_shared_region_probe_removed(kernel: bytes) -> bool:
    """Round152 took r146's five vm_unix.c probes out, and Round153 keeps them out: the namei ran
    inside shared_region_check_np, the syscall dyld makes just before it looks for the cache, and
    it was the only VFS caller of ours at that moment. The image's evidence is absence: no r146
    format string may survive, and the wrong cache name it looked up must not either."""
    label = "kernel.macho: r146 probe removed"
    found = {"prefix": b"clover r146:" in kernel,
             "arm64e": b"dyld_shared_cache_arm64e" in kernel}
    ok = not any(found.values())
    detail = ", ".join(f"{name} {'STILL PRESENT' if yes else 'absent'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_cache_open_probe(kernel: bytes) -> bool:
    """Round153 fixed the probe r151 shipped -- that one read cn_pnbuf before
    vn_open_auth_internal ran, so the read was garbage and the frame stayed silent for its own
    reasons. Round170 keeps the post-call form but throttles it: r169's frame spent the whole boot
    writing the same two lines, because every process that runs dyld init re-opens the cache
    directory and re-asks shared_region_check_np. The image must carry the throttled prints -- the
    path print, the mount print, the header read, and the per-pid dyld_flags line -- and the old
    uncapped r153 forms must be gone."""
    label = "kernel.macho: cache open probe linked"
    found = {"open": b"clover r170: open %s fmode=0x%x err=%d" in kernel,
             "mount": b" fl=0x%llx vt=%d fs=%s" in kernel,
             "read": b"clover r153: read err=%d resid=%d hdr=%s" in kernel,
             "flags": b"clover r170: check_np pid=%d comm=%s dyld_flags=0x%llx" in kernel}
    ok = all(found.values())
    if b"dyld_shared_cache" not in kernel or b"/System/Library/Caches/com.apple.dyld" not in kernel:
        found["prefixes"] = False
        ok = False
    else:
        found["prefixes"] = True
    stale = (b"clover r151:" in kernel or
             b"clover r153: check_np dyld_flags=0x%llx" in kernel or
             b"clover r153: open %s fmode=0x%x err=%d" in kernel)
    ok = ok and not stale
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    detail += f", r151/r153 uncapped form {'STILL PRESENT' if stale else 'absent'}"
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_map_slide_probe(kernel: bytes) -> bool:
    """Round154 asks what happened after the .01 header read. r153's frame ended there, with both
    cache files opened and both headers read, so the next call is syscall 536
    (shared_region_map_and_slide_np). The image must carry the entry line, the first descriptor, the
    return value, the setup stage ladder and the carrier print. r153's strings must survive: this
    round adds a question, it does not replace the previous one."""
    label = "kernel.macho: map-and-slide probe linked"
    found = {"enter": b"clover r154: map2 enter files=%u mappings=%u" in kernel,
             "fd0": b"clover r154: map2 fd0=%d m0=%u slide0=%u" in kernel,
             "return": b"clover r154: map2 <- err=%d" in kernel,
             "setup": b"clover r154: setup err=%d stage=%d at=%u files=%u mappings=%u" in kernel,
             "mapfile": b"clover r154: mapfile kr=0x%x" in kernel}
    ok = all(found.values())
    kept = b"clover r170: open %s fmode=0x%x err=%d" in kernel
    ok = ok and kept
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    detail += f", r170 open form {'kept' if kept else 'GONE'}"
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_cache_gate_probe(kernel: bytes) -> bool:
    """Round155 asks what stopped dyld after the .01 open. r154's frame carried no map2 line at all,
    so dyld never reached syscall 536, and the two remaining shapes are a header field that
    preflightCacheFile rejected silently and a code-signature read still in progress. The image must
    carry the header dump (fields plus vnode_size) and the F_ADDFILESIGS_RETURN trace (entry, hit,
    allocation, read, duration). Round156 adds one line: the blob-add failure, which is what a
    rejected signature blob would otherwise do in silence. r153's open and r154's map2 strings must
    survive: this round adds a question, it does not replace the earlier ones."""
    label = "kernel.macho: cache gate probe linked"
    found = {"hdr_size": b"clover r155: hdr %s vp=%p len=%lld sig=%llu+%llu err=%d/%d" in kernel,
             "hdr_layout": b"clover r155: hdr %s off=%u n=%u ty=%llu/%u plat=%u/%u fl=%x" in kernel,
             "hdr_maps": b"clover r155: hdr %s m0=%llu/%x ml=%llu/%x subs=%u@%u" in kernel,
             "fsig": b"clover r155: fsig vp=%p start=%llu blob=%llu size=%llu" in kernel,
             "fsig_hit": b"clover r155: fsig hit=1" in kernel,
             "fsig_mem": b"clover r155: fsig mem=%p/%llu" in kernel,
             "fsig_read": b"clover r155: fsig rd off=%llu len=%llu" in kernel,
             "fsig_done": b"clover r155: fsig rd <- err=%d resid=%d ms=%llu" in kernel,
             "fsig_add": b"clover r156: fsig add err=%d" in kernel}
    ok = all(found.values())
    kept = (b"clover r170: open %s fmode=0x%x err=%d" in kernel and
            b"clover r154: map2 enter files=%u mappings=%u" in kernel)
    ok = ok and kept
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    detail += f", r170/r154 forms {'kept' if kept else 'GONE'}"
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_spawn_probe(kernel: bytes) -> bool:
    """Round170 asks who is running and who is dying. r169's frame had no panic, no EIO and no
    signature line: the console held only the shared-cache probe's own two lines, repeated, and
    launchd's restore event. That tells us the read path is no longer the wall but not where the
    process tree went, because XNU only prints an exit reason for the critical processes -- a normal
    child's death is silent. The image must carry one exec line and one exit line, each with pid,
    parent, boot-relative seconds and the exit namespace/code, and each capped at 256 lines so the
    panel keeps up with the boot instead of repeating one line forever.
    Round309 moves that pair's token: the two rows divided clock_get_uptime()'s machine ticks by 1e9
    on a 19267584 Hz time base, so every `t=` ever read from them was compressed 51.9x. The columns
    and their order are unchanged -- only the unit and the round that owns the line. The old token
    is reverse-pinned because the standing rule couples a value's name to its producer: an image that
    carried both would let a card's old r170 bytes be read as this round's seconds."""
    label = "kernel.macho: exec/exit probe linked"
    found = {"exec": b"clover r309: exec pid=%d ppid=%d t=%llus comm=%s" in kernel,
             "exit": (b"clover r309: exit pid=%d ppid=%d t=%llus "
                      b"rv=0x%x ns=%d code=0x%llx comm=%s") in kernel,
             "old-token-gone": all(needle not in kernel for needle in (
                 b"clover r170: exec pid=%d ppid=%d t=%llus comm=%s",
                 b"clover r170: exit pid=%d ppid=%d t=%llus rv=0x%x ns=%d code=0x%llx comm=%s"))}
    ok = all(found.values())
    detail = ", ".join(f"{name} {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def macho_symbols(kernel: bytes) -> tuple[list[tuple[str, int, int, int, int]], dict[str, int]]:
    """(segments, symbols): segments as (name, vmaddr, vmsize, fileoff, filesize), symbols as name -> va."""
    ncmds = struct.unpack_from("<I", kernel, 16)[0]
    offset = 32
    segments = []
    table = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", kernel, offset)
        if cmd == 0x19 and size >= 72:
            name = kernel[offset + 8:offset + 24].rstrip(b"\0").decode(errors="replace")
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", kernel, offset + 24)
            segments.append((name, vmaddr, vmsize, fileoff, filesize))
        elif cmd == 0x2:
            table = struct.unpack_from("<IIII", kernel, offset + 8)
        offset += size
    symbols: dict[str, int] = {}
    if table is not None:
        symoff, nsyms, stroff, _ = table
        for i in range(nsyms):
            base = symoff + 16 * i
            strx, _, _, _ = struct.unpack_from("<IBBh", kernel, base)
            va = struct.unpack_from("<Q", kernel, base + 8)[0]
            if not strx or not va:
                continue
            end = kernel.index(b"\0", stroff + strx)
            symbols.setdefault(kernel[stroff + strx:end].decode(errors="replace"), va)
    return segments, symbols


def check_inline_asm_tails(kernel: bytes) -> bool:
    """Round148: the assembler was cutting every inline asm at its first `;`.

    Darwin's arm64 assembler reads `;` as the comment character, so
    "isb; mrs %0, cntvct_el0" assembled to just `isb`, and round147's watchdog
    read a garbage counter and fired on its first tick.  Source strings cannot
    prove the tail survived -- the text is in the image either way -- and a
    whole-image count is useless because CNTVCT/SPSR instructions exist in the
    kernel's own exception code.  So this resolves the three board functions by
    symbol and counts inside each one: the interrupt entry must read both
    ELR_EL1 and SPSR_EL1 and call counter(), and signalCPU() must write
    ICC_SGI1R_EL1.  Round147's image has 0/0/0 there; this round has 1/2/1.

    The panic screen is not covered here on purpose: it is built for
    aarch64-none-elf, whose assembler treats `;` as a statement separator, so
    those three sequences never lost an instruction."""
    label = "kernel.macho: asm tails survived the assembler"
    wanted = {
        "__ZN6ArmCpu9interruptEv": (("mrs SPSR_EL1", 0xd5384000),
                                    ("mrs CNTVCT_EL0", 0xd53be040)),
        "__ZN6ArmCpu9signalCPUEP5IOCPU": (("msr ICC_SGI1R_EL1", 0xd518cba0),),
    }
    try:
        segments, symbols = macho_symbols(kernel)
        va_order = sorted(set(symbols.values()))
        counts: dict[str, int] = {}
        for symbol, opcodes in wanted.items():
            start = symbols.get(symbol)
            if start is None:
                print(f"{label:42} FAIL ({symbol} is not in this image's symbol table)")
                return False
            end = next((va for va in va_order if va > start), None)
            seg = next((s for s in segments if s[1] <= start < s[1] + s[2]), None)
            end = min(end or seg[1] + seg[2], seg[1] + seg[2])
            file_start = seg[3] + (start - seg[1])
            body = kernel[file_start:file_start + (end - start)]
            words = [word for (word,) in struct.iter_unpack("<I", body[:len(body) - len(body) % 4])]
            for tag, base in opcodes:
                counts[f"{tag} in {symbol.split('ArmCpu9')[1][:9]}"] = sum(
                    1 for word in words if (word & 0xffffffe0) == base)
    except (struct.error, ValueError):
        print(f"{label:42} FAIL (the kernel is not a parseable Mach-O)")
        return False
    ok = all(count > 0 for count in counts.values())
    detail = ", ".join(f"{tag} {count}" for tag, count in counts.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def check_unattended(kernel: bytes) -> bool:
    """Round147 makes a boot answer without anyone watching: ArmCpu panics at a wall-clock
    deadline, the panic screen marks the board's restart reason for fastboot and resets,
    CloverSdhci publishes the IMEM cell and marks it as soon as PMGR runs. All three live
    behind format strings -- the deadline's panic, the driver's three IOLog lines, and the
    screen's `fb=` row -- because none of them can add a symbol the layout file lacks. A
    frame that stops at a parked panic screen looks exactly like a solved round, so the
    image has to carry all five."""
    label = "kernel.macho: unattended watchdog linked"
    found = {name: name.encode() in kernel
             for name in ("clover r148: unattended deadline %llu s (elr=%llx spsr=%llx"
                          " ticks=%llu cnt0=%llx cnt1=%llx freq=%llx)",
                          "clover r148: unattended mark %x -> %x deadline=%us\n",
                          "clover r148: unattended mark %x read back %x\n",
                          "clover r148: unattended imem map failed %llx\n",
                          "\nfb=")}
    ok = all(found.values())
    detail = ", ".join(f"{(name.split(':', 1)[1] if ':' in name else name).strip()[:28]}"
                       f" {'present' if yes else 'ABSENT'}"
                       for name, yes in found.items())
    print(f"{label:42} {'OK' if ok else 'FAIL'} ({detail})")
    return ok


def report(image: pathlib.Path) -> int:
    print(f"== {image}")
    ok = True
    blobs: dict[str, bytes] = {}
    for name in FILES:
        try:
            blobs[name] = mcopy_out(image, locate(image, name))
        except Exception as exc:  # noqa: BLE001 - report every gap, not just the first
            print(f"{name:34} MISSING ({exc})")
            ok = False
    for name, blob in blobs.items():
        print(f"{name:34} {len(blob):>9} bytes  sha256 {hashlib.sha256(blob).hexdigest()}")

    checks = [
        ("scratch-data.img == signed root", blobs.get("scratch-data.img"), LOCAL_ROOT),
        ("trustcache == published bundle", blobs.get("kernel.macho.trustcache"), LOCAL_CACHE),
    ]
    for label, inside, disk in checks:
        if inside is None or not disk.exists():
            print(f"{label:42} SKIPPED")
            ok = False
            continue
        same = inside == disk.read_bytes()
        ok &= same
        print(f"{label:42} {'OK' if same else 'DIFFERS'}")

    # Round248. unixnu.toml is the one file on this volume the firmware must read before XNU exists,
    # and boots 35 and 36 measured what a manifest it cannot parse costs: no boot id, no beat, no
    # panic record, no kernel log -- because no kernel ran. The round title carries a quoted function
    # name, the shape the emitter used to write between bare quotes, so the shipped file is parsed
    # here the way the firmware's parser parses it and required to offer this board's title, its
    # kernel and -- since round287 restores the deadline round285 removed -- a clover-unattended=
    # argument (the run script's only landing path is the deadline panic's own recovery BCB).
    manifest = blobs.get("unixnu.toml")
    label = "unixnu.toml: parses and offers this round's xnu entry"
    if manifest is None:
        print(f"{label:42} SKIPPED")
        ok = False
    else:
        try:
            parsed = tomllib.loads(manifest.decode())
        except tomllib.TOMLDecodeError as exc:
            ok = False
            print(f"{label:42} FAIL (unparsable: {exc})")
        else:
            board = tomllib.loads((pathlib.Path(__file__).resolve().parents[1] / "board.toml").read_text())
            xnu = next((entry for entry in parsed.get("entry", []) if entry.get("type") == "xnu"), {})
            parts = {"title": xnu.get("title") == board["payload"]["title"],
                     "kernel": xnu.get("path") == "Payloads/kernel.macho",
                     "deadline": "clover-unattended=" in xnu.get("args", "")}
            same = all(parts.values())
            ok &= same
            print(f"{label:42} {'OK' if same else 'FAIL'} "
                  f"({', '.join(f'{name} ok' if yes else f'{name} WRONG' for name, yes in parts.items())})")

    # Round137's root is not the initrd; it is the APFS volume /chosen names. A payload whose ADT
    # lost the dictionary boots nothing, and the kernel would wait at IOFindBSDRoot for ten timeouts.
    adt = blobs.get("kernel.macho.adt")
    label = "ADT: root-matching names the OS volume"
    wanted = b"root-matching"
    uuid = b"04769D26-2CA0-4825-B443-E2E5F88300F2"
    if adt is None:
        print(f"{label:42} SKIPPED")
        ok = False
    else:
        present = wanted in adt and uuid in adt and b"IOMedia" in adt
        ok &= present
        print(f"{label:42} {'OK' if present else 'FAIL'} "
              f"({'UUID present' if uuid in adt else 'UUID ABSENT'})")

    # Round322 REVERTS round320. boot 108 read back r320 and the desktop stall only moved: SpringBoard
    # left the migration port (0x6003 / migrationpluginwrapper gone from the window) and now idles every
    # beat on a chronod round-trip (tid=0x129, remote 0x2f03, ro=/ho=122:chronod, through the deadline).
    # UniXNU-sheng, the board that boots this image to the desktop, does so with persistent_data=true and
    # NO ephemeral-storage (make-sheng-adt.py:95: "no ephemeral-storage, so first-boot data migration
    # runs") -- round320 cited it backwards. This cheap A/B stops asserting a volatile root is ephemeral
    # so first-boot migration runs against the card's real /private/var window; the ADT key must be ABSENT.
    label = "ADT: ephemeral-storage ABSENT (r322 revert)"
    if adt is None:
        print(f"{label:42} SKIPPED")
    else:
        absent = b"ephemeral-storage" not in adt
        ok &= absent
        print(f"{label:42} {'OK' if absent else 'FAIL'}")

    # The kernel slot cannot be a byte-equality check: `unixnu build payload`
    # writes the entry probes from board.toml (entry_probe_reset_*) into the
    # copy it places on the volume, so the in-volume kernel is the locally built
    # kernel plus a few dozen patched words. What proves the round is that the
    # patch stayed local: same length, and the differing bytes confined to small
    # runs inside the executable text. A whole-file rewrite here would mean the
    # kernel itself changed under the payload build, which is a different round's
    # business. Differences outside executable text are counted and ignored:
    # they are the prelink UUID, which every `build kernel` run mints anew.
    kernel = blobs.get("kernel.macho")
    label = "kernel.macho: probe patch localized"
    if kernel is None or not LOCAL_KERNEL.exists():
        print(f"{label:42} SKIPPED")
        ok = False
    else:
        disk = LOCAL_KERNEL.read_bytes()
        ranges = exec_text_ranges(kernel)
        if len(kernel) != len(disk):
            print(f"{label:42} FAIL (length {len(kernel)} vs {len(disk)})")
            ok = False
        elif not ranges:
            print(f"{label:42} FAIL (no executable text segments found)")
            ok = False
        else:
            differing = [i for i in range(len(kernel)) if kernel[i] != disk[i]]
            offs = [i for i in differing if any(lo <= i < hi for lo, hi in ranges)]
            outside = len(differing) - len(offs)
            runs = 1 + sum(1 for a, b in zip(offs, offs[1:]) if b - a > 64) if offs else 0
            localized = bool(offs) and len(offs) <= 20000 and runs <= 64
            print(f"{label:42} {'OK' if localized else 'FAIL'} "
                  f"({len(offs)} bytes in {runs} runs, {outside} outside executable text "
                  f"ignored, sha256 {hashlib.sha256(kernel).hexdigest()})")
            ok &= localized
    if kernel is not None:
        ok &= check_pmgr(kernel)
        ok &= check_sdhci(kernel)
        ok &= check_block(kernel)
        ok &= check_owner_mount(kernel)
        ok &= check_owner_root_trust(kernel)
        ok &= check_root_mount_probe(kernel)
        ok &= check_shared_region_probe_removed(kernel)
        ok &= check_cache_open_probe(kernel)
        ok &= check_map_slide_probe(kernel)
        ok &= check_cache_gate_probe(kernel)
        ok &= check_spawn_probe(kernel)
        ok &= check_mac_probe(kernel)
        ok &= check_amfi_allow(kernel)
        ok &= check_dyld_cache_skip(kernel)
        ok &= check_read_integrity(kernel)
        ok &= check_guard_handoff(kernel)
        ok &= check_verdict_ring(kernel)
        ok &= check_tee_probe_cap(kernel)
        ok &= check_exec_sig_dump(kernel)
        ok &= check_kernel_normalized(kernel)
        ok &= check_unattended(kernel)
        ok &= check_inline_asm_tails(kernel)
        ok &= check_legacy_display(kernel)
        ok &= check_clover_input(kernel)
        ok &= check_clover_power(kernel)
        ok &= check_clover_dsp(kernel)
        ok &= check_panic_drain(kernel)
        ok &= check_log_window(kernel)
        ok &= check_spawn_why(kernel)
        ok &= check_sys_why(kernel)
        ok &= check_sys_why2(kernel)
        ok &= check_sys_why3(kernel)
        ok &= check_sys_why4(kernel)
        ok &= check_flush_stages(kernel)
        ok &= check_cache_ownership(kernel)
        ok &= check_critical_exit_probe(kernel)
        ok &= check_panic_tail_read(kernel)
        ok &= check_boot_record(kernel)
        ok &= check_flight_region(kernel)
        ok &= check_host_ticket(kernel)
        ok &= check_card_reinit(kernel)
        ok &= check_card_recover(kernel)
        ok &= check_sb_sample(kernel)
        ok &= check_signal_desc_probe(kernel)
        ok &= check_user_breakpoint_probe(kernel)
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="*", type=pathlib.Path, default=[XNU_PAYLOAD])
    args = ap.parse_args()
    return min(1, sum(report(image) != 0 for image in args.image))


if __name__ == "__main__":
    sys.exit(main())
