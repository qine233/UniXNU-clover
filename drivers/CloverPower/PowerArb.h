// SPDX-License-Identifier: BSD-2-Clause-Patent
// The SPMI PMIC arbiter v5 transaction core for drivers/CloverPower: APID discovery, ownership,
// and one bounded read -- over plain `volatile uint32_t *` register pointers, with no IOKit, no
// logging and no notion of what board it is on.
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
// number was taken from (VEND spmi-pmic-arb.c for the v5 layout, MAIN for the revid and regulator
// offsets, SHENG for the transaction shape that has been run on a real device). What is proven by
// SHENG is that this sequence of register writes produces a PMIC byte: write the command word to
// the channel's CMD, poll STATUS until DONE, take the data from RDATA0 (and RDATA1 past 4 bytes).
// What is NOT proven, and what modes 1 and 2 of the driver exist to find out, is that on THIS
// silicon the observer window answers at the offsets the v5 formula gives, that the APID map is
// populated the way the vendor driver expects, and that a read of the pm660 revid block returns
// 0x51 at 0x104.
//
// NO WRITE PATH, ON PURPOSE
//
// There is no function here that writes to a PMIC. The chnls window -- the one a v5 write goes
// through, VEND:1013 -- is never mapped by the driver, and the only STATUS/CMD writes are to the
// *arbiter's own* observer channel, which is how a read is requested (VEND:370 `pmic_arb_set_rd_cmd`
// writes to rd_base, i.e. the same observer window). If the APID's owner is not this EE, that is
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

struct pa_arbiter
{
  volatile uint32_t *core;   // the core window: version, the APID map at +0x900
  volatile uint32_t *cnfg;   // the cnfg window: the ownership table at +0x700
  volatile uint32_t *obs;    // THIS EE's observer block, kSpmiObsBlockMapSize bytes
  uint32_t ee;               // 0 on this board (DTB qcom,ee)
};

// The result of one walk of the APID map. Every field is a reading, not a conclusion, and the
// driver prints them all: the point of mode 1 is to be able to tell a populated table from a blank
// one from an unreadable one, and those three look identical to a caller that only asks "which
// APID serves this PPID".
struct pa_scan
{
  uint32_t version;          // core + kPmicArbVersion
  uint32_t word0004;         // core + 0x0004, read once and decoded twice
  uint32_t features_apids;   // word0004 & kPmicArbFeaturesApidMask -- MAIN's reading of it
  uint32_t words;            // map words walked -- VEND's bound, from the core window's size
  uint32_t mapped;           // how many of them were non-zero
  uint32_t per_ee[6];        // mapped APIDs grouped by write owner (the gate answer, counted)
  uint32_t irq_per_ee[6];    // same, counting only words with the IRQ-owner bit set
  uint32_t apid_span;        // highest mapped APID + 1
  uint32_t candidates;       // APIDs mapped to want_ppid
  int apid;                  // the one chosen for want_ppid, -1 when there is none
  uint32_t owner;            // its write owner, from cnfg
  uint32_t irq_owner;        // 1 when its map word claims IRQ ownership
  uint32_t ppid;             // want_ppid echoed back, so a line cannot be read out of order
  int irq_apid;              // last APID for want_ppid that claimed IRQ ownership, -1 when none
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

// Which EE may write APID n. Vend reads this inside its own scan (VEND:920-922); doing it as a
// function keeps the host model free to fake it.
static inline uint32_t pa_owner_of (const struct pa_arbiter *a, uint32_t apid)
{
  return a->cnfg[(kPmicArbOwnerTable + kPmicArbOwnerStride * apid) / 4] & kPmicArbOwnerMask;
}

// The observer channel for an APID. `a->obs` is already this EE's block (the driver maps
// kSpmiObsrvrPhys + kPmicArbObsEeStride * ee), so the formula left here is the vendor driver's
// `0x80 * apid` half of VEND:1005. The bound matters: 512 channels of 0x80 bytes exactly fill the
// 0x10000 block, so an APID above 511 would run off the mapping -- and 448 is the largest index
// the core window's map can hold anyway, which is why kPmicArbMapEntries is the check below and
// not 512.
static inline volatile uint32_t *pa_channel (const struct pa_arbiter *a, uint32_t apid)
{
  return a->obs + (kPmicArbObsApidStride * apid) / 4;
}

// One walk of the map, following VEND:909-932 exactly: bound by the core window's size, skip zero
// words, take the first mapping of a PPID and replace it with a later one only when this EE owns
// that later one. The IRQ-owner answer is VEND:936+'s separate rule -- the last mapping for the
// PPID that has bit 24 set -- and is reported too, because it decides who gets the touch
// controller's interrupts, which is the question after this round's.
static inline int pa_scan_apids (const struct pa_arbiter *a, uint32_t want_ppid, struct pa_scan *r)
{
  uint32_t i;
  r->version = a->core[kPmicArbVersion / 4];
  r->word0004 = a->core[kPmicArbFeatures / 4];
  r->features_apids = r->word0004 & kPmicArbFeaturesApidMask;
  r->words = 0; r->mapped = 0; r->apid_span = 0; r->candidates = 0;
  r->apid = -1; r->owner = 0; r->irq_owner = 0; r->ppid = want_ppid; r->irq_apid = -1;
  for (i = 0; i < 6; i++) { r->per_ee[i] = 0; r->irq_per_ee[i] = 0; }

  // VEND:1210-1227 branches on this same read to choose the ops table. Above V6_MIN the channel
  // formula, the map base and the ownership base are all different (SHENG's v7 has 0x2000/0x00 and
  // 0x20/0x8000 strides), so a v7 read here would produce a confidently wrong APID number rather
  // than an obvious nonsense one. Refuse instead.
  if (r->version < kPmicArbVersionV5Min || r->version >= kPmicArbVersionV6Min) {
    return PA_E_VERSION;
  }

  for (i = 0; i < kPmicArbMapEntries; i++) {
    uint32_t map, owner;
    r->words++;
    map = a->core[(kPmicArbApidMap + kPmicArbApidMapStride * i) / 4];
    if (!map) {
      continue;   // VEND:915-916
    }
    r->mapped++;
    if (r->apid_span < i + 1) {
      r->apid_span = i + 1;
    }
    owner = pa_owner_of (a, i);
    if (owner < 6) {
      r->per_ee[owner]++;
      if ((map >> kPmicArbIrqOwnerBit) & 1u) {
        r->irq_per_ee[owner]++;
      }
    }
    if (pa_ppid_of (map) != want_ppid) {
      continue;
    }
    r->candidates++;
    if (r->apid < 0 || owner == a->ee) {   // VEND:930-932, the same tie-break
      r->apid = (int)i;
      r->owner = owner;
      r->irq_owner = ((map >> kPmicArbIrqOwnerBit) & 1u);
    }
    // VEND:936+ keeps the highest APID that has the IRQ-owner bit and is readable by this EE; the
    // bit alone is reported here, without the EE filter, because the filter is about who services
    // the interrupt and this round only wants to know whether one exists.
    if ((map >> kPmicArbIrqOwnerBit) & 1u) {
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
  if (apid >= kPmicArbMapEntries) {
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

// The physical address of the observer block a caller has to map for this EE: the vendor driver's
// `0x10000 * ee` half of the offset (VEND:1005), applied to the obsrvr window's own base.
static inline uint64_t pa_obs_block_phys (uint32_t ee)
{
  return kSpmiObsrvrPhys + (uint64_t) kPmicArbObsEeStride * ee;
}

#endif // CLOVER_POWER_ARB_H
