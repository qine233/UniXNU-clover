# ArmCpu

ARM64 CPU, GIC and timer driver for UniXNU. Publishes the CPU platform device, the interrupt controller and the architected timer.

Current state: starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

Linked into XNU by `unixnu build kernel`.

## clover (Mi Pad 4, SDM660)

This directory overrides the base module for this board only. Every addition stays inert unless a
boot-arg names it; the args this board actually ships are marked **shipped** (`board.toml`'s
`kernel_args`), the rest exist for control rounds:

| Boot-arg | Effect |
| --- | --- |
| `armcpu-timer-intid=NN` **shipped (19)** | which PPI is armed in the redistributor and serviced as the timer. 27 and 19 are the *same source* — the non-secure EL1 virtual timer — under the two device-tree PPI numberings Snapdragon trees use: older ones list PPIs 13/14/11/10 (`dodge.dts:7290`), i.e. INTIDs 29/30/27/26, while this board's own node lists 1/2/3/0 (`clover.dts:2233`), i.e. INTIDs 17/18/19/16. `enableCPUTimeBase` arms `CNTV_CTL_EL0`, so clover passes 19; 27 arms a line the GIC was never given. Without the arg the value stays 27. |
| `armcpu-clock-panic=N` | `panic()` on the Nth timer interrupt, reporting where the interrupted context was. Reaching N is itself the reading: it proves the clock runs, from EL1, with no process cooperating, and it turns a silent hang into a photographed panic. The panic screen keeps the first panic, so a probe that finished earlier is untouched. 0 (default) never arms it. |
| `armcpu-smp=1` **shipped** | opt in to bringing up the secondary cores; `probe()` refuses any CPU nub whose `ml_get_cpu_number()` is not the boot CPU unless this is set. `armcpu-smp-remote-gic=1` is the other half of the same question: the boot CPU prepares a new core's redistributor before `CPU_ON` instead of the core doing it itself. |
| `armcpu-self-ipi=1` **shipped** | deliver the inter-processor signal XNU's `machine.c` expects after a CPU is online, by SGIs to self. |
| `armcpu-spi-ipi=1` | route IPIs as software-pended SPIs `1008 + logical core` instead — the source's own reason is that `ICC_SGI1R_EL1` generates nothing on this board, and its `kIpiSpiBase` comment names those eight INTIDs as 'IPI to logical core 0..7'. |
| `armcpu-skip-gic=1` | start without `prepareGic()`, for rounds that want the clock alone. |
| `armcpu-diagnostics=1` | in the interrupt handler, `panic()` if one pass drains more than 128 IRQs — a stuck line then says which CPU it is stuck on instead of livelocking the core quietly. |
| `clover-unattended=NN` **shipped (1800)** | the unattended deadline: the timer beat counts down it, and at zero the board photographs itself into fastboot rather than sitting dark. |

## How this driver meets the UniXNU driver documentation

ArmCpu is the positive control in `notes/unixnu-doc-conformance.md` §3 — the module that shows what the
documented shape looks like when all of it is present.

Layer: `docs/drivers/overview.md:11-16` puts CPU, interrupt and timer drivers in the built-in XNU layer,
and this module is wired exactly as `docs/drivers/xnu-drivers.md:118-124` and
`docs/porting/device-repository.md:90-113` describe: `build-arm-driver.py ArmCpu` in `board.toml`'s
`[kernel] archives`, `CONFIG_XNU_ARMCPU=y` in `configs/clover_defconfig`, and
`build-xnu-4k-baseline.sh:857` links the resulting `build/armcpu/libArmCpu.a` into the kernel. The
compile is `-Wall -Wextra -Werror` against the recorded `.cpo.json` flags, and the object ABI of
`docs/drivers/xnu-drivers.md:106-116` is the five-field audit `build-arm-driver.py` prints as "actual
ARM64/iOS 17 object audit passed".

Layout and personality: `docs/drivers/xnu-drivers.md:7-14` asks for the source plus an `Info.plist`
recording the personality, and `drivers/ArmCpu/Info.plist` is exactly that — `IOClass = ArmCpu`,
`IOProviderClass = IOPlatformDevice`, `IOProbeScore = 2000`. The dictionary the kernel actually reads is
the compiled one in `iokit/KernelConfigTables.cpp`, installed by the base repository's
`patches/xnu/armvirt-platform-personality.patch`, which is the mechanism of
`docs/drivers/xnu-drivers.md:147-168` and `docs/guide/build-kernel.md:135`. It is one documented
departure: the patch adds the dictionary unconditionally rather than behind a `-DXNU_<NAME>` macro, so
`KernelConfigTables.cpo_CFLAGS_ADD` (`build-xnu-4k-baseline.sh:470`) carries only the `XNU_NATIVE_*`
personalities and ArmCpu is always in the table.

Class shape and runtime stages: `docs/drivers/xnu-drivers.md:19-64` and the stage table at `:219-228` —
`ArmCpu : public IOCPU` with the standard `OSDeclareDefaultStructors`/`OSDefineMetaClassAndStructors`, a
`probe()` that checks the provider's own `device_type` and `reg` properties before handing the score to
`IOCPU::probe()`, and `registerService()` only after `start()` succeeded. Reading geometry from the
provider is what `docs/drivers/device-tree-matching.md:108-128` asks for; the clover instruments in this
repository cannot do that and take board constants instead, per `docs/drivers/xnu-drivers.md:215` — see
that drivers' READMEs and `notes/unixnu-doc-conformance.md` §5.

Overrides: a device repository keeping its own copy of a base module is the documented layout
(`docs/porting/device-repository.md:13`, `drivers/` holds device XNU drivers); how the two copies resolve
is recorded in `drivers/NativeHidInput/README.md`.

## What the watchdog panic says

```
ArmCpu: clock watchdog at 60000 ticks (elr=… spsr=… elr2=… cnt0=… cnt1=… freq=…)
```

`elr` is `ELR_EL1` read at the very entry to `ArmCpu::interrupt()`, before anything here can nest a
second exception: the address the interrupted context was executing. `spsr` is `SPSR_EL1` taken in the
same breath, and its low nibble is the exception level that address belongs to -- 5 is EL1h, the kernel,
0 is EL0t, user. `elr2`/`spsr` are re-read at the panic call itself; if `elr` and `elr2` differ, the first
one is the sample and the second is the path into `panic()`. `cnt0` is `CNTVCT_EL0` at the first tick and
`cnt1` at the Nth, so `cnt1 - cnt0` over `N - 1` ticks against `freq` (`CNTFRQ_EL0`) is the first actual
measurement of this board's tick period; until round86 the period was only ever assumed.

The report goes through `panic()`, not through the console, because the thing under trial *is* the
console: `kprintf` from inside `ttwrite` or `kmoutput` re-enters the ring buffer and the write lock being
measured, and `console_ring_try_empty` spins without bound while holding it. A tick interrupt cannot be
masked by the code it samples for long, so the sampled address arrives even when every userspace write
has stopped producing output.

Read the addresses with `scripts/decode-clover-address.py`: it names the containing function, the offset
into it, the enclosing loop if the address is inside one, and the instructions around it, all against
`out/clover/kernel/kernel.macho`. `--suspects` prints the tty and console ranges for the current image.


`armcpu_timer_intid` is published as a global so the panic screen can print which line the kernel actually armed.
