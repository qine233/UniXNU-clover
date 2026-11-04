// SPDX-License-Identifier: BSD-2-Clause-Patent
// The SPMI PMIC arbiter transaction core for drivers/CloverPower: which floor plan this silicon's
// version word selects, and then APID discovery, ownership and one bounded read -- over plain
// `volatile uint32_t *` register pointers, with no IOKit, no logging and no notion of what board it
// is on.
//
// WHY THIS FILE IS SEPARATE FROM THE DRIVER
//
// Because that is how sheng proved the same hardware block. ~/UniXNU-sheng/package/BootResetCore.h
// holds the arbiter logic (br_find, br_wait, br_read8, br_write8) as inline functions over
// register pointers, and package/tests/boot_reset_host.c drives those exact functions on a Mac
// against a software model of the arbiter with a device thread that serves the channels. The
// value of that split showed itself the hard way: the model caught the ordering bugs -- which APID
// wins when two map to one PPID, what a denied write looks like -- before any of it was written to
// a PMIC that could not be un-broken. This round is read-only, so the mistakes it can make are
// reports that are wrong rather than rails that are off, and a report that is wrong is worse: it
// sends the next round after a phantom. So the logic goes here, where scripts/test-clover-power.py
// can exercise it, and CloverPower.cpp keeps only mapping, decoding and printing.
//
// WHAT IS AND IS NOT PROVEN BY THIS CODE
//
// Every address formula here comes from PowerRegs.h, whose comments cite the file and line each
// number was taken from (VEND spmi-pmic-arb.c for the v5 and v3 layouts, MAIN for the revid and
// regulator offsets, SHENG for the transaction shape that has been run on a real device). What is
// proven by SHENG is that this sequence of register writes produces a PMIC byte: write the command
// word to the channel's CMD, poll STATUS until DONE, take the data from RDATA0 (and RDATA1 past 4
// bytes). What is NOT proven, and what the driver's modes exist to find out, is which band this
// board's version word lands in and therefore which of the two floor plans the numbers below should
// be read with -- that is the question round313's version line is shaped to answer, and until it is
// answered every observer offset, map base and ACC_ENABLE address in this file is a candidate, not
// a fact.
//
// WHY THE FLOOR PLAN IS DATA AND NOT CODE
//
// Because the version word, not this port, decides it. VEND:1210 reads core + 0x0000 once and
// installs one of four ops tables off the value (:1212-1224), and MAIN:2100-2110 does the same with
// the same floors -- two trees, independently, agreeing that [V3_MIN, V5_MIN) is v3 and that v3 has
// a layout. Round182 encoded that branch as a single test ("is it v5?") and stopped, which was the
// right first move on a board nobody had read yet and is the wrong second one: refusing to describe
// a v3 arbiter leaves the touch rails, the Wi-Fi rails and the GPU's second LDO leg all without an
// ownership answer, for a reason that was really "we only wrote down one band's addresses". So the
// differences live in `struct pa_layout` below, one row per band, and every function that had a
// constant took it from the row instead. Nothing about the transaction changed: the command encoding
// (VEND:1025-1028), the channel's CMD/STATUS/RDATA offsets (VEND:27-33) and the cnfg owner word
// (VEND:50-51) are the same in every band from v2 up, which is why one code path survives three
// tables.
//
// NO WRITE PATH, ON PURPOSE
//
// There is no function here that writes to a PMIC. The only CMD writes go to the *arbiter's own*
// observer channel, which is how a read is requested (VEND:370 `pmic_arb_set_rd_cmd()` writes to
// rd_base, i.e. the same observer window). The chnls window -- where a v5 *write* command would go,
// VEND:1013 -- is touched by this driver in one way only: the driver maps a page of it per rail APID
// and reads that page's ACC_ENABLE and IRQ_STATUS words (VEND:1066-1070, :1084-1088), never writing
// it. Under v2/v3 those same two words are not in chnls at all but in `intr` (VEND:1060-1064,
// :1078-1082), which is what `accInIntr` below says. If the APID's owner is not this EE, that is
// reported (pa_scan.owner) rather than worked around.
#ifndef CLOVER_POWER_ARB_H
#define CLOVER_POWER_ARB_H

#include <stdint.h>
#include "PowerRegs.h"

// One code per way a step can end, so a report never has to say "an error". The status codes 6/7/8
// come from the three STATUS bits the vendor driver distinguishes (VEND:261-278: -EIO for FAILURE,
// -EPERM for DENIED, -EIO for DROPPED); they stay separate here because the three mean three
// different things about the board -- a failed transaction is a bus problem, a denied one is an
// ownership answer, and a dropped one means the arbiter never put it on the bus at all.
enum {
  PA_OK = 0,
  PA_E_VERSION = 1,       // core + 0x0000 is not in the v5 band PowerRegs.h describes
  PA_E_NOENTRY = 2,       // the APID map walk found no mapping for anything at all
  PA_E_PPID = 3,          // no APID maps to the PPID that was asked for
  PA_E_LEN = 4,           // a burst outside 1..kPmicArbMaxTransBytes
  PA_E_POLL = 5,          // DONE never arrived within the bound
  PA_E_FAILED = 6,        // STATUS came back with FAILURE set
  PA_E_DENIED = 7,        // STATUS came back with DENIED set
  PA_E_DROPPED = 8,       // STATUS came back with DROPPED set
  PA_E_APID = 9           // an APID outside the one observer block that was mapped
};

#define kPmicArbMaxTransBytes 8U    // VEND:94, and the reason pa_read() refuses 9

// Barrier between the command write and the status poll, and between the done read and the data
// read. CMD, STATUS and RDATA0 are different offsets of the same peripheral, and arm64 Device
// memory only guarantees ordering of accesses to the same location, so without these two the
// hardware is allowed to see the poll before the command (and then wait for a transaction nobody
// asked for). CloverInput's windowRead() has carried a `dsb sy` after every access for the same
// reason since round169. On the host model there is no device to order against and the arrays are
// ordinary memory, so it compiles away -- which is what keeps this file testable by
// scripts/test-clover-power.py.
#if defined(__aarch64__)
#define PA_BARRIER() __asm__ volatile ("dsb sy" ::: "memory")
#else
#define PA_BARRIER() ((void) 0)
#endif

// One band's floor plan. The rows are transcriptions of the vendor driver's ops tables -- v2 at
// VEND:1131-1142, v3 at :1144-1155, v5 at :1157-1168 -- and `pa_layout_for()` mirrors VEND:1212-1224
// branch for branch. Every field is an address this file has to use rather than remember, so a reader
// of a report can check any number in it against one row.
struct pa_layout
{
  const char *name;           // "v2", "v3", "v5" -- printed, so no reader has to derive it
  uint32_t versionMin;        // the floor VEND:1212-1224 tests to reach this row
  uint32_t mapBase;           // core offset of APID 0's map word
  uint32_t mapStride;         // 4 in every band VEND has an ops table for
  uint32_t mapEntries;        // VEND:910-911's bound: (core window - mapBase) / stride
  uint32_t obsEeStride;       // observer window: this EE's block starts here
  uint32_t obsApidStride;     // ... and one APID's channel block is this far from the next
  uint32_t accInIntr;         // 1: ACC_ENABLE/IRQ_STATUS are `intr` offsets (v2/v3). 0: `chnls` (v5)
  uint32_t accEnableBase;     // base of ACC_ENABLE inside that window
  uint32_t accEnableStride;   // per APID
  uint32_t irqStatusBase;     // same window, IRQ_STATUS
  uint32_t irqClearBase;      // same window, IRQ_CLEAR
  uint32_t ownerAccBase;      // inside `intr`: the per-EE owner/ACC-status array
  uint32_t ownerAccEeStride;  // per EE
  uint32_t ownerAccGroupStride;  // per 32-APID GROUP, not per APID -- see pa_owner_acc_addr()
  uint32_t obsMapSize;        // how much of the observer window this band has to map to reach a
                              // resolved APID at all -- see PowerRegs.h's mapping paragraph
  uint32_t intrMapSize;       // ... and the same question for the interrupt window
  int mapIrqOwnerBit;         // 1 for v5 (VEND:46/:918); 0 for v2/v3, where VEND:860-861 says
                              // irq_ee = write_ee and there is no bit in the map word at all
};

static const struct pa_layout pa_layout_v2 = {
  "v2", kPmicArbVersionV2Min, kPmicArbApidMapV23, kPmicArbApidMapStride, kPmicArbMapEntriesV23,
  kPmicArbObsEeStrideV23, kPmicArbObsApidStrideV23, kPmicArbAccInIntr,
  0x0U, kPmicArbAccEnableIntrStride, kPmicArbIrqStatusIntrBase, kPmicArbIrqClearIntrBase,
  kPmicArbOwnerAccBaseV2, kPmicArbOwnerAccEeStrideV23, 0x4U,
  kSpmiObsMapSizeV23, kSpmiIntrMapSizeV23, 0
};

static const struct pa_layout pa_layout_v3 = {
  "v3", kPmicArbVersionV3Min, kPmicArbApidMapV23, kPmicArbApidMapStride, kPmicArbMapEntriesV23,
  kPmicArbObsEeStrideV23, kPmicArbObsApidStrideV23, kPmicArbAccInIntr,
  0x0U, kPmicArbAccEnableIntrStride, kPmicArbIrqStatusIntrBase, kPmicArbIrqClearIntrBase,
  kPmicArbOwnerAccBaseV3, kPmicArbOwnerAccEeStrideV23, 0x4U,
  kSpmiObsMapSizeV23, kSpmiIntrMapSizeV23, 0
};

static const struct pa_layout pa_layout_v5 = {
  "v5", kPmicArbVersionV5Min, kPmicArbApidMap, kPmicArbApidMapStride, kPmicArbMapEntries,
  kPmicArbObsEeStride, kPmicArbObsApidStride, kPmicArbAccInChnls,
  kPmicArbAccEnable, kPmicArbChnlApidStride, kPmicArbIrqStatus, kPmicArbIrqClear,
  kPmicArbOwnerAccStatus, kPmicArbOwnerAccEeStride, 0x4U,
  kSpmiObsMapSizeV5, kSpmiIntrMapSizeV5, 1
};

// VEND:1212-1224, and MAIN:2102-2110, in one function. Below V2_MIN is v1, whose layout puts rd_base
// and wr_base both at core (VEND:1213-1215) and whose DT would not declare the chnls/obsrvr/intr
// windows this board's DT does declare (:2267-2268 of the flashed DTB dump) -- so v1 is not a table
// this port has a right to guess at, and neither is anything at or above V6_MIN, which is where
// SHENG's v7 lives with 0x2000/0x00 bases and a 0x20/0x8000 stride (SHENG:22-23). Those two bands
// return null and the driver says so in words instead of reading a wrong table confidently.
//
// One deliberate divergence, named rather than buried: the vendor's own else-chain has no upper
// bound -- VEND:1222-1224 reads `else pmic_arb->ver_ops = &pmic_arb_v5;`, so a v6 or v7 arbiter
// would be driven with the v5 table by the vendor itself. MAIN:2102-2110 is the same shape. This
// port stops at V6_MIN instead, because a board whose version word says 0x6.. or 0x7.. is asking a
// question this header has no address for, and the safe answer to an unknown question is
// PA_E_VERSION with the word printed, not a plausible read. If a future board ever lands in that
// band, the fix is a fourth row here, not a widened bound.
//
// The two tables this port does know are not equally safe against each other, and the host model
// (tests/clover_power_host.cpp case 3) found the asymmetry rather than the comment assuming it. The
// v2/v3 map starts at 0x800 and runs to the end of the core window, so it *contains* the v5 map's
// 0x900..0xfff: a v5-shaped map walked under a v3 version word still finds every PPID, just at an
// APID number that is (0x900 - 0x800)/4 == 64 too high, and because ownership is indexed by that same
// shifted APID the tie-break silently changes shape as well. The reverse direction does refuse -- the
// v5 walk starts above the words a v3 map wrote, so it reads zeros and reports PA_E_NOENTRY. In other
// words nothing downstream of pa_layout_for() catches a mis-banded read; the only defence is the
// version word, which is why it gets four independent renderings on the report rather than one.
static inline const struct pa_layout * pa_layout_for (uint32_t version)
{
  if (version < kPmicArbVersionV2Min) {
    return 0;                                    // v1: no observer/chnls split, unknown here
  }
  if (version < kPmicArbVersionV3Min) {
    return &pa_layout_v2;
  }
  if (version < kPmicArbVersionV5Min) {
    return &pa_layout_v3;
  }
  if (version < kPmicArbVersionV6Min) {
    return &pa_layout_v5;
  }
  return 0;                                      // v6 and up: SHENG's territory, not this table
}

// The address of the word that says which of the 32 APIDs in a group an EE has an ACC status for.
// VEND:522-523 walks `first = min_apid >> 5` to `last = max_apid >> 5` and at :542 recovers
// `apid = id + i * 32`, so the third argument of owner_acc_status() is the GROUP index, not the APID.
// Round182 indexed it by APID, which coincides only for APIDs 0 and 1 -- the fix is here rather than
// only in the comment because the host model below now asserts both readings.
static inline uint32_t pa_owner_acc_addr (const struct pa_layout *l, uint32_t ee, uint32_t apid)
{
  return l->ownerAccBase + l->ownerAccEeStride * ee +
         l->ownerAccGroupStride * (apid >> kPmicArbOwnerAccGroupShift);
}

struct pa_arbiter
{
  volatile uint32_t *core;   // the core window: version at +0x0000, the APID map at l->mapBase
  volatile uint32_t *cnfg;   // the cnfg window: the ownership table at +0x700, every band
  volatile uint32_t *obs;    // the observer window, based at this EE's block (l->obsEeStride * ee)
  uint32_t ee;               // 0 on this board (DTB qcom,ee)
  const struct pa_layout *l; // which floor plan the version word selected
};

// The result of one walk of the APID map. Every field is a reading, not a conclusion, and the
// driver prints them all: the point of mode 1 is to be able to tell a populated table from a blank
// one from an unreadable one, and those three look identical to a caller that only asks "which
// APID serves this PPID".
struct pa_scan
{
  uint32_t version;          // core + kPmicArbVersion
  const struct pa_layout *l; // pa_layout_for(version): null when the band has no known table
  uint32_t word0004;         // core + 0x0004, read once and decoded twice
  uint32_t features_apids;   // word0004 & kPmicArbFeaturesApidMask -- MAIN's reading of it
  uint32_t words;            // map words walked -- VEND's bound, from the core window's size
  uint32_t mapped;           // how many of them were non-zero
  uint32_t per_ee[6];        // mapped APIDs grouped by write owner (the gate answer, counted)
  uint32_t irq_per_ee[6];    // same, counting only the words that claim IRQ ownership (see below)
  uint32_t apid_span;        // highest mapped APID + 1
  uint32_t candidates;       // APIDs mapped to want_ppid
  int apid;                  // the one chosen for want_ppid, -1 when there is none
  uint32_t owner;            // its write owner, from cnfg
  uint32_t irq_ee;           // who services its interrupts, in the band's own terms: 0..5, or
                             // kPaInvalidEe when the band says nobody
  uint32_t ppid;             // want_ppid echoed back, so a line cannot be read out of order
  int irq_apid;              // last APID for want_ppid whose IRQ owner is this EE, -1 when none
  uint32_t irq_none;         // mapped APIDs whose band answer is kPaInvalidEe (v5 bit-24 clear only)
  int irq_from;              // which rule produced the above: kPaIrqFromMapBit or kPaIrqFromOwner
};

// The command word: (opc << 27) | ((addr & 0xff) << 4) | (bc & 7). VEND:1025-1028 and SHENG:31,
// identically. The slave ID does not appear because the APID already selected the peripheral.
static inline uint32_t pa_cmd (uint32_t op, uint32_t byte_addr, uint32_t bc)
{
  return (op << kPmicArbCmdOpShift) |
         ((byte_addr & kPmicArbCmdAddrMask) << kPmicArbCmdAddrShift) |
         (bc & kPmicArbCmdBcMask);
}

static inline uint32_t pa_ppid_of (uint32_t map_word)
{
  return (map_word >> kPmicArbPpidShift) & kPmicArbPpidMask;
}

// VEND:47 INVALID_EE, spelled for the scan result so a report can say "nobody" without a magic 255.
#define kPaInvalidEe 0xFFU
#define kPaIrqFromMapBit 0   // v5: bit 24 of the map word (VEND:46, tested at :918)
#define kPaIrqFromOwner 1    // v2/v3: VEND:860-861 assigns irq_ee = write_ee; no bit exists to test

// Who services this APID's interrupts, in the band's own terms. v5 answers from the map word: the
// EE named in cnfg, but only when bit 24 is set, and kPaInvalidEe when it is not (VEND:924). v2 and
// v3 have no such bit at all, and VEND:860-861 hands back the cnfg owner unconditionally -- a
// different rule, from a different window, that happens to print as the same number. So the scan
// carries which rule it used rather than pretending there is one answer.
static inline uint32_t pa_irq_ee (const struct pa_layout *l, uint32_t map_word, uint32_t write_ee)
{
  if (!l->mapIrqOwnerBit) {
    return write_ee;   // VEND:860-861
  }
  return ((map_word >> kPmicArbIrqOwnerBit) & 1u) ? write_ee : kPaInvalidEe;   // VEND:918/:924
}

// Which EE may write APID n. Vend reads this inside its own scan (VEND:920-922); doing it as a
// function keeps the host model free to fake it.
static inline uint32_t pa_owner_of (const struct pa_arbiter *a, uint32_t apid)
{
  return a->cnfg[(kPmicArbOwnerTable + kPmicArbOwnerStride * apid) / 4] & kPmicArbOwnerMask;
}

// The observer channel for an APID. `a->obs` is already this EE's block (the driver bases it at
// obsrvr + l->obsEeStride * ee), so what is left here is the vendor driver's per-APID half of its
// channel formula -- `0x80 * apid` for v5 (VEND:1005) and `0x8000 * apid` for v2/v3 (VEND:983). The
// stride is the whole difference between the two bands for this driver's purposes: at 0x80 the 448
// APIDs the core window can hold fit in one 0x10000 block, and at 0x8000 they do not fit in anything
// but the whole declared observer window, which is why the mapped size comes from the layout too
// (kSpmiObsMapSizeV5 / kSpmiObsMapSizeV23 in PowerRegs.h).
static inline volatile uint32_t *pa_channel (const struct pa_arbiter *a, uint32_t apid)
{
  return a->obs + (a->l->obsApidStride * apid) / 4;
}

// One walk of the map, following VEND:909-932 exactly: bound by the core window's size, skip zero
// words, take the first mapping of a PPID and replace it with a later one only when this EE owns
// that later one. The IRQ-owner answer is band-dependent (see pa_irq_ee) and is reported together
// with the rule that produced it, because it decides who gets the touch controller's interrupts --
// the question after this round's.
static inline int pa_scan_apids (const struct pa_arbiter *a, uint32_t want_ppid, struct pa_scan *r)
{
  uint32_t i;
  r->version = a->core[kPmicArbVersion / 4];
  r->word0004 = a->core[kPmicArbFeatures / 4];
  r->features_apids = r->word0004 & kPmicArbFeaturesApidMask;
  r->words = 0; r->mapped = 0; r->apid_span = 0; r->candidates = 0;
  r->apid = -1; r->owner = 0; r->irq_ee = kPaInvalidEe; r->ppid = want_ppid; r->irq_apid = -1;
  r->irq_none = 0; r->irq_from = -1; r->l = 0;
  for (i = 0; i < 6; i++) { r->per_ee[i] = 0; r->irq_per_ee[i] = 0; }

  // VEND:1210-1227 branches on this same read to choose the ops table, and pa_layout_for() is that
  // branch. A band it does not know -- v1 below V2_MIN, or v6 and up, where SHENG's v7 lives with
  // 0x2000/0x00 map and owner bases and a 0x20/0x8000 stride -- returns null, and this walk refuses
  // rather than guessing: reading a v7 table at a v2 address produces a confidently wrong APID number
  // instead of an obviously nonsense one.
  r->l = pa_layout_for (r->version);
  if (!r->l) {
    return PA_E_VERSION;
  }
  const struct pa_layout *l = r->l;
  r->irq_from = l->mapIrqOwnerBit ? kPaIrqFromMapBit : kPaIrqFromOwner;

  for (i = 0; i < l->mapEntries; i++) {
    uint32_t map, owner, irqee;
    r->words++;
    map = a->core[(l->mapBase + l->mapStride * i) / 4];
    if (!map) {
      continue;   // VEND:915-916
    }
    r->mapped++;
    if (r->apid_span < i + 1) {
      r->apid_span = i + 1;
    }
    owner = pa_owner_of (a, i);
    irqee = pa_irq_ee (l, map, owner);
    if (owner < 6) {
      r->per_ee[owner]++;
    }
    if (irqee == kPaInvalidEe) {
      r->irq_none++;
    } else if (irqee < 6) {
      r->irq_per_ee[irqee]++;
    }
    if (pa_ppid_of (map) != want_ppid) {
      continue;
    }
    r->candidates++;
    if (r->apid < 0 || owner == a->ee) {   // VEND:930-932, the same tie-break
      r->apid = (int)i;
      r->owner = owner;
      r->irq_ee = irqee;
    }
    // VEND:936-940 keeps the *later* duplicate mapping's IRQ owner over the earlier one's when this
    // EE already has a readable APID for the PPID; the same "last one wins" here, filtered to the
    // APIDs whose band answer names this EE.
    if (irqee == a->ee) {
      r->irq_apid = (int)i;
    }
  }
  if (!r->mapped) {
    return PA_E_NOENTRY;
  }
  return r->apid < 0 ? PA_E_PPID : PA_OK;
}

// STATUS to code, the three error bits distinguished the way VEND:261-278 distinguishes them.
static inline int pa_status_code (uint32_t status)
{
  if (!(status & kPmicArbStatusDone)) {
    return PA_E_POLL;
  }
  if (status & kPmicArbStatusDenied) {
    return PA_E_DENIED;
  }
  if (status & kPmicArbStatusFailure) {
    return PA_E_FAILED;
  }
  if (status & kPmicArbStatusDropped) {
    return PA_E_DROPPED;
  }
  return PA_OK;
}

// The wait. VEND:258-284 spends PMIC_ARB_TIMEOUT_US microseconds as 1000 udelay(1) iterations; a
// driver at this point in the boot has no timer to sleep on, so this bounds the wait by plain
// polls -- SHENG:48-54 does the same with 100000 -- and the caller passes a count derived from its
// own measured deadline. Nothing here sleeps, spins longer than the bound, or panics.
static inline int pa_wait (volatile uint32_t *ch, uint32_t polls, uint32_t *status_out)
{
  uint32_t n;
  for (n = 0; n < polls; n++) {
    uint32_t status = ch[kPmicArbStatus / 4];
    if (status & kPmicArbStatusDone) {
      if (status_out) {
        *status_out = status;
      }
      return pa_status_code (status);
    }
  }
  if (status_out) {
    *status_out = ch[kPmicArbStatus / 4];
  }
  return PA_E_POLL;
}

// One read transaction: `len` bytes starting at byte_addr inside the peripheral the APID names.
// The command is written to the observer channel's CMD (VEND:370 writes rd_base + PMIC_ARB_CMD,
// and rd_base IS the observer window on v5, VEND:1227-1229), the wait is bounded, and the bytes
// come back the way VEND:376-381 takes them: up to four from the little-endian word at RDATA0,
// the rest from RDATA1.
//
// Repeated calls on one channel are legal without clearing anything: neither tree clears DONE
// (see the note in PowerRegs.h), and SHENG:71-81 runs read-write-read over one APID and gets the
// right third byte.
static inline int pa_read (const struct pa_arbiter *a, uint32_t apid, uint32_t byte_addr,
                           uint8_t *buf, uint32_t len, uint32_t polls, uint32_t *status_out)
{
  volatile uint32_t *ch;
  uint32_t bc, word, got = 0, first;
  int rc;

  if (len < 1 || len > kPmicArbMaxTransBytes) {
    return PA_E_LEN;
  }
  // No floor plan, no channel: an arbiter whose band pa_layout_for() refused must never reach a
  // transaction, however sane its APID looks.
  if (!a->l) {
    return PA_E_VERSION;
  }
  // The vendor's own bound on the map (VEND:910-911: stop when the offset reaches the core window's
  // size), which is also the largest APID this layout can name -- 448 at the v5 base of 0x900, 512
  // at v2/v3's 0x800.
  if (apid >= a->l->mapEntries) {
    return PA_E_APID;
  }
  ch = pa_channel (a, apid);
  bc = len - 1;
  ch[kPmicArbCmd / 4] = pa_cmd (kPmicArbReadOp, byte_addr & 0xff, (uint8_t) bc);
  PA_BARRIER ();
  rc = pa_wait (ch, polls, status_out);
  if (rc) {
    return rc;
  }
  PA_BARRIER ();
  // VEND:218-224 memcpy()s out of the raw register word, which on little-endian arm64 takes the
  // low byte first; MAIN:942-944 reads two bytes the same way and builds a value as (buf[1] << 8)
  // | buf[0]. So the byte order below is the register's own, not a choice.
  first = len > 4 ? 4 : len;
  word = ch[kPmicArbRdata0 / 4];
  while (got < first) {
    buf[got++] = (uint8_t) (word & 0xff);
    word >>= 8;
  }
  if (len > 4) {
    word = ch[kPmicArbRdata1 / 4];
    while (got < len) {
      buf[got++] = (uint8_t) (word & 0xff);
      word >>= 8;
    }
  }
  return PA_OK;
}

// A single byte, which is what a revid register or an enable register is.
static inline int pa_read8 (const struct pa_arbiter *a, uint32_t apid, uint32_t byte_addr,
                            uint8_t *value, uint32_t polls, uint32_t *status_out)
{
  return pa_read (a, apid, byte_addr, value, 1, polls, status_out);
}

// The physical address of the observer block a caller has to map for this EE. The EE term is part of
// the floor plan, not a constant: `0x10000 * ee` for v5 (VEND:1005) and `0x1000 * ee` for v2/v3
// (VEND:983), both applied to the obsrvr window's own base.
static inline uint64_t pa_obs_block_phys (const struct pa_layout *l, uint32_t ee)
{
  return kSpmiObsrvrPhys + (l ? (uint64_t) l->obsEeStride * ee : 0u);
}

// Which of the two windows holds this band's ACC_ENABLE / IRQ_STATUS / IRQ_CLEAR words, and where in
// it. VEND puts all three in `chnls` for v5 (:1066-1070, :1084-1088, :1102-1106) and all three in
// `intr` for v2/v3 (:1060-1064, :1078-1082, :1096-1100), with a per-APID stride of 0x10000 against
// 0x1000. A driver that kept looking in chnls after a v3 read would be reporting the word at
// chnls + 0x100 as an access-port enable, which is a transaction-register offset wearing a name.
#define kPaAccWordEnable 0
#define kPaAccWordIrqStatus 1
#define kPaAccWordIrqClear 2
static inline int pa_acc_window_is_intr (const struct pa_layout *l)
{
  return l->accInIntr != 0u;
}
static inline uint32_t pa_acc_word_addr (const struct pa_layout *l, uint32_t apid, uint32_t which)
{
  const uint32_t base = which == kPaAccWordEnable ? l->accEnableBase :
                        (which == kPaAccWordIrqStatus ? l->irqStatusBase : l->irqClearBase);
  return base + l->accEnableStride * apid;
}

#endif // CLOVER_POWER_ARB_H
