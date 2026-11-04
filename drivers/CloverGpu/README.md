# CloverGpu — the clock leg it was licensed to take, and one word read from the rail behind it

排期 3's GPU branch asks a two-part question: can XNU bring the Adreno 512 core up far enough to
identify it, and who actually feeds the `gx` domain. Round282 answered the first part with reads only
and wrote nothing, on the standing rule that a first driver must not touch a power state it cannot
undo. Round286 is the write that read was waiting to license. Round311 opened a fourth window and read
one word out of it, on the theory that the answer to the second part is not on the LDO side at all
(`notes/gpu-bringup.md` §5.12: every `qcom,cpr-corner-allow-ldo-mode` on this board is zero, so
`vdd_gfx` is not this board's supply path). Round317 changed no address and wrote nothing new; it moved
the reading-out-of-a-word into `GpuCore.h` so a host can answer it, and doing that corrected this file's
own record of boot 81.

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
`check-clover.py`'s `withRange` count refuses the driver if it stops being four. The addresses are also
bound to the device's own tree by `check_device_tree_constants()` — that gate exists because round181
shipped a base address with a missing digit.

## One boot argument, one mode

`clover-gpu=1` is the only value that runs anything — the `PE_parse_boot_argn("clover-gpu", …)` test at
the head of `clover_gpu_run()`; with any other value the driver returns without touching a register or
creating a timer, which is also what the source's closing `Opt-in through clover-gpu=1` comment claims.
So the mode table here is deliberately one row: this driver's shape is a scheduled instrument with a
beating timer, not a ladder of stages — the ladder lives in `drivers/CloverSdhci` and
`drivers/CloverPower`.

## The write side, and what it still refuses

All stores go through `clover_gpu_write()` or `clover_gpu_set_bits()`, or write a word that `GpuCore.h`
computed (`gpu_gdsc_init_word()`, `gpu_gdsc_release_word()`); round317 deleted the two helpers nothing
called once their arithmetic moved to the layer. Every write is followed by a read-back that proves it,
ordered by the `dsb sy` in `clover_gpu_read()` and `clover_gpu_write()`. That discipline is enforced, not
promised: `check-clover.py` bans indexing a window directly (the `clover_gpu_gcc[` family), bans any
store through the MEM_ACC window (the `clover_gpu_*(clover_gpu_memacc` refusal), and round294 additionally
requires round290's two branch-enable writes to be *absent* again after they had served their purpose as a
control. RCG `0x1070` is read and not reprogrammed (`clover_gpu_report_gpucc()`; the
deliberate-not is stated in the source's `THE SEQUENCE` block), no corner
volts are claimed (`GpuRegs.h:171-172`), and ADC_CTL `0x34` is untouched (`GpuRegs.h:121-122`).

## What `GpuCore.h` owns, and what the host model says about boot 81

Round317 split this driver the way round316 split `drivers/CloverBlock`: the bus stays in
`CloverGpu.cpp`, every decision a word is read through moves to `GpuCore.h`, and the split is checked on
three faces — each decision still exists, the driver calls it, and no second copy survived in the driver
(`check-clover.py` names the raw masks that may not reappear in the `.cpp`, and **runs**
`scripts/test-clover-gpu.py`). The layer re-types no number: it `#include`s `GpuRegs.h` and derives from
its names, so the device-tree binding that gate already performs keeps covering the code the board runs.

| decision | lives in | what the model proves |
| --- | --- | --- |
| the LDO block census against `sdm660_ldo_config[]` | `gpu_ldo_census()`, `gpu_ldo_shape()` | boot 81's thirteen words are 8 comparable / **1 agree** / 7 differ; all 7 non-zero expectations read zero; the shape is `list-never-written`, or `list-unwritten-blind` without the control |
| a read offset and the register it names | `gpu_ldo_word_gpucc_offset()` | an offset walked onto ADC_CTL drops `named` to 12 and the shape becomes `unnamed-mapping`; a swap of two *named* words moves only the counts — the documented limit |
| a dead page | the word test ahead of every mask | thirteen `0xffffffff` give `comparable=0`, `differ=0`, shape `dead-page` — a silent window is never reported as an unprogrammed rail |
| `PWRSWITCH_CTRL` | `gpu_ldo_decode_ctrl()` | boot 81's `0x80000038` is CLAMP_IO \| PDN \| UNDER_SW_CTRL \| **BIT(3) unnamed**; `0xffffffff` decodes as `answered=0`, not as `pdn=0` |
| the corner voltage | `gpu_ldo_decode_vref()` | the reset corner 375,000 uV is **under** this node's own `regulator-min-microvolt` 400,000; `val=0x7f` is 1,010,000 uV, over both maxima |
| the settling question | `gpu_ldo_vendor_settled()` | the vendor's test at `msm_gfx_ldo.c:307` is an OR, so READY alone passes it while SETTLED stays 0 |
| the GDSC write | `gpu_gdsc_init_word()`, `gpu_gdsc_release_word()` | the sequence produces `0x00282000`, the wait field boot 81 read back in **both** domains, each keeping one unnamed bit (CX bit29, GX bit27) |
| the GPUCC gate | `gpu_ahb_gate_open()` | `0xffffffff` has bit31 set, so a vote-only test would open GPUCC behind a dead page |
| the L1 corner select | `gpu_mem_acc_decode()` | masked before the word test, `0xffffffff` becomes corner 1; the wrong order is kept in the layer so the model demonstrates it instead of asserting it was avoided |

The row that carries all of that onto the card is

```
clover r317: gpu t=%u ldo verdict %s shape=%s named=%u/%u comparable=%u agree=%u differ=%u
    nz_zero=%u/%u no_expect=%u outside=%u dead=%u ctrl_pdn=%u ctrl_unnamed=%08x vsettle=%u
    map_ok=%u control=%u
```

printed in the same fire from the same thirteen reads as round294's census row — which is byte-identical,
so boots 81 and 82 stay comparable. `control=` is the same-beat cx_on that licenses the shape, and
`map_ok=` says the driver's own offsets still agree with the layer's table.

That section exists because the recorded verdict for boot 81 was wrong in a way only counting exposed. The
notes said "not one bit matches the vendor's list", a sentence produced by comparing twelve hex words by
eye. One word does match; the list has a ninth entry this repo never quoted; and the claim that survives is
narrower and more useful — every word the init list would write a non-zero value to reads zero, seven of
seven, which is "this list has never run on this board" rather than "the rail is at zero".

## The round311 row, and the arithmetic that makes it safe

The field is one bit wide at bit position 0 (`GpuRegs.h:165-168`) and corner 1's map value is `0x01`
(`:173-176`). A dead page reads `0xffffffff`, and masked into that field that is also `1` — so
`answered=` is decided **on the raw word before any mask** (`gpu_mem_acc_decode()` tests
`kMemAccNoAnswer` and returns before touching `kMemAccFieldMask`; rationale `GpuRegs.h:177-180`, and
`scripts/test-clover-gpu.py` runs the wrong order to show what it reports). It prints in three shapes so
an absence is never read as a reading, all three from the same function: the `map=0` early return (window
refused), the `map=1` with `gpucc=0` row (mapped while the control window is still absent), and the full
`gpucc=1 cx_on= gx_on=` row. The same beat carries a positive control that cannot be empty — the eMMC's
own `ctl_sdhc1_ahb` and
`ctl_sdhc1_apps` votes in `clover_gpu_report_gcc()`, whose comment says why they have to read 1 on a
healthy boot — and `clover_gpu_report_ldo()` with `clover_gpu_report_ldo_words()` print the power state
of the same instant.

Row tokens in the source today: `clover r286:`, `clover r290:`, `clover r291:`, `clover r294:`,
`clover r311:`, `clover r317:`. A new IOLog line takes the round token of the round that adds it, and a
new token needs its own class and cap exemption in `drivers/CloverSdhci` — both pinned in
`check-clover.py`.

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
(`-Wall -Wextra -Werror` against the recorded flags), object ABI is gated, host logic has a model of its
own since round317 — `drivers/CloverGpu/tests/clover_gpu_host.cpp`, driven by `scripts/test-clover-gpu.py`
and run by `check-clover.py`, the way `DspCore.h`, `PowerArb.h` and `BlockCore.h` are — kernel boot is
covered by the on-board loop, and the hardware row is what every flash of this driver is.
