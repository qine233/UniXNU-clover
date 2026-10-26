#!/usr/bin/env python3
"""Decode the clover logfs record window into readable text.

clover has no serial console, so the driver (drivers/CloverSdhci) and the panic
screen keep their text on the card: drivers/CloverSdhci writes two 512-byte
blocks per record at logfs start, and slot k lives at LBA first + 2k. The header
block carries a magic, the LBA, the total byte count, a byte sum and the kind;
the second block carries `PREFIX + body + "\\n"` as plain text, so the record can
also be read on a machine that has never heard of this format.

  words[0] 0x554e5855  "UXNU"
  words[1] 0x31474f4c  "LOG1"
  words[2] absolute LBA of the header block
  words[3] total bytes of the text block: prefix + body + newline
  words[4] byte sum of those bytes, low 32 bits
  words[5] kind: 17 = report (overwrite), 19 = report (append), 20 = panic screen
  words[6] body length
  words[7] slot index

Input is the first 64 sectors of logfs, which is what the recovery-side dd in
scripts/run-clover-unattended.sh reads. A record whose header is present but
whose text does not sum is still printed: a half-written panic is a reading.
"""
import argparse
import struct
import sys
from pathlib import Path

PREFIX = b"unixnu clover emmc log\n"
MAGIC = (0x554E5855, 0x31474F4C)  # "UXNU", "LOG1" in memory order
SLOTS = 16
SLOT_BYTES = 1024
KINDS = {17: "report-17", 19: "report-19", 20: "panic-20"}


def decode(data, first_lba):
    """[(slot, header dict, text bytes or None, problems [str])] in slot order."""
    records = []
    for slot in range(SLOTS):
        base = slot * SLOT_BYTES
        block = data[base:base + SLOT_BYTES]
        if len(block) < SLOT_BYTES:
            break
        magic0, magic1, lba, total, checksum, kind, body, slot_word = struct.unpack_from("<8I", block)
        if (magic0, magic1) != MAGIC:
            # The writer scans forward and stops at the first header without the
            # magic, so a gap ends the window: later slots are not records.
            break
        problems = []
        if lba != first_lba + 2 * slot:
            problems.append(f"lba {lba} != {first_lba + 2 * slot}")
        if slot_word != slot:
            problems.append(f"slot word {slot_word} != {slot}")
        if body > 480:
            problems.append(f"body {body} > 480")
        if total != len(PREFIX) + body + 1:
            problems.append(f"total {total} != {len(PREFIX) + body + 1}")
        text = block[512:]
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
            "text_ok": text_ok,
        }, content, problems))
    return records


def text_of(content):
    """The body as text. The writer stores one space for control bytes, so this is
    already printable, but a byte outside ASCII is replaced instead of guessed."""
    return bytes(byte if 0x09 <= byte < 0x7F else 0x2E for byte in content).decode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", nargs="?", default="clover-logfs.bin",
                        help="first 64 sectors of logfs (default: clover-logfs.bin)")
    parser.add_argument("--first-lba", type=lambda value: int(value, 0), default=12320768,
                        help="logfs first LBA (default 12320768, the card's p39)")
    parser.add_argument("--count", action="store_true",
                        help="print the number of records and exit (for scripts)")
    parser.add_argument("--newest", action="store_true",
                        help="print only the newest record")
    args = parser.parse_args()

    path = Path(args.dump)
    if not path.is_file():
        raise SystemExit(f"no dump at {path}")
    data = path.read_bytes()
    if len(data) < SLOT_BYTES:
        raise SystemExit(f"{path} is {len(data)} bytes; the first slot alone is {SLOT_BYTES}")
    records = decode(data, args.first_lba)
    if args.count:
        print(len(records))
        return 0 if records else 1
    if not records:
        print(f"{path}: no records (no LOG1 magic at slot 0)", file=sys.stderr)
        return 1
    if args.newest:
        records = records[-1:]

    for slot, header, content, problems in records:
        verdict = "ok" if not problems else "BAD (" + "; ".join(problems) + ")"
        print(f"slot {slot}  lba {header['lba']}  kind {header['kind']} "
              f"({KINDS.get(header['kind'], 'unknown')})  body {header['body']}  "
              f"sum {header['sum']:#010x}  {verdict}")
        if content is not None:
            body = text_of(content)
            for line in body.rstrip("\n").split("\n"):
                print("    " + line)
        else:
            print("    <no readable text block>")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
