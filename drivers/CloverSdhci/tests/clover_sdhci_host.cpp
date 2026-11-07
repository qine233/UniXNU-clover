// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): round341's host model for drivers/CloverSdhci/SdhciLogCore.h.
//
// WHAT THIS IS
//
// The record layer compiled against a fake card: a 1217-sector byte array, a PIO writer that copies
// into it, and the two staging rings' and the tee's state machines replayed with the same counters the
// driver keeps. Nothing here reads a register or pretends to be IOKit; the point is that every decision
// the driver makes about *where a record goes, what it holds and when a row is lost* can be asked twice
// as many questions as the board has ever been able to answer, because the board only ever answered the
// ones its own rate happened to produce.
//
// WHY THE SHAPE OF THE CASES IS THE POINT
//
// Boot 120's card has kind-22 records whose body is exactly the writer's 320-byte bound -- 631 of them
// across the archived readings -- and the relay's `dropped=` has been zero on every boot ever read
// because no boot's census has yet handed over more than 62 rows. Both of those are accidents of rate.
// The instrument that cannot ask "what if the consumer kept up" (case relay_drained) and "what if a line
// is longer than a record" (case tee_cut) cannot tell those two accidents from facts, which is what
// round341's two findings are.
//
// WHAT IS NOT PROVED HERE
//
// That the card holds these records at these sectors: only a read-back can say that, and this model's
// output is handed to scripts/read-clover-log.py for exactly the part a host can check -- that the
// writer's arithmetic and the reader's decode agree about the bytes the writer produced. The window file
// is written to argv[1]; the Python side decodes it with the reader's own constants, which is a second
// producer and not a restatement of the first.
#include "../SdhciLogCore.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int problems = 0;

static void fail (const char *what)
{
  std::printf ("PROBLEM %s\n", what);
  problems++;
}

static void check (const char *what, uint64_t got, uint64_t want)
{
  if (got != want) {
    std::printf ("PROBLEM %s: got %llu, expected %llu\n", what,
                 (unsigned long long) got, (unsigned long long) want);
    problems++;
  }
}

// ------------------------------------------------------------------ the fake card
// The window is the whole thing the host's dd reads: sectors [0, 1217) of the logdump partition,
// addressed here from the window's own start so gVerdictFirst's role is visible.
struct FakeCard {
  std::vector<uint8_t> window;
  uint32_t first_lba;
  uint32_t writes;
  uint32_t refusals;

  explicit FakeCard (uint32_t sectors)
    : window (sectors * sdhciLogSectorBytes, 0), first_lba (13320216u), writes (0), refusals (0) {}

  // The driver's writePioBlock: one 512-byte block at an absolute LBA, and a caller that reads the
  // window back relative to first_lba. A write outside the window is the driver's own bug and this
  // refuses it loudly rather than letting the byte go off the end of the array.
  bool write (uint32_t lba, const void *block, bool accept)
  {
    if (!accept) {
      refusals++;
      return false;
    }
    const uint64_t at = (uint64_t) (lba - first_lba) * sdhciLogSectorBytes;
    if (lba < first_lba || at + sdhciLogSectorBytes > window.size ()) {
      fail ("a record was written outside the window the host reads");
      return false;
    }
    std::memcpy (&window[at], block, sdhciLogSectorBytes);
    writes++;
    return true;
  }

  // Reading the window back the way the host's dd does: an absolute lba, a word index inside the
  // header sector. Round342's case needs it because the claim is about what survives on the card, not
  // about what the writer intended.
  uint32_t word (uint32_t lba, uint32_t index) const
  {
    const uint64_t at = (uint64_t) (lba - first_lba) * sdhciLogSectorBytes + 4u * index;
    uint32_t value = 0;
    if (at + 4u <= window.size ()) {
      std::memcpy (&value, &window[at], 4u);
    }
    return value;
  }

  char byte (uint32_t lba, uint32_t index) const
  {
    const uint64_t at = (uint64_t) (lba - first_lba) * sdhciLogSectorBytes + index;
    return at < window.size () ? (char) window[at] : 0;
  }
};

// ------------------------------------------------------------------ the staging ring, replayed
// The driver's own state, in the same shape: gRingWrite/gRingPending/gRingDropped for the RAM side and
// gRingCursor/gRingSeq/gRingWritten for the card side. The three decisions are the layer's predicates;
// the loop that moves a row from RAM to a sector mirrors flushRing, and scripts/check-clover.py's
// Round341 block is what keeps that mirror honest by refusing the tree if the driver stops calling the
// same names.
struct StagingRing {
  uint32_t which;
  std::vector<std::string> text;      // kRingRamSlots entries
  uint32_t write_cursor = 0;          // gRingWrite
  uint32_t pending = 0;               // gRingPending
  uint32_t dropped = 0;               // gRingDropped
  uint32_t cursor = 0;                // gRingCursor
  uint32_t seq = 0;                   // gRingSeq
  uint32_t written = 0;               // gRingWritten
  std::vector<uint32_t> slots_written;

  explicit StagingRing (uint32_t r)
    : which (r), text (kRingRamSlots), slots_written () {}

  void push (const std::string &line)
  {
    uint32_t length = line.size () > kVerdictTextBytes ? kVerdictTextBytes : (uint32_t) line.size ();
    if (sdhci_ram_full (pending)) {
      // Full: the oldest unread row gives its slot to the newest one, and that is counted.
      write_cursor++;
      pending--;
      dropped++;
    }
    const uint32_t slot = sdhci_ram_slot (write_cursor);
    text[slot] = line.substr (0, length);
    write_cursor++;
    pending++;
  }

  bool flush (FakeCard &card, uint32_t boot_id, uint32_t kind, bool accept)
  {
    uint32_t flushed = 0;
    while (flushed < kRingFlushPerPass[which] && pending) {
      const uint32_t slot = sdhci_ram_slot (write_cursor - pending);
      const std::string &line = text[slot];
      const uint32_t length = (uint32_t) line.size ();
      // The same one local the driver reads: the row decides its own home before the sector is chosen,
      // and the header's slot word and the lba are both derived from that single answer.
      const int pin = sdhci_pin_index (line.c_str (), length, which);
      const uint32_t lba = sdhci_ring_lba (card.first_lba, which, cursor, pin);
      uint32_t header[sdhciLogBlockWords] = {0};
      sdhci_build_header (header, lba, which, cursor, seq, boot_id, kind, pin, line.c_str (),
                          length);
      std::vector<uint8_t> block (sdhciLogSectorBytes, 0);
      sdhci_build_text (block.data (), line.c_str (), length);
      if (!card.write (lba, header, accept) ||
          !card.write (lba + 1, block.data (), accept)) {
        break;                       // a refusal stops this ring for this pass, cursor unmoved
      }
      cursor++;
      seq++;
      written++;
      pending--;
      flushed++;
      slots_written.push_back (sdhci_ring_slot (which, cursor - 1, pin));
    }
    return flushed != 0;
  }
};

// ------------------------------------------------------------------ the relay, replayed
// One producer taking sequence numbers, one consumer advancing a cursor, and the two rates set by the
// case. The layer's predicate is the only thing that decides whether a hand-out lost a row.
struct Relay {
  uint32_t seq = 0;                   // gProbeSeq
  uint32_t next = 0;                  // gProbeNext
  uint32_t dropped = 0;               // gProbeDropped
  uint32_t positional = 0;            // what round341's old rule would have reported
  std::vector<uint32_t> sealed;       // gProbeArm: the number that sealed each slot

  Relay () : sealed (kProbeSlots, 0) {}

  void say (uint32_t n)
  {
    for (uint32_t i = 0; i < n; i++) {
      const uint32_t taken = seq++;
      if (sdhci_relay_overwrote_unconsumed (taken, next)) {
        dropped++;
      }
      if (taken >= kProbeSlots) {
        positional++;
      }
      sealed[taken % kProbeSlots] = taken + 1;
    }
  }

  void consume (uint32_t n)
  {
    for (uint32_t i = 0; i < n && next < seq; i++) {
      next++;
    }
  }
};

// ------------------------------------------------------------------ the tee's assembler, replayed
// The driver's switch, byte for byte, and a witness for the rule round341 replaced: the old branch
// emitted the first 320 bytes, destroyed the byte that arrived at the bound, and then assembled the
// remainder as a second row. Both are run here so the numbers each rule produces are a reading rather
// than a description.
struct TeeRun {
  std::vector<std::string> rows;
  std::string current;
  uint32_t partial = 0;
  uint32_t truncated = 0;
  uint32_t cut = 0;
  uint32_t bytes_lost = 0;
};

static void tee_feed_new (TeeRun &run, uint8_t c)
{
  switch (sdhci_tee_feed (c, run.partial, run.truncated)) {
    case kSdhciTeeStore:
      run.current += (char) c;
      run.partial++;
      break;
    case kSdhciTeeOverflow:
      run.truncated = 1;
      break;
    case kSdhciTeeEmitCut:
      run.rows.push_back (run.current);
      run.cut++;
      run.current.clear ();
      run.partial = 0;
      run.truncated = 0;
      break;
    case kSdhciTeeEmit:
      if (run.partial) {
        run.rows.push_back (run.current);
      }
      run.current.clear ();
      run.partial = 0;
      run.truncated = 0;
      break;
    default:
      break;
  }
}

// The rule round341 replaced, kept as a witness so the comparison is a second reading and not a
// description. It is not the layer's behaviour and nothing in the driver uses it.
static void tee_feed_old (TeeRun &run, uint8_t c)
{
  if (c == '\n') {
    if (run.partial) {
      run.rows.push_back (run.current);
    }
    run.current.clear ();
    run.partial = 0;
    return;
  }
  if (run.partial < kVerdictTextBytes) {
    run.current += (char) c;
    run.partial++;
  } else {
    if (run.partial) {
      run.rows.push_back (run.current);
      run.current.clear ();
      run.partial = 0;
    }
    run.bytes_lost++;
  }
}

// ------------------------------------------------------------------ the cases
static void case_geometry (void)
{
  std::printf ("case geometry boot_record=%u kernel_end=%u window_sectors=%u window_bytes=%u\n",
               kBootRecordOffset, kRingOffset[kRingKernel] + kRingSectorsPerRecord *
               kRingCardSlots[kRingKernel], kBootRecordOffset + 1,
               (kBootRecordOffset + 1) * sdhciLogSectorBytes);
  check ("the kernel ring ends where the boot record begins",
         kRingOffset[kRingKernel] + kRingSectorsPerRecord * kRingCardSlots[kRingKernel],
         kBootRecordOffset);
  check ("the driver ring ends where the kernel ring begins",
         kRingOffset[kRingDriver] + kRingSectorsPerRecord * kRingCardSlots[kRingDriver],
         kRingOffset[kRingKernel]);
  check ("one block is 512 bytes", sdhciLogBlockWords * 4u, sdhciLogSectorBytes);
}

static void case_fits (void)
{
  // What has to be true for the reader's two-sector stride to be the whole record: every length the
  // writer can be handed, clamped or not, must fit the one text sector the reader reads. The bound is
  // swept rather than quoted, because 480 and 512 are typed for different purposes in different files
  // and a clamp that was 9 too generous would put a record's tail in a sector nobody reads -- a
  // silence with no error in it.
  uint32_t last_fitting = 0;
  for (uint32_t body = 0; body <= 600; body++) {
    if (sdhci_record_fits_one_sector (body)) {
      last_fitting = body;
    }
  }
  uint32_t widest = 0;
  for (uint32_t length = 0; length <= 4000; length++) {
    if (sdhci_record_total (sdhci_record_body (length)) > widest) {
      widest = sdhci_record_total (sdhci_record_body (length));
    }
  }
  check ("the widest record still fits one sector", widest <= sdhciLogSectorBytes ? 1u : 0u, 1u);
  check ("the clamp at 480", sdhci_record_body (500), kRecordBodyBytes);
  check ("no clamp under 480", sdhci_record_body (480), 480u);
  // The slack is measured, not assumed: the clamp is stricter than the sector by this many bytes, and
  // the staging ring's own bound sits under both.
  std::printf ("case fits clamp=%u fit_bound=%u slack=%u widest_total=%u sector=%u row_bound=%u\n",
               kRecordBodyBytes, last_fitting, last_fitting - kRecordBodyBytes, widest,
               sdhciLogSectorBytes, kVerdictTextBytes);
  check ("the clamp is the stricter of the two bounds",
         kVerdictTextBytes <= last_fitting && kVerdictTextBytes <= kRecordBodyBytes ? 1u : 0u, 1u);
}

static void case_wrap (void)
{
  FakeCard card (kBootRecordOffset + 1);
  StagingRing driver (kRingDriver);
  uint32_t passes = 0;
  for (uint32_t i = 0; i < 200; i++) {
    driver.push ("clover r341 row " + std::to_string (i));
    if (i % 4 == 3) {
      driver.flush (card, 341, 0, true);
      passes++;
    }
  }
  while (driver.pending) {
    driver.flush (card, 341, 0, true);
    passes++;
  }
  check ("every accepted row reached the card", driver.written, 200u);
  check ("nothing was dropped when the flush kept up", driver.dropped, 0u);
  check ("the card cursor advanced once per record", driver.cursor, 200u);
  // Round342 changed the denominator here. None of these 200 rows is a pinned row, so they turn in the
  // 60 slots the pinned four leave: three laps plus twenty, and the sector that repeats record 0's is
  // record 60's -- not record 64's, which is what round341's arithmetic said and what made boot 120
  // lose its first four rows. Both halves are checked, because the second one is the fact that the
  // reader's old `(slot, seq)` congruence assumption can no longer tell.
  const uint32_t rolling_slots = kRingCardSlots[kRingDriver] - kRingPinnedSlots;
  check ("the rolling part of the ring wrapped three times", driver.cursor / rolling_slots, 3u);
  check ("the slot the wrap landed on", sdhci_ring_slot (kRingDriver, 199, -1),
         kRingPinnedSlots + 199u % rolling_slots);
  const uint32_t lba_0 = sdhci_ring_lba (card.first_lba, kRingDriver, 0, -1);
  const uint32_t lba_60 = sdhci_ring_lba (card.first_lba, kRingDriver, 60, -1);
  const uint32_t lba_64 = sdhci_ring_lba (card.first_lba, kRingDriver, 64, -1);
  check ("record 60 reuses record 0's sector", lba_60, lba_0);
  if (lba_64 == lba_0) {
    fail ("record 64 landed on record 0's sector: the pinned slots are not out of the modulus");
  }
  check ("the sequence word is what orders a wrapped ring", driver.seq, 200u);
  // dropped= is printed, not only checked: scripts/test-clover-sdhci.py names it as the leg that says a
  // card ring whose flush keeps up loses nothing, and a number a reader cannot reach is not a reading.
  std::printf ("case wrap written=%u dropped=%u passes=%u first_slot_lba=%u reused_lba=%u laps=%u "
               "not_reused_lba=%u pinned=%u\n",
               driver.written, driver.dropped, passes, lba_0, lba_60,
               driver.cursor / rolling_slots, lba_64, kRingPinnedSlots);
}

static void case_kernel_wrap (void)
{
  FakeCard card (kBootRecordOffset + 1);
  StagingRing kernel (kRingKernel);
  for (uint32_t i = 0; i < 1000; i++) {
    kernel.push ("[  " + std::to_string (i) + ".000000]: kernel row " + std::to_string (i));
    if (i % 8 == 7) {
      kernel.flush (card, 341, 0, true);
    }
  }
  while (kernel.pending) {
    kernel.flush (card, 341, 0, true);
  }
  check ("the kernel ring took every row", kernel.written, 1000u);
  // The kernel ring has no pinned slots: round342's floor is a property of the driver's ring, and a
  // tee copy of a pinned sentence -- which is what every sdhciSay line also becomes -- stays in the
  // ring that wraps on its own 512. -1 here is that rule being applied, not a placeholder.
  check ("the kernel ring's slot after 999 records", sdhci_ring_slot (kRingKernel, 999, -1),
         999u % kRingCardSlots[kRingKernel]);
  const std::string kernel_copy = "clover r339: mark wrote=1 before=2 after=1";
  if (sdhci_pin_index (kernel_copy.c_str (), (uint32_t) kernel_copy.size (), kRingKernel) != -1) {
    fail ("a kernel-ring copy of a pinned row was given a pinned slot");
  }
  // The kernel ring must not reach into the boot record's sector: that is the one place where a wrap
  // that was off by one ring would erase the boot id instead of a row nobody would notice.
  const uint32_t last = sdhci_ring_lba (card.first_lba, kRingKernel, 999, -1);
  std::printf ("case kernel_wrap written=%u last_lba_offset=%u boot_record=%u\n", kernel.written,
               last - card.first_lba, kBootRecordOffset);
  if (last - card.first_lba >= kBootRecordOffset) {
    fail ("a kernel-ring record landed on or past the boot record's sector");
  }
}

static void case_pinned_slots (void)
{
  // Round342's whole claim, run rather than argued: four one-shot rows, then enough periodic rows that
  // round341's arithmetic erased them -- boot 120 wrote 68 records and its card kept seq 4..67. The
  // pins are the four heads the layer names, in the order clover_unattended_begin and the arming block
  // print them.
  FakeCard card (kBootRecordOffset + 1);
  StagingRing driver (kRingDriver);
  const std::string pins[kRingPinnedSlots] = {
    "clover r283: last reset kind=1 pid=2 aux=3 pc=4 cnt=5 beat=6 beatcnt=7 beatmagic=8 text=x",
    "clover r339: beatctl wrote=1 read=1 magic_wrote=2 magic_read=2 ok=1",
    "clover r339: mark wrote=77665500 before=0 after=77665500",
    "CloverSdhci r174: verdict ring armed lba=13320280 slots=64 pinned=4",
  };
  for (uint32_t i = 0; i < kRingPinnedSlots; i++) {
    driver.push (pins[i]);
  }
  for (uint32_t i = 0; i < 200; i++) {
    driver.push ("clover r342 periodic " + std::to_string (i));
    if (i % 4 == 3) {
      driver.flush (card, 342, 0, true);
    }
  }
  while (driver.pending) {
    driver.flush (card, 342, 0, true);
  }
  uint32_t in_pinned = 0;
  for (size_t i = 0; i < driver.slots_written.size (); i++) {
    if (driver.slots_written[i] < kRingPinnedSlots) {
      in_pinned++;
    }
  }
  // kRingPinnedSlots, not zero: the only records allowed in those slots are the pinned rows themselves,
  // one write each this boot.
  check ("records that landed in a pinned slot", in_pinned, (uint64_t) kRingPinnedSlots);
  uint32_t heads_read = 0;
  for (uint32_t i = 0; i < kRingPinnedSlots; i++) {
    const uint32_t header_lba = card.first_lba + kRingOffset[kRingDriver] +
                                kRingSectorsPerRecord * i;
    const uint32_t text_lba = header_lba + 1;
    if (card.word (header_lba, kWordMagic0) != kLogMagic0 ||
        card.word (header_lba, kWordMagic1) != kLogMagic1) {
      fail ("a pinned slot held no record");
      continue;
    }
    if (card.word (header_lba, kWordSlot) != i) {
      fail ("a pinned record's slot word disagrees with the sector it was written to");
    }
    if (card.word (header_lba, kWordBoot) != 342u) {
      fail ("a pinned record was not this boot's");
    }
    uint32_t j = 0;
    for (; j < pins[i].size () && j < kVerdictTextBytes; j++) {
      if (card.byte (text_lba, kLogPrefixLength + j) != pins[i][j]) {
        break;
      }
    }
    if (j == pins[i].size ()) {
      heads_read++;
    } else {
      fail ("a pinned slot held a row other than the one its head names");
    }
  }
  check ("pinned rows read back from the card", heads_read, (uint64_t) kRingPinnedSlots);
  // The other half of what the reader has to learn: seq is still the clock, but it no longer predicts
  // the slot. Pinned record 2 is seq 2 and slot 2 only because the pins were the first three writes;
  // the first periodic record is seq 4 in slot 4, and seq 64 lands in slot 8 rather than slot 0.
  const uint32_t periodic_seq = kRingPinnedSlots;
  check ("the first periodic record's slot",
         sdhci_ring_slot (kRingDriver, periodic_seq, -1),
         kRingPinnedSlots + periodic_seq % (kRingCardSlots[kRingDriver] - kRingPinnedSlots));
  if (sdhci_ring_slot (kRingDriver, 64, -1) == sdhci_ring_slot (kRingDriver, 0, -1)) {
    fail ("cursor 64 landed where cursor 0 landed, so the pinned slots are inside the modulus");
  }
  // A second boot overwrites the same pinned slots -- that is the point of a fixed home -- and word 9
  // is what makes the older sighting a statement about the boot that ended rather than this one.
  StagingRing next (kRingDriver);
  for (uint32_t i = 0; i < kRingPinnedSlots; i++) {
    next.push (pins[i]);
  }
  while (next.pending) {
    next.flush (card, 343, 0, true);
  }
  const uint32_t header_lba0 = card.first_lba + kRingOffset[kRingDriver];
  check ("the pinned slot followed the second boot's id",
         card.word (header_lba0, kWordBoot), 343u);
  std::printf ("case pinned_slots pinned=%u ring=%u periodic=%u read_back=%u seq64_slot=%u "
               "seq0_slot=%u\n",
               kRingPinnedSlots, kRingCardSlots[kRingDriver],
               kRingCardSlots[kRingDriver] - kRingPinnedSlots, heads_read,
               sdhci_ring_slot (kRingDriver, 64, -1), sdhci_ring_slot (kRingDriver, 0, -1));
}

static void case_ram_drop (void)
{
  StagingRing driver (kRingDriver);
  for (uint32_t i = 0; i < 200; i++) {
    driver.push ("row " + std::to_string (i));      // never flushed: the card is busy
  }
  check ("a full staging ring drops the oldest", driver.dropped, 200u - kRingRamSlots);
  check ("the staging ring stays at capacity", driver.pending, kRingRamSlots);
  FakeCard card (kBootRecordOffset + 1);
  while (driver.pending) {
    driver.flush (card, 341, 0, true);
  }
  check ("what survived was the newest 128 rows", driver.written, kRingRamSlots);
  std::printf ("case ram_drop pushed=%u dropped=%u survived=%u\n", 200u, driver.dropped,
               driver.written);
}

static void case_refused (void)
{
  FakeCard card (kBootRecordOffset + 1);
  StagingRing driver (kRingDriver);
  for (uint32_t i = 0; i < 10; i++) {
    driver.push ("row " + std::to_string (i));
  }
  driver.flush (card, 341, 0, false);               // the card is gone
  check ("a refused write costs a retry, not a row", driver.pending, 10u);
  check ("the card cursor did not move", driver.cursor, 0u);
  check ("the refusals reached the card's counter", card.refusals, 1u);
  driver.flush (card, 341, 0, true);
  check ("the next pass writes what the last one could not", driver.written,
         kRingFlushPerPass[kRingDriver]);
  std::printf ("case refused pending=%u written=%u budget=%u\n", driver.pending, driver.written,
               kRingFlushPerPass[kRingDriver]);
}

static void case_relay (void)
{
  Relay drained;
  for (uint32_t i = 0; i < 25; i++) {
    drained.say (8);
    drained.consume (8);                            // the beat keeps up, as it has since round304
  }
  Relay starved;
  starved.say (200);                                // no consumer at all: boot 93's shape
  check ("a drained relay loses nothing", drained.dropped, 0u);
  check ("a starved relay loses everything past its capacity", starved.dropped,
         200u - kProbeSlots);
  // The witness line: what the replaced rule reported in each of the two shapes. It is printed, not
  // asserted to be right, because its whole purpose is to say the old field could not tell these boots
  // apart.
  std::printf ("case relay drained_backlog=%u drained_dropped=%u positional=%u"
               " starved_dropped=%u starved_positional=%u\n",
               drained.seq - drained.next, drained.dropped, drained.positional,
               starved.dropped, starved.positional);
  if (drained.positional == drained.dropped) {
    fail ("the positional rule agrees with the backlog rule on a drained relay, so the case proves nothing");
  }
}

// A row that is not there is an empty row, not a read past the end of the vector. This exists because
// round341's N45 broke the tee's truncation rule and the model segfaulted on the very next check -- so
// the harness saw a crash and no PROBLEM line, and the gate could not say which of its legs judged.
static std::string tee_row_at (const TeeRun &run, size_t index)
{
  return index < run.rows.size () ? run.rows[index] : std::string ();
}

static void case_tee_cut (void)
{
  std::string long_line (400, 'x');
  long_line += "\n";

  TeeRun neu;
  for (char c : long_line) {
    tee_feed_new (neu, (uint8_t) c);
  }
  check ("a truncated line is one row, not two", neu.rows.size (), 1u);
  check ("the row holds the whole bound", tee_row_at (neu, 0).size (), kVerdictTextBytes);
  check ("the truncation is counted once", neu.cut, 1u);
  check ("no byte of the kept prefix is lost", neu.bytes_lost, 0u);
  check ("the kept prefix is the message's own first bytes",
         tee_row_at (neu, 0) == long_line.substr (0, kVerdictTextBytes) ? 1u : 0u, 1u);

  // A second, shorter line after a truncated one starts clean: the state that says "this line is
  // already a record" must not leak into the next.
  std::string next_line = "fl=0x1 tail\n";
  for (char c : next_line) {
    tee_feed_new (neu, (uint8_t) c);
  }
  check ("the next line is its own row", neu.rows.size (), 2u);
  check ("and holds the whole of it",
         tee_row_at (neu, 1) == next_line.substr (0, next_line.size () - 1) ? 1u : 0u, 1u);
  check ("with no further truncation", neu.cut, 1u);

  TeeRun old;
  for (char c : long_line) {
    tee_feed_old (old, (uint8_t) c);
  }
  std::printf ("case tee_cut rows_new=%u cut=%u bytes_lost_new=%u rows_old=%u bytes_lost_old=%u"
               " bound=%u\n", (uint32_t) neu.rows.size (), neu.cut, neu.bytes_lost,
               (uint32_t) old.rows.size (), old.bytes_lost, kVerdictTextBytes);
  if (old.rows.size () < 2) {
    fail ("the witness for the replaced rule did not split the line, so the case is not a comparison");
  }

  TeeRun exact;
  std::string fits (kVerdictTextBytes, 'y');
  fits += "\n";
  for (char c : fits) {
    tee_feed_new (exact, (uint8_t) c);
  }
  check ("a line exactly at the bound is not a cut", exact.cut, 0u);
  check ("and is one row", exact.rows.size (), 1u);
}

static void case_stamp (void)
{
  const char *a = "[  157.046125]: clover r333: bbswap";
  const char *b = "fl=0x14809021 vt=1 fs=apfs";
  const char *c = "[123";
  const char *d = "[]:";
  check ("a stamped line skips its stamp", sdhci_tee_stamp_skip (a, (uint32_t) strlen (a)), 16u);
  check ("an unstamped line skips nothing", sdhci_tee_stamp_skip (b, (uint32_t) strlen (b)), 0u);
  check ("an unterminated bracket is not a stamp", sdhci_tee_stamp_skip (c, 4u), 0u);
  check ("an empty stamp still consumes its colon", sdhci_tee_stamp_skip (d, 3u), 3u);
  std::printf ("case stamp stamped=%u unstamped=%u unterminated=%u empty=%u\n",
               sdhci_tee_stamp_skip (a, (uint32_t) strlen (a)),
               sdhci_tee_stamp_skip (b, (uint32_t) strlen (b)),
               sdhci_tee_stamp_skip (c, 4u), sdhci_tee_stamp_skip (d, 3u));
}

static void case_filter (void)
{
  const uint32_t boot = 341;
  // The printable filter, byte by byte: what a record's text sector holds for a line containing
  // control bytes. The newline is the one control byte kept, and a high byte is never allowed to
  // survive as itself -- the reader's text_of() would otherwise print it as raw bytes.
  const uint8_t raw[] = { 'a', 0x00, 0x07, '\n', 0x7f, 0x80, 'z' };
  uint8_t block[sdhciLogSectorBytes];
  char text[8];
  std::memcpy (text, raw, sizeof (raw));
  text[7] = 0;
  sdhci_build_text (block, text, 7);
  uint32_t dots = 0;
  for (uint32_t i = 0; i < 7; i++) {
    if (block[kLogPrefixLength + i] == '.') {
      dots++;
    }
  }
  check ("the control bytes became dots", dots, 4u);
  check ("the newline stayed a newline",
         block[kLogPrefixLength + 3], (uint64_t) '\n');
  check ("the text block ends with its own newline",
         block[kLogPrefixLength + 7], (uint64_t) '\n');
  uint32_t header[sdhciLogBlockWords];
  for (uint32_t i = 0; i < sdhciLogBlockWords; i++) {
    header[i] = 0xdeadbeefu;
  }
  sdhci_build_header (header, 13320280u, kRingDriver, 3, 7, boot, 0, -1, text, 7);
  // The reader's belief rule, replayed: it recomputes the sum over the bytes the SECTOR holds and
  // compares what it finds with word 4. A writer that sums the bytes before filtering them produces a
  // record that is on the card whole and undecodable, which is why round341 moved the filter into
  // sdhci_record_sum; the archives say it never happened (0 mismatches over 60,986 ring records), so
  // this line is the first time anything has produced the shape and said whether the sum agreed.
  uint32_t stored_sum = 0;
  for (uint32_t i = 0; i < kLogPrefixLength + 7 + 1; i++) {
    stored_sum += block[i];
  }
  const uint32_t sum_agrees = header[kWordSum] == stored_sum ? 1u : 0u;
  check ("the header's byte sum is the sum of the bytes the record stores", sum_agrees, 1u);
  uint32_t unset = 0;
  for (uint32_t i = 0; i < kRecordHeaderWords; i++) {
    if (header[i] == 0xdeadbeefu) {
      unset++;
    }
  }
  check ("a named header word the builder left unset", unset, 0u);
  // The writer's keep-set as one number, so a second file can be asked the same question about its own
  // copy of the rule. scripts/test-clover-sdhci.py computes the identical mask by running
  // read-clover-log.py's text_of over the same 256 inputs: bit b says whether a byte survives as
  // itself. The reader's copy was wider than this (it kept 0x09..0x1f, bytes sdhci_record_byte always
  // replaces) and its docstring named the wrong filler, and nothing on either side could say so --
  // round341's sixth finding, and the reason the filter now reports a set rather than four samples.
  uint8_t mask[32] = {0};
  uint32_t kept = 0;
  for (uint32_t c = 0; c < 256; c++) {
    if (sdhci_record_byte ((uint8_t) c) == (uint8_t) c) {
      mask[c / 8] |= (uint8_t) (1u << (c % 8));
      kept++;
    }
  }
  const char *hex = "0123456789abcdef";
  char keepmask[65];
  for (uint32_t i = 0; i < 32; i++) {
    keepmask[2 * i] = hex[mask[i] >> 4];
    keepmask[2 * i + 1] = hex[mask[i] & 0xFu];
  }
  keepmask[64] = 0;
  check ("the set is the newline plus printable ASCII", kept, 96u);
  std::printf ("case filter dots=%u kept_newline=%u sum_agrees=%u unset=%u header_sum=%u stored_sum=%u\n",
               dots, block[kLogPrefixLength + 3] == '\n' ? 1u : 0u, sum_agrees, unset,
               header[kWordSum], stored_sum);
  std::printf ("case keepset keepmask=%s kept=%u filler=%u\n", keepmask, kept,
               (uint32_t) sdhci_record_byte ((uint8_t) 0x01));
}

// One full window, written by the layer and left on disk for scripts/read-clover-log.py to decode.
static void case_window (const char *path)
{
  FakeCard card (kBootRecordOffset + 1);
  StagingRing driver (kRingDriver);
  StagingRing kernel (kRingKernel);
  const uint32_t boot_id = 342;

  // Round342: the four pinned rows are pushed first, the way a real boot prints them, because the
  // window the reader decodes has to look like a card written by a pinning driver. Without them the
  // ring's first slots are empty, and the reader's new walk -- which names an empty pinned slot
  // instead of ending the ring there -- has nothing to be tested against.
  const std::string host_pins[kRingPinnedSlots] = {
    "clover r283: last reset kind=1 pid=2 aux=3 pc=4 cnt=5 beat=6 beatcnt=7 beatmagic=8 text=x",
    "clover r339: beatctl wrote=1 read=1 magic_wrote=2 magic_read=2 ok=1",
    "clover r339: mark wrote=77665500 before=0 after=77665500",
    "CloverSdhci r174: verdict ring armed lba=13320280 slots=64 pinned=4",
  };
  for (uint32_t i = 0; i < kRingPinnedSlots; i++) {
    driver.push (host_pins[i]);
  }
  // Numbered by the seq each row will be written with, not by push order: the pinned rows take the
  // first sequence numbers, and the reader's leg that says "the content is the row its seq names"
  // checks exactly that identity. Naming them 0.. would have made the leg's first finding a numbering
  // disagreement inside the model.
  for (uint32_t i = 0; i < 70; i++) {
    driver.push ("CloverSdhci r341: host row " +
                 std::to_string (kRingPinnedSlots + i));
  }
  // One row carries control bytes on purpose. The byte sum is the one place where the writer's filter
  // and the writer's own checksum have to agree, and across 60,986 archived ring records not one row
  // ever did carry a control byte -- so the reading "undecodable" and the reading "nothing to disagree
  // about" were the same number, 0, with nothing to tell them apart.
  driver.push (std::string ("CloverSdhci r341: host row 74 tab\tx07\x07high\xff end"));
  // Flushed as it is filled, the way the beat does it, so the ring the reader decodes is deep rather
  // than limited by the 128-slot staging depth.
  for (uint32_t i = 0; i < 600; i++) {
    kernel.push ("[  " + std::to_string (i) + ".500000]: unixnu clover kernel row " +
                 std::to_string (i));
    if (i % 8 == 7) {
      kernel.flush (card, boot_id, 0, true);
    }
  }
  // Two of the kernel rows go past the bound on purpose: the reader's own bound counter has to have
  // something to find, and 0 is otherwise indistinguishable from a boot that printed nothing long.
  kernel.push ("[  999.000000]: " + std::string (kVerdictTextBytes + 60, 'L'));
  kernel.push ("[  999.100000]: " + std::string (kVerdictTextBytes, 'S'));
  while (driver.pending) {
    driver.flush (card, boot_id, 0, true);
  }
  while (kernel.pending) {
    kernel.flush (card, boot_id, 0, true);
  }
  // The boot record, one sector past the kernel ring, in the same header positions the rings use.
  {
    uint32_t header[sdhciLogBlockWords] = {0};
    const uint32_t lba = card.first_lba + kBootRecordOffset;
    header[kWordMagic0] = kLogMagic0;
    header[kWordMagic1] = kLogMagic1;
    header[kWordLba] = lba;
    header[kWordKind] = kBootKind;
    header[kWordBoot] = boot_id;
    card.write (lba, header, true);
  }
  // The panic screen's report record, in the window's own first two sectors. This is the driver's
  // second producer of the same ten words -- different slot rule (a forward scan, not a cursor),
  // different kind -- and until this leg it was the one record layer entry point no host instrument
  // had ever called. Its sentence is deliberately longer than one block and carries control bytes,
  // because both are things the copy it replaced got wrong or unmeasured.
  {
    uint32_t header[sdhciLogBlockWords] = {0};
    uint8_t block[sdhciLogSectorBytes];
    // Longer than one record, with the control bytes at the FRONT so the clamp cannot cut off the very
    // evidence the leg looks for: a tail of three filtered bytes after 480 fillers is a tail the writer
    // never stores.
    const std::string sentence = std::string ("panic:\t\x07\xff") + std::string (500, 'P');
    sdhci_build_panic_header (header, card.first_lba, 0, 7, boot_id,
                              sentence.c_str (), (uint32_t) sentence.size ());
    card.write (card.first_lba, header, true);
    sdhci_build_text (block, sentence.c_str (), (uint32_t) sentence.size ());
    card.write (card.first_lba + 1, block, true);
    // Its own case name: scripts/test-clover-sdhci.py keys the report by name and a second `case
    // window` line would overwrite the first one's fields, including the byte count it checks.
    std::printf ("case panic body=%u total=%u sum=%u kind=%u clamp=%u\n",
                 header[kWordBody], header[kWordTotal], header[kWordSum],
                 header[kWordKind], (uint32_t) kRecordBodyBytes);
  }
  FILE *out = std::fopen (path, "wb");
  if (!out) {
    fail ("the window could not be written for the reader to decode");
    return;
  }
  std::fwrite (card.window.data (), 1, card.window.size (), out);
  std::fclose (out);
  std::printf ("case window bytes=%u writes=%u driver_written=%u kernel_written=%u boot=%u path=%s\n",
               (uint32_t) card.window.size (), card.writes, driver.written, kernel.written,
               boot_id, path);
}

int main (int argc, char **argv)
{
  if (argc < 2) {
    std::printf ("usage: clover_sdhci_host WINDOW.bin\n");
    return 2;
  }
  // Unbuffered, because the report is the reading. Round341's N45 broke the tee rule and the process
  // died with every case line still in the buffer, so the harness that parses them reported a crash
  // and named nothing; a line-by-line report survives whatever the next case does.
  setvbuf (stdout, nullptr, _IONBF, 0);
  case_geometry ();
  case_fits ();
  case_wrap ();
  case_kernel_wrap ();
  case_pinned_slots ();
  case_ram_drop ();
  case_refused ();
  case_relay ();
  case_tee_cut ();
  case_stamp ();
  case_filter ();
  case_window (argv[1]);
  std::printf ("sdhci host model: %s\n", problems ? "FAILED" : "OK");
  std::printf ("sdhci host model problems=%d\n", problems);
  return problems ? 1 : 0;
}
