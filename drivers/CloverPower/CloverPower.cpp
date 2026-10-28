// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the power instrument -- can this kernel ask a PMIC what state a rail
// is in, and what does the answer say about the touch controller's supplies?
//
// WHY THIS FILE EXISTS
//
// Schedule item 6 is power, and round181 is the reason it comes now. That driver asked the panel's
// two I2C addresses who they are and closed with "rails unproven" -- because both DT touch nodes
// declare supplies (clover.dts `focaltech@38` `vdd-supply`/`vcc_i2c-supply`, and the same pair on
// `gt9xx@5d`) and nothing in the port had ever read one. A NACK on I2C means "no chip at that
// address" only if the chip has power; without a rail reading, "the controller is missing" and
// "nobody turned it on" are the same observation, and a round that cannot separate them sends the
// next round after a phantom. So this driver opens the only door to a PMIC -- the SPMI arbiter --
// and reads, in order: the arbiter's own identity, the APID map the boot left behind, who owns each
// APID, the two PMICs' revid blocks, and the enable and voltage registers of the two rails in
// question.
//
// It is also the prerequisite for 排期 4's second gate. That gate is "may XNU switch a rail on at
// all", and the answer is not in the command encoding -- it is in `cnfg + 0x700 + 4*apid`, which
// names the execution environment allowed to write that APID (VEND:50-51, read at :920-922). If the
// number there is not 0, XNU -- EE 0, per DTB `qcom,ee = <0x00>` -- gets a denied transaction from
// hardware however correct its command word is, and the port has to ask RPM instead. This round
// produces that number before anyone writes anything.
//
// WHY THE TRANSACTION LOGIC IS NOT IN THIS FILE
//
// Because sheng proved that split pays for itself. ~/UniXNU-sheng/package/BootResetCore.h keeps its
// arbiter logic as inline functions over plain `volatile uint32_t *`, and
// package/tests/boot_reset_host.c drives those exact functions on a Mac against a software model of
// the arbiter with a device thread serving the channels; the model caught the ordering bugs -- which
// APID wins when two map to one PPID, what a denial looks like -- before any of it reached a PMIC
// that could not be un-broken. This round is read-only, so the mistakes it can make are reports that
// are wrong rather than rails that are off, and a wrong report is the worse of the two. So the APID
// walk, the ownership lookup and the bounded read live next door in PowerArb.h, where the host model
// can exercise them, and this file keeps only what genuinely needs the kernel: mapping, decoding,
// printing, and the observation array.
//
// WHY IT IS READ-ONLY, IN THE HARD SENSE
//
// Every transaction here is an EXT_READL on an *observer* channel. VEND:124-125 states the rule --
// from v2 on, rd_base is "obsrvr" and wr_base is "chnls" -- and :1227-1236 is the code that gets them
// by name, so a read cannot land on a PMIC input. The chnls window, which is where a v5 *write*
// command would go (VEND:1013), is mapped one page per rail APID solely to read that page's
// ACC_ENABLE and IRQ_STATUS words (VEND:1067-1069, :1085-1087 -- both defined as wr_base plus an
// offset), and no CMD word is ever written there, so no transaction can leave the arbiter toward a
// PMIC register. The one write in this driver is the command word that starts a read, into the
// observer channel's CMD, which is what a read IS: VEND:370 `pmic_arb_set_rd_cmd()` writes
// rd_base + PMIC_ARB_CMD. A rail that turns out to be off stays off; enabling it is the round after
// this one, and that round has to earn the ownership answer printed here first.
//
// THE THREE QUESTIONS, ONE PER BOOT ARGUMENT (clover-power=N)
//
// Mode 1 asks what the arbiter itself says, and starts no transaction. Map the four windows, read
// the version (VEND:1210 branches on exactly this word to choose its ops table), read core + 0x0004
// once and print it under BOTH decodings the sources give it -- PMIC_ARB_INT_EN in VEND:24, which
// that tree never reads, and PMIC_ARB_FEATURES in MAIN:34, whose bits 10:0 MAIN:1361-1380 treats as
// the APID count -- because the port cannot settle that argument from software and a report that
// shows the word both ways can. Then walk the APID map (core + 0x900, VEND:1113-1116) bounded by the
// core window's own size, which is what VEND:910-911 does, histogram the mapped entries by write
// owner, and list the ones belonging to this board's two PMICs. Nothing is written: not even the
// observer CMD.
//
// Mode 2 does mode 1 and asks the two PMICs who they are. MAIN's qcom-spmi-pmic.c reads the revid
// block as single-byte reads in the order type, subtype, rev2, minor, major (:169-198) plus a sixth
// for fab_id on the subtypes that have it; this driver asks the same span as one 5-byte burst first,
// which is both fewer transactions and the question every later multi-byte read has to have answered
// anyway -- and if the burst does not come back, the same five bytes are asked one at a time and the
// report says which path answered. A QPNP device must report 0x51 at +0x04 (MAIN:26
// PMIC_TYPE_VALUE), and MAIN refuses to name the chip if it does not; the subtype at +0x05 is
// compared with 0x1b for pm660 and 0x1a for pm660l (MAIN include/soc/qcom/qcom-spmi-pmic.h:35-36).
// The fab_id read is applied to the subtype the chip *reported*, not to the board's label, because
// that is MAIN's own condition (:193-197) and the DTB's `qcom,fab-id-valid` on a node whose MAIN
// branch is not taken is not a promise.
//
// Mode 3 does mode 2 and reads the two rails: the same 5-byte identity burst MAIN's
// spmi_regulator_match() uses at :1696 (0x01 dig major rev through 0x05 subtype -- the reading that
// proves the peripheral is the LDO MAIN's regulator table claims and not something else wearing that
// APID), a 2-byte voltage burst at 0x40 exactly as MAIN:942 makes it, then mode 0x45 and enable 0x46.
// The voltage is decoded as millivolts only when the subtype is one of MAIN's L660 parts (:147-153,
// all bound to the FTSMPS426 layout at :1623-1637); for any other subtype 0x40 is a range selector
// and 0x41 an index into a table this port has not proven, and printing microvolts then would be an
// invention. The enable byte is the answer this round exists to produce: 0x80 means the LDO is on
// (VEND:212-214), and round181's NACK with 0x00 in both rails means software, not silicon.
//
// WHY NOTHING HERE PANICS
//
// The reasoning round176 established and round181 applied: the kernel log reaches the host by itself
// (IOLog writes msgbuf, CloverSdhci's tee copies any line containing "clover r" into the card-backed
// kernel ring, scripts/read-clover-log.py --tee reads it off the card), so halting the boot buys
// nothing and costs whatever the rest of the boot would have printed. A power reading is worth more
// than a halt, and a boot that finds an empty APID map still reaches launchd with the emptiness
// recorded. Every wait here is bounded twice -- a poll count calibrated against the generic timer, and
// the arbiter's own status bits -- and every refusal has its own code.
//
// WHAT IT DOES NOT CLAIM
//
// No clocks. The DTB's arbiter node declares none, VEND's probe requests none, and MAIN's read path
// asks for none, so mode 1 tests the assumption by reading core and reporting what it gets rather
// than inventing a GCC sequence to vote something that may not exist. No voltage setting, no enable
// write, no mode write, no pull-down, no soft-start, no RPM protocol -- even though this board's DT
// names both rails by their RPM identifiers (`rpm-regulator-ldoa11` at DTB:5236 with
// `qcom,resource-id = <0x0b>`, `rpm-regulator-ldob3` at :5539 with `<0x03>`), which is itself evidence
// that on the shipping product these LDOs are managed through RPM rather than by direct SPMI register
// writes. That is a hypothesis the ownership reading can be checked against, not a conclusion printed
// here. No interrupt programming either: ACC_ENABLE, IRQ_STATUS and the per-EE owner-access word are
// read and reported, and none of them is touched.
//
// WHY IT RUNS FROM sheng_pmgr_run()
//
// Same reason as CloverSdhci and CloverInput: a device repository has no XNU patch slot for an IOKit
// personality, so a board driver exports a C entry point and CloverPMGR calls it from the bsd_init
// hook. It runs after clover_input_run(), so the three instruments cannot collide at one schedule
// point and the touch report is always upstream of this one: when this round prints both enables as
// 0x00, round181's "no address ACKed" stops meaning "no controller" and starts meaning "nobody turned
// it on" -- which is the pair of facts the next round is built from.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "PowerArb.h"

extern "C" {
// drivers/CloverSdhci's commit of both log rings to the card. At this scheduling point the block
// device may not exist yet, and then this returns 0 without touching the card -- the tee collects
// these lines on the next block operation anyway. It is called on every exit path so that a later
// round, running after bring-up, gets a real commit for free.
uint32_t clover_emmc_flush_cache (void);
}

// ------------------------------------------------------------------ the windows
//
// Four are fixed and mapped before anything is read: the arbiter's core, its cnfg (where the
// ownership table lives), this EE's observer block, and the first page of the interrupt window for
// this EE. A fifth family is mapped later -- one page per rail APID -- because the channel window is
// 16 MiB and there is no reason to map all of it in order to read two registers of two channels.
struct Window
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t base = 0;
  uint64_t phys = 0;
  uint32_t size = 0;
};

static Window gCore;                          // 0x0800f000 +0x1000: version, +0x900 APID map
static Window gCnfg;                          // 0x0800a000 +0x3000: ownership table at +0x700
static Window gObs;                           // 0x09400000 +0x10000: EE 0's observer block
static Window gIntr;                          // 0x0a400000 +0x1000: EE 0's owner-access words
static Window gChnl[kPmicArbRailCount];       // one page per rail APID: ACC_ENABLE, IRQ_STATUS
static uint32_t gChnlApid[kPmicArbRailCount];
static uint32_t gChnlCount = 0;

// The three pointers the pure core works on. Everything transactional goes through this struct, so
// the same pa_read() that the host model tests is the one running here.
static struct pa_arbiter gArb;

// Same three steps CloverSdhci has used since round87 and CloverInput since round181: ask the
// platform for the range, map it uncached and anywhere, keep only the virtual address. A range the
// platform memory map does not have returns null rather than a wrong pointer.
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

// ------------------------------------------------------------------ time, bounded
//
// The generic timer as the other two board drivers use it: the frequency read sanity-checked against
// 1 MHz..1 GHz with the board's 19.2 MHz XO as the fallback, and every wait carrying a deadline in
// ticks. sheng_pmgr_run() runs about ten seconds into a boot whose timer may not be serving anything
// yet, and round87 proved that a hang inside a driver at this point produces no panic at all -- so a
// bound that reports is the only way a stuck DONE bit becomes a reading instead of a dead boot.
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

static uint64_t gTicksPerMs = 19200ULL;   // the XO default; refreshed at entry

static void refreshTicksPerMs (void)
{
  const uint64_t frequency = counterFrequency ();
  if (frequency >= 1000000ULL && frequency <= 1000000000ULL) {
    gTicksPerMs = frequency / 1000ULL;
  }
}

// ------------------------------------------------------------------ what survives the screen
//
// The raw observations, kept for the same reason CloverSdhci keeps its own and CloverInput keeps
// clover_input_observation: a number that outlives a truncated ring line and can be decoded offline
// against this file. Slots:
//   [0]  mode, [1] the phys of a window that failed to map (0 when all four mapped),
//   [2]  arbiter version, [3] mapped APID entries in the core map,
//   [4]  pm660 revid type (0x51 expected), [5] pm660 subtype (0x1b expected),
//   [6]  pm660l revid type, [7] pm660l subtype (0x1a expected),
//   [8]  A11 enable byte (0x80 = on), [9] B3 enable byte,
//   [10] A11 decoded uV (0 = not decoded), [11] B3 decoded uV,
//   [12] A11's APID (0xffff = none found), [13] B3's APID,
//   [14] transactions run in the high half, transactions that failed in the low half,
//   [15] the last verdict code (see the note at each site).
volatile uint64_t clover_power_observation[16];

// One word per way a step can end, so the ring line and the observation array cannot disagree about
// which stage stopped. Positive values are steps; a failure stores step*1000 + fail, the shape
// CloverInput uses.
static const uint64_t kStepCalibrate = 1, kStepScan = 2, kStepAcc = 3, kStepRevid = 4,
                      kStepRail = 5, kStepComplete = 9;
static const uint64_t kFailMap = 100, kFailVersion = 101, kFailNoEntry = 102, kFailNoApid = 103,
                      kFailPoll = 104, kFailFailed = 105, kFailDenied = 106, kFailDropped = 107,
                      kFailLen = 108, kFailApidRange = 109, kFailType = 110;

// The bound on one transaction's wait, measured rather than guessed. VEND:93 spends
// PMIC_ARB_TIMEOUT_US (1000 microseconds) as a thousand udelay(1) iterations, which needs a timer
// this boot may not have; SHENG:29 bounds by 100000 plain polls instead, which needs nothing but
// does not know how long it actually waited. Doing only the first would make the timeout a function
// of a timer nobody has checked yet, and only the second would let a stuck bit eat the boot. So: time
// a burst of core reads -- the same class of access as a STATUS poll, a Device-memory read of the same
// peripheral -- scale that by kPmicArbDeadlineMs, and clamp into kPmicArbPolls.
static uint32_t gPolls = kPmicArbPolls;

static uint32_t calibratePolls (void)
{
  const uint32_t trials = 2048;
  const uint64_t start = readCounter ();
  for (uint32_t i = 0; i < trials; i++) {
    (void) windowRead (gCore, kPmicArbVersion);
  }
  const uint64_t elapsed = readCounter () - start;
  const uint64_t budget = static_cast<uint64_t> (kPmicArbDeadlineMs) * gTicksPerMs;
  uint64_t polls = elapsed ? (static_cast<uint64_t> (trials) * budget) / elapsed : kPmicArbPolls;
  if (polls < 64) {
    polls = 64;            // never so tight that a working transaction looks broken
  }
  if (polls > kPmicArbPolls) {
    polls = kPmicArbPolls; // never so loose that a stuck bit outlives the boot's budget
  }
  gPolls = static_cast<uint32_t> (polls);
  IOLog ("clover r182: calibrate %u core reads=%llu ticks, budget=%ums -> polls=%u\n",
         trials, static_cast<unsigned long long> (elapsed), kPmicArbDeadlineMs, gPolls);
  clover_power_observation[15] = kStepCalibrate;
  return gPolls;
}

// ------------------------------------------------------------------ the transactions
static uint32_t gXactRun = 0;
static uint32_t gXactFail = 0;
static uint32_t gXactTimeout = 0;
static uint32_t gXactDenied = 0;

// Printable bytes, hex and ASCII side by side: the reading that decides the next round is a number
// with a name attached (a subtype, an enable byte, a millivolt count), and a ring dump has no way to
// ask a follow-up question. Eight bytes is the longest transaction this driver asks.
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
  IOLog ("clover r182: %s n=%u hex=%s ascii=[%s]\n", label, len, hex, text);
}

// A pa_* code in words. "rc=5" and "rc=7" are the difference between "the bus is slow" and "somebody
// else owns this peripheral", and only one of those is fixed by better code -- so the name travels
// with the number.
static const char *paCodeName (int rc)
{
  switch (rc) {
    case PA_E_VERSION: return "version";
    case PA_E_NOENTRY: return "no-map-entry";
    case PA_E_PPID:    return "no-apid-for-ppid";
    case PA_E_LEN:     return "bad-length";
    case PA_E_POLL:    return "no-done";
    case PA_E_FAILED:  return "failure";
    case PA_E_DENIED:  return "denied";
    case PA_E_DROPPED: return "dropped";
    case PA_E_APID:    return "apid-out-of-range";
    default:           return "ok";
  }
}

static uint64_t paCodeToFail (int rc)
{
  switch (rc) {
    case PA_E_VERSION: return kFailVersion;
    case PA_E_NOENTRY: return kFailNoEntry;
    case PA_E_PPID:    return kFailNoApid;
    case PA_E_LEN:     return kFailLen;
    case PA_E_POLL:    return kFailPoll;
    case PA_E_FAILED:  return kFailFailed;
    case PA_E_DENIED:  return kFailDenied;
    case PA_E_DROPPED: return kFailDropped;
    case PA_E_APID:    return kFailApidRange;
    default:           return 0;
  }
}

// One read, asked of one APID's observer channel, with the accounting an instrument needs: the
// transaction is counted, and only a failure prints a line -- because a success is already visible as
// the bytes the caller prints next, and a run that succeeds would otherwise double the report. The
// failure line carries the STATUS word and the command word the channel was left holding, which
// between them say whether the arbiter saw the request at all.
static int readAt (const char *why, uint64_t step, uint32_t apid, uint32_t byte_addr,
                   uint8_t *buf, uint32_t len)
{
  uint32_t status = 0;
  gXactRun++;
  const int rc = pa_read (&gArb, apid, byte_addr, buf, len, gPolls, &status);
  if (rc) {
    gXactFail++;
    gXactTimeout += rc == PA_E_POLL ? 1u : 0u;
    gXactDenied += rc == PA_E_DENIED ? 1u : 0u;
    // The command word the channel was left holding is the difference between "the arbiter never saw
    // the request" (whatever the boot had there) and "it saw it and the PMIC refused" (this round's
    // encoding), so it is read back and printed. Bounded because the offset is only inside the mapped
    // observer block while apid < 512 channels of 0x80, which kPmicArbMapEntries already guarantees --
    // the check is here so a later round with a bigger window cannot read off it by accident.
    const uint32_t cmdWord = apid < kSpmiObsBlockMapSize / kPmicArbObsApidStride ?
        windowRead (gObs, kPmicArbObsApidStride * apid + kPmicArbCmd) : 0u;
    IOLog ("clover r182: read %s apid=%u addr=0x%02x len=%u rc=%d(%s) status=%08x cmd=%08x\n",
           why, apid, byte_addr & 0xff, len, rc, paCodeName (rc), status, cmdWord);
    clover_power_observation[15] = step * 1000 + paCodeToFail (rc);
  }
  return rc;
}

// ------------------------------------------------------------------ mode 1: what is the arbiter?
//
// The version decides whether anything below it is meaningful, so it is said first. VEND:1210
// branches on this word to install its ops table; a value outside the v5 band would mean PowerRegs.h
// describes the wrong floor plan for this silicon, and every offset printed afterwards would be
// confidently wrong rather than wrong in an interesting way.
//
// The three extra words on the first line are the *first* register of three of the mapped windows --
// cnfg + 0x700 is APID 0's owner word, obs + 0x00 is APID 0's command word, intr + 0x00 is APID 0's
// owner-access word -- which is the cheapest way to show that all three windows answer, in one line,
// before any transaction is attempted.
static void dumpVersion (void)
{
  const uint32_t version = windowRead (gCore, kPmicArbVersion);
  const uint32_t word0004 = windowRead (gCore, kPmicArbIntEn);
  clover_power_observation[2] = version;
  IOLog ("clover r182: core=%08x ee=%u chan=%u intid=%u | cnfg700=%08x obs00=%08x intr00=%08x\n",
         version, kPmicArbEe, kPmicArbChannel, kPmicArbPeriphIntid,
         windowRead (gCnfg, kPmicArbOwnerTable), windowRead (gObs, kPmicArbCmd),
         windowRead (gIntr, kPmicArbOwnerAccStatus));
  // One word, two names, two decodings, printed side by side. VEND:24 calls core + 0x0004
  // PMIC_ARB_INT_EN and never reads it; MAIN:34 calls the same address PMIC_ARB_FEATURES and its v5
  // init (:1361-1380) takes bits 10:0 as the APID count. Both cannot be true of the same bits, the
  // port cannot settle it from software, and a reader of a ring dump can settle it in one look if all
  // three numbers are here. Picking one silently would hide the disagreement from the next round,
  // which is the one thing a reading must not do.
  IOLog ("clover r182: core+0004=%08x vend_int_en=%u main_apids=%u (mask %x)\n",
         word0004, word0004, word0004 & kPmicArbFeaturesApidMask, kPmicArbFeaturesApidMask);
  if (version < kPmicArbVersionV5Min) {
    IOLog ("clover r182: below V5_MIN %08x: map base would be v2/v3's 0x800, not 0x900\n",
           kPmicArbVersionV5Min);
  } else if (version >= kPmicArbVersionV6Min) {
    IOLog ("clover r182: at or above V6_MIN %08x: not the v5 floor plan (SHENG's v7 is 0x2000/0x00)\n",
           kPmicArbVersionV6Min);
  }
}

// The map, in the only form that answers the question a blank table leaves open -- not "is it empty"
// but "what is in it". Every mapped APID whose PPID belongs to either PMIC this board has (sid 0 or
// sid 2) is listed eight to a line, bounded at 64 entries, which keeps a fully populated table to
// eight lines while still showing whether the pm660 peripherals got APIDs at all. The owner digit
// travels with each PPID because the interesting failure is a populated table owned entirely by an EE
// that is not this one. The line is built with an explicit cursor rather than strlen(), because
// snprintf() returns the length it *would* have written and a cursor taken naively from it can walk
// past the end of a buffer that truncated.
static void dumpMapListing (uint32_t *listedOut, uint32_t *skippedOut)
{
  // "%03x/%u" is at most 8 bytes with its separator, so 96 bytes cannot truncate eight entries and the
  // cursor below stays honest.
  char line[8 * 9 + 8];
  uint32_t inLine = 0, first = 0, listed = 0, lines = 0, used = 0;
  *skippedOut = 0;
  IOLog ("clover r182: map entries of sid 0 and sid 2 as ppid/owner:\n");
  for (uint32_t i = 0; i < kPmicArbMapEntries && listed < 64; i++) {
    const uint32_t map = windowRead (gCore, kPmicArbApidMap + kPmicArbApidMapStride * i);
    if (!map) {
      continue;   // VEND:915-916
    }
    const uint32_t ppid = pa_ppid_of (map);
    const uint32_t sid = ppid >> 8;
    if (sid != kSidPm660 && sid != kSidPm660l) {
      (*skippedOut)++;
      continue;
    }
    if (!inLine) {
      first = i;
      used = 0;
      line[0] = '\0';
    }
    const uint32_t owner = windowRead (gCnfg, kPmicArbOwnerTable + kPmicArbOwnerStride * i) &
                           kPmicArbOwnerMask;
    listed++;
    inLine++;
    used += static_cast<uint32_t> (snprintf (line + used, sizeof (line) - used, "%s%03x/%u",
                                            used ? " " : "", ppid, owner));
    if (inLine == 8 || used > sizeof (line) - 16) {
      IOLog ("clover r182:   apid %03u-%03u %s\n", first, i, line);
      lines++;
      inLine = 0;
    }
  }
  if (inLine) {
    IOLog ("clover r182:   apid %03u-.. %s\n", first, line);
    lines++;
  }
  IOLog ("clover r182: listing lines=%u shown=%u other_sid=%u bound=%u\n",
         lines, listed, *skippedOut, static_cast<unsigned> (kPmicArbMapEntries));
  *listedOut = listed;
}

// One walk of the map for one PPID, reported in full. pa_scan_apids() implements the vendor driver's
// own selection rule (VEND:930-932: the first mapping wins unless a later one is owned by this EE),
// so what is printed is the vendor's answer plus the statistics that make the answer checkable: how
// many APIDs mapped to the PPID, which one was chosen, who owns it, whether it claims the interrupts,
// and how the whole table splits across the six execution environments.
static struct pa_scan gScan;

static int scanFor (uint32_t ppid, const char *label)
{
  const int rc = pa_scan_apids (&gArb, ppid, &gScan);
  IOLog ("clover r182: scan %s ppid=0x%03x rc=%d(%s) ver=%08x words=%u mapped=%u span=%u\n",
         label, ppid, rc, paCodeName (rc), gScan.version, gScan.words, gScan.mapped, gScan.apid_span);
  IOLog ("clover r182: scan %s apid=%d owner=%u irq_owner=%u candidates=%u irq_apid=%d ee0=%u ee1=%u ee2=%u ee3=%u ee4=%u ee5=%u\n",
         label, gScan.apid, gScan.owner, gScan.irq_owner, gScan.candidates, gScan.irq_apid,
         gScan.per_ee[0], gScan.per_ee[1], gScan.per_ee[2], gScan.per_ee[3], gScan.per_ee[4],
         gScan.per_ee[5]);
  if (gScan.apid < 0) {
    // The most useful failure this round can produce: the boot never gave this peripheral an APID, so
    // no command encoding can reach it and the rail is unproven rather than off. It has to be sayable
    // in words, because a reader of a ring cannot tell it apart from a transaction that timed out.
    IOLog ("clover r182: %s ppid=0x%03x has no APID; not reachable this boot\n", label, ppid);
  } else if (gScan.owner != gArb.ee) {
    // The 排期 4 gate, named. Reads are unaffected -- the observer path does not ask the write owner's
    // permission -- but a write with this APID is what VEND:1008-1011 turns into -EPERM before it ever
    // reaches the bus.
    IOLog ("clover r182: %s apid=%d write-owner is EE %u, this EE is %u: reads ok, writes denied\n",
           label, gScan.apid, gScan.owner, gArb.ee);
  }
  if (rc == PA_OK) {
    clover_power_observation[15] = kStepScan;
  }
  return rc;
}

// The per-EE access status and the per-channel interrupt state, read and not programmed. These are the
// registers that would have to be set before a PMIC interrupt could reach a handler, so what the boot
// left in them is the starting condition of that round -- and `owner_acc_status` for this EE is a
// second, independent answer to "may we use this channel", which is worth having next to the
// ownership table rather than instead of it.
static void dumpAccessState (int apid, const char *label)
{
  // The APID has to be one the core window's map can hold before its owner-access word is inside the
  // one page of the interrupt window that was mapped (0x4 per APID, 0x1000 mapped, so 448 is the
  // largest index with a word in it -- which is kPmicArbMapEntries exactly).
  if (apid < 0 || static_cast<uint32_t> (apid) >= kPmicArbMapEntries) {
    return;
  }
  const uint32_t ch = static_cast<uint32_t> (apid);
  const uint32_t ownerAcc = windowRead (gIntr, kPmicArbOwnerAccStatus +
                                               kPmicArbOwnerAccStride * ch);
  IOLog ("clover r182: %s apid=%u intr+0x%x owner_acc=%08x\n", label, ch,
         kPmicArbOwnerAccStatus + kPmicArbOwnerAccStride * ch, ownerAcc);
  for (uint32_t i = 0; i < gChnlCount; i++) {
    if (gChnlApid[i] != ch) {
      continue;
    }
    IOLog ("clover r182: %s apid=%u chn=%llx acc=%08x irq=%08x clr=%08x\n", label, ch,
           static_cast<unsigned long long> (kSpmiChnlsPhys +
               static_cast<uint64_t> (kPmicArbChnlApidStride) * ch),
           windowRead (gChnl[i], kPmicArbAccEnable),
           windowRead (gChnl[i], kPmicArbIrqStatus),
           windowRead (gChnl[i], kPmicArbIrqClear));
    clover_power_observation[15] = kStepAcc;
    return;
  }
  IOLog ("clover r182: %s apid=%u chnls page not mapped, acc/irq unread\n", label, ch);
}

// ------------------------------------------------------------------ mode 2: who are the PMICs?
//
// The revid registers are absolute addresses in MAIN (0x101..0x105, 0x1f2 inside peripheral 0x100). The
// arbiter command carries only the low byte, because the APID has already selected the peripheral
// (VEND:1025-1028), so each is passed through `& 0xff` and pa_read masks it again -- which is why the
// same transaction reaches 0x101 and 0x1f2 with one formula.
static void readRevid (const char *label, int apid, uint32_t expectedSubtype,
                       uint64_t typeSlot, uint64_t subtypeSlot)
{
  uint8_t burst[5] = { 0 };
  uint8_t single[5] = { 0 };
  uint32_t values[5] = { 0 };
  const uint32_t ch = static_cast<uint32_t> (apid);

  if (readAt ("revid burst", kStepRevid, ch, kPmicRev2 & 0xff, burst, 5) == PA_OK) {
    for (uint32_t i = 0; i < 5; i++) {
      values[i] = burst[i];
    }
    printBytes (label, burst, 5);
  } else {
    // MAIN's own shape: the same five addresses, one byte at a time. If this succeeds where the burst
    // failed, the board answers single reads and every later round has to build its reads that way --
    // and the report says which of the two happened instead of quietly using whichever worked.
    IOLog ("clover r182: %s burst failed, asking the five singly (MAIN's shape)\n", label);
    uint32_t ok = 0;
    for (uint32_t i = 0; i < 5; i++) {
      uint8_t one = 0;
      char why[32];
      snprintf (why, sizeof (why), "revid byte %u", i);
      if (readAt (why, kStepRevid, ch, (kPmicRev2 + i) & 0xff, &one, 1) == PA_OK) {
        single[i] = one;
        values[i] = one;
        ok++;
      }
    }
    if (ok != 5) {
      IOLog ("clover r182: %s identity unknown, %u/5 single reads answered\n", label, ok);
      return;
    }
    printBytes (label, single, 5);
    IOLog ("clover r182: %s burst no, singles yes: sequential reads unproven on this arbiter\n",
           label);
  }
  // Burst and singles share the index order the addresses are in: 0 = +0x01 rev2, 1 = +0x02 minor
  // (MAIN's PMIC_REV3), 2 = +0x03 major (PMIC_REV4), 3 = +0x04 type, 4 = +0x05 subtype. MAIN checks
  // type FIRST and refuses to name the chip if it is not 0x51 (qcom-spmi-pmic.c:171-174), so the
  // refusal is the headline here rather than a footnote under the bytes.
  const uint32_t type = values[3];
  const uint32_t subtype = values[4];
  clover_power_observation[typeSlot] = type;
  clover_power_observation[subtypeSlot] = subtype;
  IOLog ("clover r182: %s rev2=%u minor=%u major=%u type=%02x want=%02x subtype=%02x want=%02x\n",
         label, values[0], values[1], values[2], type, kPmicTypeValue, subtype, expectedSubtype);
  if (type != kPmicTypeValue) {
    IOLog ("clover r182: %s type is not PMIC_TYPE_VALUE; identity NOT established\n", label);
    clover_power_observation[15] = kStepRevid * 1000 + kFailType;
    return;
  }
  IOLog ("clover r182: %s is QPNP, subtype %s the DTB\n", label,
         subtype == expectedSubtype ? "matches" : "does not match");
  clover_power_observation[15] = kStepRevid;
  // MAIN:193-197 reads fab_id only when the subtype is PM660's or PMI8998's. Applied here to what the
  // chip reported rather than to what the board is supposed to have, so a pm660l answering with the
  // pm660 subtype still gets the read, and the DTB's `qcom,fab-id-valid` on a node whose MAIN branch
  // is not taken is not treated as a promise.
  if (subtype == kPm660Subtype || subtype == 0x15 /* PMI8998, MAIN:32, not this board */) {
    uint8_t fab = 0;
    if (readAt ("fab_id", kStepRevid, ch, kPmicFabId & 0xff, &fab, 1) == PA_OK) {
      IOLog ("clover r182: %s fab_id=%02x (MAIN reads it for PM660/PMI8998 subtypes)\n", label, fab);
    }
  } else {
    IOLog ("clover r182: %s subtype %02x: MAIN does not read fab_id, so neither does this report\n",
           label, subtype);
  }
}

// ------------------------------------------------------------------ mode 3: is the rail on?
//
// One rail, five questions, in the order the drivers that work ask them. Identity first, because it is
// MAIN's own gate (spmi_regulator_match(), :1688-1709): a peripheral whose 0x04 is not 0x04 is not an
// LDO, and every number after that would be read in the wrong layout. Voltage second, as the 2-byte
// burst at 0x40 MAIN:942 makes. Mode and enable last, one byte each -- the enable byte is the answer
// this round exists to produce, so a voltage read must not be the thing that stops the report.
static bool isL660Subtype (uint32_t subtype)
{
  return subtype == kLdoSubtypeL660N300St || subtype == kLdoSubtypeL660N600St ||
         subtype == kLdoSubtypeL660P50 || subtype == kLdoSubtypeL660P150 ||
         subtype == kLdoSubtypeL660P600 || subtype == kLdoSubtypeL660Lvp150 ||
         subtype == kLdoSubtypeL660Lvp600;
}

struct Rail
{
  const char *label;
  uint32_t ppid;
  uint32_t minUv;          // what the DTB declares, for the comparison line
  uint32_t maxUv;
  uint64_t enableSlot;
  uint64_t uvSlot;
};

static const struct Rail kRails[kPmicArbRailCount] = {
  { "ldo-a11-vcc_i2c", kPpidLdoA11, kLdoA11MinUv, kLdoA11MaxUv, 8, 10 },
  { "ldo-b3-vdd",      kPpidLdoB3,  kLdoB3FixedUv, kLdoB3FixedUv, 9, 11 },
};

static void readRail (const struct Rail &rail, int apid)
{
  uint8_t id[5] = { 0 };
  const uint32_t ch = static_cast<uint32_t> (apid);
  if (readAt ("ldo identity", kStepRail, ch, kLdoRegDigMajorRev, id, 5) != PA_OK) {
    return;
  }
  printBytes (rail.label, id, 5);
  const uint32_t type = id[3];
  const uint32_t subtype = id[4];
  IOLog ("clover r182: %s dig_major=%u type=%02x want=%02x subtype=%02x\n",
         rail.label, id[0], type, kLdoTypeLdo, subtype);
  if (type != kLdoTypeLdo) {
    IOLog ("clover r182: %s is not an LDO at 0x04; stopping before any decoding\n", rail.label);
    clover_power_observation[15] = kStepRail * 1000 + kFailType;
    return;
  }
  const bool l660 = isL660Subtype (subtype);

  uint8_t volts[2] = { 0 };
  if (readAt ("ldo voltage", kStepRail, ch, kLdoVoltageLsb, volts, 2) == PA_OK) {
    printBytes (rail.label, volts, 2);
    const uint32_t raw = (static_cast<uint32_t> (volts[1]) << 8) | volts[0];
    IOLog ("clover r182: %s 0x40/0x41 raw=%u dt=%u-%u\n", rail.label, raw, rail.minUv, rail.maxUv);
    if (l660) {
      // MAIN:942-947 takes this pair as an absolute millivolt count, and only turns it into a
      // regulator selector by subtracting ftsmps426's set-point floor. That floor and the table's
      // 1352000 uV maximum (MAIN:537-539) sit BELOW the DTB's declared 1780000-1950000 uV for A11 and
      // well below B3's fixed 3000000 uV, so MAIN's selector arithmetic would be meaningless for these
      // two rails and this report deliberately skips it: the millivolts are multiplied out and
      // compared with the board's own tree, which is the only comparison available.
      const uint32_t uv = raw * 1000;
      clover_power_observation[rail.uvSlot] = uv;
      IOLog ("clover r182: %s uv=%u (ftsmps426 layout, subtype %02x) in_dt=%u\n",
             rail.label, uv, subtype, (uv >= rail.minUv && uv <= rail.maxUv) ? 1u : 0u);
    } else {
      // Not one of MAIN's L660 parts, so 0x40 is a range selector and 0x41 an index into a table this
      // port has not proven for this chip. The bytes are the reading; anything else would be a guess
      // wearing a unit.
      IOLog ("clover r182: %s subtype %02x is not an L660 part: raw bytes only, no uV claim\n",
             rail.label, subtype);
    }
  }

  uint8_t mode = 0;
  if (readAt ("ldo mode", kStepRail, ch, kLdoRegMode, &mode, 1) == PA_OK) {
    IOLog ("clover r182: %s mode=%02x hpm=%u auto=%u bypass=%u awake=%u en_pin=%x\n",
           rail.label, mode, (mode & kLdoModeHpm) ? 1u : 0u, (mode & kLdoModeAuto) ? 1u : 0u,
           (mode & kLdoModeBypass) ? 1u : 0u, (mode & kLdoModeFollowAwake) ? 1u : 0u,
           mode & (kLdoEnableFollowEn0 | kLdoEnableFollowEn1 | kLdoEnableFollowEn2 |
                   kLdoEnableFollowEn3));
  }

  uint8_t enable = 0;
  if (readAt ("ldo enable", kStepRail, ch, kLdoRegEnable, &enable, 1) == PA_OK) {
    clover_power_observation[rail.enableSlot] = enable;
    // The line this whole round is for. 0x80 is "the LDO is on" (VEND:212-214, MAIN:257-259); the low
    // nibble is which hardware enable pin it follows (VEND:215-219), and a non-zero nibble on a rail
    // that reads off means a pin or RPM is deciding its state rather than software. Read together with
    // round181: OFF here and NACK there is a boot argument away from a working digitizer.
    IOLog ("clover r182: %s enable=%02x state=%s follow=%02x\n", rail.label, enable,
           (enable & kLdoEnableMask) ? "ON" : "OFF",
           enable & (kLdoEnableFollowEn0 | kLdoEnableFollowEn1 | kLdoEnableFollowEn2 |
                     kLdoEnableFollowEn3));
  }
  clover_power_observation[15] = kStepRail;
}

// ------------------------------------------------------------------ the entry point
//
// Opt-in, silent without the boot argument, and never fatal. The chain is sheng_pmgr_run() ->
// clover_emmc_run() -> clover_input_run() -> this, so the touch report is always ahead of this one and
// a rails-off answer lands immediately after the "no address ACKed" line it explains.
extern "C" void clover_power_run (void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn ("clover-power", &mode, sizeof (mode)) || !mode) {
    return;
  }
  clover_power_observation[0] = mode;
  if (mode != 1 && mode != 2 && mode != 3) {
    IOLog ("clover r182: mode %u is not implemented (1 arbiter, 2 revid, 3 rails)\n", mode);
    return;
  }
  refreshTicksPerMs ();

  // Map everything first, and say which window failed if one does. A base the platform memory map does
  // not contain is a payload-geometry answer, not a hardware answer, and it has to be distinguishable
  // from an arbiter that reads back zeros -- so this stops with the failing phys recorded rather than
  // reporting a version of 00000000 as though the hardware had said it.
  struct Needed { Window *window; uint64_t phys; uint32_t size; const char *label; };
  const Needed needed[] = {
    { &gCore, kSpmiCorePhys, kSpmiMapSize, "spmi-core" },
    { &gCnfg, kSpmiCnfgPhys, kSpmiCnfgSize, "spmi-cnfg" },
    { &gObs, pa_obs_block_phys (kPmicArbEe), kSpmiObsBlockMapSize, "spmi-obs-ee0" },
    { &gIntr, kSpmiIntrPhys + kPmicArbOwnerAccEeStride * kPmicArbEe, kSpmiMapSize, "spmi-intr-ee0" },
  };
  for (uint32_t i = 0; i < sizeof (needed) / sizeof (needed[0]); i++) {
    if (!mapWindow (*needed[i].window, needed[i].phys, needed[i].size)) {
      IOLog ("clover r182: map %s %llx failed\n", needed[i].label,
             static_cast<unsigned long long> (needed[i].phys));
      clover_power_observation[1] = needed[i].phys;
      clover_power_observation[15] = kFailMap;
      clover_emmc_flush_cache ();
      return;
    }
  }
  gArb.core = reinterpret_cast<volatile uint32_t *> (gCore.base);
  gArb.cnfg = reinterpret_cast<volatile uint32_t *> (gCnfg.base);
  gArb.obs = reinterpret_cast<volatile uint32_t *> (gObs.base);
  gArb.ee = kPmicArbEe;

  // What the device tree declares, printed once so a reading can be compared with the declaration
  // rather than with a remembered number. The five spans are the DT's own (clover.dts:2267-2268); this
  // mapping used 0x1000 of core, all 0x3000 of cnfg, one 0x10000 block of the 0x1000000 observer window
  // and one 0x1000 page of the 0x220000 interrupt window, and this line is the record of that gap. The
  // channel window is named here and mapped per rail APID below.
  IOLog ("clover r182: declared core=%llx+%u chnls=%llx+%llu obsrvr=%llx+%llu intr=%llx+%llu cnfg=%llx+%u\n",
         static_cast<unsigned long long> (kSpmiCorePhys), kSpmiCoreSize,
         static_cast<unsigned long long> (kSpmiChnlsPhys),
         static_cast<unsigned long long> (kSpmiChnlsSize),
         static_cast<unsigned long long> (kSpmiObsrvrPhys),
         static_cast<unsigned long long> (kSpmiObsrvrSize),
         static_cast<unsigned long long> (kSpmiIntrPhys),
         static_cast<unsigned long long> (kSpmiIntrSize),
         static_cast<unsigned long long> (kSpmiCnfgPhys), kSpmiCnfgSize);
  const uint32_t polls = calibratePolls ();
  IOLog ("clover r182: mode %u windows up, tps=%llu polls=%u\n", mode,
         static_cast<unsigned long long> (gTicksPerMs), polls);

  dumpVersion ();

  // The first scan is also the version check: pa_scan_apids() refuses anything outside the v5 band
  // before it walks, because a v7 table would be read at a v2 address and produce a confidently wrong
  // APID number rather than an obviously nonsense one.
  struct pa_scan revid0, revidL, a11, b3;
  const int rc0 = scanFor (kPpidPm660Revid, "pm660-revid");
  revid0 = gScan;
  clover_power_observation[3] = revid0.mapped;
  if (rc0 == PA_E_VERSION) {
    IOLog ("clover r182: not a v5 arbiter (%08x); stopping, the map layout is unknown\n",
           revid0.version);
    clover_power_observation[15] = kStepScan * 1000 + kFailVersion;
    clover_emmc_flush_cache ();
    return;
  }

  const int rcL = scanFor (kPpidPm660lRevid, "pm660l-revid");
  revidL = gScan;
  const int rcA = scanFor (kPpidLdoA11, "ldo-a11");
  a11 = gScan;
  const int rcB = scanFor (kPpidLdoB3, "ldo-b3");
  b3 = gScan;
  clover_power_observation[12] = rcA == PA_OK ? static_cast<uint64_t> (a11.apid) : 0xffffULL;
  clover_power_observation[13] = rcB == PA_OK ? static_cast<uint64_t> (b3.apid) : 0xffffULL;

  // The listing is the diagnostic for the case the four scans above cannot describe: a table that is
  // populated but does not contain what this board's DT says it should. Printed when anything is
  // missing (and always in mode 1, which exists to describe the table), skipped when all four targets
  // resolved, because then the answer is already in the four scan lines.
  uint32_t listed = 0, skipped = 0;
  if (mode == 1 || rc0 != PA_OK || rcL != PA_OK || rcA != PA_OK || rcB != PA_OK) {
    dumpMapListing (&listed, &skipped);
  }

  // Map the channel page for each rail APID, purely to read its ACC_ENABLE and IRQ_STATUS words
  // (VEND:1067-1069, :1085-1087). This is the window a *write* command would go through; reading two
  // registers of it starts nothing, and no CMD word is ever written here -- which is the whole
  // difference between this round and the one that turns a rail on.
  const int railApid[kPmicArbRailCount] = { a11.apid, b3.apid };
  for (uint32_t i = 0; i < kPmicArbRailCount; i++) {
    if (railApid[i] < 0) {
      continue;
    }
    bool already = false;
    for (uint32_t j = 0; j < gChnlCount; j++) {
      already = already || gChnlApid[j] == static_cast<uint32_t> (railApid[i]);
    }
    if (already) {
      continue;
    }
    const uint64_t phys = kSpmiChnlsPhys +
                          static_cast<uint64_t> (kPmicArbChnlApidStride) *
                          static_cast<uint32_t> (railApid[i]);
    if (!mapWindow (gChnl[gChnlCount], phys, kSpmiMapSize)) {
      IOLog ("clover r182: map chnls page apid=%d %llx failed\n", railApid[i],
             static_cast<unsigned long long> (phys));
      continue;
    }
    gChnlApid[gChnlCount] = static_cast<uint32_t> (railApid[i]);
    gChnlCount++;
  }
  dumpAccessState (a11.apid, kRails[0].label);
  dumpAccessState (b3.apid, kRails[1].label);

  if (mode == 1) {
    // Read-only, as asked: nothing above wrote a byte and no transaction was started. The obs command
    // word in the version line is whatever the boot left there.
    IOLog ("clover r182: mode 1 started no transaction; mapped=%u listed=%u other_sid=%u ee0=%u\n",
           revid0.mapped, listed, skipped, revid0.per_ee[0]);
    clover_emmc_flush_cache ();
    return;
  }

  if (rc0 == PA_OK) {
    readRevid ("pm660", revid0.apid, kPm660Subtype, 4, 5);
  } else {
    IOLog ("clover r182: pm660 revid unreachable (rc=%d), no identity asked\n", rc0);
  }
  if (rcL == PA_OK) {
    readRevid ("pm660l", revidL.apid, kPm660lSubtype, 6, 7);
  } else {
    IOLog ("clover r182: pm660l revid unreachable (rc=%d), no identity asked\n", rcL);
  }

  if (mode == 2) {
    IOLog ("clover r182: mode 2 asked %u transactions, %u failed, no rail read\n",
           gXactRun, gXactFail);
    clover_power_observation[14] = (static_cast<uint64_t> (gXactRun) << 16) | gXactFail;
    clover_emmc_flush_cache ();
    return;
  }

  if (rcA == PA_OK) {
    readRail (kRails[0], a11.apid);
  } else {
    IOLog ("clover r182: ldo-a11 has no APID (rc=%d), vcc_i2c unproven\n", rcA);
  }
  if (rcB == PA_OK) {
    readRail (kRails[1], b3.apid);
  } else {
    IOLog ("clover r182: ldo-b3 has no APID (rc=%d), vdd unproven\n", rcB);
  }

  IOLog ("clover r182: done xfers=%u fail=%u timeout=%u denied=%u polls=%u\n",
         gXactRun, gXactFail, gXactTimeout, gXactDenied, gPolls);
  clover_power_observation[14] = (static_cast<uint64_t> (gXactRun) << 16) | gXactFail;
  clover_power_observation[15] = kStepComplete;
  // The tee in CloverSdhci picks these lines out of msgbuf on the next block operation, so this call is
  // a courtesy rather than the commit. Boot continues either way.
  clover_emmc_flush_cache ();
}
