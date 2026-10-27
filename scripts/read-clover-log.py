#!/usr/bin/env python3
"""Decode the clover logfs record windows into readable text.

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

Header words:

  words[0] 0x554e5855  "UXNU"
  words[1] 0x31474f4c  "LOG1"
  words[2] absolute LBA of the header block
  words[3] total bytes of the text block: prefix + body + newline
  words[4] byte sum of those bytes, low 32 bits
  words[5] kind: 17, 19, 20 above, 21 = driver verdict line, 22 = kernel log line
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

Input is a dd of logfs from the card. The default window needs the first 64
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
VERDICT_OFFSET = 64   # sectors past logfs start, clear of the report/panic blocks
KERNEL_SLOTS = 512    # round176's ring: a whole boot of allowlisted kernel text
KERNEL_OFFSET = 192   # 64 + 2 * VERDICT_SLOTS: immediately after the driver's ring
DUMP_SECTORS = KERNEL_OFFSET + 2 * KERNEL_SLOTS   # one dd that reaches the end of both rings
KINDS = {17: "report-17", 19: "report-19", 20: "panic-20",
         21: "verdict-21", 22: "kernel-22"}
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
    first_lba = 12320768
    # The two rings have to tile the partition with no overlap: the driver ring ends exactly where
    # the kernel ring starts, and a dd of DUMP_SECTORS reaches the last slot of the second one.
    assert VERDICT_OFFSET + 2 * VERDICT_SLOTS == KERNEL_OFFSET, KERNEL_OFFSET
    assert DUMP_SECTORS == KERNEL_OFFSET + 2 * KERNEL_SLOTS, DUMP_SECTORS

    dump = bytearray()
    dump += build_record(19, 0, first_lba, "bring-up report")
    dump += build_record(20, 1, first_lba + 2, "panic line")
    dump += bytes((VERDICT_OFFSET - 4) * SECTOR)          # gap before the driver ring: not records
    driver_lines = ["CloverSdhci r174: verdict ring armed lba=12320832 slots=64",
                    "CloverSdhci r176: kernel ring armed lba=12320960 slots=512",
                    "CloverSdhci r166: readcheck lba=12320768 sample=32 trusted=1",
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
    print("read-clover-log selftest: report window, both rings, wrap order, ring kinds and bad "
          "records all ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", nargs="?", default="clover-logfs.bin",
                        help="logfs sectors from the card (default: clover-logfs.bin)")
    parser.add_argument("--first-lba", type=lambda value: int(value, 0), default=12320768,
                        help="logfs first LBA (default 12320768, the card's p39)")
    parser.add_argument("--verdict", action="store_true",
                        help="read the driver's verdict ring instead (kind 21, sectors 64..191)")
    parser.add_argument("--tee", action="store_true",
                        help="read the kernel-log ring the round175 tee writes (kind 22, "
                             "sectors 192..1215)")
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
