# CloverGpu — the clock leg it was licensed to take, and one word read from the rail behind it

排期 3's GPU branch asks a two-part question: can XNU bring the Adreno 512 core up far enough to
identify it, and who actually feeds the `gx` domain. Round282 answered the first part with reads only
and wrote nothing, on the standing rule that a first driver must not touch a power state it cannot
undo. Round286 is the write that read was waiting to license. Round311 opened a fourth window and read
one word out of it, on the theory that the answer to the second part is not on the LDO side at all
(`notes/gpu-bringup.md` §5.12: every `qcom,cpr-corner-allow-ldo-mode` on this board is zero, so
`vdd_gfx` is not this board's supply path).

It is still **not** an Adreno driver: the source says so in its own header comment, under
`WHY THIS ROUND WRITES`. It powers and clocks the core, then reads `chipid`, and stops — no GMU, no
HFI, no command processor, no rings. The desktop remains CPU-first.

## The four windows, and where each address came from

| window | physical | size | DT node | mapped |
| --- | --- | --- | --- | --- |
| GCC | `0x00100000` | `0x94000` | `clock-controller@100000` (`GpuRegs.h:4-5,21-22`) | first, `clover_gpu_map_gcc()` |
| GPUCC | `0x05065000` | `0x10000` | `gfx@5065000` (`GpuRegs.h:43-44`) | lazily, after the AHB vote: `clover_gpu_open_gpucc()` |
| GPU core | `0x05000000` | `0x40000` | `qcom,kgsl-3d0@5000000` (`GpuRegs.h:207-208`) | gated: `clover_gpu_open_core()` |
| MEM_ACC | `0x01fcf004` | `0x4` | `regulator@01fcf004`, `reg-names = "acc-sel-l1"` (`GpuRegs.h:161-162`) | round311: `clover_gpu_open_mem_acc()` |

Every one of these is `IODeviceMemory::withRange(...)->map(kIOMapAnywhere | kIOMapInhibitCache)`; the
LDO block rides the GPUCC extent instead of opening a fifth map (`GpuRegs.h:85-88`), and
`check-clover.py:3232-3236` refuses the driver if the `withRange(` count stops being four. The
addresses are also bound to the device's own tree by `check_device_tree_constants()` — that gate exists
because round181 shipped a base address with a missing digit.

## One boot argument, one mode

`clover-gpu=1` is the only value that runs anything — the `PE_parse_boot_argn("clover-gpu", …)` test at
the head of `clover_gpu_run()`; with any other value the driver returns without touching a register or
creating a timer, which is also what the source's closing `Opt-in through clover-gpu=1` comment claims.
So the mode table here is deliberately one row: this driver's shape is a scheduled instrument with a
beating timer, not a ladder of stages — the ladder lives in `drivers/CloverSdhci` and
`drivers/CloverPower`.

## The write side, and what it still refuses

All stores go through four helpers — `clover_gpu_write`/`set_bits`/`clear_bits`/`update_bits` — and
every write is followed by a read-back that proves it, ordered by the `dsb sy` in `clover_gpu_read()`
and `clover_gpu_write()`. That discipline is enforced, not promised: the checker bans indexing a window
directly (`check-clover.py:3201-3205`), bans any store through the MEM_ACC window
(`:3213-3219`, the `clover_gpu_*(clover_gpu_memacc` refusal), and round294 additionally requires
round290's two branch-enable writes to be *absent* again (`:3192-3197`) after they had served their
purpose as a control. RCG `0x1070` is read and not reprogrammed (`clover_gpu_report_gpucc()`; the
deliberate-not is stated in the source's `THE SEQUENCE` block), no corner
volts are claimed (`GpuRegs.h:171-172`), and ADC_CTL `0x34` is untouched (`GpuRegs.h:121-122`).

## The round311 row, and the arithmetic that makes it safe

The field is one bit wide at bit position 0 (`GpuRegs.h:165-168`) and corner 1's map value is `0x01`
(`:173-176`). A dead page reads `0xffffffff`, and masked into that field that is also `1` — so
`answered=` is decided **on the raw word before any mask** (`clover_gpu_report_mem_acc()` compares
against `kMemAccNoAnswer` first and masks only inside that branch; rationale `GpuRegs.h:177-180`); the
reason this row is worth writing down at all. It prints in three shapes so an absence is never read as a
reading, all three from the same function: the `map=0` early return (window refused), the `map=1` with
`gpucc=0` row (mapped while the control window is still absent), and the full `gpucc=1 cx_on= gx_on=`
row. The same beat carries a positive control that cannot be empty — the eMMC's own `ctl_sdhc1_ahb` and
`ctl_sdhc1_apps` votes in `clover_gpu_report_gcc()`, whose comment says why they have to read 1 on a
healthy boot — and `clover_gpu_report_ldo()` with `clover_gpu_report_ldo_words()` print the power state
of the same instant.

Row tokens in the source today: `clover r286:`, `clover r290:`, `clover r291:`, `clover r294:`,
`clover r311:`. A new IOLog line takes the round token of the round that adds it.

## How this driver meets the UniXNU driver documentation

Layer and wiring follow `docs/drivers/overview.md:11-16` (built-in XNU driver) and
`docs/drivers/xnu-drivers.md:118-124` (archive from `[kernel] archives`, `build-arm-driver.py
CloverGpu`). The addresses are board constants rather than provider properties exactly as
`docs/drivers/xnu-drivers.md:215` prescribes for a device below the `arm-io` range. Object ABI follows
`docs/drivers/xnu-drivers.md:106-116` and is checked by `scripts/build-arm-driver.py:120,129`.

Two documented parts it does **not** have, and why: no IOKit class and no personality, so no
`docs/drivers/xnu-drivers.md:19-64` class shape and no `Info.plist` reference record. It is reached as
`extern "C" void clover_gpu_run(void)`, chained from `CloverPMGR`, and the reason — plus the three
concrete prerequisites for giving it a personality — is `notes/unixnu-doc-conformance.md` §5. The one
thing it takes from IOKit is the resources workloop for its timer, which `clover_gpu_run()` asks the
owner for with `getWorkLoop()`.

Testing, against the five rows of `docs/drivers/xnu-drivers.md:230-239`: compile is gated
(`-Wall -Wextra -Werror` against the recorded flags), object ABI is gated, host logic has **no model of
its own** — `DspCore.h`/`PowerArb.h` show what that gap looks like closed, and it is listed as an open
item in `notes/unixnu-doc-conformance.md` §7 — kernel boot is covered by the on-board loop, and the
hardware row is what every flash of this driver is.
