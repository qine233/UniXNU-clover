// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the registers the GPU line's first reading touches, and nothing else.
//
// Every address below is the board's own, not a nearby SoC's. The windows are the DT nodes
// `clock-controller@100000` (GCC, reg = <0x100000 0x94000>), `gfx@5065000` (GPUCC, reg =
// <0x5065000 0x10000>), `qcom,kgsl-3d0@5000000` (the GPU core, reg = <0x5000000 0x40000>) and
// `regulator@01fcf004` (the L1 MEM_ACC select, reg = <0x1fcf004 0x4>); every one of those four bases
// and sizes is compared against this unit's own `/proc/device-tree/soc/*/reg` dump by
// scripts/check-clover.py. The register offsets inside them come from that tree's own clock drivers;
// the GPU GDSC nodes come from its `msm-gdsc-660.dtsi`. A later round that writes any of these
// registers inherits the same map, which is why the offsets are quoted with their source rather
// than typed once into a probe.

#ifndef CLOVER_GPU_GPUREGS_H
#define CLOVER_GPU_GPUREGS_H

// GCC. The DT base is 0x00100000; the offsets below are the ones gcc-sdm660.c spells, so the
// physical address is kGccPhys + offset (0x71004 -> 0x081004). The base notes/gpu-bringup.md
// carried before this file existed was a wrong pin, not a second GCC; round282 corrected it in
// the notes and in porting.toml, and CloverInput's own registers had it right all along.
#define kGccPhys            0x00100000ULL
#define kGccSize            0x94000U

// gcc-sdm660.c:1580 `gcc_gpu_cfg_ahb_clk`, .enable_reg = 0x71004, BIT(0), halt_check BRANCH_VOTED,
// flags CLK_IS_CRITICAL. The vendor probe enables this before touching GPUCC at all, with the
// comment "This clock is used for all GPUCC register access" -- that comment is this probe's gate.
#define kGccGpuCfgAhb       0x71004U
// gcc-sdm660.c:1567 `gcc_gpu_bimc_gfx_clk`, .enable_reg = 0x71010, BIT(0).
#define kGccGpuBimcGfx      0x71010U
// gcc-sdm660.c:1029 `gcc_bimc_gfx_clk`, .enable_reg = 0x7106c, BIT(0).
#define kGccBimcGfx         0x7106cU

// Positive controls, on the same GCC page and in the same read. `sdhc_1` is the eMMC host at
// 0xc0c4000 (sdm660.dtsi:1128) whose every block operation proves these two branches are running;
// a GPU row that reads zero only means something next to them reading one.
// gcc-sdm660.c:1878 `gcc_sdcc1_ahb_clk`, .enable_reg = 0x16008, BIT(0).
#define kGccSdcc1Ahb        0x16008U
// gcc-sdm660.c:1891 `gcc_sdcc1_apps_clk`, .enable_reg = 0x16004, BIT(0).
#define kGccSdcc1Apps       0x16004U

// GPUCC. Base from `clock_gfx: gfx@5065000` (sdm660.dtsi:1081, reg at :1083 -- `clock_gpu:
// clock-controller@5065000` is the same extent at :1074/:1076), same 0x10000 length.
#define kGpuccPhys          0x05065000ULL
#define kGpuccSize          0x10000U

// msm-gdsc-660.dtsi:128 `gdsc_gpu_cx: qcom,gdsc@5066004`. Its `hw-ctrl-addr` names the syscon at
// 0x5066008, and gdsc-regulator.c's poll_gdsc_status() reads that register instead of the GDSCR
// when hw_ctrl is present. So the state bit for CX lives at 0x1008, not at 0x1004.
#define kGpuccGdscCx        0x1004U   // GDSCR: bit0 SW_COLLAPSE, bit31 is not the status here
#define kGpuccCxHwCtrl      0x1008U   // bit31 PWR_ON
// msm-gdsc-660.dtsi:149 `gdsc_gpu_gx: qcom,gdsc@5066094`; no hw-ctrl, so its own bit31 is the state.
#define kGpuccGdscGx        0x1094U
// ... and the two syscons that node names, read only to report them.
#define kGpuccGxDomain      0x0130U   // gpu_gx_domain_addr: syscon@5065130
#define kGpuccGxSwReset     0x1090U   // gpu_gx_sw_reset:    syscon@5066090

// gpucc-sdm660.c: `gpucc_gfx3d_clk` branch .enable_reg = 0x1098, BIT(0); `gfx3d_clk_src`
// .cmd_rcgr = 0x1070; `gpucc_cxo_clk` 0x1020 BIT(0). Printed raw: a branch enable and an RCG
// command word are two different answers, and a round that is not writing them should not pretend
// to decode a rate it did not set.
#define kGpuccGfx3dEn       0x1098U
#define kGpuccGfx3dRcg      0x1070U
#define kGpuccCxoEn         0x1020U

// Round290, the two votes the DT's own GPU clock list names and no round of ours ever touched.
// `msm_gpu clock-names` (sdm660-gpu.dtsi:75-76) asks for six clocks; r286 opened three of them plus
// the gfx3d branch, and left "rbbmtimer_clk" and "rbcpr_clk" alone. Their branch registers are
// gpucc-sdm660.c:276 `gpucc_rbbmtimer_clk` .enable_reg = 0x10d0 and :294 `gpucc_rbcpr_clk`
// .enable_reg = 0x1054, both BIT(0) with halt_check BRANCH_HALT.
#define kGpuccRbbmtimerEn   0x10d0U
#define kGpuccRbcprEn       0x1054U
// ... and the RCG command words those two branches are fed from, read this round so a row can say
// what was requested of them: :215 rbbmtimer .cmd_rcgr = 0x10b0, :236 rbcpr .cmd_rcgr = 0x1030.
#define kGpuccRbbmtimerRcg  0x10b0U
#define kGpuccRbcprRcg      0x1030U

// The halt bit. mainline clk-branch.h:59 names it `CBCR_CLK_OFF = BIT(31)`, and clk-branch.h:31
// spells `BRANCH_HALT 0 /* pol: 1 = halt */` -- which is the halt_check every one of the four GPUCC
// branches above and the three GCC ones declares. So a 1 at bit31 says the clock is NOT running
// however the enable request at bit0 reads, and r286's `gfx3d_en=` field only ever printed bit0.
// Boot 74 could therefore say "gfx3d was asked for" without saying "gfx3d runs"; round290 closed that gap by reading
// bit31 beside bit0 on the same beat.
#define kBranchClkOff       0x80000000U

// Round291: the GFX LDO. `gfx_ldo_vreg: ldo@0506e000` (sdm660-regulator.dtsi:523) declares
// reg = <0x0506e000 0x34>, and 0x0506e000 - 0x05065000 = 0x9000 falls inside the GPUCC extent the
// line above already maps -- two DT nodes claim that block (`clock_gpu` and `clock_gfx` both say
// 0x10000), so r291 reads through the mapping r286 opened rather than asking IOKit for a second
// overlapping one. Offsets, masks and the decode come from the vendor driver for this exact
// compatible: notes/sources/sdm660/msm_gfx_ldo.c (sha256 929bc99f86f1), whose of_match table names
// "qcom,sdm660-gfx-ldo" at :1504. LDO_STATUS1 0x24 is defined but never read by that driver and
// ADC_CTL 0x34 sits outside the DT window, so this round touches neither.
#define kGpuccLdoVref       0x9018U   // LDO_VREF_SET_REG (:34)
#define kGpuccLdoCtrl       0x901cU   // PWRSWITCH_CTRL_REG (:39)
#define kGpuccLdoStatus     0x9028U   // PWRSWITCH_STATUS_REG (:56)
#define kLdoVrefMask        0x0000007fU   // VREF_VAL_MASK = GENMASK(6, 0) (:37)
#define kLdoMinUv           375000u       // MIN_LDO_VOLTAGE (:65)
#define kLdoStepUv          5000u         // LDO_STEP_VOLATGE (:67)
// The driver's own get-voltage decode, msm_gfx_ldo.c:1371: `*val = (reg & VREF_VAL_MASK) *
// LDO_STEP_VOLATGE + MIN_LDO_VOLTAGE`. That is the arithmetic this row repeats, not an invention.
#define kLdoCprBypass       0x40000000U   // CPR_BYPASS_IN_LDO_MODE_BIT (:41)
#define kLdoBypass          0x00000040U   // LDO_BYPASS_BIT (:47)
#define kLdoPdn             0x00000020U   // LDO_PDN_BIT (:48)
#define kLdoReady           0x00000004U   // LDO_READY_BIT (:58)
#define kLdoSettled         0x00000010U   // LDO_VREF_SETTLED_BIT (:57)
// msm_gfx_ldo.c:307 is the driver's own settling test -- it waits while
// `(status & (LDO_VREF_SETTLED_BIT | LDO_READY_BIT)) == 0`, then pr_err()s "LDO_VREF_SETTLED not
// set". Those two bits are therefore what "the rail is up at its programmed corner" means in the
// vendor's words, which is the question boot 77's `gx_on=0` now points at.
// DT bounds for the decode, regulator.dtsi:528-529: regulator-min-microvolt 400000, max 925000 --
// val 5 and val 110 in the driver's units. Printed only here, so a reading can be compared by hand.

// Round294 reads the rest of the LDO block instead of only the three words round291 asked. The
// vendor driver names each of these offsets in notes/sources/sdm660/msm_gfx_ldo.c: LDO_ATEST_REG :24,
// LDO_CFG0_REG :25, LDO_CFG1_REG :26, LDO_CFG2_REG :27, LDO_LD_DATA_REG :28, LDO_VREF_TEST_CFG :30,
// LDO_VREF_SET_REG :34 and PWRSWITCH_CTRL_REG :39 (both read by r291), LDO_STATUS1_REG :54 (defined
// there but never read by that driver), PWRSWITCH_STATUS_REG :56 (r291), REF_CURRENT_X1_REG :61 and
// REF_CURRENT_X2_REG :62. The word at 0x20 has no #define in that file, but the vendor's own dump
// table names it -- register_str[] :240-251 lists "LDO_STATUS0" at index 8, and dump_registers()
// (:253-262) reads offset i*4 -- so a reading may be called LDO_STATUS0. The row still prints only
// the word, and nothing is claimed about its bits. ADC_CTL_REG 0x34 stays untouched: `gfx_ldo_vreg` declares
// reg = <0x0506e000 0x34> (sdm660-regulator.dtsi:523), so 0x30 is the last word inside the window.
// sdm660_ldo_config[] :147-158 is the vendor's own expected word for the named ones -- ATEST 0x00000080,
// CFG0 0x0100A600, CFG1 0x000000A0, CFG2 0x0000C3FE, LD_DATA 0x00000000, VREF_TEST_CFG 0x00401100,
// REF_X1 0x00000230, REF_X2 0x00000048 -- and probe writes exactly that list at :1135-1140
// (writel_relaxed of ldo_init_config[i].value at ldo_init_config[i].offset). Those words are quoted
// so a reading can be compared by hand; this round writes none of them.
#define kGpuccLdoAtest      0x9000U   // LDO_ATEST_REG (msm_gfx_ldo.c:24)
#define kGpuccLdoCfg0       0x9004U   // LDO_CFG0_REG (:25)
#define kGpuccLdoCfg1       0x9008U   // LDO_CFG1_REG (:26)
#define kGpuccLdoCfg2       0x900cU   // LDO_CFG2_REG (:27)
#define kGpuccLdoLdData     0x9010U   // LDO_LD_DATA_REG (:28)
#define kGpuccLdoVrefTest   0x9014U   // LDO_VREF_TEST_CFG (:30)
#define kGpuccLdoStatus1    0x9024U   // LDO_STATUS1_REG (:54)
#define kGpuccLdoRefX1      0x902cU   // REF_CURRENT_X1_REG (:61)
#define kGpuccLdoRefX2      0x9030U   // REF_CURRENT_X2_REG (:62)

// Round311, the L1 domain's memory-accessor corner select. `regulator@01fcf004` in the tree this
// unit actually ships (`~/mu-clover/Resources/DTBs/clover.dtb`, sha256 815b8255..., read with
// `dtc -I dtb -O dts` at :7633-7645 -- the method §5.12 requires, not the vendor dtsi) is the only
// MEM_ACC node in the tree, and the GPU's own CPR4 corner consumer points at it:
// `cpr4-ctrl@05061000` has `mem-acc-supply = <0x163>` and 0x163 is this node's phandle. Verbatim:
//
//   compatible = "qcom,mem-acc-regulator";
//   reg = <0x1fcf004 0x04>;
//   reg-names = "acc-sel-l1";
//   regulator-name = "gfx_mem_acc_corner";
//   regulator-min-microvolt = <0x01>;
//   regulator-max-microvolt = <0x02>;
//   qcom,corner-acc-map = <0x01 0x00>;
//   qcom,acc-sel-l1-bit-pos = <0x00>;
//   qcom,acc-sel-l1-bit-size = <0x01>;
//   phandle = <0x163>;
//
// Two things make this the next reading rather than any other word on the board. It is pure MMIO --
// four bytes, one register, and the node declares no clocks and no supply of its own, so touching it
// is not the unclocked-bus gamble the GPU core window is. And §5.12 established that this board's GX
// corner is not the GFX LDO at all (`vdd-supply` is a stub regulator, `system-supply` is
// pm660l_s3_level whose level is an RPM SMD request this boot chain has never sent), so mem-acc is
// the half of the same question that can be answered without an RPM.
#define kMemAccPhys         0x01fcf004ULL
#define kMemAccSize         0x4U
// The field the node describes: bit-pos 0, bit-size 1. The row prints the whole word as well, because
// a reading that keeps only the field cannot tell a live 0 from a page that reads zero.
#define kMemAccBitPos       0U
#define kMemAccBitSize      1U
#define kMemAccFieldMask    ((((1u << kMemAccBitSize) - 1u) << kMemAccBitPos))
// The corners this node offers, from `regulator-min-microvolt`/`-max-` -- for this compatible those
// two properties carry corner numbers, which is why they read 1 and 2 and not 400000/925000 -- and
// the field value each corner asks for, in the order `qcom,corner-acc-map` lists them: corner 1 ->
// 0x01, corner 2 -> 0x00. Nothing here claims to know what the two corners mean in volts: the node
// has no voltage table, and §5.12 says the level that does exist is an RPM request.
#define kMemAccMinCorner    1U
#define kMemAccMaxCorner    2U
#define kMemAccCorner1Field 0x01U
#define kMemAccCorner2Field 0x00U
// All-ones is this board's "the page did not answer" signature -- the same one round282 and round286
// gate GPUCC behind. It has to be tested on the word before the field is cut out of it: with
// bit-size 1 the masked field of 0xffffffff is 1, which is also corner 1's own map value, so a row
// that decoded first and asked later would report a dead page as a powered corner.
#define kMemAccNoAnswer     0xffffffffU

// gdsc.c:32 SW_COLLAPSE_MASK = BIT(0), written 0 to power on; gdsc-regulator.c:33 PWR_ON_MASK =
// BIT(31). The pair is what makes a read-only row decodable: "bit31=1 and bit0=0" is powered on and
// nobody asking to collapse; "bit31=0" is off regardless of the collapse bit.
#define kGdscSwCollapse     0x00000001U
#define kGdscPwrOn          0x80000000U

// gdsc.c's remaining bits, needed only now that this round writes. gdsc_init() clears the hw-trigger
// and sw-override bits so collapse/restore follows register writes, and sets the three wait timers;
// gdsc_toggle_logic(true) then clears SW_COLLAPSE and polls PWR_ON. The write side mirrors both, so
// these are the bits the write touches, quoted from the same file the offsets above came from.
#define kGdscHwControl      0x00000002U   // BIT(1): hw trigger for collapse/restore
#define kGdscSwOverride     0x00000004U   // BIT(2): sw override of the state machine
#define kGdscEnRestWaitMask 0x00F00000U   // gdsc.c EN_REST_WAIT_MASK  GENMASK(23,20)
#define kGdscEnFewWaitMask  0x000F0000U   // gdsc.c EN_FEW_WAIT_MASK   GENMASK(19,16)
#define kGdscClkDisWaitMask 0x0000F000U   // gdsc.c CLK_DIS_WAIT_MASK  GENMASK(15,12)
#define kGdscEnRestWaitVal  (0x2u << 20)
#define kGdscEnFewWaitVal   (0x8u << 16)
#define kGdscClkDisWaitVal  (0x2u << 12)
#define kGdscWaitMask       (kGdscEnRestWaitMask | kGdscEnFewWaitMask | kGdscClkDisWaitMask)
#define kGdscWaitVal        (kGdscEnRestWaitVal | kGdscEnFewWaitVal | kGdscClkDisWaitVal)

// GPU core. Base from `msm_gpu` reg = <0x5000000 0x40000> (sdm660-gpu.dtsi). This window is mapped
// only after the CX and GX power polls read on and the AHB vote is set; before that, touching it is
// the unclocked-bus gamble round282 declined and this round still refuses to take.
#define kGpuPhys            0x05000000ULL
#define kGpuSize            0x40000U

// The DT's qcom,chipid, read at kGpuPhys + 0x10200. a512 reports 0x05010200 once the core is powered
// and the AHB path is clocked; the read is gated on both and is the round's acceptance.
#define kGpuChipid          0x10200U
#define kGpuChipidValue     0x05010200U

#endif // CLOVER_GPU_GPUREGS_H
