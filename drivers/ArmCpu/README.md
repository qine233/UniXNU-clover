# ArmCpu

ARM64 CPU, GIC and timer driver for UniXNU. Publishes the CPU platform device, the interrupt controller and the architected timer.

Current state: starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

Linked into XNU by `unixnu build kernel`.

## clover (Mi Pad 4, SDM660)

This directory overrides the base module for this board only, with two additions that stay inert unless a boot-arg names them:

| Boot-arg | Effect |
| --- | --- |
| `armcpu-timer-intid=NN` | which PPI is armed in the redistributor and serviced as the timer. 27 and 19 are the *same source* — the non-secure EL1 virtual timer — under the two device-tree PPI numberings Snapdragon trees use: older ones list PPIs 13/14/11/10 (`dodge.dts:7290`), i.e. INTIDs 29/30/27/26, while this board's own node lists 1/2/3/0 (`clover.dts:2233`), i.e. INTIDs 17/18/19/16. `enableCPUTimeBase` arms `CNTV_CTL_EL0`, so clover passes 19; 27 arms a line the GIC was never given. Without the arg the value stays 27. |
| `armcpu-clock-panic=N` | `panic()` on the Nth timer interrupt. Reaching N is itself the reading: it proves the clock runs, from EL1, with no process cooperating, and it turns a silent hang into a photographed panic. The panic screen keeps the first panic, so a probe that finished earlier is untouched. 0 (default) never arms it. |

`armcpu_timer_intid` is published as a global so the panic screen can print which line the kernel actually armed.
