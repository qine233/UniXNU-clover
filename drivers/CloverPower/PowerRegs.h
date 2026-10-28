// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm SPMI PMIC Arbiter v5, QPNP PMIC revid and dig-LODO register definitions for
// drivers/CloverPower.
//
// WHY THESE NUMBERS AND NOT OTHER ONES
//
// Schedule item 6 is power: nothing downstream of this -- display bias, backlight, the touch
// controller's VDD -- can be reasoned about until the port can ask a PMIC what state a rail is
// in. The arbiter is the only door to a PMIC, so this header is the door's floor plan.
//
// Four sources, and every constant below names the one it came from:
//
//   DTB   the device's own flattened tree, decompiled into ~/uefi-clover/Resources/DTBs/clover.dts.
//         `qcom,spmi@800f000` at line 2265 is the arbiter node and is the authority for the five
//         window bases, their sizes, the EE, the channel and the peripheral interrupt. The two
//         PMIC children (`qcom,pm660@0` line 2283, `qcom,pm660l@2` line 2852) are the authority
//         for the slave IDs and the revid peripheral, and `rpm-regulator-ldoa11` (line 5236) and
//         `rpm-regulator-ldob3` (line 5539) are the authority for which rail is which regulator.
//   VEND  drivers/spmi/spmi-pmic-arb.c, drivers/regulator/qcom-spmi-regulator.c and
//         arch/arm64/boot/dts/vendor/qcom/{sdm660,pm660,pm660l,sdm660-regulator}.dtsi +
//         xiaomi/mi/clover/clover-overlay.dtsi of
//         https://github.com/E7G/clover-sdm660-ksb @ 5c81a3faf37e332be5008512c9b2dc2444dbcc01
//         (Xiaomi's own 4.19 CAF tree for this exact tablet). This is the driver that runs on the
//         shipping product, so its register layout is the one the hardware is configured with.
//   MAIN  drivers/spmi/spmi-pmic-arb.c (as sdm660-mainline's `spmi-pmic-arb.c`),
//         drivers/regulator/qcom-spmi-regulator.c, drivers/mfd/qcom-spmi-pmic.c and
//         include/soc/qcom/qcom-spmi-pmic.h of
//         https://github.com/sdm660-mainline/linux @ dd1ed52c8ceef4f16715118ed0252fe75adbe71d
//         (branch qcom-sdm660-7.2.y). Used where MAIN has something VEND does not: the revid
//         register addresses and the PMIC subtype numbers, which the vendor tree reads through a
//         `qcom,qpnp-revid` node that does not name the offsets.
//   SHENG ~/UniXNU-sheng/package/BootResetCore.h -- the same arbiter on SM8550 (version 7),
//         already proven on real hardware by round's-far predecessor: it writes one byte of PMIC
//         SDAM through the observer/read-write channel pair and the stock bootloader reads it
//         back. Its *numbers* are v7 and do not transfer; its *shape* does, and where a decision
//         could go either way -- the read opcode, the status error mask, which of two APIDs wins
//         for a PPID -- the proven code is quoted and the reason it is safe here is written down.
//
// Where the sources disagree it is said in the comment at the constant. There are two real
// disagreements: which read opcode to send (kPmicArbReadOp, below), and what core + 0x0004 means --
// an interrupt enable that nobody reads (VEND) or the APID count that bounds the map scan (MAIN).
//
// This header claims nothing about what the hardware is doing. It only names the places to look.
#ifndef CLOVER_POWER_REGS_H
#define CLOVER_POWER_REGS_H

// ------------------------------------------------------------------------ the five windows
//
// DTB `qcom,spmi@800f000`, line 2267-2268:
//   reg = <0x800f000 0x1000 0x8400000 0x1000000 0x9400000 0x1000000 0xa400000 0x220000
//          0x800a000 0x3000>;
//   reg-names = "core\0chnls\0obsrvr\0intr\0cnfg";
// and the same five pairs, byte-reversed, in this unit's own dump
// (facts/81ffcaaa-20261022-210928/dt/soc-reg.txt line 159:
// `00f00008 00100000 00004008 00000001 00004009 00000001 0000400a 00002200 00a00008 00300000`).
// The device's /proc/iomem gives the same five spans under the same five names
// (facts/81ffcaaa-20261022-210928/proc/iomem.txt:36-40: 0800a000-0800cfff cnfg,
// 0800f000-0800ffff core, 08400000-093fffff chnls, 09400000-0a3fffff obsrvr,
// 0a400000-0a61ffff intr). Three independent sources, no disagreement: these are transcriptions.
//
// The names matter more than the sizes, because they decide which window a transaction goes
// through. VEND spmi-pmic-arb.c:124-125 states the rule in a comment -- `rd_base` is "observer"
// and `wr_base` is "chnls" from v2 on -- and :1227-1236 is the code that gets them by name
// (devm_ioremap_resource of "obsrvr" into rd_base, "chnls" into wr_base). v5 selects that split at
// VEND:1210-1227, off the version read at core + 0x0000. Every read below therefore happens
// against rd_base, which is what makes the write window's existence irrelevant to this driver.
#define kSpmiCorePhys         0x0800f000ULL
#define kSpmiCoreSize         0x1000U
#define kSpmiChnlsPhys        0x08400000ULL
#define kSpmiChnlsSize        0x01000000U
#define kSpmiObsrvrPhys       0x09400000ULL
#define kSpmiObsrvrSize       0x01000000U
#define kSpmiIntrPhys         0x0a400000ULL
#define kSpmiIntrSize         0x00220000U
#define kSpmiCnfgPhys         0x0800a000ULL
#define kSpmiCnfgSize         0x3000U

// The window this driver actually maps out of each. Nothing here needs the 16 MiB of channel
// memory: a v5 observer channel is 0x80 bytes at 0x80*apid inside the EE's own 0x10000 block
// (VEND:1005), so one EE block holds 512 observer channels -- more than the 448 APIDs this core
// window can describe -- and the whole read path fits in a single 64 KiB mapping.
#define kSpmiMapSize          0x1000U
#define kSpmiObsBlockMapSize  0x10000U    // one EE's observer block: 512 channels at 0x80 each

// ------------------------------------------------------------------------ the arbiter's own registers
//
// All of these are offsets inside the `core` window unless said otherwise.
//
// VEND spmi-pmic-arb.c:20-24 defines PMIC_ARB_VERSION as 0x0000 with the three version floors
// next to it, and :1210 is the `readl_relaxed(core + PMIC_ARB_VERSION)` whose value picks the
// version ops. :23-24 give V3_MIN and V5_MIN; a board whose read lands at or above V5_MIN is the
// layout this header describes.
//
// core + 0x0004 has TWO MEANINGS, and this is one of the two real disagreements between the
// sources. VEND:24 calls it PMIC_ARB_INT_EN and never reads it (there is no reference to it
// anywhere in that file -- the vendor tree leaves the interrupt enable exactly as the boot had it).
// MAIN gives the same address two names, PMIC_ARB_INT_EN at :32 and PMIC_ARB_FEATURES at :34, and
// its v5 bring-up MAIN:1361-1380 `pmic_arb_init_apid_v5()` reads it as a count:
// `bus->apid_count = readl_relaxed(core + PMIC_ARB_FEATURES) & PMIC_ARB_FEATURES_PERIPH_MASK`
// with the mask GENMASK(10, 0) at MAIN:35, then refuses the value if it exceeds max_periphs.
// Both readings cannot be simultaneously true of one register's bits, so the driver below reads
// the word once and prints it under both decodings, and bounds the APID scan by the number that
// does not depend on the argument at all -- the core window's size, which is what VEND:910-911
// actually uses (`offset >= pmic_arb->core_size`, i.e. 448 words from 0x900 inside 0x1000).
#define kPmicArbVersion       0x0000U
#define kPmicArbIntEn         0x0004U         // VEND:24's name for it; VEND never reads it
#define kPmicArbFeatures      0x0004U         // MAIN:34's name for the same address
#define kPmicArbFeaturesApidMask 0x7ffU       // MAIN:35 PMIC_ARB_FEATURES_PERIPH_MASK, bits 10:0
#define kPmicArbVersionV2Min  0x20010000U
#define kPmicArbVersionV3Min  0x30000000U
#define kPmicArbVersionV5Min  0x50000000U
#define kPmicArbVersionV6Min  0x60000000U   // the floor SHENG's v7 code sits above; v5 stops here

// APID -> PPID mapping table. VEND:1113-1116 `pmic_arb_apid_map_offset_v5()` returns
// `0x900 + 0x4 * n` -- MAIN:1774-1777 has the identical function -- and VEND:909-911 is the loop
// that walks it, stopping when the offset reaches the core window's size
// (`offset >= pmic_arb->core_size`, i.e. 448 entries for this DTB's 0x1000 core). VEND:917 decodes
// a word: `ppid = (regval >> 8) & PMIC_ARB_PPID_MASK` with the mask 0xFFF at :97, and :918 tests
// the IRQ-owner bit, `BIT(24)` at :46. A zero word means "no mapping" and is skipped (:915-916
// `if (!regval) continue;`).
//
// SHENG BootResetCore.h:22 puts the same table at core + 0x2000 on v7 with the same bit layout
// (`bits 19:8 PPID, bit 24 IRQ owner`), which is why the extraction below is safe to reuse even
// though the base is not: the v5 table starts 0x1700 earlier and inside a smaller window.
//
// Which APID wins for a PPID is decided at VEND:930-932: `if (!valid || apidd->write_ee == ee)`
// stores this index, so the first mapping found is used unless a later one is owned by this EE --
// and that is the rule pa_scan_apids() in PowerArb.h implements, with the same walk order and the
// same tie-break.
#define kPmicArbApidMap         0x900U
#define kPmicArbApidMapStride   0x4U
#define kPmicArbPpidShift       8U
#define kPmicArbPpidMask        0xFFFU
#define kPmicArbIrqOwnerBit     24U
#define kPmicArbMapEntries      ((kSpmiCoreSize - kPmicArbApidMap) / kPmicArbApidMapStride)

// Ownership table: which execution environment may *write* APID n. VEND:50-51
// `SPMI_OWNERSHIP_TABLE_REG(N) = (0x0700 + (4 * (N)))` and `SPMI_OWNERSHIP_PERIPH2OWNER(X) = ((X) & 0x7)`,
// read at VEND:920-922 inside the same scan. MAIN:1790-1793 `pmic_arb_apid_owner_v2()` computes the
// same address (`bus->cnfg + 0x700 + 0x4 * n`) and the v5 ops table installs it. SHENG:23 keeps the
// same 4-byte stride and the same 3-bit field but at cnfg + 0x00, so on v5 the table lives 0x700
// into the cnfg window.
//
// This is the register that answers the question this whole round is a prerequisite for: if the
// owner of a touch-rail APID is not EE 0, then XNU -- which is EE 0 -- cannot write that rail no
// matter how correct the command encoding is, and the port has to go through RPM instead.
#define kPmicArbOwnerTable      0x700U
#define kPmicArbOwnerStride     0x4U
#define kPmicArbOwnerMask       0x7U

// Channel registers, identical in the observer and the read/write channel blocks.
// VEND:27-33. `bc` is byte-count-minus-one (:62 PMIC_ARB_CMD_MAX_BYTE_COUNT 8, :94
// PMIC_ARB_MAX_TRANS_BYTES (8), and pmic_arb_read_cmd's `u8 bc = len - 1` at :340).
#define kPmicArbCmd             0x00U
#define kPmicArbConfig          0x04U
#define kPmicArbStatus          0x08U
#define kPmicArbWdata0          0x10U
#define kPmicArbWdata1          0x14U
#define kPmicArbRdata0          0x18U
#define kPmicArbRdata1          0x1CU

// STATUS bits, VEND:54-58 (enum pmic_arb_chnl_status). DONE is the only "the transaction is over"
// signal; the other three are why it ended, and VEND:261-278 returns a different error for each
// (-EPERM for DENIED, -EIO for FAILURE and DROPPED), which is why the codes below stay distinct.
// SHENG:21 collapses the three into one mask -- 0xe -- and that is the shape kept below for the
// "did it end badly" test, because a report that says "denied" when it means "the bits were 6" is
// a report that cannot be checked. MAIN:305-333 has the same three branches for v1 through v8.
#define kPmicArbStatusDone      (1U << 0)
#define kPmicArbStatusFailure   (1U << 1)
#define kPmicArbStatusDenied    (1U << 2)
#define kPmicArbStatusDropped   (1U << 3)
#define kPmicArbStatusErrors    0xeU

// NOTHING CLEARS DONE, AND THAT IS PROVEN BY USE, NOT BY READING
//
// Neither tree writes to a channel's STATUS after a transaction: VEND:244-289 is the whole wait
// and MAIN:383-414 is the same shape with the branches moved into check_chnl_status. Both simply
// issue the next command. So a second transaction on the same channel is only safe if writing CMD
// drops the stale DONE, and the evidence that it does is SHENG's br_set_bootloader()
// (BootResetCore.h:71-81): read, write, read again over the same APID with no clear in between,
// and the third transaction returns the byte the bootloader then acts on. Three transactions, two
// channels, one DONE bit each, no explicit reset. The mode 2/3 paths below run up to twenty
// transactions this way, so this is the reasoning that makes that legal rather than lucky.

// Opcodes, VEND:65-80 (enum pmic_arb_cmd_op_code): EXT_WRITEL 0, EXT_READL 1, ... READ 15.
//
// WHICH READ OPCODE, AND WHY THIS PORT PICKS EXT_READL
//
// VEND:358-364 normalises whatever the SPMI core handed it: an on-the-wire opcode of 0x60..0x7F
// becomes PMIC_ARB_OP_READ (15), 0x20..0x2F becomes EXT_READ (13) and 0x38..0x3F becomes
// EXT_READL (1). So both are "correct" as far as the driver is concerned, and which one a given
// transaction gets depends on include/linux/spmi/spmi.h's opcode selection, which is not in either
// tree fetched here. Rather than guess, this port copies SHENG:58 -- the one SPMI read on this
// class of hardware that has been *proven on a device*: `br_read8()` sends BR_OP_EXT_READL (=1)
// with bc 0, and the value it returns is the byte the stock bootloader then acts on. EXT_READL is
// therefore the opcode with evidence behind it, and it is what kPmicArbReadOp below is.
//
// The corresponding write opcode, for whoever runs this table after the rails are understood, is
// EXT_WRITEL (0) -- also from SHENG:66, also proven. It is defined here and never sent: this
// driver issues no writes to a PMIC.
#define kPmicArbOpExtWritel     0U
#define kPmicArbOpExtReadl      1U
#define kPmicArbOpRead          15U
#define kPmicArbReadOp          kPmicArbOpExtReadl

// Command word format. VEND:1025-1028 `pmic_arb_fmt_cmd_v2()` -- which is what v5 installs at
// VEND:1162 -- is `(opc << 27) | ((addr & 0xff) << 4) | (bc & 0x7)`. The slave ID is *not* in the
// command: by v5 the APID has already selected the peripheral, so only the low byte of the
// register address travels. SHENG:31 has the identical expression, which is the strongest
// confirmation available that the packing is right.
//
// Where the bytes come back from, VEND:376-381 (identical in MAIN's pmic_arb_read_cmd_unlocked):
// the first min(bc, 3) + 1 bytes are the little-endian word at RDATA0, and if bc > 3 the remaining
// bc - 4 bytes come from RDATA1. So a burst of 5 bytes -- which is exactly what a regulator's
// identity block is -- is two register reads, and pmic_arb_read_data at VEND:218-224 is the code
// that says so (`memcpy(buf, &data, (bc & 3) + 1)` on the raw word, i.e. low byte first).
#define kPmicArbCmdOpShift      27U
#define kPmicArbCmdAddrShift    4U
#define kPmicArbCmdAddrMask     0xffU
#define kPmicArbCmdBcMask       0x7U

// Channel offsets. VEND:990-1017 `pmic_arb_offset_v5()`:
//   observer (read) channel:  0x10000 * ee + 0x80 * apid      (:1005)
//   read/write channel:       0x10000 * apid                  (:1013), gated first on
//       `apid_data[apid].write_ee != pmic_arb->ee` -> -EPERM (:1008-1011)
// SHENG:25-26 gives v7's strides -- 0x8000 per EE and 0x20 per APID for observers, 0x1000 per
// APID for channels -- smaller ones, same structure, which is what makes the two products of the
// v5 formula easy to tell apart in a report.
#define kPmicArbObsEeStride     0x10000U
#define kPmicArbObsApidStride   0x80U
#define kPmicArbChnlApidStride  0x10000U

// Transaction timeout. VEND:93 `#define PMIC_ARB_TIMEOUT_US 1000`, spent as 1000 iterations of
// `udelay(1)` around a status read (VEND:258-284). SHENG:29 instead bounds the wait by 100000
// plain polls, which is the shape a driver with no usable timer at this point in the boot has to
// use. Both numbers are recorded; the driver polls kPmicArbPolls times and additionally gives up
// past kPmicArbDeadlineMs wall-clock milliseconds, so neither a stuck bit nor a slow SPMI bus can
// eat a boot.
#define kPmicArbTimeoutUs       1000U
#define kPmicArbPolls           200000U
#define kPmicArbDeadlineMs      50U

// Interrupt-side registers, read in mode 1 and nowhere programmed.
//   access enable / irq status / irq clear for APID n live in the *chnls* window at
//       0x100 + 0x10000*n, 0x104 + 0x10000*n, 0x108 + 0x10000*n
//   (VEND:1067-1069, :1085-1087, :1103-1105 -- the three _v5 accessors, each of which returns
//    `pmic_arb->wr_base + ...`, i.e. the chnls window),
//   and the per-EE owner access status is `intr + 0x10000*m + 0x4*n` (VEND:1049-1052 -- m is the
//   EE, n is the APID).
// VEND:100 notes the one enable bit: SPMI_PIC_ACC_ENABLE_BIT = BIT(0).
#define kPmicArbAccEnable        0x100U
#define kPmicArbIrqStatus        0x104U
#define kPmicArbIrqClear         0x108U
#define kPmicArbAccEnableBit     (1U << 0)
#define kPmicArbOwnerAccStatus   0x0U
#define kPmicArbOwnerAccEeStride 0x10000U
#define kPmicArbOwnerAccStride   0x4U

// qpnpint register addresses inside a PMIC peripheral's own 256-byte interrupt block, for the one
// read this driver does of the interrupt state: VEND:446-449 (enum qpnpint_regs) puts the
// real-time status at +0x10, the type at +0x11 and the two polarity registers at +0x12/+0x13.
#define kQpnpintRtSts           0x10U
#define kQpnpintSetType         0x11U
#define kQpnpintPolHigh         0x12U
#define kQpnpintPolLow          0x13U

// ------------------------------------------------------------------------ what the board says about itself
//
// DTB line 2270-2272: `qcom,ee = <0x00>`, `qcom,channel = <0x00>`, `qcom,reserved-chan = <0x1ff>`.
// VEND:1266-1291 is the driver reading both properties, rejecting either above 5 (:1272 and
// :1287), which is why both numbers are asserted rather than assumed. EE 0 is HLOS, and after this boot XNU is what
// lives there.
//
// DTB line 2272 `interrupts = <0x00 0x146 0x00>` with `interrupt-names = "periph_irq"`
// (line 2269): GIC SPI 0x146 = 326, so the INTID is 358. VEND:1260 is the
// `platform_get_irq_byname(pdev, "periph_irq")` that consumes it. Printed, not used: this driver
// does not touch the GIC.
#define kPmicArbEe              0U
#define kPmicArbChannel         0U
#define kPmicArbReservedChan    0x1ffU
#define kPmicArbPeriphIntid     358U          // SPI 326 + 32

// The arbiter node declares no clocks. That is not an omission to work around: VEND's probe reads
// no clock and neither does MAIN's, so SPMI transactions are assumed to be reachable the moment
// the boot hands over. Any clock story this round might invent would be unproven, so there is
// none -- mode 1 verifies the assumption by reading core and reporting what it gets.

// ------------------------------------------------------------------------ the two PMICs
//
// DTB line 2283 `qcom,pm660@0 { reg = <0x00 0x00>; }` and line 2852 `qcom,pm660l@2`. The second
// cell of each `reg` is `SPMI_USID` (= 0) from dt-bindings/spmi/spmi.h, so the slave IDs are 0 and
// 2. VEND pm660.dtsi:6-8 and pm660l.dtsi:6-8 agree, and pm660l.dtsi:64 shows a third die
// (`qcom,pm660l@3`) whose registers this driver does not read.
#define kSidPm660               0x0U
#define kSidPm660l              0x2U
#define kSidPm660l3             0x3U

// Both PMICs put their revid block at peripheral 0x100, size 0x100: DTB line 2289-2291
// (`qcom,revid@100`, `reg = <0x100 0x100>`, with `qcom,fab-id-valid` and `qcom,tp-rev-valid`) and
// VEND pm660.dtsi:12-16 / pm660l.dtsi:12-15.
#define kPmicRevidAddr          0x100U

// The revid registers themselves. VEND only has the `qcom,qpnp-revid` node, whose driver is not
// in the fetched tree, so these come from MAIN drivers/mfd/qcom-spmi-pmic.c:19-24 -- a driver
// that is in the 7.2.y tree that drives this tablet today, reading the same block:
//   0x101 rev2, 0x102 minor, 0x103 major, 0x104 type, 0x105 subtype, 0x1f2 fab_id.
// MAIN:169-198 is the sequence: read type, reject anything that is not PMIC_TYPE_VALUE, then read
// subtype, rev2, minor, major, and -- only for PMI8998 and PM660 subtypes -- fab_id. That last
// condition is the DTB's `qcom,fab-id-valid` expressed in code, and it is why 0x1f2 is read here
// for pm660 and reported as unproven for pm660l, whose DTB revid node carries the flag too but
// whose MAIN branch is not taken.
#define kPmicRev2               0x101U
#define kPmicMinor              0x102U
#define kPmicMajor              0x103U
#define kPmicType               0x104U
#define kPmicSubtype            0x105U
#define kPmicFabId              0x1f2U
#define kPmicTypeValue          0x51U         // MAIN qcom-spmi-pmic.c:26 PMIC_TYPE_VALUE
#define kPm660Subtype           0x1bU         // MAIN include/soc/qcom/qcom-spmi-pmic.h:36
#define kPm660lSubtype          0x1aU         // MAIN include/soc/qcom/qcom-spmi-pmic.h:35

// ------------------------------------------------------------------------ the two rails round181 could not prove
//
// drivers/CloverInput ends its no-answer report with "rails unproven", because both DT touch
// nodes declare supplies and that driver never touches SPMI. Those supplies are the two rails
// this section is about, and the board's own tree names them twice over:
//
//   DTB `focaltech@38` (clover.dts:2236-2248 area, and VEND xiaomi/mi/clover/clover-overlay.dtsi
//   :689-699) -- `vdd-supply = <&pm660l_l3>`, `vcc_i2c-supply = <&pm660_l11>`.
//   DTB `gt9xx@5d` (VEND clover-overlay.dtsi:701-717) -- `vdd_ana-supply = <&pm660l_l3>`,
//   `vcc_i2c-supply = <&pm660_l11>`.
// Both candidates are fed by the same pair, so the fitted-controller question round181 leaves
// open does not change what has to be read here.
//
// Which physical peripheral each name is:
//   pm660_l11 -- MAIN drivers/regulator/qcom-spmi-regulator.c:2054 `pm660_regulators[]`, entry
//                `{ "l11", 0x4a00, "vdd_l8_l9_l10_l11_l12_l13_l14" }` (line 2064 in that table).
//   pm660l_l3 -- MAIN:2083 `pm660l_regulators[]`, entry `{ "l3", 0x4200, "vdd_l3_l5_l7_l8" }`
//                (line 2088).
// The vendor tree reaches the same two rails by their RPM names, and the DTB keeps those nodes:
// `rpm-regulator-ldoa11` with `qcom,resource-id = <0x0b>` and `regulator-name = "pm660_l11"`
// (DTB:5236-5254), and `rpm-regulator-ldob3` with `qcom,resource-id = <0x03>` and
// `regulator-name = "pm660l_l3"` (DTB:5539-5556). Those are the "A11" and "B3" of the schedule
// note: LDO-A number 11 on pm660, LDO-B number 3 on pm660l.
//
// The DTB also carries the ranges, which is what makes a voltage reading interpretable:
//   A11: min 0x1b2920 = 1780000 uV, max 0x1dc130 = 1950000 uV   (DTB:5249-5250; VEND
//        sdm660-regulator.dtsi:244 pm660_l11 agrees, and MAIN sdm660-xiaomi-clover-regular-common
//        .dtsi calls it the touch vcc_i2c)
//   B3 : min = max = 0x2dc6c0 = 3000000 uV                      (DTB:5552-5553 -- a fixed 3.0 V
//        rail, so a reading that is not 3000 mV means the rail is off or not in its DT-declared
//        set point, not that the rail is mis-set)
#define kLdoA11Sid              kSidPm660
#define kLdoA11Addr             0x4a00U
#define kLdoA11MinUv            1780000U
#define kLdoA11MaxUv            1950000U
#define kLdoB3Sid               kSidPm660l
#define kLdoB3Addr              0x4200U
#define kLdoB3FixedUv           3000000U

// How many rails this round reads: two, because that is how many the DT's touch nodes share, and the
// driver sizes its per-rail arrays from it rather than from a literal. A third rail (the panel's own
// supply, or a bias) belongs to the round that reads the display's power, not this one.
#define kPmicArbRailCount       2U

// A peripheral number, and so a PPID, is the register address with its low byte removed:
// ppid = (sid << 8) | (addr >> 8). VEND:996 in pmic_arb_offset_v5 computes exactly that, and
// VEND's qpnpint irq_translate unpacks the same way (`sid = hwirq>>20`, `periph_id = hwirq>>16
// & 0xff`). The four values below are therefore arithmetic on the transcriptions above, not new
// claims:
//   pm660 revid  : (0 << 8) | (0x100 >> 8) = 0x001
//   pm660l revid : (2 << 8) | (0x100 >> 8) = 0x201
//   pm660 l11    : (0 << 8) | (0x4a00 >> 8) = 0x04a
//   pm660l l3    : (2 << 8) | (0x4200 >> 8) = 0x242
#define kPpidOf(sid, addr)      ((((sid) & 0xfU) << 8) | (((addr) >> 8) & 0xffU))
#define kPpidPm660Revid         0x001U
#define kPpidPm660lRevid        0x201U
#define kPpidLdoA11             0x04aU
#define kPpidLdoB3              0x242U

// ------------------------------------------------------------------------ a dig-LODO's own registers
//
// VEND drivers/regulator/qcom-spmi-regulator.c:161-170 (and MAIN's spmi_common_regulator_registers) is
// `enum spmi_common_regulator_registers`, the per-peripheral register map every QPNP regulator
// uses:
//   0x01 dig major revision, 0x04 type, 0x05 subtype, 0x40 voltage range, 0x41 voltage set,
//   0x45 mode, 0x46 enable, 0x48 pull-down, 0x4c soft start, 0x61 step ctrl.
// MAIN:109 `SPMI_REGULATOR_TYPE_LDO = 0x04` is the type a digital LDO must report; a 0x04 at 0x04
// is the reading that proves the address really is an LDO and not something else sharing the
// peripheral number.
//
// MAIN:1688-1697 `spmi_regulator_match()` is why the driver below reads 0x01..0x05 as one 5-byte
// burst: `u8 version[SPMI_COMMON_REG_SUBTYPE - SPMI_COMMON_REG_DIG_MAJOR_REV + 1]` (:1693) read in
// a single spmi_vreg_read at :1696, and the fields indexed as `[reg - SPMI_COMMON_REG_DIG_MAJOR_REV]`
// at :1702-1709.
#define kLdoRegDigMajorRev      0x01U
#define kLdoRegType             0x04U
#define kLdoRegSubtype          0x05U
#define kLdoRegVoltageRange     0x40U
#define kLdoRegVoltageSet       0x41U
#define kLdoRegMode             0x45U
#define kLdoRegEnable           0x46U
#define kLdoRegPullDown         0x48U
#define kLdoTypeLdo             0x04U

// Enable and mode bits. VEND:212-214 / MAIN:257-259: SPMI_COMMON_ENABLE_MASK = 0x80, ENABLE = 0x80,
// DISABLE = 0x00 -- bit 7 of 0x46 is "the LDO is on", and the low nibble of the same register is
// the hardware-pin follow mask (VEND:215-219, EN0..EN3 = 0x01, 0x02, 0x04, 0x08). VEND:222-230 /
// MAIN:267-275 give the mode register's bits: HPM 0x80, AUTO 0x40, BYPASS 0x20, FOLLOW_AWAKE 0x10,
// then EN0..EN3.
//
// The 0x46 reading is the answer this round wants most, because it is the one number that
// separates "the touch controller is dead" from "nobody turned it on": round181's mode 3 can NACK
// forever if VCC_I2C is off, and no register dump on the I2C side can tell those two apart.
#define kLdoEnableMask          0x80U
#define kLdoEnableFollowEn0     0x01U
#define kLdoEnableFollowEn1     0x02U
#define kLdoEnableFollowEn2     0x04U
#define kLdoEnableFollowEn3     0x08U
#define kLdoModeHpm             0x80U
#define kLdoModeAuto            0x40U
#define kLdoModeBypass          0x20U
#define kLdoModeFollowAwake     0x10U

// Voltage decoding, and why it is conditional.
//
// A pm660 LDO is not in the legacy "range selector + set-point index" layout. MAIN's regulator
// table lists the L660 LDO subtypes -- 0x42 N300_ST, 0x43 N600_ST, 0x46 P50, 0x47 P150, 0x49 P600,
// 0x4d LVP150, 0x4f LVP600 (MAIN:147-153) -- and every one of them is bound to the FTSMPS426
// register layout and the `ftsmps426` set points at MAIN:1623-1637. That layout puts the voltage at
// 0x40/0x41 as a 16-bit little-endian **millivolt** count: MAIN:935-946
// `spmi_regulator_ftsmps426_get_voltage()` reads two bytes from SPMI_FTSMPS426_REG_VOLTAGE_LSB
// (= 0x40, MAIN:204) and computes `((buf[1] << 8) | buf[0]) * 1000` microvolts, and MAIN:537-539
// `ftsmps426_ranges[]` is a single range with step 4000 uV from a 320000 uV set-point floor.
//
// So: if 0x04 says 0x04 (an LDO) and 0x05 says one of the L660 subtypes, the 0x40/0x41 pair is
// millivolts and the driver below prints microvolts next to it. If the subtype is anything else
// -- which would mean this rail is not the L660 part MAIN describes -- only the raw bytes are
// printed, because then 0x40 would be a range selector and 0x41 an index, and reading them as
// millivolts would be an invention.
#define kLdoVoltageLsb          kLdoRegVoltageRange   // 0x40, the FTSMPS426 LSB
#define kLdoVoltageMsb          kLdoRegVoltageSet     // 0x41, the FTSMPS426 MSB
#define kLdoSubtypeL660N300St   0x42U
#define kLdoSubtypeL660N600St   0x43U
#define kLdoSubtypeL660P50      0x46U
#define kLdoSubtypeL660P150     0x47U
#define kLdoSubtypeL660P600     0x49U
#define kLdoSubtypeL660Lvp150   0x4dU
#define kLdoSubtypeL660Lvp600   0x4fU
#define kLdoFtsmps426StepUv     4000U
#define kLdoFtsmps426MinUv      320000U

#endif // CLOVER_POWER_REGS_H
