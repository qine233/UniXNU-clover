// SPDX-License-Identifier: BSD-2-Clause-Patent
// The block-decoding core for drivers/CloverBlock: the SHNGROOT manifest inside the flashed root
// image, the three windows it declares, the bound a caller's block request has to satisfy before a
// command is built, the chunking of a request over the staging buffer, and the accounting of what a
// partially-succeeded command actually delivered -- over plain `const uint8_t *` spans and integers,
// with no IOKit, no bus, no logging and no panic().
//
// WHY THIS FILE IS SEPARATE FROM THE DRIVER
//
// Same reason as PowerArb.h and DspCore.h: the reading that decides the next round is a decode, and a
// decode that has only ever run on the device is a decode whose bugs are discovered on the device. This
// one has the extra hazard the storage area has been paying for since round128: the numbers it produces
// become *card LBAs*. A window off by one row is not a wrong report, it is the kernel mounting a
// filesystem over a run of bytes that belongs to something else, and every errno above it then describes
// a disk this port never asked for. The questions that make the walk safe -- "do the rows this header
// counts fit the blocks I read", "is this offset inside the partition that declared it", "does the sum
// of first+sectors still describe the card" -- are all answerable about this code on a Mac, against a
// manifest the harness wrote, before they are asked of a card.
//
// WHAT IS PROVEN HERE AND WHAT IS NOT
//
// Proven by a host model (drivers/CloverBlock/tests/clover_block_host.cpp, run by
// scripts/test-clover-block.py and refused by scripts/check-clover.py): every refusal above fires for
// its own reason and returns the number the driver prints; the relative-to-partition offset becomes the
// card-absolute LBA the host driver expects; a row set in any order yields the same three windows; and a
// failed command contributes only the blocks the card said it took -- the rule round157 had to teach
// this file the hard way, when `done += read` reported words that never reached the caller.
//
// Not proven here, and not provable here: that this board's card holds a manifest this parser accepts.
// The shape comes from the flashed root image the host can read (scripts/make-clover-root-image.py and
// round128's measurements), the 128-byte row stride and the field offsets from that same reading, and
// nothing in this file is a guess about a second layout. If the card's manifest is a different release's
// manifest, the version refusal is the answer, and the frame says so.
//
// NOTHING HERE TOUCHES A BUS OR A REGISTRY
//
// No IOKit type, no `volatile`, no allocation, no print, no atomic. The driver owns the read that fills
// the span, the atomic stores that publish a window to other CPUs, and the panic that ends a boot; this
// file owns the arithmetic those decisions are made from. That split is what makes the same code testable
// against a `uint8_t` array -- the reason the vendor's own walker (package/QcomSmem.c in sheng, DspCore.h
// here) is written the same way.
#ifndef CLOVER_BLOCK_CORE_H
#define CLOVER_BLOCK_CORE_H

#include <stdint.h>

// One code per way a step can end, and one number beside it. The codes are what the driver maps onto its
// panic tags; the pairing is fixed because a frame has to be able to tell "no manifest" from "a manifest
// whose rows do not fit" from "a window that runs off the partition" -- three different findings about the
// same flashed image, and round128 showed that collapsing them into one refusal costs a whole boot.
enum {
  BLK_OK = 0,
  BLK_E_MAGIC = 1,     // bytes 0..7 are not SHNGROOT: this is not a root-image manifest at all
  BLK_E_VERSION = 2,   // a manifest version this code does not implement
  BLK_E_COUNT = 3,     // zero rows, more rows than the format allows, or rows that would not fit the span
  BLK_E_ATTR = 4,      // a row's block size or read-only flag contradicts what this window must be
  BLK_E_BOUNDS = 5,    // the row's offset/size is not a whole number of blocks inside its own partition
  BLK_E_GEOMETRY = 6,  // the row is inside the partition but the partition-relative sum leaves the card
  BLK_E_NAME = 7,      // no row named the root window
  BLK_E_SYSTEM = 8,    // no row named the SystemOS window, in a mode that requires it
  BLK_E_DATA = 9       // no row named the Data window, in a mode that requires it
};

// Which of the three windows a refusal is about. The driver spells the tag from this -- "root-attr",
// "system-attr", "data-attr" -- so one code still yields the three distinct frames the boot needs.
enum {
  BLK_W_OS = 0,
  BLK_W_SYSTEM = 1,
  BLK_W_DATA = 2
};

// The manifest's own geometry, in bytes, all of it transcribed from the flashed root image this port
// builds (scripts/make-clover-root-image.py) and read back by round128's host-side parse of the same
// bytes. kBlkSpanBytes is also the size of the read the driver issues, which is why the row-count check
// below can bound the walk against the span it was handed.
enum {
  kBlkBytes = 512,
  kBlkBlockWords = kBlkBytes / 4,
  kBlkSpanBlocks = 8,
  kBlkSpanBytes = kBlkSpanBlocks * kBlkBytes,
  kBlkMagicBytes = 8,
  kBlkVersionAt = 8,
  kBlkCountAt = 12,
  kBlkRowsAt = 24,
  kBlkRowStride = 128,
  kBlkRowsMax = 4,
  kBlkRowNameAt = 0,
  kBlkRowOffsetAt = 32,
  kBlkRowSizeAt = 40,
  kBlkRowBlockSizeAt = 48,
  kBlkRowFlagsAt = 52,
  kBlkRowDigestAt = 56,
  kBlkRowDigestBytes = 32,
  kBlkVersion = 1,
  kBlkFlagReadOnly = 1,
  kBlkFlagWritable = 0
};

// Verdicts for a caller's request, before any command exists.
enum {
  BLK_REQ_OK = 0,
  BLK_REQ_BUFFER = 1,  // no usable buffer, or a direction this device does not serve
  BLK_REQ_RANGE = 2    // the request is not inside the capacity this device claims
};

// What a partially-succeeded command delivered, in the two shapes the caller has to be told about.
enum {
  BLK_DONE_ALL = 0,    // every block of this chunk reached the media (write) or the caller (read)
  BLK_DONE_PARTIAL = 1,// some whole blocks reached the media; the rest is the caller's to retry
  BLK_DONE_NONE = 2    // nothing was delivered, so `done` must not move at all
};

// The three names this boot binds a window to, with their lengths *including* the terminator -- the
// comparison is a memcmp over exactly that many bytes, which is why "OS" is three and not two. The
// driver keeps the literals; the core is told what to look for, so a rename stays a one-place change.
struct blk_names {
  const char *os;
  uint32_t os_len;
  const char *system;
  uint32_t system_len;
  const char *data;
  uint32_t data_len;
};

// The card as this boot measured it: where the partition holding the manifest starts, how big it is, and
// how big the whole card is. The last one is what turns "the manifest says 0x4b000000 bytes at offset
// 0x1000" from a claim into a window that can actually be read.
struct blk_card {
  uint32_t partition_first;
  uint32_t partition_sectors;
  uint32_t card_sectors;
};

// One row's declared identity, copied out so the kernel can name the exact bytes a manifest pointed at
// without rehashing a multi-gigabyte window at every exec.
struct blk_entry {
  uint8_t digest[kBlkRowDigestBytes];
  uint64_t offset;
  uint64_t size;
};

// The three windows, in card-absolute LBAs, plus which rows were actually found. A window whose sectors
// field is zero was not found; the entries belong to their windows and are only meaningful when it was.
struct blk_windows {
  uint32_t os_first;
  uint32_t os_sectors;
  uint32_t system_first;
  uint32_t system_sectors;
  uint32_t data_first;
  uint32_t data_sectors;
  struct blk_entry os_entry;
  struct blk_entry system_entry;
  struct blk_entry data_entry;
  int have_os;
  int have_system;
  int have_data;
};

static inline uint32_t blk_le32 (const uint8_t *at)
{
  return static_cast<uint32_t> (at[0]) | (static_cast<uint32_t> (at[1]) << 8) |
         (static_cast<uint32_t> (at[2]) << 16) | (static_cast<uint32_t> (at[3]) << 24);
}

static inline uint64_t blk_le64 (const uint8_t *at)
{
  return static_cast<uint64_t> (blk_le32 (at)) | (static_cast<uint64_t> (blk_le32 (at + 4)) << 32);
}

// A name, byte for byte, over exactly `len` bytes -- terminator included, so a row whose name is "OSx"
// does not match "OS\0" and a row that is the prefix of another is refused rather than guessed at.
static inline int blk_name_eq (const uint8_t *row, uint32_t at, const char *name, uint32_t len)
{
  for (uint32_t index = 0; index < len; index++) {
    if (row[at + index] != static_cast<uint8_t> (name[index])) {
      return 0;
    }
  }
  return 1;
}

// The first kBlkMagicBytes bytes of a row, matched against the SHNGROOT signature the way the driver
// did it before this file existed: a byte loop, not a word load, because the signature is eight ASCII
// characters and eight is exactly the width where a word-read transcription error is invisible.
static inline int blk_has_root_magic (const uint8_t *bytes)
{
  static const char kMagic[kBlkMagicBytes] = { 'S', 'H', 'N', 'G', 'R', 'O', 'O', 'T' };
  return blk_name_eq (bytes, 0, kMagic, kBlkMagicBytes);
}

static inline void blk_copy_entry (struct blk_entry *slot, const uint8_t *row)
{
  for (uint32_t index = 0; index < kBlkRowDigestBytes; index++) {
    slot->digest[index] = row[kBlkRowDigestAt + index];
  }
  slot->offset = blk_le64 (row + kBlkRowOffsetAt);
  slot->size = blk_le64 (row + kBlkRowSizeAt);
}

// The manifest walk. Returns BLK_OK with all three windows filled (the two optional ones only when a row
// named them) or the first refusal it met, with `*window` naming the window it was refused in -- or
// BLK_W_OS when the refusal is about the manifest as a whole -- and `*bad` carrying the one number the
// frame should print. The order of the checks is the order the driver's panic steps were in before this
// file existed, and it is load-bearing: a boot that reports "root-count" is a boot whose read succeeded
// and whose header was intact, which is a different next question than "root-read".
static inline int blk_parse_manifest (const uint8_t *bytes, uint32_t nbytes,
                                      const struct blk_card *card, const struct blk_names *names,
                                      int require_system, int require_data,
                                      struct blk_windows *out, int *window, uint64_t *bad)
{
  *window = BLK_W_OS;
  *bad = 0;
  for (uint32_t index = 0; index < sizeof (*out); index++) {
    reinterpret_cast<uint8_t *> (out)[index] = 0;
  }

  if (!blk_has_root_magic (bytes)) {
    *bad = blk_le32 (bytes);
    return BLK_E_MAGIC;
  }
  const uint32_t version = blk_le32 (bytes + kBlkVersionAt);
  if (version != kBlkVersion) {
    *bad = version;
    return BLK_E_VERSION;
  }
  const uint32_t count = blk_le32 (bytes + kBlkCountAt);
  // Three ways the row table can be impossible, and one refusal because all three mean "the rows I was
  // promised are not the bytes I hold": no rows, more rows than this format has, and -- the one a board
  // cannot show on its own -- rows that would run past the span the driver read. The last is dead at
  // today's constants (24 + 4 * 128 = 536 < 4096), and it is here because the day kBlkRowsMax or the read
  // length moves, it stops being dead; the model proves both halves.
  if (!count || count > kBlkRowsMax ||
      static_cast<uint64_t> (kBlkRowsAt) +
        static_cast<uint64_t> (count) * kBlkRowStride > nbytes) {
    *bad = count;
    return BLK_E_COUNT;
  }

  const uint64_t partition_bytes = static_cast<uint64_t> (card->partition_sectors) * kBlkBytes;
  for (uint32_t index = 0; index < count; index++) {
    const uint8_t *row = bytes + kBlkRowsAt + kBlkRowStride * index;
    int which = -1;
    if (blk_name_eq (row, kBlkRowNameAt, names->os, names->os_len)) {
      which = BLK_W_OS;
    } else if (blk_name_eq (row, kBlkRowNameAt, names->system, names->system_len)) {
      which = BLK_W_SYSTEM;
    } else if (blk_name_eq (row, kBlkRowNameAt, names->data, names->data_len)) {
      which = BLK_W_DATA;
    }
    if (which < 0) {
      continue;
    }
    // Both sealed windows are read-only images and say so in their rows. Data is the first window the
    // owner mount attaches for writing: a read-only row there would mount, then fail at the first write,
    // so the manifest is refused here instead -- with its own tag.
    const uint32_t expect_flags = which == BLK_W_DATA ? kBlkFlagWritable : kBlkFlagReadOnly;
    const uint64_t offset = blk_le64 (row + kBlkRowOffsetAt);
    const uint64_t size = blk_le64 (row + kBlkRowSizeAt);
    if (blk_le32 (row + kBlkRowBlockSizeAt) != kBlkBytes ||
        blk_le32 (row + kBlkRowFlagsAt) != expect_flags) {
      *window = which;
      *bad = index;
      return BLK_E_ATTR;
    }
    // Subtract on the right-hand side so an offset and a size that overflow cannot pass, and refuse the
    // manifest's own bytes as a window: a row that points at the first 4096 bytes of the partition points
    // at the table that declares it, not at a filesystem.
    if (offset < kBlkSpanBytes || (offset % kBlkBytes) || size < kBlkBytes || (size % kBlkBytes) ||
        offset > partition_bytes || size > partition_bytes - offset) {
      *window = which;
      *bad = offset;
      return BLK_E_BOUNDS;
    }
    const uint64_t first = card->partition_first + offset / kBlkBytes;
    const uint64_t sectors = size / kBlkBytes;
    if (sectors > 0xffffffffull || first > 0xffffffffull || first + sectors > card->card_sectors) {
      *window = which;
      *bad = sectors;
      return BLK_E_GEOMETRY;
    }
    struct blk_entry *entry = which == BLK_W_OS ? &out->os_entry
                                : which == BLK_W_SYSTEM ? &out->system_entry : &out->data_entry;
    blk_copy_entry (entry, row);
    if (which == BLK_W_OS) {
      out->os_first = static_cast<uint32_t> (first);
      out->os_sectors = static_cast<uint32_t> (sectors);
      out->have_os = 1;
    } else if (which == BLK_W_SYSTEM) {
      out->system_first = static_cast<uint32_t> (first);
      out->system_sectors = static_cast<uint32_t> (sectors);
      out->have_system = 1;
    } else {
      out->data_first = static_cast<uint32_t> (first);
      out->data_sectors = static_cast<uint32_t> (sectors);
      out->have_data = 1;
    }
    // Keep looking: the manifest holds one row per window and any order is valid.
  }
  if (!out->have_os) {
    *bad = count;
    return BLK_E_NAME;
  }
  if (require_system && !out->have_system) {
    *window = BLK_W_SYSTEM;
    *bad = count;
    return BLK_E_SYSTEM;
  }
  if (require_data && !out->have_data) {
    *window = BLK_W_DATA;
    *bad = count;
    return BLK_E_DATA;
  }
  return BLK_OK;
}

// The bound a request has to satisfy before a command is built, over the numbers the caller's descriptor
// and this device's capacity actually are. `has_buffer` is the caller's side of the question -- is there
// a descriptor at all, pointing the way this device serves -- and it is a separate argument because a
// descriptor that exists and is empty is *not* the same reading as no descriptor: the first is a request
// this device accepts and completes with nothing delivered, the second is one it refuses outright.
// `accepted` is what the loop will try to deliver: the caller's block count, clamped to the capacity
// *and* to the buffer it handed over -- and it is legal for that to come back zero. A buffer smaller than
// one block yields an accepted request that completes with no blocks delivered; that is the behaviour
// this boot shipped with before this file existed and it is kept deliberately, because inventing a
// refusal here is a policy question the frame cannot answer. The model asserts it rather than hiding it.
static inline int blk_admit_request (int has_buffer, uint64_t block, uint64_t nblks,
                                     uint64_t buffer_bytes, uint32_t sectors, uint64_t *accepted)
{
  if (!has_buffer) {
    *accepted = 0;
    return BLK_REQ_BUFFER;
  }
  if (block >= sectors || nblks > sectors - block) {
    *accepted = 0;
    return BLK_REQ_RANGE;
  }
  const uint64_t fitting = buffer_bytes / kBlkBytes;
  *accepted = nblks > fitting ? fitting : nblks;
  return BLK_REQ_OK;
}

// How many blocks of a request the next command may carry. The limit is the staging buffer's, and the
// caller owns it; passing it in is what lets the model ask "and if the buffer were half that size".
static inline uint32_t blk_next_chunk (uint64_t remaining, uint32_t limit)
{
  if (!limit) {
    return 1;
  }
  return remaining > limit ? limit : static_cast<uint32_t> (remaining);
}

// A write: the card took `delivered` whole blocks of this chunk. Those blocks did reach the media, so
// they are the only part of the chunk that is done, and a chunk that fell short ends the request -- the
// rest is the caller's to retry. A `copied` of zero means the caller's bytes never even reached staging,
// which delivers nothing regardless of what the card would have taken.
static inline int blk_account_write (uint32_t copied, uint32_t delivered, uint32_t chunk, uint64_t *done)
{
  if (!copied) {
    return BLK_DONE_NONE;
  }
  if (delivered != chunk) {
    *done += delivered;
    return delivered ? BLK_DONE_PARTIAL : BLK_DONE_NONE;
  }
  *done += chunk;
  return BLK_DONE_ALL;
}

// A read: the words the card did not return never reach the caller's buffer, so a short read delivers
// *nothing* -- not `delivered` blocks, which is the distinction round157 had to add after r156's
// `done += read` told a completion about data that had not arrived. Only a full chunk that also copied
// counts as done.
static inline int blk_account_read (uint32_t delivered, uint32_t copied, uint32_t chunk, uint64_t *done)
{
  if (delivered != chunk || !copied) {
    return BLK_DONE_NONE;
  }
  *done += chunk;
  return BLK_DONE_ALL;
}

// Does this write land inside the log window? The log's own records are the only writes on this card with
// no filesystem above them to issue a sync, and the boot that wrote the last one is the boot that is
// ending -- so a write that touches the window is committed here instead of waiting for a sync that never
// comes. Half-open ranges: a request ending exactly at the window's first block does not touch it, and a
// request that starts inside and runs past the end does.
static inline int blk_overlaps (uint64_t first, uint64_t count, uint32_t window_first,
                                uint32_t window_sectors)
{
  if (!window_sectors || !count) {
    return 0;
  }
  return first < static_cast<uint64_t> (window_first) + window_sectors &&
         first + count > window_first;
}

// The last block a request may name. Kept next to blk_admit_request because the two are one decision
// expressed twice: IOBlockStorageDriver asks reportMaxValidBlock() for the same bound it will later check
// a request against, and a device that answers the first question differently from the second is a device
// that refuses its own legal requests.
static inline int blk_last_block (uint32_t sectors, uint64_t *max_block)
{
  if (!sectors) {
    return -1;
  }
  *max_block = static_cast<uint64_t> (sectors) - 1;
  return 0;
}

#endif
