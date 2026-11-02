// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the registers the GPU line's first reading touches, and nothing else.
//
// Every address below is the board's own, not a nearby SoC's. The two windows are the DT nodes
// `clock-controller@100000` (GCC, reg = <0x100000 0x94000>) and `gfx@5065000` (GPUCC, reg =
// <0x5065000 0x10000>). The register offsets inside them come from that tree's own clock drivers;
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

// GPUCC. Base from `clock_gfx: gfx@5065000` (sdm660.dtsi:799), same 0x10000 length.
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

// gdsc.c:32 SW_COLLAPSE_MASK = BIT(0), written 0 to power on; gdsc-regulator.c:33 PWR_ON_MASK =
// BIT(31). The pair is what makes a read-only row decodable: "bit31=1 and bit0=0" is powered on and
// nobody asking to collapse; "bit31=0" is off regardless of the collapse bit.
#define kGdscSwCollapse     0x00000001U
#define kGdscPwrOn          0x80000000U

#endif // CLOVER_GPU_GPUREGS_H
