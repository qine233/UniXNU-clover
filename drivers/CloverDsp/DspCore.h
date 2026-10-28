// SPDX-License-Identifier: BSD-2-Clause-Patent
// The SMEM / SMP2P reading core for drivers/CloverDsp: the shared-memory header, the global table of
// contents, the partition table and its private partitions, the items inside them, the SMP2P item
// layout, the APCS doorbell word, and the two image-presence scans -- over plain `volatile uint8_t *`
// spans, with no IOKit, no logging and no notion of which subsystem is supposed to be on the other
// side.
//
// WHY THIS FILE IS SEPARATE FROM THE DRIVER
//
// Same reason as PowerArb.h, and the same reason in the same words: the reading that decides the next
// round is a decode, and a decode that has only ever run on the device is a decode whose bugs are
// discovered on the device. SMEM in particular is a *linked* structure -- the partition table points
// at partitions, partitions point at items, items point at their data -- and every pointer in it is a
// 32-bit field in memory another processor is concurrently writing. The bounds questions that make
// such a walk safe ("is this offset inside what I mapped", "may I trust `size` from a header whose
// magic I have not validated yet") are exactly the questions a software model can answer about this
// code before it runs against a real heap. So the walk lives here, scripts/test-clover-dsp.py drives
// it on a Mac against a synthesized SMEM region, and CloverDsp.cpp keeps mapping, printing and the
// observation array.
//
// WHAT IS PROVEN AND WHAT IS NOT
//
// Proven, in the sense that a Qualcomm DSP has been read this way before: the shape of the walk.
// SHENG's package/QcomSmem.c (partition table one page from the region end, "$TOC" version 1, 48-byte
// entries, "$PRT" partition headers with two free marks, the uncached list growing up from a 32-byte
// header and the cached area growing down from the partition's end, 16-byte item entries with canary
// 0xa5a5 and item in the upper 16 bits of word 0) has run against an ADSP that answered, and this
// file reproduces those formulas rather than re-deriving them. The offsets are cross-checked against
// VEND smem.c:101-239, where the same layout is expressed as C structs.
//
// Not proven, and the reason modes 1 and 2 exist: that this board's SMEM region is populated the way
// both implementations assume. Specifically -- whether `initialized` is non-zero, whether a "$TOC" is
// at 0x1FF000 at all (a legacy non-partitioned layout would put the header's TOC there instead and
// this code reports that as DS_E_MAGIC rather than guessing), which partitions name host 0, and
// whether items 429/443 are the ADSP's SMP2P pair or something else wearing those numbers. The last
// one is why an item is only accepted as an SMP2P edge after ds_smp2p_head() has validated its magic,
// its version, its total_entries and both pid fields.
//
// READ-ONLY, STRUCTURALLY
//
// There is no function here that takes a value to store. The span's base pointer is `volatile uint8_t
// *` because the same struct is used for the ADSP carve-out, whose contents belong to another
// processor's firmware, but every operation is a read; the doorbell block is read with a plain word
// load and never has a bit written to it. Ringing a doorbell is what the round after this one does,
// and it should do it knowing which edge is already up.
#ifndef CLOVER_DSP_CORE_H
#define CLOVER_DSP_CORE_H

#include <stdint.h>
#include "DspRegs.h"

// One code per way a step can end. The three that matter most are DS_E_RANGE (the structure asked me
// to look outside the mapping, which is a *finding* about a heap being written by somebody else and
// the reason the walk is bounded rather than trusted), DS_E_MAGIC (the layout is not the one this code
// implements) and DS_E_ITEM (this item is simply not allocated, which for the ADSP edges is the
// strongest available evidence that nobody ever brought the DSP up in this boot).
enum {
  DS_OK = 0,
  DS_E_SPAN = 1,        // a null or too-small span to hold the structure being read
  DS_E_RANGE = 2,       // an offset the walk was asked to follow, outside the mapped span
  DS_E_ALIGN = 3,       // an unaligned word read; SMEM is a word array in every implementation
  DS_E_MAGIC = 4,       // a magic field that is not the expected magic
  DS_E_VERSION = 5,     // a version field this code does not implement
  DS_E_COUNT = 6,       // a count field larger than the space that could hold it
  DS_E_CANARY = 7,      // an item header's canary is wrong: the list is not where it is claimed
  DS_E_PART = 8,        // a partition header contradicts its own table entry
  DS_E_ITEM = 9,        // the item is in neither the uncached nor the cached list
  DS_E_SIZE = 10,       // the item is smaller than the record being decoded
  DS_E_PID = 11         // an "$SMP" item whose pid fields do not name the edge it was read for
};

// A mapped span, and the physical address it corresponds to. `phys` exists so that resolved offsets
// come back as addresses a human can check against /proc/iomem rather than as offsets into a mapping
// only the driver can see -- and so the host model can assert the arithmetic with a fake base.
struct ds_span
{
  volatile uint8_t *base;
  uint32_t size;
  uint64_t phys;
};

// Compiler barrier between the reads that form one logical observation. SMEM's structures are written
// by two processors without a lock on the read side (VEND smem.c's read path takes no hardware
// spinlock; only the allocator does), so the ordering this code needs is "do not reorder the header
// fields into a different generation than the data" -- which a compiler barrier plus a volatile access
// gives. A full `dsb sy` per word of a 2 MiB scan would cost the boot for a guarantee Device-memory
// already provides, and the vendor and SHENG implementations both stop at the compiler barrier; where
// the driver needs the stronger form it does it itself, in windowRead().
#if defined(__aarch64__)
#define DS_BARRIER() __asm__ volatile ("dsb ld" ::: "memory")
#else
#define DS_BARRIER() __asm__ volatile ("" ::: "memory")
#endif

static inline int ds_rd32 (const struct ds_span &span, uint32_t at, uint32_t *out)
{
  if (!span.base || !out) {
    return DS_E_SPAN;
  }
  if (at & 3u) {
    return DS_E_ALIGN;
  }
  if (at + 4 > span.size) {           // no overflow: at is bounded by span.size first
    return DS_E_RANGE;
  }
  *out = *reinterpret_cast<volatile uint32_t *> (span.base + at);
  DS_BARRIER ();
  return DS_OK;
}

static inline int ds_rd16 (const struct ds_span &span, uint32_t at, uint32_t *out)
{
  if (!span.base || !out) {
    return DS_E_SPAN;
  }
  if (at + 2 > span.size) {
    return DS_E_RANGE;
  }
  *out = static_cast<uint32_t> (span.base [at]) |
         (static_cast<uint32_t> (span.base [at + 1]) << 8);
  DS_BARRIER ();
  return DS_OK;
}

// Does `mask` name any bit currently set in the doorbell word? The mask is the DT's own
// `qcom,irq-bitmask`/`qcom,irq-mask`; the word is what 0x17911008 reads. A set bit here means the AP
// rang that edge and the remote has not (yet) cleared it -- on the IPCC-style blocks the remote clears
// it by acknowledging, so "set" is an event in flight rather than a state, which is why the driver
// prints the raw word alongside the per-edge answers instead of only printing booleans.
static inline int ds_bitSet (uint32_t value, uint32_t mask)
{
  return mask != 0 && (value & mask) != 0;
}

// ------------------------------------------------------------------ the header
struct ds_header
{
  uint32_t initialized;        // 0xC0; non-zero means the boot set up this heap at all
  uint32_t free_offset;        // 0xC4
  uint32_t available;          // 0xC8
  uint32_t reserved;           // 0xCC; VEND says must be 0, so printing it is a validity check
  uint32_t sbl_version;        // version[7]: the SBL's own version record
  uint32_t pc_command;         // proc_comm[0].command, the legacy interface's last request
  uint32_t pc_status;          // ... and its reply, which is the only trace of a proc_comm boot
  uint32_t toc_scanned;        // TOC entries inside the mapping that were examined
  uint32_t toc_allocated;      // ... of those, the ones with allocated != 0
  uint32_t toc_highest;        // the largest allocated item number + 1, i.e. this heap's item count
};

static inline int ds_header (const struct ds_span &smem, struct ds_header *out)
{
  if (!smem.base || smem.size < kSmemToc) {
    return DS_E_SPAN;
  }
  struct ds_header result;
  int rc = ds_rd32 (smem, kSmemInitialized, &result.initialized);
  rc |= ds_rd32 (smem, kSmemFreeOffset, &result.free_offset);
  rc |= ds_rd32 (smem, kSmemAvailable, &result.available);
  rc |= ds_rd32 (smem, kSmemReserved, &result.reserved);
  rc |= ds_rd32 (smem, kSmemVersionTable + kSmemSblVersionIndex * kSmemVersionStride, &result.sbl_version);
  rc |= ds_rd32 (smem, kSmemProcComm, &result.pc_command);
  rc |= ds_rd32 (smem, kSmemProcComm + 4, &result.pc_status);
  if (rc) {
    return DS_E_RANGE;
  }
  // The TOC walk is bounded by the mapping, not by kSmemItemCount, and stops at the first offset that
  // would not fit. A 2 MiB mapping covers all 512 entries; a truncated mapping covers some of them and
  // the driver prints toc_scanned so the reader can see which case this was.
  result.toc_scanned = 0;
  result.toc_allocated = 0;
  result.toc_highest = 0;
  for (uint32_t item = 0; item < kSmemItemCount; item++) {
    const uint32_t at = kSmemToc + item * kSmemTocEntrySize;
    uint32_t allocated = 0;
    if (ds_rd32 (smem, at, &allocated)) {
      break;
    }
    result.toc_scanned++;
    if (allocated) {
      result.toc_allocated++;
      result.toc_highest = item + 1;
    }
  }
  *out = result;
  return DS_OK;
}

// ------------------------------------------------------------------ one global TOC entry
struct ds_tocEntry
{
  uint32_t item;
  uint32_t allocated;
  uint32_t offset;           // from the region base
  uint32_t size;
  uint32_t aux_base;         // masked with kSmemAuxBaseMask
  uint64_t phys;             // offset resolved against span.phys, only meaningful when aux_base is 0
  int in_span;               // 1 when [offset, offset+size) is inside the mapping
};

static inline int ds_toc (const struct ds_span &smem, uint32_t item, struct ds_tocEntry *out)
{
  if (item >= kSmemItemCount) {
    return DS_E_RANGE;
  }
  const uint32_t at = kSmemToc + item * kSmemTocEntrySize;
  ds_tocEntry entry;
  uint32_t raw = 0;
  int rc = ds_rd32 (smem, at + kSmemTocAllocated, &entry.allocated);
  rc |= ds_rd32 (smem, at + kSmemTocOffset, &entry.offset);
  rc |= ds_rd32 (smem, at + kSmemTocSize, &entry.size);
  rc |= ds_rd32 (smem, at + kSmemTocAuxBase, &raw);
  if (rc) {
    return DS_E_RANGE;
  }
  entry.item = item;
  entry.aux_base = raw & kSmemAuxBaseMask;
  entry.phys = smem.phys + entry.offset;
  // `offset + size` cannot be checked without overflow: both come from memory another processor owns.
  entry.in_span = entry.offset <= smem.size &&
                  entry.size <= smem.size - entry.offset ? 1 : 0;
  *out = entry;
  return DS_OK;
}

// ------------------------------------------------------------------ the partition table
struct ds_ptable
{
  uint32_t magic;
  uint32_t version;
  uint32_t num_entries;
  uint32_t at;                 // where the table was found, for the report line
  uint32_t info_magic;         // the smem_info that follows the entries, when it is there
  uint32_t info_num_items;     // its 16-bit bound on the TOC
  uint32_t capacity;           // entries the page could hold; num_entries above it is DS_E_COUNT
};

static inline int ds_ptable (const struct ds_span &smem, struct ds_ptable *out)
{
  // SHENG QcomSmem.c:22 refuses a region that is not a whole number of pages or smaller than 8 KiB,
  // because "one page from the end" is only a partition table if the end is where the region is.
  if (!smem.base || smem.size < 0x2000 || (smem.size & 0xfff)) {
    return DS_E_SPAN;
  }
  const uint32_t at = smem.size - 0x1000;
  struct ds_ptable table;
  int rc = ds_rd32 (smem, at, &table.magic);
  rc |= ds_rd32 (smem, at + kSmemPtableVersion, &table.version);
  rc |= ds_rd32 (smem, at + kSmemPtableNumEntries, &table.num_entries);
  if (rc) {
    return DS_E_RANGE;
  }
  table.at = at;
  table.capacity = (0x1000 - kSmemPtableSize) / kSmemPtableEntrySize;
  if (table.magic != kSmemPtableMagic) {
    *out = table;
    return DS_E_MAGIC;
  }
  if (table.version != 1) {                     // SHENG QcomSmem.c:25 requires exactly 1
    *out = table;
    return DS_E_VERSION;
  }
  if (table.num_entries > table.capacity) {     // SHENG QcomSmem.c:26, same bound
    *out = table;
    return DS_E_COUNT;
  }
  // smem_info sits immediately after the last entry (VEND smem.c:855). Its magic is read through the
  // span so a table that fills the page cannot make this read leave the mapping.
  table.info_magic = 0;
  table.info_num_items = 0;
  const uint32_t info = at + kSmemPtableSize + table.num_entries * kSmemPtableEntrySize;
  uint32_t magic = 0;
  if (ds_rd32 (smem, info, &magic) == DS_OK) {
    table.info_magic = magic;
    if (magic == kSmemInfoMagic) {
      uint32_t words = 0;
      // num_items is a u16 at +0x10 (VEND smem.c:256), so it is read as bytes: an unaligned 32-bit
      // load would be a different field's lower half on some alignments.
      if (ds_rd16 (smem, info + kSmemInfoNumItems, &words) == DS_OK) {
        table.info_num_items = words;
      }
    }
  }
  *out = table;
  return DS_OK;
}

// One row of the partition table, plus the header it points at. The header fields are read as part of
// the row because a partition whose "$PRT" contradicts its own table entry is the finding, not an
// implementation detail: SHENG treats exactly that as QcomSmemBadPartition (:42).
struct ds_partition
{
  uint32_t index;
  uint32_t offset;            // from the region base, where the partition header lives
  uint32_t size;
  uint32_t flags;
  uint32_t cacheline;
  uint32_t host0;
  uint32_t host1;
  uint32_t part_magic;        // the "$PRT" found at that offset, whatever it was
  uint32_t part_hosts;        // the header's own host pair, to compare against the table's
  uint32_t part_size;         // ... and its own size
  uint32_t free_uncached;
  uint32_t free_cached;
  int header_ok;              // 1 when magic, hosts and size all agree
};

// Reads table row `index` and follows it. Does not require the partition to be usable: the driver
// prints every row it is allowed to look at, because "there is a partition for 0:2 and it is
// self-consistent" is the answer mode 1 is asking for.
static inline int ds_partition_at (const struct ds_span &smem, const struct ds_ptable &table,
                                   uint32_t index, struct ds_partition *out)
{
  if (index >= table.num_entries) {
    return DS_E_RANGE;
  }
  const uint32_t at = table.at + kSmemPtableSize + index * kSmemPtableEntrySize;
  ds_partition part;
  uint32_t hosts = 0;
  int rc = ds_rd32 (smem, at + kSmemPtEntryOffset, &part.offset);
  rc |= ds_rd32 (smem, at + kSmemPtEntrySize, &part.size);
  rc |= ds_rd32 (smem, at + kSmemPtEntryFlags, &part.flags);
  rc |= ds_rd32 (smem, at + kSmemPtEntryHosts, &hosts);
  rc |= ds_rd32 (smem, at + kSmemPtEntryCacheline, &part.cacheline);
  if (rc) {
    return DS_E_RANGE;
  }
  part.index = index;
  part.host0 = hosts & 0xffff;
  part.host1 = (hosts >> 16) & 0xffff;
  part.part_magic = 0;
  part.part_hosts = 0;
  part.part_size = 0;
  part.free_uncached = 0;
  part.free_cached = 0;
  part.header_ok = 0;
  // A row that points outside the mapping is reported as such rather than followed: this is the
  // bounds question the whole file exists to answer. The two comparisons are written in subtractive
  // form because offset and size both come from memory a remote processor owns, and `offset + size`
  // would overflow rather than fail.
  if (!part.offset || !part.size || part.offset > smem.size || part.size > smem.size - part.offset ||
      part.size < kSmemPartHeaderSize) {
    *out = part;
    return DS_E_RANGE;
  }
  rc = ds_rd32 (smem, part.offset, &part.part_magic);
  rc |= ds_rd32 (smem, part.offset + kSmemPartHosts, &part.part_hosts);
  rc |= ds_rd32 (smem, part.offset + kSmemPartSize, &part.part_size);
  rc |= ds_rd32 (smem, part.offset + kSmemPartFreeUncached, &part.free_uncached);
  rc |= ds_rd32 (smem, part.offset + kSmemPartFreeCached, &part.free_cached);
  if (rc) {
    *out = part;
    return DS_E_RANGE;
  }
  part.header_ok = part.part_magic == kSmemPartMagic &&
                   part.part_hosts == hosts &&
                   part.part_size == part.size &&
                   part.free_uncached >= kSmemPartHeaderSize &&
                   part.free_uncached <= part.free_cached &&
                   part.free_cached <= part.size ? 1 : 0;
  *out = part;
  return DS_OK;
}

// The partition two named hosts share, in either order -- the pairing this AP would use to reach a
// remote pid. Returns the first self-consistent row whose host pair matches.
static inline int ds_find_partition (const struct ds_span &smem, uint32_t host, uint32_t remote,
                                     struct ds_partition *out)
{
  struct ds_ptable table;
  const int rc = ds_ptable (smem, &table);
  if (rc) {
    return rc;
  }
  for (uint32_t i = 0; i < table.num_entries && i < kPtableDumpLimit; i++) {
    ds_partition part;
    if (ds_partition_at (smem, table, i, &part)) {
      continue;
    }
    const int pair = (part.host0 == host && part.host1 == remote) ||
                     (part.host0 == remote && part.host1 == host);
    if (!pair || !part.header_ok) {
      continue;
    }
    *out = part;
    return DS_OK;
  }
  return DS_E_PART;
}

// ------------------------------------------------------------------ an item inside a partition
struct ds_item
{
  uint32_t item;
  uint32_t at;                // absolute offset of the 16-byte entry header
  uint32_t data;              // absolute offset of the data
  uint32_t size;              // data bytes, excluding the padding the allocation rounded up
  uint32_t raw_size;          // the header's size field, padding included
  uint32_t padding_data;
  uint32_t padding_header;
  int cached;                 // 0 uncached (grows up), 1 cached (grows down)
};

// The uncached list, exactly as VEND smem.c:373-440 and SHENG QcomSmem.c:47-70 walk it: entries from
// kSmemPartHeaderSize up to the partition's `offset_free_uncached`, each one 16 bytes of header
// followed by padding_hdr bytes of pad and `size` bytes of data. Every step is checked against the
// end marker rather than trusted, because a single wrong `size` turns a walk into a scan of arbitrary
// memory -- and the last check, `at == end`, is what separates "this list is well-formed" from "I
// stopped somewhere and called it a list".
static inline int ds_walk_uncached (const struct ds_span &smem, const struct ds_partition &part,
                                    uint32_t item, struct ds_item *out, int *found)
{
  *found = 0;
  const uint32_t end = part.free_uncached;
  uint32_t at = kSmemPartHeaderSize;
  while (at < end) {
    uint32_t word0 = 0, rawSize = 0, pads = 0;
    if (at + kSmemEntrySize > part.size) {
      return DS_E_RANGE;
    }
    if (ds_rd32 (smem, part.offset + at, &word0) ||
        ds_rd32 (smem, part.offset + at + 4, &rawSize) ||
        ds_rd32 (smem, part.offset + at + 8, &pads)) {
      return DS_E_RANGE;
    }
    if ((word0 & 0xffff) != kSmemEntryCanary) {
      return DS_E_CANARY;
    }
    const uint32_t padding_data = pads & 0xffff;
    const uint32_t padding_header = (pads >> 16) & 0xffff;
    if (rawSize > part.size || rawSize < padding_data ||
        at + kSmemEntrySize + padding_header + rawSize > end) {
      return DS_E_RANGE;
    }
    if ((word0 >> 16) == item) {
      ds_item entry;
      entry.item = item;
      entry.at = part.offset + at;
      entry.raw_size = rawSize;
      entry.padding_data = padding_data;
      entry.padding_header = padding_header;
      entry.data = part.offset + at + kSmemEntrySize + padding_header;
      entry.size = rawSize - padding_data;
      entry.cached = 0;
      if (entry.data + entry.size > smem.size) {
        return DS_E_RANGE;
      }
      *out = entry;
      *found = 1;
      return DS_OK;
    }
    at += kSmemEntrySize + padding_header + rawSize;
  }
  return at == end ? DS_OK : DS_E_CANARY;
}

// Both lists, in the order the vendor driver tries them (smem.c:704-709: private uncached, private
// cached, then global). The cached side is walked down from the partition's end, one entry header per
// cache line, each item's data sitting immediately below its header -- SHENG QcomSmem.c:80-96, which
// is the implementation that has been run.
static inline int ds_find_item (const struct ds_span &smem, const struct ds_partition &part,
                                uint32_t item, struct ds_item *out)
{
  if (!part.header_ok) {
    return DS_E_PART;
  }
  int found = 0;
  const int rc = ds_walk_uncached (smem, part, item, out, &found);
  if (rc) {
    return rc;
  }
  if (found) {
    return DS_OK;
  }
  const uint32_t cacheline = part.cacheline ? part.cacheline : 8;
  if (cacheline & (cacheline - 1)) {
    return DS_E_PART;                       // SHENG QcomSmem.c:41: a non-power-of-two line is a bad row
  }
  const uint32_t header = (kSmemEntrySize + cacheline - 1) & ~(cacheline - 1);
  if (part.size < header) {
    return DS_E_RANGE;
  }
  uint32_t at = part.size - header;
  while (at > part.free_cached) {
    uint32_t word0 = 0, rawSize = 0, pads = 0;
    if (ds_rd32 (smem, part.offset + at, &word0) ||
        ds_rd32 (smem, part.offset + at + 4, &rawSize) ||
        ds_rd32 (smem, part.offset + at + 8, &pads)) {
      return DS_E_RANGE;
    }
    const uint32_t padding_data = pads & 0xffff;
    if ((word0 & 0xffff) != kSmemEntryCanary || rawSize > at || rawSize < padding_data) {
      return DS_E_CANARY;
    }
    if ((word0 >> 16) == item) {
      ds_item entry;
      entry.item = item;
      entry.at = part.offset + at;
      entry.raw_size = rawSize;
      entry.padding_data = padding_data;
      entry.padding_header = (pads >> 16) & 0xffff;
      entry.data = part.offset + at - rawSize;
      entry.size = rawSize - padding_data;
      entry.cached = 1;
      if (entry.data + entry.size > smem.size || at < rawSize + header) {
        return DS_E_RANGE;
      }
      *out = entry;
      return DS_OK;
    }
    if (at < rawSize + header) {
      return DS_E_RANGE;
    }
    at -= rawSize + header;
  }
  return DS_E_ITEM;
}

// An item resolved through the *global* partition instead: the TOC entry's own offset and size. Used
// when the private pairing does not carry the item, which is the difference between "the heap has no
// AP:DSP partition" and "the item was never allocated".
static inline int ds_find_item_global (const struct ds_span &smem, uint32_t item, struct ds_item *out)
{
  ds_tocEntry entry;
  const int rc = ds_toc (smem, item, &entry);
  if (rc) {
    return rc;
  }
  if (!entry.allocated || !entry.in_span) {
    return DS_E_ITEM;
  }
  ds_item resolved;
  resolved.item = item;
  resolved.at = 0;
  resolved.data = entry.offset;
  resolved.size = entry.size;
  resolved.raw_size = entry.size;
  resolved.padding_data = 0;
  resolved.padding_header = 0;
  resolved.cached = -1;                     // neither list: a global TOC item
  if (entry.aux_base) {
    return DS_E_RANGE;                      // lives in a region this span is not
  }
  *out = resolved;
  return DS_OK;
}

// ------------------------------------------------------------------ the SMP2P item
struct ds_smp2p
{
  uint32_t magic;
  uint32_t version;
  uint32_t features;
  uint32_t local_pid;
  uint32_t remote_pid;
  uint32_t total_entries;
  uint32_t valid_entries;
  uint32_t flags;
  uint32_t data;               // absolute offset the item was read from
  uint32_t available_bytes;    // bytes this span offers from `data` onward
};

// Decodes and validates one candidate. Every rejection is a different report, which matters because
// "no such item", "an item that is not an SMP2P item", and "an SMP2P item for a different pair of
// processors" are the three answers a follow-up round needs to distinguish -- and this board's shipped
// DTB names none of them, so the layout is the only witness.
static inline int ds_smp2p_head (const struct ds_span &smem, uint32_t data,
                                 uint32_t expect_local, uint32_t expect_remote,
                                 struct ds_smp2p *out)
{
  if (!smem.base || data > smem.size) {
    return DS_E_RANGE;
  }
  ds_smp2p item;
  item.data = data;
  item.available_bytes = smem.size - data;
  if (item.available_bytes < kSmp2pItemBytes) {
    *out = item;
    return DS_E_SIZE;
  }
  int rc = ds_rd32 (smem, data + kSmp2pHeaderMagic, &item.magic);
  uint32_t versionWord = 0, pids = 0, counts = 0;
  rc |= ds_rd32 (smem, data + kSmp2pHeaderVersion, &versionWord);
  rc |= ds_rd32 (smem, data + kSmp2pHeaderLocalPid, &pids);
  rc |= ds_rd32 (smem, data + kSmp2pHeaderTotal, &counts);
  rc |= ds_rd32 (smem, data + kSmp2pHeaderFlags, &item.flags);
  if (rc) {
    *out = item;
    return DS_E_RANGE;
  }
  // Byte fields inside words: version is the low byte of its word and features the upper three (VEND
  // smp2p.c:77-78, a u8 followed by a 24-bit field in a __packed struct), and the two pids and the two
  // counts share a word each the same way.
  item.version = versionWord & 0xff;
  item.features = (versionWord >> 8) & 0xffffff;
  item.local_pid = pids & 0xffff;
  item.remote_pid = (pids >> 16) & 0xffff;
  item.total_entries = counts & 0xffff;
  item.valid_entries = (counts >> 16) & 0xffff;
  *out = item;

  if (item.magic != kSmp2pMagic) {
    return DS_E_MAGIC;
  }
  if (item.version != kSmp2pVersion) {
    return DS_E_VERSION;
  }
  if (item.total_entries != kSmp2pMaxEntries || item.valid_entries > kSmp2pMaxEntries) {
    return DS_E_COUNT;
  }
  if (expect_local != 0xffffffff && item.local_pid != expect_local) {
    return DS_E_PID;
  }
  if (expect_remote != 0xffffffff && item.remote_pid != expect_remote) {
    return DS_E_PID;
  }
  return DS_OK;
}

// One entry of an item already validated as an SMP2P item: the 16-byte name and the 32-bit value.
static inline int ds_smp2p_entry (const struct ds_span &smem, const struct ds_smp2p &item,
                                  uint32_t index, char *name, uint32_t nameSize, uint32_t *value)
{
  if (index >= item.valid_entries || !name || nameSize < kSmp2pMaxEntryName + 1) {
    return DS_E_RANGE;
  }
  const uint32_t at = item.data + 20 + index * kSmp2pEntryStride;
  if (at + kSmp2pEntryStride > smem.size) {
    return DS_E_RANGE;
  }
  for (uint32_t i = 0; i < kSmp2pMaxEntryName; i++) {
    name [i] = static_cast<char> (smem.base [at + i]);
  }
  name [kSmp2pMaxEntryName] = '\0';
  // The value is a u32 at entry+16 (VEND smp2p.c:86-87); the item is 4-byte aligned by construction,
  // so this is an aligned word read and not a byte assembly.
  return ds_rd32 (smem, at + kSmp2pEntryValue, value);
}

// ------------------------------------------------------------------ the two image scans
struct ds_image
{
  uint32_t pages;              // pages of the window that were looked at
  uint32_t nonzero;            // ... of those, the ones that are not all zeroes
  uint32_t first_nonzero;      // page index of the first, 0xffffffff when there was none
  uint32_t elf_at;             // byte offset of the first ELF magic, 0xffffffff when none was seen
  uint32_t elf_count;          // how many words of the scanned window read as that magic
  uint32_t head [4];           // the first sixteen bytes, as words
};

// Presence, not identity: whether anything was ever copied into a carve-out during this boot chain.
// Both scans bound themselves to the mapped window and neither reads past a page it has not covered.
static inline int ds_image (const struct ds_span &window, uint32_t pages, struct ds_image *out)
{
  if (!window.base) {
    return DS_E_SPAN;
  }
  struct ds_image image;
  image.pages = 0;
  image.nonzero = 0;
  image.first_nonzero = 0xffffffff;
  image.elf_at = 0xffffffff;
  image.elf_count = 0;
  for (uint32_t i = 0; i < 4; i++) {
    image.head [i] = 0;
  }
  const uint32_t limit = pages * kImagePageSize <= window.size ? pages * kImagePageSize : window.size;
  if (limit >= 16) {
    for (uint32_t i = 0; i < 4; i++) {
      uint32_t word = 0;
      if (ds_rd32 (window, i * 4, &word)) {
        return DS_E_RANGE;
      }
      image.head [i] = word;
    }
  }
  for (uint32_t page = 0; page * kImagePageSize < limit; page++) {
    image.pages++;
    int any = 0;
    for (uint32_t at = page * kImagePageSize; at + 4 <= (page + 1) * kImagePageSize && at + 4 <= limit;
         at += 4) {
      uint32_t word = 0;
      if (ds_rd32 (window, at, &word)) {
        return DS_E_RANGE;
      }
      if (word) {
        any = 1;
      }
      if (word == kImageElfMagic) {
        image.elf_count++;
        if (image.elf_at == 0xffffffff) {
          image.elf_at = at;
        }
      }
    }
    if (any) {
      image.nonzero++;
      if (image.first_nonzero == 0xffffffff) {
        image.first_nonzero = page;
      }
    }
  }
  *out = image;
  return image.pages ? DS_OK : DS_E_RANGE;
}

#endif // CLOVER_DSP_CORE_H
