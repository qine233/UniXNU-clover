# ArmCpu

ARM64 CPU, GIC and timer driver for UniXNU. Publishes the CPU platform device, the interrupt controller and the architected timer.

Current state: starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

Linked into XNU by `unixnu build kernel`.

## clover (Mi Pad 4, SDM660)

This directory overrides the base module for this board only, with two additions that stay inert unless a boot-arg names them:

| Boot-arg | Effect |
| --- | --- |
| `armcpu-timer-intid=NN` | which PPI is armed in the redistributor and serviced as the timer. 27 and 19 are the *same source* — the non-secure EL1 virtual timer — under the two device-tree PPI numberings Snapdragon trees use: older ones list PPIs 13/14/11/10 (`dodge.dts:7290`), i.e. INTIDs 29/30/27/26, while this board's own node lists 1/2/3/0 (`clover.dts:2233`), i.e. INTIDs 17/18/19/16. `enableCPUTimeBase` arms `CNTV_CTL_EL0`, so clover passes 19; 27 arms a line the GIC was never given. Without the arg the value stays 27. |
| `armcpu-clock-panic=N` | `panic()` on the Nth timer interrupt, reporting where the interrupted context was. Reaching N is itself the reading: it proves the clock runs, from EL1, with no process cooperating, and it turns a silent hang into a photographed panic. The panic screen keeps the first panic, so a probe that finished earlier is untouched. 0 (default) never arms it. |

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
