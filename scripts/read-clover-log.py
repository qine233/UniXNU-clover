#!/usr/bin/env python3
"""Decode the clover logfs record windows into readable text.

clover has no serial console, so the driver (drivers/CloverSdhci) and the panic
screen keep their text on the card: each record is two 512-byte blocks, a header
and a text block that carries `PREFIX + body + "\\n"`, so the record can also be
read on a machine that has never heard of this format.

Two windows live in the partition:

  report / panic window   sectors 0..31, 16 slots at first + 2k
      kinds 17 (report overwrite), 19 (report append), 20 (panic screen)
  verdict ring            sectors 64..127, 32 slots at first + 64 + 2k
      kind 21, written by round174: the driver's own measurement lines, so a
      reading no longer has to still be on the panel when the photo is taken.
      From round175 the same ring also carries lines the kernel printed --
      printf() output drained out of msgbufp and filtered by allowlist -- so
      the APFS, code-signing, shared-region and exec/exit readings get the
      same after-the-fact treatment. `--tee` shows only those.

Header words:

  words[0] 0x554e5855  "UXNU"
  words[1] 0x31474f4c  "LOG1"
  words[2] absolute LBA of the header block
  words[3] total bytes of the text block: prefix + body + newline
  words[4] byte sum of those bytes, low 32 bits
  words[5] kind: 17, 19, 20 above, 21 = driver verdict line
  words[6] body length
  words[7] slot index
  words[8] sequence number (kind 21 only; 0 in the report/panic records)

The sequence number is what makes a wrapped ring orderable: after the 33rd
verdict, slot index and time diverge, so `--verdict` sorts by seq.

Producer is read off the text, not the header: a line that starts with
"CloverSdhci " was said by the driver itself, anything else in the ring came
from the kernel log. That distinction is the round175 question -- which of the
two is missing tells you whether the tee ran.

Input is a dd of logfs from the card. The default window needs the first 64
sectors; `--verdict` needs 128, which is what scripts/run-clover-unattended.sh
now reads. A record whose header is present but whose text does not sum is still
printed: a half-written record is a reading.
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
VERDICT_SLOTS = 32
VERDICT_OFFSET = 64   # sectors past logfs start, clear of the report/panic blocks
KINDS = {17: "report-17", 19: "report-19", 20: "panic-20", 21: "verdict-21"}
DRIVER_PREFIX = b"CloverSdhci "


def producer(content):
    """"driver" for a line CloverSdhci said, "kernel" for one the round175 tee pulled
    out of msgbuf, "-" for a record with no readable text. The driver's own readings
    always start with its name because sdhciSay is the only way it speaks, so no
    header bit is needed to tell the two apart -- and a kernel line that happens to
    begin with the driver's name would be the driver's line by any useful definition."""
    if content is None:
        return "-"
    return "driver" if content.startswith(DRIVER_PREFIX) else "kernel"


def decode(data, first_lba, slots=REPORT_SLOTS, base_sector=0):
    """[(slot, header dict, text bytes or None, problems [str])] in slot order.

    The writers scan forward and stop at the first header without the magic, so a
    gap ends the window: later slots are not records of this boot.
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
        said = f"  said by {producer(content)}" if header["kind"] == 21 else ""
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
    """Round-trip both windows, including a wrapped verdict ring. The ring is new
    in round174 and its reader is the only thing that turns a boot into a number,
    so the shape is asserted here rather than trusted from a device frame. Round175
    puts a second kind of line in the same ring -- kernel text the tee lifted out of
    msgbuf -- so the producer split and the --tee filter are asserted here too."""
    first_lba = 12320768
    dump = bytearray()
    dump += build_record(19, 0, first_lba, "bring-up report")
    dump += build_record(20, 1, first_lba + 2, "panic line")
    dump += bytes((VERDICT_OFFSET - 4) * SECTOR)          # gap before the ring: not records
    expected = ["CloverSdhci r174: verdict ring armed lba=12320832 slots=32",
                "CloverSdhci r166: readcheck lba=12320768 sample=32 trusted=1",
                "load code signature error 4 for file launchd",
                "CloverSdhci r169: block guard waited 31 ms",
                "[apfs_jhash:146] hash mismatch for 16384 bytes at offset 12128256"]
    for index, line in enumerate(expected):
        # Slots 0..2 with seq 5..7: a ring that wrapped, so slot order is not time order.
        slot = index
        lba = first_lba + VERDICT_OFFSET + 2 * slot
        dump += build_record(21, slot, lba, line, seq=5 + index)
    data = bytes(dump)

    reports = decode(data, first_lba)
    assert len(reports) == 2, reports
    assert [r[1]["kind"] for r in reports] == [19, 20], reports
    assert all(not r[3] for r in reports), reports
    assert text_of(reports[0][2]) == "bring-up report", reports[0]

    verdicts = decode(data, first_lba, slots=VERDICT_SLOTS, base_sector=VERDICT_OFFSET)
    assert len(verdicts) == len(expected), verdicts
    assert all(v[1]["kind"] == 21 for v in verdicts), verdicts
    assert all(not v[3] for v in verdicts), verdicts
    ordered = sorted(verdicts, key=lambda v: v[1]["seq"])
    assert [text_of(v[2]) for v in ordered] == expected, ordered
    assert ordered[0][1]["seq"] == 5, ordered[0]

    # Who said each line. The driver's own readings all begin with its name, so the
    # tee's contribution is exactly the rest -- and a dump with zero kernel lines is
    # the difference between "the tee ran and nothing matched" and "the tee never ran".
    said = [producer(record[2]) for record in ordered]
    assert said == ["driver", "driver", "kernel", "driver", "kernel"], said
    assert [text_of(record[2]) for record in ordered if producer(record[2]) == "kernel"] == \
        [expected[2], expected[4]], said
    # An unreadable record has no producer to invent.
    assert producer(None) == "-", producer(None)

    # A record that does not sum is still printed, with the mismatch named -- and so is a
    # half-written one whose text block lost the prefix, because both are readings.
    for offset, expect in ((SECTOR + len(PREFIX) + 1, "sum"), (SECTOR, "prefix missing")):
        broken = bytearray(bytes(VERDICT_OFFSET * SECTOR) +
                           build_record(21, 0, first_lba + VERDICT_OFFSET, "line", seq=1))
        broken[VERDICT_OFFSET * SECTOR + offset] = ord("X")
        problems = decode(bytes(broken), first_lba,
                          slots=VERDICT_SLOTS, base_sector=VERDICT_OFFSET)[0][3]
        assert any(problem.startswith(expect) for problem in problems), (expect, problems)
    print("read-clover-log selftest: report window, verdict ring, wrap order and bad records all ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", nargs="?", default="clover-logfs.bin",
                        help="logfs sectors from the card (default: clover-logfs.bin)")
    parser.add_argument("--first-lba", type=lambda value: int(value, 0), default=12320768,
                        help="logfs first LBA (default 12320768, the card's p39)")
    parser.add_argument("--verdict", action="store_true",
                        help="read the round174 verdict ring instead (sectors 64..127)")
    parser.add_argument("--tee", action="store_true",
                        help="read the verdict ring and print only the lines the round175 tee took "
                             "from the kernel log (implies --verdict)")
    parser.add_argument("--driver", action="store_true",
                        help="the other half of --tee: only the lines CloverSdhci said itself")
    parser.add_argument("--count", action="store_true",
                        help="print the number of records and exit (for scripts)")
    parser.add_argument("--newest", action="store_true",
                        help="print only the newest record")
    parser.add_argument("--selftest", action="store_true",
                        help="round-trip both windows in memory and exit (no card needed)")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    only = "kernel" if args.tee else ("driver" if args.driver else None)
    if only:
        args.verdict = True

    path = Path(args.dump)
    if not path.is_file():
        raise SystemExit(f"no dump at {path}")
    data = path.read_bytes()
    if len(data) < SLOT_BYTES:
        raise SystemExit(f"{path} is {len(data)} bytes; the first slot alone is {SLOT_BYTES}")
    if args.verdict:
        need = (VERDICT_OFFSET + 2 * VERDICT_SLOTS) * SECTOR
        if len(data) < need:
            raise SystemExit(f"{path} is {len(data)} bytes; the verdict ring needs {need}")
        records = decode(data, args.first_lba, slots=VERDICT_SLOTS, base_sector=VERDICT_OFFSET)
    else:
        records = decode(data, args.first_lba)
    if only:
        records = [record for record in records if producer(record[2]) == only]
    if args.count:
        print(len(records))
        return 0 if records else 1
    if not records:
        window = "verdict ring" if args.verdict else "report window"
        if only:
            window += f" ({only} lines)"
        print(f"{path}: no records ({window} slot 0 has no LOG1 magic)", file=sys.stderr)
        return 1
    report(records, newest_only=args.newest, order_by_seq=args.verdict)
    return 0


if __name__ == "__main__":
    sys.exit(main())
