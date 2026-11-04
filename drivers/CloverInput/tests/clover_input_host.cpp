// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the host model for drivers/CloverInput/InputCore.h -- run by
// scripts/test-clover-input.py, which is run by scripts/check-clover.py's "# Round318." block rather
// than merely asserted to exist.
//
// Every case here is one of three kinds, and the kind is said in the comment above it:
//   PRESERVE  -- the arithmetic CloverInput.cpp performed inline before this layer existed, pinned so
//                the refactor cannot silently change a `clover r181:` field.
//   MEASURE   -- a number taken from a real reading on the card or from a real source file, so the
//                claim is checked against something that is not this repository's opinion.
//   DIRECTION -- both branches of a guard, run to see what the WRONG one would have reported. Round313
//                established that this is the only way to learn whether a misread fails loudly or
//                quietly, and this file has the same shape twice: a pin index that wraps and a mask
//                that hides.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../InputCore.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));             \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, want, what)                                           \
    do {                                                                       \
        g_checks++;                                                            \
        if ((actual) != (want)) {                                              \
            g_failures++;                                                      \
            printf("FAIL %s:%d: %s -- got %llu want %llu\n", __FILE__, __LINE__, \
                (what), (unsigned long long) (actual), (unsigned long long) (want)); \
        }                                                                      \
    } while (0)

static void check_pin_map(void)
{
    uint32_t offset = 0xdeadbeef;

    // PRESERVE: gpio10's CTL is at window offset 0 and gpio11's at one pin stride, which is what the
    // two-window map in the driver has always relied on.
    CHECK_EQ(kInputPinSda, 10u, "sda pin number derives to 10");
    CHECK_EQ(kInputPinScl, 11u, "scl pin number derives to 11");
    CHECK_EQ(kInputPinRst, 66u, "reset pin number derives to 66");
    CHECK_EQ(kInputPinIrq, 67u, "irq pin number derives to 67");
    CHECK_EQ(kInputPinsPerWindow, 2u, "two pins per mapped pair");
    CHECK(input_pin_offset(kInputPinSda, kPinCtl, &offset) && offset == 0x000u, "gpio10 CTL");
    CHECK(input_pin_offset(kInputPinSda, kPinIo, &offset) && offset == 0x004u, "gpio10 IO");
    CHECK(input_pin_offset(kInputPinScl, kPinCtl, &offset) &&
          offset == kTlmmPinStride + kPinCtl, "gpio11 CTL");
    CHECK(input_pin_offset(kInputPinIrq, kPinIntrStatus, &offset) &&
          offset == kTlmmPinStride + kPinIntrStatus, "gpio67 INTR_STATUS");
    CHECK_EQ((unsigned) input_pin_pair_of(kInputPinSda), (unsigned) kInputPairI2c, "sda is the i2c pair");
    CHECK_EQ((unsigned) input_pin_pair_of(kInputPinIrq), (unsigned) kInputPairTouch, "irq is the touch pair");

    // DIRECTION: the pre-layer expression `(pin <= 11) ? 10 : 66` sends pin 12 into the touch window and
    // subtracts 66 from it in unsigned arithmetic. Run that expression to see what it produced rather
    // than assert it was bad: it is an offset of roughly four tebibytes, and the window is 0x2000.
    {
        const uint32_t old_style = (12u - 66u) * kTlmmPinStride + kPinCtl;
        CHECK(old_style > kPinPairSize, "the pre-layer pin-12 offset is far outside the window");
        CHECK(!input_pin_offset(12u, kPinCtl, &offset), "pin 12 is refused by pair, not by size");
        CHECK(!input_pin_offset(65u, kPinCtl, &offset), "pin 65 sits below the touch pair and is refused");
        CHECK(!input_pin_offset(68u, kPinCtl, &offset), "pin 68 sits above it and is refused");
        CHECK(!input_pin_offset(kInputPinSda, kPinCtl, 0), "a null out pointer is refused, not written");
    }
}

static void check_rcg(void)
{
    struct InputRcg rcg;

    // MEASURE: F(19200000, P_XO, 1, 0, 0) is the table's first row and the row programRcg() writes.
    input_rcg_decode(input_rcg_xo_cfg(0u), &rcg);
    CHECK_EQ((unsigned long long) rcg.hz, (unsigned long long) kXoHz, "XO row is 19.2 MHz");
    CHECK(strcmp(rcg.parent, "xo") == 0, "XO row names xo");
    CHECK(rcg.known && rcg.div == kRcgDiv1 && rcg.src == kRcgSrcXo, "XO row fields");

    // PRESERVE: a zero divisor on the XO row is read as 1 (the driver's `div ? div : 1`), but a zero
    // divisor on a GPLL row is no rate at all -- the two branches were different and still are.
    input_rcg_decode((uint32_t) (kRcgSrcXo << kRcgCfgSrcShift), &rcg);
    CHECK_EQ((unsigned long long) rcg.hz, (unsigned long long) kXoHz, "XO with div 0 is XO");
    input_rcg_decode((uint32_t) (kRcgSrcGpll0 << kRcgCfgSrcShift), &rcg);
    CHECK_EQ((unsigned long long) rcg.hz, 0u, "GPLL0 with div 0 claims no rate");
    CHECK(rcg.known, "GPLL0 with div 0 is still a named parent");

    // MEASURE: F(50000000, P_GPLL0, 12, 0, 0) is the row that makes GPLL0 600 MHz by arithmetic
    // (gcc-sdm660.c's ftbl, quoted in InputRegs.h:87-91).
    input_rcg_decode((uint32_t) ((kRcgSrcGpll0 << kRcgCfgSrcShift) | 12u), &rcg);
    CHECK_EQ((unsigned long long) rcg.hz, 50000000u, "GPLL0 / 12 is 50 MHz");

    // PRESERVE: selector 6 is gpll0_early_div, and any other selector names nothing and claims nothing.
    input_rcg_decode((uint32_t) (6u << kRcgCfgSrcShift), &rcg);
    CHECK(strcmp(rcg.parent, "gpll0_early_div") == 0 && rcg.known, "selector 6 is named");
    input_rcg_decode((uint32_t) (3u << kRcgCfgSrcShift | 7u), &rcg);
    CHECK(!rcg.known && rcg.hz == 0 && strcmp(rcg.parent, "unknown") == 0,
        "a selector outside the map invents no rate");

    // PRESERVE: the writable-field mask programRcg() applies, including leaving the mode bits alone
    // only where kRcgCfgWritable covers them.
    {
        const uint32_t dirty = kRcgCfgSrcMask | kRcgCfgDivMask | kRcgCfgModeMask | kRcgCfgHwCtrl |
            0x80000000u;
        const uint32_t next = input_rcg_xo_cfg(dirty);
        CHECK((next & kRcgCfgHwCtrl) == 0 && (next & kRcgCfgModeMask) == 0,
            "the XO write clears hw_ctrl and mode");
        CHECK_EQ(next & 0x80000000u, 0x80000000u, "a bit outside the writable mask is left alone");
        CHECK(input_rcg_update_pending(input_rcg_update_word(0u)), "CMD_UPDATE reads as pending");
        CHECK(!input_rcg_update_pending(0x80u), "CMD_UPDATE self-clears to nothing pending");
    }
}

static void check_branch_polarity(void)
{
    // PRESERVE: BRANCH_HALT's polarity -- 1 means halted -- and the NOC FSM escape that a voted branch
    // reports through instead. These two are the reason `gfx3d_en=1` in the GPU line never meant the
    // clock ran, and the reason this driver's ahb/branch rows mean what they say.
    CHECK(input_branch_halted(kCbcrClkOff), "CLK_OFF set with FSM 0 is halted");
    CHECK(!input_branch_halted(0u), "CLK_OFF clear is running");
    CHECK(!input_branch_halted(kCbcrClkOff | (kCbcrFsmOn << kCbcrFsmShift)),
        "CLK_OFF set but FSM ON is running: the voted-branch escape");
    CHECK(input_branch_halted(kCbcrClkOff | (1u << kCbcrFsmShift)), "FSM 1 is not ON");
    CHECK(input_branch_requested(kCbcrEnable), "BIT(0) is the enable request");
    CHECK(!input_branch_requested(kCbcrClkOff), "a set CLK_OFF says nothing about the request");
    // The enable write is read-modify-write over the same word the poll then watches, so its only
    // effect must be the one bit -- a cleared neighbour here would reprogram a divider nobody asked
    // about.
    CHECK_EQ(input_branch_enable_word(0x00000000u), (uint32_t) kCbcrEnable,
        "the enable word on a zero register is just the bit");
    CHECK_EQ(input_branch_enable_word(0x80000001u), 0x80000001u,
        "and on a word with everything else set it adds nothing (BIT(0) already set)");

    // The 0xffffffff case, which is what an unanswering window reads as: CLK_OFF is SET there, so the
    // halted test comes back "halted" and nothing mistakes a dead page for a running clock -- the same
    // shape round317 had to prove about the GDSC word, where bit 31 of 0xffffffff is what makes a naive
    // PWR_ON test say "powered".
    CHECK(input_branch_halted(0xffffffffu),
        "0xffffffff is read as halted: FSM 7 is not ON and bit 31 is set");
    CHECK_EQ(input_branch_fsm(0xffffffffu), 7u, "and its FSM field is the all-ones value, not a state");
    CHECK(input_ahb_voted(0xffffffffu),
        "an unanswering page DOES read as voted -- only the literal 0xffffffff test separates the two");
    CHECK(input_ahb_voted(1u << kAhbVoteBit), "BIT(17) is the AHB vote");
    CHECK_EQ(input_ahb_vote_word(0u) & (1u << kAhbVoteBit), 1u << kAhbVoteBit, "the vote sets one bit");
    CHECK_EQ(input_ahb_vote_word(0x1234u), 0x1234u | (1u << kAhbVoteBit), "the vote keeps the rest");
}

static void check_pin_words(void)
{
    struct InputPinState state;

    // PRESERVE: i2c_3_active writes mux, pull and drive only. Start from a CTL the boot left dirty in
    // every field, and require OE and the unknown upper bits to survive untouched -- the driver never
    // touches OE because the QUP drives these lines open-drain.
    {
        const uint32_t dirty = (3u << kPinMuxShift) | kPinPullUp | (5u << kPinDrvShift) |
            (1u << kPinOeBit) | 0xf0000000u;
        const uint32_t next = input_i2c_pin_ctl(dirty);
        CHECK_EQ((next & kPinMuxMask) >> kPinMuxShift, kPinFuncBlspI2c3, "mux becomes blsp_i2c3");
        CHECK_EQ(next & kPinPullMask, kPinPullNone, "bias becomes disable");
        CHECK_EQ((next & (kPinDrvMask << kPinDrvShift)) >> kPinDrvShift, kPinDrv2mA, "drive becomes 2 mA");
        CHECK(next & (1u << kPinOeBit), "OE is inherited, not cleared");
        CHECK_EQ(next & 0xf0000000u, 0xf0000000u, "fields the state does not name are left alone");
    }

    // PRESERVE: pinctrl-msm's two directions. Output sets the IO latch and OE; input clears OE and
    // touches nothing else. Both force mux to gpio.
    CHECK_EQ(input_pin_output_io(0u, 1u) & (1u << kPinOutBit), 1u << kPinOutBit, "output high sets latch");
    CHECK_EQ(input_pin_output_io(1u << kPinOutBit, 0u), 0u, "output low clears latch");
    CHECK_EQ(input_pin_output_io(1u << kPinInBit, 1u) & (1u << kPinInBit), 1u << kPinInBit,
        "the input status bit survives a latch write");
    CHECK_EQ(input_pin_output_ctl(kPinPullUp) & (1u << kPinOeBit), 1u << kPinOeBit, "output sets OE");
    CHECK_EQ(input_pin_output_ctl(3u << kPinMuxShift) & kPinMuxMask, 0u, "output forces mux to gpio");
    CHECK_EQ(input_pin_input_ctl((1u << kPinOeBit) | kPinPullUp) & (1u << kPinOeBit), 0u,
        "input clears OE");
    CHECK_EQ(input_pin_input_ctl((1u << kPinOeBit) | kPinPullUp) & kPinPullMask, kPinPullUp,
        "input leaves the bias alone -- the DT's bias-disable is a different state");
    CHECK_EQ((input_pin_drive_ctl(0u, kTsIntDrive) & (kPinDrvMask << kPinDrvShift)) >> kPinDrvShift,
        kPinDrv16mA, "ts_int_defalut's 16 mA");

    // MEASURE: drive reg value r means (r+1)*2 mA, and the DT's two asks are the endpoints of the
    // range pinctrl-msm.c:470-473 admits (2..16 mA in even steps).
    CHECK_EQ(input_pin_drive_ma(kPinDrv2mA), 2u, "raw 0 is 2 mA");
    CHECK_EQ(input_pin_drive_ma(kPinDrv8mA), 8u, "raw 3 is 8 mA");
    CHECK_EQ(input_pin_drive_ma(kPinDrv16mA), 16u, "raw 7 is 16 mA");

    input_pin_decode((2u << kPinMuxShift) | (kPinDrv16mA << kPinDrvShift) | (1u << kPinOeBit),
        1u << kPinInBit, (7u << kPinIntrTargetShift) | (2u << kPinIntrDetectShift) |
        kPinIntrPolarity | kPinIntrEnable, kPinIntrRawBit | kPinIntrStatusBit, &state);
    CHECK_EQ(state.mux, kPinFuncBlspI2c3, "decoded mux");
    CHECK_EQ(state.drive_raw, kPinDrv16mA, "decoded drive raw");
    CHECK_EQ(state.drive_ma, 16u, "decoded drive in mA");
    CHECK_EQ(state.oe, 1u, "decoded OE");
    CHECK_EQ(state.in_level, 1u, "decoded input level from the IO register");
    CHECK_EQ(state.out_level, 0u, "decoded output latch from the IO register");
    CHECK_EQ(state.intr_target, 7u, "decoded interrupt target");
    CHECK_EQ(state.intr_detect, 2u, "decoded interrupt detection");
    CHECK_EQ(state.intr_polarity, 1u, "decoded falling/edge polarity");
    CHECK_EQ(state.intr_raw, 1u, "decoded raw status");
    CHECK_EQ(state.intr_status_bit, 1u, "decoded latched status");

    // The target field's own geometry. Round181 shipped a mask naming bits [12:8] against a shift
    // naming bit 5, and this is the check that would have refused it: the mask must be the width at the
    // shift, and it must not touch any other field in the same word.
    CHECK_EQ(kPinIntrTargetMask, 0xE0u, "INTR_CFG[7:5]");
    CHECK_EQ(kPinIntrTargetMask >> kPinIntrTargetShift, (1u << kPinIntrTargetWidth) - 1u,
        "the mask is exactly the width, shifted by exactly the shift");
    CHECK_EQ(kPinIntrTargetMask & (3u << kPinIntrDetectShift), 0u,
        "target and detection are different bits");
    CHECK_EQ(kPinIntrTargetMask & (kPinIntrPolarity | kPinIntrEnable), 0u,
        "target and polarity/enable are different bits");

    // MEASURE: the value a correctly routed pin should show. .intr_target_kpss_val = 3
    // (pinctrl-sdm660.c:65) is what the driver writes to send the interrupt to the Apps processor, so
    // a cfg word carrying the vendor's own routing decodes as 3 and not as 96 or 0.
    {
        struct InputPinState routed;
        input_pin_decode(0u, 0u, (3u << kPinIntrTargetShift) | kPinIntrEnable, 0u, &routed);
        CHECK_EQ(routed.intr_target, 3u, "the kpss target value round-trips through the field");
        CHECK_EQ(routed.intr_enable, 1u, "and the enable bit is still its own bit");
    }

    // DIRECTION: the round181 expression applied to the same words. Two things it does, both silent: a
    // correctly routed pin (target 3 at bits [7:5]) reports 0, which reads as "no target configured";
    // and any bit set between 8 and 12 -- whatever those bits are, the two trees on disk do not name
    // them -- is reported as that bit moved down by five, so the column can print 8 or 120 for a field
    // whose entire range is 0..7. Neither is an error, so only running both branches shows the old
    // column could not have been answering the question its name asks.
    CHECK_EQ((0xE0u & 0x1F00u) >> kPinIntrTargetShift, 0u,
        "the old mask cut a correctly-routed pin down to zero");
    CHECK_EQ((0x100u & 0x1F00u) >> kPinIntrTargetShift, 8u,
        "one bit above the field reports 8, which three bits cannot hold");
    CHECK_EQ((0xF00u & 0x1F00u) >> kPinIntrTargetShift, 120u,
        "and four of them report 120, against a field range of 0..7");
    CHECK_EQ(kPinIntrTargetMask & 0x1F00u, 0u,
        "the corrected mask and the old one have no bit in common, so the two columns are not merely "
        "different, they are reading different registers' worth of bits");

    // The mux numbering the driver's own pin programming rests on. pinctrl-sdm660.c's enum
    // sdm660_functions (:372) puts blsp_i2c3 at ordinal 30; the field a pin's function lives in is
    // 4 bits (mux_bit 2, GENMASK(5,2) = 0x3c). So the ordinal reading is arithmetically impossible --
    // it does not fit the hole it is supposed to be written into -- and the index reading, which is
    // what pinctrl-msm-mainline.c:234-237 actually writes, is the only one left. This check is that
    // argument, so the choice is on the disk rather than in a sentence.
    CHECK(((30u << kPinMuxShift) & ~kPinMuxMask) != 0u,
        "the SoC-wide function ordinal for blsp_i2c3 spills out of a 4-bit mux field");
    CHECK(((kPinFuncBlspI2c3 << kPinMuxShift) & ~kPinMuxMask) == 0u,
        "the pin's own index fits, and fits without touching the pull or drive bits");

    // One expression, not two: the struct field and the single-field helper the driver calls on the
    // failing path must cut the same bits, and RAW_STATUS_EN (INTR_CFG bit 4, just below the target
    // field) must survive a target being set.
    {
        struct InputPinState one;
        const uint32_t cfg = (3u << kPinIntrTargetShift) | kPinIntrRawBit | kPinIntrEnable;
        input_pin_decode(0u, 0u, cfg, 0u, &one);
        CHECK_EQ(one.intr_target, input_pin_intr_target(cfg),
            "the decoded struct and the helper agree");
        CHECK_EQ(input_pin_raw_status_enabled(cfg), 1u,
            "RAW_STATUS_EN is bit 4 of INTR_CFG and the target field does not eat it");
        CHECK_EQ(input_pin_intr_target(cfg), 3u, "the target itself is still 3");
        CHECK_EQ(input_pin_raw_status_enabled(3u << kPinIntrTargetShift), 0u,
            "and it is off in a word that only carries a target");
    }

    input_pin_decode(0u, 0u, 0u, 0u, 0);   // the null-out path must not write anything
}

static void check_fifo_sizes(void)
{
    uint32_t out_fifo = 0, in_fifo = 0;

    // MEASURE: boot 48's own IO_MODE read, `io=c0a5`, from the r260 beat row in
    // notes/development-log.md's r260 reading. Both FIFOs come out at 64 bytes, which is what makes the
    // OUT_SERVICE divergence inert for every request this driver issues.
    input_fifo_sizes(0xc0a5u, &out_fifo, &in_fifo);
    CHECK_EQ(out_fifo, 64u, "boot 48 io=c0a5 gives a 64-byte output FIFO");
    CHECK_EQ(in_fifo, 64u, "boot 48 io=c0a5 gives a 64-byte input FIFO");

    // The block-size index is 2 bits, so index 3 is reachable and has no row in blk_sizes[] = {4,16,32}.
    // It must be zero, which the driver prints as a size it cannot vouch for, rather than reading past
    // the table.
    CHECK_EQ(input_block_size(0u), 4u, "blk_sizes[0]");
    CHECK_EQ(input_block_size(2u), 32u, "blk_sizes[2]");
    CHECK_EQ(input_block_size(3u), 0u, "a 2-bit index with no table row yields no size");
    input_fifo_sizes((uint32_t) (1u << 0), &out_fifo, 0);
    CHECK_EQ(out_fifo, 16u * 2u, "out block 1 with fifo field 0 is 32");
    input_fifo_sizes(0u, 0, &in_fifo);
    CHECK_EQ(in_fifo, 4u * 2u, "an IO_MODE of all zeros still describes 8 bytes");
}

static void check_state_acceptance(void)
{
    // PRESERVE: qup_i2c_change_state() requires BOTH the field and STATE_VALID. The negative case is the
    // one that matters: a state register that already contains the wanted value but says "not valid"
    // must keep the loop spinning, because it is the boot's residue, not the core's answer.
    CHECK(input_state_reached(kQupStateValid | kQupStateRun, kQupStateRun), "run with valid is reached");
    CHECK(!input_state_reached(kQupStateRun, kQupStateRun), "run without valid is refused");
    CHECK(!input_state_reached(kQupStateValid | kQupStatePause, kQupStateRun), "pause is not run");
    CHECK(input_state_reset_done(kQupStateValid), "reset is state 0, so validity is the whole test");
    CHECK(!input_state_reset_done(kQupStateValid | kQupStateRun), "a running core is not in reset");
    CHECK(input_core_tags_taken(kQupI2cMastGen | kQupStateValid), "BIT(4) plus valid means the tag mode took");
    CHECK(!input_core_tags_taken(kQupI2cMastGen), "BIT(4) without valid is not an answer");
    CHECK_EQ(input_core_config(0), (uint32_t) (kI2cMiniCore | kI2cNValV2 | kQupNoInput),
        "a write-only segment sets NO_INPUT");
    CHECK_EQ(input_core_config(1), (uint32_t) (kI2cMiniCore | kI2cNValV2),
        "a reading segment names the mini-core with 8-bit FIFO entries and no NO_INPUT");
    CHECK((input_core_config(0) ^ input_core_config(1)) == kQupNoInput,
        "NO_INPUT is the only difference between the two");
    // Bring-up names the block once, before any transfer, so it must write the word WITHOUT the
    // per-transfer no-input bit -- the round181 probe wrote exactly these two bits, nothing else.
    CHECK_EQ(input_core_mode_word(), (uint32_t) (kI2cMiniCore | kI2cNValV2),
        "the bring-up word is mini-core plus v2 tags and no no-input bit");
    CHECK_EQ(input_core_config(1), input_core_mode_word(), "a reading segment adds nothing to it");
    CHECK_EQ(input_core_config(0), (uint32_t) (input_core_mode_word() | kQupNoInput),
        "and a write-only segment is that word plus the one bit");
    CHECK_EQ(input_done_mask(0), (uint32_t) kQupMxOutputDone, "a write-only segment waits on output done");
    CHECK_EQ(input_done_mask(4), (uint32_t) kQupMxInputDone, "a reading segment waits on input done");
    // BLOCKING: the deadline the poll loop uses, named once, and what it is at this board's tick.
    CHECK_EQ(kInputXferTimeoutMs, 100u, "the 100 ms deadline round181 shipped, now a named constant");
    CHECK_EQ(input_ticks_for_ms(kInputXferTimeoutMs, 19200u), 1920000ULL,
        "100 ms at 19,200 ticks/ms is 1,920,000 ticks -- one division cannot turn it into 0");
}

static void check_tx_cursor(void)
{
    struct InputTx tx;
    uint8_t bytes[12];
    uint32_t word = 0;
    uint32_t words = 0;
    uint32_t i;

    // The largest write segment this driver ever builds: the Goodix register address is two bytes and
    // the v2 tag block is four, so 6 bytes go into the output FIFO for a `probe goodix gt9xx @5d`.
    bytes[0] = 0x81; bytes[1] = 0xba; bytes[2] = 0x82; bytes[3] = 0x02;
    bytes[4] = 0x81; bytes[5] = 0x40;
    for (i = 6u; i < sizeof (bytes); i++) {
        bytes[i] = (uint8_t) (0x10u + i);      // filler for the oversized case below only
    }

    // PRESERVE: six bytes into a 64-byte FIFO is two words, the second zero-padded, in one pass. This is
    // the shape boot 48 actually exercised.
    input_tx_reset(&tx, 64u);
    while (input_tx_next(&tx, bytes, 6u, &word)) {
        if (words == 0u) {
            CHECK_EQ(word, 0x0282ba81u, "first word is the four tag bytes low-byte-first");
        } else {
            CHECK_EQ(word, 0x00004081u, "final short word is zero-padded above the real bytes");
        }
        words++;
    }
    CHECK_EQ(words, 2u, "six bytes are two words");
    CHECK_EQ(tx.sent, 6u, "the cursor consumed exactly the message");
    CHECK_EQ(tx.room, 64u - 8u, "room is accounted per word, not per byte");
    CHECK_EQ(input_tx_room_left(64u, tx.sent), 58u, "and the driver's own room expression still has 58");
    CHECK_EQ(input_tx_room_left(4u, 6u), 0u, "a message longer than the FIFO reports no room, not a wrap");

    // DIRECTION: the smallest FIFO the IO_MODE field can describe is 8 bytes (block 4 x (2 << 0)). A
    // 10-byte message does not fit, and the cursor must stop at 8 bytes sent rather than overrun.
    input_tx_reset(&tx, 8u);
    words = 0;
    while (input_tx_next(&tx, bytes, 10u, &word)) {
        words++;
    }
    CHECK_EQ(words, 2u, "an 8-byte FIFO takes exactly two words");
    CHECK_EQ(tx.sent, 8u, "two bytes of a 10-byte message are left for the service flag");
    CHECK(!input_tx_next(&tx, bytes, 10u, &word), "and then it stops rather than writing past the FIFO");

    // The upstream oracle, written here from i2c-qup-mainline.c:1288-1300 (a per-byte tx_fifo_free and a
    // byte accumulator carried across calls), to show the two policies put the SAME bytes in the FIFO for
    // every request this driver issues. This is not the driver's algorithm; it is a check on it.
    {
        uint32_t fifo_free = 64u;
        uint32_t pos = 0, len = 6u, produced = 0;
        uint32_t accumulator = 0;
        uint32_t upstream_words[4] = { 0, 0, 0, 0 };
        while (pos < len && fifo_free) {
            accumulator |= (uint32_t) bytes[pos] << ((pos % 4u) * 8u);
            pos++;
            fifo_free--;
            if (pos % 4u == 0u) {
                upstream_words[produced++] = accumulator;
                accumulator = 0;
            }
        }
        if (pos % 4u) {
            upstream_words[produced++] = accumulator;
        }
        CHECK_EQ(produced, 2u, "upstream's byte-wise path also emits two words");
        CHECK_EQ(upstream_words[0], 0x0282ba81u, "and the same first word");
        CHECK_EQ(upstream_words[1], 0x00004081u, "and the same padded last word");
    }

    CHECK_EQ(input_pack_word(bytes, 0u), 0u, "a zero-length chunk is a zero word, not garbage");
    CHECK_EQ(input_pack_word(bytes, 5u), 0u, "more than four bytes is refused outright");
    CHECK_EQ(input_pack_word(0, 4u), 0u, "a null buffer is refused outright");
}

static void check_rx_cursor(void)
{
    struct InputRx rx;
    uint8_t out[6];
    uint32_t popped = 0;

    memset(out, 0xee, sizeof (out));

    // PRESERVE: a v2 read echoes READ_RX_TAGS_LEN tag bytes back inside the first delivered word, so a
    // 1-byte read arrives as [tag0 tag1 data0 0] in one word and the data is the third byte.
    input_rx_reset(&rx, 1u, kQupRxTagLen);
    CHECK(input_rx_needs_word(&rx), "an empty cursor wants a word");
    input_rx_word(&rx, 0x00112233u);          // tag0=0x33 tag1=0x22 data0=0x11
    CHECK(!input_rx_needs_word(&rx), "a held word is consumed before asking for another");
    while (rx.got < rx.want && popped < 16u) {
        input_rx_pop(&rx, out);
        popped++;
    }
    CHECK_EQ(popped, 3u, "two tags then one byte: three pops for one byte of data");
    CHECK_EQ(out[0], 0x11u, "the data byte is the third byte of the word");
    CHECK(input_rx_complete(&rx), "the cursor is done");

    // A 6-byte read with the two echoed DATARD tags is 8 wire bytes, so it arrives as two delivered words:
    // the tags dropped at the front, the FIFO padding never reaching the caller's buffer. The six bytes
    // here are buf[2..7] of the buffer gtp_get_fw_info reads (gt9xx.c:1222-1244) -- 'G','T','9' then the
    // NUL that is that function's own 3-character-pid test, then the two version digits it prints as
    // %02X%02X. Same shape, real fields, so a reader can check the drop against a row the vendor prints.
    {
        const uint32_t delivered[2] = { 0x54478685u,   /* tag 0x85, tag 0x86, 'G', 'T' */
                                        0x30310039u }; /* '9', NUL, '1', '0' */
        uint32_t next = 0, guard = 0;
        input_rx_reset(&rx, 6u, kQupRxTagLen);
        memset(out, 0, 6u);
        while (guard++ < 32u && rx.got < rx.want) {
            if (input_rx_needs_word(&rx)) {
                CHECK(next < 2u, "the drain asks for no more words than the hardware delivered");
                if (next >= 2u) {
                    break;
                }
                input_rx_word(&rx, delivered[next++]);
            }
            input_rx_pop(&rx, out);
        }
        CHECK_EQ(rx.got, 6u, "six data bytes land from two delivered words");
        CHECK_EQ(next, 2u, "and exactly two words were needed for them");
        CHECK(out[0] == 0x47 && out[1] == 0x54 && out[2] == 0x39 && out[3] == 0x00 &&
              out[4] == 0x31 && out[5] == 0x30,
            "the bytes are the data half of each word, in order, tags dropped");
        CHECK(!input_rx_needs_word(&rx), "a complete cursor asks for nothing");
        CHECK_EQ(out[3], 0u, "buf[5] == 0 is the vendor's own 3-character-pid condition, and it survived the drop");
    }

    // DIRECTION: without the tag drop the first two tag bytes would be reported as identity. The test is
    // that a reset with tag_len 2 and one with tag_len 0 differ by exactly those two bytes.
    input_rx_reset(&rx, 4u, 0u);
    input_rx_word(&rx, 0x04030201u);
    {
        uint32_t guard = 0;
        while (rx.got < rx.want && guard++ < 16u) {
            input_rx_pop(&rx, out);
        }
        CHECK_EQ(out[0], 0x01u, "no tags means the low byte is data");
    }
    input_rx_reset(&rx, 4u, kQupRxTagLen);
    input_rx_word(&rx, 0x04030201u);
    {
        uint32_t guard = 0;
        while (rx.got < rx.want && guard++ < 16u) {
            input_rx_pop(&rx, out);
        }
        CHECK_EQ(out[0], 0x03u, "two tags dropped means the third byte is the first data");
    }
    input_rx_reset(&rx, 0u, kQupRxTagLen);
    CHECK(!input_rx_needs_word(&rx), "a read of nothing wants no words");
    CHECK(input_rx_complete(&rx), "a read of nothing is complete at once");
    input_rx_word(&rx, 0xffffffffu);
    CHECK(!input_rx_pop(&rx, out), "and it stores nothing even if a word is pushed at it");
    CHECK_EQ(input_rx_pop(&rx, 0), 0, "a null buffer stores nothing");
}

static void check_verdict(void)
{
    // PRESERVE: boot 48's four failures were NACKs with err=0 and a status word carrying the NACK bit,
    // and the driver classified that as a NACK rather than a timeout.
    CHECK_EQ(input_xfer_verdict(0x90u, 0u, 0x10000c8u, kQupMxOutputDone, 7u, 7u, 0u, 0u),
        kInputXferNack, "boot 48's shape classifies as nack");
    CHECK(strcmp(input_xfer_verdict_name(kInputXferNack), "nack") == 0, "the nack name");

    // The two error words name different culprits: a core flag alone is a mini-core problem, a bus flag
    // without the NACK bit is a bus problem, and both is still reported as the bus because that is the
    // half with a line-level meaning.
    CHECK_EQ(input_xfer_verdict(0u, 0x04u, 0u, kQupMxOutputDone, 1u, 1u, 0u, 0u),
        kInputXferCoreError, "core flag alone is core-error");
    CHECK_EQ(input_xfer_verdict(0u, 0u, 0x04u, kQupMxOutputDone, 1u, 1u, 0u, 0u),
        kInputXferBusError, "bus flag without nack is bus-error");
    CHECK(input_xfer_is_nack(0x8u) && !input_xfer_is_nack(0x4u), "only BIT(3) is a nack");

    // The strictness the driver documents: a done bit with the wrong byte count is NOT a success. This is
    // the branch that turns "short data that looks real" into a reading.
    CHECK_EQ(input_xfer_verdict(kQupMxInputDone, 0u, 0u, kQupMxInputDone, 4u, 4u, 1u, 3u),
        kInputXferIncomplete, "done bit with a short read is incomplete");
    CHECK_EQ(input_xfer_verdict(kQupMxOutputDone, 0u, 0u, kQupMxOutputDone, 6u, 7u, 0u, 0u),
        kInputXferIncomplete, "done bit with a short write is incomplete");
    CHECK_EQ(input_xfer_verdict(0u, 0u, 0u, kQupMxOutputDone, 7u, 7u, 0u, 0u),
        kInputXferIncomplete, "no done bit at all is the timeout shape");
    CHECK_EQ(input_xfer_verdict(kQupMxInputDone, 0u, 0u, kQupMxInputDone, 4u, 4u, 3u, 3u),
        kInputXferDone, "done bit with both counts agreeing is success");
    CHECK(strcmp(input_xfer_verdict_name(kInputXferDone), "done") == 0 &&
          strcmp(input_xfer_verdict_name(kInputXferBusError), "bus-error") == 0 &&
          strcmp(input_xfer_verdict_name(kInputXferCoreError), "core-error") == 0 &&
          strcmp(input_xfer_verdict_name(kInputXferIncomplete), "incomplete") == 0,
        "all five classes have their own name, and none shares another's");
    CHECK(strcmp(input_xfer_verdict_name(99), "incomplete") == 0,
        "an out-of-range class falls back to the name that says nothing worked");
}

static void check_bus_status(void)
{
    struct InputBusStatus status;
    struct InputCoreFlags flags;

    // MEASURE: boot 48's captured word. It is the archived value AFTER the mask, and the point of this
    // case is the second half: two of the thirteen conditions are not inside that mask at all.
    input_bus_status_decode(0x10000c8u, &status);
    CHECK_EQ(status.nack, 1u, "BIT(3) is the packet-nacked condition");
    CHECK_EQ(status.failed, 1u, "BIT(6) is QUP_FAILED, named only in the vendor header");
    CHECK_EQ(status.invalid_read_addr, 1u, "BIT(24) is QUP_INVALID_READ_ADDR");
    CHECK_EQ(status.sda_high, 0u, "SDA sense is clear in the archived word");
    CHECK_EQ(status.scl_high, 0u, "SCL sense is clear in the archived word");
    CHECK_EQ(status.residue, 0x80u, "BIT(7) is named in neither tree and must be reported");
    CHECK_EQ(status.mainline_error, 0x10000c8u, "MAIN's mask keeps all four conditions");
    CHECK_EQ(status.vendor_error, 0x1000008u,
        "the vendor's own mask drops BIT(6) and BIT(7) and keeps nack plus invalid-read-addr");

    // DIRECTION, and it is the finding of this round: the mask the driver has always applied makes the
    // line sensors constant. Whatever the hardware says about SDA and SCL, a field filled from
    // `raw & kI2cStatusErrorMask` reads 0 -- so boot 48's `status=` can never have shown a held bus.
    {
        const uint32_t held = kI2cStatusPacketNacked | kI2cStatusSdaSensor | kI2cStatusSclSensor;
        input_bus_status_decode(held, &status);
        CHECK_EQ(held & kI2cStatusErrorMask, kI2cStatusPacketNacked,
            "the mask erases both line sensors -- they are outside it by construction");
        CHECK_EQ(status.sda_high, 1u, "decoded from the RAW word, the sensor reads high");
        CHECK_EQ(status.scl_high, 1u, "and so does SCL");
        CHECK_EQ(status.mainline_error, 0x8u, "the masked field says nothing about the lines");
    }

    // DIRECTION: the disagreement between the two trees, made visible. A word with only BIT(6)|BIT(7) is
    // an error to MAIN's 0x7c-shaped mask and nothing at all to the vendor's.
    {
        const uint32_t only_borderline = kI2cStatusFailed | 0x80u;
        input_bus_status_decode(only_borderline, &status);
        CHECK(status.mainline_error != 0u, "MAIN calls BIT(6)|BIT(7) an error");
        CHECK_EQ(status.vendor_error, 0u, "the vendor's mask does not");
    }

    input_bus_status_decode(0u, 0);
    input_bus_status_decode(0u, &status);
    CHECK_EQ(status.residue, 0u, "an all-zero word has no residue to report");
    CHECK_EQ(status.bus_active, 0u, "BUS_ACTIVE is 0 in the archived word's shape");

    // MEASURE: the same shape on the core flags. boot 48 read err=0, so this case is about the mask, not
    // a reading: MAIN's 0x7c includes BIT(6), the vendor's 0x3c does not.
    input_core_flags_decode(0x7cu, &flags);
    CHECK_EQ(flags.input_over_run, 1u, "BIT(2) is input over run");
    CHECK_EQ(flags.output_under_run, 1u, "BIT(3) is output under run");
    CHECK_EQ(flags.input_under_run, 1u, "BIT(4) is input under run");
    CHECK_EQ(flags.output_over_run, 1u, "BIT(5) is output over run");
    CHECK_EQ(flags.residue, 0x40u, "BIT(6) of ERROR_FLAGS is named in neither tree");
    CHECK_EQ(flags.mainline_error, 0x7cu, "MAIN's mask covers all seven bits");
    CHECK_EQ(flags.vendor_error, 0x3cu, "the vendor's mask stops at BIT(5)");
    input_core_flags_decode(0u, &flags);
    CHECK_EQ(flags.mainline_error, 0u, "boot 48's err=0 really is no core flag");
}

static void check_verdict_codes(void)
{
    // PRESERVE: observation slot 15 is step * 1000 + failure, and boot 48's 102 is the bare failure code
    // for a transfer, not a scaled one.
    CHECK_EQ(input_verdict_code(kInputStepRun, kInputFailPoll), 7101u, "run-stuck encodes as 7101");
    CHECK_EQ(input_verdict_step(7101u), kInputStepRun, "and decodes back to the run step");
    CHECK_EQ(input_verdict_failure(7101u), kInputFailPoll, "and to a stuck poll");
    CHECK(strcmp(input_step_name(input_verdict_step(7101u)), "run") == 0, "the run step is named");
    CHECK(strcmp(input_failure_name(input_verdict_failure(7101u)), "poll-stuck") == 0,
        "a stuck poll is named as its own failure");
    // bringUpCore's reset is a step of its own: a soft reset that never leaves reset must encode as
    // 5101 and name "core-reset", not borrow the run step's name for a different wire.
    CHECK_EQ(input_verdict_code(kInputStepResetCore, kInputFailPoll), 5101u,
        "core-reset stuck encodes as 5101");
    CHECK_EQ(input_verdict_step(5101u), kInputStepResetCore, "and decodes back to the reset step");
    CHECK(strcmp(input_step_name(kInputStepResetCore), "core-reset") == 0,
        "the reset step is named core-reset");
    CHECK_EQ(input_verdict_step(kInputFailXfer), kInputFailXfer, "102 is a bare failure code");
    CHECK_EQ(input_verdict_failure(kInputFailXfer), kInputFailXfer, "and its own failure field");
    CHECK(strcmp(input_failure_name(input_verdict_failure(kInputFailXfer)), "xfer-failed") == 0,
        "boot 48's verdict=102 reads as xfer-failed");
    CHECK(strcmp(input_failure_name(input_verdict_failure(100u)), "map") == 0, "100 reads as map");
    CHECK(strcmp(input_failure_name(0u), "none") == 0, "no failure is named, not silently 0");
    CHECK(strcmp(input_step_name(42u), "unknown-step") == 0, "an unknown step says so");
    CHECK(strcmp(input_failure_name(77u), "unknown-failure") == 0, "an unknown failure says so");
    CHECK(strcmp(input_step_name(0u), "unknown-step") == 0,
        "step 0 is not a step: nothing ran, and the name must not invent one");
}

static void check_tags(void)
{
    uint8_t tags[8];

    // PRESERVE + MEASURE: the exact four bytes for the Goodix candidate at 0x5d, checked against
    // qup_i2c_set_tags() (i2c-qup-mainline.c:569-613) and the tag values at :107-112.
    CHECK_EQ(input_write_tags((uint8_t) kGtAddr, 2u, 0, tags, sizeof (tags)), kQupMaxTagsLen,
        "a goodix register write builds four tag bytes");
    CHECK_EQ(tags[0], kQupTagV2Start, "START is 0x81");
    CHECK_EQ(tags[1], 0xba, "0x5d shifted is 0xba");
    CHECK_EQ(tags[2], kQupTagV2DataWr, "a write followed by a read is DATAWR, never the STOP variant");
    CHECK_EQ(tags[3], 2u, "and the count is the register length");

    CHECK_EQ(input_read_tags((uint8_t) kGtAddr, 1u, tags, sizeof (tags)), kQupMaxTagsLen,
        "a goodix read builds four tag bytes too");
    CHECK_EQ(tags[1], 0xbb, "the same address with the R/W bit set");
    CHECK_EQ(tags[2], kQupTagV2DataRdStop, "and DATARD_STOP, because this is the last block");
    CHECK_EQ(tags[3], 1u, "one byte asked for");

    CHECK(input_write_tags((uint8_t) kFtAddr, 1u, 0, tags, sizeof (tags)) == kQupMaxTagsLen &&
          tags[1] == 0x70, "0x38 shifted is 0x70");
    CHECK(input_read_tags((uint8_t) kFtAddr, 22u, tags, sizeof (tags)) == kQupMaxTagsLen &&
          tags[1] == 0x71 && tags[3] == 22u, "the EDT name read is 22 bytes");

    // The STOP variant of the write tag is reachable only for a write-only transaction, which chipRead
    // never issues -- pinned here so the branch is tested even though the driver cannot reach it.
    CHECK(input_write_tags((uint8_t) kGtAddr, 2u, 1, tags, sizeof (tags)) == kQupMaxTagsLen &&
          tags[2] == kQupTagV2DataWrStop, "the last-of-all write tag does use STOP");

    // The 256 bound: a length of QUP_READ_LIMIT encodes as 0, which means 256 to this core and would mean
    // "nothing" to anything reading the tag afterwards. Refused, in both directions of the API.
    CHECK_EQ(input_tag_len_field(kQupReadLimit), -1, "256 has no length field");
    CHECK_EQ(input_tag_len_field(kQupReadLimit - 1u), 255, "255 is the largest honest ask");
    CHECK_EQ(input_write_tags((uint8_t) kGtAddr, kQupReadLimit, 0, tags, sizeof (tags)), 0u,
        "and a write of 256 registers builds no tag block");
    CHECK_EQ(input_read_tags((uint8_t) kGtAddr, kQupReadLimit, tags, sizeof (tags)), 0u,
        "and a read of 256 bytes builds no tag block");
    CHECK_EQ(input_read_tags((uint8_t) kGtAddr, 1u, tags, 3u), 0u,
        "a buffer that cannot hold four tag bytes is refused rather than overrun");
    CHECK_EQ(input_write_tags(0x5du, 2u, 0, 0, 8u), 0u, "a null tag buffer is refused");

    // PRESERVE: MX_WRITE_CNT is the whole tx length and MX_READ_CNT is tags + data, with BIT(31) added on
    // every sub-transfer after the first -- the mechanism that makes a repeated start possible.
    {
        struct InputCounters counters;
        input_counters(7u, 0u, 1, &counters);
        CHECK_EQ(counters.write_cnt, 7u, "the first segment writes its own length");
        CHECK(!counters.has_read, "a write-only segment does not touch MX_READ_CNT");
        input_counters(4u, 1u, 0, &counters);
        CHECK_EQ(counters.write_cnt, kQupMxConfigDuringRun | 4u, "a later segment sets BIT(31)");
        CHECK_EQ(counters.read_cnt, kQupMxConfigDuringRun | 3u, "and reads tags plus one data byte");
        CHECK(counters.has_read, "and says so");
        input_counters(4u, 254u, 1, &counters);
        CHECK_EQ(counters.read_cnt, 256u, "the largest legal read is tags plus 254");
    }
}

static void check_request_bounds(void)
{
    // PRESERVE: exactly the four refusals chipRead() made inline, in the same order of meaning.
    CHECK(!input_read_request_ok(0u, 1u), "no register address is refused");
    CHECK(!input_read_request_ok(3u, 1u), "three register bytes is more than this chip family addresses");
    CHECK(!input_read_request_ok(1u, 0u), "a zero-length read is the 256 trap");
    CHECK(!input_read_request_ok(2u, kQupReadLimit - kQupRxTagLen + 1u),
        "one byte past one block is refused");
    CHECK(input_read_request_ok(2u, kQupReadLimit - kQupRxTagLen), "the last legal length is legal");
    CHECK(input_read_request_ok(1u, 1u), "the smallest real question the two chip drivers ask");
    CHECK(input_read_request_ok(2u, kFtNameLen), "the EDT 22-byte name read");
}

static void check_identification(void)
{
    struct InputGoodixId id;
    uint8_t data[6];

    // MEASURE: gt9xx.c:1222-1258 reads {GTP_REG_VERSION >> 8, & 0xff} into an 8-byte buffer, so its
    // buf[2..7] are this driver's data[0..5]. The vendor prints buf[2],buf[3],buf[4] as the pid, takes a
    // fourth when buf[5] != 0, and computes the version as (buf[7] << 8) | buf[6].
    data[0] = '9'; data[1] = 'x'; data[2] = 'x'; data[3] = 0; data[4] = 0x21; data[5] = 0x01;
    input_goodix_identify(data, sizeof (data), &id);
    CHECK_EQ(id.pid_length, 3u, "a zero after the three pid bytes means a three-character pid");
    CHECK(id.pid[0] == '9' && id.pid[1] == 'x' && id.pid[2] == 'x', "the three characters are [0..2]");
    CHECK_EQ(id.version, 0x0121u, "version is data[5] high, data[4] low");
    CHECK_EQ(id.printable, 3u, "three printable pid bytes");
    // PRESERVE: the empty fourth slot is a blank in the row, not a '.', because that is what the
    // round181 expression `pidLength == 4 ? first[3] : ' '` printed and the row is a fixed four %c.
    CHECK(id.pid[3] == ' ', "a three-character pid leaves a blank fourth slot");

    data[3] = 'A';
    input_goodix_identify(data, sizeof (data), &id);
    CHECK_EQ(id.pid_length, 4u, "a non-zero fourth byte means a four-character pid");
    CHECK(id.pid[3] == 'A', "and the fourth character is data[3]");
    CHECK_EQ(id.printable, 4u, "four printable");

    data[3] = 0x01;   // non-zero (so pid_length is 4) but not printable
    input_goodix_identify(data, sizeof (data), &id);
    CHECK_EQ(id.pid_length, 4u, "a control byte still counts as the optional fourth");
    CHECK(id.pid[3] == ' ', "and renders as the blank, never as a control character in the log");
    CHECK_EQ(id.printable, 3u, "an unprintable fourth is not counted printable");

    data[0] = 0x00; data[1] = 0xff; data[2] = 'z'; data[3] = 0x20;
    data[4] = 0x00; data[5] = 0x7f;
    input_goodix_identify(data, sizeof (data), &id);
    CHECK(id.pid[0] == '.' && id.pid[1] == '.', "unprintable pid bytes are dotted, not passed through");
    CHECK_EQ(id.printable, 2u, "the space at 0x20 and 0x7f are the printable pair");
    CHECK_EQ(id.version, 0x7f00u, "the version bytes are taken unsigned, 0x7f and 0x00");
    input_goodix_identify(data, 5u, &id);
    CHECK_EQ(id.version, 0x7f00u, "a short buffer is refused rather than read past");

    // The vendor's double-check rule, counted rather than bool-ed.
    {
        uint8_t other[6];
        memcpy(other, data, 6u);
        CHECK_EQ(input_bytes_agree(data, other, 6u), 6u, "identical answers agree on six");
        other[4] ^= 0x01u;
        CHECK_EQ(input_bytes_agree(data, other, 6u), 5u, "one differing byte reads as five agreeing");
        CHECK_EQ(input_bytes_agree(data, 0, 6u), 0u, "a null answer agrees on nothing");
    }

    // gt9xx.c:1250: 0x06 or above means the config blob was never sent.
    CHECK(input_goodix_sensor_id_valid(0u) && input_goodix_sensor_id_valid(5u), "0..5 is a valid id");
    CHECK(!input_goodix_sensor_id_valid(6u), "6 is the first invalid one");
    CHECK(!input_goodix_sensor_id_valid(0xffu), "and all-ones, which is what an unanswered read leaves");

    // edt-ft5x06-mainline.c:846-900: M06 answers with a leading response byte, so the name starts at [1].
    {
        uint8_t name[kFtNameBuf];
        memset(name, 0, sizeof (name));
        name[1] = 'E'; name[2] = 'P'; name[3] = '0';
        CHECK(input_ft_is_m06(name, kFtNameLen), "EP0 at [1] is an M06");
        name[1] = 'e';
        CHECK(input_ft_is_m06(name, kFtNameLen), "and so is a lower-case e");
        name[0] = 'E'; name[1] = 'P'; name[2] = '0';
        memset(name, 0, sizeof (name));
        name[0] = 'E'; name[1] = 'P'; name[2] = '0';
        CHECK(!input_ft_is_m06(name, kFtNameLen), "EP0 at [0] is the M12 or generic branch");
        memset(name, 0, sizeof (name));
        CHECK(!input_ft_is_m06(name, 1u), "a one-byte name cannot decide anything");
        CHECK(!input_ft_is_m06(0, kFtNameLen), "and a null one is refused");
    }
}

static void check_time(void)
{
    // MEASURE: this board's mach timer runs at 19,267,584 Hz (round170's compressed-clock lesson, recorded
    // in memory as the reason every `t=` must go through absolutetime_to_nanoseconds). The sanity window is
    // 1 MHz..1 GHz, so the real frequency is accepted and the 19.2 MHz XO is the fallback, not the answer.
    CHECK_EQ(input_ticks_per_ms(19267584ULL), 19267ULL, "the board's own 19.267584 MHz is trusted");
    CHECK_EQ(input_ticks_per_ms(1000000ULL), 1000ULL, "1 MHz is the lower accepted edge");
    CHECK_EQ(input_ticks_per_ms(1000000000ULL), 1000000ULL, "1 GHz is the upper accepted edge");
    CHECK_EQ(input_ticks_per_ms(999999ULL), kXoHz / 1000ULL, "below the window falls back to the XO");
    CHECK_EQ(input_ticks_per_ms(0ULL), 19200ULL, "a zero read falls back rather than dividing by zero");
    CHECK_EQ(input_ticks_per_ms(1000000001ULL), 19200ULL, "and above it too");

    CHECK_EQ(input_ticks_for_ms(20u, 19267ULL), 385340ULL, "the vendor's T2 is 20 ms in ticks");
    CHECK_EQ(input_elapsed_ms(0ULL, 0ULL), 0u, "a zero clock still yields zero, not a trap");
    CHECK_EQ(input_elapsed_ms(19267584ULL, 19267ULL), 1000u, "one second of this board is 1000 ms");

    // DIRECTION: round170's mistake, kept as a case so the lesson has a witness. Treating ticks as
    // nanoseconds compresses this board's clock by about 51.9x -- a `t=` field built that way reads
    // plausible and is wrong in a way no reader can spot from the number alone.
    {
        const uint64_t one_second_of_ticks = 19267584ULL;
        const uint64_t as_if_nanoseconds = one_second_of_ticks / 1000000000ULL;
        CHECK(as_if_nanoseconds == 0u, "one real second reads as zero when the ticks are mis-scaled");
        CHECK(input_elapsed_ms(one_second_of_ticks, 19267ULL) == 1000u,
            "and the same second is 1000 ms through the right divisor");
    }
}

int main(void)
{
    check_pin_map();
    check_rcg();
    check_branch_polarity();
    check_pin_words();
    check_fifo_sizes();
    check_state_acceptance();
    check_tx_cursor();
    check_rx_cursor();
    check_verdict();
    check_bus_status();
    check_verdict_codes();
    check_tags();
    check_request_bounds();
    check_identification();
    check_time();

    if (g_failures) {
        printf("FAIL: %d of %d checks in clover_input_host.cpp\n", g_failures, g_checks);
        return 1;
    }
    printf("PASS: %d checks -- pin map refuses an unmapped pin (the pre-layer expression wrapped to "
        "~4 TiB); RCG rows only name rates the table can hold; CLK_OFF polarity and the NOC-FSM escape; "
        "drive raw 7 is the DT's 16 mA; boot 48's io=c0a5 is 64/64 FIFOs so the OUT_SERVICE policy "
        "cannot diverge for any request this driver issues; the tx cursor puts the same two words in the "
        "FIFO as upstream's byte-wise path and stops at 8 bytes in an 8-byte FIFO; the rx cursor drops "
        "exactly READ_RX_TAGS_LEN and never overruns; five verdict classes with the strict done-AND-count "
        "rule; boot 48's status word decodes nack+failed+invalid-read-addr with BIT(7) as unnamed "
        "residue, and SDA/SCL sit OUTSIDE the mask the driver has always applied -- so its `status=` "
        "field could never show a held bus; MAIN's 0x38000fc vs the vendor's 0x380003c differ by exactly "
        "those two unnamed bits; 7101 decodes to a stuck run-step poll and 102 to xfer-failed; the v2 tag "
        "bytes for 0x5d and 0x38 match i2c-qup-mainline.c:569-613 with 256 refused in both directions; "
        "gt9xx.c's buf[2..7] shift gives pid length 3 or 4 and version (data[5]<<8)|data[4]; the 0x06 "
        "sensor-id bound; the M06 leading-response-byte test; INTR_CFG's target is cut as [7:5] with the "
        "round181 0x1f00-at-shift-5 expression run both ways; the mux value is the pin's own index because "
        "the SoC-wide ordinal will not fit a 4-bit field; and 19,267,584 Hz trusted with round170's "
        "51.9x mis-scale kept as a case\n", g_checks);
    return 0;
}
