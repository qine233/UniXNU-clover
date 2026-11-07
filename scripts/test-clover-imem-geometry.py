#!/usr/bin/env python3
"""Who owns each byte the IMEM reset record touches -- derived, not transcribed.

Round338 (#719). Round336 concluded that the 160 bytes from IMEM 0x660 are rewritten by firmware
every boot, and left the record's own geometry unexamined: the comment in drivers/CloverSdhci
describes the slot as twenty words *because its author read the gap as twenty words*. This script
answers the prior question, from the two device-tree sources archived in
notes/sources/sdm660/imem-children.dts and from the driver's own constants:

  * what is the largest address range inside the 4 KiB IMEM window that NO child node claims,
    starting at the offset the record is built at, and how many 64-bit words is that;
  * for every word the driver reads or writes, which child node owns the eight bytes underneath it
    -- named, or `free` -- so a word that steps on `boot_stats` cannot be described as ours;
  * do the board's own tree and the vendor tree agree on that map (two sources, compared as
    integers, because they spell the same numbers differently).

Nothing here edits or flashes: it is the instrument that has to be green before anybody resizes the
record or moves it. The exit code is about THIS INSTRUMENT's integrity -- it fails if a source block
cannot be parsed, if the two sources disagree, if the driver's constants cannot be found, or if any
word ends up with an empty owner. It deliberately does not fail when a word belongs to a child node:
that is a reading, and today it is a reading with teeth.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ARCHIVE = REPO / "notes/sources/sdm660/imem-children.dts"
POWEROFF = REPO / "notes/sources/sdm660/msm-poweroff.c"
DRIVER = REPO / "drivers/CloverSdhci/CloverSdhci.cpp"

CHILD = re.compile(r"^\s*([A-Za-z0-9_,.-]+)@([0-9a-fA-F]+)\s*\{")
COMPAT = re.compile(r'compatible\s*=\s*"([^"]+)"')
REG = re.compile(r"reg\s*=\s*<([^>]*>)*\s*;")
WORDS = re.compile(r"\s*record\[(\d+)\]\s*=\s*(?!=)")
READS = re.compile(r"\s*(?:const\s+)?uint64_t\s+(\w+)\s*=\s*record\[(\d+)\]")
CELLS = re.compile(r"\b(0x[0-9a-fA-F]+|\d+)\b")


def numbers(text: str) -> list[int]:
    """Every cell literal in a dtc `reg`/`#-*-cells` value, as an integer."""
    return [int(token, 0) for token in CELLS.findall(text)]


def parse_source(block: str, label: str) -> dict[str, dict]:
    """One `qcom,msm-imem@base { ... }` block to {child name: (offset, length, compatible)}."""
    parent = re.search(r"qcom,msm-imem@([0-9a-fA-F]+)\s*\{", block)
    if parent is None:
        raise SystemExit(f"{label}: no qcom,msm-imem@ parent node to parse")
    lines = block.splitlines()
    # #address-cells/#size-cells decide how reg is cut up; both sources say 1 and 1, and this
    # refuses to assume it silently.
    cells = {"#address-cells": 1, "#size-cells": 1}
    for key in cells:
        found = re.search(rf"{key}\s*=\s*<([^>]+)>", block)
        if found is None:
            raise SystemExit(f"{label}: {key} is absent, so reg cannot be cut up")
        cells[key] = numbers(found.group(1))[0]
    children: dict[str, dict] = {}
    depth = 0
    current: str | None = None
    body: list[str] = []
    for line in lines:
        if current is None:
            match = CHILD.match(line)
            if match and match.group(1) != "qcom,msm-imem":
                current, body, depth = f"{match.group(1)}@{match.group(2)}", [line], line.count("{") - line.count("}")
                if depth == 0:
                    current = None
                continue
            continue
        body.append(line)
        depth += line.count("{") - line.count("}")
        if depth == 0:
            text = "\n".join(body)
            reg = re.search(r"reg\s*=\s*<([^>]*)>", text)
            compat = COMPAT.search(text)
            if reg is None:
                raise SystemExit(f"{label}: {current} has no reg")
            values = numbers(reg.group(1))
            if len(values) < 2:
                raise SystemExit(f"{label}: {current} has a reg with {len(values)} cell(s)")
            children[current] = {"offset": values[0], "length": values[1],
                                 "compatible": compat.group(1) if compat else "unnamed"}
            current = None
    if not children:
        raise SystemExit(f"{label}: zero children parsed -- the record would be checked against "
                         "an empty map, which is how a scan that stops at the first hit reads as "
                         "no hits at all")
    return children


def driver_constants() -> dict:
    """The numbers the driver actually uses, taken from the file rather than from prose."""
    text = DRIVER.read_text(encoding="utf-8", errors="replace")
    out: dict[str, object] = {"text": text}
    for name, pattern in (("base", r"kImemPhys\s*=\s*(0x[0-9a-f]+)ULL"),
                          ("size", r"kImemSize\s*=\s*(0x[0-9a-f]+)ULL"),
                          ("restart_offset", r"kRestartReasonOffset\s*=\s*(0x[0-9a-f]+)"),
                          ("record_offset", r"kResetRecordOffset\s*=\s*(0x[0-9a-f]+)")):
        found = re.search(pattern, text)
        if found is None:
            raise SystemExit(f"drivers/CloverSdhci/CloverSdhci.cpp declares no {pattern!r}: "
                             "the geometry has nothing to be checked against")
        out[name] = int(found.group(1), 16)
    zero = re.search(r"for \(unsigned int i = 0; i < (\d+); \+\+i\) \{\s*\n\s*record\[i\] = 0;", text)
    if zero is None:
        raise SystemExit("no bounded whole-slot zeroing loop was found: the span the driver wipes "
                         "cannot be read off the file, and a comment is not a span")
    out["zero_words"] = int(zero.group(1))
    writes = sorted({int(m.group(1)) for m in WORDS.finditer(text)})
    reads = {int(m.group(2)): m.group(1) for m in READS.finditer(text)}
    out["write_words"] = writes
    out["read_words"] = sorted(reads)
    out["read_names"] = reads
    declared = re.search(r"Layout, twenty 64-bit words|Layout, (\d+) 64-bit words", text)
    out["layout_comment"] = (re.search(r"// Layout, [a-z0-9]+ 64-bit words.*", text) or
                             re.search(r"Layout, (\d+) 64-bit words", text))
    out["declared_words"] = int(declared.group(1)) if declared and declared.group(1) else 20
    return out


RESTART_WRITE = re.compile(r"__raw_writel\(\s*(0x7766[0-9a-fA-F]{4}|0x0)\s*,\s*restart_reason\s*\)")
CMD_STRING = re.compile(r'(?:strncmp|strcmp)\(\s*cmd\s*,\s*"([^"]+)"')
PANIC_TEST = re.compile(r"\bin_panic\b")
FUNCTION = re.compile(r"^static\s+(?:int|void|ssize_t|long)\s+(\w+)\s*\(")
BRANCH = re.compile(r"^(?:\}\s*)?(?:else\s+if\s*\(.*\)\s*\{|if\s*\(.*\)\s*\{|else\s*\{)\s*$")
PREPROC = re.compile(r"^\s*#\s*(if|ifdef|ifndef|else|elif|endif|if\s+0)\b(.*)")


def reason_table() -> list[dict]:
    """Every value the vendor kernel writes to restart_reason@65c, with the label it writes it under.

    Round338's second leg. The IMEM geometry says WHERE the record lives; this says what the words
    already in that window mean, because drivers/CloverSdhci names one of them kRestartFastboot on the
    strength of a comment about what the stock kernel writes. Reading the comment is not reading the
    kernel: the pairing here comes from the branch the write sits in -- the `cmd` string in that
    branch's opener, else an `in_panic` test in it, else "unconditional else", else the enclosing
    function -- and every row prints which of the four it was and the line it was read off. `#if 0`
    blocks are kept but marked, because the dm-verity branch shares a value with the panic branch and
    only the preprocessor separates them.
    """
    if not POWEROFF.is_file():
        raise SystemExit(f"{POWEROFF} is missing: the restart_reason values would have no authority "
                         "to be checked against, which is the same hole as reading the driver's own "
                         "comment about the stock kernel")
    lines = POWEROFF.read_text(encoding="utf-8", errors="replace").splitlines()
    out: list[dict] = []
    guards: list[str] = []
    braces = 0                     # brace depth the current line lives at
    openers: list[tuple[int, int]] = []   # (line index, brace depth of that branch's body)
    current_fn = "top-level"
    for index, line in enumerate(lines):
        conditional = PREPROC.match(line)
        if conditional:
            keyword = conditional.group(1)
            if keyword.startswith("if"):
                guards.append(conditional.group(2).strip() or "1")
            elif keyword == "else":
                guards[-1:] = [f"else of {guards[-1]}" if guards else "else"]
            elif keyword == "endif":
                guards.pop()
        named = FUNCTION.match(line)
        if named:
            current_fn = named.group(1)
            openers = []
        found = RESTART_WRITE.search(line)
        if found is not None:
            value = int(found.group(1), 16)
            # The label has to come from the branch THIS write is in. Two versions of this got it wrong
            # in two different ways and both were caught by the table disagreeing with the file: the
            # first scanned back for the nearest `cmd` string or `in_panic` test, which labelled the
            # unconditional `} else {` arm at 579 with the arm above it (it would have reported
            # 0x77665501 as a panic value); the second scanned back for the nearest thing that LOOKS
            # like a branch, which labelled the straight-line write at 690 with an `} else {` whose
            # scope had closed nineteen lines earlier. So the pairing is now made on brace depth: the
            # branch that owns a write is the last opener whose body lives at the write's own depth.
            inside = [i for (i, body_depth) in openers if body_depth == braces and i < index]
            text = lines[inside[-1]].strip() if inside else ""
            if inside:
                cmd = CMD_STRING.search(text)
                if cmd:
                    label, via = cmd.group(1), "cmd"
                elif PANIC_TEST.search(text):
                    label, via = "in_panic", "panic"
                else:
                    label, via = "else", "else"
                how = f"`{text[:52]}` at line {inside[-1] + 1}"
            else:
                label, via = current_fn, "function"
                how = f"no branch at this depth above it in {current_fn}()"
            out.append({"value": value, "line": index + 1, "label": label, "how": how, "via": via,
                        "conditional": list(guards)})
        stripped = line.strip()
        if BRANCH.match(stripped):
            openers.append((index, braces + stripped.count("{") - stripped.count("}")))
        braces += line.count("{") - line.count("}")
    if guards:
        raise SystemExit("the preprocessor conditionals in msm-poweroff.c did not balance, so which "
                         "of these writes compiles for this board is not knowable from the file")
    if braces != 0:
        raise SystemExit("the brace depth of msm-poweroff.c did not return to zero, so the branch each "
                         "write sits in was attributed against a drifting counter")
    if not out:
        raise SystemExit("no restart_reason write parsed from msm-poweroff.c: the table below would be "
                         "empty, and an empty table reads as 'the value means nothing' rather than "
                         "'nothing was looked at'")
    return out


def owners(children: dict[str, dict], start: int, end: int) -> list[tuple[int, int, str]]:
    """Split [start, end) at child boundaries: (from, to, label). `free` is a real answer."""
    out: list[tuple[int, int, str]] = []
    cursor = start
    while cursor < end:
        hit = None
        for name, cell in children.items():
            base, length = cell["offset"], cell["length"]
            if base <= cursor < base + length:
                hit = (name, base + length)
                break
        if hit:
            stop = min(end, hit[1])
            out.append((cursor, stop, hit[0]))
        else:
            stop = end
            for name, cell in children.items():
                if cell["offset"] > cursor:
                    stop = min(stop, cell["offset"])
            out.append((cursor, stop, "free"))
        cursor = stop
    return out


def main() -> int:
    raw = ARCHIVE.read_text(encoding="utf-8", errors="replace")
    marks = [m.start() for m in re.finditer(r"^=== SOURCE", raw, re.M)]
    if len(marks) != 2:
        raise SystemExit(f"{ARCHIVE} holds {len(marks)} SOURCE blocks, not 2: the two-leg check "
                         "needs both the board's tree and the vendor's tree")
    board = parse_source(raw[marks[0]:marks[1]], "board tree")
    vendor = parse_source(raw[marks[1]:], "vendor tree")

    drv = driver_constants()
    record_at = int(drv["record_offset"])
    words = max(int(drv["zero_words"]), int(drv["declared_words"]),
                max(drv["write_words"], default=0) + 1)
    span = words * 8

    disagreements = []
    for name in sorted(set(board) | set(vendor)):
        if board.get(name) != vendor.get(name):
            disagreements.append(f"{name}: board {board.get(name)} vendor {vendor.get(name)}")

    print(f"imem window: base {int(drv['base']):#x} size {int(drv['size']):#x}")
    print(f"children parsed: board={len(board)} vendor={len(vendor)}; "
          f"two-source disagreements={len(disagreements)}")
    for line in disagreements:
        print(f"  DISAGREE {line}")
    print(f"restart_reason cell: {int(drv['restart_offset']):#x} (4 bytes, so the gap begins at "
          f"{int(drv['restart_offset']) + 4:#x})")
    print(f"reset record: offset {record_at:#x}, {words} words touched by this driver "
          f"(zeroing loop {drv['zero_words']}, layout comment {drv['declared_words']}, "
          f"highest indexed word {max(drv['write_words'], default=0)}), span "
          f"{record_at:#x}..{record_at + span - 1:#x}")

    free_run = 0
    claimed_words = 0
    claimed_indexed_writes = 0
    claimed_reads = 0
    print("Every word the driver touches, and who owns its eight bytes. `wiped` is the boot-start "
          "zeroing loop, which runs over words 0..N-1 whatever else happens.")
    print("word  address   owner                 read/used-by         indexed  wiped")
    for index in range(words):
        start = record_at + index * 8
        pieces = owners(board, start, start + 8)
        labels = "|".join(sorted({p[2] for p in pieces if p[2]}))
        entirely_free = pieces == [(start, start + 8, "free")]
        if entirely_free and free_run == index * 8:
            free_run += 8
        if not entirely_free:
            claimed_words += 1
            if index in drv["write_words"]:
                claimed_indexed_writes += 1
            else:
                claimed_reads += 1
        name = drv["read_names"].get(index, "")
        writes = index in drv["write_words"]
        print(f"[{index:>2}]  {start:#05x}  {labels or 'UNNAMED':<20} {name or '-':<21} "
              f"{'WRITE' if writes else '     '}    "
              f"{'yes' if index < int(drv['zero_words']) else 'no'}")

    print(f"largest contiguous free run starting at the record offset: {free_run} bytes "
          f"= {free_run // 8} words (probed {span} bytes, {words} words)")
    print(f"words whose eight bytes are not entirely free: {claimed_words} of {words}; "
          f"of those, indexed writes: {claimed_indexed_writes}, read-only: {claimed_reads}")
    owned = sorted({p[2] for index in range(words)
                    for p in owners(board, record_at + index * 8, record_at + index * 8 + 8)
                    if p[2] != "free"})
    print(f"child nodes the record spans: {' '.join(owned) if owned else 'none'} "
          f"(asked about {words} words, named {len(owned)})")
    if any("UNNAMED" in "|".join(sorted({q[2] or "UNNAMED" for q in
                                         owners(board, record_at + index * 8,
                                                record_at + index * 8 + 8)}))
           for index in range(words)):
        raise SystemExit("a word resolved to a zero-length owner: the table would read as free "
                         "space while a child node claims it")
    if disagreements:
        raise SystemExit("the board's tree and the vendor tree disagree on the map, so the map "
                         "below is not two-source confirmed")

    # The second question: what the words in this window MEAN, in the only kernel that writes them.
    table = reason_table()
    our_found = re.search(r"kRestartFastboot\s*=\s*(0x[0-9a-fA-F]+)", str(drv["text"]))
    if our_found is None:
        raise SystemExit("drivers/CloverSdhci/CloverSdhci.cpp declares no kRestartFastboot: the "
                         "constant whose meaning is being asked of the vendor table is not there")
    our_value = int(our_found.group(1), 16)
    off = [t for t in table if "0" in t["conditional"]]
    by_how = {"cmd": 0, "panic": 0, "else": 0, "function": 0}
    for entry in table:
        by_how[entry["via"]] += 1
    print(f"restart_reason writes parsed from {POWEROFF.name}: {len(table)} of them; the branch each "
          f"one sits in names a reboot command={by_how['cmd']}, tests in_panic={by_how['panic']}, "
          f"is an unconditional else={by_how['else']}, had no branch opener above it="
          f"{by_how['function']}; under a #if 0: {len(off)}")
    if by_how["function"] == len(table):
        raise SystemExit("no write in the table could be attributed to a branch at all: the parser is "
                         "reading the wrong function, and an empty judgement is not a passing one")
    for entry in table:
        guard = " compiled-out (#if 0)" if entry in off else ""
        print(f"  {entry['value']:#010x}  line {entry['line']:>4}  {entry['label']:<28} "
              f"via {entry['how']}{guard}")
    ours = [t for t in table if t["value"] == our_value and t not in off]
    boot = [t for t in table if t["label"] == "bootloader" and t not in off]
    rows = ", ".join("%s@%d" % (t["label"], t["line"]) for t in ours)
    print(f"this port: kRestartFastboot = {our_value:#010x}; vendor rows carrying that value and "
          f"compiling for clover: {len(ours)} ({rows or 'none'})")
    print(f"the value the vendor writes for `reboot bootloader`: "
          f"{boot[0]['value']:#010x} (line {boot[0]['line']})" if boot else
          "the value the vendor writes for `reboot bootloader`: NOT FOUND in the parsed table")
    named_boot = [t for t in ours if t["label"] == "bootloader"]
    print(f"agreement check: our value is labelled bootloader by the vendor in "
          f"{len(named_boot)} of {len(ours)} rows that write it "
          f"(asked about {len(table)} writes, {len(off)} of them compiled out)")
    if not boot:
        raise SystemExit("no row labelled bootloader was parsed, so the comparison above could not "
                         "have said anything about what fastboot means to this boot chain")
    print("imem geometry selftest: the record's words are each attributed to a named node or to "
          "free space, from both archived sources, and every value the driver names for "
          "restart_reason is looked up in the table the vendor code itself builds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
