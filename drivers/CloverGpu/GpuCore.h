// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the GPU line's bus-free layer -- what the words CloverGpu.cpp reads
// *mean*, with no window, no IOKit object and no printf in it.
//
// WHY THIS FILE EXISTS (round317)
//
// Round314 recorded that the heaviest real gap against the UniXNU driver documentation was this
// repository's host-logic having no model (notes/unixnu-doc-conformance.md §7, gap 3), and round316
// closed it for CloverBlock's manifest walk. The GPU line had the same shape of code and the same
// absence: every decoding it performs was written inline in a printf argument list, so the only way to
// check one of them was to flash a board and read a row. That is not a hypothetical cost. Boot 81's
// LDO verdict was produced by a human comparing thirteen hex fields against a table in another file,
// and the sentence that came out -- "not one bit matches sdm660_ldo_config[]" -- was wrong in a way a
// function would not have been. The vendor's list has nine entries; eight of them name an offset this
// census reads; seven of those eight expect a non-zero word and read zero; the eighth,
// LDO_LD_DATA_REG, expects 0x00000000 and reads 0x00000000, so it *does* match. And two of the
// thirteen words are not zero at all. The conclusion round294 drew -- nobody has ever run this init
// list on this board -- survives intact and is now the thing actually measured; the arithmetic around
// it did not.
//
// WHAT MAY LIVE HERE
//
// Bit-field identity, bounds, counts, and the order in which a question gets asked -- for instance
// that a page answering 0xffffffff has to be excluded from a match count before its bits are cut,
// because cutting them first turns a dead page into a corner value. Nothing that reads or writes a
// register, nothing that needs a kernel address, nothing that formats a row. CloverGpu.cpp keeps the
// MMIO, the gates that decide *whether* to read, and every printed field.
//
// NO NUMBER IS RE-TYPED
//
// GpuRegs.h is already a bus-free header of offsets, masks and values, each quoted to the vendor file
// it came from. This layer includes it and derives from those names rather than writing the same
// literals again: two copies of 0x9018 would be one bug away from a census that compares the wrong
// register's expectation against the right register's value. What is new here is only what GpuRegs.h
// did not need -- the vendor's expectation table, the register names, and the bounds its own decoder
// does not state.
//
// SOURCES ARE LINE-NUMBERED ON PURPOSE
//
// notes/unixnu-doc-conformance.md §8 permits a line-number citation only for a frozen target.
// notes/sources/sdm660/msm_gfx_ldo.c (sha256 929bc99f86f1) is a vendored vendor file this repository
// does not edit, so its line numbers hold; every table row and bit name below is transcribed from it.

#ifndef CLOVER_GPU_GPUCORE_H
#define CLOVER_GPU_GPUCORE_H

#include <stdint.h>

#include "GpuRegs.h"

// The board's own "this page did not answer" signature: the one round282 and round286 gate GPUCC
// behind, and the reason this layer tests it before it decodes anything. Every decoder here that could
// mistake it for data checks it first.
static const uint32_t kGpuNoAnswer = kMemAccNoAnswer;

// ---------------------------------------------------------------------------
// The LDO block: geometry, names, expectations
// ---------------------------------------------------------------------------

// `gfx_ldo_vreg: ldo@0506e000` declares reg = <0x0506e000 0x34> (sdm660-regulator.dtsi:523-525) and the
// GPUCC window this line already maps starts at 0x05065000, so the block sits at GPUCC offset 0x9000 --
// which is what GpuRegs.h's kGpuccLdoAtest already says, so it is taken from there and not restated.
// Two DT nodes claim that extent, which is why the line reads through the existing mapping instead of
// asking IOKit for a fourth window; that decision lives in CloverGpu.cpp, the arithmetic of the offset
// lives here.
static const uint32_t kGpuLdoBlockBase = kGpuccLdoAtest;
static const uint32_t kGpuLdoWindowBytes = 0x34U;      // the DT's own length: offsets 0x00..0x33
static const uint32_t kGpuLdoWordCount = 13U;          // every word inside it, 0x00..0x30

// The vendor's own census walks 11 words at i*4 (MAX_LDO_REGS :69, dump_registers() :253-262) and names
// them in register_str[] (:240-251). The two REF_CURRENT registers are named by #defines (:61, :62) at
// 0x2c and 0x30 -- inside the DT extent but outside that dump loop. The table below is the union, which
// is what round294's row actually reads.
struct GpuLdoName {
    uint32_t offset;          // block-relative, in the units the vendor's #defines are in
    const char *name;
};

static const GpuLdoName kGpuLdoNames[] = {
    {0x00U, "LDO_ATEST"},          // LDO_ATEST_REG :24, register_str[0]
    {0x04U, "LDO_CFG0"},           // LDO_CFG0_REG :25, register_str[1]
    {0x08U, "LDO_CFG1"},           // LDO_CFG1_REG :26, register_str[2]
    {0x0cU, "LDO_CFG2"},           // LDO_CFG2_REG :27, register_str[3]
    {0x10U, "LDO_LD_DATA"},        // LDO_LD_DATA_REG :28, register_str[4]
    {0x14U, "LDO_VREF_TEST_CFG"},  // LDO_VREF_TEST_CFG :30, register_str[5]
    {0x18U, "LDO_VREF_SET"},       // LDO_VREF_SET_REG :34, register_str[6]
    {0x1cU, "PWRSWITCH_CTL"},      // PWRSWITCH_CTRL_REG :39, register_str[7]
    {0x20U, "LDO_STATUS0"},        // register_str[8] only -- no #define names this offset
    {0x24U, "LDO_STATUS1"},        // LDO_STATUS1_REG :54, register_str[9]
    {0x28U, "PWRSWITCH_STATUS"},   // PWRSWITCH_STATUS_REG :56, register_str[10]
    {0x2cU, "REF_CURRENT_X1"},     // REF_CURRENT_X1_REG :61 -- not in the vendor's dump loop
    {0x30U, "REF_CURRENT_X2"},     // REF_CURRENT_X2_REG :62 -- not in the vendor's dump loop
};

static const uint32_t kGpuLdoNameCount =
    static_cast<uint32_t> (sizeof (kGpuLdoNames) / sizeof (kGpuLdoNames[0]));

// sdm660_ldo_config[] (msm_gfx_ldo.c:147-158): the list this SoC's probe writes. Probe sets
// ops_type = VOLTAGE for SDM660_SOC_ID (:1561-1562) and its loop (:1135-1140) writel_relaxed's each
// .value at each .offset until the LDO_MAX_OFFSET sentinel. Nine entries, eight of which name an offset
// inside the DT extent; ADC_CTL_REG 0x34 is the ninth and is out of bounds, so this line not reading it
// is correct rather than a gap -- and saying which of the two it is needs the count.
struct GpuLdoExpect {
    uint32_t offset;
    uint32_t value;
};

static const GpuLdoExpect kGpuLdoExpected[] = {
    {0x00U, 0x00000080U},  // LDO_ATEST_REG
    {0x04U, 0x0100A600U},  // LDO_CFG0_REG
    {0x08U, 0x000000A0U},  // LDO_CFG1_REG
    {0x0cU, 0x0000C3FEU},  // LDO_CFG2_REG
    {0x10U, 0x00000000U},  // LDO_LD_DATA_REG -- expected zero, so a zero here is an agreement
    {0x14U, 0x00401100U},  // LDO_VREF_TEST_CFG
    {0x2cU, 0x00000230U},  // REF_CURRENT_X1_REG
    {0x30U, 0x00000048U},  // REF_CURRENT_X2_REG
    {0x34U, 0x00000000U},  // ADC_CTL_REG -- inside the table, outside the DT window
};

static const uint32_t kGpuLdoExpectCount =
    static_cast<uint32_t> (sizeof (kGpuLdoExpected) / sizeof (kGpuLdoExpected[0]));

// The vendor's probe performs one read-modify-write before that list, at :1128-1133: clear
// LDO_CLAMP_IO_BIT (BIT(31), :40) and set EN_LDOAP_CTRL_CPR_BIT (BIT(29), :42) in PWRSWITCH_CTRL_REG.
// A later round that writes this block inherits exactly those two bits plus the nine-entry list, so the
// whole plan is spelled once, here, where a host can check it before a board pays for it.
static const uint32_t kGpuLdoCtrlClampIo = 0x80000000U;      // LDO_CLAMP_IO_BIT :40
static const uint32_t kGpuLdoCtrlEnLdoapCpr = 0x20000000U;   // EN_LDOAP_CTRL_CPR_BIT :42

// The word the vendor's first write produces. Returns false for a page that did not answer, because
// "clear CLAMP_IO, set EN_LDOAP_CTRL_CPR" on 0xffffffff is not a value anybody should write: it is
// 0x7fffffff, every other bit still high -- bypass, power-down and all three boot-head-switch bits set --
// dressed up as a programmed register.
static inline bool
gpu_ldo_ctrl_probe_word (uint32_t before, uint32_t *after)
{
    if (before == kGpuNoAnswer) {
        return false;
    }
    *after = (before & ~kGpuLdoCtrlClampIo) | kGpuLdoCtrlEnLdoapCpr;
    return true;
}

// The GPUCC offset word `index` is read at, in the units the driver's window pointer is indexed in.
static inline uint32_t
gpu_ldo_word_gpucc_offset (uint32_t index)
{
    return kGpuLdoBlockBase + 4U * index;
}

// The vendor's name for a block offset, or a null pointer when that offset is not this block's. The
// driver passes `kGpuccLdo<X> - kGpuLdoBlockBase`, so
// a named constant that drifts away from its documented offset stops naming a word, and the census's
// `named` count falls off 13 and says so on the next fire. That is the guard against the one failure
// mode a table like this can hide: a silent shift, where the wrong register's expectation is compared
// against the right register's value and the verdict still looks like a verdict.
static inline const char *
gpu_ldo_name (uint32_t block_offset)
{
    for (uint32_t i = 0; i < kGpuLdoNameCount; i++) {
        if (kGpuLdoNames[i].offset == block_offset) {
            return kGpuLdoNames[i].name;
        }
    }
    return 0;
}

// The vendor's expected word for a block offset, or false when sdm660_ldo_config[] names nothing there.
static inline bool
gpu_ldo_expect (uint32_t block_offset, uint32_t *value)
{
    for (uint32_t i = 0; i < kGpuLdoExpectCount; i++) {
        if (kGpuLdoExpected[i].offset == block_offset) {
            if (value != 0) {
                *value = kGpuLdoExpected[i].value;
            }
            return true;
        }
    }
    return false;
}

// Whether an offset the table names is inside the extent the device tree declares. ADC_CTL_REG 0x34 is
// the table's ninth entry and the DT's first excluded byte.
static inline bool
gpu_ldo_expect_in_window (uint32_t block_offset)
{
    return block_offset < kGpuLdoWindowBytes;
}

// How many of the nine sit inside that extent. It is eight, and the number belongs in the layer rather
// than in a sentence: every match rate below is out of this, and a reader who assumed nine would say
// "8 of 9 differ" where the honest claim is "7 of 8 comparable words differ, and the eighth agrees
// because both the reading and the expectation are zero".
static inline uint32_t
gpu_ldo_expect_inside_window (void)
{
    uint32_t inside = 0;

    for (uint32_t i = 0; i < kGpuLdoExpectCount; i++) {
        if (gpu_ldo_expect_in_window (kGpuLdoExpected[i].offset)) {
            inside++;
        }
    }
    return inside;
}

// ---------------------------------------------------------------------------
// The census verdict
// ---------------------------------------------------------------------------

struct GpuLdoCensus {
    uint32_t words;              // what the caller read
    uint32_t named;              // offsets the vendor names at all -- must be `words` or the map is wrong
    uint32_t comparable;         // named, in the window, tabled, and the page answered
    uint32_t agree;              // comparable AND equal to the vendor's word
    uint32_t differ;             // comparable AND not equal
    uint32_t dead;               // words that read 0xffffffff, excluded from agree and differ
    uint32_t zero;               // words that read exactly 0, named or not
    uint32_t nonzero_expected;   // comparable words whose vendor expectation is not 0
    uint32_t nonzero_read_zero;  // of those, the ones reading 0 -- "this list was never written"
    uint32_t no_expectation;     // inside the window and named, but the table is silent
    uint32_t table_outside;      // table entries the DT extent excludes
};

// `control_on` is the same-beat proof that the GPUCC window answers: CloverGpu.cpp reads CX's PWR_ON
// beside the census for exactly this purpose, because a block of zeros without a control is a page that
// may simply not have replied, and a block of zeros *with* one is a page that replied "zero".
static inline void
gpu_ldo_census (const uint32_t *block_offsets, const uint32_t *values, uint32_t count,
                GpuLdoCensus *out)
{
    *out = GpuLdoCensus ();
    out->words = count;

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t offset = block_offsets[i];
        const bool named = gpu_ldo_name (offset) != 0;
        uint32_t expected = 0;

        if (named) {
            out->named++;
        }
        if (values[i] == 0U) {
            out->zero++;
        }
        if (values[i] == kGpuNoAnswer) {
            out->dead++;
            continue;
        }
        if (!gpu_ldo_expect (offset, &expected)) {
            if (named && gpu_ldo_expect_in_window (offset)) {
                out->no_expectation++;
            }
            continue;
        }
        if (!gpu_ldo_expect_in_window (offset)) {
            // Reachable only if a caller reads past the DT extent. Counted by the loop below, never
            // here, so one out-of-bounds table entry is never in two fields at once.
            continue;
        }
        out->comparable++;
        if (expected != 0U) {
            out->nonzero_expected++;
            if (values[i] == 0U) {
                out->nonzero_read_zero++;
            }
        }
        if (values[i] == expected) {
            out->agree++;
        } else {
            out->differ++;
        }
    }

    // Table entries the device tree's own extent excludes. A property of the two tables, not of the
    // reading: it is the difference between "the vendor's list has nine words and eight are comparable
    // here" and "we compared everything there is to compare".
    for (uint32_t i = 0; i < kGpuLdoExpectCount; i++) {
        if (!gpu_ldo_expect_in_window (kGpuLdoExpected[i].offset)) {
            out->table_outside++;
        }
    }
}

enum GpuLdoShape {
    GPU_LDO_SHAPE_NO_WORDS = 0,      // the caller read nothing; a verdict would be invented
    GPU_LDO_SHAPE_UNNAMED,           // the offsets are not this block's -- a mapping mistake, not a reading
    GPU_LDO_SHAPE_DEAD_PAGE,         // every word reads 0xffffffff
    GPU_LDO_SHAPE_BLIND,             // the list is unwritten and the same-beat control did not answer
    GPU_LDO_SHAPE_UNPROGRAMMED,      // the list is unwritten, control on: nobody has ever run it
    GPU_LDO_SHAPE_PARTIAL,           // written, but not to this table
    GPU_LDO_SHAPE_AS_EXPECTED        // every comparable word matches the table
};

// The names this layer hands back, so a row's shape field is one function's output and not a string a
// caller types. They are CStrings a payload verifier can find in an image; the enum alone would not be.
static const char *const kGpuLdoShapeNames[] = {
    "no-words", "unnamed-mapping", "dead-page", "list-unwritten-blind", "list-never-written",
    "partial", "as-expected"
};

static inline const char *
gpu_ldo_shape_name (GpuLdoShape shape)
{
    const uint32_t last = static_cast<uint32_t> (
        sizeof (kGpuLdoShapeNames) / sizeof (kGpuLdoShapeNames[0]));

    return static_cast<uint32_t> (shape) < last ? kGpuLdoShapeNames[shape] : "unknown-shape";
}

static inline GpuLdoShape
gpu_ldo_shape (const GpuLdoCensus &census, bool control_on)
{
    if (census.words == 0) {
        return GPU_LDO_SHAPE_NO_WORDS;
    }
    // A census whose offsets the vendor does not name is not a reading of this block at all, so every
    // verdict below would be a coincidence. This is the only arm that can fire on a mapping mistake.
    if (census.named != census.words) {
        return GPU_LDO_SHAPE_UNNAMED;
    }
    if (census.dead == census.words) {
        return GPU_LDO_SHAPE_DEAD_PAGE;
    }
    // The claim boot 81 actually supports, stated as the thing that was measured rather than as "all
    // thirteen words are zero" -- which they are not: PWRSWITCH_CTL holds its reset value and
    // LDO_STATUS1 reads 0x0d. What holds is that every word the init list would write a non-zero value
    // to reads zero, seven of seven. That is "this list has never run on this board".
    const bool list_unwritten = census.nonzero_expected != 0 &&
        census.nonzero_read_zero == census.nonzero_expected;

    if (list_unwritten) {
        return control_on ? GPU_LDO_SHAPE_UNPROGRAMMED : GPU_LDO_SHAPE_BLIND;
    }
    if (census.zero == census.words) {
        // No non-zero expectation at all was reachable, so all that can be said is that the window is
        // blank; without the control that is indistinguishable from a page that is not there.
        return control_on ? GPU_LDO_SHAPE_UNPROGRAMMED : GPU_LDO_SHAPE_BLIND;
    }
    if (census.comparable != 0 && census.agree == census.comparable) {
        return GPU_LDO_SHAPE_AS_EXPECTED;
    }
    return GPU_LDO_SHAPE_PARTIAL;
}

// ---------------------------------------------------------------------------
// PWRSWITCH_CTRL_REG (0x1c): the bit names the vendor file actually has
// ---------------------------------------------------------------------------

struct GpuLdoCtrl {
    uint32_t raw;
    bool clamp_io;          // BIT(31) LDO_CLAMP_IO_BIT :40
    bool cpr_bypass;        // BIT(30) CPR_BYPASS_IN_LDO_MODE_BIT :41
    bool en_ldoap_cpr;      // BIT(29) EN_LDOAP_CTRL_CPR_BIT :42
    bool cx_cpr_bypass;     // BIT(10) CX_CPR_BYPASS_IN_LDO_MODE_BIT :43
    bool pwr_src_sel;       // BIT(9)  PWR_SRC_SEL_BIT :44
    bool ack_sw_ovr;        // BIT(8)  ACK_SW_OVR_BIT :45
    bool preon_sw_ovr;      // BIT(7)  LDO_PREON_SW_OVR_BIT :46
    bool bypass;            // BIT(6)  LDO_BYPASS_BIT :47
    bool pdn;               // BIT(5)  LDO_PDN_BIT :48
    bool under_sw_ctrl;     // BIT(4)  LDO_UNDER_SW_CTRL_BIT :49
    bool bhs_rest;          // BIT(2)  BHS_EN_REST_BIT :51
    bool bhs_few;           // BIT(1)  BHS_EN_FEW_BIT :52
    bool bhs_under_sw_ctl;  // BIT(0)  BHS_UNDER_SW_CTL :53
    uint32_t unnamed;       // every set bit that list does not cover
    bool answered;          // false for 0xffffffff: a dead page's bits are not a state
};

// The bits msm_gfx_ldo.c:39-53 names, and the residue it does not. BIT(3) is named by nobody in that
// file -- GpuRegs.h:101-105 quotes only five of these thirteen, and round291's row printed three of
// them. Boot 81 read 0x80000038, which is CLAMP_IO | PDN | UNDER_SW_CTRL | BIT(3): the residue is why
// this struct carries `unnamed` at all. A decoder that dropped it silently would have made that word
// look fully explained, which is the mistake the instrument-horizon rule in the notes is about.
static const uint32_t kGpuLdoCtrlNamedBits = 0x80000000U | 0x40000000U | 0x20000000U | 0x00000400U |
    0x00000200U | 0x00000100U | 0x00000080U | kLdoBypass | kLdoPdn | 0x00000010U | 0x00000004U |
    0x00000002U | 0x00000001U;

static inline void
gpu_ldo_decode_ctrl (uint32_t raw, GpuLdoCtrl *out)
{
    *out = GpuLdoCtrl ();
    out->raw = raw;
    out->answered = raw != kGpuNoAnswer;
    out->unnamed = raw & ~kGpuLdoCtrlNamedBits;
    if (!out->answered) {
        return;
    }
    out->clamp_io = (raw & kGpuLdoCtrlClampIo) != 0;
    out->cpr_bypass = (raw & kLdoCprBypass) != 0;
    out->en_ldoap_cpr = (raw & kGpuLdoCtrlEnLdoapCpr) != 0;
    out->cx_cpr_bypass = (raw & 0x00000400U) != 0;
    out->pwr_src_sel = (raw & 0x00000200U) != 0;
    out->ack_sw_ovr = (raw & 0x00000100U) != 0;
    out->preon_sw_ovr = (raw & 0x00000080U) != 0;
    out->bypass = (raw & kLdoBypass) != 0;
    out->pdn = (raw & kLdoPdn) != 0;
    out->under_sw_ctrl = (raw & 0x00000010U) != 0;
    out->bhs_rest = (raw & 0x00000004U) != 0;
    out->bhs_few = (raw & 0x00000002U) != 0;
    out->bhs_under_sw_ctl = (raw & 0x00000001U) != 0;
}

// ---------------------------------------------------------------------------
// LDO_VREF_SET_REG (0x18) and PWRSWITCH_STATUS_REG (0x28)
// ---------------------------------------------------------------------------

static const uint32_t kGpuLdoDriverMaxUv = 980000U;  // MAX_LDO_VOLTAGE :66
static const uint32_t kGpuLdoDtMinUv = 400000U;      // regulator-min-microvolt, dtsi:528
static const uint32_t kGpuLdoDtMaxUv = 925000U;      // regulator-max-microvolt, dtsi:529

struct GpuLdoVref {
    uint32_t raw;
    uint32_t val;
    uint32_t uv;
    bool answered;
    bool in_driver_range;   // what msm_gfx_ldo.c's own constants allow
    bool in_dt_range;       // what this board's node allows -- and it is the tighter of the two
};

// The decode is the vendor's, get_voltage() :1371: `*val = (reg & VREF_VAL_MASK) * LDO_STEP_VOLATGE +
// MIN_LDO_VOLTAGE`. What this layer adds is the two ranges, because the mask is seven bits and seven
// bits allow 0x7f: val 127 is 1,010,000 uV, above the driver's own MAX_LDO_VOLTAGE and above the DT's
// max. A row that printed only `val=` and `uv=` could therefore describe a voltage no regulator on this
// board accepts -- and did, for every reading in the interval. The DT's bounds are the tighter pair
// (val 5..110, which is what GpuRegs.h quotes at :110-111), so a word between the two sets of bounds is
// legal to the vendor driver and illegal on this unit.
static inline void
gpu_ldo_decode_vref (uint32_t raw, GpuLdoVref *out)
{
    *out = GpuLdoVref ();
    out->raw = raw;
    out->answered = raw != kGpuNoAnswer;
    if (!out->answered) {
        return;
    }
    out->val = raw & kLdoVrefMask;
    out->uv = out->val * kLdoStepUv + kLdoMinUv;
    out->in_driver_range = out->uv <= kGpuLdoDriverMaxUv && out->uv >= kLdoMinUv;
    out->in_dt_range = out->uv <= kGpuLdoDtMaxUv && out->uv >= kGpuLdoDtMinUv;
}

// PWRSWITCH_STATUS_REG's two bits (:57-58) and the predicate the vendor actually waits on, :307:
// `while ((status & (LDO_VREF_SETTLED_BIT | LDO_READY_BIT)) == 0)`. The predicate is an *or*, so a page
// that sets READY alone passes the vendor's own settling test even though the pr_err() after it names
// LDO_VREF_SETTLED. Report the two bits and the predicate as three fields: a row that carried only the
// predicate would let one bit answer a question that has two inputs.
static inline bool
gpu_ldo_status_ready (uint32_t raw)
{
    return raw != kGpuNoAnswer && (raw & kLdoReady) != 0;
}

static inline bool
gpu_ldo_status_settled (uint32_t raw)
{
    return raw != kGpuNoAnswer && (raw & kLdoSettled) != 0;
}

static inline bool
gpu_ldo_vendor_settled (uint32_t raw)
{
    return raw != kGpuNoAnswer && (raw & (kLdoSettled | kLdoReady)) != 0;
}

// ---------------------------------------------------------------------------
// GDSC: the word the vendor's sequence produces, and what a read word means
// ---------------------------------------------------------------------------

// gdsc_init()'s write: clear HW_CONTROL and SW_OVERRIDE, set the three wait timers to the values
// gdsc_toggle_logic() then relies on. Pure arithmetic on the word that was there -- the part a host can
// prove and a board cannot. The mask and the value are GpuRegs.h's, built there from three GENMASKs.
static inline uint32_t
gpu_gdsc_init_word (uint32_t before)
{
    return (before & ~(kGdscHwControl | kGdscSwOverride | kGdscWaitMask)) | kGdscWaitVal;
}

// gdsc_toggle_logic(true)'s write: clear SW_COLLAPSE and nothing else.
static inline uint32_t
gpu_gdsc_release_word (uint32_t before)
{
    return before & ~kGdscSwCollapse;
}

// The two tests a read word answers, kept apart on purpose. `pwr_on` is bit31 alone, which is what
// gdsc-regulator.c's poll waits on and what every row of this line has printed since round286.
// `powered` is the pair GpuRegs.h:183-185 documents -- bit31 set *and* bit0 clear. A GDSCR that reads
// PWR_ON with SW_COLLAPSE still set has not actually been released, and boot 77's gx is the reading
// that made the difference worth having a name for.
static inline bool gpu_gdsc_pwr_on (uint32_t word) { return (word & kGdscPwrOn) != 0; }
static inline bool gpu_gdsc_collapsed (uint32_t word) { return (word & kGdscSwCollapse) != 0; }

static inline bool
gpu_gdsc_powered (uint32_t word)
{
    return gpu_gdsc_pwr_on (word) && !gpu_gdsc_collapsed (word);
}

// ---------------------------------------------------------------------------
// A clock branch: the request and the halt are two answers, not one
// ---------------------------------------------------------------------------

// BRANCH_HALT is `0 /* pol: 1 = halt */` (clk-branch.h:31), so a 1 at bit31 says the branch is stopped
// however bit0 reads. Round286 printed bit0 alone, which is why boot 74 could say "gfx3d was asked for"
// and mean nothing more; round290 added the halt bit. `running` is the conjunction, and it is the only
// one of the three that is allowed to be called the state of a clock.
static inline bool gpu_branch_requested (uint32_t word) { return (word & 1u) != 0; }
static inline bool gpu_branch_halted (uint32_t word) { return (word & kBranchClkOff) != 0; }

static inline bool
gpu_branch_running (uint32_t word)
{
    return gpu_branch_requested (word) && !gpu_branch_halted (word);
}

// The GCC gate round282 measured and round286 moved after its own write: the AHB vote has to be set
// *and* the word has to have answered. Both halves matter, and they are different failures -- a vote of
// 0 means nobody clocked the path, a word of 0xffffffff means the path does not reply, and round286's
// row would happily have powered GPUCC behind the second one if it had tested only bit0.
static inline bool
gpu_ahb_gate_open (uint32_t ahb_word)
{
    return gpu_branch_requested (ahb_word) && ahb_word != kGpuNoAnswer;
}

// ---------------------------------------------------------------------------
// The chipid gate: which refusal a row has to name
// ---------------------------------------------------------------------------

enum GpuChipidGate {
    GPU_CHIPID_READS = 0,    // all three gates open, so a bad chipid is a fact about the core
    GPU_CHIPID_NO_CX,        // the CX domain never powered up
    GPU_CHIPID_NO_GX,        // CX is up, GX is not -- boot 77's and boot 81's shape
    GPU_CHIPID_NO_GFX3D      // both domains up, the gfx3d branch was never requested
};

// The order is the driver's, because one refusal row names one gate and the naming has to be
// deterministic when several are closed. CX first: the AHB vote and the CX poll are what every later
// write rides on.
static inline GpuChipidGate
gpu_chipid_gate (bool cx_on, bool gx_on, bool gfx3d_requested)
{
    if (!cx_on) {
        return GPU_CHIPID_NO_CX;
    }
    if (!gx_on) {
        return GPU_CHIPID_NO_GX;
    }
    if (!gfx3d_requested) {
        return GPU_CHIPID_NO_GFX3D;
    }
    return GPU_CHIPID_READS;
}

static const char *const kGpuChipidGateNames[] = {
    "reads", "no-cx", "no-gx", "no-gfx3d"
};

static inline const char *
gpu_chipid_gate_name (GpuChipidGate gate)
{
    const uint32_t last = static_cast<uint32_t> (
        sizeof (kGpuChipidGateNames) / sizeof (kGpuChipidGateNames[0]));

    return static_cast<uint32_t> (gate) < last ? kGpuChipidGateNames[gate] : "unknown-gate";
}

// ---------------------------------------------------------------------------
// The L1 MEM_ACC corner select (round311's window)
// ---------------------------------------------------------------------------

// `regulator@01fcf004` declares acc-sel-l1 at bit-pos 0, bit-size 1, and maps corner 1 to field 0x01 and
// corner 2 to field 0x00. Because the field is one bit wide, the masked field of 0xffffffff is 1 -- which
// is also corner 1's map value. A decoder that masked first and asked afterwards would report a page that
// never replied as a powered corner, and those are opposite answers. GpuRegs.h:177-181 states the rule;
// this is where it lives as code.
struct GpuMemAcc {
    uint32_t raw;
    bool answered;        // tested before the mask, and that order is the whole point
    uint32_t field;       // 0 when unanswered, not the masked residue of 0xffffffff
    uint32_t corner;      // 1, 2, or 0 when the field maps to no corner this node declares
    bool corner_named;
};

static inline void
gpu_mem_acc_decode (uint32_t raw, GpuMemAcc *out)
{
    *out = GpuMemAcc ();
    out->raw = raw;
    out->answered = raw != kMemAccNoAnswer;
    if (!out->answered) {
        return;
    }
    out->field = raw & kMemAccFieldMask;
    if (out->field == kMemAccCorner1Field) {
        out->corner = kMemAccMinCorner;
        out->corner_named = true;
    } else if (out->field == kMemAccCorner2Field) {
        out->corner = kMemAccMaxCorner;
        out->corner_named = true;
    }
}

// The wrong version, kept here so the host model can *demonstrate* the mistake rather than assert that
// it was avoided. It is not called from CloverGpu.cpp; nothing in a shipped path depends on it.
static inline uint32_t
gpu_mem_acc_decode_before_mask (uint32_t raw)
{
    const uint32_t field = raw & kMemAccFieldMask;

    return field == kMemAccCorner1Field ? kMemAccMinCorner :
        (field == kMemAccCorner2Field ? kMemAccMaxCorner : 0U);
}

#endif // CLOVER_GPU_GPUCORE_H
