# CloverSdhci — asking the eMMC host a question without writing to it, and then booting from the answer

clover boots from eMMC. Every byte this port has ever executed -- the kernel, the prelinked
image, the AMFI trust cache, the root ramdisk, the flat device tree -- came out of the card
behind the host at `0xc0c4000`, under a driver we cannot read: the firmware's only SDHCI driver
is `SdccDxe`, shipped as a binary blob
(`uefi-clover/Binaries/clover/QcomPkg/Drivers/SdccDxe/SdccDxe.inf`). There is no init sequence in
this repository to copy, no known-good clock tree, no known-good tuning state, and no upstream
XNU SDHCI host to port.

What there is, is a controller that was demonstrably working a few milliseconds before the kernel
took over, and a card the firmware demonstrably left in transfer state. So the first round asks
exactly one thing of it:

> **does the host still answer, and what state did the firmware leave it in?**

## Mode 1: look, do not touch

`clover-emmc=1` maps the host window read-only and fetches about thirty registers. It issues no
command, sets no clock, touches no reset line, enables no interrupt, and writes nothing anywhere
-- including to the interrupt status register, which is the tempting one.

This is the control, not the shipping mode. The board asks for `clover-emmc=21` (`board.toml:1968`),
and round87's one-question rule is what mode 21 breaks: it is a boot rather than a photograph. The
sections below say where the ladder between them runs and what mode 21 stops doing.

The register map is the SDHCI system address block (`SdhciRegs.h` transcribes it with a file:line
citation per offset) plus the two candidate layouts Qualcomm's `sdhci-msm.c` knows for its vendor
extensions, carried at `SdhciRegs.h:252-253`: `sdhci_msm_offset_mci_removed` (that file's line 206)
is MCI version 0x318 / generics 0x320 / DLL 0x200+0x208 and is read relative to the SDHCI register
base, `sdhci_msm_offset_mci_present` (its line 239) is 0x050 / 0x070 / 0x100 / 0x108 and is read
relative to a separate `core_mem` base. Both are read, because which one is a register on this SDCC
is exactly the kind of thing the device tree does not say.

Those two vendor line numbers point into Linux trees this repository re-fetches rather than keeps;
`SdhciRegs.h:5-14` records which tree and which saved copy each block came from, and the in-repo
citation is always to the header.

How to read the photograph:

| what appears | what it means |
| --- | --- |
| `cap=` with a base clock in the 50..400 MHz field range and `ver=` between 1.0 and 4.2 | the window is live, the SDCC clocks are running, the bus is not hung -- proceed to a command |
| every field `00000000` | the interface clock is gated; the next round is a clock-tree question, not a storage one |
| every field `ffffffff` | the read aborted at the NoC; the window is not addressable from this kernel's page tables |
| `pst=` with `SDHCI_PRESENT_STATE_COMMAND_INHIBIT` set | the firmware left a transaction open; a command must not be issued before it clears |
| `clk=` / `div=` / `sd=` | the SDRAM clock the firmware last programmed, from `CLOCK_CONTROL` -- the frequency a later CMD must be valid at |
| `arg= rsp=` | the firmware's last command and its response, free of charge |
| `msm v50/318` | the layout question answered: whichever side is a small version-looking number |

The whole dump is drawn on one panic screen, because a photograph is the only output here and a
silent success would be indistinguishable from a hang. That panic is the reading, not a failure --
and it is still what every mode but 21 ends on, because those modes exist only to answer a question.

## Why it runs from `sheng_pmgr_run()`

An in-kernel UniXNU driver normally starts from a personality compiled into
`iokit/KernelConfigTables.cpp` by a patch (`docs/drivers/xnu-drivers.md:14`, `:149`;
`docs/guide/build-kernel.md:135`). This section used to say a device repository cannot add one, and
round314 corrected that: 73 clover patches are tracked in the base repository's `patches/xnu` today,
and the assembled tree links both `out/clover/tree/patches/xnu` -> base and
`out/clover/tree/patches/xnu-clover` -> this repository. What is true is the narrower thing the
directory names already show -- the kernel applies the *base* series, so a clover patch must be
committed there to run and this repo keeps the reviewed copy, because a device-side directory named
`patches/xnu` would shadow the upstream set instead of joining it.

What keeps this driver off the documented path is three concrete prerequisites, not a missing
permission, and `notes/unixnu-doc-conformance.md` §5 lists them: the ADT publishes only one `/arm-io`
child (`intc`), so there is no provider for `IONameMatch` to find; `IOProbeScore` orders candidates
but cannot express "the DSP census runs after the block driver has armed the rings", which is what
this chain of C entry points does express; and the archives link without `-Wl,-force_load`, so a class
only a personality would reference would be dropped.

So `clover_emmc_run()` is exported and called at the end of `sheng_pmgr_run()`, the
`bsd/kern/bsd_init.c` hook this board has run through since round83 (`CONFIG_XNU_SHENG_PMGR=y`,
after `bsd_bufferinit`, with IOKit up and the panic screen armed). That is a scheduling decision,
not a design one: closing it means those three items, in that order.

It runs after the PMGR publication on purpose. If CloverPMGR fails, its panic comes first and
names its own stage; the eMMC dump only appears on a boot where the PMGR step held.

## Link wiring

| piece | file | note |
| --- | --- | --- |
| compile | `scripts/build-arm-driver.py CloverSdhci` | the same recorded `IOPlatformExpert.cpo.json` flags every other device driver uses, plus the ARM64/iOS 17 object audit |
| archive | `build/clover-sdhci/libCloverSdhci.a` | named by the `[kernel] archives` row `board.toml:69` |
| link | `configs/clover_defconfig` → `CONFIG_XNU_EXTRA_LD_ARCHIVES` (`:70`) | appended **raw**, not through `-Wl,-force_load`, at `scripts/build-xnu-4k-baseline.sh:1002`; `configs/clover_defconfig:66-69` says why that is enough here |
| schedule | `drivers/CloverPMGR/CloverPMGR.cpp` | `clover_emmc_run()` at the tail of `sheng_pmgr_run()` |
| switch | `board.toml` → `kernel_args` | `clover-emmc=21` (`board.toml:1968`) |
| verify | `scripts/verify-clover-payload.py` | greps `kernel.macho` for the driver's own strings and both exported symbols (`_clover_emmc_run`, `_clover_emmc_block_read`) -- `:129-146`; the plain-archive link is what makes this check necessary rather than redundant |

## What came after mode 1

The three things this file used to list as "what comes after" are all built and booted: CMD13
(mode 2, round88), the first data read (mode 10, round96) and the log partition (mode 17 stored the
report as text on the card, round103; round174 and round176 turned that into the two rings the
unattended loop reads back, and round185 moved the window to `logdump`). Between them sit the
identify chain (modes 7--9), the drain-pace findings (modes 11--13), high-speed plus multi-block
write (round159, batched to 128 blocks in round171), ADMA2 (rounds 172--173), the recovery BCB
pre-arm (round198) and the whole-card re-initialisation that replaced waiting on a watchdog
(rounds 213--215).

Mode 21 is where that ladder lands, and it is deliberately not a report: `clover_emmc_run()` calls
`clover_block_request()` and **returns**, so the boot continues into `drivers/CloverBlock`; the
`panic()` at the end of the function is unreachable in mode 21 by design. Every other mode still
ends on the panic screen with its report. Per-mode history is recorded mode-by-mode in this file's
own `WHY THIS FILE EXISTS IN THIS SHAPE` header block and the comments above each stage function --
that block, not this README, is the authoritative ladder.

## What mode 1 does not do -- and what mode 21 does

Mode 1 writes nothing, enables no interrupt, issues no command and touches no reset line, including
the write-1-to-clear `INT_STATUS` that is the tempting one. Mode 21 does write: the verdict and
kernel-log rings go to the card on every beat, the recovery BCB is pre-armed, and through
`_clover_emmc_block_read` it serves reads of the userdata window that `drivers/CloverBlock` turns
into the boot's root volume. What has stayed true of every mode is the boundary this driver does not
cross itself: no block device, no `IOMedia`, no root volume and no filesystem is published here --
that is `drivers/CloverBlock`'s job, asked for by mode 21's tail call.

The three windows (host, GCC, IMEM) are mapped `kIOMapAnywhere | kIOMapInhibitCache` into static
`OSSharedPtr` slots by `mapRange()` and never released, because nothing here may be freed at
`bsd_bufferinit` time. The safety net is no longer a tick-count watchdog: `armcpu-clock-panic` is
**not** in this board's `kernel_args` (the shipped `armcpu-*` rows are `armcpu-timer-intid=19`,
`armcpu-smp=1` and `armcpu-self-ipi=1`). What guards a hung MMIO read today is
`clover-unattended=1800` (`board.toml:2597`) -- a wall-clock deadline measured on CNTVCT inside
`drivers/ArmCpu`, which writes the flight record and resets into fastboot rather than leaving a
photograph nobody is there to take. Its `elr=`/PC fields still name the stuck instruction for
`scripts/decode-clover-address.py`.

## How this driver meets the UniXNU driver documentation

Layer and wiring are the documented ones: this is a built-in XNU driver, not a kext
(`docs/drivers/overview.md:11-16`); it is compiled from `[kernel] archives`
(`board.toml:69`) by `scripts/build-arm-driver.py CloverSdhci` as `docs/drivers/xnu-drivers.md:118-124`
and `porting/device-repository.md:90-113` prescribe, and the object ABI of
`docs/drivers/xnu-drivers.md:106-116` is enforced rather than promised -- `build-arm-driver.py:120,129`
refuses anything whose magic/CPU/subtype/filetype is not `(0xfeedfacf, 0x100000c, 0, 1)` or whose
build version is not platform 2 at minimum OS `0x110000`. The layout of `docs/drivers/xnu-drivers.md:7-14`
is the driver's own three files (`CloverSdhci.cpp`, `SdhciRegs.h`, this README) with no `.inf` and no
`Info.plist`; the missing plist is the correct shape here, because `:14` treats the plist as the record
of a personality and this driver has none to record. Its addresses are board constants pinned to the
device's own tree exactly as `docs/drivers/xnu-drivers.md:215` prescribes for a device below the
`arm-io` range (`kSdhciHostPhys` / `kSdhciHostSize` at `SdhciRegs.h:30-31`, `sdhci@c0c4000`).

Two documented parts it does not have, with the reason measured rather than asserted: no IOKit class
(`docs/drivers/xnu-drivers.md:19-64`) and no personality (`:147-168`, `docs/guide/build-kernel.md:135`).
It is reached as `extern "C" void clover_emmc_run()`, and `-Wl,-force_load` (`docs/drivers/xnu-drivers.md:128-145`)
is deliberately not used -- `configs/clover_defconfig:66-69` states the narrow reason that makes the
omission safe (CloverPMGR names the symbol, so ordinary archive resolution pulls the member in), and
`scripts/verify-clover-payload.py:129-146` is what proves it held in the linked kernel. The three
prerequisites for putting this driver on the documented path, in order, are
`notes/unixnu-doc-conformance.md` §5.

Against the five testing rows of `docs/drivers/xnu-drivers.md:230-239`: compile is gated
(`-Wall -Wextra -Werror` against the recorded `IOPlatformExpert.cpo.json` flags), object ABI is gated,
kernel boot is the on-board loop every flash of this driver is, hardware is the loop too, and host
logic has **no model of its own**. That is this port's largest instrument with no `tests/` directory:
the completion criteria, the ring arithmetic and the per-token tee budgets (the `TeeProbeClass` table
`kTeeProbeClasses`) are the parts that would benefit most from the treatment
`drivers/CloverDsp/tests/clover_dsp_host.cpp` and `drivers/CloverPower/tests/clover_power_host.cpp`
give their drivers -- rule 3 of `docs/drivers/sheng-novatek-touch.md:141-148`. It is open item 3 of
`notes/unixnu-doc-conformance.md` §7; what gates this file today instead is `check-clover.py`, whose
`drivers/CloverSdhci` blocks assert the build/link/argument/schedule triangle (`check-clover.py:625-641`),
the round198 recovery-BCB line, and one `kTeeProbeClasses` row per reporting round.
