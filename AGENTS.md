# Agent instructions

This repository owns clover board configuration, the eMMC storage path, firmware
selection, the Apple device tree, and payload assembly. Shared XNU sources, the
component catalog, and the build tools belong to `UniXNU`. The companion UEFI
firmware is `uefi-clover`.

Read the following before editing:

| Task | Required reading |
| --- | --- |
| Every port | [Porting](https://unixnu.com/porting/), [repository format](https://unixnu.com/porting/device-repository), [component catalog](https://unixnu.com/reference/components) |
| Hardware | [Device trees](https://unixnu.com/porting/device-tree), [matching](https://unixnu.com/drivers/device-tree-matching), `notes/clover-device-tree-sources.md`, `facts/<serial>-<stamp>/` |
| Kernel | Selected OS profile, XNU source, [patch reference](https://unixnu.com/xnu/source-and-patches), `patches/xnu-clover/` |
| Storage | [Bring-up](https://unixnu.com/porting/bring-up), `drivers/CloverSdhci/`, `drivers/CloverBlock/` |
| Images | [Prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), [UEFI firmware](https://unixnu.com/porting/uefi-firmware) |
| Verification | [Building](https://unixnu.com/building), `scripts/check-clover.py`, `scripts/verify-clover-payload.py` |

Search UniXNU's `components.toml` and read existing implementations before
adding a capability. Use canonical component names in sources, paths, build
commands, and dependencies. Preserve source filenames, runtime classes, bundle
IDs, plugin filenames, symbols, and protocol IDs.

Board hardware constants come from two device trees that agree with each other
and from `facts/`. Do not invent a register or a memory range: record where it
came from in `board.toml` or in a note. `sdhci@0xc0c4000`, the 0x9D400000 splash
reservation, msm-id 317, and the 1200x1920 panel are device-verified.

Run from the UniXNU checkout:

```sh
./unixnu defconfig clover_defconfig --device ../UniXNU-clover
./unixnu status --device ../UniXNU-clover --json
git -C out/clover/tree/.cache/xnu-work apply -R \
  ../../patches/xnu/zzy-clover-dyld-cache-skip.patch   # see the note below
./unixnu build kernel --device ../UniXNU-clover
python3 ../UniXNU-clover/scripts/patch-clover-amfi.py \
  out/clover/kernel/kernel.macho ~/clover-out/roundNNN-kernel-amfi.macho
python3 ../UniXNU-clover/scripts/normalize-kernel-macho.py \
  ~/clover-out/roundNNN-kernel-amfi.macho
./unixnu build payload --device ../UniXNU-clover --unique-chip-id HEX \
  --uefi ../mu-clover \
  --kernel PATH/roundNNN-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

`build-xnu-4k-baseline.sh` replays `patches/xnu/*.patch` in name order and skips
a patch whose `--reverse --check` passes, which is what makes a rebuild of one
tree one run. `zzy-clover-dyld-cache-skip.patch` edits the same `ubc_subr.c` and
`ubc_internal.h` context as the earlier `native-4k-signature-hash-cache.patch`,
so once it is in the tree the earlier patch is neither forward- nor
reverse-appliable and the replay stops with `patch does not apply`. Reversing it
first is the same convention the script already uses for the three base patches
that overlap root/exec context -- reverse the newest, let the loop re-apply in
name order -- and it is not a source change: the patch comes back on the same
run, in the same order, in the same bytes.

Build the kernel before the payload, and pass the kernel re-patched by
`scripts/patch-clover-amfi.py`: every fresh prelink restores the two AMFI
branches to stock, so the plain kernel would ship the wrong policy.

Normalise that patched kernel before the payload build. A link cannot reproduce
three fields: the `LC_UUID` the linker mints, the mtime each object-path debug
symbol carries in `n_value`, and the `_PrelinkKCID` that hashes the first two.
Two builds of this identical tree differ in exactly those -- 3,861 bytes of
52,043,776 -- and after this step they are one file. Nothing in the boot
contract reads them: kernelcollection trust is a cdhash over the executable
segments, and the device never parses its own symbol table. The step goes after
`patch-clover-amfi.py`, because the patcher moves bytes the identity fields
would otherwise have to describe; `--check` verifies without rewriting and
`--prove A B` asserts two builds of one tree converge.

The display archive is built before that link, so within one run it audits the
kernel the previous round shipped. Re-run it once the new kernel exists --
`python3 scripts/build-clover-gop-legacy-display.py` inside `out/clover/tree` --
and read the two object hashes in `build/native-legacy-display/build-report.json`
before trusting the image: every pinned binding is an address inside Apple's
family text, and the rebuild is what proves the archive now linked into this
kernel is still qualified against this kernel. `archive_sha256` is not that
evidence; `ar` stamps members with their mtime, so it moves on every run while
the objects stay identical.

After changing the board description or a driver, run both checks:

```sh
scripts/check-clover.py
python3 scripts/verify-clover-payload.py
```

The payload builder accepts only the three-module trust cache made by
`scripts/make-clover-trust-bundle.py`; the one-module bundle is rejected by the
`trust_cache_sha256` pin. A changed kernel or payload needs a cold flash of the
`cache` partition; never use `fastboot boot`, and never write a partition
without saying which one and why.

Report commands, outputs, missing inputs, and hardware observations separately.
Record the boot frame, its readings, and the next discriminating measurement.
When a probe is silent, say whether the probe was not reached or was reached and
printed nothing; those are different facts.

## Porting record

Read the device's `porting.toml` and the
[record guide](https://unixnu.com/porting/records) before implementation.
Update each affected area's findings or evidence with its code change. The
documentation site renders this same record. Follow its schema and completion
levels for both maintainer and agent work.

Every porting area is present even when this board does not implement it.
`display`, `input`, `gpu`, `network`, and `audio` are `planned`, because the
components sheng uses for them are SM8550-specific (MDP/DPU, Novatek touch,
Adreno 740, ath12k, ADSP) and cannot be selected here. `storage` is the active
area: `CloverSdhci` and `CloverBlock` own the eMMC path. The r170 frame showed
the wall is still inside storage, not upstream of it: launchd does reach
`Early boot complete` and execs real daemons, and pid 1 then dies of SIGBUS
because a 102-second checkpoint flush on the Data volume held the block guard
while every other read reported `read guard ... st=00000000` -- contention, not
media. Read a driver failure line by its stage before blaming the card, the
filesystem, or dyld.

XNU patches are replayed by filename order, `git apply --reverse --check` first
and `set -e`. A new probe must never be inserted inside a block added by a
patch that sorts earlier: reversing that patch then fails and the build aborts.
Regenerate the earlier patch itself with `scripts/save-xnu-patch.py` after
reversing the later patches that touch the same file.

`README.md` carries the current status only. The round-by-round log of what each
boot frame showed and what was inferred from it lives in
`notes/development-log.md`; new frames are appended there, and the README's
progress tables are edited in place.
