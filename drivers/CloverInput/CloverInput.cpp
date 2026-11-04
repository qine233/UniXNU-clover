// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the touch bus instrument -- can this kernel reach the panel's I2C?
//
// DOCUMENTED CONTRACT (round314; the clause list is notes/unixnu-doc-conformance.md §2)
//
// Built-in XNU layer per docs/drivers/overview.md:11-16; archive per docs/drivers/xnu-drivers.md:118-124;
// object ABI per docs/drivers/xnu-drivers.md:106-116 (checked by build-arm-driver.py:120,129). The QUP
// address is a board constant because there is no arm-io node for it, as docs/drivers/xnu-drivers.md:215
// prescribes, and the staged boot argument of docs/drivers/native-novatek-touch.md:87-98 is what
// clover-input=1/2/3 is. Against the worked example's rules at :130-137 this file is honest about its
// own gap: rules 1 and 3 (a pure layer separated behind a bus struct, host tests for it before the
// hardware probe) are NOT met here -- drivers/CloverPower/PowerArb.h and drivers/CloverDsp/DspCore.h
// show the shape, and notes/unixnu-doc-conformance.md §7 schedules it. No personality, so no plist per
// docs/drivers/xnu-drivers.md:14; see notes/unixnu-doc-conformance.md §5.
//
// WHY THIS FILE EXISTS
//
// Schedule item 5 is "input": a tablet that boots without a digitizer cannot be driven, and the first
// question is not what a finger looks like but whether the bus answers at all. Everything the port
// knows about this bus comes from three sources that all agree on the topology and disagree on
// nothing that matters: the device's own flattened device tree (clover.dts `i2c@c177000`, with a
// `gt9xx@5d` and a `focaltech@38` hanging off it), the 7.2.y tree that drives this tablet's touch
// today (sdm660-mainline/linux), and Xiaomi's own 4.19 CAF tree (E7G/clover-sdm660-ksb), whose
// gt9xx.c carries a config blob that is byte-identical to the cfg-group0 in this DT -- which is
// how the fitted part was decided without opening the unit. All of it is transcribed in
// drivers/CloverInput/InputRegs.h with a file and a line next to every number.
//
// WHY IT IS AN INSTRUMENT AND NOT A DRIVER YET
//
// sheng solved the same problem in three separable pieces (~/UniXNU-sheng/package/TouchProbe.cpp,
// ShengNovatekTouch.c, ShengHidInput.cpp): the board bring-up, the chip protocol, and the
// IOHIDDevice that publishes digits. Its transport is an injected `struct nvt_bus { ctx; xfer();
// delay_ms(); }` vtable, which is why the same protocol code runs over SPI on sheng and can run
// over anything else later. Copying that shape here means the first increment is exactly sheng's
// first increment: map, vote clocks, set the pins, read an identity, publish it. No HID, no
// multitouch, no descriptor. Those are rounds after this one, and which of them to write first
// depends on a reading this round is going to produce: which address answers.
//
// THE THREE QUESTIONS, ONE PER BOOT ARGUMENT (clover-input=N)
//
// Mode 1 asks what state the firmware left the bus in, and touches nothing. The kernel is not the
// first thing to own this hardware this boot -- the splash framebuffer at 0x9d400000 is live, so
// UEFI ran and handed over. Whether it left blsp_i2c3's clocks gated, its pins in the bootloader
// mux, and its mini-core in reset is the difference between "bring-up is five register writes"
// and "bring-up has to produce a 400 kHz clock from a controller we have never driven". Mode 1
// reads GCC's three branch/RCG registers, TLMM's four registers for each of gpio10, gpio11,
// gpio66, gpio67, and the QUP's whole visible register set, decodes the mux/pull/drive of each
// pin and the block/FIFO geometry of the core, and stops.
//
// Mode 2 asks whether the kernel can bring the bus up at all, in the order the drivers that work
// use: the AHB (iface) vote first because without it the QUP's registers answer nothing, then the
// RCG -- CFG = XO with pre-div 1, then CMD_REG's update bit, which the hardware self-clears --
// then the apps-branch gate, then the two pins (mux 2 = blsp_i2c3, 2 mA, no bias, exactly what
// clover.dts `i2c_3_active` asks for), then the mini-core: SW_RESET, I2C mini-core + v2 N value,
// MASTER_GEN with tags enabled, IO_MODE PACK|UNPACK with the block counters zeroed, and the
// bit-clock divisor. Every wait is bounded and every write is read back, because the verdict that
// matters is which step stopped.
//
// Mode 3 does mode 2 and then asks the chip who it is. First without touching the reset line, in
// case the firmware already released it and a transaction is all that is needed: read one byte at
// each candidate's own address, using that candidate's own address width (Goodix: 2 bytes big-endian
// at 0x8040; EDT/FocalTech: 1 byte at 0x00). If neither answers, run the reset the vendor driver
// runs -- RST low 20 ms, INT driven low (gtp_int_output(client->addr == 0x14), so low for 0x5d),
// 2 ms, RST high, 6 ms, RST high again, then int-sync: INT low for 50 ms and back to input
// (gt9xx.c:507-561) -- and try both again. Whatever answers then gets the identity read it uses
// in its own driver: for Goodix 6 bytes at 0x8140 and 1 at 0x814A (gt9xx.c:1222-1256), read twice
// so a plausible-looking first answer can be checked against itself; for the other candidate the
// 22-byte name at 0xBB that edt-ft5x06.c:846 reads. Raw hex and ASCII, not an interpretation.
//
// WHY NOTHING HERE PANICS
//
// Every earlier board driver in this repository ends its probe modes on panic(), because on those
// rounds a silent boot was indistinguishable from a successful one and the panel was the only
// reader. Round176 changed what the panel is for: the kernel log now reaches the host by itself.
// IOLog() writes into msgbuf, CloverSdhci's tee copies any line containing "clover r" into the
// card-backed kernel ring (kTeeAllowlist[0], CloverSdhci.cpp:396), and scripts/read-clover-log.py
// --tee reads it back off the card with no photograph involved. That channel has no length limit
// per line and no need for the boot to stop -- and stopping the boot is now a *loss*, because the
// lines the rest of the boot would print never get written. So this driver says what it measured
// and returns. The touch bus is not on the boot's critical path: a boot that finds no digitizer
// is a boot that still reaches launchd, and that is worth more readings than any single I2C verdict.
//
// WHAT IT DOES NOT CLAIM
//
// No power rails. The flashed DTB gives both touch nodes a supply (`vdd-supply` = phandle 0x187,
// `vcc_i2c-supply` = 0x17b), and round312 resolved those two inside the same file: 0x187 is
// `pm660l_l3` (fixed 3000000 uV, DT `0x2dc6c0`) and 0x17b is `pm660_l11` (0x1b2920..0x1dc130 =
// 1780000..1950000 uV), both declared
// `compatible = "qcom,rpm-smd-regulator"` -- so they are held by RPM over SMD, not by an AP-side
// register, and `pm8950` appears nowhere in this board's DT (the earlier "PM8950 rails through SPMI"
// in this paragraph was a vendor-tree analogy and is withdrawn; notes/wifi-bringup.md section 4).
// What r313 changes here is the second half of that sentence: an RPM *binding* does not make the
// peripheral unaddressable -- pm660 l11 is still SPMI PPID 0x04a (sid 0, `0x4a00`) and pm660l l3 is
// 0x242 -- and round313 gave CloverPower the v3 floor plan, so the enable byte at 0x46 is now
// reachable pending one answer: which band this board's arbiter version word really names.
// If no address answers here, the next question is still whether VDD is on, not whether the bus is
// broken, and the report says so instead of concluding "no touch controller". No interrupt setup either:
// gpio67's INTR_CFG/INTR_STATUS are read and printed but not programmed, so the point-report round
// starts from whatever state the boot left, not from a claim. No HID, no IOHIDDevice, no registry
// publication, and no coordinate reading -- the 0x814E point format is pinned in the header and
// stays unused until the identity is known.
//
// WHY IT RUNS FROM sheng_pmgr_run()
//
// Same reason as CloverSdhci (see its header comment): a device repository has no XNU patch slot
// for an IOKit personality, so a board driver exports a C entry point and CloverPMGR calls it from
// the bsd_init hook. It is called after clover_emmc_run() so that an eMMC verdict and a touch
// verdict can never share a schedule point, and after the block stack has been requested, so the
// tee is already armed by the time these lines are printed.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include <stdarg.h>
#include "InputRegs.h"
// The bus-free layer: every bit-field, verdict class, tag byte, cursor and unit conversion this
// driver uses lives there, along with the host model (tests/clover_input_host.cpp) that runs them
// without a board. Round318; notes/unixnu-doc-conformance.md §7 gap 3.
#include "InputCore.h"

extern "C" {
// drivers/CloverSdhci's commit of both log rings to the card. At this scheduling point the block
// device has not been created yet -- clover_block_request() only asked for it, and the stack runs
// on the IOBSD publication -- so this returns 0 without touching the card. It is still the right
// call: the same line becomes the real commit as soon as a later round runs this after bring-up,
// and it costs a load of one flag to find out.
uint32_t clover_emmc_flush_cache (void);
}

// ------------------------------------------------------------------ the windows
//
// One struct per mapping, because this driver needs six of them and a report that says which one
// failed to map is worth more than a driver that quietly reads zeros. The maps live as long as the
// boot: the platform owns them from here on, the same as CloverSdhci's host and GCC windows.
struct Window
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t base = 0;
  uint64_t phys = 0;
  uint32_t size = 0;
};

static Window gAhbHalt;   // 0x117000: the AHB branch's CBCR
static Window gI2cClock;  // 0x11d000: the apps branch and its RCG
static Window gAhbVote;   // 0x152000: the AHB vote bit
static Window gPinsI2c;   // gpio10 (SDA) and gpio11 (SCL)
static Window gPinsTouch; // gpio66 (reset) and gpio67 (interrupt)
static Window gQup;       // the QUP v2 core itself

// Same three steps CloverSdhci has used since round87: ask the platform for the range, map it
// uncached and anywhere, keep only the virtual address. A range the platform memory map does not
// have returns null rather than a wrong pointer.
static uintptr_t mapWindow (Window &window, uint64_t phys, uint32_t size)
{
  window.phys = phys;
  window.size = size;
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, size);
  if (!memory) {
    return 0;
  }
  window.map = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  if (!window.map) {
    return 0;
  }
  window.base = static_cast<uintptr_t> (window.map->getVirtualAddress ());
  return window.base;
}

static uint32_t windowRead (const Window &window, uint32_t offset)
{
  const uint32_t value = *reinterpret_cast<volatile uint32_t *> (window.base + offset);
  __asm__ volatile ("dsb sy" ::: "memory");
  return value;
}

static void windowWrite (const Window &window, uint32_t offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (window.base + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Pin registers are addressed by pin, because the four this driver cares about are two pairs and
// a report that says "gpio67 CTL" is worth more than one that says "+0x1008".
static uint32_t pinRead (uint32_t pin, uint32_t reg)
{
  const Window &window = (pin <= 11) ? gPinsI2c : gPinsTouch;
  const uint32_t offset = (pin - ((pin <= 11) ? 10u : 66u)) * kTlmmPinStride + reg;
  return windowRead (window, offset);
}

static void pinWrite (uint32_t pin, uint32_t reg, uint32_t value)
{
  const Window &window = (pin <= 11) ? gPinsI2c : gPinsTouch;
  const uint32_t offset = (pin - ((pin <= 11) ? 10u : 66u)) * kTlmmPinStride + reg;
  windowWrite (window, offset, value);
}

static uint32_t qupRead (uint32_t reg)
{
  return windowRead (gQup, reg);
}

static void qupWrite (uint32_t reg, uint32_t value)
{
  windowWrite (gQup, reg, value);
}

// ------------------------------------------------------------------ time, bounded
//
// The generic timer, exactly as CloverSdhci uses it: the frequency read is sanity-checked against
// 1 MHz..1 GHz and falls back to the board's 19.2 MHz XO, and every wait below carries a deadline
// in ticks. sheng_pmgr_run() runs about ten seconds of boot before the timer is serving anything,
// and round87 proved a hang inside a driver at this point produces no panic at all -- so a bound
// that reports is the only way a stuck bit becomes a reading instead of a dead boot.
static inline uint64_t readCounter (void)
{
  uint64_t value;
  __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (value));
  return value;
}

static inline uint64_t counterFrequency (void)
{
  uint64_t value;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (value));
  return value;
}

static uint64_t gTicksPerMs = input_ticks_per_ms (kXoHz);   // the XO default; refreshed at bring-up

// The sanity window and the XO fallback are the layer's; this function only fetches the register.
static void refreshTicksPerMs (void)
{
  gTicksPerMs = input_ticks_per_ms (counterFrequency ());
}

// Busy wait. Nothing sleeps here: at this point in the boot a thread switch is not available to
// code running from bsd_init's hook, and every delay this driver needs is a datasheet timing
// requirement measured in milliseconds, not a scheduling opportunity.
static void waitMs (uint32_t ms)
{
  const uint64_t start = readCounter ();
  const uint64_t want = input_ticks_for_ms (ms, gTicksPerMs);
  while (readCounter () - start < want) {
    __asm__ volatile ("yield" ::: "memory");
  }
}

// ------------------------------------------------------------------ the raw observations
//
// Kept for the same reason CloverSdhci keeps clover_emmc_observation: a number that survives to
// the panic screen and to any later offline decoder without depending on a log ring. Slots:
//   [0]  mode, [1] which window failed to map (its phys) or 0 when all six mapped,
//   [2]  QUP HW_VERSION, [3] IO_MODE after bring-up, [4] OPERATIONAL at the last verdict,
//   [5] ERROR_FLAGS, [6] I2C_STATUS, [7] STATE, [8] I2C_CLK_CTL written,
//   [9] the address that answered (0 when none did), [10] first identity byte, [11] second,
//   [12] sub-transfers run, [13] sub-transfers that timed out, [14] NACK count,
//   [15] the last verdict code (see the note at each site).
volatile uint64_t clover_input_observation[16];

// Round319: the last sub-transfer's raw words, latched so package/CloverHidInputProbe.cpp can re-say
// the r318 detail on its 120 s beat after the one-time rows are overwritten by the tee's ring (the
// r260 pattern: nothing is re-asked, only the reading repeats). Slots:
//   [0] raw I2C_STATUS, [1] raw ERROR_FLAGS, [2] SDA high, [3] SCL high,
//   [4] the vendor tree's mask over the raw status word, [5] the residue bits inside MAIN's mask.
// All zeros means the instrument never ran a segment, which a missing beat row cannot say.
volatile uint64_t clover_input_last_xfer[6];

// A one-word verdict code, so the ring line and the observation array cannot disagree about which
// step stopped. Positive values are steps, negative values are the two ways a step can fail. The
// numbers and their names both live in InputCore.h (round318); this file has no copy of them.

// ------------------------------------------------------------------ mode 1: what is the state?
//
// Every line is a reading with its fields named, because the reader is a photograph or a dd of a
// ring and has no way to ask a follow-up question.
static void dumpClocks (void)
{
  const uint32_t ahbHalt = windowRead (gAhbHalt, kGccAhbHaltOff);
  const uint32_t ahbVote = windowRead (gAhbVote, kGccAhbVoteOff);
  const uint32_t branch = windowRead (gI2cClock, kGccI2cBranchOff);
  const uint32_t cmd = windowRead (gI2cClock, kGccI2cCmdOff);
  const uint32_t cfg = windowRead (gI2cClock, kGccI2cCfgOff);
  // Only the rows this RCG's frequency table can be left in are named. Anything else prints the
  // selector and stops, rather than inventing a rate -- and that refusal is the layer's, not a
  // half-remembered branch here.
  struct InputRcg rcg;
  input_rcg_decode (cfg, &rcg);
  IOLog ("clover r181: gcc vote=%08x ahb_halt=%08x i2c_branch=%08x\n",
         ahbVote, ahbHalt, branch);
  IOLog ("clover r181: rcg cmd=%08x cfg=%08x src=%u div=%u mode=%u hw=%u\n",
         cmd, cfg, rcg.src, rcg.div, rcg.mode, rcg.hw_ctrl ? 1u : 0u);
  IOLog ("clover r181: apps clk parent=%s hz=%llu branch_on=%u ahb_on=%u\n",
         rcg.parent, static_cast<unsigned long long> (rcg.hz),
         input_branch_running (branch) ? 1u : 0u,
         input_branch_running (ahbHalt) ? 1u : 0u);
}

// One decoded line per pin, in the same order pinctrl-msm.c's debug print gives: direction,
// function, level, drive, bias. The values are this SoC's own encodings -- drive reg value r is
// (r+1)*2 mA, bias 0 none / 1 down / 2 keeper / 3 up -- and every one of those cut-and-name
// decisions is InputCore.h's input_pin_decode, so the row and the host model read the same field.
static void dumpPin (uint32_t pin, const char *label)
{
  const uint32_t ctl = pinRead (pin, kPinCtl);
  const uint32_t io = pinRead (pin, kPinIo);
  const uint32_t cfg = pinRead (pin, kPinIntrCfg);
  const uint32_t status = pinRead (pin, kPinIntrStatus);
  struct InputPinState state;
  input_pin_decode (ctl, io, cfg, status, &state);
  IOLog ("clover r181: pin %u %s ctl=%08x io=%08x mux=%u oe=%u in=%u out=%u drv=%u mA pull=%u\n",
         pin, label, ctl, io, state.mux, state.oe, state.in_level, state.out_level,
         state.drive_ma, state.pull);
  IOLog ("clover r181: pin %u intr cfg=%08x status=%08x tgt=%u det=%u pol=%u en=%u raw=%u\n",
         pin, cfg, status, state.intr_target, state.intr_detect, state.intr_polarity,
         state.intr_enable, state.intr_raw);
}

// The core's own state: what version, what the boot left in the counters, and what the FIFOs are.
// The block/FIFO depth is the layer's input_fifo_sizes, which reproduces i2c-qup.c:1856-1890 --
// out_blk_sz = blk_sizes[QUP_OUTPUT_BLOCK_SIZE(io_mode)], out_fifo_sz = out_blk_sz * (2 << size).
static uint32_t gOutFifoSize = 0;
static uint32_t gInFifoSize = 0;

static void dumpQup (const char *when)
{
  const uint32_t ioMode = qupRead (kQupIoMode);
  input_fifo_sizes (ioMode, &gOutFifoSize, &gInFifoSize);
  clover_input_observation[2] = qupRead (kQupHwVersion);
  clover_input_observation[3] = ioMode;
  IOLog ("clover r181: qup %s hw=%08x state=%08x cfg=%08x io=%08x op=%08x\n",
         when, qupRead (kQupHwVersion), qupRead (kQupState), qupRead (kQupConfig), ioMode,
         qupRead (kQupOperational));
  IOLog ("clover r181: qup %s err=%08x err_en=%08x op_mask=%08x status=%08x clk=%08x\n",
         when, qupRead (kQupErrorFlags), qupRead (kQupErrorFlagsEn), qupRead (kQupOperationalMask),
         qupRead (kQupI2cStatus), qupRead (kQupI2cClkCtl));
  IOLog ("clover r181: qup %s cnt out=%08x write=%08x in=%08x read=%08x fifo=%u/%u\n",
         when, qupRead (kQupMxOutputCnt), qupRead (kQupMxWriteCnt), qupRead (kQupMxInputCnt),
         qupRead (kQupMxReadCnt), gOutFifoSize, gInFifoSize);
}

// ------------------------------------------------------------------ mode 2: bring the bus up
//
// Order is not a style choice. The AHB vote is first because a QUP whose interface clock is gated
// returns nothing to a register read -- and, worse for a report, may stall the bus. Then the RCG,
// then the branch, then the pins, then the core, because that is the order clk_enable_regmap() +
// clk_branch_wait() and qup_i2c_runtime_resume() + qup_i2c_probe() perform, which is the order
// that is known to work on this silicon.
static bool voteAhb (void)
{
  const uint32_t before = windowRead (gAhbVote, kGccAhbVoteOff);
  if (input_ahb_voted (before)) {
    IOLog ("clover r181: ahb already voted %08x\n", before);
  } else {
    windowWrite (gAhbVote, kGccAhbVoteOff, input_ahb_vote_word (before));
  }
  // BRANCH_HALT_VOTED: clk_branch_wait() polls the halt register of 0x17004, not the vote
  // register of 0x52004, and accepts either polarity of CLK_OFF or the NOC FSM saying "on".
  const uint32_t haltPhys = windowRead (gAhbHalt, kGccAhbHaltOff);
  uint32_t halt = haltPhys;
  uint64_t spins = 0;
  while (input_branch_halted (halt)) {
    if (++spins >= kCbcrSpin) {
      IOLog ("clover r181: ahb halt stuck %08x vote=%08x->%08x\n",
             halt, before, windowRead (gAhbVote, kGccAhbVoteOff));
      clover_input_observation[15] = input_verdict_code (kInputStepAhbVote, kInputFailPoll);
      return false;
    }
    halt = windowRead (gAhbHalt, kGccAhbHaltOff);
  }
  IOLog ("clover r181: ahb halt=%08x vote %08x->%08x spins=%u\n",
         halt, before, windowRead (gAhbVote, kGccAhbVoteOff), static_cast<unsigned> (spins));
  clover_input_observation[15] = kInputStepAhbVote;
  return true;
}

// The RCG's XO row: __clk_rcg2_configure_mnd() masks out div|mode|hw_ctrl and writes pre_div = 1,
// __clk_rcg2_configure_parent() masks src and writes P_XO's 0, then update_config() sets CMD bit 0
// and polls it for at most 500 us. mnd_width is 0 for this RCG, so M/N/D are not part of the row
// and are left exactly as the boot left them -- which is what round180's eMMC sibling learned the
// hard way about (mode 5's 400 kHz row there really did use M/N; this one does not).
static bool programRcg (void)
{
  const uint32_t cfgBefore = windowRead (gI2cClock, kGccI2cCfgOff);
  const uint32_t cfg = input_rcg_xo_cfg (cfgBefore);
  windowWrite (gI2cClock, kGccI2cCfgOff, cfg);
  const uint32_t cmdBefore = windowRead (gI2cClock, kGccI2cCmdOff);
  windowWrite (gI2cClock, kGccI2cCmdOff, input_rcg_update_word (cmdBefore));
  uint32_t cmd = windowRead (gI2cClock, kGccI2cCmdOff);
  uint64_t spins = 0;
  while (input_rcg_update_pending (cmd)) {
    if (++spins >= kRcgUpdateSpin) {
      IOLog ("clover r181: rcg update stuck cmd=%08x cfg %08x->%08x\n",
             cmd, cfgBefore, windowRead (gI2cClock, kGccI2cCfgOff));
      clover_input_observation[15] = input_verdict_code (kInputStepRcgUpdate, kInputFailPoll);
      return false;
    }
    cmd = windowRead (gI2cClock, kGccI2cCmdOff);
  }
  IOLog ("clover r181: rcg cfg %08x->%08x cmd=%08x cleared spins=%u\n",
         cfgBefore, windowRead (gI2cClock, kGccI2cCfgOff), cmd, static_cast<unsigned> (spins));
  clover_input_observation[15] = kInputStepRcgUpdate;
  return true;
}

static bool enableBranch (void)
{
  const uint32_t before = windowRead (gI2cClock, kGccI2cBranchOff);
  windowWrite (gI2cClock, kGccI2cBranchOff, input_branch_enable_word (before));
  uint32_t halt = before;
  uint64_t spins = 0;
  while (input_branch_halted (halt)) {
    if (++spins >= kCbcrSpin) {
      IOLog ("clover r181: branch stuck %08x wrote %08x\n", halt,
             input_branch_enable_word (before));
      clover_input_observation[15] = input_verdict_code (kInputStepBranch, kInputFailPoll);
      return false;
    }
    halt = windowRead (gI2cClock, kGccI2cBranchOff);
  }
  IOLog ("clover r181: branch %08x->%08x spins=%u\n", before, halt, static_cast<unsigned> (spins));
  clover_input_observation[15] = kInputStepBranch;
  return true;
}

// Both lines to blsp_i2c3, 2 mA, no bias: clover.dts `i2c_3_active`'s mux { function =
// "blsp_i2c3" } + config { drive-strength = <2>; bias-disable }. msm_pinmux_set_mux() writes only
// the CTL mux field, and the config path only the fields the state names -- so the input/output
// latch bits are left as the boot had them, which is exactly what the dump above will show was
// true. The QUP drives these two as open-drain, so the driver deliberately never touches OE.
static void programI2cPins (void)
{
  for (uint32_t i = 0; i < 2; i++) {
    const uint32_t pin = 10u + i;
    const uint32_t ctl = pinRead (pin, kPinCtl);
    const uint32_t next = input_i2c_pin_ctl (ctl);
    pinWrite (pin, kPinCtl, next);
    IOLog ("clover r181: pin %u i2c ctl %08x->%08x io=%08x\n",
           pin, ctl, pinRead (pin, kPinCtl), pinRead (pin, kPinIo));
  }
  clover_input_observation[15] = kInputStepPins;
}

// The reset sequence's own pin states. Following the vendor driver's ordering:
// gpio_direction_output(n, v) writes the IO latch and then CTL's OE bit (pinctrl-msm.c:575-597),
// gpio_direction_input() clears OE (pinctrl-msm.c:551-567) and nothing else. Mux is forced to
// gpio (0) the first time either pin is driven, because a boot that had used the pin for anything
// else would otherwise stay in that function -- and gpio66/67 have no other function that
// matters here (InputRegs.h), so 0 is the only value that can be wrong in a useful direction.
static void pinOutput (uint32_t pin, uint32_t level)
{
  const uint32_t io = pinRead (pin, kPinIo);
  pinWrite (pin, kPinIo, input_pin_output_io (io, level));
  const uint32_t ctl = pinRead (pin, kPinCtl);
  pinWrite (pin, kPinCtl, input_pin_output_ctl (ctl));
}

static void pinInput (uint32_t pin)
{
  const uint32_t ctl = pinRead (pin, kPinCtl);
  pinWrite (pin, kPinCtl, input_pin_input_ctl (ctl));
}

// The mini-core, in qup_i2c_xfer_v2()'s and qup_i2c_probe()'s order: SW_RESET and wait for the
// state to read back reset, name the block I2C mini-core with the v2 N value, enable tags through
// I2C_MASTER_GEN and wait for BIT(4) to say the mini-core took it, then IO_MODE with the packing
// hardware on and both block counters zeroed (FIFO mode: i2c-qup.c:1182-1204), then the divisor.
//
// Upstream runs this whole sequence at the head of EVERY transfer (i2c-qup.c:1585-1596), not once
// per device, because SW_RESET is what drops the done flags and the two FIFO counters back to their
// reset values. So this is called once per candidate read as well, and it is written to be
// idempotent: the only state that survives it is the clock divisor, which the first sub-transfer of
// each transfer reprograms anyway.
static bool bringUpCore (void)
{
  qupWrite (kQupSwReset, 1);
  uint64_t spins = 0;
  uint32_t state = qupRead (kQupState);
  while (!input_state_reset_done (state)) {
    if (++spins >= kCbcrSpin) {
      IOLog ("clover r181: sw_reset stuck state=%08x\n", state);
      clover_input_observation[15] = input_verdict_code (kInputStepResetCore, kInputFailPoll);
      return false;
    }
    state = qupRead (kQupState);
  }
  qupWrite (kQupConfig, input_core_mode_word ());
  qupWrite (kQupI2cMasterGen, kQupV2TagsEn);
  spins = 0;
  state = qupRead (kQupState);
  while (!input_core_tags_taken (state)) {
    if (++spins >= kCbcrSpin) {
      IOLog ("clover r181: mast_gen stuck state=%08x gen=%08x\n",
             state, qupRead (kQupI2cMasterGen));
      clover_input_observation[15] = input_verdict_code (kInputStepMasterGen, kInputFailPoll);
      return false;
    }
    state = qupRead (kQupState);
  }
  qupWrite (kQupMxOutputCnt, 0);
  qupWrite (kQupMxInputCnt, 0);
  qupWrite (kQupIoMode, kQupRepackEn);
  clover_input_observation[15] = kInputStepResetCore;
  return true;
}

// State changes are polled, not assumed: qup_i2c_change_state() waits for STATE_VALID, writes, and
// then waits for the field to read back (i2c-qup.c:430-441). A transition takes 3 AHB + 3 master
// clocks, which at 400 kHz is 15 us, so the bound is generous. The step argument is which verdict
// code to leave behind if it will not transition, because "state stuck" at run and at pause are
// different hardware answers.
static bool changeState (uint32_t want, const char *label, uint64_t step)
{
  qupWrite (kQupState, want);
  uint64_t spins = 0;
  uint32_t state = qupRead (kQupState);
  while (!input_state_reached (state, want)) {
    if (++spins >= kCbcrSpin) {
      IOLog ("clover r181: state %s=%u stuck %08x\n", label, want, state);
      clover_input_observation[15] = input_verdict_code (step, kInputFailPoll);
      return false;
    }
    state = qupRead (kQupState);
  }
  return true;
}

// ------------------------------------------------------------------ the transfer engine
//
// FIFO mode only, which is what the upstream mode selection picks for anything up to out_fifo_sz
// bytes (i2c-qup.c:1552-1563) and every transaction this round issues is 12 bytes or less. A
// transfer is the pair the two chip drivers use -- write the register address, then read N bytes
// -- and the repeated start comes from moving to PAUSE between the sub-transfers instead of
// letting the last tag emit a stop, which is qup_i2c_conf_xfer_v2()'s change_pause_state.
struct Segment
{
  const uint8_t *tx;      // bytes to send, including the tag bytes
  uint32_t txLen;
  uint8_t *rx;            // buffer for the received data (tags excluded)
  uint32_t rxLen;
  bool first;             // first sub-transfer of the whole transfer: programs the clock
  bool last;              // last one: emit STOP
};

static uint32_t gXfersRun = 0;
static uint32_t gXfersTimeout = 0;
static uint32_t gXfersNack = 0;

// Push whole words, low byte first, into the output FIFO. A short final word is written with its
// unused bytes zero, which is what upstream's send_last_word does, and the counter written to
// MX_WRITE_CNT tells the hardware how many of those bytes are real. The cursor -- how far the
// message has got and how much FIFO room is left to write into -- is InputTx's.
static void pushTx (struct InputTx *tx, const uint8_t *bytes, uint32_t len)
{
  uint32_t word = 0;
  while (input_tx_next (tx, bytes, len, &word)) {
    qupWrite (kQupOutFifoBase, word);
  }
}

// Drain the input FIFO level-gated, the way round98 learned to drain the eMMC FIFO: a byte is only
// taken when the hardware says a word is buffered, so this can never read past what arrived. The
// first kQupRxTagLen bytes of a read are the tags the core echoes back, and are dropped -- by
// input_rx_pop, not here, so the tag count and the drop cannot drift apart.
static void drainRx (struct InputRx *rx, uint8_t *out)
{
  while (!input_rx_complete (rx)) {
    if (input_rx_needs_word (rx)) {
      if (!(qupRead (kQupOperational) & kQupInNotEmpty)) {
        return;
      }
      input_rx_word (rx, qupRead (kQupInFifoBase));
    }
    input_rx_pop (rx, out);   // a tag byte returns 0 and still consumes its position
  }
}

static bool runSegment (const Segment &seg, const char *why)
{
  // config_run: the first sub-transfer programs the counters normally, a later one sets BIT(31)
  // to say "reprogram while RUNNING" -- the mechanism that makes a repeated start possible without
  // releasing the bus. Both counters and the CONFIG word come from the layer, so the read count and
  // the "this transfer reads nothing" bit are derived from the same two lengths.
  struct InputCounters counters;
  input_counters (seg.txLen, seg.rxLen, seg.first ? 1 : 0, &counters);
  qupWrite (kQupMxWriteCnt, counters.write_cnt);
  if (counters.has_read) {
    qupWrite (kQupMxReadCnt, counters.read_cnt);
  }
  qupWrite (kQupConfig, input_core_config (seg.rxLen != 0));

  if (seg.first) {
    // RUN, program the bit clock, back to PAUSE: i2c-qup.c:1500-1505. The divisor only takes
    // effect while running, and the pause is what holds the bus between sub-transfers.
    if (!changeState (kQupStateRun, "run0", kInputStepRun)) {
      return false;
    }
    qupWrite (kQupI2cClkCtl, kQupI2cClk400k);
    clover_input_observation[8] = kQupI2cClk400k;
    clover_input_observation[15] = kInputStepClk;
    if (!changeState (kQupStatePause, "pause0", kInputStepPause)) {
      return false;
    }
  }

  struct InputTx tx;
  input_tx_reset (&tx, gOutFifoSize);
  pushTx (&tx, seg.tx, seg.txLen);

  if (!changeState (kQupStateRun, "run", kInputStepRun)) {
    return false;
  }

  const uint32_t want = input_done_mask (seg.rxLen);
  struct InputRx rx;
  input_rx_reset (&rx, seg.rxLen, kQupRxTagLen);
  const uint64_t start = readCounter ();
  const uint64_t deadline = input_ticks_for_ms (kInputXferTimeoutMs, gTicksPerMs);
  uint32_t op = 0, err = 0, bus = 0, rawErr = 0, rawStatus = 0;
  struct InputBusStatus busStatus;
  struct InputCoreFlags coreFlags;
  for (;;) {
    op = qupRead (kQupOperational);
    // Two views of the same two reads: the masked words keep round181's poll and its r181 row
    // byte-identical, and the raw words are what the r318 rows below report. A second read of either
    // register would let the two rows describe two different moments.
    rawErr = qupRead (kQupErrorFlags);
    rawStatus = qupRead (kQupI2cStatus);
    input_core_flags_decode (rawErr, &coreFlags);
    input_bus_status_decode (rawStatus, &busStatus);
    err = coreFlags.mainline_error;
    bus = busStatus.mainline_error;
    if (err || bus) {
      break;
    }
    if (op & kQupOutSvcFlag) {
      qupWrite (kQupOperational, kQupOutSvcFlag);   // write-1-to-clear, as the ISR does
      tx.room = gOutFifoSize;
      pushTx (&tx, seg.tx, seg.txLen);
      tx.room = input_tx_room_left (gOutFifoSize, tx.sent);
    }
    if (seg.rxLen && (op & (kQupInSvcFlag | kQupInNotEmpty))) {
      drainRx (&rx, seg.rx);
      if (op & kQupInSvcFlag) {
        qupWrite (kQupOperational, kQupInSvcFlag);
      }
    }
    // Stricter than upstream's FIFO-mode write path, which lets any handled flag end the
    // sub-transfer: here the done bit AND the byte count have to agree, so a transfer that ended
    // early is a timeout rather than short data that looks real.
    if ((op & want) && (!seg.rxLen || input_rx_complete (&rx)) && tx.sent == seg.txLen) {
      break;
    }
    if (readCounter () - start > deadline) {
      op = qupRead (kQupOperational);
      break;
    }
  }
  const uint32_t ms = input_elapsed_ms (readCounter () - start, gTicksPerMs);
  const int verdict = input_xfer_verdict (op, err, bus, want, tx.sent, seg.txLen,
                                          rx.got, seg.rxLen);
  gXfersRun++;
  clover_input_observation[4] = op;
  clover_input_observation[5] = err;
  clover_input_observation[6] = bus;
  clover_input_observation[7] = qupRead (kQupState);
  // The raw-status reading, on every segment and not only on failures: the r181 bus= column is the
  // masked word, so SDA/SCL's own bits and whatever sits outside either tree's mask cannot appear
  // there by construction. `vend=` is the vendor tree's mask over the same raw word, `res=` the bits
  // inside MAIN's mask that neither tree names -- so a reader can see which of the three readings a
  // non-zero belongs to. class= is the same verdict the branch below takes. Carried by v0.7.30 and
  // later only.
  IOLog ("clover r318: xfer %s class=%s status=%08x err=%08x vend=%08x res=%08x sda=%u scl=%u "
         "op=%08x t=%ums\n",
         why, input_xfer_verdict_name (verdict), rawStatus, rawErr, busStatus.vendor_error,
         busStatus.residue, busStatus.sda_high, busStatus.scl_high, op, ms);
  // ERROR_FLAGS' own four run counters, named. They are the only bits of that register the vendor
  // tree can set during a transfer, and each one names a different FIFO fault, so a non-zero raw=
  // above is only actionable once this row says which.
  IOLog ("clover r318: core %s class=%s flags=%08x err=%08x vend=%08x res=%08x "
         "iovr=%u ouvr=%u iur=%u oovr=%u\n",
         why, input_xfer_verdict_name (verdict), rawErr, coreFlags.mainline_error,
         coreFlags.vendor_error, coreFlags.residue, coreFlags.input_over_run,
         coreFlags.output_under_run, coreFlags.input_under_run, coreFlags.output_over_run);
  clover_input_last_xfer[0] = rawStatus;
  clover_input_last_xfer[1] = rawErr;
  clover_input_last_xfer[2] = busStatus.sda_high;
  clover_input_last_xfer[3] = busStatus.scl_high;
  clover_input_last_xfer[4] = busStatus.vendor_error;
  clover_input_last_xfer[5] = busStatus.residue;
  if (verdict != kInputXferDone) {
    if (verdict == kInputXferIncomplete) {
      gXfersTimeout++;
      IOLog ("clover r181: xfer %s timeout op=%08x sent=%u/%u got=%u/%u t=%ums\n",
             why, op, tx.sent, seg.txLen, rx.got, seg.rxLen, ms);
      clover_input_observation[15] = kInputFailPoll;
    } else {
      const bool nack = input_xfer_is_nack (bus) != 0;
      gXfersNack += nack ? 1u : 0u;
      IOLog ("clover r181: xfer %s err=%08x bus=%08x op=%08x %s t=%ums\n",
             why, err, bus, op, nack ? "NACK" : "error", ms);
      clover_input_observation[15] = kInputFailXfer;
    }
    // qup_i2c_error() (i2c-qup.c:918-936) answers a bus or core error with a SW_RESET, which is
    // also what drops the stale done flags before the next candidate is asked.
    qupWrite (kQupSwReset, 1);
    return false;
  }
  if (!seg.last) {
    if (!changeState (kQupStatePause, "pause", kInputStepPause)) {
      return false;
    }
  }
  IOLog ("clover r181: xfer %s ok op=%08x got=%u t=%ums\n", why, op, rx.got, ms);
  return true;
}

// The end of one transfer: wait for the bus to say it is idle, then put the core back in reset,
// which is what clears the done flags for the next one (i2c-qup.c:1616-1622, where
// qup_i2c_bus_active() is followed by qup_i2c_change_state(qup, QUP_RESET_STATE)).
static bool finishTransfer (void)
{
  // BUS_ACTIVE is the same bit input_bus_status_decode names, so the spin and the r318 row that
  // reports it cannot use two different definitions of "the bus is still busy".
  struct InputBusStatus status;
  input_bus_status_decode (qupRead (kQupI2cStatus), &status);
  uint64_t spins = 0;
  while (status.bus_active) {
    if (++spins >= kCbcrSpin) {
      IOLog ("clover r181: bus still active status=%08x\n", status.raw);
      break;
    }
    input_bus_status_decode (qupRead (kQupI2cStatus), &status);
  }
  return changeState (kQupStateReset, "reset", kInputStepResetCore);
}

// One i2c transfer: the register-address write, then optionally the read. Tags are built the way
// qup_i2c_set_tags() does it for v2 (i2c-qup.c:569-611) -- START, the 7-bit address shifted with the
// R/W bit in bit 0, a data tag whose STOP variant only appears on the last sub-transfer, and the
// byte count. A count of 0 in that field means 256, so this refuses it rather than sending one.
static bool chipRead (uint8_t address, const uint8_t *reg, uint32_t regLen, uint8_t *out,
                      uint32_t outLen, const char *why)
{
  if (!input_read_request_ok (regLen, outLen)) {
    IOLog ("clover r181: %s refused, reg=%u len=%u outside one block\n", why, regLen, outLen);
    return false;
  }
  // Upstream resets the mini-core at the head of every transfer, not once per device
  // (i2c-qup.c:1584), so do the same before each candidate is asked.
  if (!bringUpCore ()) {
    return false;
  }
  // tx = {START, addr_wr, DATAWR, len} then the register bytes: 4 tag bytes + regLen data. The write
  // segment of a two-part transfer never carries the STOP variant, because the repeated start that
  // follows it is the whole point (qup_i2c_set_tags() only picks _STOP when this is the last block
  // of the last message).
  uint8_t wire[kQupMaxTagsLen + 2];
  const uint32_t wireLen = input_write_tags (address, regLen, outLen ? 0 : 1, wire, sizeof (wire));
  if (wireLen != kQupMaxTagsLen) {
    IOLog ("clover r181: %s refused, reg=%u has no v2 tag encoding\n", why, regLen);
    return false;
  }
  for (uint32_t i = 0; i < regLen; i++) {
    wire[kQupMaxTagsLen + i] = reg[i];
  }
  Segment write { wire, wireLen + regLen, nullptr, 0, true, false };
  if (!runSegment (write, why)) {
    return false;
  }
  // rx = {START, addr_rd, DATARD_STOP, len}: the tag bytes leave through the SAME (output) FIFO,
  // which is qup_i2c_write_rx_tags_v2()'s whole job (i2c-qup.c:1305-1312, called from 1426), and
  // MX_READ_CNT counts the two tag bytes the core echoes back into the input FIFO along with the
  // data (i2c-qup.c:1474-1482).
  uint8_t tags[kQupMaxTagsLen];
  if (!input_read_tags (address, outLen, tags, sizeof (tags))) {
    return false;
  }
  Segment read { tags, kQupMaxTagsLen, out, outLen, false, true };
  if (!runSegment (read, why)) {
    return false;
  }
  return finishTransfer ();
}

// A bare "does this address ACK?" ping with no data after it is deliberately NOT provided: in a v2
// DATAWR/DATARD tag a byte count of 0 does not mean zero, it means 256 (i2c-qup.c:600-608, where
// qup_i2c_get_data_len() == QUP_READ_LIMIT is encoded as 0), so the smallest honest question this
// core can ask is one register byte read back -- which is also the smallest question the two chip
// drivers ask. That is what probeCandidate() below sends.

// ------------------------------------------------------------------ mode 3: who is on the bus?
//
// Printable form of a response, because the reading that decides the next round is a name, not a
// number: the Goodix pid field is ASCII, and so is the EDT name.
static void printBytes (const char *label, const uint8_t *data, uint32_t len)
{
  char hex[3 * 8 + 1];
  char text[8 + 1];
  uint32_t shown = len > 8 ? 8 : len;
  uint32_t h = 0, t = 0;
  for (uint32_t i = 0; i < shown; i++) {
    hex[h++] = "0123456789abcdef"[(data[i] >> 4) & 0xf];
    hex[h++] = "0123456789abcdef"[data[i] & 0xf];
    hex[h++] = ' ';
    text[t++] = (data[i] >= 0x20 && data[i] < 0x7f) ? static_cast<char> (data[i]) : '.';
  }
  hex[h] = '\0';
  text[t] = '\0';
  IOLog ("clover r181: %s n=%u hex=%s ascii=[%s]\n", label, len, hex, text);
}

// The vendor's reset, gt9xx.c:532-561, with the vendor's own timings: T2 the argument (20 ms), T3
// 2 ms, T4 6 ms, then int-sync's 50 ms. INT is driven to (address == 0x14), i.e. low for the
// candidate at 0x5d; the reset line is left high at the end and INT goes back to input, which is
// the state the driver runs in once it has probed.
//
// The drive strength comes from the DT, not from a guess: clover.dts's `ts_int_defalut` state puts
// gpio67 at 16 mA (kTsIntDrive) and the pin has no i2c or other function to contend with. Setting
// it here is the only CTL write this sequence makes beyond mux and OE, and it is done once, before
// the line is driven, so both `ts_int_output_low` and `ts_int_output_high` inherit it.
static void goodixReset (uint8_t address)
{
  const uint32_t ctl = pinRead (67, kPinCtl);
  pinWrite (67, kPinCtl, input_pin_drive_ctl (ctl, kTsIntDrive));
  pinOutput (66, 0);
  waitMs (20);
  pinOutput (67, address == 0x14 ? 1 : 0);
  waitMs (2);
  pinOutput (66, 1);
  waitMs (6);
  pinOutput (66, 1);
  pinOutput (67, 0);
  waitMs (50);
  pinInput (67);
  IOLog ("clover r181: reset done addr=0x%02x rst=%08x int=%08x\n",
         address, pinRead (66, kPinCtl), pinRead (67, kPinCtl));
}

// One candidate, one convention. Goodix addresses its registers with two bytes, big-endian
// (GTP_ADDR_LENGTH 2, and gt9xx.c:1225 builds {REG >> 8, REG & 0xff}); EDT/FocalTech uses one.
// The first transaction asked of each is the smallest real read its own driver does, so a NACK
// here means the address is empty, not that a made-up probe failed.
static void probeCandidate (uint8_t address, const char *name, bool isGoodix)
{
  uint8_t reg[2];
  reg[0] = isGoodix ? static_cast<uint8_t> (kGtRegCommand >> 8) : static_cast<uint8_t> (kFtRegMode);
  reg[1] = isGoodix ? static_cast<uint8_t> (kGtRegCommand & 0xff) : 0;
  const uint32_t regLen = isGoodix ? 2u : 1u;
  uint8_t one = 0;
  char why[48];
  snprintf (why, sizeof (why), "probe %s", name);
  if (!chipRead (address, reg, regLen, &one, 1, why)) {
    IOLog ("clover r181: %s no answer\n", name);
    return;
  }
  IOLog ("clover r181: %s ACK reg0=%02x\n", name, one);
  clover_input_observation[9] = address;
  clover_input_observation[10] = one;
  if (isGoodix) {
    // gtp_get_fw_info(): 6 bytes at 0x8140, twice, because a bus nobody has driven before can
    // produce a plausible first answer and the two have to agree before the name is trusted.
    uint8_t first[kGtVersionLen] = { 0 };
    uint8_t second[kGtVersionLen] = { 0 };
    reg[0] = static_cast<uint8_t> (kGtRegVersion >> 8);
    reg[1] = static_cast<uint8_t> (kGtRegVersion & 0xff);
    snprintf (why, sizeof (why), "goodix version");
    if (chipRead (address, reg, 2, first, kGtVersionLen, why)) {
      printBytes ("goodix 0x8140 a", first, kGtVersionLen);
      snprintf (why, sizeof (why), "goodix version b");
      if (chipRead (address, reg, 2, second, kGtVersionLen, why)) {
        printBytes ("goodix 0x8140 b", second, kGtVersionLen);
        // gtp_i2c_read_dbl_check's agreement count, and the vendor's own decode (gt9xx.c:1222-1258),
        // with its 8-byte buffer's [2..7] being this 6-byte data read's [0..5]. Both are the layer's,
        // so the 8-to-6 shift lives in exactly one expression.
        const uint32_t agree = input_bytes_agree (first, second, kGtVersionLen);
        struct InputGoodixId id;
        input_goodix_identify (first, kGtVersionLen, &id);
        IOLog ("clover r181: goodix agree=%u/6 pid=%c%c%c%c ver=%04x\n",
               agree, id.pid[0], id.pid[1], id.pid[2], id.pid[3], id.version);
        clover_input_observation[11] = id.version;
      }
    }
    uint8_t sensor = 0;
    reg[0] = static_cast<uint8_t> (kGtRegSensorId >> 8);
    reg[1] = static_cast<uint8_t> (kGtRegSensorId & 0xff);
    snprintf (why, sizeof (why), "goodix sensor_id");
    if (chipRead (address, reg, 2, &sensor, 1, why)) {
      IOLog ("clover r181: goodix sensor_id=%02x valid=%u\n", sensor,
             input_goodix_sensor_id_valid (sensor));
    }
    return;
  }
  // edt_ft5x06_ts_identify(): 22 bytes at 0xBB, "EP0" at [1] means M06 (a leading response byte),
  // at [0] means M12, anything else is a generic FT firmware.
  uint8_t name22[kFtNameBuf] = { 0 };
  reg[0] = static_cast<uint8_t> (kFtRegName);
  snprintf (why, sizeof (why), "ft name");
  if (chipRead (address, reg, 1, name22, kFtNameLen, why)) {
    printBytes ("ft 0xbb", name22, kFtNameLen);
    const bool m06 = input_ft_is_m06 (name22, kFtNameLen) != 0;
    IOLog ("clover r181: ft guess=%s\n", m06 ? "m06-ish" : "m12-or-generic");
    uint8_t fw[2] = { 0 };
    reg[0] = static_cast<uint8_t> (kFtRegFwVersion);
    snprintf (why, sizeof (why), "ft fw");
    if (chipRead (address, reg, 1, fw, 2, why)) {
      printBytes ("ft 0xa6", fw, 2);
    }
  }
}

// ------------------------------------------------------------------ the entry point
//
// Opt-in, silent when the boot argument is absent, and never fatal. The chain is
// sheng_pmgr_run() -> clover_emmc_run() -> this, so the eMMC report is always ahead of this one.
extern "C" void clover_input_run (void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn ("clover-input", &mode, sizeof (mode)) || !mode) {
    return;
  }
  clover_input_observation[0] = mode;
  if (mode != 1 && mode != 2 && mode != 3) {
    IOLog ("clover r181: mode %u is not implemented (1 dump, 2 bring-up, 3 probe)\n", mode);
    return;
  }
  refreshTicksPerMs ();

  // Map everything first. A window that is not in the platform's memory map is a payload-geometry
  // answer, not a hardware answer, and it has to be distinguishable from a dead bus -- so a
  // failure says which base and stops, instead of reporting the zeros a null read would give.
  struct Needed { Window *window; uint64_t phys; uint32_t size; const char *label; };
  const Needed needed[] = {
    { &gAhbHalt, kGccAhbHaltPhys, kGccWindowSize, "gcc-ahb-halt" },
    { &gAhbVote, kGccAhbVotePhys, kGccWindowSize, "gcc-ahb-vote" },
    { &gI2cClock, kGccI2cPhys, kGccWindowSize, "gcc-i2c" },
    { &gPinsI2c, kPinGpio10Phys, kPinPairSize, "tlmm-gpio10" },
    { &gPinsTouch, kPinGpio66Phys, kPinPairSize, "tlmm-gpio66" },
    { &gQup, kQupPhys, kQupSize, "qup3-i2c" },
  };
  for (uint32_t i = 0; i < sizeof (needed) / sizeof (needed[0]); i++) {
    if (!mapWindow (*needed[i].window, needed[i].phys, needed[i].size)) {
      IOLog ("clover r181: map %s %llx failed\n", needed[i].label,
             static_cast<unsigned long long> (needed[i].phys));
      clover_input_observation[1] = needed[i].phys;
      clover_input_observation[15] = kInputFailMap;
      return;
    }
  }
  // What the device tree says about this hardware, printed once so a reading can be compared with
  // the declaration rather than with a remembered number. These four are the constants the mapping
  // above deliberately did NOT use -- the QUP block is mapped a whole 4 KiB page where the DT
  // declares 0x600, and GCC's three registers live 0x3b000 apart inside a node the DT describes as
  // one 0x94000 range, so this line is the record of that gap. The interrupt is named but never
  // used: every transfer here is polled.
  IOLog ("clover r181: declared qup=%llx size=%u dt_size=%u gcc=%llx+%u intid=%u\n",
         static_cast<unsigned long long> (kQupPhys), kQupSize, kQupDtSize,
         static_cast<unsigned long long> (kGccPhys), kGccSize, kQupIntid);
  IOLog ("clover r181: mode %u windows up, tps=%llu\n", mode,
         static_cast<unsigned long long> (gTicksPerMs));

  dumpClocks ();
  dumpPin (10, "sda");
  dumpPin (11, "scl");
  dumpPin (66, "rst");
  dumpPin (67, "irq");
  dumpQup ("before");

  if (mode == 1) {
    // Read-only, as asked. Nothing above wrote a single byte.
    IOLog ("clover r181: mode 1 wrote nothing\n");
    clover_emmc_flush_cache ();
    return;
  }

  bool up = voteAhb ();
  if (up) {
    up = programRcg ();
  }
  if (up) {
    up = enableBranch ();
  }
  if (up) {
    programI2cPins ();
    up = bringUpCore ();
  }
  dumpClocks ();
  dumpQup ("after");
  if (!up) {
    IOLog ("clover r181: bring-up stopped at verdict=%llu, not probing\n",
           static_cast<unsigned long long> (clover_input_observation[15]));
    clover_emmc_flush_cache ();
    return;
  }

  if (mode == 2) {
    // A bring-up that got here says the clocks took and the core came out of reset. Whether the
    // FIFO sizes make sense is the last thing mode 2 can answer without driving the bus, so it is
    // said in words: out_fifo_sz came from the block/FIFO fields, and a zero would mean the
    // IO_MODE read was not the hardware's own size description.
    IOLog ("clover r181: bring-up ok fifo out=%u in=%u clk=%08x\n",
           gOutFifoSize, gInFifoSize, qupRead (kQupI2cClkCtl));
    clover_emmc_flush_cache ();
    return;
  }

  // Both candidates, before any reset: if the firmware left the panel released, this is the whole
  // answer, and a probe that needed a reset would have hidden it.
  probeCandidate (static_cast<uint8_t> (kGtAddr), kGtAddrName, true);
  probeCandidate (static_cast<uint8_t> (kFtAddr), kFtAddrName, false);
  if (!clover_input_observation[9]) {
    IOLog ("clover r181: neither address answered before reset, pulsing\n");
    goodixReset (static_cast<uint8_t> (kGtAddr));
    probeCandidate (static_cast<uint8_t> (kGtAddr), kGtAddrName, true);
    if (!clover_input_observation[9]) {
      probeCandidate (static_cast<uint8_t> (kFtAddr), kFtAddrName, false);
    }
  }
  if (!clover_input_observation[9]) {
    // Say what is still unproven rather than concluding the controller is missing: no rail has
    // been enabled (InputRegs.h: both DT touch nodes have supplies, this driver never touched
    // SPMI), and a bus whose SDA/SCL are held low by a chip mid-transfer answers nothing at all.
    IOLog ("clover r181: no address ACKed; rails unproven, sda=%08x scl=%08x status=%08x op=%08x\n",
           pinRead (10, kPinIo), pinRead (11, kPinIo), qupRead (kQupI2cStatus),
           qupRead (kQupOperational));
  }
  IOLog ("clover r181: done xfers=%u nack=%u timeout=%u state=%08x\n",
         gXfersRun, gXfersNack, gXfersTimeout, qupRead (kQupState));
  clover_input_observation[12] = gXfersRun;
  clover_input_observation[13] = gXfersTimeout;
  clover_input_observation[14] = gXfersNack;
  // The tee in CloverSdhci picks these lines out of msgbuf on the next block operation, so this
  // call is a courtesy, not the commit. Boot continues either way.
  clover_emmc_flush_cache ();
}
