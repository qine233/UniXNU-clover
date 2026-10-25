// SPDX-License-Identifier: BSD-2-Clause-Patent
// SDHCI / Qualcomm SDCC register definitions for drivers/CloverSdhci.
//
// Every offset and every bit below is transcribed from a named source file, not recalled. The
// two Linux trees are the ones this repository can re-fetch:
//
//   SDHCI  torvalds/linux master `drivers/mmc/host/sdhci.h` (fetched 2026-10-25, saved as
//          /tmp/sdhci-research/sdhci-reg.h) and `drivers/mmc/host/sdhci.c` (sdhci-reg.h's own
//          include list is what the host controller spec calls the "system address block").
//   MSM    E7G/clover-sdm660-ksb (Xiaomi's own 4.19 CAF tree for this tablet) main
//          `drivers/mmc/host/sdhci-msm.c` (/tmp/sdhci-research/sdhci-msm.c).
//   JEDEC  the same tree's `include/linux/mmc/mmc.h` (/tmp/sdhci-research/mmc.h).
//   DT     the device's own flattened tree, decompiled into
//          ~/mu-clover/Resources/DTBs/clover.dts (node `sdhci@c0c4000`), cross-checked against
//          the fetched CAF `arch/arm64/boot/dts/vendor/qcom/sdm660.dtsi:1128` and
//          `sdm660-common.dtsi:545`, and against `facts/81ffcaaa-20261022-210928/sdhci.txt`,
//          which is /proc/device-tree read off the running tablet.
//
// This header claims nothing about what the hardware is doing. It only names the places to look.
#ifndef CLOVER_SDHCI_REGS_H
#define CLOVER_SDHCI_REGS_H

// ------------------------------------------------------------------ the windows (DT-verified)
//
// clover.dts `sdhci@c0c4000`: reg = <0xc0c4000 0x1000 0xc0c5000 0x1000>, reg-names =
// "hc_mem\0cmdq_mem"; the CAF sdm660.dtsi adds a third, <0xc0c8000 0x8000> "cqhci_ice".
// /proc/device-tree/soc/sdhci@c0c4000/reg on the tablet reads back 0x0c0c4000/0x1000 and
// 0x0c0c5000/0x1000 (cells printed byte-reversed by the dump), and
// /proc/device-tree/soc/sdcc1ice@c0c8000/reg is 0x0c0c8000/0x8000.
#define kSdhciHostPhys    0xc0c4000ULL   // hc_mem: the SDHCI system address block + vendor core
#define kSdhciHostSize    0x1000U
#define kSdhciCmdqPhys    0xc0c5000ULL   // cmdq_mem / cqhci_mem: command queue host controller
#define kSdhciCmdqSize    0x1000U
#define kSdhciIcePhys     0xc0c8000ULL   // cqhci_ice: the inline-crypto engine
#define kSdhciIceSize     0x8000U
// hc_irq is <0x00 0x6e 0x00> in clover.dts, i.e. GIC_SPI 110 => INTID 142; pwr_irq is SPI 112
// => INTID 144. Round86's panic screen read GICD_TYPER's ITLinesNumber as 19, which bounds this
// GIC at (19+1)*32 = 640 INTIDs, so 142 and 144 are inside the distributor.
#define kSdhciHcIntid     142U
#define kSdhciPwrIntid    144U

// ------------------------------------------------------------- SDHCI system address block
// All names and values are `drivers/mmc/host/sdhci.h`, the "Controller registers" block.
#define SDHCI_DMA_ADDRESS      0x00   // u32; also Argument2 for a 48-bit command, and the
#define SDHCI_ARGUMENT2        SDHCI_DMA_ADDRESS          //   32-bit block count
#define SDHCI_BLOCK_SIZE       0x04   // u16: bits 15:12 DMA boundary, 11:0 block size in bytes
#define SDHCI_BLOCK_COUNT      0x06   // u16
#define SDHCI_ARGUMENT         0x08   // u32: the command argument (Argument1)
#define SDHCI_TRANSFER_MODE    0x0C   // u16
#define SDHCI_COMMAND          0x0E   // u16
#define SDHCI_RESPONSE         0x10   // four u32 words, 0x10/0x14/0x18/0x1C -- see the mapping
#define SDHCI_BUFFER           0x20   // the data FIFO; 32-bit accesses only, see kFifoWords
#define SDHCI_PRESENT_STATE    0x24   // u32
#define SDHCI_HOST_CONTROL     0x28   // u8
#define SDHCI_POWER_CONTROL    0x29   // u8
#define SDHCI_BLOCK_GAP_CONTROL 0x2A  // u8
#define SDHCI_WAKE_UP_CONTROL  0x2B   // u8
#define SDHCI_CLOCK_CONTROL    0x2C   // u16
#define SDHCI_TIMEOUT_CONTROL  0x2E   // u8
#define SDHCI_SOFTWARE_RESET   0x2F   // u8
#define SDHCI_INT_STATUS       0x30   // u32, write-1-to-clear
#define SDHCI_INT_ENABLE       0x34   // u32, the interrupt line mask
#define SDHCI_SIGNAL_ENABLE    0x38   // u32, which of those raise the interrupt line
#define SDHCI_AUTO_CMD_STATUS  0x3C   // u32
#define SDHCI_CAPABILITIES     0x40   // u32 (timeout / base clock / max block / DMA abilities)
#define SDHCI_CAPABILITIES_1   0x44   // u32 (speed modes, driver types, clock multiplier, HS400)
#define SDHCI_MAX_CURRENT      0x48   // u32: 180 mA units in bytes 0, 1, 2 for 3.3/3.0/1.8 V
#define SDHCI_SET_ACMD12_ERROR 0x50   // u8
#define SDHCI_SET_INT_ERROR    0x52   // u8   (host version 4.10)
#define SDHCI_ADMA_ERROR       0x54   // u8
#define SDHCI_ADMA_ADDRESS     0x58   // u32, low in 64-bit mode
#define SDHCI_ADMA_ADDRESS_HI  0x5C   // u32, high in 64-bit mode
#define SDHCI_PRESET_FOR_SDR12  0x66  // u16 ... the preset table, valid when CAPABILITIES_1 says so
#define SDHCI_PRESET_FOR_SDR25  0x68
#define SDHCI_PRESET_FOR_SDR50  0x6A
#define SDHCI_PRESET_FOR_SDR104 0x6C
#define SDHCI_PRESET_FOR_DDR50  0x6E
#define SDHCI_PRESET_FOR_HS400  0x74  // non-standard
#define SDHCI_ADMA3_ADDRESS    0x78
#define SDHCI_HOST_CONTROL2    0x3E   // u16 -- note it shares a word with the 0x3C u32 above
#define SDHCI_SLOT_INT_STATUS  0xFC   // u16
#define SDHCI_HOST_VERSION     0xFE   // u16: 15:8 vendor version, 7:0 specification version

// SDHCI_HOST_CONTROL (0x28, u8)
#define SDHCI_CTRL_LED          0x01
#define SDHCI_CTRL_4BITBUS      0x02
#define SDHCI_CTRL_HISPD        0x04
#define SDHCI_CTRL_DMA_MASK     0x18
#define  SDHCI_CTRL_SDMA        0x00
#define  SDHCI_CTRL_ADMA1       0x08
#define  SDHCI_CTRL_ADMA32      0x10
#define  SDHCI_CTRL_ADMA64      0x18
#define SDHCI_CTRL_8BITBUS      0x20

// SDHCI_POWER_CONTROL (0x29, u8)
#define SDHCI_POWER_ON          0x01
#define SDHCI_POWER_180         0x0A
#define SDHCI_POWER_300         0x0C
#define SDHCI_POWER_330         0x0E

// SDHCI_CLOCK_CONTROL (0x2C, u16)
#define SDHCI_CLOCK_INT_EN      0x0001
#define SDHCI_CLOCK_INT_STABLE  0x0002
#define SDHCI_CLOCK_CARD_EN     0x0004
#define SDHCI_CLOCK_PLL_EN      0x0008
#define SDHCI_PROG_CLOCK_MODE   0x0020
#define SDHCI_DIVIDER_SHIFT     8     // 7:0 first divider stage, 9:8 upper bits of the pair
#define SDHCI_DIV_HI_MASK       0x300

// SDHCI_SOFTWARE_RESET (0x2F, u8) -- every bit here self-clears when the reset finishes
#define SDHCI_RESET_ALL         0x01
#define SDHCI_RESET_CMD         0x02
#define SDHCI_RESET_DATA        0x04

// SDHCI_PRESENT_STATE (0x24, u32)
#define SDHCI_CMD_INHIBIT       0x00000001   // a command is still in progress: do not issue one
#define SDHCI_DATA_INHIBIT      0x00000002   // the data line is still busy
#define SDHCI_DOING_WRITE       0x00000100   // a write transfer is being serviced
#define SDHCI_DOING_READ        0x00000200   // ditto, a read
#define SDHCI_SPACE_AVAILABLE   0x00000400   // the FIFO can take another word
#define SDHCI_DATA_AVAILABLE    0x00000800   // the FIFO holds another word
#define SDHCI_CARD_PRESENT      0x00010000
#define SDHCI_CD_STABLE         0x00020000
#define SDHCI_CD_LVL            0x00040000
#define SDHCI_WRITE_PROTECT     0x00080000
#define SDHCI_DATA_LVL_MASK     0x00F00000   // data[3:0] line levels, 20:23
#define SDHCI_CMD_LVL           0x01000000

// SDHCI_INT_STATUS / _ENABLE / _SIGNAL_ENABLE (0x30/0x34/0x38, u32)
#define SDHCI_INT_RESPONSE      0x00000001   // command complete (the response is in SDHCI_RESPONSE)
#define SDHCI_INT_DATA_END      0x00000002   // transfer complete
#define SDHCI_INT_BLK_GAP       0x00000004
#define SDHCI_INT_DMA_END       0x00000008
#define SDHCI_INT_SPACE_AVAIL   0x00000010
#define SDHCI_INT_DATA_AVAIL    0x00000020
#define SDHCI_INT_CARD_INSERT   0x00000040
#define SDHCI_INT_CARD_REMOVE   0x00000080
#define SDHCI_INT_CARD_INT      0x00000100
#define SDHCI_INT_RETUNE        0x00001000
#define SDHCI_INT_CQE           0x00004000
#define SDHCI_INT_ERROR         0x00008000
#define SDHCI_INT_TIMEOUT       0x00010000   // command CRC/timeout/index errors set this group
#define SDHCI_INT_CRC           0x00020000
#define SDHCI_INT_END_BIT       0x00040000
#define SDHCI_INT_INDEX         0x00080000
#define SDHCI_INT_DATA_TIMEOUT  0x00100000
#define SDHCI_INT_DATA_CRC      0x00200000
#define SDHCI_INT_DATA_END_BIT  0x00400000
#define SDHCI_INT_BUS_POWER     0x00800000
#define SDHCI_INT_AUTO_CMD_ERR  0x01000000
#define SDHCI_INT_ADMA_ERROR    0x02000000
#define SDHCI_INT_TUNING_ERROR  0x04000000
#define SDHCI_INT_CMD_MASK      (SDHCI_INT_RESPONSE | SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | \
                                 SDHCI_INT_END_BIT | SDHCI_INT_INDEX | SDHCI_INT_AUTO_CMD_ERR)
#define SDHCI_INT_DATA_MASK     (SDHCI_INT_DATA_END | SDHCI_INT_DMA_END | SDHCI_INT_DATA_AVAIL | \
                                 SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_TIMEOUT | \
                                 SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT | \
                                 SDHCI_INT_ADMA_ERROR)

// SDHCI_TRANSFER_MODE (0x0C, u16)
#define SDHCI_TRNS_DMA          0x0001   // clear it for FIFO (PIO) transfers
#define SDHCI_TRNS_BLK_CNT_EN   0x0002
#define SDHCI_TRNS_AUTO_CMD12   0x0004
#define SDHCI_TRNS_AUTO_CMD23   0x0008
#define SDHCI_TRNS_READ         0x0010   // set for a read, clear for a write
#define SDHCI_TRNS_MULTI        0x0020

// SDHCI_COMMAND (0x0E, u16)   SDHCI_MAKE_CMD(index, flags) is the upstream spelling.
#define SDHCI_CMD_RESP_MASK     0x03
#define  SDHCI_CMD_RESP_NONE    0x00
#define  SDHCI_CMD_RESP_LONG    0x01     // R2, the 136-bit CID/CSD
#define  SDHCI_CMD_RESP_SHORT   0x02     // R1/R6/R7 and R1b
#define  SDHCI_CMD_RESP_SHORT_BUSY 0x03  // R4/R5 and R1b-as-busy spelling
#define SDHCI_CMD_CRC           0x08     // enable command CRC checking
#define SDHCI_CMD_INDEX         0x10     // enable command-index checking
#define SDHCI_CMD_DATA          0x20     // this command carries a data transfer
#define SDHCI_CMD_ABORTCMD      0xC0
#define SDHCI_MAKE_CMD(index, flags) ((((index) & 0xff) << 8) | ((flags) & 0xff))

// SDHCI_HOST_CONTROL2 (0x3E, u16)
#define SDHCI_CTRL_UHS_MASK     0x0007
#define  SDHCI_CTRL_UHS_SDR12   0x0000
#define  SDHCI_CTRL_UHS_SDR25   0x0001
#define  SDHCI_CTRL_UHS_SDR50   0x0002
#define  SDHCI_CTRL_UHS_SDR104  0x0003
#define  SDHCI_CTRL_UHS_DDR50   0x0004
#define  SDHCI_CTRL_HS400       0x0005   // non-standard
#define SDHCI_CTRL_VDD_180      0x0008
#define SDHCI_CTRL_DRV_TYPE_MASK 0x0030
#define SDHCI_CTRL_EXEC_TUNING  0x0040
#define SDHCI_CTRL_TUNED_CLK    0x0080
#define SDHCI_CTRL_PRESET_VAL_ENABLE 0x8000

// SDHCI_CAPABILITIES (0x40, u32), the fields a dump has to be read against
#define SDHCI_TIMEOUT_CLK_MASK  0x0000003F
#define SDHCI_TIMEOUT_CLK_UNIT  0x00000080
#define SDHCI_CLOCK_BASE_MASK   0x0000FF00   // 15:8, MHz; the 6-bit 13:8 field is SDHCI 1.0 only
#define SDHCI_CLOCK_BASE_SHIFT  8
#define SDHCI_MAX_BLOCK_MASK    0x00030000
#define SDHCI_CAN_DO_8BIT       0x00040000
#define SDHCI_CAN_DO_ADMA2      0x00080000
#define SDHCI_CAN_DO_ADMA1      0x00100000
#define SDHCI_CAN_DO_HISPD      0x00200000
#define SDHCI_CAN_DO_SDMA       0x00400000
#define SDHCI_CAN_DO_SUSPEND    0x00800000
#define SDHCI_CAN_VDD_330       0x01000000
#define SDHCI_CAN_VDD_300       0x02000000
#define SDHCI_CAN_VDD_180       0x04000000
#define SDHCI_CAN_64BIT         0x10000000
#define SDHCI_CAP2_SDR50_NEEDS_TUNING 0x00002000   // SDHCI_USE_SDR50_TUNING
#define SDHCI_CAP2_HS400_SUPPORT 0x80000000

// The response words, from sdhci.c's sdhci_read_rsp_136() and sdhci_finish_command(): a 48- or
// 136-bit response is held most-significant-word-first at 0x1C down to 0x10 (so a 32-bit R1 is
// simply the word at SDHCI_RESPONSE), and for the 136-bit form the CRC byte is stripped, which
// is why Linux shifts every word left by 8 and pulls in the next word's top byte.
#define SDHCI_RSP_R1_OFFSET     0x10

// ------------------------------------------------------------------- Qualcomm SDCC extensions
//
// sdhci-msm.c keeps the vendor registers behind one struct of offsets and instantiates it two
// ways, picked by whether the SDCC has its "MCI" block merged into the host window:
//
//   sdhci_msm_offset_mci_removed  (sdhci-msm.c:206) -- read relative to the SDHCI register base
//   sdhci_msm_offset_mci_present  (sdhci-msm.c:239) -- read relative to a separate core_mem
//
// The two tables are reproduced here because a dump has to read both candidate places to find
// out which one this SDCC is: an offset that is not a register reads as zero or as the register
// that happens to live there, and the version words are what settles it.
#define MSM_V5_MCI_DATA_CNT      0x35C   // mci_removed: 0x35C, mci_present: 0x030
#define MSM_V5_MCI_STATUS        0x324   //              0x324,                0x034
#define MSM_V5_GENERICS          0x320   //              0x320,                0x070
#define MSM_V5_MCI_VERSION       0x318   //              0x318,                0x050
#define MSM_V5_FIFO_CNT          0x308   //              0x308,                0x044
#define MSM_V5_DLL_CONFIG        0x200   //              0x200,                0x100
#define MSM_V5_DLL_STATUS        0x208   //              0x208,                0x108
#define MSM_V5_VENDOR_SPEC       0x20C   //              0x20C,                0x10C
#define MSM_V5_VENDOR_SPEC2      0x210   // FUNC2:       0x210,                0x110
#define MSM_V5_DDR_200_CFG       0x224   //              0x224,                0x184
#define MSM_V5_DLL_CONFIG_2      0x254   //              0x254,                0x1B4
#define MSM_V5_DDR_CONFIG        0x25C   //              --,                   0x1BC
#define MSM_V5_DLL_CONFIG_3      0x258   //              0x258,                0x1B8

// DLL/DDR status bits (sdhci-msm.c:100-160 region and the CDR/DLL helpers).
#define CORE_DLL_LOCK_STATE      (1 << 2)   // DLL_STATUS: the delay line locked
#define CORE_DLL_TEST_DBOUT      (1 << 3)
#define CORE_DLL_CLOCK_DISABLE   (1 << 21)
#define CORE_DLL_PWR_SAVE_EN     (1 << 1)
#define CORE_CK_OUT_EN           (1 << 4)   // DLL_CONFIG
#define CORE_DLL_LOCK            (1 << 5)   // DLL_CONFIG: DLL_READY_STATUS lives in DLL_STATUS
#define CORE_DLL_RST             (1 << 0)
#define CORE_MCI_STATUS_CMD_CFG  (1 << 2)

// ------------------------------------------------------------------- the card's own commands
//
// From the CAF tree's include/linux/mmc/mmc.h:31-86. The suffix comment is the JEDEC type:
// bc/bcr = broadcast with/without response, ac = addressed command, adtc = addressed with data.
#define MMC_GO_IDLE_STATE          0   /* bc                        */
#define MMC_SEND_OP_COND           1   /* bcr  [31:0] OCR       R3   */
#define MMC_ALL_SEND_CID           2   /* bcr                     R2 */
#define MMC_SET_RELATIVE_ADDR      3   /* ac   [31:16] RCA        R1 */
#define MMC_SWITCH                 6   /* ac   [31:0] see below  R1b */
#define MMC_SELECT_CARD            7   /* ac   [31:16] RCA        R1 */
#define MMC_SEND_EXT_CSD           8   /* adtc                    R1 */
#define MMC_SEND_CSD               9   /* ac   [31:16] RCA        R2 */
#define MMC_SEND_CID              10   /* ac   [31:16] RCA        R2 */
#define MMC_STOP_TRANSMISSION     12   /* ac                      R1b */
#define MMC_SEND_STATUS           13   /* ac   [31:16] RCA        R1 */
#define MMC_SET_BLOCKLEN          16   /* ac   [31:0] length      R1 */
#define MMC_READ_SINGLE_BLOCK     17   /* adtc [31:0] address     R1 */
#define MMC_READ_MULTIPLE_BLOCK   18   /* adtc [31:0] address     R1 */
#define MMC_WRITE_BLOCK           24   /* adtc [31:0] address     R1 */
#define MMC_APP_CMD               55   /* ac   [31:16] RCA        R1 */

// The R1 response's own fields (mmc.h:154-168): the current state is five bits at 9:13.
#define R1_CURRENT_STATE(x)        (((x) & 0x00001E00) >> 9)
#define R1_STATE_IDLE              0
#define R1_STATE_READY             1
#define R1_STATE_IDENT             2
#define R1_STATE_STBY              3
#define R1_STATE_TRAN              4
#define R1_STATE_DATA              5
#define R1_STATE_RCV               6
#define R1_STATE_PRG               7
#define R1_STATE_DIS               8
#define R1_READY_FOR_DATA          (1 << 8)
#define R1_APP_CMD                 (1 << 5)

#endif
