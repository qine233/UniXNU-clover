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
                          "_clover_emmc_block_userdata_window", "SHNGROOT", "EFI PART",
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
                ("no-answer reading", "no address ACKed; rails unproven, sda=%08x scl=%08x status=%08x op=%08x"))


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
CLOVER_POWER = (("power round stamp", "clover r182:"),
                ("declared windows", "declared core=%llx+%u chnls=%llx+%llu obsrvr=%llx+%llu"),
                ("map walk", "scan %s ppid=0x%03x rc=%d(%s) ver=%08x words=%u mapped=%u span=%u"),
                ("ownership gate", "write-owner is EE %u, this EE is %u: reads ok, writes denied"),
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
    crash-behavior bits and deadline of every critical exit, and it has to keep the r170 exit line
    -- that one is what pairs the death with its pid and wait status in the same frame. Round211
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
             "kept-r170": (b"clover r170: exit pid=%d ppid=%d t=%llus rv=0x%x ns=%d code=0x%llx "
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
    boot id in every row, plus the probe ring the kernel's kill-point lines are relayed into."""
    label = "kernel.macho: panic-time log tail linked"
    found = {"tail": (b"CloverSdhci r212: panic tail lines=%u unseen=%u bufx=%d tee=%u boot=%u "
                      b"cursor=%u size=%u lap=%u refused=%u pend=%u drop=%u guard=%u last=%ums "
                      b"cache=%u append_rc=%u\n") in kernel,
             "refused": (b"CloverSdhci r212: panic tail refused reason=%u magic=0x%x size=%d "
                         b"bufx=%d tee=%u boot=%u\n") in kernel,
             "enter": (b"CloverSdhci r212: drain enter boot=%u first=%u panic_first=%u "
                       b"verdict=%u tee=%u append_rc=%u probe_seen=%u\n") in kernel,
             "exit": (b"CloverSdhci r212: drain exit boot=%u wrote=%u dropped=%u tee_passes=%u "
                      b"probe=%u probe_drop=%u probe_pend=%u\n") in kernel,
             "kept-r184": (b"CloverSdhci r184: panic drain wrote=%u dropped=%u tee_passes=%u\n")
                          in kernel}
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
             "sbpc": (b"clover r218: sbpc t=%llu tid=0x%llx x16=%llx n=%u") in kernel}
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
    lines, and it has to keep the r170 exit line that pairs the pid with its death."""
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
             "kept-r170": (b"clover r170: exit pid=%d ppid=%d t=%llus rv=0x%x ns=%d code=0x%llx "
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
    panel keeps up with the boot instead of repeating one line forever."""
    label = "kernel.macho: exec/exit probe linked"
    found = {"exec": b"clover r170: exec pid=%d ppid=%d t=%llus comm=%s" in kernel,
             "exit": (b"clover r170: exit pid=%d ppid=%d t=%llus "
                      b"rv=0x%x ns=%d code=0x%llx comm=%s") in kernel}
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
