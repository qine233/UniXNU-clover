// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the DSP instrument -- is there a shared heap between this processor and
// the ADSP, does anybody own the doorbell, and was the DSP's firmware ever put in place by *this* boot
// chain?
//
// WHY THIS FILE EXISTS
//
// Schedule item 5 is network and audio, and on this board neither of those words describes something
// the AP can reach directly. The audio section of the shipped tree contains no codec at all: the
// WCD9335 card is `status = "disabled"` (clover.dts:14020-14035), the generic card's every CPU DAI is
// named `msm-dai-q6-*`, the two SLIMBus controllers live inside the LPASS register window next to the
// ADSP's own `qcom,lpass@15700000`, and the only child of `qcom,msm-audio-apr` is a dummy node
// (:13988). The Wi-Fi is `qcom,icnss@18800000` (:5833), whose four rails, MSA region and lifecycle are
// managed by that same ADSP's SSR. Both answers therefore sit behind one question, and that question
// has a hardware answer that costs nothing to ask: the AP and the ADSP share a 2 MiB window at
// 0x86000000 whose header says whether the boot built a table of contents in it, whose partition table
// says which processors were given private heaps, and whose SMP2P items say which edges were ever
// established. One 32-bit register at 0x17911008 says which doorbells rang.
//
// The reading that makes this round worth a release is the one no amount of source reading can
// produce, because it is a fact about *our* boot rather than about the vendor's: whether the ADSP is
// already up. If SMEM is initialized, the ADSP's SMP2P pair exists and validates, and the ADSP's
// carve-out holds a loaded image, then 排期 5 is a protocol job -- open a GLINK channel, speak APR --
// and the port does not need PIL, SCM, an authenticated load or a rail vote. If those readings come
// back empty, then 排期 5's first step is 排期 6's: `qcom,pas-id = <1>` over a `vdd_cx` that is
// pm660l L9 at RPM level TURBO (:5861-5867), which is exactly the gate round182 measured the owner of.
// Those two futures look identical from a device tree and differ in everything downstream of this
// file, so this is where the fork gets resolved.
//
// WHY THE LOGIC IS NOT IN THIS FILE
//
// Because SMEM is a linked structure whose every pointer is written by a processor this port does not
// control, and because round182 already proved the pattern pays. The header walk, the partition-table
// decode, the two item lists, the SMP2P validation and the image scans live in DspCore.h as inline
// functions over plain `volatile uint8_t *` spans -- the same split sheng uses for its own ADSP stack
// (package/QcomSmem.c is a walker over a byte pointer with no IOKit in it) -- and
// scripts/test-clover-dsp.py drives those exact functions on a Mac against a synthesized SMEM region:
// a well-formed heap, a legacy non-partitioned heap, a heap whose partition row points off the end,
// and a heap with an "$SMP" item wearing the wrong pids. The bounds decisions that make the walk safe
// are testable there and nowhere else.
//
// WHY IT IS READ-ONLY, AND WHY THAT IS NOT ENOUGH ON ITS OWN
//
// Nothing here writes a byte, and unlike round182 there is not even a command to issue: reading SMEM
// is a load, and reading 0x17911008 is a load. The risk that replaces transaction risk is an MPU. The
// SMEM node declares `qcom,mpu-enabled` (clover.dts:4692), which means the shipping kernel configured
// the Q6's MPU to permit the AP's access -- a permission a boot chain that never ran the ADSP's
// driver may not have granted, and a denied access to a Device- or normal-memory range is a data
// abort, not a zero read. Three things follow, and they are the reason this driver is structured the
// way it is: the mapping is done before any read so a range the platform cannot map is reported as a
// mapping failure rather than touched; every offset the walk is asked to follow is bounded against
// the span it came from (DspCore.h's DS_E_RANGE), because one unchecked `size` field turns a walk into
// a scan of memory belonging to another processor; and mode 1 begins with the *lowest-addressed*
// word of the region, so if an MPU denial happens it happens on the first read, with the mode, the
// phys and the slot array already printed. A rail that is off stays off; a doorbell stays unrung;
// the ADSP is not poked. Enabling anything is the round after this one.
//
// THE THREE QUESTIONS, ONE PER BOOT ARGUMENT (clover-dsp=N)
//
// Mode 1 asks what the heap says. Map the 2 MiB region, read the header's own words -- `initialized`,
// `free_offset`, `available`, `reserved` (which the vendor says must be zero, so printing it is a
// validity check on the whole decode), the SBL version record and the legacy proc_comm pair -- then
// walk the 512-entry global TOC bounded by the mapping, report how many entries are allocated and what
// the highest one is, and list the first kTocDumpLimit of them with their offset, size and physical
// address, because "populated with items we have no name for" and "empty" are different answers. Then
// the partition table, one page from the region's end: its magic,
// version, entry count, the `SIII` info record's `num_items`, and every row's host pair, offset, size
// and cacheline, each followed far enough to check that its "$PRT" header agrees with the table entry
// that points at it. That is a complete description of the heap and it starts nothing.
//
// Mode 2 asks who is on the other side of it. The ADSP's doorbell word at 0x17911008 is read once and
// reported two ways -- raw, and per edge against the mask this board's own device tree declares (the
// seven rows of DspRegs.h's kDspEdges: lpass 0x200, adsp 0x400, mpss 0x8000, modem 0x4000, cdsp
// 0x20000000, cdsp-smp2p 0x40000000, rpm 0x01). Each edge line also carries the outbound item number
// the vendor's group numbering implies for that pid and whether it agrees with the item the CAF tree
// declares for the edge -- the comparison mode 2's candidates are chosen from. Then the candidate items. Each SMP2P edge is looked
// for twice, in the order the vendor's resolver uses: first in the private partition this AP shares
// with that edge's remote pid, then through the global TOC -- and a candidate is only counted when
// DspCore.h's ds_smp2p_head() accepts it, which requires "$SMP", version 1, total_entries exactly 16
// and both pid fields naming the edge it was read for. The numbers come from three places and are
// labelled as they are tried: the CAF tree's explicit pair (443/429 for the ADSP), the arithmetic the
// same tree's three edges imply (427 + remote_pid), and the shipped tree's silence -- which is why an
// item found by magic is reported as found, and one not found is reported as *not found* rather than
// as "the ADSP is down" until both paths have been tried. Item 423 (the ADSP's SSR/crash slot, named
// by the shipped PIL node's `qcom,smem-id = <0x1a7>` and called kItemCrash by sheng) and the GLINK
// native-transport items 478/479/480 are read the same way, and their first bytes printed with an
// ASCII column, because an SSR table's payload is subsystem names.
//
// Mode 3 adds the two images. The ADSP's carve-out (`adsp_fw_region@92a00000` + 31 MiB, the phandle
// the PIL node's `memory-region` names) and the Wi-Fi's MSA (`wlan_msa_mem@85700000` + 1 MiB, plus the
// guard page region immediately below it) are each mapped page by page, 16 pages, and asked two
// questions: how many of those pages are non-zero, and does any word read as an ELF header. The
// answer is a presence claim, not an identity claim: a PIL loader copies segments to their `p_paddr`,
// so the .mdt's ELF magic is not necessarily at offset 0 and the scan therefore reports *where* it saw
// one rather than asserting where it should be. An all-zero ADSP carve-out with an all-zero MSA is the
// reading that says these subsystems were never brought up in this boot, which is the fact the whole
// round exists to obtain.
//
// WHAT IT DOES NOT CLAIM
//
// No GLINK. Once the channels are located, the FIFO layout the vendor's native transport uses is a
// protocol problem and sheng already owns a tested one (package/QcomGlink.c, whose intent/FIFO model
// this port will reuse); proving a transport with a hand-rolled reader here would be a second
// implementation of the same thing. No doorbell write, no SSR force-stop, no PIL load, no SCM call, no
// clock or vote for `vdd_cx`, no RPM message. No APR and no Q6: the vendor's audio GLINK child is
// `apr_audio_svc` (sdm660.dtsi:1526-1529), which is a protocol this port does not have yet and which
// sheng's GPR/AudioReach stack explicitly is not (its audio is AudioReach on SM8550 -- ShengGpr.c,
// AudioProbe.cpp -- and GPR does not speak to an ADSP that answers APR). Interrupts are named in
// DspRegs.h with their SPI numbers and the INTID arithmetic the shipped tree implies, and none is
// attached.
//
// WHY IT RUNS FROM sheng_pmgr_run()
//
// Same reason as CloverSdhci, CloverInput and CloverPower: a device repository has no XNU patch slot
// for an IOKit personality, so a board driver exports a C entry point and CloverPMGR calls it from the
// bsd_init hook. It runs after clover_power_run(), which is not merely an ordering courtesy: round182
// prints the two rails' enable bytes and their owner-EE table, and this round's "the ADSP was never
// loaded" answer combined with "L9 is off" is what turns "load the DSP ourselves" into a two-step plan
// with the rail first, instead of a PIL attempt that hangs on an unpowered CX domain.
//
// WHY NOTHING HERE PANICS
//
// Because round176 established that the kernel log reaches the host by itself -- IOLog writes msgbuf,
// CloverSdhci's tee copies any line containing "clover r" into the card-backed kernel ring, and
// scripts/read-clover-log.py --tee reads it off the card -- so halting the boot buys no additional
// reading and costs whatever the rest of the boot would have printed. There is also nothing to wait
// for: this driver has no poll loop, no deadline and no calibrated bound, because every operation is a
// load from memory or from one register, and the only unbounded quantity (the partition walk) is
// bounded by the span instead of by a timer. Every refusal has its own code, and a boot that finds an
// empty SMEM still reaches launchd with the emptiness recorded.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include "DspCore.h"

extern "C" {
// drivers/CloverSdhci's commit of both log rings to the card. At this scheduling point the block
// device may not exist yet, and then this returns 0 without touching the card -- the tee collects
// these lines on the next block operation anyway. Called on every exit path so a later round, running
// after bring-up, gets a real commit for free.
uint32_t clover_emmc_flush_cache (void);
}

// ------------------------------------------------------------------ the windows
//
// One for SMEM (2 MiB, mapped whole so the TOC, the partition table at the far end and every item they
// point at are inside one mapping), one for the APCS block's page, and one per image probe -- because
// a 31 MiB carve-out is not mapped in order to look at sixteen pages of it.
struct Window
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t base = 0;
  uint64_t phys = 0;
  uint32_t size = 0;
};

static Window gSmem;
static Window gApcs;
static Window gImage;

// Same three steps CloverSdhci, CloverInput and CloverPower use: ask the platform for the range, map
// it uncached and anywhere, keep only the virtual address. A range the platform memory map does not
// have returns null rather than a wrong pointer -- which for SMEM is itself a finding, since a
// carve-out the boot removed from the allocator can legitimately be missing from the device tree's
// memory map that IODeviceMemory is built from.
static uintptr_t mapWindow (Window &window, uint64_t phys, uint32_t size)
{
  window.phys = phys;
  window.size = size;
  window.base = 0;
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

// A device-register read, with the barrier the arbiter driver used. SMEM is not read through this:
// DspCore.h owns the memory reads because they are the tested logic, and this is only for 0x17911008.
static uint32_t windowRead (const Window &window, uint32_t offset)
{
  const uint32_t value = *reinterpret_cast<volatile uint32_t *> (window.base + offset);
  __asm__ volatile ("dsb sy" ::: "memory");
  return value;
}

static struct ds_span smemSpan (void)
{
  struct ds_span span;
  span.base = reinterpret_cast<volatile uint8_t *> (gSmem.base);
  span.size = gSmem.size;
  span.phys = gSmem.phys;
  return span;
}

// ------------------------------------------------------------------ what survives the screen
//
// The raw observations, kept for the same reason CloverSdhci, CloverInput and CloverPower keep theirs:
// a number that outlives a truncated ring line and can be decoded offline against this file. Slots:
//   [0]  mode, [1] the phys of a window that failed to map (0 when every needed window mapped),
//   [2]  SMEM's initialized word, [3] its free_offset,
//   [4]  TOC entries allocated (low 16) and the highest allocated item + 1 (high 16),
//   [5]  the partition table's magic, [6] its num_entries (low 16) and smem_info's num_items (high 16),
//   [7]  the ADSP outbound item's verdict code, [8] the inbound's,
//   [9]  the doorbell word as read, [10] the bit-per-edge bitmap of kDspEdges,
//   [11] ADSP carve-out non-zero pages (low 16) and pages scanned (high 16), [12] its ELF magic offset
//        plus one (0 when the scan saw no magic),
//   [13] WLAN MSA non-zero pages and pages scanned, [14] the same plus-one form for its magic offset,
//   [15] the last verdict code (see the note at each site).
volatile uint64_t clover_dsp_observation[16];

// One word per way a step can end, so the ring line and the observation array cannot disagree about
// which stage stopped. Positive values are steps; a failure stores step*1000 + code.
static const uint64_t kStepMap = 1, kStepHeader = 2, kStepPtable = 3, kStepPartition = 4,
                      kStepDoorbell = 5, kStepItem = 6, kStepImage = 7, kStepComplete = 9;
static const uint64_t kFailMap = 100;

// ------------------------------------------------------------------ printing
//
// Printable bytes, hex and ASCII side by side. For this round the ASCII column is not decoration: an
// SMP2P item's payload entries are names ("master-kernel", "slave-kernel") and item 423's are
// subsystem names, so the text is the reading.
static void printBytes (const char *label, const volatile uint8_t *data, uint32_t len)
{
  char hex [3 * kBytesDumpMax + 1];
  char text [kBytesDumpMax + 1];
  uint32_t shown = len > kBytesDumpMax ? kBytesDumpMax : len;
  uint32_t h = 0, t = 0;
  for (uint32_t i = 0; i < shown; i++) {
    hex [h++] = "0123456789abcdef"[(data [i] >> 4) & 0xf];
    hex [h++] = "0123456789abcdef"[data [i] & 0xf];
    hex [h++] = ' ';
    text [t++] = (data [i] >= 0x20 && data [i] < 0x7f) ? static_cast<char> (data [i]) : '.';
  }
  hex [h] = '\0';
  text [t] = '\0';
  IOLog ("clover r183: %s n=%u hex=%s ascii=[%s]\n", label, len, hex, text);
}

// A ds_* code in words, for the same reason r182 named its transaction codes: DS_E_RANGE and
// DS_E_MAGIC are different findings about a heap somebody else writes, and only one of them means the
// layout this port implements is the wrong one.
static const char *dsCodeName (int rc)
{
  switch (rc) {
    case DS_E_SPAN:    return "span-too-small";
    case DS_E_RANGE:   return "outside-span";
    case DS_E_ALIGN:   return "unaligned";
    case DS_E_MAGIC:   return "bad-magic";
    case DS_E_VERSION: return "bad-version";
    case DS_E_COUNT:   return "bad-count";
    case DS_E_CANARY:  return "bad-canary";
    case DS_E_PART:    return "bad-partition";
    case DS_E_ITEM:    return "item-absent";
    case DS_E_SIZE:    return "item-too-small";
    case DS_E_PID:     return "pid-mismatch";
    default:           return "ok";
  }
}

static void note (uint64_t step, int rc)
{
  clover_dsp_observation[15] = rc ? step * 1000 + static_cast<uint64_t> (rc) : step;
}

// ------------------------------------------------------------------ mode 1: the heap
//
// The header words and the global TOC. Printed even when everything else fails, because `initialized`
// is the single word that separates "the boot never built a SMEM heap" from "the heap is there and the
// partition table is elsewhere" -- and those two need different next rounds.
static int dumpHeader (void)
{
  struct ds_header header;
  const struct ds_span span = smemSpan ();
  const int rc = ds_header (span, &header);
  if (rc) {
    IOLog ("clover r183: smem header rc=%d (%s) size=%u\n", rc, dsCodeName (rc), span.size);
    note (kStepHeader, rc);
    return rc;
  }
  IOLog ("clover r183: smem initialized=%08x free=%u available=%u reserved=%08x sbl=%08x pc=%08x/%08x\n",
         header.initialized, header.free_offset, header.available, header.reserved,
         header.sbl_version, header.pc_command, header.pc_status);
  IOLog ("clover r183: global toc scanned=%u allocated=%u highest=%u\n",
         header.toc_scanned, header.toc_allocated, header.toc_highest);
  clover_dsp_observation[2] = header.initialized;
  clover_dsp_observation[3] = header.free_offset;
  clover_dsp_observation[4] = (static_cast<uint64_t> (header.toc_highest) << 16) |
                              (header.toc_allocated & 0xffff);
  if (header.initialized == 0) {
    // Not a failure -- a reading. It says the boot that owns this heap never marked it alive, and the
    // rest of the walk is then expected to be empty. Kept going rather than stopped, because a zero
    // `initialized` with a populated "$TOC" is exactly the legacy-layout surprise this driver would
    // rather find than assume.
    IOLog ("clover r183: note initialized=0; continuing, the table may still be there\n");
  }
  note (kStepHeader, DS_OK);
  return DS_OK;
}

// The global TOC listing, which is mode 1's answer to the question the four named items of mode 2
// cannot ask: a heap can be alive and hold things this port has no name for. Printing the first
// allocated entries distinguishes "the heap is empty" from "the heap is populated with items whose
// numbers we did not guess", and the second is a finding about the numbering rather than about the
// DSP. Two bounds, both stated: kTocDumpLimit lines so the log ring keeps its useful lines, and the
// walk itself bounded by the mapping through ds_toc(), because a 2 MiB window is what makes all 512
// entries reachable and a smaller mapping is a payload-geometry answer.
//
// `declared` is smem_info's num_items when the partition table carried one -- the number the vendor
// driver uses as the bound of its own TOC walk (VEND smem.c:955 `num_items < SMEM_NUM_ITEMS`). Entries
// past it that are still non-zero are counted, not printed: they mean the heap and the table disagree
// about how big the heap is, which changes how every number below should be read.
static void dumpTocListing (uint32_t declared, uint32_t *listed, uint32_t *beyond)
{
  const struct ds_span span = smemSpan ();
  *listed = 0;
  *beyond = 0;
  if (span.size / 4 < kSmemTocWords) {
    IOLog ("clover r183: toc truncated in the mapping: size=%u words=%u need=%u\n",
           span.size, span.size / 4, kSmemTocWords);
  }
  uint32_t allocated = 0;
  for (uint32_t item = 0; item < kSmemItemCount; item++) {
    struct ds_tocEntry entry;
    if (ds_toc (span, item, &entry)) {
      break;                       // past the mapping; the line below says how far the walk got
    }
    if (!entry.allocated) {
      continue;
    }
    allocated++;
    if (declared && item >= declared) {
      (*beyond)++;
    }
    if (*listed >= kTocDumpLimit) {
      continue;
    }
    (*listed)++;
    IOLog ("clover r183: toc[%u] off=%u size=%u in_span=%u aux=%08x phys=%llx\n", item,
           entry.offset, entry.size, entry.in_span, entry.aux_base,
           static_cast<unsigned long long> (entry.phys));
  }
  IOLog ("clover r183: toc allocated=%u listed=%u beyond_declared=%u declared=%u\n",
         allocated, *listed, *beyond, declared);
}

static void dumpPtable (struct ds_ptable *outTable)
{
  const struct ds_span span = smemSpan ();
  struct ds_ptable table;
  const int rc = ds_ptable (span, &table);
  clover_dsp_observation[5] = table.magic;
  clover_dsp_observation[6] = (static_cast<uint64_t> (table.info_num_items) << 16) |
                              (table.num_entries & 0xffff);
  if (rc) {
    IOLog ("clover r183: ptable at %08x magic=%08x version=%u entries=%u rc=%d (%s)\n",
           table.at, table.magic, table.version, table.num_entries, rc, dsCodeName (rc));
    note (kStepPtable, rc);
    *outTable = table;
    return;
  }
  IOLog ("clover r183: ptable at %08x magic=$TOC version=%u entries=%u info=%08x num_items=%u capacity=%u\n",
         table.at, table.version, table.num_entries, table.info_magic,
         table.info_num_items, table.capacity);
  note (kStepPtable, DS_OK);
  // Every row, followed far enough to say whether its own header agrees. A row that points off the
  // mapping is printed with rc rather than dereferenced; that is the case the host model asserts.
  const uint32_t rows = table.num_entries > kPtableDumpLimit ? kPtableDumpLimit : table.num_entries;
  for (uint32_t i = 0; i < rows; i++) {
    struct ds_partition partition;
    const int prc = ds_partition_at (span, table, i, &partition);
    // A host number outside the vendor's SMEM_HOST_COUNT is not in itself a refusal -- the row is
    // still printed with its real contents -- but it is the signature of a table read at the wrong
    // stride, and once the numbers are in the log the two explanations can be told apart.
    const int hostsSane = partition.host0 < kSmemHostCount && partition.host1 < kSmemHostCount;
    IOLog ("clover r183: part[%u] hosts=%u:%u off=%u size=%u cacheline=%u hdr=%08x ok=%d hosts_ok=%d rc=%d (%s)\n",
           i, partition.host0, partition.host1, partition.offset, partition.size,
           partition.cacheline, partition.part_magic, partition.header_ok, hostsSane, prc,
           dsCodeName (prc));
  }
  note (kStepPartition, DS_OK);
  *outTable = table;
}

// ------------------------------------------------------------------ mode 2: the edges
//
// The doorbell word, read once, printed raw and then per edge against the masks this board's tree
// declares. Nothing is written: on the shipping block a write of BIT(n) is the interrupt, so this load
// is the only way to ask the question without becoming part of the answer.
static void dumpDoorbells (uint32_t *bitmap)
{
  const uint32_t value = windowRead (gApcs, kApcsIpcOffset);
  clover_dsp_observation[9] = value;
  // Printed as the word the shipped tree names, not as the block plus an offset: the reader compares
  // this address against /proc/device-tree/soc/qcom,smp2p-adsp@17911008/reg, and a base that had lost
  // a digit would not survive that comparison as 17911008 would.
  IOLog ("clover r183: doorbell %llx+04 = %08x\n",
         static_cast<unsigned long long> (kApcsIpcPhys), value);
  for (uint32_t i = 0; i < kDspEdgeCount; i++) {
    const struct DspEdge &edge = kDspEdges [i];
    const int set = ds_bitSet (value, edge.irqMask);
    if (set) {
      *bitmap |= 1u << i;
    }
    // The outbound item the vendor's numbering implies: SMEM_SMP2P_APPS_BASE plus the remote's pid,
    // which is the rule the five groups of kSmemSmp2pItemStride apart make visible (modem 428 = 427+1,
    // audio/ADSP 429 = 427+2, cdsp 432 = 427+5) and which stops holding at the group boundary -- hence
    // the bound rather than a bare addition. Compared against what the CAF tree actually declares for
    // this edge, not assumed: the inbound side is the one that breaks it (the cdsp's is 94), and a row
    // where prediction and declaration agree is a row whose arithmetic earned the next round's trust.
    const uint32_t predicted = edge.remotePid < kSmemSmp2pItemStride ?
                               kSmemSmp2pAppsBase + edge.remotePid : 0xffffffff;
    const int law = predicted == 0xffffffff || edge.smemOutboundItem == 0xffffffff ? -1 :
                    (predicted == edge.smemOutboundItem ? 1 : 0);
    // One line per edge, always: "no bits set at all" has to be distinguishable from "this edge's
    // mask was never in the register", and the only way to show that is to print the mask alongside
    // the word for every row, including the ones that answer no.
    IOLog ("clover r183: edge[%u] %s pid=%u mask=%08x mbox=%s spi=%u set=%d out=%u pred=%u law=%d in=%u\n",
           i, edge.label, edge.remotePid, edge.irqMask,
           edge.cafMboxIndex == 0xffffffff ? "none" : "", edge.inboundSpi, set,
           edge.smemOutboundItem, predicted, law, edge.smemInboundItem);
  }
  clover_dsp_observation[10] = *bitmap;
  note (kStepDoorbell, DS_OK);
}

// An item number is only a guess until the bytes at it agree. So: try the private partition the two
// hosts share, then the global TOC, and validate whatever comes back as an SMP2P header. `expect_local`
// is the AP's own pid (0) for the outbound direction and the remote's for the inbound one -- the pair
// the vendor driver's own fields would have to show if the item really belongs to this edge.
static void dumpSmp2pItem (const char *what, uint32_t item, uint32_t expectLocal,
                           uint32_t expectRemote, int *verdictSlot)
{
  const struct ds_span span = smemSpan ();
  struct ds_item found;
  int rc = DS_E_ITEM;
  int path = 0;                       // 0 not found, 1 private partition, 2 global TOC

  struct ds_partition partition;
  const int prc = ds_find_partition (span, kSmemHostApps, expectRemote, &partition);
  if (prc == DS_OK) {
    rc = ds_find_item (span, partition, item, &found);
    if (rc == DS_OK) {
      path = 1;
    }
  }
  if (path == 0) {
    const int grc = ds_find_item_global (span, item, &found);
    if (grc == DS_OK) {
      rc = DS_OK;
      path = 2;
    } else if (rc != DS_OK) {
      // Report whichever refusal explains more: the private-partition code when there was a partition
      // to walk, the TOC code when there was not.
      rc = prc == DS_OK ? rc : prc;
    }
  }
  if (path == 0) {
    IOLog ("clover r183: %s item=%u absent rc=%d (%s)\n", what, item, rc, dsCodeName (rc));
    if (verdictSlot) {
      *verdictSlot = rc;
    }
    return;
  }
  // Found in the heap; now the question the shipped DTB cannot answer: is it this edge's item?
  struct ds_smp2p head;
  const int hrc = ds_smp2p_head (span, found.data, expectLocal, expectRemote, &head);
  IOLog ("clover r183: %s item=%u path=%s data=%llx phys=%llx size=%u hdr magic=%08x ver=%u "
         "pids=%u:%u total=%u valid=%u flags=%08x\n",
         what, item, path == 1 ? "private" : "global",
         static_cast<unsigned long long> (found.data),
         static_cast<unsigned long long> (span.phys + found.data), found.size,
         head.magic, head.version, head.local_pid, head.remote_pid,
         head.total_entries, head.valid_entries, head.flags);
  if (hrc) {
    IOLog ("clover r183: %s item=%u rejected rc=%d (%s)\n", what, item, hrc, dsCodeName (hrc));
    if (verdictSlot) {
      *verdictSlot = hrc;
    }
    note (kStepItem, hrc);
    return;
  }
  if (verdictSlot) {
    *verdictSlot = DS_OK;
  }
  // Two further decodings of the header's own words, printed because they are the lifecycle handshake
  // rather than the layout: `features`' SSR-ack bit says the edge takes part in a subsystem restart
  // negotiation at all (VEND smp2p.c:49), and the two `flags` bits are that negotiation's two halves
  // (:47-48). Which side writes which is a protocol question this round does not answer -- but an item
  // whose bits are all zero is an edge that was established and never restarted, and that is a third
  // possible answer between "no item" and "item present", for free.
  IOLog ("clover r183: %s features=%06x ssr_ack=%d flags=%08x restart_done=%d restart_ack=%d\n",
         what, head.features, (head.features & kSmp2pFeatureSsrAck) != 0, head.flags,
         (head.flags & kSmp2pFlagRestartDone) != 0,
         (head.flags & kSmp2pFlagRestartAck) != 0);
  // The entry names are the content of an established edge: "master-kernel" and "slave-kernel" mean
  // the SSRL handshake this board's PIL node drives (VEND sdm660.dtsi:1935, DTB:5877), and an edge
  // whose entries have names is an edge somebody actually negotiated.
  const uint32_t shown = head.valid_entries > kSmp2pEntryDumpLimit ?
                         kSmp2pEntryDumpLimit : head.valid_entries;
  for (uint32_t i = 0; i < shown; i++) {
    char name [kSmp2pMaxEntryName + 1];
    uint32_t value = 0;
    const int erc = ds_smp2p_entry (span, head, i, name, sizeof (name), &value);
    if (erc) {
      IOLog ("clover r183: %s entry[%u] rc=%d (%s)\n", what, i, erc, dsCodeName (erc));
      continue;
    }
    IOLog ("clover r183: %s entry[%u] name=[%s] value=%08x\n", what, i, name, value);
  }
  const volatile uint8_t *data = span.base + found.data;
  printBytes (what, data, kBytesDumpMax);
  note (kStepItem, DS_OK);
}

// A raw item, for the slots that are not SMP2P headers: the ADSP's SSR state (item 423, the same slot
// sheng calls kItemCrash) and the GLINK native-transport descriptor and FIFOs. Printed as bytes,
// because the honest statement about their layout at this point is that it is a protocol question --
// but an ASCII payload is a presence answer, and presence is what was asked.
static void dumpRawItem (const char *what, uint32_t item, uint32_t remotePid)
{
  const struct ds_span span = smemSpan ();
  struct ds_item found;
  int rc = DS_E_ITEM;
  int path = 0;
  struct ds_partition partition;
  if (remotePid != 0xffffffff &&
      ds_find_partition (span, kSmemHostApps, remotePid, &partition) == DS_OK &&
      ds_find_item (span, partition, item, &found) == DS_OK) {
    path = 1;
  } else if (ds_find_item_global (span, item, &found) == DS_OK) {
    path = 2;
  } else {
    IOLog ("clover r183: %s item=%u absent rc=%d (%s)\n", what, item, rc, dsCodeName (rc));
    return;
  }
  IOLog ("clover r183: %s item=%u path=%s data=%llx phys=%llx size=%u\n", what, item,
         path == 1 ? "private" : "global", static_cast<unsigned long long> (found.data),
         static_cast<unsigned long long> (span.phys + found.data), found.size);
  printBytes (what, span.base + found.data, found.size < kBytesDumpMax ? found.size : kBytesDumpMax);
}

// ------------------------------------------------------------------ mode 3: the images
//
// One region at a time, sixteen pages each, mapped fresh per region and released before the next. The
// window is a whole number of pages because ds_image() counts pages, and the phys is printed with the
// result so a non-zero page can be located by an address rather than by an offset into a mapping that
// no longer exists. `slot` keeps the page counts; `elfSlot` keeps the byte offset of the first word
// that read as the ELF magic, plus one -- plus one because offset 0 is the case that matters most (a
// PIL load puts the ELF header at the region base) and a slot that cannot distinguish "nothing" from
// "at zero" would be a slot the offline decoder has to guess about. The slots are the observation
// array's own elements, so they carry its volatility: the array is volatile because a panic between
// two of its writes still has to leave the first one in memory for whatever dumps the card next, and
// a plain uint64_t * parameter would quietly strip that on the way in.
static void dumpImage (const char *what, uint64_t phys, uint32_t size, volatile uint64_t *slot,
                       volatile uint64_t *elfSlot)
{
  const uint32_t windowSize = kImageScanPages * kImagePageSize;
  const uint32_t mapped = size < windowSize ? size : windowSize;
  if (!mapWindow (gImage, phys, mapped)) {
    IOLog ("clover r183: image %s map %llx+%u failed\n", what,
           static_cast<unsigned long long> (phys), mapped);
    clover_dsp_observation[1] = phys;
    note (kStepImage, static_cast<int> (kFailMap));
    return;
  }
  struct ds_span span;
  span.base = reinterpret_cast<volatile uint8_t *> (gImage.base);
  span.size = mapped;
  span.phys = phys;
  struct ds_image image;
  const int rc = ds_image (span, kImageScanPages, &image);
  if (rc) {
    IOLog ("clover r183: image %s rc=%d (%s)\n", what, rc, dsCodeName (rc));
    gImage.map = OSSharedPtr<IOMemoryMap> ();
    note (kStepImage, rc);
    return;
  }
  IOLog ("clover r183: image %s %llx+%u pages=%u nonzero=%u first=%u elf_at=%s head0=%08x\n",
         what, static_cast<unsigned long long> (phys), mapped, image.pages, image.nonzero,
         image.first_nonzero, image.elf_at == 0xffffffff ? "none" : "", image.head [0]);
  if (image.elf_at != 0xffffffff) {
    IOLog ("clover r183: image %s elf words=%u at+%x phys=%llx\n", what, image.elf_count,
           image.elf_at, static_cast<unsigned long long> (phys + image.elf_at));
  }
  if (slot) {
    *slot = (static_cast<uint64_t> (image.nonzero) << 16) | (image.pages & 0xffff);
  }
  if (elfSlot) {
    *elfSlot = image.elf_at == 0xffffffff ? 0 : static_cast<uint64_t> (image.elf_at) + 1;
  }
  gImage.map = OSSharedPtr<IOMemoryMap> ();
  gImage.base = 0;
  note (kStepImage, DS_OK);
}

// ------------------------------------------------------------------ the entry point
//
// Opt-in, silent without the boot argument, and never fatal. The chain is sheng_pmgr_run() ->
// clover_emmc_run() -> clover_input_run() -> clover_power_run() -> this, so the touch report and the
// rail report are always upstream of this one: a mode-3 "the ADSP was never loaded" that follows a
// mode-3 "L9 enable = 0x00" is a two-step plan, and the same reading after a "L9 = 0x80" is a one-step
// plan. Order matters for the report even though nothing here depends on it.
extern "C" void clover_dsp_run (void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn ("clover-dsp", &mode, sizeof (mode)) || !mode) {
    return;
  }
  clover_dsp_observation[0] = mode;
  if (mode != 1 && mode != 2 && mode != 3) {
    IOLog ("clover r183: mode %u is not implemented (1 heap, 2 edges, 3 images)\n", mode);
    return;
  }

  IOLog ("clover r183: mode %u smem=%llx+%u apcs=%llx+%x\n", mode,
         static_cast<unsigned long long> (kSmemPhys), kSmemSize,
         static_cast<unsigned long long> (kApcsPhys), kApcsSize);

  // Map before reading, and name the window that failed. For SMEM this is the more interesting failure
  // than it looks: the region is DRAM the shipping kernel took out of its allocator, and if our boot's
  // memory map does not carry it, the answer is a payload-geometry problem rather than a DSP problem.
  if (!mapWindow (gSmem, kSmemPhys, kSmemSize)) {
    IOLog ("clover r183: map smem %llx+%u failed\n",
           static_cast<unsigned long long> (kSmemPhys), kSmemSize);
    clover_dsp_observation[1] = kSmemPhys;
    clover_dsp_observation[15] = kStepMap * 1000 + kFailMap;
    clover_emmc_flush_cache ();
    return;
  }
  if (!mapWindow (gApcs, kApcsPhys, kApcsSize)) {
    IOLog ("clover r183: map apcs %llx+%u failed\n",
           static_cast<unsigned long long> (kApcsPhys), kApcsSize);
    clover_dsp_observation[1] = kApcsPhys;
    clover_dsp_observation[15] = kStepMap * 1000 + kFailMap;
    clover_emmc_flush_cache ();
    return;
  }

  const int hrc = dumpHeader ();
  struct ds_ptable table;
  dumpPtable (&table);
  uint32_t listed = 0, beyond = 0;
  dumpTocListing (table.info_num_items, &listed, &beyond);

  // A heap whose header cannot be read has no point in looking for edges in it: every item lookup
  // would be a read of an unmapped or unvalidated structure, and the abort that could produce is the
  // one cost this round is not allowed to pay. The partition table is skipped for the same reason
  // when its magic is wrong, but not when it is merely absent -- an absent table with a live header is
  // the legacy layout, and mode 2's global-TOC path is exactly the one that still works there.
  if (hrc && hrc != DS_E_SPAN) {
    IOLog ("clover r183: header rc=%d; partition paths disabled, global lookups only\n", hrc);
  }

  if (mode == 1) {
    IOLog ("clover r183: mode 1 read the heap only, no doorbell, no item, no image (listed=%u beyond=%u)\n",
           listed, beyond);
    clover_emmc_flush_cache ();
    return;
  }

  uint32_t edgeBitmap = 0;
  dumpDoorbells (&edgeBitmap);

  // The four numbers the two trees disagree about being present: the CAF tree's explicit pair for the
  // ADSP edge, the same pair for the modem edge, the arithmetic the shipped tree would have to be
  // using, and the cdsp inbound that does not follow that arithmetic at all. Each is tried as a
  // candidate and judged by its own header.
  int outRc = 0, inRc = 0;
  dumpSmp2pItem ("adsp-out", kSmemItemAdspSmp2pOut, 0, 2, &outRc);
  dumpSmp2pItem ("adsp-in", kSmemItemAdspSmp2pIn, 2, 0, &inRc);
  clover_dsp_observation[7] = static_cast<uint64_t> (outRc);
  clover_dsp_observation[8] = static_cast<uint64_t> (inRc);
  dumpSmp2pItem ("modem-out", kSmemItemModemSmp2pOut, 0, 1, nullptr);
  dumpSmp2pItem ("modem-in", kSmemItemModemSmp2pIn, 1, 0, nullptr);
  dumpSmp2pItem ("cdsp-in", kSmemItemCdspSmp2pIn, 5, 0, nullptr);

  // The transport the audio and compute channels would ride, and the ADSP's own lifecycle record.
  dumpRawItem ("adsp-ssr", kSmemItemAdspSsr, 2);
  dumpRawItem ("glink-desc", kSmemItemGlinkDesc, 2);
  dumpRawItem ("glink-fifo0", kSmemItemGlinkFifo0, 2);
  dumpRawItem ("glink-fifo1", kSmemItemGlinkFifo1, 2);
  dumpRawItem ("image-version", kSmemItemImageVersion, 2);

  if (mode == 2) {
    IOLog ("clover r183: mode 2 done doorbell=%08x edges_set=%02x adsp_out=%d adsp_in=%d\n",
           static_cast<uint32_t> (clover_dsp_observation[9]), edgeBitmap, outRc, inRc);
    clover_emmc_flush_cache ();
    return;
  }

  dumpImage ("adsp-fw", kAdspFwPhys, kAdspFwSize, &clover_dsp_observation[11],
             &clover_dsp_observation[12]);
  dumpImage ("wlan-msa", kWlanMsaPhys, kWlanMsaSize, &clover_dsp_observation[13],
             &clover_dsp_observation[14]);
  dumpImage ("wlan-guard", kWlanGuardPhys, kWlanGuardSize, nullptr, nullptr);

  // The verdict this round is built to produce, stated in the two numbers that decide 排期 5's shape.
  // Everything above is evidence; this line is the conclusion the next round is chosen from, and it is
  // deliberately phrased as a reading rather than a plan: "no SMP2P item and an empty carve-out" would
  // still be compatible with a DSP the AP cannot see because the MPU refuses, which is what the
  // doorbell word and the mapping failures above are there to separate.
  IOLog ("clover r183: verdict adsp_edge=%s adsp_loaded=%s (mode 3 done)\n",
         outRc == DS_OK || inRc == DS_OK ? "seen" : "not seen",
         clover_dsp_observation[11] & 0xffff ? "pages nonzero" : "pages zero");
  clover_dsp_observation[15] = kStepComplete;
  clover_emmc_flush_cache ();
}
