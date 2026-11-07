#!/usr/bin/env python3
"""Round341 (#653). Build and run the host model of drivers/CloverSdhci/SdhciLogCore.h, then read what
it wrote with the reader.

Same shape as scripts/test-clover-input.py (round318), scripts/test-clover-gpu.py (round317) and
scripts/test-clover-block.py (round316): compile the layer on a Mac, drive it through the rates the
board has never produced, assert the answers, and let a wrong one cost a rebuild instead of a boot.
check-clover.py's `# Round341.` block runs this file and refuses the tree if it does not pass.

WHAT THIS ROUND IS DEFENDING AGAINST
====================================

Two of CloverSdhci's counters answered a different question than the one they are named after, and both
were invisible because the board's own rate never produced the case where they disagree:

* `dropped=` on the relay (kinds 25) counted `seq >= kProbeSlots` -- a position, not a backlog. Since
  round304 the beat and the block tails drain the relay, so a boot that loses nothing reports a loss for
  every hand-out after its sixty-fourth. Across the archived readings the largest `seen` is 62 and
  `dropped` is 0 in every one of 575 probe rows, so nothing was ever mis-read -- but the reading rule
  §Round304 ⑤ registered ("`dropped=` >0 means 64 slots were overwritten between flushes") was
  unreadable in principle. case relay measures both rules on the same 200 hand-outs: 0 with a consumer
  that keeps up, 136 with none, and the replaced rule reports 136 either way.

* The tee's assembler, on a kernel line longer than one record, handed over the first 320 bytes,
  destroyed the byte that arrived at the bound, and then assembled the remainder as a second row that
  the allowlist rejected -- so a truncated row counted in `kept=` as if it were the message, and the
  tail left only in `seen=` minus `kept=`, which cannot say which rows it touched. Two comments
  describe this path and disagree with each other ("truncated, not split" beside "start the next one").
  case tee_cut runs the replaced rule as a witness beside the new one: 2 rows and 1 byte destroyed
  versus 1 row, no byte lost, and a counter. Boot 120's card already carries 631 kind-22 records whose
  body is exactly the bound, so this is not a hypothetical shape.

A third thing only became a checkable claim: the clamp at 480 and the sector's 512 are typed for
different purposes, and case fits sweeps them. It measures the fit bound (488) and the slack the clamp
leaves (8 bytes) instead of asserting that the two numbers are the same number, which they are not.

The fourth is a set, not a number: read-clover-log.py's text_of kept every byte from 0x09 up, and
sdhci_record_byte replaces all but the newline in that run with '.' before a record is built, and its docstring said the writer stores
a space. case keepset now answers, for all 256 inputs, which bytes the writer keeps and what it stores
for the rest, and the leg below computes the identical mask from the reader's own function, so the two
rules are two readings of one question rather than two sentences. 22 of the 256 inputs disagreed before
the fix (the reader kept 118, the writer keeps 96).

WHAT IS NOT PROVED HERE
========================

That the card holds records at these sectors -- only a read-back can say that (#722, boot 120's card
still needs the board back in TWRP). What this file does prove, by importing scripts/read-clover-log.py
and decoding the model's own window with the reader's constants, is that the writer's arithmetic and the
reader's decode agree about every byte the writer produced -- and, with the two-sector control, that the
agreement is about these bytes rather than about anything that happens to parse.
"""
import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'drivers/CloverSdhci/tests/clover_sdhci_host.cpp'
HEADER = ROOT / 'drivers/CloverSdhci/SdhciLogCore.h'
READER = ROOT / 'scripts/read-clover-log.py'
OUT = ROOT / 'build/clover-sdhci-host'
BIN = OUT / 'clover_sdhci_host'
WINDOW = OUT / 'r341-window.bin'

problems = []


def fail (what):
    problems.append (what)


def compile_harness () -> int:
    OUT.mkdir (parents=True, exist_ok=True)
    compiler = shutil.which ('clang++') or shutil.which ('c++') or 'xcrun'
    command = ([compiler] if compiler != 'xcrun' else ['xcrun', 'clang++']) + [
        '-std=gnu++17', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-o', str (BIN), str (SOURCE)]
    built = subprocess.run (command, capture_output=True, text=True)
    if built.returncode:
        fail ('the sdhci host model did not compile (so none of its cases ran):\n' +
              (built.stdout + built.stderr)[-1200:])
    return built.returncode


def expect (cases, case, key, want, why):
    """One number out of the model's own report, required by name.

    A missing key is a failure rather than a zero: this file's claim is that a case measured something,
    and a default would let a renamed field report agreement about nothing.
    """
    got = cases.get (case, {}).get (key)
    if got is None:
        fail (f'case {case} printed no {key}: {why}')
        return None
    if got != want:
        fail (f'case {case} {key}={got}, expected {want}: {why}')
    return got


def main () -> int:
    if not HEADER.is_file ():
        fail (f'{HEADER} is missing: the model has no layer to compile')
        return 1
    if compile_harness ():
        return 1
    run = subprocess.run ([str (BIN), str (WINDOW)], capture_output=True, text=True)
    if run.returncode:
        # By name, one line each. Round341 indented these behind a '  | ' prefix and tailed the whole
        # report to 1500 characters so they would not crowd the gate's window; round342's N60 then went
        # red with the model's own sentence -- "a pinned slot held no record" -- nowhere in the gate's
        # output, because the gate names a leg by the PROBLEM lines this script prints and an indented
        # line is not one. The crowding was the gate's six-line cap, so that cap is gone and the names
        # come through. The tail survives only for a model that died before naming anything.
        named = [line[len ('PROBLEM '):].strip () for line in run.stdout.splitlines ()
                 if line.startswith ('PROBLEM ')]
        for line in named:
            fail (f'the sdhci host model named: {line}')
        if not named:
            fail ('the sdhci host model reported failures and named none of them:\n' +
                  '\n'.join ('  | ' + line for line in run.stdout.splitlines ())[-1500:])
    if 'sdhci host model: OK' not in run.stdout:
        fail ('the sdhci host model printed no "sdhci host model: OK" line: a crash would look the same '
              'as a pass to anyone reading only its exit code')

    cases = {}
    for line in run.stdout.splitlines ():
        found = re.match (r'case (\w+) (.*)', line)
        if found:
            cases[found.group (1)] = dict ((k, int (v)) for k, v in
                                           re.findall (r'(\w+)=(\d+)', found.group (2)))
    if len (cases) < 9:
        fail (f'the model printed {len (cases)} cases, expected at least 9: the report is the only '
              'thing this script judges, so a truncated one is a silence, not a pass')

    # ---- geometry: the relations inside the layer, not this script's arithmetic
    expect (cases, 'geometry', 'boot_record', 1216, 'the boot record sits at sector 1216')
    expect (cases, 'geometry', 'kernel_end', 1216, 'the kernel ring ends exactly there')
    expect (cases, 'geometry', 'window_sectors', 1217, 'one sector past the boot record is the window')
    expect (cases, 'geometry', 'window_bytes', 623104, 'and the byte count the host dd asserts')

    # ---- the clamp and the sector: measured relation, 8 bytes of slack
    expect (cases, 'fits', 'clamp', 480, 'the record body clamp')
    expect (cases, 'fits', 'fit_bound', 488, 'the largest body that still fits one text sector')
    expect (cases, 'fits', 'slack', 8, 'what the clamp leaves over: the two bounds are not one number')
    expect (cases, 'fits', 'widest_total', 504, 'prefix + clamp + newline, the widest record there is')
    expect (cases, 'fits', 'sector', 512, 'the sector it has to fit in')
    expect (cases, 'fits', 'row_bound', 320, 'the writer\'s line bound, under both')

    # ---- a wrapped card ring: reuse is by slot, order is by seq
    # Round342 makes these three numbers derived rather than typed: the ring's base sector, the pinned
    # count and the window's first LBA are read out of the two files that declare them, because a needle
    # that copies the geometry is a fourth copy of it and goes green when the copy agrees with itself.
    expect (cases, 'wrap', 'written', 200, 'every accepted row reached the card')
    expect (cases, 'wrap', 'dropped', 0, 'nothing was dropped while the flush kept up')
    expect (cases, 'wrap', 'laps', 3, '200 rolling records into the slots the pins leave is three laps')
    reader_source = READER.read_text ()
    ring_pins = re.search (r'\bkRingPinnedSlots\s*=\s*(\d+)', HEADER.read_text ())
    ring_base = re.search (r'\bkRingOffset\[kRingCount\]\s*=\s*\{(\d+)', HEADER.read_text ())
    window_lba = re.search (r'^LOG_WINDOW_FIRST_LBA = (\d+)', reader_source, re.M)
    if not (ring_pins and ring_base and window_lba):
        fail (f'the wrap leg could not read its own geometry (pinned={bool (ring_pins)}, '
              f'ring offset={bool (ring_base)}, window lba={bool (window_lba)}): the sectors it checks '
              'would be this script\'s numbers, not the writer\'s and not the reader\'s')
    else:
        pins = int (ring_pins.group (1))
        want_first = int (window_lba.group (1)) + int (ring_base.group (1)) + 2 * pins
        expect (cases, 'wrap', 'pinned', pins, 'the model pinned the count the layer declares')
        expect (cases, 'wrap', 'first_slot_lba', want_first,
                'the first rolling record lands past the pinned slots')
        expect (cases, 'wrap', 'reused_lba', want_first,
                'and one rolling lap later, the same sector is written again')
        expect (cases, 'wrap', 'not_reused_lba', want_first + 2 * (pins + 64 % (64 - pins) - pins),
                'record 64 does not land on record 0\'s sector: round341 read it as a reuse, and that '
                'reading is what erased boot 120\'s pinned rows')
    expect (cases, 'kernel_wrap', 'written', 1000, 'the kernel ring took every row it was given')
    expect (cases, 'kernel_wrap', 'last_lba_offset', 1166,
            '999 records into 512 slots ends at sector 192 + 2*54, short of the boot record at 1216')

    # ---- the staging ring's two rates
    expect (cases, 'ram_drop', 'dropped', 72, '200 pushes into 128 slots with no flush loses the oldest')
    expect (cases, 'ram_drop', 'survived', 128, 'and only the newest 128 can still reach the card')
    expect (cases, 'refused', 'written', 4, 'one pass flushes kRingFlushPerPass[0] records and no more')
    expect (cases, 'refused', 'pending', 6, 'a refused card costs a retry, never a row')

    # ---- the finding: backlog versus position
    expect (cases, 'relay', 'drained_dropped', 0, 'a relay whose consumer keeps up loses nothing')
    expect (cases, 'relay', 'positional', 136, 'and the replaced rule still reported 200-64 losses')
    expect (cases, 'relay', 'starved_dropped', 136, 'with no consumer the backlog rule agrees at last')
    if cases.get ('relay', {}).get ('drained_backlog') != 0:
        fail ('the drained relay did not have an empty backlog, so the case does not describe a consumer '
              'that kept up')

    # ---- the second finding: truncate and count, do not split and destroy
    expect (cases, 'tee_cut', 'cut', 1, 'one truncated line is one counted truncation')
    expect (cases, 'tee_cut', 'bytes_lost_new', 0, 'no byte of a kept row is destroyed')
    expect (cases, 'tee_cut', 'rows_old', 2, 'the witness rule split the message in two')
    expect (cases, 'tee_cut', 'bytes_lost_old', 1, 'and destroyed the byte at the bound')
    expect (cases, 'tee_cut', 'bound', 320, 'the bound both rules cut at')
    if cases.get ('tee_cut', {}).get ('rows_new') != 2:
        fail ('the tee did not carry on with the following line as its own row, so the truncated state '
              'leaked into the next message')

    expect (cases, 'stamp', 'stamped', 16, 'a msgbuf stamp is 16 bytes for the board\'s clock widths')
    expect (cases, 'stamp', 'unstamped', 0, 'a line that starts mid-message skips nothing')
    expect (cases, 'stamp', 'unterminated', 0, 'an unterminated bracket is not a stamp')

    # ---- the byte filter
    expect (cases, 'filter', 'dots', 4, 'the four control bytes became dots')
    expect (cases, 'filter', 'kept_newline', 1, 'and the newline stayed a newline')
    expect (cases, 'filter', 'sum_agrees', 1, 'the header\'s byte sum equals the sum of the bytes the '
            'record stores: the reader recomputes over the sector, so a writer that sums before filtering '
            'makes a whole record unbeliefable')
    expect (cases, 'filter', 'unset', 0, 'no named header word was left for the previous record')

    # ---- the panic screen's record: the driver's second producer of the same ten words, and the only
    # one whose slot comes from a forward scan instead of a ring cursor. Round341's fourth finding was
    # this copy -- spelled out again several thousand lines from the ring path, clamp literal and byte
    # sum included, and found by the ban written to catch it rather than by reading the region the
    # extraction touched. These are the numbers that copy had no instrument for.
    expect (cases, 'panic', 'kind', 20, 'the report record names itself; the reader files a record by '
            'word 5 and has no other evidence of who wrote it')
    expect (cases, 'panic', 'clamp', 480, 'the layer\'s own body bound, read back out of the header the '
            'panic path built')

    # Every leg below indexes the model's report. Round341's N45 showed why that needs saying first: the
    # model died on an out-of-range row, the buffer took the case lines with it, and this script raised
    # KeyError -- a red gate that named nothing, which is the silence this round's own findings are
    # about. Missing case is a reading, so it is reported as one.
    for required in ('window', 'panic'):
        if required not in cases:
            fail (f'the model printed no case {required}: the report ends where the process died, and '
                  'the legs below would raise KeyError instead of naming that')
            return 1

    # ---- the reader as a second producer over the model's own window
    win = WINDOW.read_bytes ()
    expect (cases, 'window', 'bytes', len (win), 'the window the model wrote is the one dd reads')
    spec = importlib.util.spec_from_file_location ('clover_reader', READER)
    reader = importlib.util.module_from_spec (spec)
    spec.loader.exec_module (reader)

    # ---- the sixth finding: the reader's byte rule against the writer's, as two sets
    # case keepset is sdhci_record_byte's own answer for all 256 inputs; the mask below is
    # read-clover-log.py's text_of asked the same question. Round341 found the reader keeping every
    # byte from 0x09 up except the newline it already kept -- bytes the writer replaces with '.' before
    # the record is ever built -- while its docstring said the filler was a space. Only one of the two
    # sides can be wrong here, and neither of them is this script's arithmetic.
    keepset = re.search (r'case keepset keepmask=([0-9a-f]{64}) kept=(\d+) filler=(\d+)', run.stdout)
    if keepset is None:
        fail ('the model printed no case keepset: the byte rule would be back to four samples in '
              'case filter, which cannot say which 256 inputs the reader agrees about')
    else:
        reader_mask = 0
        reader_kept = 0
        for byte in range (256):
            if reader.text_of (bytes ([byte])) == chr (byte):
                reader_mask |= 1 << byte
                reader_kept += 1
        reader_hex = ''.join (f'{(reader_mask >> (8 * index)) & 0xFF:02x}' for index in range (32))
        if reader_hex != keepset.group (1):
            # The count that names the difference is the xor's popcount, not either side's own size:
            # 118 reader-kept bytes and 96 writer-kept bytes agree on 234 of the 256 inputs and differ
            # on 22, and a sentence that printed one of the two sizes as if it were the agreement would
            # be the same mistake this round found in text_of. The writer's mask also has to be read
            # with the same convention it was written: keepmask prints mask[0], the byte holding inputs
            # 0..7, first, so int(..., 16) -- which puts that pair at the top of the integer -- bit-
            # reflects it, and the first version of this line said 191 bytes differed when one did.
            writer_mask = int.from_bytes (bytes.fromhex (keepset.group (1)), 'little')
            differing = bin (reader_mask ^ writer_mask).count ('1')
            fail (f'the reader and the writer disagree about {differing} of 256 bytes: text_of keeps '
                  f'{reader_kept} where sdhci_record_byte keeps {keepset.group (2)}, so one side '
                  'presents a byte as original that the other replaces before the record exists')
        if reader_kept != int (keepset.group (2)):
            fail (f'the reader keeps {reader_kept} bytes where the writer keeps '
                  f'{keepset.group (2)}: the counts are the same set\'s size read two ways, so they '
                  'cannot both be right')
        if len (reader.text_of (bytes ([0x20]))) != 1 or reader.text_of (bytes ([1])) != \
                chr (int (keepset.group (3))):
            fail ('text_of does not store the byte the layer stores for a control byte: the reader '
                  'would print what the card does not hold')
    boot_id = cases.get ('window', {}).get ('boot', 0)
    first_lba = reader.LOG_WINDOW_FIRST_LBA
    driver = reader.decode (win, first_lba, slots=reader.VERDICT_SLOTS, base_sector=reader.VERDICT_OFFSET,
                            expect_kind=(21, 25), pinned=reader.PINNED_SLOTS)
    kernel = reader.decode (win, first_lba, slots=reader.KERNEL_SLOTS, base_sector=reader.KERNEL_OFFSET,
                            expect_kind=22)
    if len (driver) != reader.VERDICT_SLOTS:
        fail (f'the reader found {len (driver)} driver-ring records, expected the ring\'s '
              f'{reader.VERDICT_SLOTS}: the model wrote {cases.get("window", {}).get("driver_written")}')
    if len (kernel) != reader.KERNEL_SLOTS:
        fail (f'the reader found {len (kernel)} kernel-ring records, expected the ring\'s '
              f'{reader.KERNEL_SLOTS}')
    for name, records in (('driver', driver), ('kernel', kernel)):
        dirty = [record for record in records if record[3]]
        if dirty:
            fail (f'the reader found problems in {name} records it decoded: {dirty[0][3]}')
        stale = [record for record in records if record[1]['boot'] != boot_id]
        if stale:
            fail (f'{len (stale)} {name} records carry boot {stale[0][1]["boot"]}, not the model\'s '
                  f'{boot_id}: a reader cannot tell a wrapped slot from an earlier boot without that word')
    # Wrap order. A card ring is addressed by slot and aged by seq, so the only reading that says which
    # record a slot last lost is the pair (slot, seq): every record must sit at seq modulo its slot count,
    # and the seq a slot holds must be the newest of that congruence class below the count the model
    # printed. Slot 0 is where the second lap lands first, and 70 records into 64 slots puts seq 64 there
    # -- not 6, which is what subtracting the slot count from the record count gives and what an earlier
    # version of this leg asserted.
    def newest_seq (slot, total, stride):
        return (total - 1) - ((total - 1 - slot) % stride)

    # The bound, read out of the layer header the writer compiles against. The reader has no name for it
    # -- it only ever sees a body length -- so the number has to come from the producer's own source, and
    # typing 320 here would make the leg compare this script's memory against itself.
    bound = re.search (r'\bkVerdictTextBytes\s*=\s*(\d+)', HEADER.read_text ())
    if bound is None:
        fail (f'{HEADER} declares no kVerdictTextBytes = <number>: the at-bound leg has no producer '
              'to agree with')
        return 1
    line_bytes = int (bound.group (1))

    counts = {'driver': cases['window']['driver_written'],
              'kernel': cases['window']['kernel_written']}
    strides = {'driver': reader.VERDICT_SLOTS, 'kernel': reader.KERNEL_SLOTS}
    # Round342 splits this leg by ring. The kernel ring still turns in all of its 512 slots, so seq
    # modulo slots predicts where a record sits; the driver ring's first PINNED_SLOTS slots are claimed
    # by named rows, and every other record turns in what is left. Keeping one formula for both would
    # have made the pinned rows the anomaly -- which is exactly how boot 120's card came to be read as
    # "four records missing and nothing to say why".
    pins = {'driver': reader.PINNED_SLOTS, 'kernel': 0}
    markers = {'driver': 'host row ', 'kernel': 'kernel row '}

    def expected_slot (seq, content, stride, pinned):
        if pinned:
            owner = reader.pinned_slot (content)
            return owner if owner is not None else pinned + seq % (stride - pinned)
        return seq % stride

    for name, records in (('driver', driver), ('kernel', kernel)):
        total, stride = counts[name], strides[name]
        pinned = pins[name]
        if not records:
            continue                     # the two count legs above already named this
        misplaced = [(slot, record['seq']) for slot, record, content, _ in records
                     if expected_slot (record['seq'], content, stride, pinned) != slot]
        if misplaced:
            fail (f'{len (misplaced)} {name} records sit where their seq does not put them, first at '
                  f'slot {misplaced[0][0]} seq {misplaced[0][1]}: the ring is addressed by its own slot '
                  'rule ({pinned} pinned of {stride}), and this is the leg that says the writer agrees')
        if pinned == 0 and records[0][1]['seq'] != newest_seq (0, total, stride):
            fail (f'slot 0 of the {name} ring holds seq {records[0][1]["seq"]}, expected the newest seq '
                  f'congruent to 0 below {total}, {newest_seq (0, total, stride)}: the sequence word is '
                  'the only thing that orders a wrapped ring')
        # Content follows seq: a row that reached the wrong slot, or a writer that stamped one row and
        # stored another, breaks this before any of the geometry legs has to be re-read. The two rows the
        # model truncated on purpose carry filler instead of a marker, so they are the at-bound leg's
        # business, not this one's. Pinned slots are a third exemption and it is the point of round342:
        # a pinned row is named by its own head, never by the marker, and it keeps its slot whatever its
        # seq says -- checking the congruence there would ask the pinned rows to be the bug they replaced.
        mismatched = []
        for slot, record, content, _ in records:
            if content is None or record['body'] == line_bytes:
                continue
            if pinned and reader.pinned_slot (content) is not None:
                continue
            text = reader.text_of (content)
            named = re.search (markers[name] + r'(\d+)', text)
            if named is None or int (named.group (1)) != record['seq']:
                mismatched.append ((slot, record['seq'],
                                    named.group (1) if named else None, text[:48]))
        if mismatched:
            fail (f'{len (mismatched)} {name} records do not hold the row their seq names, first at '
                  f'slot {mismatched[0][0]} seq {mismatched[0][1]} named {mismatched[0][2]}: '
                  f'{mismatched[0][3]!r}')
    at_bound = [record for record in kernel if record[1]['body'] == line_bytes]
    if len (at_bound) != 2:
        fail (f'{len (at_bound)} kernel records sit at the writer\'s {line_bytes} bound, expected the 2 '
              'the model truncated on purpose: a bound that reads as 0 is a control that never ran')
    # The positive control for the byte sum. The reader recomputes the sum over the bytes the sector
    # holds, so a writer that sums before filtering produces a record that is whole on the card and
    # believed by nobody; the archives have never held one (0 sum mismatches over 60,986 records), so
    # this row is the first reading that separates the two answers. Its own dot substitutions are what
    # say the filter ran, and the `dirty` leg above is what says the sum agreed with the stored bytes.
    control = [(slot, reader.text_of (content)) for slot, record, content, _ in driver
               if content is not None and
               'host row %d tab' % (reader.PINNED_SLOTS + 70) in reader.text_of (content)]
    if len (control) != 1:
        fail (f'{len (control)} driver records carry control bytes, expected the 1 the model pushes: '
              'without it the byte-sum leg only ever sees rows that could not disagree')
    else:
        text = control[0][1]
        if 'tab.' not in text or 'high.' not in text or ' end' not in text:
            fail (f'the control row reached the card as {text[:64]!r}: the dots that name the filter did '
                  'not replace both control bytes, or the tail after them did not survive')

    # The panic path's own record, decoded from the sectors the model wrote it into. `body` here is what
    # the reader's clamp allows and nothing more, because a sentence longer than one block is the case the
    # second copy met for the first time in 120 boots; the sum is then recomputed in this file, over the
    # bytes read off the window, without calling either the layer or reader.decode -- the third voice, so
    # the agreement is not reader.checksum compared against reader.checksum.
    report = reader.decode (win, first_lba, slots=reader.REPORT_SLOTS, base_sector=0, expect_kind=20)
    if len (report) != 1:
        fail (f'{len (report)} report records decoded, expected the 1 the model wrote: the reader stops at '
              'the first slot without the magic, so a panic record that does not begin at slot 0 is one '
              'nobody reads')
    else:
        _, panic_record, panic_content, panic_problems = report[0]
        if panic_problems:
            fail (f'the reader did not believe the panic record: {panic_problems}')
        if (panic_record['seq'], panic_record['boot'], panic_content is None) != (7, boot_id, False):
            fail (f'the panic record carries seq {panic_record["seq"]} boot {panic_record["boot"]} '
                  f'content={panic_content is not None}: the scan that picks its slot names the record\'s '
                  'turn, and a reader cannot tell a rotated window from a stale one without both')
        reader_clamp = re.search (r'if body > (\d+):', READER.read_text ())
        if reader_clamp is None:
            fail (f'{READER} checks no body bound: the clamp the writer truncates to has no reader-side '
                  'number to agree with')
        elif panic_record['body'] != int (reader_clamp.group (1)):
            fail (f'the panic record holds {panic_record["body"]} bytes, expected the reader\'s own bound '
                  f'{reader_clamp.group (1)}: a body the reader rejects is a panic sentence lost, and the '
                  'model wrote one 480 bytes longer than this')
        else:
            stored_text = win[reader.SECTOR:2 * reader.SECTOR]
            head = reader.PREFIX
            body_slice = stored_text[len (head):len (head) + panic_record['body']]
            recomputed = (sum (head) + sum (body_slice) + 0x0a) & 0xFFFFFFFF
            if recomputed != panic_record['sum']:
                fail (f'the panic record\'s sum is {panic_record["sum"]:#x} and the bytes the sector holds '
                      f'sum to {recomputed:#x}: the writer summed text the card does not carry, which makes '
                      'the record undecodable by anyone')
            if b'\x07' in body_slice or b'\xff' in body_slice:
                fail ('the panic record stored a control byte the printable filter should have replaced')
            # The model's sentence is "panic:" then a tab, 0x07 and 0xff, then 'P' up to the clamp. The
            # three dots are the filter's own reading -- a record whose body is the sentence unchanged
            # would mean the filter never ran here, which is the answer this path had before -- and the
            # filler to the clamp is the reading that the same body bound the rings use is the one this
            # record was cut to.
            want = b'panic:...' + b'P' * (panic_record['body'] - 9)
            if body_slice != want:
                fail (f'the panic sentence reached the card as {body_slice[:16]!r}...{body_slice[-8:]!r}, '
                      f'expected the filter\'s three dots followed by filler to {len (want)} bytes: '
                      f'{body_slice.count (0x2E)} dots were stored')

    # Control: the same bytes looked for two sectors late, asked at the base the writer actually used.
    # An earlier version also moved the asked base by two, and the two moves cancelled: every record
    # matched and the control passed by finding nothing. That is the defect class round340 named -- a
    # window read at the wrong base parses clean unless something stamps the address it wrote into the
    # record and a reader compares it.
    shifted = reader.decode (win[2 * reader.SECTOR:], first_lba,
                             slots=reader.VERDICT_SLOTS, base_sector=reader.VERDICT_OFFSET,
                             expect_kind=(21, 25))
    wrong_lba = [record for record in shifted if 'lba' in ' '.join (record[3])]
    if not shifted or not wrong_lba:
        fail ('the two-sectors-early control decoded records with no complaint: the reader would accept '
              'a window written at the wrong base, which is the defect class round340 named')

    if problems:
        for problem in problems:
            print ('PROBLEM ' + problem)
        print (f'sdhci host model: {len (problems)} problem(s)')
        return 1
    print ('sdhci host model: OK')
    print (f'sdhci host model cases={len (cases)} driver={len (driver)} kernel={len (kernel)} '
           f'at-bound={len (at_bound)} control-mismatches={len (wrong_lba)}')
    return 0


if __name__ == '__main__':
    sys.exit (main ())
