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
import json
import pathlib
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
                          "CloverBlock r145: boot os=%u+%u system=%u+%u fires=%u deferred=1",
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
                          "_commpage_update_dyld_flags",
                          "native_owner_cache_device_path",
                          "native_owner_cache_native_union")}
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


def check_shared_region_probe(kernel: bytes) -> bool:
    """Round146 asks what the dyld shared-cache path answered after r145's empty-region check.
    The probes live in vm_unix.c: the check's answer, a namei of the cache taken at that same
    moment (the file's size and the mounts behind it, which is how a union over "/" shows up),
    the map call's entry, its setup error, and its final errno. None of them adds a symbol, so
    the image's only evidence is these five format strings; losing one turns that part of the
    question back into silence."""
    label = "kernel.macho: shared-region probe linked"
    found = {name: name.encode() in kernel
             for name in ("clover r146: check err=%d start=%llx\n",
                          "clover r146: cache err=%d size=%lld mnt=%p rmnt=%p"
                          " fl=0x%llx rfl=0x%llx\n",
                          "clover r146: map enter files=%u maps=%u\n",
                          "clover r146: map setup err=%d\n",
                          "clover r146: map exit err=%d kr=0x%x\n")}
    ok = all(found.values())
    detail = ", ".join(f"{(name.split(':', 1)[1] if ':' in name else name).strip()[:28]}"
                       f" {'present' if yes else 'ABSENT'}"
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
    # runs. A whole-file rewrite here would mean the kernel itself changed under
    # the payload build, which is a different round's business.
    kernel = blobs.get("kernel.macho")
    label = "kernel.macho: probe patch localized"
    if kernel is None or not LOCAL_KERNEL.exists():
        print(f"{label:42} SKIPPED")
        ok = False
    else:
        disk = LOCAL_KERNEL.read_bytes()
        if len(kernel) != len(disk):
            print(f"{label:42} FAIL (length {len(kernel)} vs {len(disk)})")
            ok = False
        else:
            offs = [i for i in range(len(kernel)) if kernel[i] != disk[i]]
            runs = 1 + sum(1 for a, b in zip(offs, offs[1:]) if b - a > 64) if offs else 0
            localized = len(offs) <= 20000 and runs <= 64
            print(f"{label:42} {'OK' if localized else 'FAIL'} "
                  f"({len(offs)} bytes in {runs} runs, "
                  f"sha256 {hashlib.sha256(kernel).hexdigest()})")
            ok &= localized
    if kernel is not None:
        ok &= check_pmgr(kernel)
        ok &= check_sdhci(kernel)
        ok &= check_block(kernel)
        ok &= check_owner_mount(kernel)
        ok &= check_owner_root_trust(kernel)
        ok &= check_root_mount_probe(kernel)
        ok &= check_shared_region_probe(kernel)
        ok &= check_mac_probe(kernel)
        ok &= check_unattended(kernel)
        ok &= check_inline_asm_tails(kernel)
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="*", type=pathlib.Path, default=[XNU_PAYLOAD])
    args = ap.parse_args()
    return min(1, sum(report(image) != 0 for image in args.image))


if __name__ == "__main__":
    sys.exit(main())
