// Host model of this board's GPU power-and-clock readings, driving drivers/CloverGpu/GpuCore.h against
// words this repository already has on disk. Ported in shape from drivers/CloverBlock/tests/
// clover_block_host.cpp and drivers/CloverPower/tests/clover_power_host.cpp, which do the same thing for
// the manifest walk and the SPMI arbiter: the split exists because a decode that has only ever run on the
// device is a decode whose bugs are discovered on the device.
//
// WHAT THIS IS FOR, SPECIFICALLY
//
// The GPU line's entire output is decodings of words: which bits of a branch register mean "running",
// which word of an LDO block the vendor's init list would have written, which of two GDSC bits makes a
// domain powered on. None of them move bytes the way CloverBlock's manifest walk does, so the harm here
// is not corruption -- it is a *reading that means something it does not mean*. Round294 produced exactly
// one of those, and this file starts with it because that is the honest reason the layer exists.
//
// THE FINDING THIS MODEL BOUGHT
//
// Boot 81 read the thirteen-word LDO census (clover-out/r294-boot81-unattended.log, t=859 and t=980, both
// identical). What we recorded was "not one bit matches sdm660_ldo_config[]" (notes/development-log.md
// Round294 §④ and the matching porting.toml finding). Compared by a function against the nine-entry
// table, the truth is narrower and the conclusion is unchanged: eight of the nine entries name an offset
// inside the DT's 0x34-byte extent (the ninth, ADC_CTL_REG 0x34, is outside it and so could never be
// read here); of those eight, seven expect a non-zero word and read 0x00000000, and the eighth --
// LDO_LD_DATA_REG, expected 0x00000000 -- reads 0x00000000 and therefore *does* match. So: 7 differ, 1
// agrees, and "nobody has ever run this init list on this board" is supported by 7-of-7 rather than by
// "13 words of nothing", which is what round294's own words also did not say (two of the thirteen are
// non-zero). The overstatement made the finding look stronger than the arithmetic; the arithmetic is
// stronger, because it names the register whose absence from the list was silently dropped.
//
// WHY THE FAILURE PATHS ARE BUILT RATHER THAN ARGUED ABOUT
//
// Three of the arms below exist to show that a guard is load-bearing, by turning it off: a page of
// 0xffffffff counted as a disagreement (it is not -- it is a non-reading), a one-bit MEM_ACC field
// masked before the no-answer test (a dead page becomes corner 1), and the same-beat control set to 0
// (the identical census stops licensing "unprogrammed" and has to say "blind" instead). Each asserts the
// wrong answer and the right answer side by side, because a guard nobody has seen fire is a comment.
//
// WHAT THE MODEL DELIBERATELY DOES NOT REPRODUCE
//
// It does not model the MMIO, the timer, or the gates that decide whether a window is touched at all --
// those are the driver's, and a wrong one of them costs a boot rather than a rebuild. It keeps one claim
// that is a measurement rather than a design decision: the vendor's get_voltage() decode, applied to the
// reset value of LDO_VREF_SET_REG, produces 375,000 uV, which is below the 400,000 uV floor this unit's
// own device tree declares for that regulator. That is today's behaviour and the model asserts it, so
// the next round that wants a clamp there can see exactly which line it is changing.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../GpuCore.h"

namespace {

int checks = 0;

void
expect (bool condition, const char *what)
{
    if (!condition) {
        printf ("FAIL: %s\n", what);
        checks++;
    }
}

// The thirteen block offsets CloverGpu.cpp reads, in the order its row prints them. The row's fields are
// named by GpuRegs.h's constants; this array is the same list in block-relative units, which is what
// GpuCore.h indexes by.
const uint32_t kOffsets[kGpuLdoWordCount] = {
    0x00U, 0x04U, 0x08U, 0x0cU, 0x10U, 0x14U, 0x18U, 0x1cU, 0x20U, 0x24U, 0x28U, 0x2cU, 0x30U
};

// Boot 81's census, verbatim from the two `clover r294: gpu ... ldo words beat` rows in
// clover-out/r294-boot81-unattended.log. Same-beat cx_on=1, gx_on=0.
const uint32_t kBoot81[kGpuLdoWordCount] = {
    0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U,
    0x80000038U, 0x00000000U, 0x0000000dU, 0x00000000U, 0x00000000U, 0x00000000U
};

}  // namespace

int
main (void)
{
    // -----------------------------------------------------------------------
    // 1. Boot 81's census, compared by the layer instead of by eye.
    // -----------------------------------------------------------------------
    GpuLdoCensus c81;
    gpu_ldo_census (kOffsets, kBoot81, kGpuLdoWordCount, &c81);

    expect (c81.words == 13 && c81.named == 13, "boot 81: every offset read is one the vendor names");
    expect (c81.dead == 0, "boot 81: no word read 0xffffffff, so nothing here is a non-reading");
    // The correction, in numbers.
    expect (c81.comparable == 8, "boot 81: eight table entries are reachable inside the DT extent");
    expect (c81.agree == 1, "boot 81: LDO_LD_DATA_REG expects 0 and reads 0, so one entry DOES match -- "
            "round294's 'not one bit matches' was wrong");
    expect (c81.differ == 7, "boot 81: seven comparable words differ from the vendor's word");
    expect (c81.nonzero_expected == 7 && c81.nonzero_read_zero == 7,
            "boot 81: all seven non-zero expectations read zero -- the list never ran");
    expect (c81.no_expectation == 5, "boot 81: five words in the window have no table entry at all");
    expect (c81.table_outside == 1, "boot 81: the table's ninth entry (ADC_CTL 0x34) is outside the DT "
            "extent, so it is not a word this line can ever compare");
    expect (c81.zero == 11, "boot 81: eleven of thirteen words are zero, not thirteen -- the other two "
            "are PWRSWITCH_CTL and LDO_STATUS1");
    expect (gpu_ldo_expect_inside_window () == 8 && kGpuLdoExpectCount == 9,
            "the nine-entry table has eight entries inside the window");
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (c81, true)), "list-never-written") == 0,
            "boot 81 with the same-beat cx_on control is the 'this list has never run' shape");

    // -----------------------------------------------------------------------
    // 2. The control is load-bearing: the identical words, without it.
    // -----------------------------------------------------------------------
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (c81, false)), "list-unwritten-blind") == 0,
            "without cx_on the same census may only say the list is unwritten, not who failed to write "
            "it -- a page that is not there reads zero too");

    // -----------------------------------------------------------------------
    // 3. A page that does not answer is not a disagreement.
    // -----------------------------------------------------------------------
    uint32_t dead[kGpuLdoWordCount];
    for (uint32_t i = 0; i < kGpuLdoWordCount; i++) {
        dead[i] = kGpuNoAnswer;
    }
    GpuLdoCensus cdead;
    gpu_ldo_census (kOffsets, dead, kGpuLdoWordCount, &cdead);
    expect (cdead.dead == 13 && cdead.comparable == 0 && cdead.differ == 0 && cdead.agree == 0,
            "a census of all-ones must produce no comparisons at all; counting them as differences "
            "would have made a dead window look like a mis-programmed one, which is the mistake r291's "
            "row was built to avoid");
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (cdead, true)), "dead-page") == 0,
            "and the shape has to say which of the two it was");

    // -----------------------------------------------------------------------
    // 4. The mapping guard: one word read from an offset the vendor does not name.
    // -----------------------------------------------------------------------
    uint32_t shifted[kGpuLdoWordCount];
    uint32_t shifted_values[kGpuLdoWordCount];
    memcpy (shifted, kOffsets, sizeof (shifted));
    memcpy (shifted_values, kBoot81, sizeof (shifted_values));
    shifted[9] = 0x34U;               // one word walked past the census, onto ADC_CTL's offset
    GpuLdoCensus cshift;
    gpu_ldo_census (shifted, shifted_values, kGpuLdoWordCount, &cshift);
    expect (cshift.named == 12 && cshift.words == 13,
            "a word read from an offset the name table does not cover drops named below words");
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (cshift, true)), "unnamed-mapping") == 0,
            "and the verdict refuses to describe a block it is no longer reading -- this is the arm "
            "that catches a kGpuccLdo* constant drifting from its documented offset");
    // The guard is about *unnamed* drift, and a duplicated valid offset is invisible to it. Stated here
    // because it is a real limit of the check, not a bug in it: what catches a swap between two named
    // words is that their expectations differ, so the census moves agree/differ rather than the shape.
    shifted[9] = 0x28U;
    GpuLdoCensus cswap;
    gpu_ldo_census (shifted, shifted_values, kGpuLdoWordCount, &cswap);
    expect (cswap.named == 13 && cswap.agree == 1 && cswap.comparable == 8,
            "a named offset replacing a named offset keeps named == 13: the mapping guard sees "
            "out-of-table drift, and only the counts move when two tabled words trade places");

    // -----------------------------------------------------------------------
    // 5. PWRSWITCH_CTL: named bits, and the residue that has no name.
    // -----------------------------------------------------------------------
    GpuLdoCtrl ctrl;
    gpu_ldo_decode_ctrl (kBoot81[7], &ctrl);
    expect (ctrl.answered && ctrl.clamp_io && ctrl.pdn && ctrl.under_sw_ctrl && !ctrl.bypass,
            "boot 81's 0x80000038 decodes as CLAMP_IO | PDN | UNDER_SW_CTRL with bypass clear, the "
            "same three names round291's row printed and the same pdn=1 boot 78's did");
    expect (ctrl.unnamed == 0x00000008U, "and one set bit -- BIT(3) -- that no #define in "
            "msm_gfx_ldo.c:39-53 names; a decoder that dropped it would call this word explained");

    GpuLdoCtrl ctrl_dead;
    gpu_ldo_decode_ctrl (kGpuNoAnswer, &ctrl_dead);
    expect (!ctrl_dead.answered && !ctrl_dead.pdn,
            "0xffffffff has PDN set like any other bit, so a dead page must not be reported as a "
            "powered-down rail -- those are different findings about different hardware");

    // The vendor's own first write, and what a non-answer would have turned it into.
    uint32_t planned = 0;
    expect (gpu_ldo_ctrl_probe_word (kBoot81[7], &planned) && planned == 0x20000038U,
            "clearing CLAMP_IO (BIT(31)) and setting EN_LDOAP_CTRL_CPR (BIT(29)) on boot 81's word gives "
            "0x20000038, which is the write a later round inherits from msm_gfx_ldo.c:1128-1133");
    uint32_t planned_dead = 0xdeadbeefU;
    expect (!gpu_ldo_ctrl_probe_word (kGpuNoAnswer, &planned_dead) && planned_dead == 0xdeadbeefU,
            "and the same arithmetic refuses a page that did not answer instead of dressing "
            "0x7fffffff -- bypass, PDN and both boot-head bits still high -- up as a programmed register");

    // -----------------------------------------------------------------------
    // 6. The corner a reset value describes.
    // -----------------------------------------------------------------------
    GpuLdoVref top;
    gpu_ldo_decode_vref (0x0000007fU, &top);
    expect (top.val == 127U && top.uv == 1010000U,
            "the seven-bit mask allows val 127, which the vendor's own decode turns into 1,010,000 uV");
    expect (!top.in_driver_range && !top.in_dt_range,
            "and that is above MAX_LDO_VOLTAGE (980,000) and above this unit's regulator-max-microvolt "
            "(925,000): a row printing only val= and uv= can name a voltage no regulator here accepts");

    GpuLdoVref reset_value;
    gpu_ldo_decode_vref (0x00000000U, &reset_value);
    expect (reset_value.uv == kLdoMinUv && reset_value.in_driver_range && !reset_value.in_dt_range,
            "boot 81's vref=0 decodes to 375,000 uV -- the driver's floor, but below the 400,000 uV this "
            "board's own device tree declares (clover.dtb ldo@0506e000, regulator-min = 0x61a80), which "
            "is a second, independent way of seeing that val 0 is a reset value and not a corner");

    // -----------------------------------------------------------------------
    // 7. The settling predicate has two inputs, not one.
    // -----------------------------------------------------------------------
    expect (!gpu_ldo_vendor_settled (0x00000000U) && gpu_ldo_vendor_settled (0x00000004U) &&
            !gpu_ldo_status_settled (0x00000004U),
            "msm_gfx_ldo.c:307 waits on READY *or* SETTLED, so a word with only LDO_READY_BIT passes the "
            "vendor's own test even though the pr_err() that follows names LDO_VREF_SETTLED -- the row "
            "has to print the predicate and the two bits, or one bit answers a two-input question");
    expect (!gpu_ldo_status_ready (kGpuNoAnswer), "and neither bit may be claimed from a dead page");

    // -----------------------------------------------------------------------
    // 8. GDSC: the write sequence, and what PWR_ON alone is worth.
    // -----------------------------------------------------------------------
    // A GDSCR as firmware leaves it: collapsed, hw-controlled, nothing in the wait fields.
    const uint32_t as_found = kGdscSwCollapse | kGdscHwControl;
    const uint32_t after_init = gpu_gdsc_init_word (as_found);
    const uint32_t after_release = gpu_gdsc_release_word (after_init);
    expect (after_init == (kGdscWaitVal | kGdscSwCollapse),
            "gdsc_init's write clears HW_CONTROL and sets the three wait timers, and leaves SW_COLLAPSE "
            "alone -- that is the state between the vendor's two writes");
    expect (after_release == kGdscWaitVal,
            "gdsc_toggle_logic(true) then clears SW_COLLAPSE and touches nothing else");
    expect (!gpu_gdsc_pwr_on (after_release) && gpu_gdsc_collapsed (after_init),
            "so the word this sequence writes never sets PWR_ON itself: the bit has to arrive from the "
            "hardware, which is the whole reason the poll exists");

    expect (gpu_gdsc_pwr_on (kGpuNoAnswer) && !gpu_gdsc_powered (kGpuNoAnswer),
            "a page that reads all ones has PWR_ON set -- and SW_COLLAPSE set too, so the pair is what "
            "refuses to call it powered. Round286's poll reads bit31 alone, which is why the gate in "
            "front of it is a word test and not a bit test");
    expect (gpu_gdsc_pwr_on (0x80000001U) && !gpu_gdsc_powered (0x80000001U),
            "the same trap in a live word: boot 77's gx is PWR_ON with collapse still set, which is why "
            "pwr_on and powered are two functions");

    // The layer's arithmetic against the board's own read-back. Both words below are from boot 81's
    // `clover r286: gpu t=859 gpucc post` row, in the same log as the census above -- this is the part of
    // the model that says the arithmetic describes *this* board rather than a plausible one: round286's
    // write sequence ran, and the three wait fields the layer computes are exactly what the card reported
    // back, in both GDSCs, with SW_COLLAPSE clear in both.
    const uint32_t boot81_cx = 0xa0282000U;
    const uint32_t boot81_cxhw = 0x80001fe0U;
    const uint32_t boot81_gx = 0x08282000U;
    expect ((boot81_cx & kGdscWaitMask) == kGdscWaitVal &&
            (boot81_gx & kGdscWaitMask) == kGdscWaitVal,
            "boot 81 reports 0x00282000 in both GDSCRs' wait fields -- the value gpu_gdsc_init_word() "
            "writes -- so the sequence this layer models is the sequence the board executed");
    expect (!gpu_gdsc_collapsed (boot81_cx) && !gpu_gdsc_collapsed (boot81_gx),
            "and SW_COLLAPSE is clear in both, which is gpu_gdsc_release_word()'s half of it");
    expect (gpu_gdsc_pwr_on (boot81_cxhw) && gpu_gdsc_powered (boot81_cx) &&
            !gpu_gdsc_pwr_on (boot81_gx) && !gpu_gdsc_powered (boot81_gx),
            "CX is up (its status is the hw-ctrl word the driver polls, and its GDSCR agrees this boot) "
            "and GX is not: the same two words, two verdicts, which is the reading boots 77 and 81 left");
    // The residue discipline again: bits no archived file names, stated rather than dropped.
    const uint32_t named_gdsc = kGdscPwrOn | kGdscHwControl | kGdscSwOverride | kGdscSwCollapse |
        kGdscWaitMask;
    expect ((boot81_cx & ~named_gdsc) == 0x20000000U && (boot81_gx & ~named_gdsc) == 0x08000000U,
            "each word carries one further set bit -- CX bit29, GX bit27 -- that nothing in the archived "
            "GDSC sources names, and a census that reported only the named fields would have called both "
            "words fully explained");

    // -----------------------------------------------------------------------
    // 9. The AHB gate, whose second half the last case just justified.
    // -----------------------------------------------------------------------
    expect (gpu_ahb_gate_open (0x00000001U), "a voted, answering AHB branch opens the gate");
    expect (!gpu_ahb_gate_open (0x00000000U), "an unvoted one does not");
    expect (!gpu_ahb_gate_open (kGpuNoAnswer),
            "and 0xffffffff does not, even though bit0 is set: this is why round286's clause tests the "
            "word as well as the bit -- a bit test would have walked into GPUCC behind a dead page");

    // -----------------------------------------------------------------------
    // 10. A branch: asked for is not running.
    // -----------------------------------------------------------------------
    expect (gpu_branch_requested (0x80000001U) && gpu_branch_halted (0x80000001U) &&
            !gpu_branch_running (0x80000001U),
            "boot 74's gfx3d word: the request at bit0 and the halt at bit31 are both set, so r286's "
            "gfx3d_en=1 said somebody asked and nothing more -- r290's halt bit is what closes that");
    expect (gpu_branch_running (0x00000001U) && !gpu_branch_running (0x80000000U),
            "and the conjunction is true only for a voted, un-halted branch");

    // -----------------------------------------------------------------------
    // 11. The chipid refusal has to name one gate, deterministically.
    // -----------------------------------------------------------------------
    expect (gpu_chipid_gate (true, false, true) == GPU_CHIPID_NO_GX &&
            strcmp (gpu_chipid_gate_name (gpu_chipid_gate (true, false, true)), "no-gx") == 0,
            "boot 81's shape -- cx up, gx not -- names no-gx, and that is the row's only claim about gx");
    expect (gpu_chipid_gate (false, false, false) == GPU_CHIPID_NO_CX,
            "with every gate shut the row names CX, because the AHB vote and the CX poll are what every "
            "later write rides on; a row that named the last gate would hide the first");
    expect (gpu_chipid_gate (true, true, false) == GPU_CHIPID_NO_GFX3D &&
            gpu_chipid_gate (true, true, true) == GPU_CHIPID_READS,
            "and a chipid read behind all three gates is the only case where a wrong chipid is a fact "
            "about the core rather than about the bus");

    // -----------------------------------------------------------------------
    // 12. MEM_ACC: the order of two tests, demonstrated by getting it wrong.
    // -----------------------------------------------------------------------
    GpuMemAcc mem_dead;
    gpu_mem_acc_decode (kGpuNoAnswer, &mem_dead);
    expect (!mem_dead.answered && mem_dead.corner == 0U && mem_dead.field == 0U,
            "a refused page reports no corner at all");
    expect (gpu_mem_acc_decode_before_mask (kGpuNoAnswer) == kMemAccMinCorner,
            "and masking first would have called it corner 1, because the field is one bit wide and "
            "0xffffffff's masked field is 1, which is also corner 1's map value -- the two answers this "
            "order is what GpuRegs.h's rule keeps apart");
    GpuMemAcc mem_live;
    gpu_mem_acc_decode (0x00000000U, &mem_live);
    expect (mem_live.answered && mem_live.corner == kMemAccMaxCorner && mem_live.corner_named,
            "a live zero is corner 2, and here the two orders agree -- which is the control that says "
            "the difference above is specifically about 0xffffffff, not about the mask");

    // -----------------------------------------------------------------------
    // 13. An empty census cannot become a verdict.
    // -----------------------------------------------------------------------
    GpuLdoCensus none;
    gpu_ldo_census (kOffsets, kBoot81, 0, &none);
    expect (strcmp (gpu_ldo_shape_name (gpu_ldo_shape (none, true)), "no-words") == 0,
            "zero words read has its own shape: a report of 'as expected' produced from nothing is how "
            "a capped row would accuse a page that was never read");

    // -----------------------------------------------------------------------
    // 18. The end-of-pipeline timestamp packet.
    // -----------------------------------------------------------------------
    {
        GpuTsPacket ts = gpu_eop_timestamp (0x1122334455667788ULL, 0x2aU, true);
        expect (ts.count == 5U, "the timestamp packet is five dwords");
        expect ((ts.words[0] & 0xF0000000U) == 0x70000000U && ((ts.words[0] >> 16) & 0x7FU) == 0x46U,
                "type7 header with CP_EVENT_WRITE");
        expect ((ts.words[0] & 0x3FFFU) == 4U, "the header's count is the four payload words");
        expect ((ts.words[1] & 0x7FFFFFFFU) == 4U,
                "the event word is CACHE_FLUSH_TS, with the dirty bit clear in the low bits");
        expect (ts.words[1] == (4U | (1U << 31)) && ts.words[1] != 4U,
                "the dirty bit is bit 31 and the test turns it on rather than assuming it");
        expect (ts.words[2] == 0x55667788U && ts.words[3] == 0x11223344U,
                "the 64-bit address is split low then high");
        expect (ts.words[4] == 0x2aU, "the timestamp is the last payload word");
        GpuTsPacket clean = gpu_eop_timestamp (0, 1, false);
        expect (clean.words[1] == 4U, "without the dirty bit the event word is just CACHE_FLUSH_TS");
    }

    // -----------------------------------------------------------------------
    // 17. Ring allocation: the vendor's tail/head/full rules.
    // -----------------------------------------------------------------------
    {
        GpuRingAllocResult r = gpu_ring_alloc (0U, 0U, 9U);
        expect (r.kind == GPU_RING_ALLOC_OK && r.offset == 0U && r.new_wptr == 9U,
                "nine dwords at an empty ring start at zero and leave wptr at nine");
        r = gpu_ring_alloc (kGpuRingDwords - 4U, 100U, 9U);
        expect (r.kind == GPU_RING_ALLOC_WRAP && r.offset == 0U && r.new_wptr == 9U &&
                r.nop_words == 3U,
                "a tail that cannot hold nine dwords gets a three-word NOP filler and a wrap to the "
                "head, which only happens when the head is behind the write pointer");
        r = gpu_ring_alloc (100U, 0U, 9U);
        expect (r.kind == GPU_RING_ALLOC_OK && r.offset == 100U,
                "a tail that can hold them allocates in place");
        r = gpu_ring_alloc (0U, 10U, 9U);
        expect (r.kind == GPU_RING_ALLOC_OK && r.offset == 0U && r.new_wptr == 9U,
                "with the reader ahead of the writer, nine dwords fit at the head");
        r = gpu_ring_alloc (kGpuRingDwords - 4U, 5U, 9U);
        expect (r.kind == GPU_RING_ALLOC_FULL,
                "the tail cannot hold it and the head is inside the request: the allocator refuses "
                "rather than overwrite the reader");
        r = gpu_ring_alloc (0U, 0U, 0U);
        expect (r.kind == GPU_RING_ALLOC_FULL, "zero dwords is not an allocation");
        r = gpu_ring_alloc (0U, 0U, kGpuRingDwords);
        expect (r.kind == GPU_RING_ALLOC_FULL, "a ring-sized request cannot be placed at all");
        expect (gpu_ring_submit_done (9U, 9U) && !gpu_ring_submit_done (9U, 8U),
                "a submission is done only when the read pointer equals the write pointer");
        expect (gpu_ring_submit_done (0U, kGpuRingDwords - 1U) == false,
                "a wrapped pair is not equal and so is not done");
    }

    // -----------------------------------------------------------------------
    // 16. ME_INIT: the nine startup dwords.
    // -----------------------------------------------------------------------
    {
        uint32_t packets[9] = {0};
        gpu_me_init_packets (0U, packets);      /* a512: the default workaround word */
        expect ((packets[0] & 0xF0000000U) == 0x70000000U, "ME_INIT starts with a type7 header");
        expect (((packets[0] >> 16) & 0x7FU) == 0x48U, "the opcode is CP_ME_INIT's 0x48");
        expect ((packets[0] & 0x3FFFU) == 8U, "the header's count is the eight payload words");
        expect (packets[1] == 0x2FU,
                "the mask is 0x2F: max-context, error detection, header dump, default reset state and "
                "ucode workarounds");
        expect (packets[2] == 0x00000003U, "MAX_CONTEXT writes three");
        expect (packets[3] == 0x20000000U, "ERROR_DETECTION_CONTROL writes the protected-mode word");
        expect (packets[4] == 0U && packets[5] == 0U, "HEADER_DUMP writes address and enable/size as zero");
        expect (packets[6] == 0U, "a512's ucode workaround word is the default zero");
        expect (packets[7] == 0U && packets[8] == 0U, "the rest of the eight words is padded with zeros");
        uint32_t a530[9] = {0};
        gpu_me_init_packets (0x0BU, a530);
        expect (a530[6] == 0x0BU && a530[8] == 0U,
                "the A530-shaped workaround lands in the same word, not shifted by its value");
    }

    // -----------------------------------------------------------------------
    // 15. The CP ring: geometry and the control word.
    // -----------------------------------------------------------------------
    {
        expect (kGpuRingBytes == 32768U && kGpuRingDwords == 8192U,
                "KGSL_RB_SIZE is 32 KiB and KGSL_RB_DWORDS is 8192");
        expect (gpu_rb_cntl_default () == 0x20CU,
                "A5XX_CP_RB_CNTL_DEFAULT with this ring is 0x20C: ilog2(4)=2 in bits 8-12 and "
                "ilog2(4096)=12 in bits 0-5");
        expect (gpu_ring_space (0U, 0U) == kGpuRingDwords - 1U,
                "an empty ring leaves one dword of room, so a full ring is never seen as empty");
        expect (gpu_ring_space (4U, 0U) == kGpuRingDwords - 5U, "four written dwords cost four of room");
        expect (gpu_ring_space (0U, 1U) == 0U,
                "a wrapped pointer pair is a difference, not a subtraction: (0-1)&8191 is 8191 used, so "
                "no room is left");
        expect (gpu_ring_advance (kGpuRingDwords - 2U, 4U) == 2U, "advance wraps inside the ring");
        expect (gpu_ring_advance (0U, 1U) == 1U, "one dword advances the write pointer by one");
    }

    // -----------------------------------------------------------------------
    // 14. The flashables: firmware blob shape and PM4 headers.
    // -----------------------------------------------------------------------
    {
        const uint8_t blob[12] = {0x11,0x22,0x33,0x44, 0xef,0xbe,0xad,0xde, 0x01,0x00,0x00,0x00};
        GpuFwBlob fw;
        expect (gpu_fw_parse (blob, sizeof (blob), &fw),
                "a 12-byte blob with four skipped bytes parses");
        expect (fw.version == 0xdeadbeefU && fw.bytes == 8 && fw.words == 2,
                "the version is the first word after the skipped date word, like _load_firmware()'s "
                "*(unsigned int *)&fw->data[4]");
        expect (!gpu_fw_parse (blob, 4, &fw), "four bytes is only the skipped word: not a blob");
        expect (!gpu_fw_parse (blob, 6, &fw), "a size that is not whole words is not a blob");
        expect (!gpu_fw_parse (nullptr, 12, &fw), "no data is not a blob");
        expect (gpu_pm4_parity (0U) == 1U, "odd parity of zero is one, the vendor table's own value");
        expect (gpu_pm4_type4 (0U, 0U) == 0x48000080U,
                "type4 with opcode 0 and count 0 is 4<<28 plus both parity bits");
        expect (gpu_pm4_type7 (0U, 0U) == 0x70808000U,
                "type7 with opcode 0 and count 0 is 7<<28 plus both parity bits");
        expect (kGpuType4MaxPayload == 127U,
                "TYPE4_MAX_PAYLOAD is PM4_TYPE4_PKT_SIZE_MAX (128) minus the header word");
        expect (gpu_fw_packet_count (0U) == 0U && gpu_fw_packet_count (127U) == 1U &&
                gpu_fw_packet_count (128U) == 2U,
                "a payload split at the type4 limit: 128 words take two packets");
    }

    if (checks != 0) {
        printf ("FAIL: %d GPU core assertions did not hold\n", checks);
        return 1;
    }
    printf ("PASS: GPU core decoders -- boot 81's LDO census is 8 comparable / 1 agree / 7 differ, the "
        "7 non-zero expectations all read zero and the shape is list-never-written (with the control) or "
        "list-unwritten-blind (without); all-ones yields no comparisons at all; an offset walked onto "
        "ADC_CTL drops to unnamed-mapping while a swap of two named words moves only the counts; "
        "PWRSWITCH_CTL 0x80000038 leaves BIT(3) unnamed; the reset corner 375000 uV sits under this "
        "unit's declared 400000 uV floor and val 0x7f's 1010000 uV sits over both maxima; the vendor's "
        "settling test passes on READY alone; gdsc_init then release produces 0x%08x, which is the wait "
        "field boot 81 read back in BOTH GDSCRs, and each word keeps one unnamed bit (CX bit29, GX "
        "bit27) that nothing archived names; the AHB gate refuses 0xffffffff that bit31-based tests "
        "would pass; branch request and halt are separate; the chipid gate names no-gx for boot 81 and "
        "no-cx when all three are shut; a masked-first MEM_ACC decode turns a dead page into corner 1\n",
        (unsigned) after_release);
    return 0;
}
