// Host model of a Qualcomm SMEM heap, driving drivers/CloverDsp/DspCore.h against a synthesized 128 KiB
// region so that the partition walk, the two item lists, the SMP2P validation and the image scans are
// proven before they are put next to an ADSP's memory. Ported in shape from the same place as the
// arbiter model (~/UniXNU-sheng/package/tests/boot_reset_host.c): the split exists because a decode that
// has only ever run on the device is a decode whose bugs are discovered on the device.
//
// WHAT THIS IS FOR, SPECIFICALLY
//
// CloverDsp's whole report is derived from five decisions, and none of them can be checked on the board
// without a second instrument. (1) Which word form a magic has: the vendor declares `u8 magic[4]` and
// memcmps it, so its source shows "$TOC" as {0x24,0x54,0x4f,0x43} while a word load on this processor
// reads 0x434f5424 -- get that backwards and a live heap reports "no partition table" with no clue that
// the clue was a byte swap. (2) Whether the walk can be trusted with offsets it did not allocate: every
// offset and size here comes from memory another processor writes, so each one is bounded, and the
// bound has to reject an out-of-range row without dereferencing it. (3) Whether the uncached list's end
// marker is honoured, because a list walked past its end is a scan of somebody else's heap. (4) What
// makes an item an SMP2P item -- magic, version, total_entries and both pid fields -- which is the only
// reason this board, whose shipped DTB names no SMEM items at all, can say "the ADSP's edge exists"
// rather than "the number 429 happened to have bytes at it". (5) In which direction a burst of page
// reads answers "was anything loaded here".
//
// WHY ONE MODEL RATHER THAN A DEVICE THREAD
//
// The arbiter model needs a thread because a transaction has a device that answers; SMEM does not. The
// heap is data, and every branch of the walk is reachable by writing different bytes into the same
// region, which is what the corrupted fields below do. That also means the failure paths are tested
// rather than argued about: DS_E_SPAN, DS_E_RANGE, DS_E_ALIGN, DS_E_CANARY, DS_E_COUNT, DS_E_MAGIC,
// DS_E_VERSION, DS_E_PID, DS_E_SIZE, DS_E_PART and DS_E_ITEM each have a region built to produce them.
//
// WHAT THE MODEL DELIBERATELY DOES NOT REPRODUCE
//
// Real SMEM offsets are 8-byte aligned and its sizes are rounded up to 8 (SHENG's allocator aligns with
// `align_up(size, 8)`); the model uses whatever sizes make the branches reachable, because the code
// under test bounds on the numbers rather than on their alignment. It also has one partition per
// processor pair where a real heap carries dozens, and its SMP2P items have two entries of the sixteen.
// Neither is a behavioural difference: the loops are bounded by what the headers say, and that is what
// is being shown to work.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../DspCore.h"

#define kHeapSize 0x20000u                   // 128 KiB: a whole number of pages, >= the 8 KiB floor
#define kPt       (kHeapSize - 0x1000u)      // where ds_ptable() looks for the partition table

// The layout, chosen so that no two structures overlap and the TOC's own extent (0xD0 + 512*16 = 0x20D0)
// sits below all of it.
#define kGlobalItem  0x3000u                 // a couple of plain global items
#define kPartAdsp    0x4000u                 // the 0:2 private partition, 16 KiB
#define kPartAdspSize 0x4000u
#define kPartModem   0x8000u                 // the 0:1 partition, 8 KiB
#define kPartModemSize 0x2000u
#define kItemSsr     0xA000u                 // item 423, global only: not in any private partition
#define kItemVer     0xA100u                 // item 469, global but in another region (aux_base set)
#define kItemLate    0xA200u                 // item 497, the last one the vendor's bound allows
#define kPartBadHdr  0x10000u                // 2:3 whose "$PRT" is not "$PRT"
#define kPartBadHdrSize 0x800u
#define kPartBadLine 0x11000u                // 0:6 with a cacheline of 3
#define kPartBadLineSize 0x800u
#define kPartContra  0x12000u                // 0:7 whose header size contradicts its row
#define kPartContraSize 0x800u

// Item offsets inside the ADSP partition, relative to its base. The uncached list is two entries and
// the free mark lands exactly on their end; the cached list is two entries hanging down from the end of
// the partition, its free mark exactly below the last one.
#define kUxItemOut   32u                                        // first uncached entry
#define kUxItemGlink (32u + kSmemEntrySize + kSmp2pItemBytes)   // 388: second
#define kUxEnd       (kUxItemGlink + kSmemEntrySize + 64u)      // 468: the end marker
#define kCxHeader    ((kSmemEntrySize + 8u - 1u) & ~7u)         // align_up(16, cacheline 8) = 16
#define kCxA         (kPartAdspSize - kCxHeader)                // 0x3FF0
#define kCxB         (kCxA - kSmp2pItemBytes - kCxHeader)       // 0x3E8C
#define kCxEnd       (kCxB - 64u - kCxHeader)                   // 0x3E4C

static volatile uint8_t heap[kHeapSize];
static volatile uint8_t carve[kImageScanPages * kImagePageSize];
static const uint64_t kHeapPhys = 0x86000000ULL;    // the real base, so phys arithmetic is checked

static void wr (uint32_t at, uint32_t value)
{
  * (volatile uint32_t *) (heap + at) = value;
}

static void wrBytes (uint32_t at, const uint8_t *bytes, uint32_t len)
{
  for (uint32_t i = 0; i < len; i++) {
    heap [at + i] = bytes [i];
  }
}

static void wrImage (uint32_t at, uint32_t value)
{
  * (volatile uint32_t *) (carve + at) = value;
}

// The four-byte magics as the vendor spells them, so a word constant that disagrees with the byte order
// fails the model rather than the board. VEND smem.c:184/221/258 and smp2p.c:57 are where these come
// from; the constants under test are in DspRegs.h.
static const uint8_t mTOC[4] = { 0x24, 0x54, 0x4f, 0x43 };   // "$TOC"
static const uint8_t mPRT[4] = { 0x24, 0x50, 0x52, 0x54 };   // "$PRT"
static const uint8_t mSIII[4] = { 0x53, 0x49, 0x49, 0x49 };  // "SIII"
static const uint8_t mSMP[4] = { 0x24, 0x53, 0x4d, 0x50 };   // "$SMP"

// The bytes of a magic as the aligned word a little-endian load produces -- spelled out rather than
// cast, because casting the array is what the vendor's memcmp does not have to do and this file is
// where the two forms get reconciled.
static uint32_t wordOf (const uint8_t b[4])
{
  return (uint32_t) b [0] | ((uint32_t) b [1] << 8) | ((uint32_t) b [2] << 16) | ((uint32_t) b [3] << 24);
}

static void setToc (uint32_t item, uint32_t offset, uint32_t size, uint32_t aux)
{
  const uint32_t at = kSmemToc + item * kSmemTocEntrySize;
  wr (at + kSmemTocAllocated, 1);
  wr (at + kSmemTocOffset, offset);
  wr (at + kSmemTocSize, size);
  wr (at + kSmemTocAuxBase, aux);
}

static void setRow (uint32_t index, uint32_t offset, uint32_t size, uint32_t host0, uint32_t host1,
                    uint32_t cacheline)
{
  const uint32_t at = kPt + kSmemPtableSize + index * kSmemPtableEntrySize;
  wr (at + kSmemPtEntryOffset, offset);
  wr (at + kSmemPtEntrySize, size);
  wr (at + kSmemPtEntryFlags, 0);
  wr (at + kSmemPtEntryHosts, host0 | (host1 << 16));
  wr (at + kSmemPtEntryCacheline, cacheline);
}

// The partition header, whose five fields are the only ones either implementation reads (VEND
// smem.c:198-206, SHENG QcomSmem.c:39-40). The cacheline is not in it -- it comes from the table row --
// which is why setPart takes no such argument.
static void setPart (uint32_t off, uint32_t size, uint32_t host0, uint32_t host1,
                     uint32_t freeUncached, uint32_t freeCached, const uint8_t magic[4])
{
  wrBytes (off, magic, 4);
  wr (off + kSmemPartHosts, host0 | (host1 << 16));
  wr (off + kSmemPartSize, size);
  wr (off + kSmemPartFreeUncached, freeUncached);
  wr (off + kSmemPartFreeCached, freeCached);
}

// One uncached entry header, and the offset of the next -- the arithmetic the walk itself uses, written
// out again here so a stride that silently drifts makes the model produce a different list.
static uint32_t putUncached (uint32_t partOff, uint32_t at, uint32_t item, uint32_t rawSize,
                             uint32_t paddingData, uint32_t paddingHeader)
{
  const uint32_t e = partOff + at;
  wr (e, (uint32_t) kSmemEntryCanary | (item << 16));
  wr (e + 4, rawSize);
  wr (e + 8, paddingData | (paddingHeader << 16));
  wr (e + 12, 0);
  return at + kSmemEntrySize + paddingHeader + rawSize;
}

static uint32_t putCached (uint32_t partOff, uint32_t at, uint32_t item, uint32_t rawSize)
{
  const uint32_t e = partOff + at;
  wr (e, (uint32_t) kSmemEntryCanary | (item << 16));
  wr (e + 4, rawSize);
  wr (e + 8, 0);
  wr (e + 12, 0);
  return at - rawSize - kCxHeader;
}

// A complete "$SMP" item: 20 bytes of header and two named entries. The header fields are the byte
// fields VEND smp2p.c:75-89 declares -- version is one byte, the features follow it in the same word,
// and the two pids and the two counts share words -- so this writes them as bytes and the code under
// test reads them as words, which is the exchange being pinned.
static void putSmp2p (uint32_t at, uint16_t localPid, uint16_t remotePid, uint16_t valid,
                      uint32_t flags, const char *name0, uint32_t value0,
                      const char *name1, uint32_t value1)
{
  wrBytes (at, mSMP, 4);
  heap [at + 4] = kSmp2pVersion;                 // version, then features in the same word
  heap [at + 5] = kSmp2pFeatureSsrAck;
  heap [at + 6] = 0;
  heap [at + 7] = 0;
  wr (at + 8, (uint32_t) localPid | ((uint32_t) remotePid << 16));
  wr (at + 12, kSmp2pMaxEntries | ((uint32_t) valid << 16));
  wr (at + 16, flags);
  const char *names[2] = { name0, name1 };
  uint32_t values[2] = { value0, value1 };
  for (uint32_t i = 0; i < 2; i++) {
    const uint32_t e = at + 20 + i * kSmp2pEntryStride;
    uint32_t j = 0;
    for (; j < kSmp2pMaxEntryName && names [i][j]; j++) {
      heap [e + j] = (uint8_t) names [i][j];
    }
    for (; j < kSmp2pMaxEntryName; j++) {
      heap [e + j] = 0;
    }
    wr (e + kSmp2pEntryValue, values [i]);
  }
}

static struct ds_span span (uint32_t size = kHeapSize, uint64_t phys = kHeapPhys)
{
  struct ds_span s;
  s.base = (volatile uint8_t *) heap;
  s.size = size;
  s.phys = phys;
  return s;
}

// The well-formed heap: header, six global TOC items, a "$TOC" with six rows, the ADSP's 0:2 partition
// holding its SMP2P pair (outbound uncached, inbound cached) and the GLINK descriptor, the modem's 0:1
// partition, three deliberately broken rows, and an smem_info that bounds the walk at 498.
static void setUp (void)
{
  memset ((void *) heap, 0, sizeof heap);

  wr (kSmemProcComm, 0x11223344);           // the legacy proc_comm command/status pair
  wr (kSmemProcComm + 4, 0x55667788);
  wr (kSmemVersionTable + kSmemSblVersionIndex * kSmemVersionStride, 0x00010003);
  wr (kSmemInitialized, 1);
  wr (kSmemFreeOffset, 0x20D0);
  wr (kSmemAvailable, kHeapSize - 0x20D0);
  wr (kSmemReserved, 0);                     // VEND says this must be zero; the model obeys

  setToc (4, kGlobalItem, 32, 0);
  setToc (21, kGlobalItem + 0x100, 64, 0);
  setToc (kSmemItemAdspSsr, kItemSsr, 64, 0);
  setToc (kSmemItemImageVersion, kItemVer, 32, 0x80000004);   // allocated, but in another region
  setToc (kSmemItemNumItems - 1, kItemLate, 32, 0);           // item 497
  setToc (500, kPt, 0x1800, 0);                               // allocated, and entirely off the map
  wrBytes (kItemSsr, (const uint8_t *) "adsp\0\0\0\0", 8);

  wrBytes (kPt, mTOC, 4);
  wr (kPt + kSmemPtableVersion, 1);
  wr (kPt + kSmemPtableNumEntries, 6);

  setRow (0, kPartAdsp, kPartAdspSize, kSmemHostApps, 2, 8);
  setRow (1, kPartModem, kPartModemSize, kSmemHostApps, 1, 8);
  setRow (2, kPt, 0x1800, kSmemHostApps, 5, 8);               // a row that leaves the mapping
  setRow (3, kPartBadHdr, kPartBadHdrSize, 2, 3, 8);
  setRow (4, kPartBadLine, kPartBadLineSize, kSmemHostApps, 6, 3);
  setRow (5, kPartContra, kPartContraSize, kSmemHostApps, 7, 8);

  setPart (kPartAdsp, kPartAdspSize, kSmemHostApps, 2, kUxEnd, kCxEnd, mPRT);
  assert (putUncached (kPartAdsp, kUxItemOut, kSmemItemAdspSmp2pOut, kSmp2pItemBytes, 0, 0)
          == kUxItemGlink);
  putSmp2p (kPartAdsp + kUxItemOut + kSmemEntrySize, 0, 2, 2, kSmp2pFlagRestartDone,
            "master-kernel", 0x00000001, "slave-kernel", 0x00000000);
  assert (putUncached (kPartAdsp, kUxItemGlink, kSmemItemGlinkDesc, 64, 8, 0) == kUxEnd);
  assert (putCached (kPartAdsp, kCxA, kSmemItemAdspSmp2pIn, kSmp2pItemBytes) == kCxB);
  putSmp2p (kPartAdsp + kCxA - kSmp2pItemBytes, 2, 0, 1, 0, "wcss", 0x00000002, "", 0);
  assert (putCached (kPartAdsp, kCxB, kSmemItemGlinkFifo1, 64) == kCxEnd);

  setPart (kPartModem, kPartModemSize, kSmemHostApps, 1,
           32 + kSmemEntrySize + kSmp2pItemBytes, kPartModemSize - kCxHeader, mPRT);
  putUncached (kPartModem, 32, kSmemItemModemSmp2pOut, kSmp2pItemBytes, 0, 0);
  putSmp2p (kPartModem + 32 + kSmemEntrySize, 0, 1, 1, 0, "master-kernel", 0x80000000, "", 0);

  // The broken rows, each broken in exactly one way so the refusals stay distinguishable from each other.
  setPart (kPartBadHdr, kPartBadHdrSize, 2, 3, 32, kPartBadHdrSize - kCxHeader,
           (const uint8_t *) "\xef\xbe\xad\xde");
  setPart (kPartBadLine, kPartBadLineSize, kSmemHostApps, 6, 32, kPartBadLineSize - kCxHeader, mPRT);
  setPart (kPartContra, kPartContraSize + 4, kSmemHostApps, 7, 32, kPartContraSize - kCxHeader, mPRT);

  const uint32_t info = kPt + kSmemPtableSize + 6 * kSmemPtableEntrySize;
  wrBytes (info, mSIII, 4);
  wr (info + kSmemInfoBaseAddr, 0);
  heap [info + kSmemInfoNumItems] = 0xf2;          // num_items is a u16: 0x01f2 = 498
  heap [info + kSmemInfoNumItems + 1] = 0x01;
}

int main (void)
{
  struct ds_header header;
  struct ds_ptable table;
  struct ds_partition part;
  struct ds_item item;
  struct ds_smp2p head;
  struct ds_image image;
  struct ds_tocEntry toc;
  char name[kSmp2pMaxEntryName + 1];
  uint32_t value = 0;

  // 1. The constants that are transcriptions, checked before anything reads them. If the TOC's extent
  //    or the SMP2P item's size is wrong here, every bound below is wrong with it and the model would
  //    agree with the code because both were wrong the same way -- so these are asserted against the
  //    vendor's own struct arithmetic (VEND smem.c:120-125, :176-186, smp2p.c:75-89).
  assert (kSmemToc + kSmemItemCount * kSmemTocEntrySize == 0x20D0);
  assert (kSmemTocWords == 0x20D0 / 4);
  assert (kSmp2pItemBytes == 340);
  assert (kSmp2pItemBytes == 20 + 20 * kSmp2pMaxEntries);
  // The four words above are the little-endian reading of the vendor's four-byte arrays. This is the
  // single assertion most likely to be broken by someone "correcting" a constant to match the vendor
  // source, which compares them with memcmp and never as a word -- and the bug it prevents is a live
  // heap reporting "no partition table here", with nothing in the report to say the reason was a swap.
  assert (wordOf (mTOC) == kSmemPtableMagic);
  assert (wordOf (mPRT) == kSmemPartMagic);
  assert (wordOf (mSIII) == kSmemInfoMagic);
  assert (wordOf (mSMP) == kSmp2pMagic);
  // The edge table is the shipped DTB plus the CAF tree's `mboxes` and `qcom,smem` pairs, and it is what
  // the driver prints, so a typo in it is a wrong report on the board. Seven edges, and the outbound
  // item the vendor's group numbering implies for each pid agrees with the CAF tree for all six it
  // names -- which is why mode 2 may try the arithmetic as a candidate and not as a conclusion.
  assert (kDspEdgeCount == 7);
  for (uint32_t i = 0; i < kDspEdgeCount; i++) {
    const struct DspEdge *e = &kDspEdges [i];
    if (e->remotePid >= kSmemSmp2pItemStride || e->smemOutboundItem == 0xffffffff) {
      assert (e->remotePid == 0xffffffff && e->smemOutboundItem == 0xffffffff);   // the rpm row
      continue;
    }
    assert (kSmemSmp2pAppsBase + e->remotePid == e->smemOutboundItem);
  }
  assert (kDspEdges [kDspEdgeAdspGlink].irqMask == 0x200);
  assert (kDspEdges [kDspEdgeAdspSmp2p].irqMask == 0x400);
  assert (kDspEdges [kDspEdgeAdspSmp2p].smemInboundItem == kSmemItemAdspSmp2pIn);
  // The inbound side is where the arithmetic stops holding: the ADSP's is its own group base, and the
  // CDSP's is 94, which no rule from the five APPS-group bases produces. That is why an item number is
  // only ever a candidate here.
  assert (kSmemItemAdspSmp2pIn == kSmemSmp2pAppsBase + 2 * kSmemSmp2pItemStride);
  assert (kSmemItemModemSmp2pIn == kSmemSmp2pAppsBase + 1 * kSmemSmp2pItemStride);
  assert (kSmemItemCdspSmp2pIn != kSmemSmp2pAppsBase + 5 * kSmemSmp2pItemStride);
  // INTID = SPI + 32, the arithmetic round181 used for gpio67: DTB's adsp 0x9e is SPI 158, INTID 190,
  // and its modem 0x1c3 is SPI 451, INTID 483.
  assert (kDspEdges [kDspEdgeAdspSmp2p].inboundSpi + 32 == 190);
  assert (kDspEdges [3].inboundSpi + 32 == 483);

  // 2. Reads that must be refused before they touch memory: an unaligned word load (SMEM is a word array
  //    in both implementations), and an offset that ends past the mapping.
  setUp ();
  assert (ds_rd32 (span (), 1, &value) == DS_E_ALIGN);
  assert (ds_rd32 (span (), kHeapSize, &value) == DS_E_RANGE);
  struct ds_span nothing = { nullptr, 0, 0 };
  assert (ds_rd32 (nothing, 0, &value) == DS_E_SPAN);
  assert (ds_rd32 (span (), kSmemInitialized, &value) == DS_OK && value == 1);
  assert (ds_rd32 (span (kHeapSize, 0), kSmemProcComm, &value) == DS_OK && value == 0x11223344);

  // 3. The header of a live heap, and the counts the walk reports from it. Six allocated global items,
  //    and the highest is 500, so toc_highest is 501 -- including the entry whose offset is off the
  //    map, which is the point: the count says what the heap claims and in_span says what this mapping
  //    can prove.
  setUp ();
  assert (ds_header (span (), &header) == DS_OK);
  assert (header.initialized == 1 && header.free_offset == 0x20D0);
  assert (header.available == kHeapSize - 0x20D0 && header.reserved == 0);
  assert (header.sbl_version == 0x00010003);
  assert (header.pc_command == 0x11223344 && header.pc_status == 0x55667788);
  assert (header.toc_scanned == kSmemItemCount);
  assert (header.toc_allocated == 6 && header.toc_highest == 501);
  // A truncated mapping scans what it can and stops: 0x1000 bytes cover (0x1000 - 0xD0) / 16 = 243
  // entries, of which only the two below item 242 are allocated. The driver prints 243 rather than 512
  // so a reader can tell a small mapping from an empty heap.
  assert (ds_header (span (0x1000), &header) == DS_OK);
  assert (header.toc_scanned == 243 && header.toc_allocated == 2 && header.toc_highest == 22);
  // A span that cannot hold the header's own last word is not a heap.
  assert (ds_header (span (0xC0), &header) == DS_E_SPAN);
  assert (ds_header (nothing, &header) == DS_E_SPAN);

  // 4. The partition table: found one page from the end, with the magic, version and count the vendor
  //    requires, and the smem_info record read through the span so a full table cannot overrun it.
  setUp ();
  assert (ds_ptable (span (), &table) == DS_OK);
  assert (table.magic == kSmemPtableMagic && table.version == 1 && table.num_entries == 6);
  assert (table.at == kHeapSize - 0x1000);
  assert (table.capacity == (0x1000 - kSmemPtableSize) / kSmemPtableEntrySize);
  assert (table.num_entries <= table.capacity);
  assert (table.info_magic == kSmemInfoMagic && table.info_num_items == 498);
  // The refusals, each with its own code, because "not a table", "a table we do not implement" and "a
  // table too big to be true" are three different things to say about a board. On every refusal the
  // fields read so far still come back: the driver prints them, and a bare code would hide the magic.
  setUp (); wr (kPt, 0);
  assert (ds_ptable (span (), &table) == DS_E_MAGIC);
  assert (table.magic == 0 && table.num_entries == 6 && table.at == kHeapSize - 0x1000);
  setUp (); wr (kPt + kSmemPtableVersion, 2);
  assert (ds_ptable (span (), &table) == DS_E_VERSION && table.version == 2);
  setUp (); wr (kPt + kSmemPtableNumEntries, table.capacity + 1);
  assert (ds_ptable (span (), &table) == DS_E_COUNT);
  setUp (); wr (kPt + kSmemPtableNumEntries, 1000);
  assert (ds_ptable (span (), &table) == DS_E_COUNT);
  // A region that is not a whole number of pages, or is smaller than 8 KiB, has no "one page from the
  // end" that could be a table (SHENG QcomSmem.c:22 refuses it the same way).
  setUp ();
  assert (ds_ptable (span (kHeapSize - 0x10), &table) == DS_E_SPAN);
  assert (ds_ptable (span (0x1FF0), &table) == DS_E_SPAN);
  assert (ds_ptable (span (0x1000), &table) == DS_E_SPAN);

  // 5. The rows. A good row's own header agrees with it, so header_ok is 1 and the free marks are inside
  //    the partition; a row that points off the mapping is refused without being dereferenced; a row
  //    whose header contradicts it comes back with rc DS_OK and header_ok 0, because the contradiction
  //    is the finding rather than an error to hide.
  setUp ();
  assert (ds_ptable (span (), &table) == DS_OK);
  assert (ds_partition_at (span (), table, 0, &part) == DS_OK);
  assert (part.offset == kPartAdsp && part.size == kPartAdspSize);
  assert (part.host0 == kSmemHostApps && part.host1 == 2 && part.cacheline == 8);
  assert (part.part_magic == kSmemPartMagic && part.header_ok == 1);
  assert (part.free_uncached == kUxEnd && part.free_cached == kCxEnd);
  assert (ds_partition_at (span (), table, 1, &part) == DS_OK && part.header_ok == 1 && part.host1 == 1);
  assert (ds_partition_at (span (), table, 2, &part) == DS_E_RANGE);
  assert (part.host1 == 5 && part.header_ok == 0);   // filled, but never followed
  assert (ds_partition_at (span (), table, 3, &part) == DS_OK && part.header_ok == 0);
  assert (part.part_magic == 0xdeadbeefu);
  assert (ds_partition_at (span (), table, 4, &part) == DS_OK && part.header_ok == 1);
  assert (ds_partition_at (span (), table, 5, &part) == DS_OK && part.header_ok == 0);
  assert (ds_partition_at (span (), table, 6, &part) == DS_E_RANGE);   // index past the table

  // 6. The pairing, in either order, and only among self-consistent rows. The 0:2 row is index 0 and a
  //    2:3 row exists at index 3, so a search that ignored header_ok or the order would land on the
  //    wrong partition -- and the 2:3 pair has no usable row at all.
  assert (ds_find_partition (span (), kSmemHostApps, 2, &part) == DS_OK);
  assert (part.index == 0 && part.header_ok == 1);
  assert (ds_find_partition (span (), 2, kSmemHostApps, &part) == DS_OK && part.index == 0);
  assert (ds_find_partition (span (), kSmemHostApps, 1, &part) == DS_OK && part.index == 1);
  assert (ds_find_partition (span (), 2, 3, &part) == DS_E_PART);
  assert (ds_find_partition (span (), kSmemHostApps, 5, &part) == DS_E_PART);
  assert (ds_find_partition (span (), kSmemHostApps, 7, &part) == DS_E_PART);
  // On a legacy heap -- a header with no "$TOC" one page from the end -- there is no partition to find,
  // and the code says so with the table's own reason rather than with DS_E_PART.
  setUp (); wr (kPt, 0);
  assert (ds_find_partition (span (), kSmemHostApps, 2, &part) == DS_E_MAGIC);

  // 7. The two item lists. Item 429 is the first uncached entry of the 0:2 partition; item 443 is on
  //    the cached side, walked down from the partition's end; item 478 is the second uncached entry and
  //    carries 8 bytes of data padding, so its reported size is 64-8 = 56 and its data starts after the
  //    header. Reading padding_data and padding_header the other way round changes both, and the change
  //    is silent on the board.
  setUp ();
  assert (ds_find_partition (span (), kSmemHostApps, 2, &part) == DS_OK);
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_OK);
  assert (item.cached == 0 && item.at == kPartAdsp + kUxItemOut);
  assert (item.data == kPartAdsp + kUxItemOut + kSmemEntrySize && item.padding_header == 0);
  assert (item.size == kSmp2pItemBytes && item.raw_size == kSmp2pItemBytes);
  assert (item.data + item.size <= kHeapSize);
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pIn, &item) == DS_OK);
  assert (item.cached == 1 && item.at == kPartAdsp + kCxA);
  assert (item.data == kPartAdsp + kCxA - kSmp2pItemBytes && item.size == kSmp2pItemBytes);
  assert (ds_find_item (span (), part, kSmemItemGlinkDesc, &item) == DS_OK);
  assert (item.cached == 0 && item.padding_data == 8 && item.size == 56 && item.raw_size == 64);
  assert (item.data == kPartAdsp + kUxItemGlink + kSmemEntrySize);
  assert (ds_find_item (span (), part, kSmemItemGlinkFifo1, &item) == DS_OK);
  assert (item.cached == 1 && item.at == kPartAdsp + kCxB && item.size == 64);
  // An item in neither list is DS_E_ITEM. On this board, where the shipped DTB names no items at all,
  // that is the answer mode 2 is most likely to get -- and it has to stay a different answer from "the
  // partition is corrupt", which would send the next round chasing a heap that is fine.
  assert (ds_find_item (span (), part, kSmemItemAdspSsr, &item) == DS_E_ITEM);
  assert (ds_find_item (span (), part, 7, &item) == DS_E_ITEM);
  // A partition that failed its own header check is not walked at all.
  assert (ds_find_partition (span (), kSmemHostApps, 6, &part) == DS_OK);
  assert (ds_find_item (span (), part, kSmemItemGlinkDesc, &item) == DS_E_PART);
  // The 0:1 partition has one uncached item and an empty cached side: its free mark sits exactly at the
  // first cached slot, which is the case the loop condition `at > free_cached` has to get right.
  assert (ds_find_partition (span (), kSmemHostApps, 1, &part) == DS_OK);
  assert (ds_find_item (span (), part, kSmemItemModemSmp2pOut, &item) == DS_OK);
  assert (item.cached == 0 && item.data == kPartModem + 32 + kSmemEntrySize);
  assert (ds_find_item (span (), part, kSmemItemModemSmp2pIn, &item) == DS_E_ITEM);

  // 8. The bounds, made by corrupting one field at a time -- and re-reading the row afterwards, because
  //    the walk honours the free marks it was handed and a stale struct would test nothing. Each of
  //    these is a region the driver would otherwise read inside.
  setUp ();
  assert (ds_ptable (span (), &table) == DS_OK);
  wr (kPartAdsp + kSmemPartFreeUncached, kUxEnd - 4);     // the list now ends inside the second entry
  assert (ds_partition_at (span (), table, 0, &part) == DS_OK && part.header_ok == 1);
  assert (ds_find_item (span (), part, kSmemItemGlinkDesc, &item) == DS_E_RANGE);
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_OK);   // the first is intact
  setUp ();
  assert (ds_ptable (span (), &table) == DS_OK);
  wr (kPartAdsp + kSmemPartFreeUncached, kUxEnd + 0x100); // and now it claims a third entry
  assert (ds_partition_at (span (), table, 0, &part) == DS_OK);
  assert (ds_find_item (span (), part, kSmemItemGlinkFifo0, &item) == DS_E_CANARY);
  setUp ();
  assert (ds_ptable (span (), &table) == DS_OK);
  assert (ds_partition_at (span (), table, 0, &part) == DS_OK);
  wr (kPartAdsp + kUxItemGlink + 4, kPartAdspSize);       // one entry, the whole partition
  assert (ds_find_item (span (), part, kSmemItemGlinkDesc, &item) == DS_E_RANGE);
  setUp ();
  assert (ds_ptable (span (), &table) == DS_OK);
  assert (ds_partition_at (span (), table, 0, &part) == DS_OK);
  wr (kPartAdsp + kUxItemOut, 0x0000b3b3);                // right place, wrong canary
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_E_CANARY);
  // A cached entry whose size would put its data below the partition's start: `rawSize > at` is the
  // guard, and without it the resolved offset is a negative number promoted to a huge one.
  setUp ();
  assert (ds_ptable (span (), &table) == DS_OK);
  assert (ds_partition_at (span (), table, 0, &part) == DS_OK);
  wr (kPartAdsp + kCxA + 4, kPartAdspSize);
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pIn, &item) == DS_E_CANARY);

  // 9. The global path, which is what a heap with no private partition still answers through. Item 423
  //    is allocated and inside the span; 469 is allocated but names another region through aux_base, so
  //    it is refused rather than read at this span's offset; 500 is allocated but does not fit.
  setUp ();
  assert (ds_find_item_global (span (), kSmemItemAdspSsr, &item) == DS_OK);
  assert (item.cached == -1 && item.data == kItemSsr && item.size == 64 && item.at == 0);
  assert (ds_find_item_global (span (), kSmemItemImageVersion, &item) == DS_E_RANGE);
  assert (ds_find_item_global (span (), 500, &item) == DS_E_ITEM);
  assert (ds_find_item_global (span (), kSmemItemAdspSmp2pOut, &item) == DS_E_ITEM);
  assert (ds_find_item_global (span (), kSmemItemCount, &item) == DS_E_RANGE);
  // And the TOC entry itself: resolved against the span's phys so the log can print an address, with
  // in_span computed subtractively because offset and size are both somebody else's numbers.
  assert (ds_toc (span (), kSmemItemAdspSsr, &toc) == DS_OK);
  assert (toc.allocated == 1 && toc.in_span == 1 && toc.aux_base == 0);
  assert (toc.phys == kHeapPhys + kItemSsr);
  assert (ds_toc (span (), 500, &toc) == DS_OK);
  assert (toc.in_span == 0);
  assert (ds_toc (span (), kSmemItemCount, &toc) == DS_E_RANGE);
  assert (ds_toc (nothing, 4, &toc) == DS_E_RANGE);
  setUp ();
  assert (ds_toc (span (), kSmemItemImageVersion, &toc) == DS_OK);
  assert (toc.aux_base == 0x80000004u);   // the two reserved low bits are masked off; the rest is kept

  // 10. The SMP2P validation: the thing that turns "bytes at 429" into "the ADSP's outbound edge".
  setUp ();
  assert (ds_find_partition (span (), kSmemHostApps, 2, &part) == DS_OK);
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_OK);
  assert (ds_smp2p_head (span (), item.data, 0, 2, &head) == DS_OK);
  assert (head.magic == kSmp2pMagic && head.version == 1);
  assert (head.total_entries == kSmp2pMaxEntries && head.valid_entries == 2);
  assert (head.local_pid == 0 && head.remote_pid == 2);
  assert (head.features == kSmp2pFeatureSsrAck && head.flags == kSmp2pFlagRestartDone);
  assert (head.data == item.data && head.available_bytes == kHeapSize - item.data);
  // The entry names are the content of an established edge, and both the value's position inside the
  // entry (16, after a 16-byte name) and the stride (20) are what this reads.
  assert (ds_smp2p_entry (span (), head, 0, name, sizeof (name), &value) == DS_OK);
  assert (strcmp (name, "master-kernel") == 0 && value == 0x00000001);
  assert (ds_smp2p_entry (span (), head, 1, name, sizeof (name), &value) == DS_OK);
  assert (strcmp (name, "slave-kernel") == 0 && value == 0x00000000);
  assert (ds_smp2p_entry (span (), head, 2, name, sizeof (name), &value) == DS_E_RANGE);
  assert (ds_smp2p_entry (span (), head, 0, name, kSmp2pMaxEntryName, &value) == DS_E_RANGE);
  // The inbound item, read through the same partition on the other side of its free marks.
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pIn, &item) == DS_OK);
  assert (ds_smp2p_head (span (), item.data, 2, 0, &head) == DS_OK);
  assert (head.local_pid == 2 && head.remote_pid == 0 && head.valid_entries == 1);
  assert (ds_smp2p_entry (span (), head, 0, name, sizeof (name), &value) == DS_OK);
  assert (strcmp (name, "wcss") == 0 && value == 2);
  // The refusals, one field at a time, each named: the driver's report distinguishes "no item", "not an
  // SMP2P item", "a version we do not implement" and "an edge for other processors", and only the last
  // of those would be answered by trying the next candidate.
  assert (ds_smp2p_head (span (), item.data, 0, 2, &head) == DS_E_PID);     // wrong direction
  setUp ();
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_OK);
  heap [kPartAdsp + kUxItemOut + kSmemEntrySize + 4] = 2;                   // version
  assert (ds_smp2p_head (span (), item.data, 0, 2, &head) == DS_E_VERSION);
  assert (head.version == 2);
  setUp ();
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_OK);
  wr (kPartAdsp + kUxItemOut + kSmemEntrySize + 12, 15 | (2u << 16));       // total_entries
  assert (ds_smp2p_head (span (), item.data, 0, 2, &head) == DS_E_COUNT);
  setUp ();
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_OK);
  wr (kPartAdsp + kUxItemOut + kSmemEntrySize + 12, kSmp2pMaxEntries | (20u << 16));
  assert (ds_smp2p_head (span (), item.data, 0, 2, &head) == DS_E_COUNT);   // valid_entries > 16
  setUp ();
  assert (ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_OK);
  wr (kPartAdsp + kUxItemOut + kSmemEntrySize, 0);                          // magic
  assert (ds_smp2p_head (span (), item.data, 0, 2, &head) == DS_E_MAGIC);
  // The ADSP's SSR slot is not an SMP2P item at all, and saying so is what keeps the driver from
  // reporting subsystem names as edge entries.
  setUp ();
  assert (ds_find_item_global (span (), kSmemItemAdspSsr, &item) == DS_OK);
  assert (ds_smp2p_head (span (), item.data, 0, 2, &head) == DS_E_MAGIC);
  // An item so near the end of the mapping that its entries could not fit is refused before any of it
  // is read, and the code that refuses it is not the code that refuses a short read.
  assert (ds_smp2p_head (span (), kHeapSize - kSmp2pItemBytes + 4, 0, 2, &head) == DS_E_SIZE);
  assert (ds_smp2p_head (span (), kHeapSize + 4, 0, 2, &head) == DS_E_RANGE);

  // 11. The doorbell word against the masks the shipped DTB declares. 0x8400 is the adsp and mpss edges:
  //     bit 1 and bit 2 of the edge table. The lpass mask 0x200 is the adjacent bit, so a comparison
  //     that looked at the wrong half of the word shows up here instead of as a missing event on the
  //     board, where it would look like a DSP that never rang.
  {
    const uint32_t doorbell = 0x00008400;
    uint32_t bitmap = 0;
    for (uint32_t i = 0; i < kDspEdgeCount; i++) {
      bitmap |= (uint32_t) ds_bitSet (doorbell, kDspEdges [i].irqMask) << i;
    }
    assert (bitmap == 0x06);
    assert (ds_bitSet (doorbell, 0) == 0);           // a mask of 0 names no bit, and never matches
    assert (ds_bitSet (0x20000000, kDspEdges [4].irqMask) == 1);
    assert (ds_bitSet (0xffffffff, kDspEdges [6].irqMask) == 1);   // the rpm edge, mask 0x01
    assert (ds_bitSet (0, kDspEdges [0].irqMask) == 0);
    assert (ds_bitSet (0x00000400, kDspEdges [0].irqMask) == 0);   // adsp's bit is not lpass's
  }

  // 12. The image scans: presence, not identity. An empty carve-out says nothing was loaded; one ELF
  //     magic at offset 0 is what an .mdt looks like when the loader copied its header to the region
  //     base, and that offset has to be reportable as a number distinct from "no magic" -- which is why
  //     the driver's observation slot stores offset+1.
  memset ((void *) carve, 0, sizeof carve);
  struct ds_span window = { (volatile uint8_t *) carve, sizeof carve, kAdspFwPhys };
  assert (ds_image (window, kImageScanPages, &image) == DS_OK);
  assert (image.pages == kImageScanPages && image.nonzero == 0);
  assert (image.first_nonzero == 0xffffffff && image.elf_at == 0xffffffff && image.elf_count == 0);
  assert (image.head[0] == 0 && image.head[3] == 0);

  wrImage (0x3000, kImageElfMagic);
  wrImage (0x1004, 0xdead);
  assert (ds_image (window, kImageScanPages, &image) == DS_OK);
  assert (image.pages == kImageScanPages && image.nonzero == 2 && image.first_nonzero == 1);
  assert (image.elf_at == 0x3000 && image.elf_count == 1);
  assert (image.head[0] == 0 && image.head[1] == 0);

  memset ((void *) carve, 0, sizeof carve);
  wrImage (0, kImageElfMagic);
  assert (ds_image (window, kImageScanPages, &image) == DS_OK);
  assert (image.elf_at == 0 && image.first_nonzero == 0 && image.nonzero == 1);
  assert (image.head[0] == kImageElfMagic && image.head[1] == 0);
  assert (image.elf_at + 1 != 0);          // the slot encoding the driver uses can say "at zero"

  // A mapping smaller than the number of pages asked for: the scan covers the mapping, not the request,
  // because a 31 MiB carve-out is not mapped in order to count sixteen pages of it.
  memset ((void *) carve, 0, sizeof carve);
  window.size = 0x2000;
  assert (ds_image (window, kImageScanPages, &image) == DS_OK);
  assert (image.pages == 2 && image.first_nonzero == 0xffffffff);
  window.size = 8;                       // under sixteen bytes: no head words, one short page
  assert (ds_image (window, kImageScanPages, &image) == DS_OK);
  assert (image.pages == 1 && image.head[0] == 0);
  assert (ds_image (window, 0, &image) == DS_E_RANGE);
  assert (ds_image (nothing, kImageScanPages, &image) == DS_E_SPAN);

  // 13. The two paths the driver tries in order, end to end, exactly as dumpSmp2pItem() does them: the
  //     private partition first, and when it has nothing, the global TOC. Then the same on a heap whose
  //     0:2 row has been emptied -- where the private path is refused and the global one still runs, and
  //     finds nothing because a private item is not in the global table. That pair of refusals is the
  //     most likely reading on this board, and it is the one that must not be called "the DSP is down".
  setUp ();
  {
    int path = 0;
    if (ds_find_partition (span (), kSmemHostApps, 2, &part) == DS_OK &&
        ds_find_item (span (), part, kSmemItemAdspSmp2pOut, &item) == DS_OK &&
        ds_smp2p_head (span (), item.data, 0, 2, &head) == DS_OK) {
      path = 1;
    }
    assert (path == 1 && head.valid_entries == 2);
    wr (kPt + kSmemPtableSize + kSmemPtEntrySize, 0);      // row 0 loses its size
    assert (ds_find_partition (span (), kSmemHostApps, 2, &part) == DS_E_PART);
    assert (ds_find_item_global (span (), kSmemItemAdspSmp2pOut, &item) == DS_E_ITEM);
    // The global items survive the same amputation, which is why mode 2 does not stop at a bad table.
    assert (ds_find_item_global (span (), kSmemItemAdspSsr, &item) == DS_OK);
  }

  printf ("PASS: SMEM header and bounded TOC walk, $TOC/$PRT/$SMP word-form magics, partition rows "
          "with their own header checks, uncached and cached item lists with end markers, the global "
          "TOC path with aux_base and in_span refusals, SMP2P validation (version, total_entries, both "
          "pids) and entry decode, the seven DTB doorbell masks, and the two image scans\n");
  return 0;
}
