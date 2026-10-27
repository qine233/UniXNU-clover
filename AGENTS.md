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
./unixnu build kernel --device ../UniXNU-clover
./unixnu build payload --device ../UniXNU-clover --unique-chip-id HEX \
  --uefi ../mu-clover \
  --kernel PATH/roundNNN-kernel-amfi.macho \
  --data-ramdisk ../UniXNU-clover/build/clover-root/launchd.macho \
  --trust-cache ../UniXNU-clover/build/clover-trust/trustcache.bundle
```

Build the kernel before the payload, and pass the kernel re-patched by
`scripts/patch-clover-amfi.py`: every fresh prelink restores the two AMFI
branches to stock, so the plain kernel would ship the wrong policy.

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
area: `CloverSdhci` and `CloverBlock` own the eMMC path, and reads and writes
both have device readings that are clean. The remaining wall is upstream of
storage: whether the daemons launchd execs survive.

XNU patches are replayed by filename order, `git apply --reverse --check` first
and `set -e`. A new probe must never be inserted inside a block added by a
patch that sorts earlier: reversing that patch then fails and the build aborts.
Regenerate the earlier patch itself with `scripts/save-xnu-patch.py` after
reversing the later patches that touch the same file.

`README.md` carries the current status only. The round-by-round log of what each
boot frame showed and what was inferred from it lives in
`notes/development-log.md`; new frames are appended there, and the README's
progress tables are edited in place.
