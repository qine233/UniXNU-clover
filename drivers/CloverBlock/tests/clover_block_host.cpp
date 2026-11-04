// Host model of this board's root-image manifest and block request path, driving
// drivers/CloverBlock/BlockCore.h against a span the harness writes itself. Ported in shape from
// drivers/CloverDsp/tests/clover_dsp_host.cpp and drivers/CloverPower/tests/clover_power_host.cpp, which
// do the same thing for SMEM and for the SPMI arbiter: the split exists because a decode that has only
// ever run on the device is a decode whose bugs are discovered on the device -- and because in this file
// the decode's output is not a report but a *card LBA*.
//
// WHAT THIS IS FOR, SPECIFICALLY
//
// CloverBlock decides three things that cannot be checked after the fact on a boot that is already dead.
// (1) Which bytes on the card are which window. The manifest's offsets are relative to the userdata
// partition while drivers/CloverSdhci takes card-absolute LBAs, so one missing addition mounts a
// filesystem over the wrong extent and every errno above it then describes a disk this port never asked
// for. (2) Whether a row is trustworthy at all: a refusal per reason (magic, version, row count, row
// bounds, card geometry) is what let round128 tell "nothing was flashed" from "a different release's
// manifest" from "this partition is not the one the image was sized for" -- three findings, three tags,
// and the bounds arithmetic has to reject a row without dereferencing it. (3) What a partially-succeeded
// command delivered: round156 counted blocks the card never returned and r157 had to take that back, so
// the rule "a short read delivers nothing, a partial write delivers exactly what it took" is now one
// function with its own cases here.
//
// WHY THE FAILURE PATHS ARE BUILT RATHER THAN ARGUED ABOUT
//
// Every refusal below is produced by writing different bytes into the same span, which is the whole
// advantage of a model: BLK_E_MAGIC, BLK_E_VERSION, BLK_E_COUNT, BLK_E_ATTR, BLK_E_BOUNDS and
// BLK_E_GEOMETRY each have a manifest built to cause it, and each asserts the number the frame will
// print beside it. On the board these paths are reached at most once per boot, always by panic, and
// never twice with the same input.
//
// WHAT THE MODEL DELIBERATELY DOES NOT REPRODUCE
//
// Real userdata extents are hundreds of megabytes and its rows are 4 MiB-aligned; the model uses whatever
// numbers make a branch reachable, because the code bounds on arithmetic rather than on alignment. It also
// keeps one claim that is not a design decision but a measurement of today's behaviour: an accepted
// request whose buffer is smaller than one block completes with zero blocks delivered and no error. That
// is what this boot has always done, so the model asserts it, and the next round that wants a refusal
// there can see exactly which line it is changing.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../BlockCore.h"

// The card, as a boot would report it: userdata starts at LBA 0x100000 and is 1 MiB of sectors long, the
// whole card is twice that. Both are chosen so that a window can be inside its partition and still miss
// the card, which is the distinction BLK_E_GEOMETRY exists to make.
#define kPartitionFirst 0x100000u
#define kPartitionSectors 0x80000u
#define kCardSectors 0x200000u

// Three windows inside that partition, in bytes relative to its start. 0x2000 is the first legal offset
// because the manifest's own 4096 bytes may not be claimed as a window.
#define kOsOffset 0x2000u
#define kOsSize 0x10000u
#define kSystemOffset 0x20000u
#define kSystemSize 0x8000u
#define kDataOffset 0x30000u
#define kDataSize 0x40000u

static uint8_t gSpan[kBlkSpanBytes];
static const char kOsName[3] = { 'O', 'S', 0 };
static const char kSystemName[] = "SystemOS";
static const char kDataName[] = "Data";

static void wr32 (uint32_t at, uint32_t value)
{
  for (uint32_t index = 0; index < 4; index++) {
    gSpan[at + index] = static_cast<uint8_t> (value >> (8 * index));
  }
}

static void wr64 (uint32_t at, uint64_t value)
{
  wr32 (at, static_cast<uint32_t> (value));
  wr32 (at + 4, static_cast<uint32_t> (value >> 32));
}

static void wrText (uint32_t at, const char *text, uint32_t len)
{
  for (uint32_t index = 0; index < len; index++) {
    gSpan[at + index] = static_cast<uint8_t> (text[index]);
  }
}

static uint32_t rowAt (uint32_t index)
{
  return kBlkRowsAt + index * kBlkRowStride;
}

// One row, exactly as the flashed root image writes it: name, offset, size, block size, flags, digest.
static void putRow (uint32_t index, const char *name, uint32_t name_len, uint64_t offset, uint64_t size,
                    uint32_t block_size, uint32_t flags, uint8_t digest_seed)
{
  const uint32_t at = rowAt (index);
  memset (gSpan + at, 0, kBlkRowStride);
  wrText (at + kBlkRowNameAt, name, name_len);
  wr64 (at + kBlkRowOffsetAt, offset);
  wr64 (at + kBlkRowSizeAt, size);
  wr32 (at + kBlkRowBlockSizeAt, block_size);
  wr32 (at + kBlkRowFlagsAt, flags);
  for (uint32_t byte = 0; byte < kBlkRowDigestBytes; byte++) {
    gSpan[at + kBlkRowDigestAt + byte] = static_cast<uint8_t> (digest_seed + byte);
  }
}

// A manifest with `count` rows: OS first, then SystemOS and Data when asked for, each with its own digest
// seed so the model can tell three copied entries apart.
static void putManifest (uint32_t count, uint32_t version = kBlkVersion,
                         const char *magic = "SHNGROOT", bool with_system = true,
                         bool with_data = true)
{
  memset (gSpan, 0, sizeof (gSpan));
  wrText (0, magic, kBlkMagicBytes);
  wr32 (kBlkVersionAt, version);
  wr32 (kBlkCountAt, count);
  if (count >= 1) {
    putRow (0, kOsName, sizeof (kOsName), kOsOffset, kOsSize, kBlkBytes, kBlkFlagReadOnly, 0xA0);
  }
  if (count >= 2 && with_system) {
    putRow (1, kSystemName, sizeof (kSystemName), kSystemOffset, kSystemSize, kBlkBytes,
            kBlkFlagReadOnly, 0xB0);
  }
  if (count >= 3 && with_data) {
    putRow (2, kDataName, sizeof (kDataName), kDataOffset, kDataSize, kBlkBytes,
            kBlkFlagWritable, 0xC0);
  }
}

static struct blk_names kNames = { kOsName, sizeof (kOsName), kSystemName, sizeof (kSystemName),
                                   kDataName, sizeof (kDataName) };
static struct blk_card kCard = { kPartitionFirst, kPartitionSectors, kCardSectors };

static int parse (struct blk_windows *out, int *window, uint64_t *bad, int require_system = 0,
                  int require_data = 0, uint32_t span_bytes = kBlkSpanBytes)
{
  return blk_parse_manifest (gSpan, span_bytes, &kCard, &kNames, require_system, require_data,
                             out, window, bad);
}

// --------------------------------------------------------------------------- the manifest that boots

static void walk_good (void)
{
  putManifest (3);
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  assert (parse (&out, &window, &bad) == BLK_OK);
  // Partition-relative in, card-absolute out. These three numbers are the whole product of this file:
  // everything the kernel mounts sits or falls on them.
  assert (out.os_first == kPartitionFirst + kOsOffset / kBlkBytes);
  assert (out.os_sectors == kOsSize / kBlkBytes);
  assert (out.system_first == kPartitionFirst + kSystemOffset / kBlkBytes);
  assert (out.system_sectors == kSystemSize / kBlkBytes);
  assert (out.data_first == kPartitionFirst + kDataOffset / kBlkBytes);
  assert (out.data_sectors == kDataSize / kBlkBytes);
  assert (out.have_os && out.have_system && out.have_data);
  // The identity each row declared travels with its window: the kernel's per-exec seed check reads the
  // digest and the offset back out of the driver's copy, so a transcription here silently changes what
  // the seal says it covered.
  assert (out.os_entry.offset == kOsOffset && out.os_entry.size == kOsSize);
  assert (out.os_entry.digest[0] == 0xA0 && out.os_entry.digest[31] == 0xBF);
  assert (out.system_entry.digest[0] == 0xB0 && out.system_entry.digest[16] == 0xC0);
  assert (out.data_entry.digest[31] == 0xDF);
  assert (bad == 0 && window == BLK_W_OS);
}

static void walk_order (void)
{
  // Any order is valid, because the manifest names its rows instead of numbering them: Data first,
  // SystemOS second, OS last must give the identical three windows.
  putManifest (3);
  uint8_t saved[kBlkRowStride * 3];
  memcpy (saved, gSpan + kBlkRowsAt, sizeof (saved));
  memcpy (gSpan + kBlkRowsAt, saved + 2 * kBlkRowStride, kBlkRowStride);
  memcpy (gSpan + kBlkRowsAt + kBlkRowStride, saved, kBlkRowStride);
  memcpy (gSpan + kBlkRowsAt + 2 * kBlkRowStride, saved + kBlkRowStride, kBlkRowStride);
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  assert (parse (&out, &window, &bad) == BLK_OK);
  assert (out.os_first == kPartitionFirst + kOsOffset / kBlkBytes && out.have_data && out.have_system);
  assert (out.data_entry.digest[0] == 0xC0 && out.os_entry.digest[0] == 0xA0);
}

static void walk_extra_rows (void)
{
  // A fourth row this boot has no window for is ignored, not refused: the format allows up to four and the
  // manifest is Apple's to fill.
  putManifest (4);
  putRow (3, "Factory", 8, kBlkSpanBytes, 0x1000, kBlkBytes, kBlkFlagReadOnly, 0x10);
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  assert (parse (&out, &window, &bad) == BLK_OK);
  assert (out.have_os && out.have_system && out.have_data);
}

// --------------------------------------------------------------------------- each refusal, on its own

static void walk_magic (void)
{
  putManifest (3, kBlkVersion, "SHNGROOX");
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  assert (parse (&out, &window, &bad) == BLK_E_MAGIC);
  // The frame prints the first four bytes as a word, which is how a boot tells "all zeroes, nothing was
  // flashed" from "ASCII that is almost the signature".
  assert (bad == 0x474e4853u);          // 'S','H','N','G' read little-endian
  assert (window == BLK_W_OS);
}

static void walk_version (void)
{
  putManifest (3, 2);
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  assert (parse (&out, &window, &bad) == BLK_E_VERSION);
  assert (bad == 2 && window == BLK_W_OS);
}

static void walk_count (void)
{
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  putManifest (0);
  assert (parse (&out, &window, &bad) == BLK_E_COUNT);
  assert (bad == 0);
  putManifest (5);
  assert (parse (&out, &window, &bad) == BLK_E_COUNT);
  assert (bad == 5);
  // The third way the row table can be impossible: a header that promises more rows than the span the
  // driver read holds. At today's constants this cannot happen -- 24 + 4 * 128 = 536 bytes against the
  // 4096 the driver reads, which the case above this one proves accepted -- and it is here because the
  // day either constant moves it stops being arithmetic and becomes a walk off the end of a buffer.
  putManifest (4);
  assert (parse (&out, &window, &bad, 0, 0, kBlkSpanBytes) == BLK_OK);
  assert (parse (&out, &window, &bad, 0, 0, 500) == BLK_E_COUNT);
  assert (bad == 4);
}

static void walk_attr (void)
{
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  // A sealed window that claims it may be written.
  putManifest (3);
  putRow (0, kOsName, sizeof (kOsName), kOsOffset, kOsSize, kBlkBytes, kBlkFlagWritable, 0xA0);
  assert (parse (&out, &window, &bad) == BLK_E_ATTR);
  assert (window == BLK_W_OS && bad == 0);
  // The Data window claiming read-only: the refusal round159 exists for, because a mount that succeeds
  // and then fails at its first write is a much worse finding than a boot that stops here.
  putManifest (3);
  putRow (2, kDataName, sizeof (kDataName), kDataOffset, kDataSize, kBlkBytes, kBlkFlagReadOnly, 0xC0);
  assert (parse (&out, &window, &bad) == BLK_E_ATTR);
  assert (window == BLK_W_DATA && bad == 2);
  // A row whose block size is not the 512 every offset in this file divides by.
  putManifest (3);
  putRow (1, kSystemName, sizeof (kSystemName), kSystemOffset, kSystemSize, 4096, kBlkFlagReadOnly, 0xB0);
  assert (parse (&out, &window, &bad) == BLK_E_ATTR);
  assert (window == BLK_W_SYSTEM && bad == 1);
}

static void walk_bounds (void)
{
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  // Offset inside the manifest's own bytes: a window that points at the table declaring it.
  putManifest (3);
  putRow (0, kOsName, sizeof (kOsName), kBlkBytes, kOsSize, kBlkBytes, kBlkFlagReadOnly, 0xA0);
  assert (parse (&out, &window, &bad) == BLK_E_BOUNDS);
  assert (window == BLK_W_OS && bad == kBlkBytes);
  // An offset that is not a whole block, so the window would start mid-sector.
  putManifest (3);
  putRow (0, kOsName, sizeof (kOsName), kBlkSpanBytes + 1, kOsSize, kBlkBytes, kBlkFlagReadOnly, 0xA0);
  assert (parse (&out, &window, &bad) == BLK_E_BOUNDS);
  assert (bad == kBlkSpanBytes + 1);
  // A size that is not a whole block either.
  putManifest (3);
  putRow (2, kDataName, sizeof (kDataName), kDataOffset, kDataSize + 7, kBlkBytes,
          kBlkFlagWritable, 0xC0);
  assert (parse (&out, &window, &bad) == BLK_E_BOUNDS);
  assert (window == BLK_W_DATA);
  // A row whose offset is the whole partition: legal-looking, and empty of room, so `size` cannot fit.
  // The subtraction on the right of the comparison is what makes this refuse instead of wrapping.
  putManifest (3);
  const uint64_t edge = static_cast<uint64_t> (kPartitionSectors) * kBlkBytes;
  putRow (0, kOsName, sizeof (kOsName), edge, kBlkBytes, kBlkBytes, kBlkFlagReadOnly, 0xA0);
  assert (parse (&out, &window, &bad) == BLK_E_BOUNDS);
  assert (bad == edge);
  // And the overflow attempt the same clause has to survive: an offset far past the partition whose
  // addition would otherwise wrap into it.
  putManifest (3);
  putRow (0, kOsName, sizeof (kOsName), 0xffffffffffff0000ull, kBlkBytes, kBlkBytes,
          kBlkFlagReadOnly, 0xA0);
  assert (parse (&out, &window, &bad) == BLK_E_BOUNDS);
}

static void walk_geometry (void)
{
  // A window completely inside its partition, on a userdata partition the card does not have room for.
  // This is the reading round185's logdump geometry and round128's GPT row disagree about in real life,
  // which is why it needs its own tag rather than being folded into the bounds.
  struct blk_card card = { kCardSectors - 64u, kPartitionSectors, kCardSectors };
  putManifest (3);
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  assert (blk_parse_manifest (gSpan, kBlkSpanBytes, &card, &kNames, 0, 0, &out, &window, &bad)
          == BLK_E_GEOMETRY);
  assert (window == BLK_W_OS && bad == kOsSize / kBlkBytes);
}

static void walk_names (void)
{
  struct blk_windows out;
  int window = -1;
  uint64_t bad = 0;
  // No OS row at all: the root window is the one this boot cannot do without, in any mode.
  putManifest (3);
  putRow (0, "Other", 6, kOsOffset, kOsSize, kBlkBytes, kBlkFlagReadOnly, 0xA0);
  assert (parse (&out, &window, &bad, 1, 1) == BLK_E_NAME);
  assert (bad == 3 && window == BLK_W_OS);
  // Present but not required: the same manifest in a probe mode must not refuse for a missing Data row.
  putManifest (3, kBlkVersion, "SHNGROOT", true, false);
  assert (parse (&out, &window, &bad) == BLK_OK);
  assert (!out.have_data);
  // Required and missing, each of the two optional windows in its own code.
  putManifest (3, kBlkVersion, "SHNGROOT", true, false);
  assert (parse (&out, &window, &bad, 0, 1) == BLK_E_DATA);
  assert (window == BLK_W_DATA && bad == 3);
  putManifest (3, kBlkVersion, "SHNGROOT", false, true);
  assert (parse (&out, &window, &bad, 1, 0) == BLK_E_SYSTEM);
  assert (window == BLK_W_SYSTEM && bad == 3);
  // A name that is a prefix of a window's is not that window: "OSx" must not satisfy the root row, and
  // the terminator is the only reason it cannot. This is the trap a NUL-insensitive compare falls into.
  putManifest (3);
  putRow (0, "OSx", 4, kOsOffset, kOsSize, kBlkBytes, kBlkFlagReadOnly, 0xA0);
  assert (parse (&out, &window, &bad) == BLK_E_NAME);
  putManifest (3);
  putRow (1, "SystemOSx", 10, kSystemOffset, kSystemSize, kBlkBytes, kBlkFlagReadOnly, 0xB0);
  assert (parse (&out, &window, &bad, 1, 0) == BLK_E_SYSTEM);
}

// --------------------------------------------------------------------------- the request path

static void request_bound (void)
{
  uint64_t accepted = 12345;
  // No descriptor at all is the outright refusal; a descriptor of zero length is not, and the argument
  // being separate is the only reason the two do not collapse into each other.
  assert (blk_admit_request (0, 0, 1, 4096, 0x1000, &accepted) == BLK_REQ_BUFFER);
  assert (accepted == 0);
  // The end of the device's capacity, expressed three ways.
  assert (blk_admit_request (1, 0x1000, 1, 4096, 0x1000, &accepted) == BLK_REQ_RANGE);
  assert (blk_admit_request (1, 0x0fff, 2, 4096, 0x1000, &accepted) == BLK_REQ_RANGE);
  assert (blk_admit_request (1, 0x0fff, 1, 4096, 0x1000, &accepted) == BLK_REQ_OK);
  assert (accepted == 1);
  // A zero sector count refuses everything rather than admitting a request whose bound is a wrap.
  assert (blk_admit_request (1, 0, 1, 4096, 0, &accepted) == BLK_REQ_RANGE);
  // Clamped to the buffer, and only to it.
  assert (blk_admit_request (1, 0, 5, 1000, 0x1000, &accepted) == BLK_REQ_OK);
  assert (accepted == 1);                 // 1000 bytes is one whole block
  assert (blk_admit_request (1, 0, 5, 1024, 0x1000, &accepted) == BLK_REQ_OK);
  assert (accepted == 2);
  // The documented hole, asserted rather than hidden: a descriptor with no room for one block is accepted
  // and completes with nothing delivered and no error. Today's behaviour, kept on purpose -- refusing it
  // is a policy call the frame cannot make.
  assert (blk_admit_request (1, 0, 5, 0, 0x1000, &accepted) == BLK_REQ_OK);
  assert (accepted == 0);
}

static void request_chunks (void)
{
  assert (blk_next_chunk (300, 128) == 128);
  assert (blk_next_chunk (128, 128) == 128);
  assert (blk_next_chunk (100, 128) == 100);
  assert (blk_next_chunk (1, 128) == 1);
  // A caller with no staging buffer of its own gets one block per command, which is the r156 shape
  // rather than a third rule.
  assert (blk_next_chunk (57, 0) == 1);
}

static void request_delivery (void)
{
  uint64_t done = 0;
  // Write: a full chunk the card took.
  assert (blk_account_write (1, 128, 128, &done) == BLK_DONE_ALL);
  assert (done == 128);
  // Write: the card took 5 whole blocks and then the command failed. Those five are on the media, so they
  // are done; the other 123 belong to the caller's retry.
  done = 10;
  assert (blk_account_write (1, 5, 128, &done) == BLK_DONE_PARTIAL);
  assert (done == 15);
  // Write: the card took nothing. Same code path, nothing moved.
  assert (blk_account_write (1, 0, 128, &done) == BLK_DONE_NONE);
  assert (done == 15);
  // Write: the caller's bytes never reached staging, so the card was never asked.
  assert (blk_account_write (0, 0, 128, &done) == BLK_DONE_NONE);
  assert (done == 15);

  // Read: the r157 rule, in both directions. A short read delivers nothing, even though the card claims
  // it returned 127 of the 128 blocks -- those words are not in the caller's buffer, because the copy
  // step is what moves them and it never ran.
  done = 0;
  assert (blk_account_read (127, 0, 128, &done) == BLK_DONE_NONE);
  assert (done == 0);
  assert (blk_account_read (0, 0, 128, &done) == BLK_DONE_NONE);
  // A full read whose copy then failed is the same answer for the same reason.
  assert (blk_account_read (128, 0, 128, &done) == BLK_DONE_NONE);
  assert (done == 0);
  assert (blk_account_read (128, 1, 128, &done) == BLK_DONE_ALL);
  assert (done == 128);
  // And the per-block fallback is this same function at chunk 1.
  done = 0;
  assert (blk_account_read (1, 1, 1, &done) == BLK_DONE_ALL);
  assert (done == 1);
  assert (blk_account_write (1, 1, 1, &done) == BLK_DONE_ALL);
  assert (done == 2);
  assert (blk_account_write (1, 0, 1, &done) == BLK_DONE_NONE);
  assert (done == 2);
}

static void request_window (void)
{
  // The log window is [100, 150). Half-open, so ending at the window's first block is not touching it,
  // and starting at its last block is.
  assert (blk_overlaps (100, 1, 100, 50) == 1);
  assert (blk_overlaps (149, 1, 100, 50) == 1);
  assert (blk_overlaps (150, 1, 100, 50) == 0);
  assert (blk_overlaps (99, 1, 100, 50) == 0);
  assert (blk_overlaps (99, 2, 100, 50) == 1);
  assert (blk_overlaps (50, 200, 100, 50) == 1);
  assert (blk_overlaps (0, 0, 100, 50) == 0);
  assert (blk_overlaps (100, 1, 100, 0) == 0);   // no window published yet: nothing to commit for
  // A request entirely past the window must not flush, because a flush is the 102-second thing r171 was
  // written to make rare.
  assert (blk_overlaps (200, 4096, 100, 50) == 0);
}

static void request_capacity (void)
{
  uint64_t max_block = 999;
  assert (blk_last_block (0x1000, &max_block) == 0);
  assert (max_block == 0x0fff);
  assert (blk_last_block (1, &max_block) == 0);
  assert (max_block == 0);
  assert (blk_last_block (0, &max_block) == -1);
  assert (max_block == 0);                       // untouched: the refusal does not answer the question
  // The two callers of one decision: the last block reportMaxValidBlock() gives is exactly the block
  // blk_admit_request() will still accept, and the first one past it is exactly the one it refuses.
  uint64_t accepted = 0;
  assert (blk_last_block (0x1000, &max_block) == 0);
  assert (blk_admit_request (1, max_block, 1, kBlkBytes, 0x1000, &accepted) == BLK_REQ_OK);
  assert (blk_admit_request (1, max_block + 1, 1, kBlkBytes, 0x1000, &accepted) == BLK_REQ_RANGE);
}

int main (void)
{
  walk_good ();
  walk_order ();
  walk_extra_rows ();
  walk_magic ();
  walk_version ();
  walk_count ();
  walk_attr ();
  walk_bounds ();
  walk_geometry ();
  walk_names ();
  request_bound ();
  request_chunks ();
  request_delivery ();
  request_window ();
  request_capacity ();

  printf ("PASS: SHNGROOT walk with the nine refusals each on its own input (magic word, version, row "
          "count, rows-beyond-span, per-window attributes, partition bounds including the overflow "
          "attempt, card geometry, missing and prefix-wearing names), partition-relative offsets "
          "published as card-absolute LBAs, row order independence, digest and identity copied per "
          "window, the request bound and its clamp to the buffer, chunking at the staging limit, "
          "r157's short-read-delivers-nothing rule beside a partial write's delivered count, the "
          "half-open log-window overlap, and reportMaxValidBlock agreeing with the request bound\n");
  return 0;
}
