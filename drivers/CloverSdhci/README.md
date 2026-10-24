# CloverSdhci — asking the eMMC host a question without writing to it

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

The register map is the SDHCI system address block (`SdhciRegs.h` transcribes it with a file:line
citation per offset) plus the two candidate layouts Qualcomm's `sdhci-msm.c` knows for its vendor
extensions (`sdhci-msm.c:206` = MCI version 0x318 / generics 0x320 / DLL 0x200+0x208, and `:239` =
0x050 / 0x070 / 0x100 / 0x108). Both are read, because which one is a register on this SDCC is
exactly the kind of thing the device tree does not say.

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
silent success would be indistinguishable from a hang. That panic is the reading, not a failure.

## Why it runs from `sheng_pmgr_run()`

An in-kernel UniXNU driver normally starts from a personality compiled into
`iokit/KernelConfigTables.cpp` by a patch under `patches/xnu` (`docs/drivers/xnu-drivers.md:14`,
`:149`). A device repository cannot add one: `feeds.py` resolves modules by `(kind, name)` and
takes the first path that exists, so a device-side `patches/xnu` would *replace* the base
repository's directory -- hundreds of upstream patches -- instead of extending it. round86 hit the
same wall from the other side, for `osfmk/console`.

So `clover_emmc_run()` is exported and called at the end of `sheng_pmgr_run()`, the
`bsd/kern/bsd_init.c` hook this board has run through since round83 (`CONFIG_XNU_SHENG_PMGR=y`,
after `bsd_bufferinit`, with IOKit up and the panic screen armed). That is a scheduling decision,
not a design one: the day this port has its own patch slot, this driver gets its own personality
and the call disappears.

It runs after the PMGR publication on purpose. If CloverPMGR fails, its panic comes first and
names its own stage; the eMMC dump only appears on a boot where the PMGR step held.

## Link wiring

| piece | file | note |
| --- | --- | --- |
| compile | `scripts/build-arm-driver.py CloverSdhci` | the same recorded `IOPlatformExpert.cpo.json` flags every other device driver uses, plus the ARM64/iOS 17 object audit |
| archive | `build/clover-sdhci/libCloverSdhci.a` | |
| link | `configs/clover_defconfig` → `CONFIG_XNU_EXTRA_LD_ARCHIVES` | `-Wl,-force_load`, appended verbatim at `scripts/build-xnu-4k-baseline.sh:732` |
| schedule | `drivers/CloverPMGR/CloverPMGR.cpp` | `extern "C" void clover_emmc_run (void);` at the tail of `sheng_pmgr_run()` |
| switch | `board.toml` → `kernel_args` | `clover-emmc=1` |
| verify | `scripts/verify-clover-payload.py` | checks the two panic strings and `clover_emmc_run` survived into the linked kernel |

## What comes after

One flash each, in this order:

1. **CMD13 SEND_STATUS** (`ac`, R1) -- no data, so it proves the command path, the response
   registers, and the interrupt/polling handshake without a buffer in the way.
2. **CMD17 READ_SINGLE_BLOCK at LBA 1** (`adtc`, R1) -- the first five bytes of that block are the
   GPT magic `EFI PART`. A read that checks itself.
3. **The log partition** -- the thing this is all for: the kernel writes its own panic/boot log
   into a partition, and it is read back off the device. `UniXNU-sheng` and the reference port
   `ref/tb128fu` carry that blueprint end to end.

## What this is not

No block device, no `IOMedia`, no root volume, and not one byte of user data read. The window is
mapped `kIOMapInhibitCache` and never released, because nothing here may be freed at
`bsd_bufferinit` time. The tick watchdog (`armcpu-clock-panic=60000`) stays armed: if a plain
MMIO read of a gated SDCC hangs the interconnect, the panic that follows is the watchdog's, and
its `elr=` names the stuck instruction for `scripts/decode-clover-address.py`.
