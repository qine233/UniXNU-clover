// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the GPU line's first reading -- is the GPUCC interface clock voted
// on, and are the two GPU power domains up?
//
// WHY THIS FILE EXISTS
//
// The desktop is being brought up CPU-first, at the user's direction, and the GPU workstream runs
// beside it rather than in front of it. But "write an Adreno driver" is not a next step; the board
// has never read one GPU register, and the two facts a first driver depends on are still unmeasured:
// whether the GPUCC block is clocked at all, and whether the firmware left the two GDSCs on. Writing
// either register before those are known is how a bring-up round loses the whole board to a power
// state it cannot undo. This file is the measurement, and it is read-only in the hard sense: it maps
// two windows, reads a fixed list of words, prints them, and writes nothing -- not a GDSC bit, not a
// clock vote, not a reset.
//
// WHY GCC COMES FIRST, AND WHY THE EMMC BRANCHES ARE IN THE SAME ROW
//
// The downstream GCC probe enables `gcc_gpu_cfg_ahb_clk` before it touches GPUCC, with the comment
// "This clock is used for all GPUCC register access". So a read of 0x05065000 is only safe when that
// vote is on, and the first row therefore answers the GCC question and refuses the GPUCC one when
// the answer is no. That refusal is also why the row carries `gcc_sdcc1_ahb_clk` and
// `gcc_sdcc1_apps_clk`: the eMMC host at 0xc0c4000 is the one block this port exercises on every
// single boot, so those two branches are a positive control on the *same page* as the GPU votes. A
// zero GPU bit beside a one eMMC bit is a reading; a row of zeros with no control is not, and the
// port has already spent rounds learning that difference.
//
// WHY chipid IS NOT HERE
//
// The GPU core's chipid is the identification a later driver will want (a512 reads 0x05010200), but
// reaching 0x05000000 safely needs GX and the gfx3d root clock, which this round does not control.
// Reading it now would be exactly the unclocked-bus gamble the GCC gate exists to avoid. The first
// round that can write the GDSC and clock state reads chipid and checks it against that value.
//
// WHERE THE ROWS GO
//
// The same place r281's keystore rows go: IOLog, which drivers/CloverSdhci tees into the
// card-backed kernel-log ring. That ring holds 512 records and a long boot rotates out anything
// older than its horizon (boot 67 kept ~355 s), so this probe speaks on a timer -- first row at
// 240 s, then every 120 s -- instead of only at bsd_autoconf. The immediate row at arm time is kept
// because a boot that dies early still deserves whatever was already true; it is not counted on.
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

#define CLOVER_GPU_FIRST_MS   240000u
#define CLOVER_GPU_STATE_MS   120000u

static IOMemoryMap *clover_gpu_gcc_map;
static IOMemoryMap *clover_gpu_gpucc_map;
static volatile uint32_t *clover_gpu_gcc;
static volatile uint32_t *clover_gpu_gpucc;
static IOTimerEventSource *clover_gpu_timer;
static bool clover_gpu_armed;
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

static void
clover_gpu_say(OSObject *owner, IOTimerEventSource *sender)
{
    unsigned t = clover_gpu_uptime_seconds();
    uint32_t ahb, gpu_bimc, bimc, ctl_ahb, ctl_apps;
    uint32_t ahb_raw, ctl_raw;

    (void) owner;
    clover_gpu_fires++;

    // The GCC row, every fire. The two eMMC votes are the control: this board's eMMC host is used
    // before and after this row, so they have to read 1 on a healthy boot.
    ahb      = clover_gpu_read(clover_gpu_gcc, kGccGpuCfgAhb);
    gpu_bimc = clover_gpu_read(clover_gpu_gcc, kGccGpuBimcGfx);
    bimc     = clover_gpu_read(clover_gpu_gcc, kGccBimcGfx);
    ctl_ahb  = clover_gpu_read(clover_gpu_gcc, kGccSdcc1Ahb);
    ctl_apps = clover_gpu_read(clover_gpu_gcc, kGccSdcc1Apps);
    ahb_raw  = ahb;
    ctl_raw  = ctl_ahb;

    printf("clover r282: gpu t=%u fire=%u gcc gpu_ahb=%u gpu_bimc=%u bimc=%u ctl_sdhc1_ahb=%u "
        "ctl_sdhc1_apps=%u raw ahb=%08x ctl=%08x\n", t, (unsigned) clover_gpu_fires,
        (unsigned) (ahb & 1u), (unsigned) (gpu_bimc & 1u), (unsigned) (bimc & 1u),
        (unsigned) (ctl_ahb & 1u), (unsigned) (ctl_apps & 1u),
        (unsigned) ahb_raw, (unsigned) ctl_raw);

    // The gate. 0xffffffff is not a vote of one: it is a bus that did not answer, and GPUCC is not
    // read behind it.
    if ((ahb & 1u) == 0 || ahb_raw == 0xffffffffu) {
        printf("clover r282: gpu t=%u gpucc skipped ahb=%u raw=%08x (GPUCC access needs the "
            "gcc_gpu_cfg_ahb vote)\n", t, (unsigned) (ahb & 1u), (unsigned) ahb_raw);
        goto rearm;
    }

    if (clover_gpu_open_gpucc() == NULL) {
        printf("clover r282: gpu t=%u gpucc map failed phys=%llx\n", t,
            (unsigned long long) kGpuccPhys);
        goto rearm;
    }

    {
        uint32_t cx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscCx);
        uint32_t cxhw = clover_gpu_read(clover_gpu_gpucc, kGpuccCxHwCtrl);
        uint32_t gx   = clover_gpu_read(clover_gpu_gpucc, kGpuccGdscGx);
        uint32_t gfx  = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dEn);
        uint32_t cxo  = clover_gpu_read(clover_gpu_gpucc, kGpuccCxoEn);
        uint32_t rcg  = clover_gpu_read(clover_gpu_gpucc, kGpuccGfx3dRcg);
        uint32_t dom  = clover_gpu_read(clover_gpu_gpucc, kGpuccGxDomain);
        uint32_t sw   = clover_gpu_read(clover_gpu_gpucc, kGpuccGxSwReset);

        // CX's status is the hw-ctrl bit, not its GDSCR: msm-gdsc-660.dtsi gives that node an
        // hw-ctrl-addr, and gdsc-regulator.c polls the syscon instead of the GDSCR in that case.
        printf("clover r282: gpu t=%u gpucc cx=%08x cxhw=%08x cx_on=%u cx_collapse=%u "
            "gx=%08x gx_on=%u gx_collapse=%u gfx3d_en=%u cxo_en=%u rcg=%08x dom=%08x sw=%08x\n",
            t, (unsigned) cx, (unsigned) cxhw, (unsigned) ((cxhw & kGdscPwrOn) != 0),
            (unsigned) ((cx & kGdscSwCollapse) != 0), (unsigned) gx,
            (unsigned) ((gx & kGdscPwrOn) != 0), (unsigned) ((gx & kGdscSwCollapse) != 0),
            (unsigned) (gfx & 1u), (unsigned) (cxo & 1u), (unsigned) rcg, (unsigned) dom,
            (unsigned) sw);
    }

rearm:
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
        printf("clover r282: gpu map failed phys=%llx size=%x\n",
            (unsigned long long) kGccPhys, kGccSize);
        return;
    }

    // One row now, because whatever is true at bsd_autoconf is worth printing even if a long boot
    // later rotates it out; then the schedule that survives the ring horizon.
    clover_gpu_say(NULL, NULL);

    owner = IOService::getResourceService();
    wl = owner != NULL ? owner->getWorkLoop() : NULL;
    if (wl == NULL) {
        printf("clover r282: gpu t=%u notimer (resource service work loop unavailable)\n",
            clover_gpu_uptime_seconds());
        return;
    }
    clover_gpu_timer = IOTimerEventSource::timerEventSource(owner, clover_gpu_say);
    if (clover_gpu_timer == NULL || wl->addEventSource(clover_gpu_timer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(clover_gpu_timer);
        printf("clover r282: gpu t=%u notimer (timer source refused)\n",
            clover_gpu_uptime_seconds());
        return;
    }
    clover_gpu_armed = true;
    (void) clover_gpu_timer->setTimeoutMS(CLOVER_GPU_FIRST_MS);
}
