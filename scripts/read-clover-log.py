#!/usr/bin/env python3
"""Decode the clover log-window records into readable text.

Since round185 the window is the `logdump` partition (the card's p56, 64 MiB). Before that it was
`logfs` (p39), which the shipped boot now mounts as the root APFS container, so the driver has no
business writing into it.

clover has no serial console, so the driver (drivers/CloverSdhci) and the panic
screen keep their text on the card: each record is two 512-byte blocks, a header
and a text block that carries `PREFIX + body + "\\n"`, so the record can also be
read on a machine that has never heard of this format.

Three windows live in the partition:

  report / panic window   sectors 0..31, 16 slots at first + 2k
      kinds 17 (report overwrite), 19 (report append), 20 (panic screen)
  verdict ring            sectors 64..191, 64 slots at first + 64 + 2k
      kind 21, written by round174: the driver's own measurement lines, so a
      reading no longer has to still be on the panel when the photo is taken.
  kernel-log ring         sectors 192..1215, 512 slots at first + 192 + 2k
      kind 22, written from round176: the lines the round175 tee lifted out of
      msgbuf -- APFS, code-signing, shared-region, exec/exit -- each of which
      used to share the 64 records above with the driver. `--tee` reads this
      ring; the driver's state lines about the tee stay in the ring above, so
      every record here is something the kernel said.
  flight region           sectors 32..43 and 56..59, one block per record
      kind 23, written from round210: twelve entry records (sector 32) name
      the block request that is starting plus the result and duration of the
      one before it, and four beat records (sector 56) carry CPU0's four-second
      sample of SDHCI_PRESENT_STATE. `--flight` reads them. A last entry with
      no successor is the request that never came back; a beat that is absent
      is a guard that was held or a controller that refused the command.

Header words:

  words[0] 0x554e5855  "UXNU"
  words[1] 0x31474f4c  "LOG1"
  words[2] absolute LBA of the header block
  words[3] total bytes of the text block: prefix + body + newline
  words[4] byte sum of those bytes, low 32 bits
  words[5] kind: 17, 19, 20 above, 21 = driver verdict line, 22 = kernel log line;
      the flight region uses the same two magic words but one block per record,
      with kind 23 and its own header words (see decode_flight below)
  words[6] body length
  words[7] slot index
  words[8] sequence number (the two rings only; 0 in the report/panic records)

The sequence number is what makes a wrapped ring orderable: after a ring has
more records than slots, the slot index stops being time, so both ring readers
sort by seq.

Producer is read off the text as well as the kind: a line that starts with
"CloverSdhci " was said by the driver itself. In the kernel ring that is a
contradiction worth naming -- the tee drops those lines on purpose, because
they are already on the card through the driver's own ring -- so `--driver`
over `--tee` counts zero, and a nonzero count means the tee started echoing.

Input is a dd of the log window from the card -- logdump since round185. The default window needs
the first 64
sectors; the verdict ring needs 192 and the kernel ring needs 1216, which is
what scripts/run-clover-unattended.sh now reads. A record whose header is
present but whose text does not sum is still printed: a half-written record is
a reading.
"""
import argparse
import struct
import sys
from pathlib import Path

PREFIX = b"unixnu clover emmc log\n"
MAGIC = (0x554E5855, 0x31474F4C)  # "UXNU", "LOG1" in memory order
SECTOR = 512
SLOT_BYTES = 2 * SECTOR
REPORT_SLOTS = 16
VERDICT_SLOTS = 64    # round174's ring, grown by round176 because the tee no longer shares it
VERDICT_OFFSET = 64   # sectors past the log window start, clear of the report/panic blocks
KERNEL_SLOTS = 512    # round176's ring: a whole boot of allowlisted kernel text
KERNEL_OFFSET = 192   # 64 + 2 * VERDICT_SLOTS: immediately after the driver's ring
FLIGHT_ENTRY_OFFSET = 32   # round210: the gap the report window and the driver's ring leave
FLIGHT_ENTRY_SLOTS = 12
FLIGHT_BEAT_OFFSET = 56    # four one-block beats, 4 s apart, at the end of that gap
FLIGHT_BEAT_SLOTS = 4
FLIGHT_KIND = 23
LOG_WINDOW_FIRST_LBA = 13320216   # the card's p56 (logdump) since round185; p39 logfs is the root now
DUMP_SECTORS = KERNEL_OFFSET + 2 * KERNEL_SLOTS   # one dd that reaches the end of both rings
KINDS = {17: "report-17", 19: "report-19", 20: "panic-20",
         21: "verdict-21", 22: "kernel-22", 23: "flight-23"}
FLIGHT_EVENTS = {1: "read", 2: "write", 3: "beat"}
FLIGHT_DIRS = {0: "read", 1: "write", 2: "beat"}
DRIVER_PREFIX = b"CloverSdhci "


def producer(content):
    """"driver" for a line CloverSdhci said, "kernel" for one the round175 tee pulled
    out of msgbuf, "-" for a record with no readable text. The driver's own readings
    always start with its name because sdhciSay and teeSay are the only ways it speaks,
    so no header bit is needed to tell the two apart -- and a kernel line that happens
    to begin with the driver's name would be the driver's line by any useful definition.
    Since round176 the kind says the same thing independently, which is why a record in
    the kernel ring that this calls "driver" is a contradiction the report names."""
    if content is None:
        return "-"
    return "driver" if content.startswith(DRIVER_PREFIX) else "kernel"


def decode(data, first_lba, slots=REPORT_SLOTS, base_sector=0, expect_kind=None):
    """[(slot, header dict, text bytes or None, problems [str])] in slot order.

    The writers scan forward and stop at the first header without the magic, so a
    gap ends the window: later slots are not records of this boot.

    `expect_kind` is what a ring's own records must say in word 5. Round176 made the
    two rings the same shape in every other field, so the kind is the only thing in a
    record that distinguishes a driver verdict from a kernel line, and a reader that
    did not check it would happily print a stale ring as if it were the new one.
    """
    records = []
    for slot in range(slots):
        base = (base_sector + 2 * slot) * SECTOR
        block = data[base:base + SLOT_BYTES]
        if len(block) < SLOT_BYTES:
            break
        magic0, magic1, lba, total, checksum, kind, body, slot_word, seq = \
            struct.unpack_from("<9I", block)
        if (magic0, magic1) != MAGIC:
            break
        problems = []
        want_lba = first_lba + base_sector + 2 * slot
        if lba != want_lba:
            problems.append(f"lba {lba} != {want_lba}")
        if slot_word != slot:
            problems.append(f"slot word {slot_word} != {slot}")
        if expect_kind is not None and kind != expect_kind:
            problems.append(f"kind {kind} != {expect_kind}")
        if body > 480:
            problems.append(f"body {body} > 480")
        if total != len(PREFIX) + body + 1:
            problems.append(f"total {total} != {len(PREFIX) + body + 1}")
        text = block[SECTOR:]
        text_ok = True
        if not text.startswith(PREFIX):
            text_ok = False
            problems.append("prefix missing")
        elif text[len(PREFIX) + body:len(PREFIX) + body + 1] != b"\n":
            text_ok = False
            problems.append("newline missing")
        content = None
        if text_ok:
            content = text[len(PREFIX):len(PREFIX) + body]
            computed = sum(text[:len(PREFIX) + body + 1]) & 0xFFFFFFFF
            if computed != checksum:
                problems.append(f"sum {checksum:#x} != {computed:#x}")
        elif not any(text):
            problems.append("text block empty")
        records.append((slot, {
            "lba": lba,
            "total": total,
            "sum": checksum,
            "kind": kind,
            "body": body,
            "seq": seq,
            "text_ok": text_ok,
        }, content, problems))
    return records


def text_of(content):
    """The body as text. The writer stores one space for control bytes, so this is
    already printable, but a byte outside ASCII is replaced instead of guessed."""
    return bytes(byte if 0x09 <= byte < 0x7F else 0x2E for byte in content).decode("ascii")


def report(records, newest_only=False, order_by_seq=False):
    """Print records the way both windows are read. Returns the number printed.

    Producer filtering happens in main, before this is called, so --count and the
    printed listing always answer the same question about the same set of records.
    """
    if order_by_seq and records:
        # A ring that wrapped has its oldest record at the highest slot and the
        # newest at the lowest; the sequence stamp is the only time order left.
        records = sorted(records, key=lambda record: record[1]["seq"])
    if newest_only:
        records = records[-1:]
    for slot, header, content, problems in records:
        verdict = "ok" if not problems else "BAD (" + "; ".join(problems) + ")"
        said = f"  said by {producer(content)}" if header["kind"] in (21, 22) else ""
        print(f"slot {slot}  lba {header['lba']}  seq {header['seq']}  kind {header['kind']} "
              f"({KINDS.get(header['kind'], 'unknown')})  body {header['body']}  "
              f"sum {header['sum']:#010x}{said}  {verdict}")
        if content is not None:
            body = text_of(content)
            for line in body.rstrip("\n").split("\n"):
                print("    " + line)
        else:
            print("    <no readable text block>")
        print()
    return len(records)


FLIGHT_WORDS = 22


def decode_flight(data, first_lba):
    """The round210 flight region: {"entries": [...], "beats": [...]} in slot order.

    Each record is one block with FLIGHT_WORDS little-endian words and no text block, so
    there is no body and no sum -- the header *is* the record. Word order is what
    flightWrite lays down in CloverSdhci.cpp; a reader that disagrees about one word
    would print a plausible table of the wrong columns, so the selftest round-trips
    every field.

    Slots are reused modulo 12 (entries) and modulo 4 (beats) and seq restarts at zero
    each boot. Walking back from the highest seq therefore stops at the first slot whose
    stored seq is not the one the walk needs, and never below zero: records of this boot
    are the contiguous run 0..top, because flightWrite increments seq only after the
    block it wrote, so a gap means a slot that was never written this boot -- not a
    sequence number that was skipped.

    What this cannot tell apart is an earlier boot's run that also survives intact and
    reaches a higher seq than this boot's: every record of every boot is congruent with
    its own slot, so congruence alone does not name a boot. Round210 is the first boot
    to write this region, so a fresh partition has no such run; a later round has to
    stamp a boot id into the record before the chain can be trusted across two r210+
    boots.
    """
    groups = {}
    for name, base, slots, event in (("entries", FLIGHT_ENTRY_OFFSET, FLIGHT_ENTRY_SLOTS, None),
                                     ("beats", FLIGHT_BEAT_OFFSET, FLIGHT_BEAT_SLOTS, 3)):
        records = []
        for slot in range(slots):
            sector = base + slot
            block = data[sector * SECTOR:(sector + 1) * SECTOR]
            if len(block) < SECTOR:
                break
            words = struct.unpack_from(f"<{FLIGHT_WORDS}I", block)
            if (words[0], words[1]) != MAGIC:
                break
            problems = []
            want_lba = first_lba + sector
            if words[2] != want_lba:
                problems.append(f"lba {words[2]} != {want_lba}")
            if words[3] != FLIGHT_KIND:
                problems.append(f"kind {words[3]} != {FLIGHT_KIND}")
            if words[5] != slot:
                problems.append(f"slot word {words[5]} != {slot}")
            if words[6] not in FLIGHT_EVENTS:
                problems.append(f"event {words[6]} unknown")
            if words[7] not in FLIGHT_DIRS:
                problems.append(f"dir {words[7]} unknown")
            if event is not None and words[6] != event:
                problems.append(f"event {words[6]} is not a beat")
            record = {
                "slot": slot, "lba": words[2], "seq": words[4], "event": words[6],
                "dir": words[7], "op_lba": words[8], "op_blocks": words[9],
                "t_ms": words[10], "prev_done": words[11], "prev_ms": words[12],
                "present": words[13], "flags": words[14], "ops": words[15],
                "guard_waits": words[16], "beat_misses": words[17],
                "pending_kernel": words[18], "pending_driver": words[19],
                "prev_dir": words[20], "prev_blocks": words[21],
            }
            records.append((slot, record, problems))
        groups[name] = records
    return groups


def flight_chain(records):
    """This boot's run, oldest first: walk back from the highest seq whose slot holds
    that seq, stop at the first slot that does not, and never below seq 0.

    A record whose header is inconsistent is kept in the listing (it is still a reading)
    but ends the chain, because the slot can no longer be trusted to belong to this run.
    """
    by_slot = {slot: record for slot, record, problems in records
               if not problems}
    if not by_slot:
        return []
    slots = len(records) or 1
    top = max(record["seq"] for record in by_slot.values()
              if record["slot"] == record["seq"] % slots)
    chain = []
    seq = top
    while seq >= 0 and seq > top - slots:
        record = by_slot.get(seq % slots)
        if record is None or record["seq"] != seq:
            break
        chain.append(record)
        seq -= 1
    chain.reverse()
    return chain


def flight_report(groups):
    """Print the two chains and the one conclusion this round is built to support.

    The last entry record names a request whose result word belongs to the request
    *before* it, so the newest entry is the one with no successor: the request in
    flight when the writing stopped. The newest beat's time bounds when the guard and
    the controller were still accepting commands, and a gap between the two is where
    the stop happened.
    """
    entries = flight_chain(groups["entries"])
    beats = flight_chain(groups["beats"])
    written = len(groups["entries"]) + len(groups["beats"])
    if entries:
        span = f"seq {entries[0]['seq']}..{entries[-1]['seq']}"
    else:
        span = "none"
    print(f"flight: {len(entries)} entry records in this boot's window ({span})")
    for record in entries:
        flags = []
        if record["flags"] & 1:
            flags.append("cmd")
        if record["flags"] & 2:
            flags.append("dat")
        if record["flags"] & 4:
            flags.append("beat")
        print(f"  entry seq {record['seq']:>3} slot {record['slot']:>2} t={record['t_ms']:>8}ms "
              f"{FLIGHT_EVENTS.get(record['event'], '?')} lba={record['op_lba']} "
              f"blocks={record['op_blocks']} ps={record['present']:#010x} "
              f"inh={','.join(flags) or '-'} ops={record['ops']} "
              f"guard={record['guard_waits']} miss={record['beat_misses']} "
              f"pend={record['pending_kernel']}/{record['pending_driver']} "
              f"prev={FLIGHT_DIRS.get(record['prev_dir'], '?')}/{record['prev_blocks']}/"
              f"{record['prev_ms']}ms/{record['prev_done']}")
    print(f"flight: {len(beats)} beat records in this boot's window")
    for record in beats:
        flags = []
        if record["flags"] & 1:
            flags.append("cmd")
        if record["flags"] & 2:
            flags.append("dat")
        print(f"  beat  seq {record['seq']:>3} slot {record['slot']:>2} t={record['t_ms']:>8}ms "
              f"ps={record['present']:#010x} inh={','.join(flags) or '-'} "
              f"ops={record['ops']} guard={record['guard_waits']} miss={record['beat_misses']} "
              f"pend={record['pending_kernel']}/{record['pending_driver']}")
    if entries and beats:
        gap = entries[-1]["t_ms"] - beats[-1]["t_ms"]
        print(f"flight: newest entry seq {entries[-1]['seq']} at t={entries[-1]['t_ms']} ms, "
              f"newest beat seq {beats[-1]['seq']} at t={beats[-1]['t_ms']} ms, gap {gap} ms")
    elif entries:
        print("flight: entries without any beat -- no beat ever found the guard free")
    elif beats:
        print("flight: beats without any entry -- no block request started after arming")
    return written


def build_flight_record(lba, seq, event, dir_, op_lba, op_blocks, t_ms=0, present=0,
                        prev_done=0, prev_ms=0, prev_dir=0, prev_blocks=0, ops=0,
                        guard_waits=0, beat_misses=0, pending=(0, 0), slot=None, kind=FLIGHT_KIND):
    """One round210 flight block as flightWrite lays it out, for selftest."""
    if slot is None:
        slot = seq % (FLIGHT_BEAT_SLOTS if event == 3 else FLIGHT_ENTRY_SLOTS)
    flags = ((present & 1) and 1 or 0) | ((present & 2) and 2 or 0) | (4 if event == 3 else 0)
    return struct.pack(f"<{FLIGHT_WORDS}I", MAGIC[0], MAGIC[1], lba, kind, seq, slot,
                       event, dir_, op_lba, op_blocks, t_ms, prev_done, prev_ms,
                       present, flags, ops, guard_waits, beat_misses, pending[0],
                       pending[1], prev_dir, prev_blocks).ljust(SECTOR, b"\0")


def build_record(kind, slot, lba, body_text, seq=0):
    """One two-block record exactly as the driver lays it out, for selftest."""
    body = body_text.encode("ascii")
    total = len(PREFIX) + len(body) + 1
    checksum = (sum(PREFIX) + sum(body) + ord("\n")) & 0xFFFFFFFF
    header = struct.pack("<9I", MAGIC[0], MAGIC[1], lba, total, checksum, kind,
                         len(body), slot, seq) + bytes(SECTOR - 36)
    text = PREFIX + body + b"\n"
    return header + text.ljust(SECTOR, b"\0")


def selftest():
    """Round-trip all three windows, including a wrapped ring of each kind. The rings' geometry is
    asserted rather than trusted from a device frame: the driver computes the LBA of every record from
    the same offset and slot count this reader uses, so a geometry that disagrees here is a dump that
    silently reads one ring as the other. Round176 split the two producers apart -- driver verdicts
    into kind 21 at sector 64, kernel text the tee lifted out of msgbuf into kind 22 at sector 192 --
    so the per-ring kind check and the empty-intersection rule (--driver over --tee is zero) are both
    asserted here."""
    first_lba = LOG_WINDOW_FIRST_LBA
    # The two rings have to tile the partition with no overlap: the driver ring ends exactly where
    # the kernel ring starts, and a dd of DUMP_SECTORS reaches the last slot of the second one.
    assert VERDICT_OFFSET + 2 * VERDICT_SLOTS == KERNEL_OFFSET, KERNEL_OFFSET
    assert DUMP_SECTORS == KERNEL_OFFSET + 2 * KERNEL_SLOTS, DUMP_SECTORS
    # Round210's flight region lives in the gap the two windows leave: report 0..31, entries
    # 32..43, beats 56..59, verdict 64.. -- one block per record, so the whole region fits in
    # the 1216-sector dump both rings already read and no window moved to make room for it.
    assert FLIGHT_ENTRY_OFFSET == 2 * REPORT_SLOTS, FLIGHT_ENTRY_OFFSET
    assert FLIGHT_ENTRY_OFFSET + FLIGHT_ENTRY_SLOTS <= FLIGHT_BEAT_OFFSET, FLIGHT_BEAT_OFFSET
    assert FLIGHT_BEAT_OFFSET + FLIGHT_BEAT_SLOTS <= VERDICT_OFFSET, FLIGHT_BEAT_OFFSET

    dump = bytearray()
    dump += build_record(19, 0, first_lba, "bring-up report")
    dump += build_record(20, 1, first_lba + 2, "panic line")
    dump += bytes((VERDICT_OFFSET - 4) * SECTOR)          # gap before the driver ring: not records
    driver_lines = ["CloverSdhci r174: verdict ring armed lba=13320280 slots=64",
                    "CloverSdhci r176: kernel ring armed lba=13320408 slots=512",
                    "CloverSdhci r166: readcheck lba=13320216 sample=32 trusted=1",
                    "CloverSdhci r175: tee seen=512 kept=300 throttled=12 lap=0 idx=8192 size=65536",
                    "CloverSdhci r169: block guard waited 31 ms"]
    for index, line in enumerate(driver_lines):
        # Slots 0..4 with seq 5..9: a ring that wrapped, so slot order is not time order.
        slot = index
        lba = first_lba + VERDICT_OFFSET + 2 * slot
        dump += build_record(21, slot, lba, line, seq=5 + index)
    dump += bytes((KERNEL_OFFSET - VERDICT_OFFSET - 2 * len(driver_lines)) * SECTOR)
    kernel_lines = ["load code signature error 4 for file launchd",
                    "[apfs_jhash:146] hash mismatch for 16384 bytes at offset 12128256",
                    "clover r170: check_np pid=12 comm=launchd dyld_flags=0x0",
                    "apfs_mountroot failed: 19"]
    for index, line in enumerate(kernel_lines):
        slot = index
        lba = first_lba + KERNEL_OFFSET + 2 * slot
        dump += build_record(22, slot, lba, line, seq=17 + index)
    data = bytes(dump)
    assert len(data) == (KERNEL_OFFSET + 2 * len(kernel_lines)) * SECTOR, len(data)

    reports = decode(data, first_lba)
    assert len(reports) == 2, reports
    assert [r[1]["kind"] for r in reports] == [19, 20], reports
    assert all(not r[3] for r in reports), reports
    assert text_of(reports[0][2]) == "bring-up report", reports[0]

    def ring(slots, base, expect_kind):
        return sorted(decode(data, first_lba, slots=slots, base_sector=base,
                             expect_kind=expect_kind), key=lambda v: v[1]["seq"])

    verdicts = ring(VERDICT_SLOTS, VERDICT_OFFSET, 21)
    assert [text_of(v[2]) for v in verdicts] == driver_lines, verdicts
    assert all(v[1]["kind"] == 21 for v in verdicts), verdicts
    assert all(not v[3] for v in verdicts), verdicts
    assert verdicts[0][1]["seq"] == 5, verdicts[0]

    kernel = ring(KERNEL_SLOTS, KERNEL_OFFSET, 22)
    assert [text_of(v[2]) for v in kernel] == kernel_lines, kernel
    assert all(v[1]["kind"] == 22 for v in kernel), kernel
    assert all(not v[3] for v in kernel), kernel
    assert kernel[0][1]["seq"] == 17, kernel[0]

    # Who said each line, per ring. The driver ring holds the driver's readings plus the tee's state
    # lines about itself; the kernel ring holds kernel text and nothing else, which is the whole point
    # of round176: a busy daemon can no longer evict a verdict, and a --driver filter over --tee that
    # counts anything means the tee started echoing its own lines onto the card.
    assert all(producer(record[2]) == "driver" for record in verdicts), verdicts
    assert all(producer(record[2]) == "kernel" for record in kernel), kernel
    assert [record for record in kernel if producer(record[2]) == "driver"] == [], kernel
    # A dump with zero kernel lines is still the difference between "the tee ran and nothing matched"
    # and "the tee never ran": the driver ring's tee census answers it.
    assert any("tee seen=" in text_of(record[2]) for record in verdicts), verdicts
    # An unreadable record has no producer to invent.
    assert producer(None) == "-", producer(None)

    # A record whose kind does not match the ring it was read from is named, because after round176
    # the two rings are the same shape and the kind is the only thing that tells them apart.
    mixed = bytearray(bytes(KERNEL_OFFSET * SECTOR) +
                      build_record(21, 0, first_lba + KERNEL_OFFSET, "stale line", seq=1))
    problems = decode(bytes(mixed), first_lba, slots=KERNEL_SLOTS,
                      base_sector=KERNEL_OFFSET, expect_kind=22)[0][3]
    assert any(problem.startswith("kind 21 != 22") for problem in problems), problems

    # A record that does not sum is still printed, with the mismatch named -- and so is a
    # half-written one whose text block lost the prefix, because both are readings. Checked on both
    # rings: the two writers share a formatter and a record shape, so a defect is a shared defect.
    for base, slots, kind in ((VERDICT_OFFSET, VERDICT_SLOTS, 21),
                              (KERNEL_OFFSET, KERNEL_SLOTS, 22)):
        for offset, expect in ((SECTOR + len(PREFIX) + 1, "sum"), (SECTOR, "prefix missing")):
            broken = bytearray(bytes(base * SECTOR) +
                               build_record(kind, 0, first_lba + base, "line", seq=1))
            broken[base * SECTOR + offset] = ord("X")
            problems = decode(bytes(broken), first_lba, slots=slots,
                              base_sector=base, expect_kind=kind)[0][3]
            assert any(problem.startswith(expect) for problem in problems), (base, expect, problems)

    # Round210's flight region: 14 entries and 6 beats wrap both slot counts, every field is
    # round-tripped, and the chain walk picks the newest run instead of the largest time stamp --
    # the stale case below is the one that matters, because a partition is not erased per boot.
    flight = bytearray((FLIGHT_BEAT_OFFSET + FLIGHT_BEAT_SLOTS) * SECTOR)

    def place(target, block, sector):
        target[sector * SECTOR:(sector + 1) * SECTOR] = block

    for seq in range(14):
        event = 1 if seq % 2 == 0 else 2
        place(flight,
              build_flight_record(first_lba + FLIGHT_ENTRY_OFFSET + seq % FLIGHT_ENTRY_SLOTS,
                                  seq, event, 0 if event == 1 else 1, 1000 + seq, 8,
                                  t_ms=100 * seq, present=3, prev_done=1 if seq else 0,
                                  prev_ms=40 + seq, prev_dir=1, prev_blocks=8, ops=seq,
                                  guard_waits=seq // 2, beat_misses=seq // 3,
                                  pending=(7, 2)),
              FLIGHT_ENTRY_OFFSET + seq % FLIGHT_ENTRY_SLOTS)
    for seq in range(6):
        place(flight,
              build_flight_record(first_lba + FLIGHT_BEAT_OFFSET + seq % FLIGHT_BEAT_SLOTS,
                                  seq, 3, 2, 0, 0, t_ms=1000 + 4000 * seq, present=0,
                                  ops=seq, guard_waits=1, beat_misses=seq, pending=(9, 1)),
              FLIGHT_BEAT_OFFSET + seq % FLIGHT_BEAT_SLOTS)
    groups = decode_flight(bytes(flight), first_lba)
    entries = flight_chain(groups["entries"])
    beats = flight_chain(groups["beats"])
    assert [record["seq"] for record in entries] == list(range(2, 14)), entries
    assert [record["seq"] for record in beats] == [2, 3, 4, 5], beats
    newest = entries[-1]
    assert (newest["t_ms"], newest["op_lba"], newest["op_blocks"], newest["event"]) == \
           (1300, 1013, 8, 2), newest
    assert (newest["prev_dir"], newest["prev_blocks"], newest["prev_ms"], newest["prev_done"],
            newest["ops"], newest["guard_waits"], newest["beat_misses"],
            newest["pending_kernel"], newest["pending_driver"]) == (1, 8, 53, 1, 13, 6, 4, 7, 2), newest
    assert (beats[-1]["t_ms"], beats[-1]["beat_misses"], beats[-1]["pending_kernel"]) == \
           (21000, 5, 9), beats[-1]
    assert all(not problems for _, _, problems in groups["entries"] + groups["beats"])
    assert flight_report(groups) == len(groups["entries"]) + len(groups["beats"])

    # A boot that stopped after five requests leaves slots 5..11 with whatever was there
    # before. A slot whose seq word does not name its own slot ends the chain instead of
    # being printed as this boot's; the congruent case is the one decode_flight's docstring
    # says a boot id has to settle, and is not asserted as correct here.
    stale = bytearray(flight)
    for seq in range(5):
        place(stale,
              build_flight_record(first_lba + FLIGHT_ENTRY_OFFSET + seq, seq, 1, 0, 100 + seq, 1,
                                  t_ms=50 * seq),
              FLIGHT_ENTRY_OFFSET + seq)
    for slot in range(5, FLIGHT_ENTRY_SLOTS):
        place(stale,
              build_flight_record(first_lba + FLIGHT_ENTRY_OFFSET + slot, 50 + slot, 2, 1, 0, 0,
                                  t_ms=900000),
              FLIGHT_ENTRY_OFFSET + slot)
    stale_entries = flight_chain(decode_flight(bytes(stale), first_lba)["entries"])
    assert [record["seq"] for record in stale_entries] == [0, 1, 2, 3, 4], stale_entries
    assert stale_entries[-1]["t_ms"] == 200, stale_entries[-1]

    # A flight block that says some other kind is named, not printed as a r210 record.
    wrong = bytearray(flight)
    place(wrong, build_flight_record(first_lba + FLIGHT_ENTRY_OFFSET, 0, 1, 0, 0, 0, kind=21),
          FLIGHT_ENTRY_OFFSET)
    problems = decode_flight(bytes(wrong), first_lba)["entries"][0][2]
    assert any(problem.startswith("kind 21 != 23") for problem in problems), problems
    print("read-clover-log selftest: report window, both rings, flight region, wrap order, "
          "ring kinds and bad records all ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", nargs="?", default="clover-log-window.bin",
                        help="the log window dumped from the card -- logdump since round185 "
                             "(default: clover-log-window.bin)")
    parser.add_argument("--first-lba", type=lambda value: int(value, 0),
                        default=LOG_WINDOW_FIRST_LBA,
                        help="log window first LBA (default 13320216, the card's p56 logdump; "
                             "pass 12320768 to read a card flashed before round185, whose rings "
                             "would sit at the start of p39 logfs)")
    parser.add_argument("--verdict", action="store_true",
                        help="read the driver's verdict ring instead (kind 21, sectors 64..191)")
    parser.add_argument("--tee", action="store_true",
                        help="read the kernel-log ring the round175 tee writes (kind 22, "
                             "sectors 192..1215)")
    parser.add_argument("--flight", action="store_true",
                        help="read the round210 flight region instead: entry records (kind 23, "
                             "sectors 32..43) and four-second beats (sectors 56..59)")
    parser.add_argument("--driver", action="store_true",
                        help="of the ring being read, only the lines CloverSdhci said itself; over "
                             "--tee this is expected to be empty, and is a check that the tee is not "
                             "echoing")
    parser.add_argument("--count", action="store_true",
                        help="print the number of records and exit (for scripts)")
    parser.add_argument("--newest", action="store_true",
                        help="print only the newest record")
    parser.add_argument("--selftest", action="store_true",
                        help="round-trip all three windows in memory and exit (no card needed)")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    path = Path(args.dump)
    if not path.is_file():
        raise SystemExit(f"no dump at {path}")
    data = path.read_bytes()
    if len(data) < SLOT_BYTES:
        raise SystemExit(f"{path} is {len(data)} bytes; the first slot alone is {SLOT_BYTES}")
    if args.flight:
        need = (FLIGHT_BEAT_OFFSET + FLIGHT_BEAT_SLOTS) * SECTOR
        if len(data) < need:
            raise SystemExit(f"{path} is {len(data)} bytes; the flight region needs {need}")
        groups = decode_flight(data, args.first_lba)
        written = len(groups["entries"]) + len(groups["beats"])
        if args.count:
            print(written)
            return 0 if written else 1
        if not written:
            print(f"{path}: no flight records (sector {FLIGHT_ENTRY_OFFSET} has no LOG1 magic)",
                  file=sys.stderr)
            return 1
        flight_report(groups)
        return 0
    if args.tee:
        slots, base, kind, window = KERNEL_SLOTS, KERNEL_OFFSET, 22, "kernel-log ring"
    elif args.verdict:
        slots, base, kind, window = VERDICT_SLOTS, VERDICT_OFFSET, 21, "verdict ring"
    else:
        slots, base, kind, window = REPORT_SLOTS, 0, None, "report window"
    if kind is not None:
        need = (base + 2 * slots) * SECTOR
        if len(data) < need:
            raise SystemExit(f"{path} is {len(data)} bytes; the {window} needs {need}")
    records = decode(data, args.first_lba, slots=slots, base_sector=base, expect_kind=kind)
    if args.driver:
        records = [record for record in records if producer(record[2]) == "driver"]
    if args.count:
        print(len(records))
        return 0 if records else 1
    if not records:
        named = window if not args.driver else f"{window} (driver lines)"
        print(f"{path}: no records ({named} slot 0 has no LOG1 magic)", file=sys.stderr)
        return 1
    report(records, newest_only=args.newest, order_by_seq=kind is not None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
