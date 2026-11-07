# `dload_type@1c` and `restart_reason@65c` -- what the vendor kernel actually writes

Round338 host-side forensics. Two claims this round went looking for one answer and came back with the
opposite, and one claim an earlier round of this port has been shipping in a constant's name. Every line
number below is in a file archived in this directory -- `msm-poweroff.c`, `boot_stats.c`,
`qpnp-power-on.h` -- except where a sentence names a tree that is not archived, and the greps are written
out so the next reader can re-run them rather than trust this page.

Source: `notes/sources/sdm660/msm-poweroff.c` -- Xiaomi's 4.19 CAF tree for this exact tablet
(E7G/clover-sdm660-ksb @ 5c81a3faf37e332be5008512c9b2dc2444dbcc01, `drivers/power/reset/msm-poweroff.c`,
734 lines, sha256 `febf2eccb8526a4e8738e201de0d20f8d3c63d2b1863db69fcc0ce9313899fb9`), archived verbatim.
`boot_stats.c` (`drivers/soc/qcom/boot_stats.c`, 105 lines, sha256 `4fd7113b1755f4dcd88b5b187946f692…`)
and `qpnp-power-on.h` (`include/linux/input/qpnp-power-on.h`, 142 lines, sha256
`c6c27856db96ea9c66bd820c16a9206e…`) come from the same commit. Cross-checked against LineageOS
`android_kernel_xiaomi_sdm660` (lineage-20), same `msm-poweroff.c`, 690 lines: the reason table agrees on
every value it compiles; the three lines it lacks are the three behind `CONFIG_MACH_XIAOMI_CLOVER`
(panic `0x77665508`, `"other"` `0x77665501`, and the probe-time `0x77665510`).

## 1. `dload_type@1c` is a download *destination*, not an EDL trigger

This round's plan said the cell at IMEM +0x1c -- `compatible = "qcom,msm-imem-dload-type"`,
`reg = <0x1c 4>` -- was "the EDL arm cell", on the theory that boots 118/119 ending in EDL pointed at a
memory cell some firmware arms. It is not. Measured in the archived file:

| line | what it does |
|---|---|
| 54 | `#define IMEM_DL_TYPE_PROP "qcom,msm-imem-dload-type"` -- the same string the DT child carries |
| 33 | `#define EMMC_DLOAD_TYPE 0x2` |
| 311 | `dload_type_addr = map_prop_mem(IMEM_DL_TYPE_PROP)` -- the address comes from that compatible, via `of_find_compatible_node()` + `of_iomap()` |
| 388 | `__raw_writel(EMMC_DLOAD_TYPE, dload_type_addr)` -- when `/sys/kernel/dload/emmc_dload` is set to 1 |
| 390 | `__raw_writel(0, dload_type_addr)` -- when it is set to 0 |
| 362-366 | `__raw_readl(dload_type_addr)` and a comparison against `EMMC_DLOAD_TYPE` -- the read-back that prints the current destination |

So the two values this cell is ever given are 0 and 2, and the vendor's own names for them are
"download over USB/QPST" and "download over eMMC" -- a *destination selector*, one generation of the same
idea that in `android_kernel_xiaomi_sm8550` is called `QCOM_DOWNLOAD_DEST_EMMC = 2` in
`drivers/power/reset/qcom-dload-mode.c`. Writing 2 there does not ask the board to enter EDL; it asks an
EDL that is already happening to come from the card instead of the cable.

Two more negatives, both measured rather than remembered:

- `drivers/soc/qcom/dload_mode.c` **does not exist** in either sdm660 tree. The writer of this cell is
  `drivers/power/reset/msm-poweroff.c`, and `ls notes/sources/sdm660/ | grep -ci dload_mode` prints 0.
  A next hand that went looking for `dload_mode.c` on sdm660 would find nothing and could easily read
  that as "the vendor never touches +0x1c", which is the opposite of true.
- There is no `0x776655AA`/`DLOAD_TYPE_MAGIC`/`EMERG_DLOAD_ENABLE` anywhere in these trees.

## 2. What actually arms EDL on clover is an SMC, and its DT door is absent on this board

`enable_emergency_dload_mode()` is at lines 202-227 and has two legs:

- 202-218: write three magics (`:30-32` -- `0x322A4F99`, `0xC67E4350`, `0x77777777`) to the cell mapped
  from `EDL_MODE_PROP` (`:53` -- `"qcom,msm-imem-emergency_download_mode"`), then `qpnp_pon_wd_config(0)`
  so the PMIC watchdog does not auto-reset. **Guarded by `if (emergency_dload_mode_addr)`** -- and that
  address is NULL on this board:

      grep -c -i emergency notes/sources/sdm660/sdm660.dtsi                     -> 0
      grep -c -i emergency ~/uefi-clover/Resources/DTBs/clover.dts              -> 0
      grep -c -i download_mode notes/sources/sdm660/sdm660.dtsi                 -> 0
      grep -c -i download_mode ~/uefi-clover/Resources/DTBs/clover.dts          -> 0

  (both files, `grep -c ''` = 2,941 and 16,612 lines; `dload` itself matches 4 times in each -- the four
  unrelated compatibles `mem_dump_table`, `dload_type`, `diag_dload`, `ss_mdump`.) So neither the vendor
  dtsi nor the shipping board tree declares `emergency_download_mode` or `download_mode`, and the memory
  leg of EDL arming has no declared door on this board -- the same shape as the SDAM result in
  `imem-children.dts`.
- 224-227: `scm_set_dload_mode(SCM_EDLOAD_MODE, 0)` -- an SMC into TrustZone/SBL. That is the leg that is
  live here. `adb reboot edl` reaches it: `:559-567` compiles `enable_emergency_dload_mode()` unless
  `CONFIG_MACH_LONGCHEER` or `CONFIG_MACH_XIAOMI_JASON`, and CLOVER is not in that `#if`, and `:491-500`
  forces `PON_POWER_OFF_WARM_RESET` when a dload was requested.

So the EDL question is not "which IMEM word do we write" but "what the secure boot monitor was told, and
whether it survives the reset kind". Persistence of the SCM setting across a power cycle is not documented
in these trees -- that is a gap to state, not to fill by inference.

## 3. `restart_reason@65c`: the value this port writes means "normal", not "bootloader"

`msm-poweroff.c` maps the cell from `"qcom,msm-imem-restart_reason"` (`:665-670`, the same compatible the
board DT carries at `restart_reason@65c`) and is its only writer. The full table, as it compiles for
`CONFIG_MACH_XIAOMI_CLOVER`:

| value | line | command / condition |
|---|---|---|
| `0x77665500` | 522 | `reboot bootloader` (fastboot) |
| `0x77665502` | 526 | `reboot recovery` |
| `0x77665503` | 530 | `reboot rtc` |
| `0x77665508` | 515 | `in_panic` -- the clover-specific panic value (`0x77665508` for dm-verity-corrupted is inside `#if 0` at 535) |
| `0x77665509` | 540 | dm-verity enforcing |
| `0x7766550a` | 544 | keys clear |
| `0x6f656d00 \| code` | 552 | `reboot oem-XX` |
| `0x77665501` | 557, 572, 580 | `reboot other`, the default `else`, and the no-command normal restart -- **normal** |
| `0x77665510` | 690 | written unconditionally at probe |
| `0x0` | 646 | poweroff |

Nothing in that table means download/EDL. And `drivers/CloverSdhci/CloverSdhci.cpp:374` declares

    static const uint32_t kRestartFastboot = 0x77665501u;

with the comment at `:366` that "the stock kernel's `reboot bootloader` writes 0x77665501 there before its
PSCI reset; XBL reads it". The first half of that sentence is contradicted by line 522: the stock kernel's
`reboot bootloader` writes `0x77665500`. `0x77665501` is the vendor's *normal restart* value, which is
also what the stock kernel leaves in the cell after a plain reboot, and what it stamps at probe as
`0x77665510` is neither.

What this does NOT establish: that our own write has no effect. The PMIC side is a separate channel
(`qpnp_pon_set_restart_reason()` takes the enum in `qpnp-power-on.h:49-66`, archived next to this note,
and writes it into `PON_SOFT_RB_SPARE`; the caller is `drivers/input/misc/qpnp-power-on.c`, which is not
archived here and whose line numbers therefore are not quoted) -- clover/jason's header says
NORMAL `0x20`/PANIC `0x21`/OTHER `0x00`, while LineageOS's says NORMAL `0x22`/PANIC `0x21`, so even the
PMIC-side vocabulary differs between the two trees. We never read either cell from a boot that landed
anywhere. The observable record so far is only this: the board has landed in fastboot, in TWRP and in
EDL after resets in which this port wrote `0x77665501`, and no instrument on the card reports what the
next-boot firmware read. So the honest reading is that a **label is wrong and a consumer is unnamed**,
not that a mode-selection path is proven broken.

Consequences carried forward:
- the constant's name and comment, `check-clover.py`'s pin on `0x77665501u`, and `scripts/xnu-panic-screen.c:79-84`
  all repeat the same claim; changing the value is a board-visible behaviour change and therefore waits
  for a flash-capable round (boot 119's card is still unread, so no new image may be flashed).
- the reading half is cheap and is now a named debt: read `restart_reason@65c` *before* this port writes
  it, and read it back, on the same boot, printed as two literals -- then the next landing can be
  attributed to something measured instead of to a comment.

## 4. Why the 20-word reset record overlaps cells it does not own

`boot_stats@6b0` (`reg = <0x6b0 32>`) and `kaslr_offset@6d0` (`reg = <0x6d0 12>`) sit inside the span
`0x660..0x6ff` that `CloverSdhci.cpp`'s r283 record writes. From the vendor side:

- `boot_stats.c:19-24` declares the four fields (`bootloader_start`, `_end`, `_display`, `_load_kernel`)
  at the base of that node and `:77-85` only `readl_relaxed()`s them and prints `KPI:`. The whole file
  contains no write at all -- `grep -c writel boot_stats.c` prints 0 over its 105 lines -- so the kernel is
  strictly a *reader* and the writers are the boot loader. Zeroing that window therefore erases this
  board's own boot-timing numbers, and a heartbeat word living there has two writers: XBL before the OS,
  us after.
- `msm-poweroff.c:274-290`: under `CONFIG_RANDOMIZE_BASE`, `store_kaslr_offset()` writes `0xdead4ead` at
  +0, the low half of the KASLR slide at +4 and the high half at +8 of the `kaslr_offset` node -- i.e.
  +0x6d0/+0x6d4/+0x6d8, inside the same record span.

`scripts/test-clover-imem-geometry.py` is the instrument that recomputes the overlap from these two
archives; see `~/clover-out/round338-sdam-facts/imem-geometry.txt` for its current output.
