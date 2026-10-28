// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm QUP v2 I2C, SDM660 GCC/TLMM and touch-controller register definitions for
// drivers/CloverInput.
//
// Every offset and every bit below is transcribed from a named source file, not recalled. Three
// sources, and where they disagree the disagreement is written out rather than smoothed over:
//
//   DT   the device's own flattened tree, decompiled into
//        ~/uefi-clover/Resources/DTBs/clover.dts -- the /proc/device-tree of the running tablet.
//        The nodes used here are `i2c@c177000` (line 11766), its children `focaltech@38` and
//        `gt9xx@5d`, `i2c_3_active`/`i2c_3_sleep` (line 9643) and the `ts_mux` states under the
//        touch pinctrl node (line 11458).
//   MAIN drivers/qcom_clk/sdm660/gcc-sdm660.c, drivers/pinctrl/qcom/pinctrl-sdm660.c,
//        drivers/i2c/busses/i2c-qup.c, drivers/clk/qcom/{clk-rcg2.c,clk-rcg.h,clk-branch.c,
//        clk-branch.h}, drivers/pinctrl/qcom/pinctrl-msm.c and
//        drivers/input/touchscreen/edt-ft5x06.c of
//        https://github.com/sdm660-mainline/linux @ dd1ed52c8ceef4f16715118ed0252fe75adbe71d
//        (branch qcom-sdm660-7.2.y, board.toml feed `clover-mainline-dts`). This is the tree that
//        demonstrably drives this tablet's display and touch, so its encodings are the ones a
//        reading from this hardware is interpreted with.
//   VEND drivers/i2c/busses/i2c-msm-v2.c, drivers/input/touchscreen/gt9xx_zql6019/{gt9xx.c,gt9xx.h}
//        (the fitted Goodix driver, named for the panel's controller variant) and
//        drivers/input/touchscreen/edt-ft5x06.c + drivers/pinctrl/qcom/pinctrl-sdm660.c of
//        https://github.com/E7G/clover-sdm660-ksb @ 5c81a3faf37e332be5008512c9b2dc2444dbcc01
//        (Xiaomi's own 4.19 CAF tree for this exact tablet, board.toml feed
//        `clover-downstream-dts`).
//
// Where MAIN and VEND differ it is said in the comment at the constant. The only real disagreement
// found is the I2C bit-clock field packing (kQupI2cClkHz, below), and the vendor tree's own
// dt-bindings header, `i2c-msm-v2.c` and the 4.19 clk driver disagree with each other in places,
// so MAIN is the one whose arithmetic can be checked by hand.
//
// This header claims nothing about what the hardware is doing. It only names the places to look.
#ifndef CLOVER_INPUT_REGS_H
#define CLOVER_INPUT_REGS_H

// ------------------------------------------------------------------------ the windows
//
// clover.dts `i2c@c177000`: compatible = "qcom,i2c-msm-v2", reg = <0xc177000 0x600>,
// reg-names = "<\0"? (empty), interrupts = <0x00 0x61 0x00> -- GIC SPI 97 => INTID 129 --
// qcom,master-id = 0x56, qcom,clk-freq-out = 0x61a80 (400000), qcom,clk-freq-in = 0x124f800
// (19200000), qcom,i2c-dat = <0xe8 0x0a 0x00> (TLMM gpio10), qcom,i2c-clk = <0xe8 0x0b 0x00>
// (gpio11), status = "ok". The register block really is 0x600 bytes; the driver maps a whole
// 4 KiB page because IODeviceMemory::map() works at page granularity, and the same page shape is
// what round87 proved maps on this board for the eMMC host.
#define kQupPhys            0x00c17700ULL
#define kQupSize            0x1000U
#define kQupDtSize          0x600U          // what the DT declares, for the report
#define kQupIntid           129U            // SPI 97 + 32

// GCC, the global clock controller. gcc-sdm660.c:22 `#define GCC_REG_OFFSET(reg) (0x100000 + (reg))`
// and the DT node clock-controller@100000 has reg = <0x100000 0x94000>; the same tree's
// qcom,gcc-sdm660.h and Xiaomi's 4.19 gcc-sdm660.c agree on both numbers. Three of the registers
// this driver needs sit 0x3b000 apart, so they are mapped as three page-sized windows instead of
// one 600 KiB claim -- a page is the granularity round87 already proved this board's memory map
// answers, and each of the three windows below is page-aligned and inside GCC's own range.
#define kGccPhys            0x00100000ULL   // the node's base, for the report only
#define kGccSize            0x94000U        // ... and its length
#define kGccAhbHaltPhys     0x00117000ULL   // holds 0x117004: gcc_blsp1_ahb_clk's CBCR
#define kGccI2cPhys         0x0011d000ULL   // holds 0x11d008, 0x11d020, 0x11d024
#define kGccAhbVotePhys     0x00152000ULL   // holds 0x152004: the AHB vote bit
#define kGccWindowSize      0x1000U

// Absolute register addresses, exactly as gcc-sdm660.c spells them, and the window each falls in.
//
//   gcc_blsp1_ahb_clk        (gcc-sdm660.c:1068) .halt_reg = 0x17004, halt_check = BRANCH_HALT_VOTED,
//                              .enable_reg = 0x52004, .enable_mask = BIT(17)
//   gcc_blsp1_qup3_i2c_apps_clk (gcc-sdm660.c:1153) .halt_reg = .enable_reg = 0x1d008,
//                              .enable_mask = BIT(0), halt_check = BRANCH_HALT
//   blsp1_qup3_i2c_apps_clk_src (gcc-sdm660.c:342)  .cmd_rcgr = 0x1d020, .mnd_width = 0,
//                              .hid_width = 5, .parent_map = gcc_parent_map_xo_gpll0_gpll0_early_div,
//                              .freq_tbl = ftbl_blsp1_qup1_i2c_apps_clk_src
// CFG_REG is cmd_rcgr + 4 (clk-rcg2.c:35, with cfg_off = 0 for this RCG), CMD_REG is cmd_rcgr + 0.
#define kGccAhbHaltOff      0x004U          // abs 0x117004
#define kGccI2cBranchOff    0x008U          // abs 0x11d008
#define kGccI2cCmdOff       0x020U          // abs 0x11d020  (RCG: CMD_REG)
#define kGccI2cCfgOff       0x024U          // abs 0x11d024  (RCG: CFG_REG = cmd_rcgr + 4)
#define kGccAhbVoteOff      0x004U          // abs 0x152004

// The 19.2 MHz XO is what `qcom,clk-freq-in` names, and it is the first row of
// ftbl_blsp1_qup1_i2c_apps_clk_src: F(19200000, P_XO, 1, 0, 0). The second row,
// F(50000000, P_GPLL0, 12, 0, 0), is what makes GPLL0 = 600 MHz by arithmetic.
#define kXoHz               19200000U
#define kGpll0Hz            600000000U

// RCG2 CMD_REG / CFG_REG fields. clk-rcg2.c:26-43.
#define kRcgCmdUpdate       0x1U            // CMD_UPDATE = BIT(0), self-clearing
#define kRcgCfgDivMask      0x1FU           // CFG_SRC_DIV_SHIFT = 0, width = rcg->hid_width = 5
#define kRcgCfgSrcShift     8U
#define kRcgCfgSrcMask      0x700U          // CFG_SRC_SEL_MASK = 0x7 << 8
#define kRcgCfgModeShift    12U
#define kRcgCfgModeMask     0x3000U         // CFG_MODE_MASK; 2 = dual-edge (CFG_MODE_DUAL_EDGE)
#define kRcgCfgHwCtrl       0x100000U       // CFG_HW_CLK_CTRL_MASK = BIT(20)
#define kRcgDiv1            0x1U            // the table's XO row: pre_div 1, src P_XO -> cfg 0
#define kRcgCfgWritable     0x11F000U       // div | src | mode | hw_ctrl: the mask __clk_rcg2_configure
                                            //   puts on CFG before writing it back
#define kRcgUpdateSpin      65536U          // bounded stand-in for upstream's 500 x udelay(1)

// Parent selectors, from gcc_parent_map_xo_gpll0_gpll0_early_div (gcc-sdm660.c:~300):
// {P_XO, 0}, {P_GPLL0, 1}, {P_GPLL0_EARLY_DIV, 6}.
#define kRcgSrcXo           0U
#define kRcgSrcGpll0        1U
#define kRcgSrcGpll0Early   6U

// CBCR (branch) fields. clk-branch.h:63-65: CBCR_CLK_OFF = BIT(31),
// CBCR_NOC_FSM_STATUS = GENMASK(30,28), FSM_STATUS_ON = BIT(1) -- i.e. the 3-bit field == 2.
// clk-branch2_check_halt(br, enabling) returns "done" when
//   (val & BIT(31)) == 0  ||  ((val >> 28) & 7) == 2
// and clk_branch_wait() polls that 200 times with udelay(1) between reads.
#define kCbcrClkOff         0x80000000U
#define kCbcrFsmShift       28U
#define kCbcrFsmMask        0x70000000U
#define kCbcrFsmOn          2U
#define kCbcrEnable         0x1U            // branch clock gate, enable_mask BIT(0)
#define kAhbVoteBit         17U             // gcc_blsp1_ahb_clk .enable_mask = BIT(17)
#define kCbcrSpin           65536U          // bounded stand-in for upstream's 200 x udelay(1)

// ------------------------------------------------------------------------ TLMM
//
// pinctrl-sdm660.c:22-25 and 1419: the north tile, REG_SIZE = 0x1000 per pin, ngpios = 114. Two
// spellings of the tile bases agree: the A variant names reg-offsets south/center/north as
// 0x03100000/0x03500000/0x03900000, the V variant gives tlmm = 0x03000000 length 0xc00000 with
// TLMM_NORTH = 0x900000 -- 0x3000000 + 0x900000 is the same 0x03900000. Xiaomi's 4.19
// ds-pinctrl-sdm660.c agrees on the base and on every bit position used here.
#define kTlmmPhys           0x03900000ULL   // north tile
#define kTlmmPinStride      0x1000U
#define kPinGpio10Phys      (kTlmmPhys + 10ULL * kTlmmPinStride)   // SDA, DT qcom,i2c-dat
#define kPinGpio11Phys      (kTlmmPhys + 11ULL * kTlmmPinStride)   // SCL, DT qcom,i2c-clk
#define kPinGpio66Phys      (kTlmmPhys + 66ULL * kTlmmPinStride)   // touch reset
#define kPinGpio67Phys      (kTlmmPhys + 67ULL * kTlmmPinStride)   // touch interrupt
// gpio10..11 and gpio66..67 are two contiguous 8 KiB pairs; mapping the pair keeps the window
// count at two and the second page of each window is the next pin's CTL.
#define kPinPairSize        0x2000U

// Per-pin register layout, pinctrl-sdm660.c:51-54 (ctl/io/intr_cfg/intr_status) with
// intr_target_reg = the same 0x8 as intr_cfg.
#define kPinCtl             0x000U
#define kPinIo              0x004U
#define kPinIntrCfg         0x008U          // also INTR_TARGET
#define kPinIntrStatus      0x00cU

// Bit fields, pinctrl-sdm660.c:56-69 -- identical in the 4.19 tree's PINGROUP macro:
//   mux_bit 2 (mask GENMASK(5,2) = 0x3c, because nfuncs = 10 -> order_base_2 = 4), pull_bit 0
//   width 2, drv_bit 6 width 3, oe_bit 9, in_bit 0, out_bit 1, intr_enable_bit 0,
//   intr_status_bit 0, intr_raw_status_bit 4, intr_polarity_bit 1, intr_detection_bit 2 width 2,
//   intr_target_bit 5 width 5 (intr_target_kpss_val 3).
// pinctrl-msm.c:690-701 decides which register each lives in, and it is not the split the field
// names suggest: mux, pull and drive are read out of CTL, the OE bit likewise, while the input
// status and the output latch are both in the IO register.
#define kPinMuxShift        2U
#define kPinMuxMask         0x3CU
#define kPinPullMask        0x3U            // CTL[1:0]
#define kPinDrvShift        6U              // CTL[8:6]
#define kPinDrvMask         0x7U
#define kPinOeBit           9U              // CTL bit 9 = output enable
#define kPinInBit           0U              // IO bit 0 = input status
#define kPinOutBit          1U              // IO bit 1 = output latch
#define kPinIntrEnable      0x1U            // INTR_CFG bit 0
#define kPinIntrPolarity    0x2U            // INTR_CFG bit 1
#define kPinIntrDetectShift 2U              // INTR_CFG[3:2]
#define kPinIntrTargetShift 5U              // INTR_CFG[12:8]
#define kPinIntrTargetMask  0x1F00U
#define kPinIntrStatusBit   0x1U            // INTR_STATUS bit 0
#define kPinIntrRawBit      0x10U           // INTR_STATUS bit 4

// Function numbers. pinctrl-sdm660.c:27-38 puts msm_mux_gpio first in every PINGROUP's .funcs, so
// mux value 0 is GPIO, and the value a function gets is its index in that list. gpio10 and gpio11
// are PINGROUP(10, NORTH, blsp_spi3, blsp_i2c3, ...) and PINGROUP(11, NORTH, blsp_spi3,
// blsp_i2c3, ...) -- blsp_i2c3 is the second function after gpio, i.e. mux 2, on both pins.
// gpio66 and gpio67 have no i2c function at all (PINGROUP(66, NORTH, _, _, qdss_cti1_b, ...) and
// PINGROUP(67, NORTH, _, _, qdss_gpio0, ...)), which is why the touch reset and interrupt lines
// are plain GPIOs.
#define kPinFuncGpio        0U
#define kPinFuncBlspI2c3    2U

// Drive strength is encoded as (mA/2 - 1) (pinctrl-msm.c:470-473, which rejects anything outside
// 2..16 mA in even steps), and bias as MSM_NO_PULL 0 / MSM_PULL_DOWN 1 / MSM_KEEPER 2 /
// MSM_PULL_UP 3 (pinctrl-msm.c:320-324). clover.dts `i2c_3_active` asks for drive-strength 2 mA
// and bias-disable on both i2c pins; `i2c_3_sleep` keeps the same mux with bias-pull-up.
#define kPinDrv2mA          0U
#define kPinDrv8mA          3U
#define kPinDrv16mA         7U
#define kPinPullNone        0U
#define kPinPullDown        1U
#define kPinPullKeeper      2U
#define kPinPullUp          3U

// The ts_mux states in clover.dts, phandles 0x188..0x18b, all of them on gpio67:
//   ts_int_defalut     mux gpio, drive 16 mA, input-enable, bias-disable
//   ts_int_output_low  mux gpio, output-low
//   ts_int_output_high mux gpio, output-high
//   ts_int_input       mux gpio, input-enable, bias-disable
// pinctrl-msm.c:496-524 is the part that matters for reading these: INPUT_ENABLE is folded into
// the OE bit and forced to 0, so "input-enable" means "stop driving", and LEVEL (output-high /
// output-low) writes the IO latch and then also sets OE. That is the same pair of steps
// gpio_direction_output()/gpio_direction_input() perform in the vendor's gtp_int_output().
#define kTsIntDrive         kPinDrv16mA

// ------------------------------------------------------------------------ QUP v2 core
//
// `drivers/i2c/busses/i2c-qup.c` lines 25-160, verbatim. Base is 0x0c177000.
#define kQupConfig          0x000U
#define kQupState           0x004U
#define kQupIoMode          0x008U
#define kQupSwReset         0x00cU
#define kQupOperational     0x018U
#define kQupErrorFlags      0x01cU
#define kQupErrorFlagsEn    0x020U
#define kQupOperationalMask 0x028U
#define kQupHwVersion       0x030U
#define kQupMxOutputCnt     0x100U
#define kQupOutFifoBase     0x110U
#define kQupMxWriteCnt      0x150U
#define kQupMxInputCnt      0x200U
#define kQupMxReadCnt       0x208U
#define kQupInFifoBase      0x218U
#define kQupI2cClkCtl       0x400U
#define kQupI2cStatus       0x404U
#define kQupI2cMasterGen    0x408U

#define kQupStateReset      0U              // QUP_RESET_STATE
#define kQupStateRun        1U              // QUP_RUN_STATE
#define kQupStatePause      3U              // QUP_PAUSE_STATE
#define kQupStateMask       3U
#define kQupStateValid      0x4U            // BIT(2)
#define kQupI2cMastGen      0x10U           // BIT(4): the mini-core accepted MASTER_GEN
#define kQupI2cFlush        0x40U           // BIT(6): drop what is in the FIFOs

// OPERATIONAL: the three service bits the polled path needs, the two done bits, and the error bit
// the NACK verdict comes from.
#define kQupNackFlag        0x8U            // QUP_I2C_NACK_FLAG = BIT(3), also an I2C_STATUS bit
#define kQupOutNotEmpty     0x10U           // BIT(4)
#define kQupInNotEmpty      0x20U           // BIT(5)
#define kQupOutFull         0x40U           // BIT(6)
#define kQupOutSvcFlag      0x100U          // BIT(8), write-1-to-clear
#define kQupInSvcFlag       0x200U          // BIT(9), write-1-to-clear
#define kQupMxOutputDone    0x400U          // BIT(10)
#define kQupMxInputDone     0x800U          // BIT(11)
#define kQupOutBlockWriteReq 0x1000U        // BIT(12)
#define kQupInBlockReadReq  0x2000U         // BIT(13)

#define kQupNoInput         0x80U           // CONFIG BIT(7): this transfer reads nothing
#define kQupClockAutoGate   0x2000U         // CONFIG BIT(13)
#define kI2cMiniCore        0x200U          // (2 << 8): the I2C mini-core, not SPI/UART
#define kI2cNValV2          7U              // 8-bit FIFO entries (v2)
#define kQupOutputBlkMode   0x400U          // IO_MODE (1 << 10)
#define kQupInputBlkMode    0x1000U         // IO_MODE (1 << 12)
#define kQupUnpackEn        0x4000U         // IO_MODE BIT(14)
#define kQupPackEn          0x8000U         // IO_MODE BIT(15)
#define kQupRepackEn        0xC000U         // PACK|UNPACK: the word-packing hardware is on
#define kQupV2TagsEn        1U              // I2C_MASTER_GEN: v2 tag protocol

// IO_MODE's read-only size fields, and blk_sizes[] = {4, 16, 32} (i2c-qup.c:1687). The FIFO depth
// is block size * (2 << fifo_size_field).
#define kQupOutBlockSize(x) (((x) >> 0) & 3U)
#define kQupOutFifoSize(x)  (((x) >> 2) & 7U)
#define kQupInBlockSize(x)  (((x) >> 5) & 3U)
#define kQupInFifoSize(x)   (((x) >> 7) & 7U)

// v2 tags. A START tag carries the 7-bit address already shifted, so bit 0 of the byte is the R/W
// bit: 0x00 write, 0x01 read.
#define kQupTagV2Start      0x81U
#define kQupTagV2DataWr     0x82U
#define kQupTagV2DataWrStop 0x83U
#define kQupTagV2DataRd     0x85U
#define kQupTagV2DataRdNack 0x86U
#define kQupTagV2DataRdStop 0x87U

// Error masks, i2c-qup.c:131-134. QUP_STATUS_ERROR_FLAGS covers the mini-core's own flags,
// I2C_STATUS_ERROR_MASK the bus errors, and the driver resets the core on either.
#define kQupStatusErrorFlags 0x7CU
#define kI2cStatusErrorMask  0x38000fcU
#define kI2cStatusBusActive   0x100U        // BIT(8)
#define kI2cStatusWrBufferFull 0x1U         // BIT(0)

#define kQupMxConfigDuringRun 0x80000000U   // BIT(31): reprogram the counters while RUNNING
#define kQupMaxTagsLen        4U            // START + 2-byte address + DATA* + length
#define kQupRxTagLen          2U            // READ_RX_TAGS_LEN: a v2 read returns 2 tag bytes
#define kQupReadLimit         256U          // bytes per QUP block; 0 in the tag means 256

// The bit-clock divisor. i2c-qup.c:1894-1901:
//   hs_div = 3; clk <= 400 kHz -> fs_div = ((src/clk)/2) - 3, clk_ctl = (hs_div << 8) | fs_div
// which for the DT's 19.2 MHz in and 400 kHz out is fs_div = 21 and clk_ctl = 0x00000315.
// The 4.19 tree packs the same word as (ht << 16) | fs with {ht = 14, fs = 28} for 400 kHz
// (i2c-msm-v2.h, and the same values its own DT-derived fs/hc div tables use), i.e. 0x000e001c.
// Both give the same bit time: 2*(21+3) = 48 and 14+28+6 = 48 source periods, 2.5 us at 19.2 MHz.
// MAIN's value is used because that tree is the one that drives this tablet's touch today.
#define kQupI2cClk400k        0x00000315U
#define kQupI2cClk400kVendor  0x000e001cU
#define kQupFsDiv400k         21U
#define kQupHsDiv             3U
#define kQupBusKhz            400U

// ------------------------------------------------------------------------ the two candidates
//
// clover.dts hangs BOTH of these on the same bus, which is the reason mode 3 probes both instead of
// picking one:
//   `focaltech@38`  reg = <0x38>, compatible "focaltech,fts", interrupts = <0x43 0x02> (gpio67,
//                   falling), focaltech,reset-gpio = <0xe8 0x42 0x00> (gpio66), max-touch-number 10,
//                   display-coords = <0 0 0x4b0 0x780> (1200x1920).
//   `gt9xx@5d`      reg = <0x5d>, interrupts = <0x43 0x2800>, reset-gpios = <0xe8 0x42 0x0>,
//                   irq-gpios = <0xe8 0x43 0x2800>, touchscreen-max-id 10, size-x 0x4b0, size-y
//                   0x780, goodix,int-sync 1, goodix,driver-send-cfg 1, goodix,swap-x2y 0 and a
//                   204-byte goodix,cfg-group0.
// The tie-breaker is that vendor tree: Xiaomi's gt9xx.c CTP_CFG_GROUP0 is byte-identical to the
// cfg-group0 in this DT -- same 204 bytes, same tail 0x01 0x00 0xa3 0x01. A config blob that
// matches bit for bit is not a coincidence, so the fitted part is the Goodix at 0x5d. The
// 7.2.y fork calls the same silicon "edt,edt-ft5406" at 0x38, which is why both stay on the list:
// a DT that lists two drivers for one bus is a board that has been built with more than one panel.
//
// Goodix addressing (VEND gt9xx_zql6019/gt9xx.c: gt9xx.h GTP_ADDR_LENGTH 2, and gtp_get_fw_info()
// at 1222-1258 builds its read buffer as {GTP_REG_VERSION >> 8, GTP_REG_VERSION & 0xff}): a 2-byte
// register address, BIG-ENDIAN (high byte first), written and then read back with a repeated start.
//
// The index arithmetic below matters and is easy to get wrong. The vendor's buffer is 8 bytes: [0]
// and [1] are the register address it sent, and the chip's answer lands in [2]..[7]. So the three
// product-id characters it prints as buf[2], buf[3], buf[4] are THIS driver's data bytes [0], [1],
// [2], and the version it computes as (buf[7] << 8) | buf[6] is data bytes [5] and [4] -- and the
// 4-character pid variant it picks when buf[5] != 0 is this driver's [3].
#define kGtAddr             0x5dU
#define kGtAddrName         "goodix gt9xx @5d"
#define kGtRegCommand       0x8040U         // GTP_REG_COMMAND
#define kGtRegCommandCheck  0x8046U         // GTP_REG_COMMAND_CHECK
#define kGtRegConfig        0x8047U         // GTP_REG_CONFIG_DATA
#define kGtRegVersion       0x8140U         // GTP_REG_VERSION: 6 bytes, pid + fw version
#define kGtRegSensorId      0x814aU         // GTP_REG_SENSOR_ID: 1 byte, valid < 0x06 (gt9xx.c:688)
#define kGtRegCoords        0x814eU         // READ_COOR_ADDR: the point report, later round
#define kGtVersionLen       6U
#define kGtPidValidMax      6U              // sensor_id >= 0x06 means "no config was sent"

// EDT/FocalTech addressing (MAIN edt-ft5x06.c: EDT_NAME_LEN 23 at line 71, and
// edt_ft5x06_ts_identify() at 846 reads EDT_NAME_LEN - 1 = 22 bytes from register 0xBB): ONE-byte
// register address. An M06 answers with a leading response byte ("EP0" at [1..3], line 855), an M12
// answers with the string at [0..2] (line 869); anything else is GENERIC_FT (line 900), whose
// firmware version is the 2 bytes at 0xA6 and whose single model-identification byte is at 0xA8
// (lines 902-908). This board's own 4.19 tree (VEND edt-ft5x06.c:786) asks the same 0xBB read the
// same way, so both trees agree on the one question mode 3 asks.
#define kFtAddr             0x38U
#define kFtAddrName         "edt/focaltech @38"
#define kFtRegMode          0x00U           // the register MAIN:1262 reads to tell a live FT chip
#define kFtRegName          0xbbU
#define kFtRegFwVersion     0xa6U
#define kFtRegModel         0xa8U
#define kFtNameLen          22U             // EDT_NAME_LEN (23) minus the terminator
#define kFtNameBuf          24U

#endif  // CLOVER_INPUT_REGS_H
