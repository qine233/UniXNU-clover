// SPDX-License-Identifier: BSD-2-Clause-Patent
// clover (Mi Pad 4, SDM660): the card's record layer -- what CloverSdhci puts into a sector, and
// what it decides before it does, with no register, no IOKit object, no thread and no card in it.
//
// WHY THIS FILE EXISTS (round341)
//
// notes/unixnu-doc-conformance.md §7 gap 3 asked this of three drivers and issue #653 kept the sixth
// one waiting: CloverSdhci is the driver every other reading reaches the host through, and until now
// none of its record arithmetic could be asked a question without a boot. Writing it down, and then
// banning the copies left behind, turned up four things the card had been getting away with.
//
//   1. THE RELAY'S `dropped=` ANSWERED A DIFFERENT QUESTION THAN ITS NAME. The probe relay's producer
//      counted `seq >= kProbeSlots` -- a *position*: every hand-out after the ring's sixty-fourth,
//      whatever a consumer had already taken. The relay got a consume cursor in round304 and two
//      consumers that do not need a panic, so the number became a statement about history rather than
//      about backlog: at boot 105's rate, a boot whose beat drained every row would still report
//      `dropped=` growing by one per row after the sixty-fourth. §Round304 ⑤ registered the reading
//      the other way -- ">0 表示 64 槽在两次 flush 之间被覆盖" -- so the field was being interpreted as
//      the one thing its expression cannot say. The archived readings say the mistake never reached a
//      card: across 575 probe rows in ~/clover-out the largest `seen` is 62 and `dropped` is 0 in
//      every one, so this is a latent defect and no past judgment has to be retracted. The host model
//      is what makes that visible: it is the first instrument that can hand the relay 200 rows and say
//      whether each was consumed, which is a positive control the card has never supplied.
//
//   2. THE TEE'S LONG LINE ATE A BYTE AND NAMED NOTHING. The kernel ring's assembler kept
//      kVerdictTextBytes (320) characters; the byte that arrived when the buffer was full was neither
//      stored nor counted -- the row was handed over, the buffer reset and that one character
//      destroyed, and the rest of the message then assembled as a *second* row, which the allowlist
//      rejects so quietly that only the aggregate `seen=` minus `kept=` remembers it existed. Two
//      comments describe this path, and they disagree: kTeeLineBytes' line says "a longer line is
//      truncated, not split" and the overflow branch says "hand over what fits and start the next one",
//      which is a split. The card has 631 kind-22 records whose body is exactly the bound (measured
//      over 104,819 records in the archived readings), so the truncated half has been read as whole.
//      The layer keeps the documented behaviour -- truncate, discard the remainder up to the newline,
//      and count it -- and the count is printed, because r306 already gave the relay's truncation its
//      own number for exactly this reason: a row that arrived with its tail missing is a different fact
//      from a row that never arrived.
//
//   3. THE BYTE SUM RAN OVER BYTES THE SECTOR DOES NOT HOLD. The writer summed the text as it arrived
//      and stored it through the printable filter, while the reader recomputes over what the sector
//      holds -- so one control byte in a kernel line produced a record that was on the card intact and
//      that no decode would believe. Measured over the archives before the change: 106 usable windows,
//      6,714 driver-ring and 54,272 kernel-ring records, 0 sum mismatches. Latent, so no past reading is
//      retracted -- but `0` was the only answer this board had ever produced for it, so the model now
//      writes a row carrying a tab, 0x07 and 0xff and decodes it back.
//
//   4. THE PANIC SCREEN HAD ITS OWN COPY OF THE WHOLE RECORD. `panicAppendText` spelled the ten words,
//      the 480 clamp and the byte sum out a second time, several thousand lines from the ring path, and
//      carried finding 3 with it. Nothing in the extraction walked there: this one was found by the
//      ban written to catch it (`sum += static_cast<uint8_t> (text[i])` in the driver), which is the
//      gate doing the job round340 gave it. Same measurement, same verdict: 1,686 kind-20 records over
//      the same 106 windows, 0 sum mismatches and 0 storing a filtered byte, so latent -- and now gone,
//      because the path calls sdhci_build_panic_header and sdhci_build_text like everyone else.
//
// WHAT ELSE LIVES HERE, AND WHY
//
// The geometry constants that round340 found typed in three files are here once. That round's
// instrument (scripts/test-clover-sdhci-geometry.py) compares this file against the reader and the
// unattended dd, so the numbers were already measured; moving them makes the comparison say something
// stronger -- the driver has no copy of its own layout to disagree with the reader, and a leg now
// refuses the tree if CloverSdhci.cpp declares any of these names again. The literals are still
// literals where the geometry is a fact about this partition rather than a derivation
// (`kRingOffset = {64, 192}`, `kBootRecordOffset = 192 + 2 * 512`), and round340's tiling leg is what
// proves they are the same arithmetic the reader does.
//
// WHAT MAY LIVE HERE
//
// Word order, byte order, bounds, the clamp, the printable filter, the byte sum, the two header builders
// that put those in the order the reader unpacks, which slot a cursor
// lands in, whether a ring is full, whether a relay slot was still unconsumed, and how a line gets
// assembled. Nothing that reads or writes a register, nothing that needs a kernel address, nothing that
// formats a row: every `CloverSdhci rNNN:` string is still built in CloverSdhci.cpp, byte-identical to
// what it printed before this file existed, so boots 41 through 120 stay comparable.

#ifndef CLOVER_SDHCI_SDHCILOGCORE_H
#define CLOVER_SDHCI_SDHCILOGCORE_H

#include <stdint.h>

// ------------------------------------------------------------------ sector and record shape
// One sector is the unit the card's PIO path moves; the record header is a sector of big-endian-agnostic
// uint32 words and the text is the sector after it. round340's dd leg is what pins this against the
// window the host reads (1217 sectors, 623104 bytes).
static const uint32_t sdhciLogSectorBytes = 512;
static const uint32_t sdhciLogBlockWords = sdhciLogSectorBytes / 4u;   // CloverSdhci.cpp's kBlockWords
static const uint32_t kRecordHeaderWords = 10;      // the reader unpacks `<10I` and nothing else

// The text record's own bound. prefix + body + newline has to fit one sector, which is why 480 and not
// 512 is the clamp: kLogPrefixLength is 23, so the largest record is 504 bytes and never spills.
static const uint32_t kRecordBodyBytes = 480;
static const char kLogPrefix[] = "unixnu clover emmc log\n";
static const uint32_t kLogPrefixLength = sizeof (kLogPrefix) - 1;

static const uint32_t kLogMagic0 = 0x554e5855u;   // 'UNXU'
static const uint32_t kLogMagic1 = 0x31474f4cu;   // '1GOL'
static const uint32_t kLogSlots = 16;             // the report window's own slot count

// The header's word positions, named because the reader unpacks them by position and the writer used to
// fill them by position with only a comment between the two. A word moved without moving its name is a
// record whose slot index is read as its checksum, and nothing on the card would say so.
enum SdhciRecordWord {
  kWordMagic0 = 0,
  kWordMagic1 = 1,
  kWordLba = 2,
  kWordTotal = 3,
  kWordSum = 4,
  kWordKind = 5,      // which ring or writer produced the record: 21 driver, 22 kernel tee, 25 relay
  kWordBody = 6,
  kWordSlot = 7,      // cursor modulo the card ring's slots
  kWordSeq = 8,       // what makes a wrapped ring orderable
  kWordBoot = 9       // round212: the boot that wrote it, so a stale slot is not read as this boot's
};

// ------------------------------------------------------------------ the two card rings
static const uint32_t kRingDriver = 0;   // what CloverSdhci measured (round174's ring)
static const uint32_t kRingKernel = 1;   // what the kernel said (round175's tee, round176's ring)
static const uint32_t kRingCount = 2;
static const uint32_t kVerdictTextBytes = 320;   // the writer's line bound; the tee truncates here
static const uint32_t kRingRamSlots = 128;   // staging depth: deeper than the driver's card ring, shallower than the kernel's
static const uint32_t kRingCardSlots[kRingCount] = {64, 512};
static const uint32_t kRingOffset[kRingCount] = {64, 192};   // sectors past the window's first LBA
static const uint32_t kRingSectorsPerRecord = 2;   // header sector then text sector
// Records flushed per guarded pass, per ring. Two block writes each, and a flush must not hold the
// guard long: eight is 16 writes, which at the round159 rates is a few milliseconds.
static const uint32_t kRingFlushPerPass[kRingCount] = {4, 8};

static const uint32_t kBootRecordOffset = 192 + 2 * 512;   // one record past the kernel ring
static const uint32_t kBootKind = 24;
static const uint32_t kProbeKind = 25;
// The panic screen's own report window: kLogSlots two-block records at the head of the window, kind 20,
// its slot chosen by a forward scan rather than by a ring cursor, and its seq the newest seq plus one.
static const uint32_t kPanicKind = 20;

// ------------------------------------------------------------------ the flight region
static const uint32_t kFlightEntryOffset = 32;   // sectors past the window's start; 12 one-block records
static const uint32_t kFlightEntrySlots = 12;
static const uint32_t kFlightTextOffset = 44;    // twelve one-block records, one per boot
static const uint32_t kFlightTextSlots = 12;
static const uint32_t kFlightBeatOffset = 56;    // four one-block records, 4 s apart
static const uint32_t kFlightBeatSlots = 4;
static const uint32_t kFlightDeadlineOffset = 60; // one record, one write, once per boot
static const uint32_t kFlightDeadlineSlots = 1;
static const uint32_t kFlightPanicOffset = 61;    // enter and exit, one sector each
static const uint32_t kFlightPanicSlots = 2;
static const uint32_t kFlightDrainOffset = 63;    // one record, written before the drain's own gates
static const uint32_t kFlightDrainSlots = 1;
static const uint32_t kFlightKind = 23;
static const uint32_t kFlightMarkerMagic = 0x434C564Du;   // 'CLVM', the flight verdict magic

// ------------------------------------------------------------------ the relay (kind 25)
// 64 slots of 256 bytes, sealed by a word written after the text, consumed by one cursor. The bounds
// are here because the predicate that decides whether a hand-out overwrote something a consumer had
// not read needs both, and round341's first finding is what happens when it is written without them.
static const uint32_t kProbeSlots = 64;
static const uint32_t kProbeTextBytes = 256;

// ------------------------------------------------------------------ the record's arithmetic
static inline uint32_t sdhci_record_body (uint32_t length)
{
  return length > kRecordBodyBytes ? kRecordBodyBytes : length;
}

static inline uint32_t sdhci_record_total (uint32_t body)
{
  return kLogPrefixLength + body + 1u;
}

// The invariant the two-sector stride and the reader's `body` field both rest on: a record's text never
// needs more than the one sector after its header. Asserted rather than assumed because 480 and 512 are
// typed in different files for different purposes.
static inline uint32_t sdhci_record_fits_one_sector (uint32_t body)
{
  return sdhci_record_total (body) <= sdhciLogSectorBytes ? 1u : 0u;
}

// Everything the kernel prints has to survive this filter, so a control byte other than newline becomes
// a dot rather than a second line the reader would have to glue back (round335's `glued=` counts the
// ones that arrive at the start of a line without a stamp of their own).
static inline uint8_t sdhci_record_byte (uint8_t c)
{
  if (c == '\n') {
    return c;
  }
  return (c >= 0x20 && c < 0x7f) ? c : (uint8_t) '.';
}

// Byte sum over prefix + body + newline, the same three terms the reader recomputes before it believes
// a record. Kept as one function because a writer that adds a fourth term silently fails every decode.
//
// It sums the bytes as they will be STORED, not as they arrive, because the reader recomputes over what
// the sector holds: the arithmetic before this round summed the raw text while sdhci_build_text wrote the
// filtered text, so a single control byte in a kernel line made a record that was on the card intact and
// that no reader could believe. Measured over the archives before this line changed: 106 usable windows,
// 6,714 driver-ring and 54,272 kernel-ring records decoded, 0 sum mismatches -- a control byte has never
// reached a record, so this is a latent defect and no past reading is retracted. The window the host
// model writes and the reader decodes now carries one on purpose, because 0 was the only answer that
// case had never produced.
//
// The same divergence had a second producer: the panic screen built its own kind-20 header and summed
// the unfiltered text. Measured the same way -- 1,686 kind-20 report records over the same 106 windows,
// 0 sum mismatches and 0 of them storing a filtered byte -- so it too was latent, and it is gone because
// that path now calls this function rather than spelling the record out again.
static inline uint32_t sdhci_record_sum (const char *text, uint32_t body)
{
  uint32_t sum = 0;
  for (uint32_t i = 0; i < kLogPrefixLength; i++) {
    sum += (uint32_t) (uint8_t) kLogPrefix[i];
  }
  for (uint32_t i = 0; i < body; i++) {
    sum += (uint32_t) sdhci_record_byte ((uint8_t) text[i]);
  }
  sum += (uint32_t) (uint8_t) '\n';
  return sum;
}

// Word 5 when the caller named no kind: the two ring defaults, in one place, because a record whose kind
// word is wrong is filed in the wrong ring by a reader that has no other evidence about where it came
// from. round340's third voice reads these two literals back out of the writer.
static inline uint32_t sdhci_record_kind_word (uint32_t kind, uint32_t which)
{
  return kind ? kind : ((which == kRingKernel) ? 22u : 21u);
}

// Where a ring's cursor lands, given the window's first LBA. Slot index alone stops being time once a
// ring has more records than slots, which is why word 8 exists and why this is one expression and not
// two (the writer's lba and the reader's window walk have to disagree loudly, not quietly).
static inline uint32_t sdhci_ring_slot (uint32_t which, uint32_t cursor)
{
  return cursor % kRingCardSlots[which];
}

static inline uint32_t sdhci_ring_lba (uint32_t first_lba, uint32_t which, uint32_t cursor)
{
  return first_lba + kRingOffset[which] +
         kRingSectorsPerRecord * sdhci_ring_slot (which, cursor);
}

// The ten words, in the order the reader unpacks them, for a caller that already knows its kind and its
// slot. Two builders sit on top of this one because the card has two ways to choose a slot -- a ring's
// cursor modulo its depth, and the panic screen's forward scan of its own 16 -- and round341 found the
// second of those spelling the whole record out again in the driver, clamp literal and byte sum
// included. Word order lives here once.
static inline void sdhci_build_words (uint32_t *header, uint32_t lba, uint32_t kind, uint32_t slot,
                                      uint32_t seq, uint32_t boot_id,
                                      const char *text, uint32_t length)
{
  const uint32_t body = sdhci_record_body (length);
  header[kWordMagic0] = kLogMagic0;
  header[kWordMagic1] = kLogMagic1;
  header[kWordLba] = lba;
  header[kWordTotal] = sdhci_record_total (body);
  header[kWordSum] = sdhci_record_sum (text, body);
  header[kWordKind] = kind;
  header[kWordBody] = body;
  header[kWordSlot] = slot;
  header[kWordSeq] = seq;
  header[kWordBoot] = boot_id;
}

// Fill the header sector's ten words and the text sector, in the order the reader unpacks them. The
// caller zeroes neither buffer: this writes all kRecordHeaderWords words and exactly one sector of text,
// so a stale word cannot survive in a position this record did not assign.
static inline void sdhci_build_header (uint32_t *header, uint32_t lba, uint32_t which,
                                      uint32_t cursor, uint32_t seq, uint32_t boot_id,
                                      uint32_t kind, const char *text, uint32_t length)
{
  sdhci_build_words (header, lba, sdhci_record_kind_word (kind, which),
                     sdhci_ring_slot (which, cursor), seq, boot_id, text, length);
}

// The panic screen's report record. Its slot comes from the scan that chose it, not from a cursor, and
// its kind is its own name -- which is also why it is a separate entry point: a caller that reaches for
// the ring builder with a made-up `which` gets a slot word that disagrees with the sector the record was
// written to, and the reader calls that a corrupt record rather than a panic sentence.
static inline void sdhci_build_panic_header (uint32_t *header, uint32_t lba, uint32_t slot,
                                             uint32_t seq, uint32_t boot_id,
                                             const char *text, uint32_t length)
{
  sdhci_build_words (header, lba, kPanicKind, slot, seq, boot_id, text, length);
}

static inline void sdhci_build_text (uint8_t *block, const char *text, uint32_t length)
{
  const uint32_t body = sdhci_record_body (length);
  for (uint32_t i = 0; i < sdhciLogSectorBytes; i++) {
    block[i] = 0;
  }
  for (uint32_t i = 0; i < kLogPrefixLength; i++) {
    block[i] = (uint8_t) kLogPrefix[i];
  }
  for (uint32_t i = 0; i < body; i++) {
    block[kLogPrefixLength + i] = sdhci_record_byte ((uint8_t) text[i]);
  }
  block[kLogPrefixLength + body] = '\n';
}

// ------------------------------------------------------------------ the RAM staging rings
// A line goes to RAM first, and a flush moves whole records to the card. The two decisions that belong
// to the RAM ring -- which slot a cursor fills, and what "full" means -- are here because a ring that
// silently drops the newest line instead of the oldest one loses the reading that explains the boot.
static inline uint32_t sdhci_ram_slot (uint32_t write_cursor)
{
  return write_cursor % kRingRamSlots;
}

// Full means "the slot about to be written is still waiting for a flush", which pending answers exactly:
// with pending at capacity the oldest unread row is the one this cursor is about to reuse.
static inline uint32_t sdhci_ram_full (uint32_t pending)
{
  return pending >= kRingRamSlots ? 1u : 0u;
}

// ------------------------------------------------------------------ the relay's backlog
// The predicate round341 exists for. `seq` is the number just handed out and `consumed` is the consumer's
// cursor, both uint32 counters that only ever advance, so the subtraction is the backlog and the
// comparison asks whether the slot this hand-out is about to write has been read yet. Counting the
// position instead (seq >= kProbeSlots, what was here) says "the relay lost rows" on a boot whose beat
// drained every row on time, which is the opposite of the fact the field is named after.
static inline uint32_t sdhci_relay_overwrote_unconsumed (uint32_t seq, uint32_t consumed)
{
  return (uint32_t) (seq - consumed) >= kProbeSlots ? 1u : 0u;
}

// ------------------------------------------------------------------ the kernel line assembler
// The tee walks msgbuf one byte at a time and assembles lines no longer than one record. The decision is
// three-way and belongs in one pure function: store this byte, finish the line, or throw this byte away
// because this line is already as long as a record can be. Returning kSdhciTeeEmitCut is what lets the
// caller count a truncation instead of letting the tail vanish into `seen=` minus `kept=`, and
// kSdhciTeeOverflow is what lets it stop storing without losing the newline that ends the line.
enum SdhciTeeFeed {
  kSdhciTeeStore = 0,    // write the byte at `partial` and advance
  kSdhciTeeEmit = 1,     // a newline ends a line that fits a record
  kSdhciTeeEmitCut = 2,  // a newline ends a line that was truncated: emit it and count the cut
  kSdhciTeeOverflow = 3, // the bound is reached mid-line: mark it truncated, drop this byte and the rest
  kSdhciTeeDrop = 4      // a byte past the bound, or a newline with nothing assembled
};

static inline int sdhci_tee_feed (uint8_t c, uint32_t partial, uint32_t truncated)
{
  if (c == '\n') {
    if (!partial && !truncated) {
      return kSdhciTeeDrop;      // an empty line is not a record
    }
    return truncated ? kSdhciTeeEmitCut : kSdhciTeeEmit;
  }
  if (truncated) {
    return kSdhciTeeDrop;        // the remainder of a line that is already a whole record
  }
  return partial < kVerdictTextBytes ? kSdhciTeeStore : kSdhciTeeOverflow;
}

// msgbuf stamps each line it prints with `[ <monotonic>]: `; every filter that asks what a line *is* has
// to ask after the stamp, or it compares one message against another message's digits. Round335's reader
// side counts the lines that arrive without one (`glued=`), and this is the writer's answer to the same
// prefix.
static inline uint32_t sdhci_tee_stamp_skip (const char *line, uint32_t length)
{
  if (!length || line[0] != '[') {
    return 0;
  }
  uint32_t i = 0;
  while (i < length && line[i] != ']') {
    i++;
  }
  if (i >= length) {
    return 0;      // no closing bracket: this is not a stamp, so the whole line is the message
  }
  i++;
  while (i < length && (line[i] == ':' || line[i] == ' ')) {
    i++;
  }
  return i;
}

#endif /* CLOVER_SDHCI_SDHCILOGCORE_H */
