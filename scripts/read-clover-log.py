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
  flight region           sectors 32..43, 44..55, 56..59 and 60..63, one block per record
      kind 23, written from round210: twelve entry records (sector 32) name
      the block request that is starting plus the result and duration of the
      one before it, and four beat records (sector 56) carry CPU0's four-second
      sample of SDHCI_PRESENT_STATE. `--flight` reads them. A last entry with
      no successor is the request that never came back; a beat that is absent
      is a guard that was held or a controller that refused the command.
  boot record             sector 1216, one block
      kind 24, written from round212: a counter the driver reads back and
      increments once per boot, so records of two boots are no longer told
      apart by guessing. `--boot` reads it; every two-block record carries the
      same number in word 9.
  probe records           inside the driver ring, kind 25
      written from round212: the kernel-side lines (killsig, sender pc, thread
      snapshot) forwarded through clover_emmc_probe_say while the process is
      still alive. They land in the same slots as the driver's own verdict
      lines because the panic that follows may be a minute later, after msgbuf
      and the kernel ring have wrapped. `--probe` reads just these.

Header words:

  words[0] 0x554e5855  "UXNU"
  words[1] 0x31474f4c  "LOG1"
  words[2] absolute LBA of the header block
  words[3] total bytes of the text block: prefix + body + newline
  words[4] byte sum of those bytes, low 32 bits
  words[5] kind: 17, 19, 20 above, 21 = driver verdict line, 22 = kernel log line,
      25 = probe line the driver relayed;
      the flight region uses the same two magic words but one block per record,
      with kind 23 and its own header words (see decode_flight below), and round293
      stamps each record with the boot that wrote it so a leftover from the boot
      before the flash cannot be read as this boot's ending
  words[6] body length
  words[7] slot index
  words[8] sequence number (the two rings only; 0 in the report/panic records)
  words[9] boot id (0 in records written before round212, and in the flight
      region, which has its own words)

The sequence number is what makes a wrapped ring orderable: after a ring has
more records than slots, the slot index stops being time, so both ring readers
sort by seq. The boot id is what makes a slot knowable at all across boots: a
stale slot is congruent with its own position, so slot arithmetic alone cannot
say whether the record there is this boot's.

Producer is read off the text as well as the kind: a line that starts with
"CloverSdhci " was said by the driver itself. In the kernel ring that is a
contradiction worth naming -- the tee drops those lines on purpose, because
they are already on the card through the driver's own ring -- so `--driver`
over `--tee` counts zero, and a nonzero count means the tee started echoing.

Input is a dd of the log window from the card -- logdump since round185. The default window needs
the first 64
sectors; the verdict ring needs 192 and the kernel ring needs 1216, and round212's boot record
needs 1217, which is what scripts/run-clover-unattended.sh now reads. A record whose header is
present but whose text does not sum is still printed: a half-written record is
a reading.
"""
import argparse
import contextlib
import io
import re
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
FLIGHT_DEADLINE_OFFSET = 60   # round236: the deadline fires once and gets its own block
FLIGHT_DEADLINE_SLOTS = 1
FLIGHT_PANIC_OFFSET = 61      # round236: the sink's entry and exit, one block each
FLIGHT_PANIC_SLOTS = 2
# round302: the panic sentence, written blind into the twelve sectors that sat unowned between the
# entry ring and the beats. Its slot is boot id modulo 12, so the sink never reads the card to find it
# -- which is exactly the read boot 90 lost its sentence on.
FLIGHT_TEXT_OFFSET = 44
FLIGHT_TEXT_SLOTS = 12
FLIGHT_TEXT_BYTES = 352       # 88 words, the room left after the record's three own fields
# round309: the drain's own first record, in the one sector of the region that had no owner. One slot,
# so every boot overwrites the last one -- which is why the boot stamp and the STALE rule below are the
# whole of its attribution, exactly as round293 required of the deadline and panic sectors.
FLIGHT_DRAIN_OFFSET = 63
FLIGHT_DRAIN_SLOTS = 1
FLIGHT_KIND = 23
# round302: a verdict record signs itself. flightWrite writes this word in the slot right after the
# event's own extras, after them, so a sector that was never written as this event cannot be read as
# one -- boot 91's panic slot carried a marker's event words and a block request's header, and the
# decoder printed round293's boot stamp as that boot's `stage`.
FLIGHT_MARKER_MAGIC = 0x434C564D      # 'CLVM'
FLIGHT_MAGIC_WORD = {4: 46, 5: 53, 6: 126, 7: 43}
# Round310 gives the panic marker five more words, and the signature always lands one word past the last
# extra -- so this sector has carried it at 43 (rounds 302-304), 48 (305-309) and 53 (310+). A reader
# that knows only the newest index reads boot 102's own marker as "never a marker", which is the
# generation rule aimed the wrong way: the record is this boot's, the layout moved under it. So the
# older positions are tried in turn and the one that signed the sector is what gets printed.
FLIGHT_MAGIC_FALLBACK = {5: (48, 43)}
# The sink's own door-step reading, for the marker's words [15..17].
PANIC_QUIESCE = {0: "nothing outstanding", 1: "took the ticket, card back",
                 2: "took the ticket, reset refused", 3: "no holder, card back",
                 4: "no holder, reset refused", 5: "the steal lost the race"}
BOOT_OFFSET = KERNEL_OFFSET + 2 * KERNEL_SLOTS   # round212: one block after the kernel ring
BOOT_KIND = 24
PROBE_KIND = 25
LOG_WINDOW_FIRST_LBA = 13320216   # the card's p56 (logdump) since round185; p39 logfs is the root now
DUMP_SECTORS = BOOT_OFFSET + 1    # round212 adds the boot record past the end of both rings
KINDS = {17: "report-17", 19: "report-19", 20: "panic-20",
         21: "verdict-21", 22: "kernel-22", 23: "flight-23",
         24: "boot-24", 25: "probe-25"}
FLIGHT_EVENTS = {1: "read", 2: "write", 3: "beat", 4: "deadline", 5: "panic", 6: "text",
                 7: "drain-in"}
FLIGHT_DIRS = {0: "read", 1: "write", 2: "beat", 3: "none"}
# Round293: where each event's own boot id lives. Boot 79's frame is the reason: the deadline slot
# and the panic slots are the only two groups a boot may never write, and when it does not the
# previous boot's copy stays on the card and read, in this reader's own words, as if "the deadline
# fired" this boot. An entry carries no extras, so its stamp is word 35; round305 gave the beat twelve
# of its own -- its kernel second, four starvation counters and the last refused write -- so its stamp
# is the thirteenth word, 47. The deadline's ten words come first and its stamp is the eleventh;
# round236 already wrote gBootId as the panic marker's sixth extra word. A zero means a record written
# before this round stamped anything.
FLIGHT_BOOT_WORD = {1: 35, 2: 35, 3: 47, 4: 45, 5: 41, 6: 37, 7: 35}
# Round213's holder stages. A beat whose stage word has not moved for 75 s is the request that
# stopped, so the name is what the dump prints where r212 could only print the request.
FLIGHT_STAGES = {0: "idle", 1: "entry", 2: "idleWait", 3: "check", 4: "adma", 5: "batch",
                 6: "single", 7: "settle", 8: "tail", 9: "tee", 10: "flush", 11: "commit",
                 12: "reinit"}
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
    Round212 puts the relayed probe lines (kind 25) in the same slots as the driver's
    verdicts, so the driver ring accepts either and both kinds stay named.

    `boot` is word 9: round212's per-boot counter. It is carried, not checked, because
    a record written by an earlier build has zero there and is still a reading; what it
    buys is that a slot whose seq happens to be congruent can be told apart by boot.
    """
    expects = ()
    if expect_kind is not None:
        expects = tuple(expect_kind) if isinstance(expect_kind, (tuple, list, set)) else (expect_kind,)
    records = []
    for slot in range(slots):
        base = (base_sector + 2 * slot) * SECTOR
        block = data[base:base + SLOT_BYTES]
        if len(block) < SLOT_BYTES:
            break
        magic0, magic1, lba, total, checksum, kind, body, slot_word, seq, boot = \
            struct.unpack_from("<10I", block)
        if (magic0, magic1) != MAGIC:
            break
        problems = []
        want_lba = first_lba + base_sector + 2 * slot
        if lba != want_lba:
            problems.append(f"lba {lba} != {want_lba}")
        if slot_word != slot:
            problems.append(f"slot word {slot_word} != {slot}")
        if expects and kind not in expects:
            problems.append(f"kind {kind} != {expects[0]}" if len(expects) == 1
                            else f"kind {kind} not in {expects}")
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
            "boot": boot,
            "text_ok": text_ok,
        }, content, problems))
    return records


def decode_boot(data, first_lba):
    """Round212's boot record at sector 1216: {"boot": n, "problems": [...]} or None.

    One block, the same two magic words as a two-block record, kind 24, and the
    counter in word 9; words 3, 4, 6, 7 and 8 are zero because there is no text
    block and no slot. The driver reads this block at arming time, increments the
    number and writes it back, so the value here is the boot that most recently
    armed -- which is what every two-block record of that boot carries in word 9.
    """
    base = BOOT_OFFSET * SECTOR
    block = data[base:base + SECTOR]
    if len(block) < SECTOR:
        return None
    magic0, magic1, lba, _total, _sum, kind, _body, _slot, _seq, boot = \
        struct.unpack_from("<10I", block)
    if (magic0, magic1) != MAGIC:
        return None
    problems = []
    want_lba = first_lba + BOOT_OFFSET
    if lba != want_lba:
        problems.append(f"lba {lba} != {want_lba}")
    if kind != BOOT_KIND:
        problems.append(f"kind {kind} != {BOOT_KIND}")
    return {"boot": boot, "lba": lba, "problems": problems}


def text_of(content):
    """The body as text. The writer stores one space for control bytes, so this is
    already printable, but a byte outside ASCII is replaced instead of guessed."""
    return bytes(byte if 0x09 <= byte < 0x7F else 0x2E for byte in content).decode("ascii")


def of_kind(records, kinds):
    """The records whose header kind is one of `kinds`.

    decode() keeps a record whose kind is not the expected one and names the
    mismatch as a problem, because that is a reading worth printing. A reader
    that asked for one kind of record still has to drop the others itself:
    round212's driver ring holds both verdicts (21) and relayed probes (25), and
    `--probe --count` that counted the whole ring reported 36 probe lines on a
    boot that had none.
    """
    wanted = tuple(kinds) if isinstance(kinds, (tuple, list, set)) else (kinds,)
    return [record for record in records if record[1]["kind"] in wanted]


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
        said = (f"  said by {producer(content)}"
                if header["kind"] in (21, 22, PROBE_KIND) else "")
        print(f"slot {slot}  lba {header['lba']}  seq {header['seq']}  boot {header['boot']}  "
              f"kind {header['kind']} "
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


FLIGHT_WORDS = 35
FLIGHT_BLOCK_WORDS = SECTOR // 4   # round236: extras live at 35.., so the whole block is read


def decode_flight(data, first_lba):
    """The flight region: entries, beats, the deadline record and the panic markers.

    Each record is one block with FLIGHT_WORDS little-endian header words and no text block,
    so there is no body and no sum -- the header *is* the record. Word order is what
    flightWrite lays down in CloverSdhci.cpp; a reader that disagrees about one word
    would print a plausible table of the wrong columns, so the selftest round-trips
    every field. Round236 adds words 35.. as event-specific extras: the deadline's
    signed-side numbers (seconds, ticks, cnt0, cnt1, freq, each a 64-bit pair) and the
    panic marker's gate states with the sink's append rc.

    Slots are reused modulo 12 (entries), modulo 4 (beats), once (deadline) and twice
    (panic) and seq restarts at zero each boot. Walking back from the highest seq
    therefore stops at the first slot whose stored seq is not the one the walk needs,
    and never below zero: records of this boot are the contiguous run 0..top, because
    flightWrite increments seq only after the block it wrote, so a gap means a slot that
    was never written this boot -- not a sequence number that was skipped.

    What this cannot tell apart is an earlier boot's run that also survives intact and
    reaches a higher seq than this boot's -- unless the writer's boot id is in the record.
    Round293 puts it there: FLIGHT_BOOT_WORD names where each event's own gBootId sits, so
    every record can be read as this boot's or as a previous boot's leftover, and a record
    from a payload that stamped nothing reads as unattributed rather than as this boot's.
    """
    groups = {}
    for name, base, slots, event in (("entries", FLIGHT_ENTRY_OFFSET, FLIGHT_ENTRY_SLOTS, None),
                                     ("beats", FLIGHT_BEAT_OFFSET, FLIGHT_BEAT_SLOTS, 3),
                                     ("deadline", FLIGHT_DEADLINE_OFFSET, FLIGHT_DEADLINE_SLOTS, 4),
                                     ("panic", FLIGHT_PANIC_OFFSET, FLIGHT_PANIC_SLOTS, 5),
                                     ("drain", FLIGHT_DRAIN_OFFSET, FLIGHT_DRAIN_SLOTS, 7),
                                     ("text", FLIGHT_TEXT_OFFSET, FLIGHT_TEXT_SLOTS, 6)):
        records = []
        for slot in range(slots):
            sector = base + slot
            block = data[sector * SECTOR:(sector + 1) * SECTOR]
            if len(block) < SECTOR:
                break
            words = struct.unpack_from(f"<{FLIGHT_BLOCK_WORDS}I", block)
            if (words[0], words[1]) != MAGIC:
                if name == "text":
                    # round302: the sentence's slot is boot id modulo 12, so a run of them is not
                    # contiguous -- boot 92 sits in sector 44+92%12 and says nothing about the eleven
                    # neighbours. An empty sector here is an untouched slot, not the end of a ring.
                    continue
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
                problems.append(f"event {words[6]} is not a {FLIGHT_EVENTS[event]}")
            # Round302, read out of boot 91's frame: this sector holds a panic-event record whose
            # eight marker words were never written, because flightWrite rebuilds ONE shared buffer
            # and a block request can cut into the sink's own transfer. Such a sector is not a
            # marker: the stamp (extra[6]) and the gates (extra[7]) are both zero while the header
            # still carries a live request (blocks != 0, stage == kStageEntry, a pid and a name).
            # Printing it as a marker invents a `stage=91` out of the round293 boot stamp, which is
            # exactly the reading-shaped bug this file exists to refuse.
            magic_word = FLIGHT_MAGIC_WORD.get(words[6])
            magic = words[magic_word] == FLIGHT_MARKER_MAGIC if magic_word else None
            if magic is False:
                # The signature is one word past the extras, so a marker whose sector signed at an
                # older position is an older payload's marker -- not an unsigned sector. Try those
                # positions before calling it unsigned, and keep which one signed for the printer.
                for fallback in FLIGHT_MAGIC_FALLBACK.get(words[6], ()):
                    if words[fallback] == FLIGHT_MARKER_MAGIC:
                        magic, magic_word = True, fallback
                        break
            if words[6] == 6 and magic is False:
                # The event only exists from round302, so a sector here without the signature was
                # never a sentence: it is an empty sector or somebody else's record.
                problems.append("not a sentence: no r302 magic at word "
                                f"{magic_word} (boot={words[37]}, chars={words[35]})")
            if (event == 5 and words[35 + 6] == 0 and words[35 + 7] == 0
                    and words[35 + 1] == 0 and words[35 + 3] == 0):
                comm = struct.pack("<2I", words[33], words[34]).rstrip(b"\0").decode("latin1")
                problems.append(
                    "not a marker: its eight extras were never written (stamp 0, gates 0) "
                    f"while the header names a live request -- blocks={words[9]}, "
                    f"stage={words[22]}, pid={words[30] or words[32]}, comm={comm or '-'}, "
                    f"t={words[10]} ms")
            record = {
                "slot": slot, "lba": words[2], "seq": words[4], "event": words[6],
                "dir": words[7], "op_lba": words[8], "op_blocks": words[9],
                "t_ms": words[10], "prev_done": words[11], "prev_ms": words[12],
                "present": words[13], "flags": words[14], "ops": words[15],
                "guard_waits": words[16], "beat_misses": words[17],
                "pending_kernel": words[18], "pending_driver": words[19],
                "prev_dir": words[20], "prev_blocks": words[21],
                "stage": words[22], "stage_ms": words[23],
                "stuck": words[24], "recovered": words[25], "steals": words[26],
                "stuck_stage": words[27], "stage_lba": words[28], "stage_blocks": words[29],
                "stage_pid": words[30], "stage_dir": words[31],
                "cur_pid": words[32], "comm0": words[33], "comm1": words[34],
                "boot": words[FLIGHT_BOOT_WORD[words[6]]] if words[6] in FLIGHT_BOOT_WORD else 0,
                "magic": magic,
                "magic_at": magic_word if magic else None,
                "extra": words[35:],
            }
            if words[6] == 6 and magic:
                raw = struct.pack(f"<{FLIGHT_TEXT_BYTES // 4}I", *words[38:38 + FLIGHT_TEXT_BYTES // 4])
                record["chars"] = words[35]
                record["append_rc"] = words[36]
                record["total"] = words[9]
                record["text"] = raw.split(b"\0", 1)[0].decode("latin1")
            records.append((slot, record, problems))
        groups[name] = records
    return groups


def flight_pairs(words, count):
    """Round236's extras: `count` little-endian 64-bit values from the first word on."""
    return [words[2 * index] | (words[2 * index + 1] << 32) for index in range(count)]


def flight_pairs_words(values):
    """The inverse of flight_pairs, for selftest."""
    words = []
    for value in values:
        words.extend((value & 0xFFFFFFFF, value >> 32))
    return words


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


def flight_comm(record):
    """The caller's name, first eight bytes as flightEnter stores them."""
    raw = struct.pack("<II", record["comm0"], record["comm1"])
    return raw.split(b"\0", 1)[0].decode("ascii", "replace") or "-"


def r305_own(record, boot_id):
    """Whether this record was written by the payload whose layout the reader is using.

    One boot is one image, so a record that names the boot being read carries round305's extras; a
    record left by an earlier boot of an older payload does not, and its kernel second, starvation
    exits and refusal words are whatever the previous layout kept at those word numbers. Boot 95's
    frame is the case in point: the boot 94 marker left there decodes to k=1129076301s, a number no
    clock produces, and printing it as an uptime would be exactly the round293 class of defect this
    reader was written to stop.
    """
    return bool(boot_id) and record["boot"] == boot_id


def flight_stamp(records, boot_id):
    """One line saying whose record the newest of this group is.

    boot_id 0 in a record means the payload that wrote it stamped nothing, which is a different
    statement from a stamp that names another boot: the first is this reader being older than the
    field, the second is this boot never writing the group at all.
    """
    if not records:
        return "flight: no records in this group"
    newest = records[-1]
    if newest["boot"] == 0:
        return f"flight: {len(records)} records, writer unstamped (before r293)"
    if boot_id and newest["boot"] != boot_id:
        return (f"flight: {len(records)} records, newest written by boot {newest['boot']} -- STALE: "
                f"boot {boot_id} never wrote one, so this group says nothing about this boot")
    return f"flight: {len(records)} records, all written by boot {newest['boot']}"


def flight_report(groups, boot_id=0):
    """Print the four chains and the one conclusion this round is built to support.

    The last entry record names a request whose result word belongs to the request
    *before* it, so the newest entry is the one with no successor: the request in
    flight when the writing stopped. The newest beat's time bounds when the guard and
    the controller were still accepting commands, and a gap between the two is where
    the stop happened. Round236's deadline record is the one record that says the
    deadline itself fired when the sink could not, and the panic markers say whether
    the sink ran before the deadline or after it. Round293 adds the boot each of those
    was written by, because a deadline record from an earlier boot is not evidence that
    this boot reached its deadline.
    """
    entries = flight_chain(groups["entries"])
    beats = flight_chain(groups["beats"])
    deadline = flight_chain(groups["deadline"])
    panic = flight_chain(groups["panic"])
    # Newest by the writer's own boot id and clock, because these slots do not come in sequence.
    text = sorted([record for slot, record, problems in groups["text"]
                   if not problems and record["magic"]],
                  key=lambda record: (record["boot"], record["t_ms"]))
    written = sum(len(groups[name])
                  for name in ("entries", "beats", "deadline", "panic", "drain", "text"))
    if entries:
        span = f"seq {entries[0]['seq']}..{entries[-1]['seq']}"
    else:
        span = "none"
    print(f"flight: {len(entries)} entry records in the window ({span})")
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
              f"{record['prev_ms']}ms/{record['prev_done']} "
              f"stage={FLIGHT_STAGES.get(record['stage'], '?')}({record['stage']})"
              f"/{record['stage_ms']}ms pid={record['cur_pid']} "
              f"comm={flight_comm(record)} stuck={record['stuck']} "
              f"recov={record['recovered']} steal={record['steals']}")
    print(flight_stamp(entries, boot_id))
    print(f"flight: {len(beats)} beat records in the window")
    for record in beats:
        flags = []
        if record["flags"] & 1:
            flags.append("cmd")
        if record["flags"] & 2:
            flags.append("dat")
        # Round305's twelve extras: the kernel's own second, the four starvation exits (blind, panic,
        # young, held) counted since the ring was armed -- they are never cleared, because the block
        # tail's probe line reports the same four and the consumer that can be starved must not decide
        # what the other one sees -- and the last refused block write since arming: code, INT_STATUS,
        # words handed, its flight ms, which channel asked and which block, plus the running total.
        # The round293 stamp follows at word 47.
        (ksecs, blind, panicked, young, held,
         code, fail_status, fail_words, fail_ms, fail_channel, fail_lba, fail_total) = \
            record["extra"][:12]
        # The one number v0.7.14 withdrew because it had never been measured: kernel seconds minus the
        # driver's milliseconds, taken in one instruction stream. It is the arming offset, and from this
        # round every panic marker carries its own copy, so a marker no longer needs anybody else's clock.
        offset = ksecs * 1000 - record["t_ms"] if ksecs else 0
        # A record left by an older payload has no kernel second at word 35 and no starvation exits
        # after it, so those words get named as unreadable rather than printed as readings -- boot 95's
        # window holds boot 94's beat and marker records, and the reader is the one place that knows
        # which payload wrote them.
        clock_pair = (f"k={ksecs}s arming_offset={offset}ms "
                      f"starve={blind}/{panicked}/{young}/{held} "
                      f"pio={code}/{fail_status:#010x}/{fail_words}w@{fail_ms}ms "
                      f"ch={fail_channel} lba={fail_lba} total={fail_total} "
                      if r305_own(record, boot_id) else
                      "k=? starve=? pio=? (clocks unreadable: an older payload's words are here) ")
        print(f"  beat  seq {record['seq']:>3} slot {record['slot']:>2} t={record['t_ms']:>8}ms "
              f"{clock_pair}"
              f"ps={record['present']:#010x} inh={','.join(flags) or '-'} "
              f"ops={record['ops']} guard={record['guard_waits']} miss={record['beat_misses']} "
              f"pend={record['pending_kernel']}/{record['pending_driver']} "
              f"stage={FLIGHT_STAGES.get(record['stage'], '?')}({record['stage']})"
              f"/{record['stage_ms']}ms "
              f"stage_lba={record['stage_lba']} stage_blocks={record['stage_blocks']} "
              f"stage_pid={record['stage_pid']} "
              f"stuck={record['stuck']} recov={record['recovered']} steal={record['steals']} "
              f"stuck_stage={FLIGHT_STAGES.get(record['stuck_stage'], '?')}")
    print(flight_stamp(beats, boot_id))
    if entries and beats:
        if boot_id and (entries[-1]["boot"] not in (0, boot_id) or
                        beats[-1]["boot"] not in (0, boot_id)):
            print("flight: entry and beat chains name different boots -- no gap to read")
        else:
            gap = entries[-1]["t_ms"] - beats[-1]["t_ms"]
            print(f"flight: newest entry seq {entries[-1]['seq']} at t={entries[-1]['t_ms']} ms, "
                  f"newest beat seq {beats[-1]['seq']} at t={beats[-1]['t_ms']} ms, gap {gap} ms")
    elif entries:
        print("flight: entries without any beat -- no beat ever found the guard free")
    elif beats:
        print("flight: beats without any entry -- no block request started after arming")
    # Round236: the deadline record is written from the timer interrupt ahead of panic(), so it
    # exists on a boot whose sink never reached the card. The panic markers say what the sink
    # itself managed: entry with the gates, and exit with the append rc or nothing at all.
    # Round293: both readings are only about the boot that wrote them, which is what the stamp says.
    if deadline:
        record = deadline[-1]
        seconds, ticks, cnt0, cnt1, freq = flight_pairs(record["extra"], 5)
        period = ((cnt1 - cnt0) / freq) if freq and cnt1 > cnt0 else 0.0
        if boot_id and record["boot"] and record["boot"] != boot_id:
            print(f"flight: the deadline record on the card is boot {record['boot']}'s, not boot "
                  f"{boot_id}'s -- this boot never wrote one, so it either did not reach its "
                  f"deadline or could not write the record (t={record['t_ms']} ms, "
                  f"seconds={seconds}, ticks={ticks})")
        else:
            caveat = ("" if record["boot"] else
                      " (un-stamped: a payload before r293 wrote this, so the panic markers' "
                      "boot ids are the only attribution available)")
            print(f"flight: deadline fired after {seconds} s at t={record['t_ms']} ms "
                  f"boot={record['boot'] or '?'} ps={record['present']:#010x} ticks={ticks} "
                  f"cnt0={cnt0:#x} cnt1={cnt1:#x} freq={freq} elapsed={period:.3f}s{caveat}")
    else:
        print("flight: no deadline record -- the deadline either did not fire or could not write")
    rejected = [(slot, record, problems) for slot, record, problems in groups["panic"] if problems]
    if rejected:
        # Round302: a sector the chain cannot trust is still a reading, and it is the reading that
        # says the sink's marker was mixed with somebody else's record. Say it, and say whose request
        # is in the same sector, because that is the whole content of the frame.
        for slot, record, problems in rejected:
            for problem in problems:
                print(f"flight: panic slot {slot} (lba {record['lba']}, seq {record['seq']}, "
                      f"t={record['t_ms']} ms) -- {problem}")
    if panic:
        print(f"flight: {len(panic)} panic marker records in the window")
        for record in panic:
            (stage, log_ready, owner, log_first, rc, drained_never, marker_boot,
             gates) = record["extra"][:8]
            # Round305's five more: this marker's own kernel second, taken in the same instruction
            # stream as its t=, and the last block write that was refused before it. The second is what
            # the v0.7.14 correction asked for -- boot 94's marker could be bounded on the log's clock
            # but never placed on it -- and the refusal splits "the sink's next row never ran" from "it
            # ran and the card said no". `drained_never` is not printed: the entry marker is written
            # before the drain exists, so that field is a constant, not a reading.
            ksecs, fail_code, fail_status, fail_words, fail_channel = record["extra"][8:13]
            if record["boot"] != marker_boot:
                print(f"flight: the panic marker's own stamp and its gBootId word disagree "
                      f"({record['boot']} vs {marker_boot})")
            stale = ("" if not (boot_id and marker_boot and marker_boot != boot_id)
                     else f" STALE: boot {boot_id} never entered the sink")
            offset = ksecs * 1000 - record["t_ms"] if ksecs else 0
            marker_pair = (f"k={ksecs}s arming_offset={offset}ms "
                           f"pio={fail_code}/{fail_status:#010x}/{fail_words}w ch={fail_channel} "
                           if r305_own(record, boot_id) else
                           "k=? pio=? (clocks unreadable: an older payload's words are here) ")
            # round310's five words, and only when this sector signed itself where round310 puts its
            # magic: an older marker's words 48..52 are whatever that layout kept at those numbers, and
            # printing them as cursors is exactly what round305's own gate exists to refuse.
            sink_pair = ""
            if record.get("magic_at") == FLIGHT_MAGIC_WORD[5]:
                sealed, consumed, quiesce, door_stage, door_owner = record["extra"][13:18]
                sink_pair = (f" probe={sealed}/{consumed} pend={sealed - consumed}"
                             f" quiesce={quiesce}({PANIC_QUIESCE.get(quiesce, '?')})"
                             f" door={FLIGHT_STAGES.get(door_stage, '?')}/owner={door_owner:#x}")
            print(f"  panic seq {record['seq']:>3} slot {record['slot']:>2} "
                  f"t={record['t_ms']:>8}ms {marker_pair}"
                  f"stage={stage} log_ready={log_ready} "
                  f"host={owner:#x} log_first={log_first} rc={rc} "
                  f"boot={marker_boot}{stale} magic="
                  f"{'yes' if record['magic'] else 'absent (pre-r302 payload, or a sector that was never a marker)'}"
                  f" gates={gates:#x}"
                  f"({'armed,' if gates & 1 else ''}"
                  f"{'bcb,' if gates & 2 else ''}"
                  f"{'text' if gates & 4 else ''}){sink_pair}")
    else:
        print("flight: no panic marker -- the sink never entered, or its first write refused")
    # round309: the drain's first record, written before panicDrainRings() reads a gate. Boot 99 is the boot that
    # asked for it -- its panic entry marker is on the card and not one of the drain's rows is, which left "the
    # drain needs the host ticket" standing as a candidate. One slot, so the boot stamp and this STALE rule are the
    # whole attribution: a sector that names another boot is that boot's ending, not this one's.
    drain = flight_chain(groups["drain"])
    for slot, record, problems in groups["drain"]:
        for problem in problems:
            print(f"flight: drain-in slot {slot} (lba {record['lba']}, t={record['t_ms']} ms) -- {problem}")
    if drain:
        record = drain[-1]
        (stamp, owner, window, first, drained, append_rc, ksecs, probe_seq) = record["extra"][:8]
        stale = ("" if not (boot_id and stamp and stamp != boot_id)
                 else f" STALE: boot {boot_id} never wrote this sector")
        print(f"flight: drain-in boot={stamp or '?'}{stale} t={record['t_ms']}ms k={ksecs}s "
              f"host={owner:#x} window={window} verdict_first={first} again={drained} "
              f"append_rc={append_rc} probe={probe_seq} seq={record['seq']} "
              f"magic={'yes' if record['magic'] else 'absent (a sector that was never a drain-in)'}")
    else:
        print("flight: no drain-in record -- either the sink stopped before panicDrainRings(), or this "
              "boot's image predates round309; CloverSdhci r309: drain legs in= beside it separates the two "
              "when the ring itself survived")
    # round302: the sentence itself. It lives in its own twelve sectors, is chosen by boot id with no
    # read beforehand, and signs itself, so this line is either the panic's words or a stated absence.
    rejected_text = [(slot, record, problems) for slot, record, problems in groups["text"] if problems]
    for slot, record, problems in rejected_text:
        for problem in problems:
            print(f"flight: sentence slot {slot} (lba {record['lba']}, t={record['t_ms']} ms) -- {problem}")
    newest_text = text[-1] if text else None
    if newest_text is not None:
        truncated = newest_text["total"] > newest_text["chars"]
        print(f"flight: sentence boot={newest_text['boot']} slot={newest_text['slot']} "
              f"t={newest_text['t_ms']} ms chars={newest_text['chars']}/{newest_text['total']} "
              f"append_rc={newest_text['append_rc']}"
              f"{' (the block could not hold the rest)' if truncated else ''}")
        print(f'flight:   "{newest_text["text"]}"')
    else:
        print("flight: no sentence record -- the sink wrote none, or every write of it refused")
    return written


def build_flight_record(lba, seq, event, dir_, op_lba, op_blocks, t_ms=0, present=0,
                        prev_done=0, prev_ms=0, prev_dir=0, prev_blocks=0, ops=0,
                        guard_waits=0, beat_misses=0, pending=(0, 0), slot=None, kind=FLIGHT_KIND,
                        stage=0, stage_ms=0, stuck=0, recovered=0, steals=0, stuck_stage=0,
                        stage_lba=0, stage_blocks=0, stage_pid=0, stage_dir=0,
                        cur_pid=0, comm=(0, 0), extra=(), boot=0, marker_magic=False,
                        magic_at=None):
    """One flight block as flightWrite lays it out, for selftest.

    `boot` goes where round293 says that event's stamp lives, so a selftest that builds a record
    and a decoder that reads one agree on the position without either naming it twice.
    """
    if slot is None:
        slots = {3: FLIGHT_BEAT_SLOTS, 4: FLIGHT_DEADLINE_SLOTS}.get(event, FLIGHT_ENTRY_SLOTS)
        if event == 5:
            slots = FLIGHT_PANIC_SLOTS
        if event == 6:
            slots = FLIGHT_TEXT_SLOTS
        slot = seq % slots
    flags = ((present & 1) and 1 or 0) | ((present & 2) and 2 or 0) | (4 if event == 3 else 0)
    words = [MAGIC[0], MAGIC[1], lba, kind, seq, slot,
             event, dir_, op_lba, op_blocks, t_ms, prev_done, prev_ms,
             present, flags, ops, guard_waits, beat_misses, pending[0],
             pending[1], prev_dir, prev_blocks,
             stage, stage_ms, stuck, recovered, steals, stuck_stage,
             stage_lba, stage_blocks, stage_pid, stage_dir,
             cur_pid, comm[0], comm[1]]
    assert len(words) == FLIGHT_WORDS, len(words)
    words.extend(extra)
    index = FLIGHT_BOOT_WORD.get(event)
    if index is not None:
        while len(words) <= index:
            words.append(0)
        words[index] = boot
    magic_index = FLIGHT_MAGIC_WORD.get(event) if magic_at is None else magic_at
    if marker_magic and magic_index is not None:
        # Written after the extras, exactly as flightWrite does it, so a selftest record and a card
        # record agree that the signature is the last thing the writer put down.
        while len(words) <= magic_index:
            words.append(0)
        words[magic_index] = FLIGHT_MARKER_MAGIC
    assert len(words) < SECTOR // 4, len(words)
    return struct.pack(f"<{len(words)}I", *words).ljust(SECTOR, b"\0")


def build_record(kind, slot, lba, body_text, seq=0, boot=0):
    """One two-block record exactly as the driver lays it out, for selftest."""
    body = body_text.encode("ascii")
    total = len(PREFIX) + len(body) + 1
    checksum = (sum(PREFIX) + sum(body) + ord("\n")) & 0xFFFFFFFF
    header = struct.pack("<10I", MAGIC[0], MAGIC[1], lba, total, checksum, kind,
                         len(body), slot, seq, boot) + bytes(SECTOR - 40)
    text = PREFIX + body + b"\n"
    return header + text.ljust(SECTOR, b"\0")


def build_boot(lba, boot, slot=0, seq=0, kind=BOOT_KIND):
    """One round212 boot block exactly as bootIdClaim lays it out, for selftest."""
    return struct.pack("<10I", MAGIC[0], MAGIC[1], lba, 0, 0, kind, 0, slot, seq,
                       boot).ljust(SECTOR, b"\0")


def joined_literal(source, anchor):
    """Round330. The contents of the C string literal that starts at `anchor`, with every adjacent
    literal folded in the way the compiler folds them, and the two escapes this file's row formats use
    turned back into characters. A probe's row format is written as two literals because the line is
    too wide for this repo's style, so a test that pinned one half would be a test that passes while
    the shipped row renames a column in the other half."""
    index = source.index(anchor)
    parts = []
    while source[index] == '"':
        end = index + 1
        while source[end] != '"':
            end += 2 if source[end] == "\\" else 1
        parts.append(source[index + 1:end])
        index = end + 1
        while index < len(source) and source[index] in " \t\n":
            index += 1
    return "".join(parts).replace("\\n", "\n").replace("\\t", "\t")


# One sample per conversion, cycled by which occurrence of that conversion it is. The %s values are in
# the order the row's columns come: a client name, then the counters, the hot-selector triple, the
# selector-6 argument quad and the three object words. They stand in for readings of the shape boot 113
# reported for the same quantities; they are not a boot reading, and nothing here is judged on them --
# the reader is judged on round-tripping a row built from the shipped format string.
SAMPLE_VALUES = {"%llu": ["512345"], "%llx": ["1", "4", "80000001"], "%lld": ["2"],
                 "%08x": ["00000000"], "%u": ["18", "1", "7", "96", "0"], "%d": ["333"],
                 "%x": ["4"],
                 "%s": ["pid58", "41", "3", "1", "1", "2", "6:20/0/00000000",
                        "1 4 80000001 -> 00000000", "4", "4", "5"]}


def fill_conversions(fmt):
    """Round330. A row this probe could print, built from the probe's own format string. Conversions
    are filled left to right in one pass, so a wider conversion is never eaten as a shorter one."""
    out = []
    index = 0
    seen = {}
    while index < len(fmt):
        if fmt[index] != "%":
            out.append(fmt[index])
            index += 1
            continue
        spec = re.match(r"%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|q|z)?[diouxXfeEgGcsA]", fmt[index:]).group(0)
        values = SAMPLE_VALUES.get(spec)
        if values is None:
            raise AssertionError(f"no sample value for {spec} in {fmt!r}")
        position = seen.get(spec, 0)
        seen[spec] = position + 1
        out.append(values[position % len(values)])
        index += len(spec)
    return "".join(out)


def selftest():
    """Round-trip all three windows, including a wrapped ring of each kind. The rings' geometry is
    asserted rather than trusted from a device frame: the driver computes the LBA of every record from
    the same offset and slot count this reader uses, so a geometry that disagrees here is a dump that
    silently reads one ring as the other. Round176 split the two producers apart -- driver verdicts
    into kind 21 at sector 64, kernel text the tee lifted out of msgbuf into kind 22 at sector 192 --
    so the per-ring kind check and the empty-intersection rule (--driver over --tee is zero) are both
    asserted here. Round212 adds two shapes to the same dump: the boot counter at sector 1216, and
    the relayed probe lines (kind 25) that share the driver ring's slots."""
    first_lba = LOG_WINDOW_FIRST_LBA
    boot_id = 7
    # The two rings have to tile the partition with no overlap: the driver ring ends exactly where
    # the kernel ring starts, and a dd of DUMP_SECTORS reaches the boot record one block past the
    # last slot of the second ring.
    assert VERDICT_OFFSET + 2 * VERDICT_SLOTS == KERNEL_OFFSET, KERNEL_OFFSET
    assert BOOT_OFFSET == KERNEL_OFFSET + 2 * KERNEL_SLOTS, BOOT_OFFSET
    assert DUMP_SECTORS == BOOT_OFFSET + 1, DUMP_SECTORS
    # Round210's flight region lives in the gap the two windows leave: report 0..31, entries
    # 32..43, beats 56..59, verdict 64.. -- one block per record, so the whole region fits in
    # the 1216-sector dump both rings already read and no window moved to make room for it.
    assert FLIGHT_ENTRY_OFFSET == 2 * REPORT_SLOTS, FLIGHT_ENTRY_OFFSET
    assert FLIGHT_ENTRY_OFFSET + FLIGHT_ENTRY_SLOTS <= FLIGHT_BEAT_OFFSET, FLIGHT_BEAT_OFFSET
    assert FLIGHT_BEAT_OFFSET + FLIGHT_BEAT_SLOTS <= FLIGHT_DEADLINE_OFFSET, FLIGHT_DEADLINE_OFFSET
    assert FLIGHT_DEADLINE_OFFSET + FLIGHT_DEADLINE_SLOTS <= FLIGHT_PANIC_OFFSET, FLIGHT_PANIC_OFFSET
    assert FLIGHT_PANIC_OFFSET + FLIGHT_PANIC_SLOTS <= FLIGHT_DRAIN_OFFSET, FLIGHT_PANIC_OFFSET
    assert FLIGHT_DRAIN_OFFSET + FLIGHT_DRAIN_SLOTS <= VERDICT_OFFSET, FLIGHT_DRAIN_OFFSET

    dump = bytearray()
    dump += build_record(19, 0, first_lba, "bring-up report", boot=boot_id)
    dump += build_record(20, 1, first_lba + 2, "panic line", boot=boot_id)
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
        dump += build_record(21, slot, lba, line, seq=5 + index, boot=boot_id)
    # Round212's relayed probe line is a kernel line in the driver ring: same slots, kind 25.
    probe_line = "clover r208: sender upc=0000000180001234 uinsn=d503201f spid=71 scomm=backboardd"
    dump += build_record(PROBE_KIND, len(driver_lines),
                         first_lba + VERDICT_OFFSET + 2 * len(driver_lines), probe_line,
                         seq=90, boot=boot_id)
    dump += bytes((KERNEL_OFFSET - VERDICT_OFFSET - 2 * (len(driver_lines) + 1)) * SECTOR)
    kernel_lines = ["load code signature error 4 for file launchd",
                    "[apfs_jhash:146] hash mismatch for 16384 bytes at offset 12128256",
                    "clover r170: check_np pid=12 comm=launchd dyld_flags=0x0",
                    "apfs_mountroot failed: 19"]
    # Round330: round329's display state row is the widest line this port has ever asked the card to
    # carry, and both of its shapes have to be readable -- the row itself and the `dispstatecut` the
    # probe prints when the row would not fit. Neither is retyped here. The row is built from the
    # probe's own format literal, so a column that the probe renames or drops changes this test's line
    # too, and its width budget is read from the same `kDispStateRowBytes` the snprintf writes into.
    # What that buys is the one claim a reader test cannot otherwise make: the reader is checked against
    # the row the shipped source actually produces, not against a row someone remembered.
    probe_path = Path(__file__).resolve().parents[1] / "package/CloverLegacyDisplayProbe.cpp"
    assert probe_path.is_file(), f"{probe_path}: the row this reader is tested against lives there"
    probe_src = probe_path.read_text()
    row_cap = int(re.search(r"kDispStateRowBytes\s*=\s*(\d+)", probe_src).group(1))
    dispstate_row = fill_conversions(joined_literal(probe_src, '"clover r329: dispstate ms='))
    dispstatecut_row = fill_conversions(joined_literal(probe_src, '"clover r329: dispstatecut bytes='))
    for name, line in (("dispstate", dispstate_row), ("dispstatecut", dispstatecut_row)):
        assert "%" not in line, (name, line)
        assert line.endswith("\n"), (name, line)
        assert len(line[:-1].encode()) < row_cap, (name, len(line), row_cap)
        kernel_lines.append(line[:-1])
    # The probe truncates its row at kDispStateRowBytes, so the row that reaches msgbuf is never wider
    # than that number. A record is one block -- prefix, body, newline -- so the two caps compared is the
    # whole of the claim that the card never splits this row: the probe's own ceiling already sits inside
    # the block, whatever the counters happen to print.
    assert row_cap <= SECTOR - len(PREFIX) - 1, (row_cap, SECTOR - len(PREFIX) - 1)
    for index, line in enumerate(kernel_lines):
        slot = index
        lba = first_lba + KERNEL_OFFSET + 2 * slot
        dump += build_record(22, slot, lba, line, seq=17 + index, boot=boot_id)
    assert len(dump) == (KERNEL_OFFSET + 2 * len(kernel_lines)) * SECTOR, len(dump)
    dump += bytes((BOOT_OFFSET - KERNEL_OFFSET - 2 * len(kernel_lines)) * SECTOR)
    dump += build_boot(first_lba + BOOT_OFFSET, boot_id)
    data = bytes(dump)
    assert len(data) == DUMP_SECTORS * SECTOR, len(data)

    reports = decode(data, first_lba)
    assert len(reports) == 2, reports
    assert [r[1]["kind"] for r in reports] == [19, 20], reports
    assert all(not r[3] for r in reports), reports
    assert all(r[1]["boot"] == boot_id for r in reports), reports
    assert text_of(reports[0][2]) == "bring-up report", reports[0]

    def ring(slots, base, expect_kind):
        return sorted(decode(data, first_lba, slots=slots, base_sector=base,
                             expect_kind=expect_kind), key=lambda v: v[1]["seq"])

    # Round212: the driver ring now holds two kinds. Reading it with both accepted is the normal
    # case; reading it with only kind 21 accepted has to name the probe line rather than drop it.
    verdicts = ring(VERDICT_SLOTS, VERDICT_OFFSET, (21, PROBE_KIND))
    assert [text_of(v[2]) for v in verdicts if v[1]["kind"] == 21] == driver_lines, verdicts
    assert [text_of(v[2]) for v in verdicts if v[1]["kind"] == PROBE_KIND] == [probe_line], verdicts
    assert all(not v[3] for v in verdicts), verdicts
    assert all(v[1]["boot"] == boot_id for v in verdicts), verdicts
    assert verdicts[0][1]["seq"] == 5, verdicts[0]
    strict = decode(data, first_lba, slots=VERDICT_SLOTS, base_sector=VERDICT_OFFSET,
                    expect_kind=21)
    assert any(problem.startswith(f"kind {PROBE_KIND} != 21")
               for _, _, _, problems in strict for problem in problems), strict
    # Asking for the relayed probes alone has to drop the verdicts that share the ring,
    # which is what --probe --count does.
    assert of_kind(verdicts, PROBE_KIND) == [v for v in verdicts if v[1]["kind"] == PROBE_KIND], verdicts
    assert len(of_kind(verdicts, PROBE_KIND)) == 1, verdicts
    assert of_kind(verdicts, 21) == [v for v in verdicts if v[1]["kind"] == 21], verdicts

    kernel = ring(KERNEL_SLOTS, KERNEL_OFFSET, 22)
    assert [text_of(v[2]) for v in kernel] == kernel_lines, kernel
    assert all(v[1]["kind"] == 22 for v in kernel), kernel
    assert all(not v[3] for v in kernel), kernel
    assert kernel[0][1]["seq"] == 17, kernel[0]

    # Who said each line, per ring. The driver ring holds the driver's readings plus the tee's state
    # lines about itself; the kernel ring holds kernel text and nothing else, which is the whole point
    # of round176: a busy daemon can no longer evict a verdict, and a --driver filter over --tee that
    # counts anything means the tee started echoing its own lines onto the card. Round212's relayed
    # probe line is the one kernel line that is allowed in the driver ring, and it says so by kind.
    assert all(producer(record[2]) == "driver"
               for record in verdicts if record[1]["kind"] == 21), verdicts
    assert [record for record in verdicts if producer(record[2]) == "kernel"] == \
        [record for record in verdicts if record[1]["kind"] == PROBE_KIND], verdicts
    assert all(producer(record[2]) == "kernel" for record in kernel), kernel
    assert [record for record in kernel if producer(record[2]) == "driver"] == [], kernel
    # A dump with zero kernel lines is still the difference between "the tee ran and nothing matched"
    # and "the tee never ran": the driver ring's tee census answers it.
    assert any("tee seen=" in text_of(record[2]) for record in verdicts), verdicts
    # An unreadable record has no producer to invent.
    assert producer(None) == "-", producer(None)

    # Round212's boot record: one block past the kernel ring, counter in word 9, no text block.
    boot = decode_boot(data, first_lba)
    assert boot is not None and not boot["problems"], boot
    assert boot["boot"] == boot_id, boot
    wrong_boot = bytearray(data)
    wrong_boot[BOOT_OFFSET * SECTOR:(BOOT_OFFSET + 1) * SECTOR] = build_boot(
        first_lba + BOOT_OFFSET, boot_id, kind=21)
    problems = decode_boot(bytes(wrong_boot), first_lba)["problems"]
    assert any(problem.startswith("kind 21 != 24") for problem in problems), problems
    assert decode_boot(bytes(BOOT_OFFSET * SECTOR), first_lba) is None

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
    flight = bytearray((FLIGHT_DRAIN_OFFSET + FLIGHT_DRAIN_SLOTS) * SECTOR)

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
                                  pending=(7, 2),
                                  stage=1 + seq % 11, stage_ms=10 * seq, stuck=seq // 4,
                                  recovered=seq // 5, steals=seq // 6, stuck_stage=seq % 12,
                                  stage_lba=2000 + seq, stage_blocks=seq % 9,
                                  stage_pid=100 + seq, stage_dir=1, cur_pid=300 + seq,
                                  comm=(int.from_bytes(b"apfs", "little"), 0), boot=boot_id),
              FLIGHT_ENTRY_OFFSET + seq % FLIGHT_ENTRY_SLOTS)
    for seq in range(6):
        place(flight,
              build_flight_record(first_lba + FLIGHT_BEAT_OFFSET + seq % FLIGHT_BEAT_SLOTS,
                                  seq, 3, 2, 0, 0, t_ms=1000 + 4000 * seq, present=0,
                                  ops=seq, guard_waits=1, beat_misses=seq, pending=(9, 1),
                                  boot=boot_id),
              FLIGHT_BEAT_OFFSET + seq % FLIGHT_BEAT_SLOTS)
    # Round236: one deadline record with its five 64-bit numbers, and the sink's two markers
    # with the entry gates and the append's return code. The deadline's dir word is 3 ("none")
    # because the record is not a sector request, and the decoder has to accept it as such.
    deadline_values = (1800, 108000, 0x1000, 0x2000, 19200000)
    place(flight,
          build_flight_record(first_lba + FLIGHT_DEADLINE_OFFSET, 0, 4, 3, 0, 0,
                              t_ms=1800000, present=0, stage=12, stage_ms=79000,
                              extra=flight_pairs_words(deadline_values), boot=boot_id),
          FLIGHT_DEADLINE_OFFSET)
    panic_entry = (1, 1, 0x77, 1, 103, 3, boot_id, 7)
    panic_exit = (1, 1, 0x77, 1, 2, 3, boot_id, 7)
    place(flight,
          build_flight_record(first_lba + FLIGHT_PANIC_OFFSET, 0, 5, 3, 0, 0,
                              t_ms=1800100, present=0, extra=panic_entry, boot=boot_id),
          FLIGHT_PANIC_OFFSET)
    place(flight,
          build_flight_record(first_lba + FLIGHT_PANIC_OFFSET + 1, 1, 5, 3, 0, 0,
                              t_ms=1800200, present=0, extra=panic_exit, boot=boot_id),
          FLIGHT_PANIC_OFFSET + 1)
    # round309: the drain's own first record, on its one sector, with the ticket the marker only showed
    # afterwards. The held host (0x20fdf is boot 90's) is on purpose: this record exists to say what the
    # gate saw, and a selftest that built it with a free ticket would not prove the column is read.
    drain_in = (boot_id, 0x20fdf, 1, 1, 0, 2, 1800, 37)
    place(flight,
          build_flight_record(first_lba + FLIGHT_DRAIN_OFFSET, 0, 7, 3, 0, 0,
                              t_ms=1800300, present=0, extra=drain_in, boot=boot_id,
                              marker_magic=True),
          FLIGHT_DRAIN_OFFSET)
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
    # Round213's words 22..34: a misordered pair would print a plausible table of the wrong
    # columns, and the stage name is the whole reading, so every new field is round-tripped.
    assert (newest["stage"], newest["stage_ms"], newest["stuck"], newest["recovered"],
            newest["steals"], newest["stuck_stage"], newest["stage_lba"],
            newest["stage_blocks"], newest["stage_pid"], newest["stage_dir"],
            newest["cur_pid"]) == (3, 130, 3, 2, 2, 1, 2013, 4, 113, 1, 313), newest
    assert flight_comm(newest) == "apfs", newest
    assert all(not problems for _, _, problems in groups["entries"] + groups["beats"])
    # Round236's two record kinds: the deadline's 64-bit numbers and the panic marker's gate
    # words round-trip through the same decoder, so a swapped pair would print a plausible
    # deadline that never happened.
    deadline_chain = flight_chain(groups["deadline"])
    assert [record["seq"] for record in deadline_chain] == [0], deadline_chain
    assert deadline_chain[0]["event"] == 4, deadline_chain[0]
    assert deadline_chain[0]["dir"] == 3, deadline_chain[0]
    assert flight_pairs(deadline_chain[0]["extra"], 5) == list(deadline_values), deadline_chain[0]
    assert (deadline_chain[0]["stage"], deadline_chain[0]["stage_ms"]) == (12, 79000), deadline_chain[0]
    panic_chain = flight_chain(groups["panic"])
    assert [record["seq"] for record in panic_chain] == [0, 1], panic_chain
    assert tuple(panic_chain[0]["extra"][:8]) == panic_entry, panic_chain[0]
    assert tuple(panic_chain[1]["extra"][:8]) == panic_exit, panic_chain[1]
    assert all(not problems for _, _, problems in groups["deadline"] + groups["panic"])
    # round309: the drain-in sector. Its eight words and its stamp are the reading, so a decoder that
    # disagrees about where the magic sits would print a sector that was never written as one.
    drain_chain = flight_chain(groups["drain"])
    assert [record["seq"] for record in drain_chain] == [0], drain_chain
    assert drain_chain[0]["event"] == 7 and drain_chain[0]["dir"] == 3, drain_chain[0]
    assert tuple(drain_chain[0]["extra"][:8]) == drain_in, drain_chain[0]
    assert drain_chain[0]["boot"] == boot_id, drain_chain[0]
    assert drain_chain[0]["magic"], drain_chain[0]
    assert all(not problems for _, _, problems in groups["drain"])
    # Round293's stamp: the boot that wrote each record, at the word flightWrite puts it in. The
    # entries and beats have no extras so theirs is word 35, the deadline's follows its ten
    # numbers at 45, and the panic marker's is the same word its own gBootId extra already names.
    assert all(record["boot"] == boot_id for record in entries + beats), (entries[-1], beats[-1])
    assert deadline_chain[0]["boot"] == boot_id, deadline_chain[0]
    assert all(record["boot"] == boot_id for record in panic_chain), panic_chain
    assert flight_report(groups, boot_id) == sum(len(groups[name]) for name in
                                                 ("entries", "beats", "deadline", "panic", "drain"))
    # ... and once it says so, the sector that carries another boot's stamp must not read as this boot's ending.
    foreign_drain = bytearray(flight)
    place(foreign_drain,
          build_flight_record(first_lba + FLIGHT_DRAIN_OFFSET, 0, 7, 3, 0, 0,
                              t_ms=1800300, present=0, extra=drain_in, boot=boot_id - 1),
          FLIGHT_DRAIN_OFFSET)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decode_flight(bytes(foreign_drain), first_lba), boot_id)
    said = out.getvalue()
    assert (f"flight: drain-in boot={boot_id - 1} STALE: boot {boot_id} never wrote this sector"
            in said), said

    # A previous boot's deadline and panic slots stay on the card when this boot never reaches
    # them, which is exactly what boot 79's frame did: this reader printed "the deadline fired
    # after 1800 s" for a record boot 76 wrote. A window whose boot record disagrees has to say so.
    foreign = bytearray(flight)
    foreign_deadline = flight_pairs_words(deadline_values)
    place(foreign,
          build_flight_record(first_lba + FLIGHT_DEADLINE_OFFSET, 0, 4, 3, 0, 0,
                              t_ms=1800000, present=0, stage=12, stage_ms=79000,
                              extra=foreign_deadline, boot=boot_id - 1),
          FLIGHT_DEADLINE_OFFSET)
    for slot, seq in ((0, 0), (1, 1)):
        marker = (1, 1, 0x77, 1, 103 if slot == 0 else 2, 3, boot_id - 1, 7)
        place(foreign,
              build_flight_record(first_lba + FLIGHT_PANIC_OFFSET + slot, seq, 5, 3, 0, 0,
                                  t_ms=1800100 + 100 * slot, present=0, extra=marker,
                                  boot=boot_id - 1),
              FLIGHT_PANIC_OFFSET + slot)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decode_flight(bytes(foreign), first_lba), boot_id)
    said = out.getvalue()
    assert (f"the deadline record on the card is boot {boot_id - 1}'s, not boot {boot_id}'s"
            in said), said
    assert f"STALE: boot {boot_id} never entered the sink" in said, said
    assert "deadline fired" not in said, said
    # ... and the same slots, once this boot writes them, read as this boot's again.
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(groups, boot_id)
    assert "deadline fired after 1800 s" in out.getvalue(), out.getvalue()

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

    # Round302, read out of boots 88 and 91: a panic slot can hold a record whose event and dir
    # words are the marker's while its header names a live block request and its eight extras were
    # never written -- flightWrite rebuilds one shared buffer, so a request can cut into the sink's
    # transfer. Such a slot must be named as not-a-marker: the alternative prints the round293 boot
    # stamp as `stage=`, which is a number with no meaning and looks exactly like a reading.
    mixed = bytearray(flight)
    chimera = bytearray(build_flight_record(first_lba + FLIGHT_PANIC_OFFSET, 0, 5, 3, 0, 8,
                                            t_ms=631323, stage=1, stage_lba=29044832,
                                            stage_blocks=8, stage_pid=195, cur_pid=195,
                                            comm=(0x6e61736f, 0x74796c61), boot=0))
    struct.pack_into("<I", chimera, 35 * 4, boot_id)   # the no-extras stamp lands in word 35
    place(mixed, bytes(chimera), FLIGHT_PANIC_OFFSET)
    decoded = decode_flight(bytes(mixed), first_lba)
    problems = decoded["panic"][0][2]
    assert any(problem.startswith("not a marker") for problem in problems), problems
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decoded, boot_id)
    said = out.getvalue()
    assert "panic slot 0 (lba 13320277" not in said or "not a marker" in said, said
    assert "not a marker: its eight extras were never written" in said, said
    assert "osanalyt" in said, said
    # ... and it is not a marker, so no line may claim a stage for it.
    assert not any(line.strip().startswith("panic seq") and "stage=91" in line
                   for line in said.splitlines()), said
    # Round302's own half: a verdict record that owns its buffer and signs itself. Positive control
    # first -- a marker and a sentence that carry the magic must read as what they are, with the
    # sentence's words on the card, and the sector must never be called a hybrid.
    signed = bytearray(flight)
    sentence = b"panic(cpu 3 caller 0xffffff80deadbeef): clover unattended deadline"
    words = list(sentence)
    words += [0] * (-len(words) % 4)
    text_words = [int.from_bytes(bytes(words[i:i + 4]), "little")
                  for i in range(0, len(words), 4)]
    text_words += [0] * (FLIGHT_TEXT_BYTES // 4 - len(text_words))
    place(signed,
          build_flight_record(first_lba + FLIGHT_TEXT_OFFSET + boot_id % FLIGHT_TEXT_SLOTS,
                              boot_id, 6, 3, 0, len(sentence), t_ms=642000,
                              extra=[len(sentence), 2, boot_id] + text_words,
                              boot=boot_id, marker_magic=True),
          FLIGHT_TEXT_OFFSET + boot_id % FLIGHT_TEXT_SLOTS)
    place(signed,
          build_flight_record(first_lba + FLIGHT_PANIC_OFFSET, 0, 5, 3, 0, 0, t_ms=641900,
                              extra=[0, 1, 0x20fdf, 13320216, 2, 0, boot_id, 7],
                              boot=boot_id, marker_magic=True, magic_at=43),
          FLIGHT_PANIC_OFFSET)
    decoded = decode_flight(bytes(signed), first_lba)
    assert not decoded["text"][0][2], decoded["text"][0][2]
    assert decoded["text"][0][1]["text"] == sentence.decode("latin1"), decoded["text"][0][1]
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decoded, boot_id)
    said = out.getvalue()
    assert "not a marker" not in said, said
    assert "magic=yes" in said, said
    assert sentence.decode("latin1") in said, said
    assert f"sentence boot={boot_id}" in said, said
    # The same sector without the signature is not a sentence, and must not print quoted words:
    # this is the rule that keeps a rebuilt shared buffer from ever being read as a verdict again.
    unsigned = bytearray(signed)
    struct.pack_into("<I", unsigned, (FLIGHT_TEXT_OFFSET + boot_id % FLIGHT_TEXT_SLOTS) * SECTOR
                     + 126 * 4, 0)
    decoded = decode_flight(bytes(unsigned), first_lba)
    assert any(problem.startswith("not a sentence") for problem in decoded["text"][0][2]), \
        decoded["text"][0][2]
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decoded, boot_id)
    said = out.getvalue()
    assert "not a sentence: no r302 magic at word 126" in said, said
    assert "panic(cpu 3 caller" not in said, said
    # Round305: the two clocks in one record, the starvation counters and the refusal that names itself.
    # The geometry is the part worth asserting -- the beat's boot id moved from word 35 to word 47 the
    # round its kernel second arrived at word 35, and a reader that kept looking at 35 would print 741 as
    # "written by boot 741", which is the round293 defect re-made by this round's own change.
    both = bytearray(flight)
    beat_extra = [741, 0, 3, 5, 12, 2, 0x00000011, 7, 640500, 125, 13320256, 4, boot_id]
    place(both,
          build_flight_record(first_lba + FLIGHT_BEAT_OFFSET, 8, 3, 2, 0, 0, t_ms=729750,
                              extra=beat_extra, boot=boot_id),
          FLIGHT_BEAT_OFFSET)
    place(both,
          build_flight_record(first_lba + FLIGHT_PANIC_OFFSET, 0, 5, 3, 0, 0, t_ms=729900,
                              extra=[1, 1, 0, 13320216, 100, 0, boot_id, 7,
                                     741, 2, 0x00000011, 7, 103],
                              boot=boot_id, marker_magic=True, magic_at=48),
          FLIGHT_PANIC_OFFSET)
    decoded = decode_flight(bytes(both), first_lba)
    beat = decoded["beats"][0][1]
    assert not decoded["beats"][0][2], decoded["beats"][0][2]
    assert beat["boot"] == boot_id and beat["extra"][0] == 741, beat
    assert beat["extra"][1:5] == (0, 3, 5, 12), beat
    marker = decoded["panic"][0][1]
    assert marker["magic"] and marker["extra"][8] == 741 and marker["extra"][12] == 103, marker
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decoded, boot_id)
    said = out.getvalue()
    assert "k=741s arming_offset=11250ms" in said, said
    assert "starve=0/3/5/12" in said, said
    assert "pio=2/0x00000011/7w@640500ms ch=125 lba=13320256 total=4" in said, said
    assert "drained=" not in said, said
    assert "k=741s arming_offset=11100ms" in said, said
    # Round310 grows this record by five words, and the signature always sits one past the last of them,
    # so one sector has carried it at 43, at 48 and now at 53. Boot 102's own marker is the middle case:
    # it must still read as a marker (that is the fallback, and a reader that only knew 53 would call the
    # sink silent on a frame that signed itself), while the new columns stay shut, because words 48..52 of
    # an r309 marker are its kernel second and its refusal words -- not cursors.
    assert marker["magic"] and marker["magic_at"] == 48, marker
    r309_line = next(line for line in said.splitlines() if "panic seq" in line and "magic=yes" in line)
    assert "pend=" not in r309_line, r309_line
    sink310 = bytearray(flight)
    place(sink310,
          build_flight_record(first_lba + FLIGHT_PANIC_OFFSET, 0, 5, 3, 0, 0, t_ms=569031,
                              extra=[0, 1, 0x28c1d, 13320216, 0, 0, boot_id, 7,
                                     581, 0, 0, 0, 0, 71, 69, 1, 1, 0x28c1d],
                              boot=boot_id, marker_magic=True),
          FLIGHT_PANIC_OFFSET)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decode_flight(bytes(sink310), first_lba), boot_id)
    said = out.getvalue()
    assert "magic=yes" in said, said
    # Seventy-one rows sealed into the relay, sixty-nine taken by a flush: the two that boot 102 left
    # unanswerable, in the one record the sink is known to land.
    assert "probe=71/69 pend=2" in said, said
    assert "quiesce=1(took the ticket, card back)" in said, said
    assert "door=entry/owner=0x28c1d" in said, said
    unsigned310 = bytearray(sink310)
    struct.pack_into("<I", unsigned310, FLIGHT_PANIC_OFFSET * SECTOR + 53 * 4, 0)
    assert not decode_flight(bytes(unsigned310), first_lba)["panic"][0][1]["magic"]
    # A beat from a payload before this round has zeros where the pair now lives. That reads as no
    # measurement, not as an arming offset of the whole uptime: the kernel second must stay out of the
    # arithmetic when it was never taken.
    old = bytearray(flight)
    place(old, build_flight_record(first_lba + FLIGHT_BEAT_OFFSET, 0, 3, 2, 0, 0, t_ms=9000,
                                   boot=boot_id), FLIGHT_BEAT_OFFSET)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decode_flight(bytes(old), first_lba), boot_id)
    assert "k=0s arming_offset=0ms" in out.getvalue(), out.getvalue()
    # ... and the case boot 95 actually produced: a record left in the region by an older payload, read
    # through this reader's tables. Its word 35 is not a kernel second and its word 43 is not a refusal
    # code, so the frame has to say it cannot read them -- boot 94's marker in boot 95's window decoded
    # to k=1129076301s, a number no clock reaches, and printing that as an uptime is the round293 defect
    # wearing this round's field names.
    foreign = boot_id - 1
    mixed = bytearray(flight)
    place(mixed,
          build_flight_record(first_lba + FLIGHT_BEAT_OFFSET, 9, 3, 2, 0, 0, t_ms=729750,
                              extra=[1129076301, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, foreign],
                              boot=foreign),
          FLIGHT_BEAT_OFFSET)
    place(mixed,
          build_flight_record(first_lba + FLIGHT_PANIC_OFFSET, 0, 5, 3, 0, 0, t_ms=729900,
                              extra=[0, 1, 0x3154c, 13320216, 0, 0, foreign, 7,
                                     1129076301, 0, 0, 0, 0],
                              boot=foreign),
          FLIGHT_PANIC_OFFSET)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        flight_report(decode_flight(bytes(mixed), first_lba), boot_id)
    said = out.getvalue()
    assert "clocks unreadable" in said, said
    assert "1129076301" not in said and "1129075571250" not in said, said
    timeline_selftest()
    print("read-clover-log selftest: report window, both rings, flight region, wrap order, "
          "ring kinds, bad records, the r302 verdict signatures, the r305 clock pair, the r310 "
          "sink-door words and the older-payload clock guard all ok")
    return 0


# Round335's tee-line shape counter. CloverSdhci's tee copies a kernel msgbuf line and
# CloverSdhci's own teeStampSkip() drops a leading "[ <mono>]:" stamp before it hashes the
# producer -- so a line the kernel wrote is supposed to arrive on the card with that stamp at
# its start. Boot 117 proved two of this round's rows did not: an audit text printed without a
# trailing newline glued the next row onto its tail, and the reader who greps by line lost the
# glued one entirely. This is the reader-side half of §Round333 ⑥: it cannot tell WHY a line is
# un-stamped, only that the ring has lost the boundary, and it names at most three heads so the
# shape is checkable against the card rather than asserted from this file.
TEE_STAMP = re.compile(r"^\[\s*\d+\.\d+\]:")


def tee_glue_summary(records):
    """Print `tee lines: N stamped=S glued=G` and up to three un-stamped heads, first 80 bytes.

    The count is over records the tee actually said (kind 22), which main has already filtered.
    A blank body counts as neither: it is a slot the tee left empty, not a row whose boundary
    was eaten.
    """
    stamped = glued = 0
    heads = []
    for _slot, _header, content, _problems in records:
        if content is None:
            continue
        body = text_of(content).lstrip("\x00").strip()
        if not body:
            continue
        if TEE_STAMP.match(body):
            stamped += 1
        else:
            glued += 1
            if len(heads) < 3:
                heads.append(body[:80])
    print(f"tee lines: {stamped + glued} stamped={stamped} glued={glued}")
    for head in heads:
        print(f"  un-stamped head: {head!r}")


TIMELINE_WATCH = re.compile(r"^# watch (\S+) budget=(\S+) opened (\S+)")
TIMELINE_ROW = re.compile(r"^(\d{9,}) (\d+) (\S+) (\S+) (\S+)\s*$")

NEITHER_EDL = (
    "no adb and no fastboot, but the USB tree at the close enumerated a Qualcomm bulk node: the "
    "board reset into download mode, which is a reset this host did see. So this does not leave "
    "open whether the deadline fired or whether the board reset; it leaves open this boot's card "
    "read, which needs the board back in TWRP and does not lose the card")
NEITHER_DIAG = ("no adb and no fastboot, but the tree at the close enumerated a diagnostic port: "
                "the board reset into a mode these two tools do not read")
NEITHER_NO_NODE = ("no adb, no fastboot and no Qualcomm node at the close: whether the deadline "
                   "fired, whether the panic screen is being displayed, and whether the board "
                   "enumerated at all all stay open")
TWRP_NOT_RUNNING = ("either recovery never started or its adbd never started; a board showing "
                    "TWRP's splash with no adb is the second, and that is a host-side USB question")

# The same close token means different things in different watches: `fastboot` in wait-fastboot is
# an arrival (the board is in the bootloader, which is what that wait asks for), while `fastboot`
# in a landing watch is a verdict about the reset -- the board rebooted normally instead of taking
# the recovery command in misc. So the reading is keyed by the watch, and the key it used is
# printed. An unlisted label is counted as `other`, named, and never folded into a known reading.
WATCH_READINGS = {
    # Arrival: was the board ever in this mode during the window.
    "wait-fastboot": {
        "fastboot": "the board enumerated in fastboot during this window",
        "timeout": "neither tool saw the board for the whole budget: it is in a mode fastboot does "
                   "not answer, or off, or hung before the bootloader",
    },
    "wait-adb": {
        "adb": "recovery's adbd came up: the board is in TWRP",
        "timeout": f"no adb within the budget; {TWRP_NOT_RUNNING}",
    },
    # Landing: what the board did after a reset this script asked for.
    "watch-landing": {
        "adb": "the board reset and misc held a recovery command, so it landed in TWRP",
        "fastboot": "the board reset again into fastboot rather than recovery",
        "neither-edl": NEITHER_EDL,
        "neither-diag": NEITHER_DIAG,
        "neither-no-node": NEITHER_NO_NODE,
    },
    "after-deadline": {
        "adb": "the deadline's own reset landed in TWRP: the payload ran and misc held a recovery "
               "command",
        "fastboot": "the board came back to fastboot: it reset, and misc held no recovery command",
        "neither-edl": NEITHER_EDL,
        "neither-diag": NEITHER_DIAG,
        "neither-no-node": NEITHER_NO_NODE,
    },
}
# run-clover-unattended.sh names a watch it knows nothing else about with these three prefixes
# only if it changes; an unknown watch label is reported as unkeyed rather than guessed at.
WATCH_UNKEYED = "no reading registered for this watch/close pair"


def timeline_class_from_tree(tree_lines):
    """Classify a landing from the USB tree the run photographed at its close.

    This is the second leg, not a replacement: runs from before the close line carried a class are
    classified here and said to come from the tree, so a derived reading is never mistaken for one
    the run reported itself.
    """
    text = "\n".join(tree_lines)
    if re.search(r"QUSB_BULK|QPS6000|HS-USB.*9008", text):
        return "edl"
    if re.search(r"(?i)diag|qdss|900e", text):
        return "diag"
    return "no-node"


def timeline_sections(text):
    """Group a timeline file into its watches, keeping the polls that belong to each."""
    watches = []
    current = None
    collecting = None
    for line in text.splitlines():
        header = TIMELINE_WATCH.match(line)
        if header:
            current = {"label": header.group(1), "budget": header.group(2),
                       "opened": header.group(3), "polls": [], "close": None,
                       "open_tree": [], "close_tree": []}
            watches.append(current)
            collecting = None
            continue
        if line.startswith("# usb at open"):
            collecting = "open_tree"
            continue
        if line.startswith("# usb at close"):
            collecting = "close_tree"
            continue
        if line.startswith("#"):
            collecting = None
            continue
        if current is None:
            continue
        row = TIMELINE_ROW.match(line)
        if row:
            # A data row also ends a tree capture: ioreg's output and the next poll are adjacent in
            # the file, so nothing but the row's own shape can tell them apart.
            collecting = None
            _epoch, elapsed, adb, fastboot, event = row.groups()
            if event.startswith("close:"):
                current["close"] = {"elapsed": int(elapsed), "adb": adb, "fastboot": fastboot,
                                    "result": event[len("close:"):]}
            elif event == "poll":
                current["polls"].append((int(elapsed), adb, fastboot))
            else:
                current.setdefault("events", []).append((int(elapsed), event))
            continue
        if collecting:
            current[collecting].append(line)
    return watches


def timeline_selftest():
    """Build a timeline from the writer's own format strings and check what the reader says.

    The samples are not typed out here: they come from the printf literals in
    run-clover-unattended.sh, so a format change on the writer's side is a failure here rather than
    a file that keeps parsing a shape nobody writes any more.
    """
    writer = Path(__file__).resolve().parent / "run-clover-unattended.sh"
    script = writer.read_text(errors="replace")
    formats = re.findall(r"printf '(#[^']*?)'\n?", script) + re.findall(r"printf '([^']*?)'\n?", script)

    def format_of(needle):
        for candidate in formats:
            if needle in candidate:
                return candidate.replace("\\n", "\n")
        raise SystemExit(f"selftest: no printf in {writer} contains {needle!r}")

    open_fmt = format_of("# watch")
    close_fmt = format_of("close:")
    poll_fmt = [candidate for candidate in formats
                if candidate.count("%s") == 5 and "close:" not in candidate
                and not candidate.startswith("#")][0].replace("\\n", "\n")

    text = (open_fmt % ("after-deadline", "2220", "2026-11-07T02:42:13Z")
            + "# epoch elapsed adb fastboot event\n"
            + poll_fmt % ("1794020116", "783", "-", "-", "poll")
            + poll_fmt % ("1794020176", "843", "81ffcaaa", "-", "poll")
            + close_fmt % ("1794020200", "867", "-", "-", "neither-edl")
            + "# usb at close (neither-edl)\n+-o QUSB_BULK_SN:F6548A52@01143000\n"
            + open_fmt % ("wait-adb", "180", "2026-11-07T01:43:01Z")
            + "# epoch elapsed adb fastboot event\n"
            + close_fmt % ("1794010000", "0", "81ffcaaa", "-", "adb")
            + "# usb at close (adb)\n+-o Mi Pad 4@0130000f\n"
            + open_fmt % ("watch-brand-new", "60", "2026-11-07T03:00:00Z")
            + close_fmt % ("1794021000", "5", "-", "-", "neither-no-node")
            + "# usb at close (neither-no-node)\n+-o nothing@01400000\n")

    watches = timeline_sections(text)
    if len(watches) != 3:
        raise SystemExit(f"selftest: timeline parse saw {len(watches)} watches, not 3")
    landing, arrival, unkeyed = watches
    # The tree capture must not eat the rows that follow it: polls are counted, not collected.
    if len(landing["polls"]) != 2 or landing["close"]["result"] != "neither-edl":
        raise SystemExit(f"selftest: the first watch parsed as {landing['polls']} "
                         f"{landing['close']}")
    if arrival["close"]["result"] != "adb":
        raise SystemExit("selftest: the second watch's close was not read as adb")
    counts = {}
    import io
    import contextlib
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        timeline_report_from_text(text, "selftest", counts)
    lines = buffer.getvalue().splitlines()
    if not any("download mode" in line for line in lines):
        raise SystemExit("selftest: neither-edl did not read as a board that reset into download "
                         "mode -- the reading is what this instrument is for")
    if "UNKEYED=['watch-brand-new/neither-no-node']" not in lines[-1]:
        raise SystemExit(f"selftest: an unkeyed watch was not named, it was folded in: {lines[-1]}")
    # A class the writer never reports must come from the tree, and must say it was derived.
    derived_text = text.replace('close:neither-edl', 'close:neither')
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        timeline_report_from_text(derived_text, "selftest", {})
    if "class derived from this run's own" not in buffer.getvalue():
        raise SystemExit("selftest: a classless neither was classified without saying the class "
                         "came from the tree rather than from the run")
    print("timeline selftest: three watches, both close paths, the derived-class note and the "
          "unkeyed name all ok (samples built from run-clover-unattended.sh's own printf)")
    return 0


def timeline_report(path, counts=None):
    """Say what each watch of a host-side run proved, including which watches never closed."""
    with open(path, "r", errors="replace") as handle:
        timeline_report_from_text(handle.read(), str(path), counts if counts is not None else {})


def timeline_report_from_text(text, source, counts):
    watches = timeline_sections(text)
    if not watches:
        print(f"timeline: {source} has no `# watch` section -- nothing was watched, which is a "
              "reading about this run, not about the board")
        return
    for watch in watches:
        close = watch["close"]
        readings = WATCH_READINGS.get(watch["label"], {})
        seen = sorted({e for (_t, adb, fb) in watch["polls"] for e in (adb, fb) if e != "-"})
        if close is None:
            label = "open-no-close"
            reading = ("this watch never wrote its close line, so the run ended inside it -- what "
                       "the board did afterwards is not in this file")
            tail = f"last poll at {watch['polls'][-1][0] if watch['polls'] else 'none'}s"
        else:
            label = close["result"]
            derived = ""
            if label not in readings and label.startswith("neither"):
                # Runs from before the close line carried a class: classify from the tree that run
                # already photographed, and say that is where the class came from.
                label = f"neither-{timeline_class_from_tree(watch['close_tree'])}"
                derived = " (class derived from this run's own `usb at close` tree, not reported by it)"
                reading = readings.get(label, WATCH_UNKEYED)
            else:
                reading = readings.get(label, WATCH_UNKEYED)
            tail = (f"closed with adb={close['adb']} fastboot={close['fastboot']} at "
                    f"{close['elapsed']}s of budget {watch['budget']}{derived}")
        counts[f"{watch['label']}/{label}"] = counts.get(f"{watch['label']}/{label}", 0) + 1
        print(f"watch {watch['label']} opened {watch['opened']} polls={len(watch['polls'])} "
              f"other-answer-seen={','.join(seen) if seen else 'none'} {tail}: {reading}")
    named = " ".join(f"{key}={counts[key]}" for key in sorted(counts)
                     if key.rsplit("/", 1)[1] != "open-no-close"
                     and WATCH_READINGS.get(key.rsplit("/", 1)[0], {})
                     .get(key.rsplit("/", 1)[1]) is not None)
    unkeyed = [key for key in sorted(counts)
               if WATCH_READINGS.get(key.rsplit("/", 1)[0], {})
               .get(key.rsplit("/", 1)[1]) is None]
    print(f"timeline: {len(watches)} watches, closes: {named or 'none'}"
          + (f" UNKEYED={unkeyed}" if unkeyed else ""))


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
                        help="read the driver's verdict ring instead (kinds 21 and 25, "
                             "sectors 64..191)")
    parser.add_argument("--probe", action="store_true",
                        help="read only the round212 probe lines the driver relayed from the "
                             "kernel (kind 25, inside the driver ring)")
    parser.add_argument("--boot", action="store_true",
                        help="print the round212 boot counter record (kind 24, sector 1216)")
    parser.add_argument("--tee", action="store_true",
                        help="read the kernel-log ring the round175 tee writes (kind 22, "
                             "sectors 192..1215)")
    parser.add_argument("--flight", action="store_true",
                        help="read the flight region instead: entry records (kind 23, sectors "
                             "32..43), four-second beats (sectors 56..59), round236's deadline "
                             "record (sector 60) and panic markers (sectors 61..62)")
    parser.add_argument("--driver", action="store_true",
                        help="of the ring being read, only the lines CloverSdhci said itself; over "
                             "--tee this is expected to be empty, and is a check that the tee is not "
                             "echoing")
    parser.add_argument("--count", action="store_true",
                        help="print the number of records the chosen window and filters select, "
                             "and exit (for scripts)")
    parser.add_argument("--newest", action="store_true",
                        help="print only the newest record")
    parser.add_argument("--selftest", action="store_true",
                        help="round-trip all three windows in memory and exit (no card needed)")
    parser.add_argument("--timeline", action="store_true",
                        help="read the host's own timeline instead of the card: the positional is "
                             "the log window path and the timeline is that path plus "
                             ".timeline.txt, or pass the timeline file itself. Each watch is "
                             "reported with its polls, its close label and what that label excludes")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    if args.timeline:
        candidate = Path(args.dump)
        if candidate.suffix == ".txt":
            timeline_path = candidate
        else:
            timeline_path = candidate.parent / f"{candidate.name}.timeline.txt"
        if not timeline_path.is_file():
            raise SystemExit(f"no timeline at {timeline_path}")
        timeline_report(timeline_path)
        return 0

    path = Path(args.dump)
    if not path.is_file():
        raise SystemExit(f"no dump at {path}")
    data = path.read_bytes()
    if len(data) < SLOT_BYTES:
        raise SystemExit(f"{path} is {len(data)} bytes; the first slot alone is {SLOT_BYTES}")
    if args.flight:
        need = (FLIGHT_DRAIN_OFFSET + FLIGHT_DRAIN_SLOTS) * SECTOR
        if len(data) < need:
            raise SystemExit(f"{path} is {len(data)} bytes; the flight region needs {need}")
        groups = decode_flight(data, args.first_lba)
        written = sum(len(groups[name]) for name in
                      ("entries", "beats", "deadline", "panic", "drain"))
        if args.count:
            print(written)
            return 0 if written else 1
        if not written:
            print(f"{path}: no flight records (sector {FLIGHT_ENTRY_OFFSET} has no LOG1 magic)",
                  file=sys.stderr)
            return 1
        # Round293: whose records these are. The boot counter at sector 1216 names this boot, and a
        # record stamped with another boot's id is a leftover, not a reading about this one.
        boot = decode_boot(data, args.first_lba) if len(data) >= DUMP_SECTORS * SECTOR else None
        flight_report(groups, boot["boot"] if boot else 0)
        return 0
    if args.boot:
        need = DUMP_SECTORS * SECTOR
        if len(data) < need:
            raise SystemExit(f"{path} is {len(data)} bytes; the boot record needs {need}")
        boot = decode_boot(data, args.first_lba)
        if boot is None:
            print(f"{path}: no boot record (sector {BOOT_OFFSET} has no LOG1 magic)",
                  file=sys.stderr)
            return 1
        verdict = "ok" if not boot["problems"] else "BAD (" + "; ".join(boot["problems"]) + ")"
        print(f"boot {boot['boot']}  lba {boot['lba']}  kind {BOOT_KIND} "
              f"({KINDS[BOOT_KIND]})  {verdict}")
        return 0
    if args.tee:
        slots, base, kind, window = KERNEL_SLOTS, KERNEL_OFFSET, 22, "kernel-log ring"
    elif args.probe:
        slots, base, kind, window = VERDICT_SLOTS, VERDICT_OFFSET, PROBE_KIND, "probe lines"
    elif args.verdict:
        slots, base, kind, window = (VERDICT_SLOTS, VERDICT_OFFSET, (21, PROBE_KIND),
                                     "verdict ring")
    else:
        slots, base, kind, window = REPORT_SLOTS, 0, None, "report window"
    if kind is not None:
        need = (base + 2 * slots) * SECTOR
        if len(data) < need:
            raise SystemExit(f"{path} is {len(data)} bytes; the {window} needs {need}")
    records = decode(data, args.first_lba, slots=slots, base_sector=base, expect_kind=kind)
    if args.probe:
        records = of_kind(records, PROBE_KIND)
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
    if args.tee:
        tee_glue_summary(records)
    return 0


if __name__ == "__main__":
    sys.exit(main())
