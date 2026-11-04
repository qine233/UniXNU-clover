// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the touch/I2C line's bus-free layer -- what the words CloverInput.cpp
// reads *mean*, with no window, no IOKit object and no printf in it.
//
// WHY THIS FILE EXISTS (round318)
//
// notes/unixnu-doc-conformance.md §7 gap 3 asked for this of three drivers; round316 did CloverBlock,
// round317 did CloverGpu. CloverInput is the last one, and it is the one where the layer was not a
// style upgrade: the touch area's standing verdict is "electrically alive, nothing answers"
// (round260, boot
// 48: four sub-transfers, four NACKs, verdict=102), and that verdict has always been read out of a
// single masked hex word. Writing the mask down as a function showed the instrument cannot answer the
// question it is quoted as answering. `clover_input_observation[6]` stores
// `qupRead(kQupI2cStatus) & kI2cStatusErrorMask`, and the mask's top bit is BIT(25) -- so BIT(26) and
// BIT(27), which are the mini-core's own sense of SDA and SCL (i2c-msm-v2.h:131-132), are erased by
// construction. The alternative explanation the driver itself prints at the end of a failed probe --
// "a bus whose SDA/SCL are held low by a chip mid-transfer answers nothing at all" -- could therefore
// never have been tested by that field, and the `sda=%08x scl=%08x` in that same line reads the TLMM
// IO registers of pins whose mux is blsp_i2c3, which is not where the line state lives either.
//
// WHAT ELSE CAME OUT OF WRITING IT DOWN
//
// The two trees this driver cites disagree about which bits count as an error at all: MAIN's
// I2C_STATUS_ERROR_MASK is 0x38000fc, the vendor's QUP_MSTR_STTS_ERR_MASK is 0x380003c (the pair
// BIT(6)|BIT(7)); on the core flags MAIN's 0x7c vs the vendor's 0x3c (BIT(6)). A word with only those
// bits set is a failure to one tree and silence to the other, so this layer keeps both masks and says
// which one produced a given answer. And the wire encoding -- the thing every NACK has been blamed on
// before being blamed on power -- checks out byte for byte against i2c-qup-mainline.c:569-613 and
// :1478-1483, which is why §2 of notes/input-bringup.md can say the tags are not the problem rather
// than assume it.
//
// WHAT MAY LIVE HERE
//
// Bit-field identity, bounds, counts, byte order, and the order in which a question gets asked -- for
// instance that a request whose length field would encode as 0 is refused rather than sent as 256.
// Nothing that reads or writes a register, nothing that needs a kernel address, nothing that formats a
// row. CloverInput.cpp keeps the MMIO, the gates that decide *whether* to touch a register, and every
// printed field, including every `clover r181:` row, which is byte-identical to what it printed before
// this file existed so that boots 41 through 102 stay comparable.
//
// NO NUMBER IS RE-TYPED
//
// InputRegs.h is the bus-free header of offsets, masks and values, each quoted to the DT node or the
// source file it came from. This layer includes it and derives from those names rather than writing
// the same literals again: the pin numbers come out of the pin physical addresses divided by the tile
// stride, the FIFO block sizes come out of the same IO_MODE field extraction the driver uses, and the
// tag bytes are built from kQupTag* only. Two copies of one address is how round181 shipped a base
// with a missing digit; two copies of one *mask* is how a field ends up silently unable to say what it
// is named after.
//
// SOURCES ARE LINE-NUMBERED ON PURPOSE
//
// notes/unixnu-doc-conformance.md §8 permits a line-number citation only for a frozen target.
// notes/sources/sdm660/{i2c-qup-mainline.c, i2c-msm-v2.c, i2c-msm-v2.h, gt9xx.c, gt9xx.h,
// edt-ft5x06.c, edt-ft5x06-mainline.c} are vendored files this repository does not edit; their sha256
// values and the tree plus commit each came from are recorded in notes/input-bringup.md §1.

#ifndef CLOVER_INPUT_INPUTCORE_H
#define CLOVER_INPUT_INPUTCORE_H

#include <stdint.h>

#include "InputRegs.h"

// ---------------------------------------------------------------------------
// The four pins, and the two windows they live in
// ---------------------------------------------------------------------------

// The pin *numbers* are not written here. They come out of the physical addresses InputRegs.h pins to
// the DT (qcom,i2c-dat 0xe8 0x0a -> gpio10, qcom,i2c-clk 0xe8 0x0b -> gpio11, and the touch nodes'
// reset 0xe8 0x42 / irq 0xe8 0x43) divided by the tile stride, so a header that moves a pin moves it
// here too instead of leaving a bare 10 behind.
static const uint32_t kInputPinStride = kTlmmPinStride;
static const uint32_t kInputPinSda =
    (uint32_t) ((kPinGpio10Phys - kTlmmPhys) / kInputPinStride);
static const uint32_t kInputPinScl =
    (uint32_t) ((kPinGpio11Phys - kTlmmPhys) / kInputPinStride);
static const uint32_t kInputPinRst =
    (uint32_t) ((kPinGpio66Phys - kTlmmPhys) / kInputPinStride);
static const uint32_t kInputPinIrq =
    (uint32_t) ((kPinGpio67Phys - kTlmmPhys) / kInputPinStride);

// How many pins fit in one mapped pair window. kPinPairSize is 0x2000 and a pin occupies
// kTlmmPinStride, so this is 2 -- the reason the driver can map four pins with two windows.
static const uint32_t kInputPinsPerWindow = kPinPairSize / kInputPinStride;

enum InputPinPair {
    kInputPairNone = 0,     // not in either mapped window: the caller must refuse
    kInputPairI2c,          // gpio10 and gpio11, the SDA/SCL pair
    kInputPairTouch         // gpio66 and gpio67, reset and interrupt
};

static const char *const kInputPinPairNames[3] = { "none", "i2c", "touch" };

static inline const char *input_pin_pair_name(int pair)
{
    return (pair >= 0 && pair <= kInputPairTouch) ? kInputPinPairNames[pair]
                                                  : kInputPinPairNames[kInputPairNone];
}

static inline int input_pin_pair_of(uint32_t pin)
{
    if (pin == kInputPinSda || pin == kInputPinScl) {
        return kInputPairI2c;
    }
    if (pin == kInputPinRst || pin == kInputPinIrq) {
        return kInputPairTouch;
    }
    return kInputPairNone;
}

// The first pin of the pair this pin belongs to, because the window base is that pin's CTL and every
// other pin's registers sit at (pin - first) * stride from there.
static inline uint32_t input_pin_pair_base(uint32_t pin)
{
    return (input_pin_pair_of(pin) == kInputPairTouch) ? kInputPinRst : kInputPinSda;
}

// THE GUARD, AND WHY IT IS NEW. Before this layer the driver computed the window from one test,
// `(pin <= 11) ? i2c : touch`, and the offset from `(pin - 66) * stride` for everything above it. That
// is correct for exactly the four pins this driver is given and dangerous for any other: pin 12 takes
// the touch branch and subtracts to a negative number in unsigned arithmetic, which wraps into an
// offset of about 4 TiB -- a read far outside a 0x2000 window that no fault would name. Nothing calls
// it with such a pin today, and that is precisely why the rule belongs in a function a host can ask.
static inline int input_pin_offset(uint32_t pin, uint32_t reg, uint32_t *offset)
{
    const int pair = input_pin_pair_of(pin);
    if (pair == kInputPairNone || offset == 0) {
        return 0;
    }
    *offset = (pin - input_pin_pair_base(pin)) * kInputPinStride + reg;
    return *offset < kPinPairSize;
}

// ---------------------------------------------------------------------------
// Clocks: the RCG's own rate table, and the two polarities a branch is judged with
// ---------------------------------------------------------------------------

struct InputRcg {
    uint32_t raw;
    uint32_t src;
    uint32_t div;
    uint32_t mode;
    uint32_t hw_ctrl;
    const char *parent;
    uint64_t hz;
    int known;            // 0 = no row of this RCG's frequency table, so no rate may be claimed
};

// Only the rows this RCG's parent map can be left in are named:
// gcc_parent_map_xo_gpll0_gpll0_early_div. Anything else prints the selector and stops, which is the
// same rule the driver's row has always followed -- inventing a rate is worse than admitting one.
static inline void input_rcg_decode(uint32_t cfg, struct InputRcg *out)
{
    if (out == 0) {
        return;
    }
    out->raw = cfg;
    out->src = (cfg & kRcgCfgSrcMask) >> kRcgCfgSrcShift;
    out->div = cfg & kRcgCfgDivMask;
    out->mode = (cfg & kRcgCfgModeMask) >> kRcgCfgModeShift;
    out->hw_ctrl = (cfg & kRcgCfgHwCtrl) ? 1u : 0u;
    out->hz = 0;
    out->known = 0;
    out->parent = "unknown";
    if (out->src == kRcgSrcXo) {
        out->parent = "xo";
        out->hz = kXoHz / (out->div ? out->div : 1u);
        out->known = 1;
    } else if (out->src == kRcgSrcGpll0 || out->src == kRcgSrcGpll0Early) {
        out->parent = (out->src == kRcgSrcGpll0) ? "gpll0" : "gpll0_early_div";
        if (out->div) {
            out->hz = kGpll0Hz / out->div;
        }
        out->known = 1;
    }
}

// clk-branch2_check_halt(): a branch is done when CLK_OFF is clear OR the NOC FSM says ON. Both
// halves matter -- a voted AHB branch reports through the FSM, and a plain gate through bit 31.
static inline uint32_t input_branch_fsm(uint32_t word)
{
    return (word & kCbcrFsmMask) >> kCbcrFsmShift;
}

static inline int input_branch_halted(uint32_t word)
{
    return (word & kCbcrClkOff) && input_branch_fsm(word) != kCbcrFsmOn;
}

static inline int input_branch_running(uint32_t word)
{
    return !input_branch_halted(word);
}

static inline int input_branch_requested(uint32_t word)
{
    return (word & kCbcrEnable) ? 1 : 0;
}

// The write that asks the branch to run: set its enable bit, leave every other field alone. The poll
// that follows is input_branch_halted()'s, so "asked" and "took" are two different questions.
static inline uint32_t input_branch_enable_word(uint32_t before)
{
    return before | kCbcrEnable;
}

static inline int input_ahb_voted(uint32_t word)
{
    return (word & (1u << kAhbVoteBit)) ? 1 : 0;
}

static inline uint32_t input_ahb_vote_word(uint32_t before)
{
    return before | (1u << kAhbVoteBit);
}

// The XO row __clk_rcg2_configure_mnd()/__configure_parent() produce for a zero-M/N RCG: clear the
// writable fields and the source, set pre_div = 1 and P_XO's selector 0, leave everything else as the
// boot had it.
static inline uint32_t input_rcg_xo_cfg(uint32_t before)
{
    return (before & ~(kRcgCfgWritable | kRcgCfgSrcMask)) | kRcgDiv1 |
           (kRcgSrcXo << kRcgCfgSrcShift);
}

static inline uint32_t input_rcg_update_word(uint32_t cmd_before)
{
    return cmd_before | kRcgCmdUpdate;
}

static inline int input_rcg_update_pending(uint32_t cmd)
{
    return (cmd & kRcgCmdUpdate) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// TLMM: the CTL/IO words the driver writes, and the fields it reports
// ---------------------------------------------------------------------------

// i2c_3_active: mux blsp_i2c3, drive 2 mA, bias-disable -- and only those three fields, because
// msm_pinmux_set_mux() writes only the mux and the config path only what the state names. The OE bit
// is deliberately not touched: the QUP drives these lines open-drain.
static inline uint32_t input_i2c_pin_ctl(uint32_t ctl)
{
    return (ctl & ~(kPinMuxMask | kPinPullMask | (kPinDrvMask << kPinDrvShift))) |
           (kPinFuncBlspI2c3 << kPinMuxShift) | (kPinDrv2mA << kPinDrvShift) | kPinPullNone;
}

// gpio_direction_output(n, v): the IO latch first, then CTL with mux forced to gpio and OE set
// (pinctrl-msm-mainline.c's gpio_direction_output, which this driver mirrors in two calls).
static inline uint32_t input_pin_output_io(uint32_t io, uint32_t level)
{
    return level ? (io | (1u << kPinOutBit)) : (io & ~(1u << kPinOutBit));
}

static inline uint32_t input_pin_output_ctl(uint32_t ctl)
{
    return (ctl & ~kPinMuxMask) | (1u << kPinOeBit);
}

// gpio_direction_input(): clear OE and nothing else, which is what "input-enable" means here
// (pinctrl-msm.c folds INPUT_ENABLE into the OE bit).
static inline uint32_t input_pin_input_ctl(uint32_t ctl)
{
    return (ctl & ~kPinMuxMask) & ~(1u << kPinOeBit);
}

// ts_int_defalut's 16 mA, applied once before the line is driven so both output states inherit it.
static inline uint32_t input_pin_drive_ctl(uint32_t ctl, uint32_t drive)
{
    return (ctl & ~(kPinDrvMask << kPinDrvShift)) | (drive << kPinDrvShift);
}

// INTR_CFG's three-bit interrupt target. INTR_CFG[7:5]; see InputRegs.h for where the shift and the
// width each come from and why the mask is derived rather than written out.
static inline uint32_t input_pin_intr_target(uint32_t intr_cfg)
{
    return (intr_cfg & kPinIntrTargetMask) >> kPinIntrTargetShift;
}

// RAW_STATUS_EN, which is INTR_CFG bit 4 -- pinctrl-msm-mainline.c:1120 sets BIT(g->intr_raw_status_bit)
// into the value it writes with msm_writel_intr_cfg(). The pin row's raw= column reads bit 4 of the
// STATUS register, which is the same bit number in a different register; the two trees on disk name
// only the CFG one. Both words are printed on the round318 row so the proven one is readable.
static inline uint32_t input_pin_raw_status_enabled(uint32_t intr_cfg)
{
    return (intr_cfg & kPinIntrRawBit) ? 1u : 0u;
}

struct InputPinState {
    uint32_t ctl, io, intr_cfg, intr_status;
    uint32_t mux, pull, drive_raw, drive_ma;
    uint32_t oe, in_level, out_level;
    uint32_t intr_target, intr_detect, intr_polarity, intr_enable, intr_raw, intr_status_bit;
};

// The encoding the report is written in: drive register value r means (r+1)*2 mA, and bias is
// 0 none / 1 down / 2 keeper / 3 up (pinctrl-msm.c:320-324,470-473).
static inline uint32_t input_pin_drive_ma(uint32_t drive_raw)
{
    return (drive_raw + 1u) * 2u;
}

static inline void input_pin_decode(uint32_t ctl, uint32_t io, uint32_t intr_cfg,
                                    uint32_t intr_status, struct InputPinState *out)
{
    if (out == 0) {
        return;
    }
    out->ctl = ctl;
    out->io = io;
    out->intr_cfg = intr_cfg;
    out->intr_status = intr_status;
    out->mux = (ctl & kPinMuxMask) >> kPinMuxShift;
    out->pull = ctl & kPinPullMask;
    out->drive_raw = (ctl & (kPinDrvMask << kPinDrvShift)) >> kPinDrvShift;
    out->drive_ma = input_pin_drive_ma(out->drive_raw);
    out->oe = (ctl >> kPinOeBit) & 1u;
    out->in_level = (io >> kPinInBit) & 1u;
    out->out_level = (io >> kPinOutBit) & 1u;
    out->intr_target = input_pin_intr_target(intr_cfg);
    out->intr_detect = (intr_cfg >> kPinIntrDetectShift) & 3u;
    out->intr_polarity = (intr_cfg & kPinIntrPolarity) ? 1u : 0u;
    out->intr_enable = (intr_cfg & kPinIntrEnable) ? 1u : 0u;
    out->intr_raw = (intr_status & kPinIntrRawBit) ? 1u : 0u;
    out->intr_status_bit = (intr_status & kPinIntrStatusBit) ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// The QUP core: FIFO depth, state acceptance, and the two byte streams
// ---------------------------------------------------------------------------

// blk_sizes[] = {4, 16, 32} (i2c-qup-mainline.c:1687); depth = block size * (2 << fifo_size_field)
// (i2c-qup-mainline.c:1856-1890). An out-of-range index yields 0 rather than a guess, and the driver
// prints the sizes so a zero reads as "the IO_MODE read was not a size description".
static const uint32_t kInputBlkSizes[3] = { 4, 16, 32 };
static const uint32_t kInputBlkSizeCount = 3U;

static inline uint32_t input_block_size(uint32_t index)
{
    return index < kInputBlkSizeCount ? kInputBlkSizes[index] : 0;
}

static inline void input_fifo_sizes(uint32_t io_mode, uint32_t *out_fifo, uint32_t *in_fifo)
{
    const uint32_t out_blk = input_block_size(kQupOutBlockSize(io_mode));
    const uint32_t in_blk = input_block_size(kQupInBlockSize(io_mode));
    if (out_fifo != 0) {
        *out_fifo = out_blk * (2u << kQupOutFifoSize(io_mode));
    }
    if (in_fifo != 0) {
        *in_fifo = in_blk * (2u << kQupInFifoSize(io_mode));
    }
}

// qup_i2c_change_state(): the field must read back the value written AND say it is valid. A state that
// is right but not marked valid is the hardware telling us it has not taken the change yet.
static inline int input_state_reached(uint32_t state, uint32_t want)
{
    return (state & kQupStateMask) == want && (state & kQupStateValid) ? 1 : 0;
}

static inline int input_state_reset_done(uint32_t state)
{
    return input_state_reached(state, kQupStateReset);
}

// The mini-core accepting I2C_MASTER_GEN is reported by a different bit than the state field.
static inline int input_core_tags_taken(uint32_t state)
{
    return (state & kQupI2cMastGen) && (state & kQupStateValid) ? 1 : 0;
}

// The word qup_i2c_probe() writes when it names the block: I2C mini-core, v2 tag encoding. The
// no-input bit is a property of one transfer, so it belongs to input_core_config() below and not to
// this word -- which is why bring-up writes THIS one and every sub-transfer writes the other.
static inline uint32_t input_core_mode_word(void)
{
    return kI2cMiniCore | kI2cNValV2;
}

static inline uint32_t input_core_config(int reads)
{
    return input_core_mode_word() | (reads ? 0u : kQupNoInput);
}

// Which done flag a sub-transfer waits for. A segment that reads finishes on INPUT_DONE; a segment
// that only writes finishes on OUTPUT_DONE. Keeping this here means the poll condition and the
// verdict's `want` cannot disagree about which direction the segment ran.
static inline uint32_t input_done_mask(uint32_t rx_len)
{
    return rx_len ? kQupMxInputDone : kQupMxOutputDone;
}

// One sub-transfer's deadline, in milliseconds. A 400 kHz 12-byte transfer is under 1 ms, so this is
// two orders of magnitude of slack -- the number round181 shipped, now named once.
static const uint32_t kInputXferTimeoutMs = 100U;

// A word of up to four bytes, low byte first, with the unused top bytes left zero -- what upstream's
// send_last_word writes into QUP_OUT_FIFO_BASE.
static inline uint32_t input_pack_word(const uint8_t *bytes, uint32_t count)
{
    uint32_t word = 0;
    uint32_t i;
    if (bytes == 0 || count > 4u) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        word |= (uint32_t) bytes[i] << (i * 8);
    }
    return word;
}

// The output side's cursor. `room` is tracked in FIFO bytes and decremented by a whole word per write,
// which is the driver's own accounting and differs from upstream's per-byte tx_fifo_free
// (i2c-qup-mainline.c:1288-1289); the two cannot disagree for any request this driver issues because
// every one of them is shorter than one word of slack -- see notes/input-bringup.md §5, which measures
// the real FIFOs at 64/64 bytes from the card's own IO_MODE.
struct InputTx {
    uint32_t sent;
    uint32_t room;
};

static inline void input_tx_reset(struct InputTx *tx, uint32_t fifo_size)
{
    if (tx == 0) {
        return;
    }
    tx->sent = 0;
    tx->room = fifo_size;
}

// How much of the FIFO is still writable for a message whose `sent` bytes are already on their way.
// This is the driver's own inline expression, and it is measured against the message, not against the
// cursor's word accounting -- the two differ by the padding of the last partial word, which is why a
// second function is not worth having.
static inline uint32_t input_tx_room_left(uint32_t fifo_size, uint32_t sent)
{
    return fifo_size > sent ? fifo_size - sent : 0;
}

// Produces the next word to write, advancing the cursor. Returns 0 when the message is whole or the
// FIFO has less than one word left -- the two conditions upstream's loop tests in the same order.
static inline int input_tx_next(struct InputTx *tx, const uint8_t *bytes, uint32_t len,
                                uint32_t *word)
{
    uint32_t chunk;
    if (tx == 0 || word == 0 || bytes == 0) {
        return 0;
    }
    if (tx->sent >= len || tx->room < 4u) {
        return 0;
    }
    chunk = len - tx->sent;
    if (chunk > 4u) {
        chunk = 4u;
    }
    *word = input_pack_word(bytes + tx->sent, chunk);
    tx->sent += chunk;
    tx->room -= 4u;
    return 1;
}

// The input side's cursor: a delivered word is held, and tag bytes the core echoes back are dropped
// before anything reaches the caller's buffer. Level-gated, so this can never consume a byte the
// hardware did not deliver -- the rule round98 learned draining the eMMC FIFO.
struct InputRx {
    uint32_t got;
    uint32_t want;
    uint32_t tag_left;
    uint32_t word;
    uint32_t word_left;
};

static inline void input_rx_reset(struct InputRx *rx, uint32_t want, uint32_t tag_len)
{
    if (rx == 0) {
        return;
    }
    rx->got = 0;
    rx->want = want;
    rx->tag_left = want ? tag_len : 0;
    rx->word = 0;
    rx->word_left = 0;
}

static inline int input_rx_needs_word(const struct InputRx *rx)
{
    return rx != 0 && rx->word_left == 0 && rx->got < rx->want;
}

static inline int input_rx_complete(const struct InputRx *rx)
{
    return rx != 0 && rx->got == rx->want;
}

// Accept one word the hardware said is buffered. The caller only calls this after it has tested
// kQupInNotEmpty, which is what makes the whole drain level-gated.
static inline void input_rx_word(struct InputRx *rx, uint32_t word)
{
    if (rx == 0) {
        return;
    }
    rx->word = word;
    rx->word_left = 4u;
}

// Pop one byte: dropped while the echoed tags remain, stored otherwise. Returns 1 only when a byte
// reached the caller's buffer. Every call consumes one byte position of the held word, so a caller
// looping on `got < want` always advances and can never read past what arrived.
static inline int input_rx_pop(struct InputRx *rx, uint8_t *out)
{
    uint8_t byte;
    if (rx == 0 || rx->word_left == 0) {
        return 0;
    }
    byte = (uint8_t) (rx->word & 0xffu);
    rx->word >>= 8;
    rx->word_left--;
    if (rx->tag_left) {
        rx->tag_left--;
        return 0;
    }
    if (rx->got < rx->want && out != 0) {
        out[rx->got] = byte;
        rx->got++;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Requests: the bounds that refuse, and the tags that go on the wire
// ---------------------------------------------------------------------------

// A length field of 0 does not mean zero bytes, it means QUP_READ_LIMIT (i2c-qup-mainline.c:607-609),
// so the smallest honest question is one register byte and 256 is refused rather than wrapped.
static inline int input_tag_len_field(uint32_t len)
{
    return len == kQupReadLimit ? -1 : (int) len;
}

static inline int input_read_request_ok(uint32_t reg_len, uint32_t out_len)
{
    return reg_len >= 1 && reg_len <= 2 && out_len >= 1 &&
           out_len <= kQupReadLimit - kQupRxTagLen;
}

// qup_i2c_set_tags() for a v2 write of a 1- or 2-byte register address that is followed by a read:
// START, the 8-bit write address, DATAWR (never the STOP variant, because the repeated start is the
// whole point), then the byte count. Returns the number of tag bytes, 0 when the request would encode
// a length this core cannot express.
static inline uint32_t input_write_tags(uint8_t address, uint32_t reg_len, int last,
                                        uint8_t *tags, uint32_t tag_space)
{
    if (tags == 0 || tag_space < kQupMaxTagsLen || input_tag_len_field(reg_len) < 0) {
        return 0;
    }
    tags[0] = kQupTagV2Start;
    tags[1] = (uint8_t) (address << 1);
    tags[2] = last ? kQupTagV2DataWrStop : kQupTagV2DataWr;
    tags[3] = (uint8_t) reg_len;
    return kQupMaxTagsLen;
}

// The read tag block: same four bytes with R/W set and DATARD_STOP. It leaves through the OUTPUT FIFO
// (qup_i2c_write_rx_tags_v2), which is why it is a tx buffer and not a rx one.
static inline uint32_t input_read_tags(uint8_t address, uint32_t out_len, uint8_t *tags,
                                       uint32_t tag_space)
{
    if (tags == 0 || tag_space < kQupMaxTagsLen || input_tag_len_field(out_len) < 0) {
        return 0;
    }
    tags[0] = kQupTagV2Start;
    tags[1] = (uint8_t) ((address << 1) | 1u);
    tags[2] = kQupTagV2DataRdStop;
    tags[3] = (uint8_t) out_len;
    return kQupMaxTagsLen;
}

struct InputCounters {
    uint32_t write_cnt;    // MX_WRITE_CNT: the tx bytes, tags included
    uint32_t read_cnt;     // MX_READ_CNT: the echoed tag bytes plus the data
    int has_read;
};

// qup_i2c_conf_count_v2() (i2c-qup-mainline.c:1478-1483 is the same arithmetic): a read of N bytes
// asks the core for READ_RX_TAGS_LEN + N, and a pure write leaves READ_CNT untouched.
static inline void input_counters(uint32_t tx_len, uint32_t rx_len, int first,
                                  struct InputCounters *out)
{
    const uint32_t config_run = first ? 0u : kQupMxConfigDuringRun;
    if (out == 0) {
        return;
    }
    out->write_cnt = config_run | tx_len;
    out->has_read = rx_len ? 1 : 0;
    out->read_cnt = out->has_read ? (config_run | (kQupRxTagLen + rx_len)) : 0;
}

// ---------------------------------------------------------------------------
// Verdicts: what a finished sub-transfer means, and what the two error words mean
// ---------------------------------------------------------------------------

enum InputXferVerdict {
    kInputXferDone = 0,
    kInputXferNack,
    kInputXferBusError,
    kInputXferCoreError,
    kInputXferIncomplete
};

static const char *const kInputXferVerdictNames[5] = {
    "done", "nack", "bus-error", "core-error", "incomplete"
};

static inline const char *input_xfer_verdict_name(int verdict)
{
    return (verdict >= kInputXferDone && verdict <= kInputXferIncomplete)
               ? kInputXferVerdictNames[verdict]
               : kInputXferVerdictNames[kInputXferIncomplete];
}

// NACK outranks everything else because it is the one condition with a named meaning that is not a
// protocol fault of ours: the address byte was not acknowledged. Both error words are tested with
// their own tree's mask so a reader can see whether the other tree would have called it an error at
// all.
static inline int input_xfer_is_nack(uint32_t bus_status)
{
    return (bus_status & kQupNackFlag) ? 1 : 0;
}

// Stricter than upstream's FIFO write path, which lets any handled flag end the sub-transfer: the done
// bit AND both byte counts have to agree, so a transfer that stopped early is `incomplete` rather than
// short data that looks real.
static inline int input_xfer_verdict(uint32_t op, uint32_t core_err, uint32_t bus_err,
                                     uint32_t want, uint32_t sent, uint32_t tx_len,
                                     uint32_t got, uint32_t rx_len)
{
    if (core_err || bus_err) {
        if (input_xfer_is_nack(bus_err)) {
            return kInputXferNack;
        }
        return bus_err ? kInputXferBusError : kInputXferCoreError;
    }
    if (!(op & want) || got != rx_len || sent != tx_len) {
        return kInputXferIncomplete;
    }
    return kInputXferDone;
}

struct InputBusStatus {
    uint32_t raw;
    uint32_t mainline_error;   // raw & kI2cStatusErrorMask
    uint32_t vendor_error;     // raw & kI2cStatusVendorErrorMask
    uint32_t residue;          // set bits inside MAIN's mask that no source of the two names
    uint32_t bus_error, nack, arb_lost, invalid_write, failed, bus_active, bus_master;
    uint32_t invalid_tag, invalid_read_addr, invalid_read_seq, sda_high, scl_high;
};

// The whole point of this struct is sda_high/scl_high: they are the only fields here that MAIN's mask
// cannot carry, so a driver that reports the masked word and names the field `bus=` is reporting two
// constants and calling them a reading (notes/input-bringup.md §3).
static inline void input_bus_status_decode(uint32_t raw, struct InputBusStatus *out)
{
    uint32_t named;
    if (out == 0) {
        return;
    }
    out->raw = raw;
    out->mainline_error = raw & kI2cStatusErrorMask;
    out->vendor_error = raw & kI2cStatusVendorErrorMask;
    out->bus_error = (raw & kI2cStatusBusError) ? 1u : 0u;
    out->nack = (raw & kI2cStatusPacketNacked) ? 1u : 0u;
    out->arb_lost = (raw & kI2cStatusArbLost) ? 1u : 0u;
    out->invalid_write = (raw & kI2cStatusInvalidWrite) ? 1u : 0u;
    out->failed = (raw & kI2cStatusFailed) ? 1u : 0u;
    out->bus_active = (raw & kI2cStatusBusActive) ? 1u : 0u;
    out->bus_master = (raw & kI2cStatusBusMaster) ? 1u : 0u;
    out->invalid_tag = (raw & kI2cStatusInvalidTag) ? 1u : 0u;
    out->invalid_read_addr = (raw & kI2cStatusInvalidReadAddr) ? 1u : 0u;
    out->invalid_read_seq = (raw & kI2cStatusInvalidReadSeq) ? 1u : 0u;
    out->sda_high = (raw & kI2cStatusSdaSensor) ? 1u : 0u;
    out->scl_high = (raw & kI2cStatusSclSensor) ? 1u : 0u;
    named = kI2cStatusBusError | kI2cStatusPacketNacked | kI2cStatusArbLost |
            kI2cStatusInvalidWrite | kI2cStatusFailed | kI2cStatusBusActive |
            kI2cStatusBusMaster | kI2cStatusInvalidTag | kI2cStatusInvalidReadAddr |
            kI2cStatusInvalidReadSeq | kI2cStatusSdaSensor | kI2cStatusSclSensor;
    out->residue = out->mainline_error & ~(named & kI2cStatusErrorMask);
}

struct InputCoreFlags {
    uint32_t raw;
    uint32_t mainline_error;   // raw & kQupStatusErrorFlags
    uint32_t vendor_error;     // raw & kQupCoreVendorErrorMask
    uint32_t residue;
    uint32_t input_over_run, output_under_run, input_under_run, output_over_run;
};

static inline void input_core_flags_decode(uint32_t raw, struct InputCoreFlags *out)
{
    uint32_t named;
    if (out == 0) {
        return;
    }
    out->raw = raw;
    out->mainline_error = raw & kQupStatusErrorFlags;
    out->vendor_error = raw & kQupCoreVendorErrorMask;
    out->input_over_run = (raw & kCoreInputOverRun) ? 1u : 0u;
    out->output_under_run = (raw & kCoreOutputUnderRun) ? 1u : 0u;
    out->input_under_run = (raw & kCoreInputUnderRun) ? 1u : 0u;
    out->output_over_run = (raw & kCoreOutputOverRun) ? 1u : 0u;
    named = kCoreInputOverRun | kCoreOutputUnderRun | kCoreInputUnderRun | kCoreOutputOverRun;
    out->residue = out->mainline_error & ~(named & kQupStatusErrorFlags);
}

// ---------------------------------------------------------------------------
// The step/failure code in observation slot 15, decoded
// ---------------------------------------------------------------------------

// One word that says which step stopped, so the ring line, the observation array and the panic screen
// cannot each invent their own meaning for it. Positive values are steps that were reached; 100+ are
// the three ways a run stops; a stuck poll leaves step * 1000 + failure.
static const uint64_t kInputStepAhbVote = 1, kInputStepRcgUpdate = 2, kInputStepBranch = 3,
                      kInputStepPins = 4, kInputStepResetCore = 5, kInputStepMasterGen = 6,
                      kInputStepRun = 7, kInputStepPause = 8, kInputStepClk = 9;
static const uint64_t kInputFailMap = 100, kInputFailPoll = 101, kInputFailXfer = 102;

static inline uint64_t input_verdict_code(uint64_t step, uint64_t fail)
{
    return step * 1000u + fail;
}

// Which step a code names, for both shapes: a bare step number is the step that was reached, and a
// 1000-scaled one is the step that did not complete.
static inline uint64_t input_verdict_step(uint64_t code)
{
    return code >= 1000u ? code / 1000u : code;
}

static inline uint64_t input_verdict_failure(uint64_t code)
{
    if (code >= 1000u) {
        return code % 1000u;
    }
    return (code == kInputFailMap || code == kInputFailPoll || code == kInputFailXfer) ? code : 0;
}

static inline const char *input_step_name(uint64_t step)
{
    switch (step) {
        case kInputStepAhbVote: return "ahb-vote";
        case kInputStepRcgUpdate: return "rcg-update";
        case kInputStepBranch: return "branch";
        case kInputStepPins: return "pins";
        case kInputStepResetCore: return "core-reset";
        case kInputStepMasterGen: return "master-gen";
        case kInputStepRun: return "run";
        case kInputStepPause: return "pause";
        case kInputStepClk: return "clk";
        default: return "unknown-step";
    }
}

// Three different failures, three different names. `poll` means a bit that would not settle, which is
// a hardware answer; `xfer` means the bus refused a transaction, which is a different one; `map` means
// this driver never got to ask either question, which is a payload answer. Collapsing them into "fail"
// is how an instrument reads its own absence as the world's.
static inline const char *input_failure_name(uint64_t failure)
{
    switch (failure) {
        case 0: return "none";
        case kInputFailMap: return "map";
        case kInputFailPoll: return "poll-stuck";
        case kInputFailXfer: return "xfer-failed";
        default: return "unknown-failure";
    }
}

// ---------------------------------------------------------------------------
// Identification: Goodix's firmware block and FocalTech's name string
// ---------------------------------------------------------------------------

struct InputGoodixId {
    uint32_t pid_length;         // 3, or 4 when the byte after the three is not zero
    uint32_t version;            // (data[5] << 8) | data[4], big-endian as the vendor spells it
    uint32_t printable;          // how many of the pid bytes are printable ASCII
    char pid[5];
};

// gt9xx.c:1222-1258. The vendor's buffer is 8 bytes and its [0..1] are the register address it sent,
// so its pid bytes buf[2..4], its 4th-pid test buf[5] and its version (buf[7]<<8)|buf[6] are this
// driver's data[0..2], data[3] and data[5],data[4] -- the shift that InputRegs.h:322-326 documents and
// that nothing but a function can keep correct when someone reads the vendor file again.
static inline void input_goodix_identify(const uint8_t *data, uint32_t len,
                                         struct InputGoodixId *out)
{
    uint32_t i;
    if (out == 0 || data == 0 || len < kGtVersionLen) {
        return;
    }
    out->pid_length = data[3] ? 4u : 3u;
    out->version = ((uint32_t) data[5] << 8) | data[4];
    out->printable = 0;
    for (i = 0; i < 4u; i++) {
        const uint8_t byte = data[i];
        const int printable = byte >= 0x20 && byte < 0x7f;
        if (i < out->pid_length && printable) {
            out->printable++;
        }
        // The fourth slot is the vendor's optional character: an empty one -- which is exactly the
        // zero byte that makes pid_length 3 -- is rendered as the blank the round181 row printed,
        // and only the three mandatory slots get the '.' placeholder for a byte outside ASCII.
        if (i == 3u) {
            out->pid[i] = printable ? (char) byte : ' ';
        } else {
            out->pid[i] = printable ? (char) byte : '.';
        }
    }
    out->pid[4] = '\0';
}

// gtp_i2c_read_dbl_check() (gt9xx.c:217-246) asks twice and requires the two answers to be identical.
// Counting the agreeing bytes instead of returning a bool is this driver's choice, and it is the one
// that lets a reading say how far apart the two were; the vendor pre-fills its buffers 0xAA/0xAB so
// "read nothing" and "read zero" stay distinguishable, which this count cannot do.
static inline uint32_t input_bytes_agree(const uint8_t *a, const uint8_t *b, uint32_t len)
{
    uint32_t i;
    uint32_t agree = 0;
    if (a == 0 || b == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        agree += (a[i] == b[i]) ? 1u : 0u;
    }
    return agree;
}

// gt9xx.c:1250: a sensor_id of 0x06 or more means no config blob was ever sent, i.e. the chip answered
// but the driver-side config step has not run.
static inline int input_goodix_sensor_id_valid(uint8_t sensor_id)
{
    return sensor_id < kGtPidValidMax ? 1 : 0;
}

// edt-ft5x06-mainline.c:846-900: an M06 answers the 0xBB read with a leading response byte, so "EP0"
// starts at [1]; an M12 starts at [0]. The test the driver makes is on the letter E in either case.
static inline int input_ft_is_m06(const uint8_t *name, uint32_t len)
{
    if (name == 0 || len < 2) {
        return 0;
    }
    return name[1] == 'E' || name[1] == 'e';
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// The generic timer's frequency is only trusted inside 1 MHz..1 GHz, and the fallback is the board's
// own 19.2 MHz XO -- the same sanity test CloverSdhci runs, and the reason a bogus cntfrq read cannot
// silently become a 0-length deadline.
static inline uint64_t input_ticks_per_ms(uint64_t counter_frequency)
{
    if (counter_frequency >= 1000000ULL && counter_frequency <= 1000000000ULL) {
        return counter_frequency / 1000ULL;
    }
    return kXoHz / 1000ULL;
}

static inline uint64_t input_ticks_for_ms(uint32_t ms, uint64_t ticks_per_ms)
{
    return (uint64_t) ms * ticks_per_ms;
}

// The division that produced round170's compressed clock: an elapsed-tick count becomes milliseconds
// only through a non-zero ticks_per_ms, so a broken timer says 0 rather than a made-up number.
static inline uint32_t input_elapsed_ms(uint64_t elapsed_ticks, uint64_t ticks_per_ms)
{
    return (uint32_t) (elapsed_ticks / (ticks_per_ms ? ticks_per_ms : 1ULL));
}

#endif  // CLOVER_INPUT_INPUTCORE_H
