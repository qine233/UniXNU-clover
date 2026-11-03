// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the GPU line's second step -- power the two GDSCs and the gfx3d branch
// clock, read them back, and read chipid once the core can answer.
//
// WHY THIS ROUND WRITES
//
// Round282 read the two windows and wrote nothing, because a first driver must not touch a power
// state it cannot undo. The read it was built for is the running boot's job; this round is the
// write that read was waiting to license. It is still not an Adreno driver: it powers and clocks
// the core and then identifies it, which is the notes' step 2 (GDSC cx -> gx, AHB/CFG, GFX3D root,
// write then read back GDSCR), and it stops there. Nothing here touches a GPU register other than
// chipid, and it reads chipid only after both power polls and the branch vote read on.
//
// THE SEQUENCE
//
//   baseline GCC row (the r282 row, with the eMMC votes as its positive control)
//   write gcc_gpu_cfg_ahb / gcc_gpu_bimc_gfx / gcc_bimc_gfx, read them back
//   if the AHB vote still reads 0 or 0xffffffff, refuse GPUCC (the r282 gate, now after the write)
//   GDSC CX on, then GDSC GX on -- each mirrors gdsc_init() then gdsc_toggle_logic(true) from the
//     vendor gdsc.c: clear the hw-trigger and sw-override bits, set the wait timers, clear
//     SW_COLLAPSE, poll PWR_ON on the status register (hw-ctrl for CX, the GDSCR itself for GX)
//   set the gfx3d branch enable (0x1098); the RCG command word 0x1070 is deliberately NOT
//     reprogrammed this round and stays reported raw -- a branch enable and a rate a round did not
//     set are two different answers
//   round290 adds the two votes the DT's clock list still owed (rbcpr 0x1054, rbbmtimer 0x10d0) and
//     reads every GPUCC branch's halt bit (BIT(31), CBCR_CLK_OFF) next to its enable request,
//     because "gfx3d_en=1" on boot 74 only ever said somebody asked
//   round291 reads the GFX LDO that the gx corner is set by -- vref/pwrswitch-ctrl/pwrswitch-status
//     at 0x9018/0x901c/0x9028, the vendor's own decode and its own settling bits -- beside the
//     same-beat gx_on, because boot 77 left gx collapse-released with PWR_ON never arriving
//   read back the GDSCRs (the acceptance), then chipid at 0x05010200 gated on cx-on && gx-on &&
//     gfx3d-enable, checked against a512's 0x05010200
//
// WHY THE VERDICTS REPEAT
//
// The gdsc row prints once, but every field it carries is repeated by the steady-state gpucc row on
// every later timer fire, and the chipid row prints on every fire too. A line a long boot rotates
// out of the 512-record ring is the mistake r283 fixed; this round repeats the verdicts instead of
// printing them once.
//
// Opt-in through `clover-gpu=1`; absent, this touches no register and creates no timer.

#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
// IOMemoryMap is declared in IOMemoryDescriptor.h in this SDK; there is no IOMemoryMap.h.
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include "GpuRegs.h"

#define CLOVER_GPU_FIRST_MS    240000u
#define CLOVER_GPU_STATE_MS    120000u
// Bounded stand-in for gdsc.c's 100 x udelay(1): a driver at this point in the boot has no timer to
// sleep on, so the poll is plain Device-memory reads -- the same class of access as the status read
// itself -- bounded the way CloverPower bounds its PMIC-arbiter wait. The write phase runs once, so
// a stuck bit costs at most this many reads, and a working GDSC answers in the first few.
#define CLOVER_GPU_GDSC_SPIN   100000u

static IOMemoryMap *clover_gpu_gcc_map;
static IOMemoryMap *clover_gpu_gpucc_map;
static IOMemoryMap *clover_gpu_core_map;
static volatile uint32_t *clover_gpu_gcc;
static volatile uint32_t *clover_gpu_gpucc;
static volatile uint32_t *clover_gpu_core;
static IOTimerEventSource *clover_gpu_timer;
static bool clover_gpu_powered;
static uint32_t clover_gpu_fires;

static unsigned
clover_gpu_uptime_seconds(void)
{
    uint64_t uptime = 0;
    uint64_t nsecs = 0;

    clock_get_uptime(&uptime);
    absolutetime_to_nanoseconds(uptime, &nsecs);
    return (unsigned) (nsecs / 1000000000ull);
}

// An ordinary read plus one barrier: the loads are ordered before the row that reports them. Same
// shape as drivers/CloverInput's windowRead and round281's watchdog read.
static uint32_t
clover_gpu_read(volatile uint32_t *window, uint32_t offset)
{
    uint32_t value = window[offset / 4];

    __asm__ volatile ("dsb sy" ::: "memory");
    return value;
}

// The write side, introduced by this round. All stores go through these four helpers, so the
// windows are never indexed directly from a row the way a bare subscript on the window pointer
// would be -- the same discipline the read side already keeps. The barrier orders the store
// before the read-back that proves it.
static void
clover_gpu_write(volatile uint32_t *window, uint32_t offset, uint32_t value)
{
    window[offset / 4] = value;
    __asm__ volatile ("dsb sy" ::: "memory");
}

static void
clover_gpu_set_bits(volatile uint32_t *window, uint32_t offset, uint32_t bits)
{
    clover_gpu_write(window, offset, clover_gpu_read(window, offset) | bits);
}

static void
clover_gpu_clear_bits(volatile uint32_t *window, uint32_t offset, uint32_t bits)
{
    clover_gpu_write(window, offset, clover_gpu_read(window, offset) & ~bits);
}

static void
clover_gpu_update_bits(volatile uint32_t *window, uint32_t offset, uint32_t mask, uint32_t value)
{
    clover_gpu_write(window, offset, (clover_gpu_read(window, offset) & ~mask) | (value & mask));
}

// gdsc_toggle_logic's poll, with the timer replaced by a bounded spin (see CLOVER_GPU_GDSC_SPIN).
static bool
clover_gpu_poll_pwr_on(volatile uint32_t *window, uint32_t status_offset)
{
    for (uint32_t i = 0; i < CLOVER_GPU_GDSC_SPIN; i++) {
        if ((clover_gpu_read(window, status_offset) & kGdscPwrOn) != 0) {
            return true;
        }
    }
    return (clover_gpu_read(window, status_offset) & kGdscPwrOn) != 0;
}

// gdsc_init() then gdsc_toggle_logic(true), for one GDSC. The hw controller updates its status bit
// about eight xo cycles after the write (gdsc.c's comment), so the CX path passes has_hw_ctrl and
// its poll reads the hw-ctrl syscon; GX has no hw-ctrl and its poll reads the GDSCR itself.
static bool
clover_gpu_gdsc_on(volatile uint32_t *gpucc, uint32_t gdscr_offset, uint32_t status_offset,
                   bool has_hw_ctrl)
{
    (void) has_hw_ctrl;
    // gdsc_init: clear HW_CONTROL | SW_OVERRIDE, set the three wait fields.
    clover_gpu_update_bits(gpucc, gdscr_offset,
        kGdscHwControl | kGdscSwOverride | kGdscWaitMask, kGdscWaitVal);
    // gdsc_toggle_logic(true): clear SW_COLLAPSE.
    clover_gpu_clear_bits(gpucc, gdscr_offset, kGdscSwCollapse);
    return clover_gpu_poll_pwr_on(gpucc, status_offset);
}

// Lazy, and remembered: GPUCC is only mapped once the GCC row says its AHB vote is on. A map that
// fails is retried on the next fire because that failure can be the platform map settling, and the
// cost of a retry is one map attempt every two minutes.
static volatile uint32_t *
clover_gpu_open_gpucc(void)
{
    IODeviceMemory *memory;

    if (clover_gpu_gpucc != NULL) {
        return clover_gpu_gpucc;
    }
    memory = IODeviceMemory::withRange(kGpuccPhys, kGpuccSize);
    if (memory == NULL) {
        return NULL;
    }
    clover_gpu_gpucc_map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (clover_gpu_gpucc_map != NULL) {
        clover_gpu_gpucc = (volatile uint32_t *) clover_gpu_gpucc_map->getVirtualAddress();
    }
    return clover_gpu_gpucc;
}

// The GPU core window, mapped only after both power polls and the branch vote read on. Until then a
// touch of 0x05000000 is the unclocked-bus gamble round282 declined; the gate lives in the caller.
static volatile uint32_t *
clover_gpu_open_core(void)
{
    IODeviceMemory *memory;

    if (clover_gpu_core != NULL) {
        return clover_gpu_core;
    }
    memory = IODeviceMemory::withRange(kGpuPhys, kGpuSize);
    if (memory == NULL) {
        return NULL;
    }
    clover_gpu_core_map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (clover_gpu_core_map != NULL) {
        clover_gpu_core = (volatile uint32_t *) clover_gpu_core_map->getVirtualAddress();
    }
    return clover_gpu_core;
}

// The GCC row, every fire. The two eMMC votes are the control: this board's eMMC host is used
// before and after this row, so they have to read 1 on a healthy boot.
static void
clover_gpu_report_gcc(unsigned t)
{
    uint32_t ahb      = clover_gpu_read(clover_gpu_gcc, kGccGpuCfgAhb);
    uint32_t gpu_bimc = clover_gpu_read(clover_gpu_gcc, kGccGpuBimcGfx);
    uint32_t bimc     = clover_gpu_read(clover_gpu_gcc, kGccBimcGfx);
    uint32_t ctl_ahb  = clover_gpu_read(clover_gpu_gcc, kGccSdcc1Ahb);
    uint32_t ctl_apps = clover_gpu_read(clover_gpu_gcc, kGccSdcc1Apps);

    printf("clover r286: gpu t=%u fire=%u gcc gpu_ahb=%u gpu_bimc=%u bimc=%u ctl_sdhc1_ahb=%u "
        "ctl_sdhc1_apps=%u raw ahb=%08x ctl=%08x\n", t, (unsigned) clover_gpu_fires,
        (unsigned) (ahb & 1u), (unsigned) (gpu_bimc & 1u), (unsigned) (bimc & 1u),
        (unsigned) (ctl_ahb & 1u), (unsigned) (ctl_apps & 1u),
        (unsigned) ahb, (unsigned) ctl_ahb);
}

// The GPUCC row. `post` picks the label: "pre" is what firmware left, "post" is the read-back this
// round's write has to produce. CX's status is the hw-ctrl bit, not its GDSCR: msm-gdsc-660.dtsi
// gives that node an hw-ctrl-addr, and gdsc-regulator.c polls the syscon instead of the GDSCR.
static void
clover_gpu_report_gpucc(unsigned t, bool post)
{
    uint32_t cx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscCx);
    uint32_t cxhw = clover_gpu_read(clover_gpu_gpucc, kGpuccCxHwCtrl);
    uint32_t gx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscGx);
    uint32_t gfx  = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dEn);
    uint32_t cxo  = clover_gpu_read(clover_gpu_gpucc, kGpuccCxoEn);
    uint32_t rcg  = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dRcg);
    uint32_t dom  = clover_gpu_read(clover_gpu_gpucc, kGpuccGxDomain);
    uint32_t sw   = clover_gpu_read(clover_gpu_gpucc, kGpuccGxSwReset);

    if (post) {
        printf("clover r286: gpu t=%u gpucc post cx=%08x cxhw=%08x cx_on=%u cx_collapse=%u "
            "gx=%08x gx_on=%u gx_collapse=%u gfx3d_en=%u cxo_en=%u rcg=%08x dom=%08x sw=%08x\n",
            t, (unsigned) cx, (unsigned) cxhw, (unsigned) ((cxhw & kGdscPwrOn) != 0),
            (unsigned) ((cx & kGdscSwCollapse) != 0), (unsigned) gx,
            (unsigned) ((gx & kGdscPwrOn) != 0), (unsigned) ((gx & kGdscSwCollapse) != 0),
            (unsigned) (gfx & 1u), (unsigned) (cxo & 1u), (unsigned) rcg, (unsigned) dom,
            (unsigned) sw);
    } else {
        printf("clover r286: gpu t=%u gpucc pre cx=%08x cxhw=%08x cx_on=%u cx_collapse=%u "
            "gx=%08x gx_on=%u gx_collapse=%u gfx3d_en=%u cxo_en=%u rcg=%08x dom=%08x sw=%08x\n",
            t, (unsigned) cx, (unsigned) cxhw, (unsigned) ((cxhw & kGdscPwrOn) != 0),
            (unsigned) ((cx & kGdscSwCollapse) != 0), (unsigned) gx,
            (unsigned) ((gx & kGdscPwrOn) != 0), (unsigned) ((gx & kGdscSwCollapse) != 0),
            (unsigned) (gfx & 1u), (unsigned) (cxo & 1u), (unsigned) rcg, (unsigned) dom,
            (unsigned) sw);
    }
}

// Round290's row. Every GPUCC branch this board's own clock list asks for, read in the two fields
// that carry meaning: the enable request at BIT(0) and the halt state at BIT(31) (CBCR_CLK_OFF,
// with BRANCH_HALT's "pol: 1 = halt"). r286 printed the request alone, so boot 74's `gfx3d_en=1`
// said "somebody asked" and never said "it runs" -- and a GDSC that will not power up is exactly
// the shape a halted parent produces. Nothing here decodes the RCG command words; they are printed
// raw next to the halt bits so a later round can tell a requested source from an active one.
static void
clover_gpu_report_branches(unsigned t, const char *stage)
{
    uint32_t gfx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dEn);
    uint32_t cxo   = clover_gpu_read(clover_gpu_gpucc, kGpuccCxoEn);
    uint32_t rbcpr = clover_gpu_read(clover_gpu_gpucc, kGpuccRbcprEn);
    uint32_t rbbm  = clover_gpu_read(clover_gpu_gpucc, kGpuccRbbmtimerEn);
    uint32_t rcg3d = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dRcg);
    uint32_t rcgr  = clover_gpu_read(clover_gpu_gpucc, kGpuccRbcprRcg);
    uint32_t rcgb  = clover_gpu_read(clover_gpu_gpucc, kGpuccRbbmtimerRcg);

    printf("clover r290: gpu t=%u branches %s gfx3d=%u/%u cxo=%u/%u rbcpr=%u/%u rbbm=%u/%u "
        "en/halt rcg3d=%08x rcg_rbcpr=%08x rcg_rbbm=%08x\n", t, stage,
        (unsigned) (gfx & 1u), (unsigned) ((gfx & kBranchClkOff) != 0),
        (unsigned) (cxo & 1u), (unsigned) ((cxo & kBranchClkOff) != 0),
        (unsigned) (rbcpr & 1u), (unsigned) ((rbcpr & kBranchClkOff) != 0),
        (unsigned) (rbbm & 1u), (unsigned) ((rbbm & kBranchClkOff) != 0),
        (unsigned) rcg3d, (unsigned) rcgr, (unsigned) rcgb);
}

// Round291's row: the GFX LDO that sets the corner GX powers up against. Boot 77 left the two
// branches downstream of gx halted while cxo and rbcpr run, and the gx GDSC reads collapse-released
// (bit0=0) with PWR_ON never arriving -- a domain whose supply never came, not one nobody asked.
// So this row reads the three registers the vendor driver names for that supply, in the window the
// DT declares (0x0506e000+0x34, which sits inside the GPUCC extent already mapped). It prints both
// the raw words and the driver's own decode (uV = (word & 0x7f) * 5000 + 375000, msm_gfx_ldo.c:1371)
// and the driver's own settling bits (LDO_READY | LDO_VREF_SETTLED, :307), beside the same-beat
// cx_on/gx_on read from the GDSC registers -- those are the control that says this window answers,
// so a row of 0xffffffff here means a bus that did not reply rather than a rail at zero.
static void
clover_gpu_report_ldo(unsigned t, const char *stage)
{
    uint32_t vref   = clover_gpu_read(clover_gpu_gpucc, kGpuccLdoVref);
    uint32_t ctrl   = clover_gpu_read(clover_gpu_gpucc, kGpuccLdoCtrl);
    uint32_t sts    = clover_gpu_read(clover_gpu_gpucc, kGpuccLdoStatus);
    uint32_t val    = vref & kLdoVrefMask;
    uint32_t uv     = val * kLdoStepUv + kLdoMinUv;
    uint32_t cxhw   = clover_gpu_read(clover_gpu_gpucc, kGpuccCxHwCtrl);
    uint32_t gx     = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscGx);

    printf("clover r291: gpu t=%u ldo %s vref=%08x val=%u uv=%u ctrl=%08x byp=%u pdn=%u "
        "cpr_byp=%u sts=%08x ready=%u settled=%u cx_on=%u gx_on=%u\n", t, stage,
        (unsigned) vref, (unsigned) val, (unsigned) uv, (unsigned) ctrl,
        (unsigned) ((ctrl & kLdoBypass) != 0), (unsigned) ((ctrl & kLdoPdn) != 0),
        (unsigned) ((ctrl & kLdoCprBypass) != 0), (unsigned) sts,
        (unsigned) ((sts & kLdoReady) != 0), (unsigned) ((sts & kLdoSettled) != 0),
        (unsigned) ((cxhw & kGdscPwrOn) != 0), (unsigned) ((gx & kGdscPwrOn) != 0));
}

// Round294 widens that reading to the whole LDO block the vendor driver names, because boot 78 answered
// "is the rail programmed" for three words and left the other ten unasked -- and the init list the vendor
// probe writes (msm_gfx_ldo.c:1135-1140 over sdm660_ldo_config[] :147-158) names an expected word for each
// of them. This row is read-only by design: the next round may write, and the words it finds here are the
// baseline it will be judged against. The same-beat cx_on/gx_on ride along for the reason r291 gave: a row
// of 0xffffffff with cx_on still set is a page that did not answer, not a block at zero.
static void
clover_gpu_report_ldo_words(unsigned t, const char *stage)
{
    uint32_t cxhw = clover_gpu_read(clover_gpu_gpucc, kGpuccCxHwCtrl);
    uint32_t gx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscGx);

    printf("clover r294: gpu t=%u ldo words %s w00=%08x w04=%08x w08=%08x w0c=%08x w10=%08x "
        "w14=%08x w18=%08x w1c=%08x w20=%08x w24=%08x w28=%08x w2c=%08x w30=%08x cx_on=%u gx_on=%u\n",
        t, stage,
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoAtest),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoCfg0),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoCfg1),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoCfg2),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoLdData),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoVrefTest),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoVref),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoCtrl),
        // 0x20: no name in msm_gfx_ldo.c, so it is printed as a word and nothing more.
        (unsigned) clover_gpu_read(clover_gpu_gpucc, 0x9020U),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoStatus1),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoStatus),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoRefX1),
        (unsigned) clover_gpu_read(clover_gpu_gpucc, kGpuccLdoRefX2),
        (unsigned) ((cxhw & kGdscPwrOn) != 0), (unsigned) ((gx & kGdscPwrOn) != 0));
}

// The gated chipid read. a512 reports 0x05010200 once the core is powered and the AHB path is
// clocked; the gate is cx-on && gx-on && gfx3d-enable, so a chipid of 0xffffffff or 0 cannot be
// blamed on a bus nobody clocked. The refusal row says which gate did not open.
static void
clover_gpu_report_chipid(unsigned t)
{
    uint32_t cxhw = clover_gpu_read(clover_gpu_gpucc, kGpuccCxHwCtrl);
    uint32_t gx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscGx);
    uint32_t gfx  = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dEn);
    bool cx_on  = (cxhw & kGdscPwrOn) != 0;
    bool gx_on  = (gx & kGdscPwrOn) != 0;
    bool gfx_on = (gfx & 1u) != 0;

    if (!(cx_on && gx_on && gfx_on)) {
        printf("clover r286: gpu t=%u chipid skipped cx_on=%u gx_on=%u gfx3d_en=%u\n",
            t, (unsigned) cx_on, (unsigned) gx_on, (unsigned) gfx_on);
        return;
    }
    if (clover_gpu_open_core() == NULL) {
        printf("clover r286: gpu t=%u core map failed phys=%llx\n", t,
            (unsigned long long) kGpuPhys);
        return;
    }
    {
        uint32_t chipid = clover_gpu_read(clover_gpu_core, kGpuChipid);
        printf("clover r286: gpu t=%u chipid=%08x expect=%08x ok=%u\n", t,
            (unsigned) chipid, (unsigned) kGpuChipidValue,
            (unsigned) (chipid == kGpuChipidValue));
    }
}

// The write phase, run once per boot. It prints its own rows; on any refusal it returns without
// setting clover_gpu_powered, so the next timer fire retries the whole sequence. The writes are
// idempotent, so a retry after a partial failure redoes safe work.
static void
clover_gpu_power_on(unsigned t)
{
    uint32_t ahb, gpu_bimc, bimc;

    clover_gpu_set_bits(clover_gpu_gcc, kGccGpuCfgAhb, 1);
    clover_gpu_set_bits(clover_gpu_gcc, kGccGpuBimcGfx, 1);
    clover_gpu_set_bits(clover_gpu_gcc, kGccBimcGfx, 1);

    ahb      = clover_gpu_read(clover_gpu_gcc, kGccGpuCfgAhb);
    gpu_bimc = clover_gpu_read(clover_gpu_gcc, kGccGpuBimcGfx);
    bimc     = clover_gpu_read(clover_gpu_gcc, kGccBimcGfx);
    printf("clover r286: gpu t=%u gcc post gpu_ahb=%u gpu_bimc=%u bimc=%u raw ahb=%08x "
        "bimc_gfx=%08x bimc=%08x\n", t, (unsigned) (ahb & 1u), (unsigned) (gpu_bimc & 1u),
        (unsigned) (bimc & 1u), (unsigned) ahb, (unsigned) gpu_bimc, (unsigned) bimc);

    // 0xffffffff is not a vote of one: it is a bus that did not answer, and GPUCC is not touched
    // behind it -- the r282 gate, now applied after the write instead of before it.
    if ((ahb & 1u) == 0 || ahb == 0xffffffffu) {
        printf("clover r286: gpu t=%u gpucc skipped ahb=%u raw=%08x (GPUCC access needs the "
            "gcc_gpu_cfg_ahb vote)\n", t, (unsigned) (ahb & 1u), (unsigned) ahb);
        return;
    }

    if (clover_gpu_open_gpucc() == NULL) {
        printf("clover r286: gpu t=%u gpucc map failed phys=%llx\n", t,
            (unsigned long long) kGpuccPhys);
        return;
    }

    clover_gpu_report_gpucc(t, false);

    {
        bool cx_on = clover_gpu_gdsc_on(clover_gpu_gpucc, kGpuccGdscCx, kGpuccCxHwCtrl, true);
        bool gx_on = clover_gpu_gdsc_on(clover_gpu_gpucc, kGpuccGdscGx, kGpuccGdscGx, false);
        uint32_t cx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscCx);
        uint32_t cxhw = clover_gpu_read(clover_gpu_gpucc, kGpuccCxHwCtrl);
        uint32_t gx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscGx);
        uint32_t gfx  = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dEn);

        clover_gpu_set_bits(clover_gpu_gpucc, kGpuccGfx3dEn, 1);
        // Round294 takes these two writes back out. They are round290's -- the rbcpr and rbbmtimer
        // branch enables the DT's clock list still owed -- and the boot record lines up with them
        // exactly: boots 73 and 76, the two longest this port has run (3145 s without a deadline, and a
        // full 1800 s to the deadline's own panic), were both built before r290, while boots 77, 78, 79
        // and 80 -- every boot since it -- each ended in a silent, no-panic, pre-deadline reset at
        // 718 s, 1184 s, 1248 s and 1335 s. Nothing between 77 and 80 touched the GPU again, so these
        // two words are the only new writes the whole run has in common. That is a correlation, not a
        // cause, and a correlation this tight gets tested rather than argued: the reads stay (r290's row
        // still prints both branches, so a boot that stops dying still shows the two votes withheld), and
        // only the BIT(0) presses go. If the silent resets continue anyway, the hypothesis is dead and
        // this comment is the record of it being killed.
        gfx = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dEn);

        printf("clover r286: gpu t=%u gdsc cx_on=%u gx_on=%u gfx3d_en=%u cx=%08x cxhw=%08x "
            "gx=%08x\n", t, (unsigned) cx_on, (unsigned) gx_on, (unsigned) (gfx & 1u),
            (unsigned) cx, (unsigned) cxhw, (unsigned) gx);
    }

    clover_gpu_powered = true;
    clover_gpu_report_branches(t, "post");
    clover_gpu_report_ldo(t, "post");
    clover_gpu_report_ldo_words(t, "post");
    clover_gpu_report_gpucc(t, true);
    clover_gpu_report_chipid(t);
}

static void
clover_gpu_say(OSObject *owner, IOTimerEventSource *sender)
{
    unsigned t = clover_gpu_uptime_seconds();

    (void) owner;
    clover_gpu_fires++;

    clover_gpu_report_gcc(t);

    if (!clover_gpu_powered) {
        clover_gpu_power_on(t);
    } else {
        // Steady state: the post-write readback and the gated chipid, repeated so a long boot that
        // rotates the ring out never loses the verdict.
        clover_gpu_report_branches(t, "beat");
        clover_gpu_report_ldo(t, "beat");
        clover_gpu_report_ldo_words(t, "beat");
        clover_gpu_report_gpucc(t, true);
        clover_gpu_report_chipid(t);
    }

    // Re-armed from inside the callback: wakeAtTime() takes a new deadline while the source is
    // enabled, and that is how a repeating IOKit timer is spelled.
    if (sender != NULL) {
        (void) sender->setTimeoutMS(CLOVER_GPU_STATE_MS);
    }
}

static bool
clover_gpu_map_gcc(void)
{
    IODeviceMemory *memory;

    memory = IODeviceMemory::withRange(kGccPhys, kGccSize);
    if (memory == NULL) {
        return false;
    }
    clover_gpu_gcc_map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (clover_gpu_gcc_map == NULL) {
        return false;
    }
    clover_gpu_gcc = (volatile uint32_t *) clover_gpu_gcc_map->getVirtualAddress();
    return clover_gpu_gcc != NULL;
}

extern "C" void clover_gpu_run(void)
{
    unsigned mode = 0;
    IOService *owner;
    IOWorkLoop *wl;

    if (!PE_parse_boot_argn("clover-gpu", &mode, sizeof(mode)) || mode != 1) {
        return;
    }
    if (!clover_gpu_map_gcc()) {
        printf("clover r286: gpu map failed phys=%llx size=%x\n",
            (unsigned long long) kGccPhys, kGccSize);
        return;
    }

    // One row now, because whatever is true at bsd_autoconf is worth printing even if a long boot
    // later rotates it out; then the schedule that survives the ring horizon.
    clover_gpu_say(NULL, NULL);

    owner = IOService::getResourceService();
    wl = owner != NULL ? owner->getWorkLoop() : NULL;
    if (wl == NULL) {
        printf("clover r286: gpu t=%u notimer (resource service work loop unavailable)\n",
            clover_gpu_uptime_seconds());
        return;
    }
    clover_gpu_timer = IOTimerEventSource::timerEventSource(owner, clover_gpu_say);
    if (clover_gpu_timer == NULL || wl->addEventSource(clover_gpu_timer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(clover_gpu_timer);
        printf("clover r286: gpu t=%u notimer (timer source refused)\n",
            clover_gpu_uptime_seconds());
        return;
    }
    (void) clover_gpu_timer->setTimeoutMS(CLOVER_GPU_FIRST_MS);
}
