#!/usr/bin/env python3
"""Round340 (#653), re-aimed by round341. The card's geometry exists in several files; this
script makes the relations between them a reading.

Four files hold the card's geometry now, and this script makes the relations between them a reading.

drivers/CloverSdhci/SdhciLogCore.h declares it and builds a record (round341 took both out of
drivers/CloverSdhci/CloverSdhci.cpp, which has no copy of its own layout left -- leg 1b is what says
so). scripts/read-clover-log.py reads them, from constants typed a second time.
scripts/run-clover-unattended.sh dd's the window, from a third copy of the size. Nothing in the tree
compared the three before this file: a record written at a sector the reader does not look at is a
silent absence, which is the same class of defect that made round319's 512-slot tee ring read as "no
rows" and round191's window arithmetic read as "no verdicts". This is that lesson aimed at the writing
side of the card: the gate runs this script rather than grep it, and every number comes out of a source
file, not out of anybody's memory.

Seven legs, deliberately not one leg seven times:

  1. pairwise -- every name the driver's side declares and the reader re-declares must carry the same
     value, read out of each file by that file's own syntax;
  1b. the move -- the names parsed above must each be declared exactly once, in the layer, with the
     .cpp including that layer and declaring none of them again. The list comes from this script's own
     table-building lines, so a constant added to the comparison is a constant this leg covers;
  2. tiling -- the named regions must cover the read window exactly, per side: sorted by start,
     each one ending where the next begins, the last ending at the window. A region that grew past
     its neighbour is boot 91's defect class (two writers, one sector, one record read as another
     boot's), and a gap is a sector nobody owns inside a window people trust;
  3. words -- the driver's trailing comments make number claims ("deeper than either card ring"),
     and a comment that contradicts the constant beside it is a sentence the next round will trust
     as a measurement. The claim is checked against the table, so prose becomes an instrument
     subject;
  4. shape -- the record's word order, clamp, sum and block order are needles on the writer's own
     lines -- the layer's for the record and the .cpp's for the two sector writes -- because leg 5
     replicates that arithmetic and would otherwise be a claim about a formula that may have moved.
     Round341 re-pinned these to the layer and added two claims the move made possible: the byte sum
     runs over the filtered bytes, and the header's positions come from the writer's own enum.;
  4b. locals -- the driver's own translation unit, which neither this script nor the gate compiles:
     a `const T name = sdhci_* (...);` the enclosing block never reads again is the shape the board's
     -Werror archive step stops on, and round341's first build died there two days after the finding
     it came from;
  5. roundtrip -- records are built here from the driver's own arithmetic and decoded by importing
     the reader and calling its decode() with the reader's constants. That is the only leg that
     exercises code rather than numbers, and its two controls -- the same records looked for two
     sectors earlier, and one sector later -- prove the leg can find nothing when the geometry is
     wrong, which is the failure mode every other leg here would otherwise share.

A leg that parsed nothing is an error, never a pass: every name is required by name, so pointing
one of the two parsers at the wrong file turns this red instead of green.
"""
import bisect
import importlib.util
import pathlib
import re
import struct
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
DRIVER = REPO / "drivers/CloverSdhci/CloverSdhci.cpp"
LAYER = REPO / "drivers/CloverSdhci/SdhciLogCore.h"
READER = REPO / "scripts/read-clover-log.py"
RUNNER = REPO / "scripts/run-clover-unattended.sh"
# What a failed parse names as its subject: the driver side of the comparison is the layer plus
# the file that includes it, and the leg after the tables says which of the two a name belongs to.
DRIVER_SOURCE = "the driver side (SdhciLogCore.h, included by CloverSdhci.cpp)"
SECTOR = 512

problems = []


def fail (what):
    problems.append (what)


def shown (value, as_hex=False):
    """A number for the summary line, or the word that says it was never parsed.

    The legs above already name a failed parse; this only keeps the report itself from raising,
    because a traceback would replace the reading with the interpreter's opinion of it.
    """
    if value is None:
        return "unparsed"
    if isinstance (value, (list, tuple)):
        return "[" + ", ".join (shown (item, as_hex) for item in value) + "]"
    return f"{value:#x}" if as_hex else str (value)


def scalar (text, name, source, env=None):
    """One `name = value` in `text`, evaluated only if digits or arithmetic over parsed names.

    Missing is fatal rather than zero: this file's whole claim is that a number was read from a
    source, and a default would let a renamed constant report agreement about nothing. A right-hand
    side that names another constant (the reader writes `DUMP_SECTORS = BOOT_OFFSET + 1`) is
    resolved from that file's own parsed table, so the relation stays the reader's arithmetic and
    not this script's.
    """
    found = re.search (rf"^\s*(?:static const \w+ )?\b{name}\s*=\s*([^;\n#]+)", text, re.M)
    if found is None:
        fail (f"{source} declares no {name}: the leg would report agreement about a number "
              "nobody wrote down")
        return None
    raw = found.group (1).strip ().rstrip (",;")
    expression = re.sub (r"\b0[xX][0-9a-fA-F]+u?\b",
                         lambda m: str (int (m.group (0).rstrip ("uU"), 0)), raw)
    # A plain decimal with a C suffix (`128u`) is the same number; the suffix is stripped only when
    # it follows a digit, so it cannot eat an identifier's leading letter.
    expression = re.sub (r"(?<=[0-9])[uU]\b", "", expression)
    if not re.fullmatch (r"[0-9+\-*/() a-zA-Z_]+", expression):
        fail (f"{source}'s {name} = {raw!r} is neither a literal nor arithmetic over names this "
              "script can resolve, so the value would have to be typed here instead")
        return None
    if re.search (r"[A-Za-z_]", expression) and env is None:
        fail (f"{source}'s {name} = {raw!r} names another constant but this leg passes no table "
              "to resolve it from")
        return None
    try:
        return int (eval (expression, {"__builtins__": {}}, dict (env or {})))
    except (SyntaxError, ValueError, NameError, TypeError) as error:
        fail (f"{source}'s {name} = {raw!r} does not evaluate: {error}")
        return None


def array (text, name, source, count):
    """A `{a, b, ...}` initialiser for an array constant, with its declared length checked.

    Anchored to the declaration line, exactly like `scalar`. Round341's N36 found the unanchored
    search satisfied by a comment two lines above the table that restates it: a value edited only in
    the declaration was still read as the old number, so the leg reported agreement about a table
    nobody changed. Prose is not a number's source.
    """
    found = re.search (rf"^\s*(?:static const \w+ )?\b{name}\s*(?:\[[^\]]*\])?\s*=\s*\{{([^}}]*)\}}",
                       text, re.M)
    if found is None:
        fail (f"{source} declares no array initialiser for {name}")
        return None
    values = [int (item.strip (), 0) for item in found.group (1).split (",") if item.strip ()]
    if len (values) != count:
        fail (f"{source}'s {name} holds {len (values)} entries, expected {count}: a ring table "
              "whose length moved is not a table the other files can index the same way")
    return values


def numeric_table (text):
    """Every top-level `NAME = <number or arithmetic over names>` in `text`, resolved.

    Each file's derived numbers come out of its own arithmetic: the reader writes BOOT_OFFSET as
    `KERNEL_OFFSET + 2 * KERNEL_SLOTS` and DUMP_SECTORS as `BOOT_OFFSET + 1`, so a table resolved
    through here says what that file says -- a renamed or deleted intermediate is this script's
    problem to report, not this script's number to invent.
    """
    table = {}
    for name, rhs in re.findall (r"^([A-Z][A-Z0-9_]*) = ([^#\n]+)", text, re.M):
        rhs = rhs.strip ()
        if re.fullmatch (r"[0-9A-Za-z_ +*/()-]+", rhs):
            table[name] = rhs
    for _ in range (len (table) + 4):
        pending = [name for name, rhs in table.items () if not re.fullmatch (r"-?\d+", rhs)]
        if not pending:
            break
        moved = False
        for name in pending:
            substituted = re.sub (r"[A-Za-z_][A-Za-z0-9_]*",
                                  lambda m: str (table[m.group (0)]) if m.group (0) in table
                                  and re.fullmatch (r"-?\d+", table[m.group (0)]) else m.group (0),
                                  table[name])
            if re.fullmatch (r"-?\d+(?:\s*[ +*/-]\s*-?\d+)*", substituted):
                try:
                    table[name] = str (int (eval (substituted, {"__builtins__": {}}, {})))
                    moved = True
                except Exception:
                    pass
        if not moved:
            break
    return {name: int (rhs) for name, rhs in table.items () if re.fullmatch (r"-?\d+", rhs)}


driver_code = DRIVER.read_text (encoding="utf-8", errors="replace")
layer_code = LAYER.read_text (encoding="utf-8", errors="replace")
reader_text = READER.read_text (encoding="utf-8", errors="replace")
runner_text = RUNNER.read_text (encoding="utf-8", errors="replace")

# Round341 moved the driver's geometry declarations into the pure layer, so the driver side of every
# comparison below is that layer's text plus the file that includes it. The concatenation alone would
# also pass if a name went back into the .cpp while the layer kept its copy, so the three claims that
# make the move real are checked after the tables: the driver includes the layer, each name this script
# parses is declared exactly once and in the layer, and none of them is declared in the driver again.
driver_text = layer_code + driver_code

ring_count = scalar (driver_text, "kRingCount", DRIVER_SOURCE)
driver = {
    "ring_offset": array (driver_text, "kRingOffset", DRIVER_SOURCE, ring_count),
    "ring_card_slots": array (driver_text, "kRingCardSlots", DRIVER_SOURCE, ring_count),
    "ring_flush_per_pass": array (driver_text, "kRingFlushPerPass", DRIVER_SOURCE, ring_count),
    "ring_ram_slots": scalar (driver_text, "kRingRamSlots", DRIVER_SOURCE),
    "verdict_text_bytes": scalar (driver_text, "kVerdictTextBytes", DRIVER_SOURCE),
    "boot_record_offset": scalar (driver_text, "kBootRecordOffset", DRIVER_SOURCE),
    "boot_kind": scalar (driver_text, "kBootKind", DRIVER_SOURCE),
    "probe_kind": scalar (driver_text, "kProbeKind", DRIVER_SOURCE),
    "log_magic0": scalar (driver_text, "kLogMagic0", DRIVER_SOURCE),
    "log_magic1": scalar (driver_text, "kLogMagic1", DRIVER_SOURCE),
    "report_slots": scalar (driver_text, "kLogSlots", DRIVER_SOURCE),
    "flight_entry_offset": scalar (driver_text, "kFlightEntryOffset", DRIVER_SOURCE),
    "flight_entry_slots": scalar (driver_text, "kFlightEntrySlots", DRIVER_SOURCE),
    "flight_beat_offset": scalar (driver_text, "kFlightBeatOffset", DRIVER_SOURCE),
    "flight_beat_slots": scalar (driver_text, "kFlightBeatSlots", DRIVER_SOURCE),
    "flight_deadline_offset": scalar (driver_text, "kFlightDeadlineOffset", DRIVER_SOURCE),
    "flight_deadline_slots": scalar (driver_text, "kFlightDeadlineSlots", DRIVER_SOURCE),
    "flight_panic_offset": scalar (driver_text, "kFlightPanicOffset", DRIVER_SOURCE),
    "flight_panic_slots": scalar (driver_text, "kFlightPanicSlots", DRIVER_SOURCE),
    "flight_text_offset": scalar (driver_text, "kFlightTextOffset", DRIVER_SOURCE),
    "flight_text_slots": scalar (driver_text, "kFlightTextSlots", DRIVER_SOURCE),
    "flight_drain_offset": scalar (driver_text, "kFlightDrainOffset", DRIVER_SOURCE),
    "flight_drain_slots": scalar (driver_text, "kFlightDrainSlots", DRIVER_SOURCE),
    "flight_kind": scalar (driver_text, "kFlightKind", DRIVER_SOURCE),
    "flight_marker_magic": scalar (driver_text, "kFlightMarkerMagic", DRIVER_SOURCE),
}

# --- leg 1b: the move, claimed three ways (round341) ---
# Parsing the layer plus the driver would also pass if a number were declared in both files, which is
# the exact shape this whole instrument exists to refuse -- so the driver is asked to have no copy of
# its own layout, and the names checked are read out of this script's own table-building text rather
# than typed again here. A name added above is a name this leg covers.
layer_names = sorted (set (re.findall (r'\(driver_text, "(k[A-Za-z0-9_]+)"',
                                       pathlib.Path (__file__).read_text (encoding="utf-8"))))
if len (layer_names) < 20:
    fail (f'the self-read of this script found {len (layer_names)} driver-side names, expected at least '
          "20: the derivation is the leg's only denominator, and a smaller one says the parsing lines "
          "changed shape rather than that the geometry moved")


def declaration (name):
    return re.compile (rf"^\s*static const [^=;]*\b{name}\b\s*(?:\[[^\]]*\])?\s*=", re.M)


if '#include "SdhciLogCore.h"' not in driver_code:
    fail ('CloverSdhci.cpp does not include SdhciLogCore.h, so the geometry this leg compares would not '
          "be the geometry the driver compiles")
twice = [name for name in layer_names if len (declaration (name).findall (layer_code)) != 1]
if twice:
    fail (f"{twice} are not declared exactly once in SdhciLogCore.h: one of the two files' tables would "
          "be decided by which copy the parser reached first")
redeclared = [name for name in layer_names if declaration (name).search (driver_code)]
if redeclared:
    fail ("CloverSdhci.cpp declares " + str (redeclared) + " again beside the layer's copy: the defect "
          "class round340 named is a number typed in two files that no leg compares until a boot reads "
          "one as the other")

# The reader's own arithmetic, resolved from its own file before any derived name is asked for.
reader_env = numeric_table (reader_text)
reader = {
    "ring_offset": [scalar (reader_text, "VERDICT_OFFSET", "the reader"),
                    scalar (reader_text, "KERNEL_OFFSET", "the reader")],
    "ring_card_slots": [scalar (reader_text, "VERDICT_SLOTS", "the reader"),
                        scalar (reader_text, "KERNEL_SLOTS", "the reader")],
    "boot_record_offset": scalar (reader_text, "BOOT_OFFSET", "the reader", reader_env),
    "boot_kind": scalar (reader_text, "BOOT_KIND", "the reader"),
    "probe_kind": scalar (reader_text, "PROBE_KIND", "the reader"),
    "log_magic0": None,
    "log_magic1": None,
    "report_slots": scalar (reader_text, "REPORT_SLOTS", "the reader"),
    "dump_sectors": scalar (reader_text, "DUMP_SECTORS", "the reader", reader_env),
    "flight_entry_offset": scalar (reader_text, "FLIGHT_ENTRY_OFFSET", "the reader"),
    "flight_entry_slots": scalar (reader_text, "FLIGHT_ENTRY_SLOTS", "the reader"),
    "flight_beat_offset": scalar (reader_text, "FLIGHT_BEAT_OFFSET", "the reader"),
    "flight_beat_slots": scalar (reader_text, "FLIGHT_BEAT_SLOTS", "the reader"),
    "flight_deadline_offset": scalar (reader_text, "FLIGHT_DEADLINE_OFFSET", "the reader"),
    "flight_deadline_slots": scalar (reader_text, "FLIGHT_DEADLINE_SLOTS", "the reader"),
    "flight_panic_offset": scalar (reader_text, "FLIGHT_PANIC_OFFSET", "the reader"),
    "flight_panic_slots": scalar (reader_text, "FLIGHT_PANIC_SLOTS", "the reader"),
    "flight_text_offset": scalar (reader_text, "FLIGHT_TEXT_OFFSET", "the reader"),
    "flight_text_slots": scalar (reader_text, "FLIGHT_TEXT_SLOTS", "the reader"),
    "flight_drain_offset": scalar (reader_text, "FLIGHT_DRAIN_OFFSET", "the reader"),
    "flight_drain_slots": scalar (reader_text, "FLIGHT_DRAIN_SLOTS", "the reader"),
    "flight_kind": scalar (reader_text, "FLIGHT_KIND", "the reader"),
    "flight_marker_magic": scalar (reader_text, "FLIGHT_MARKER_MAGIC", "the reader"),
}
magics = re.search (r"^MAGIC\s*=\s*\(\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\)",
                    reader_text, re.M)
if magics is None:
    fail ("the reader declares no MAGIC pair of two hex words: every record it finds is decided "
          "by those two words, so the leg that compares them has no subject")
else:
    reader["log_magic0"], reader["log_magic1"] = int (magics.group (1), 0), int (magics.group (2), 0)
dump_sectors = reader["dump_sectors"]

# Only names both sides declare are compared; a name one side lacks is not a disagreement, and the
# two lists printed below are what makes that visible rather than assumed.
shared = [name for name in driver if name in reader and driver[name] is not None
          and reader[name] is not None]
agree = 0
for name in shared:
    if driver[name] == reader[name]:
        agree += 1
    else:
        fail (f"{name}: the driver says {driver[name]!r} and the reader says {reader[name]!r}; "
              "a record written at one of these is read at the other, which is a silence, not a "
              "disagreement -- nothing in the output would say which file moved")
driver_only = sorted (set (driver) - set (reader))
reader_only = sorted (set (reader) - set (driver) - {"dump_sectors"})
# A name in both tables whose parse failed is not "absent from one side" -- the failure already
# named itself above, and counting it as an absence would hide which file moved.
silently_missing = sorted (name for name in shared
                           if driver[name] is None or reader[name] is None)
if silently_missing:
    fail (f"{silently_missing} are declared by both files but parsed on neither side alone: the "
          "denominator below would read as a smaller agreement rather than as a parse failure")
shared = [name for name in shared if driver[name] is not None and reader[name] is not None]

# --- leg 2: each side's regions must tile the window that side says it reads ---
# A ring record is two sectors (header block, text block); every other region here is one sector
# per slot, which is why the two lengths below are not the same expression.
def region_spans (table):
    def two (slots):
        return None if slots is None else 2 * slots
    return sorted ([
        ("report window", 0, two (table["report_slots"])),
        ("flight entry", table["flight_entry_offset"], table["flight_entry_slots"]),
        ("flight text", table["flight_text_offset"], table["flight_text_slots"]),
        ("flight beat", table["flight_beat_offset"], table["flight_beat_slots"]),
        ("flight deadline", table["flight_deadline_offset"], table["flight_deadline_slots"]),
        ("flight panic", table["flight_panic_offset"], table["flight_panic_slots"]),
        ("flight drain", table["flight_drain_offset"], table["flight_drain_slots"]),
        ("driver ring", table["ring_offset"][0], two (table["ring_card_slots"][0])),
        ("kernel ring", table["ring_offset"][1], two (table["ring_card_slots"][1])),
        ("boot record", table["boot_record_offset"], 1),
    ], key=lambda span: span[1] if span[1] is not None else -1)


def check_tiling (side, table, window):
    if any (value is None for name, start, length in region_spans (table)
            for value in (start, length)) or table["ring_offset"][0] is None \
            or window is None:
        return False
    spans = [(start, start + length, name) for name, start, length in region_spans (table)]
    ok = True
    if spans[0][0] != 0:
        ok = False
        fail (f"{side}'s window does not start where its first region does: {spans[0][2]} is "
              f"listed at sector {spans[0][0]}")
    for (_, previous_end, previous), (start, _, name) in zip (spans, spans[1:]):
        if start != previous_end:
            ok = False
            fail (f"{side}'s regions are not adjacent: {previous} ends at {previous_end} and "
                  f"{name} starts at {start} -- "
                  + ("an overlap, two writers in one sector, which is boot 91's defect arriving "
                     "from a geometry change rather than from a reader"
                     if start < previous_end
                     else "a gap, sectors nobody owns inside a window people trust"))
    if spans[-1][1] != window:
        ok = False
        fail (f"{side}'s last region ends at sector {spans[-1][1]} while its window is "
              f"{window} sectors: "
              + ("the read is short of the geometry, so the last records are never read"
                 if window < spans[-1][1]
                 else "the window reaches past the geometry, so the read includes sectors no "
                      "writer claims"))
    return ok


driver_window = driver["boot_record_offset"] + 1 if driver["boot_record_offset"] else None
driver_tiled = check_tiling ("the driver", driver, driver_window)
reader_tiled = check_tiling ("the reader", reader, dump_sectors)
reader_window_ok = (dump_sectors is not None and reader["boot_record_offset"] is not None
                    and dump_sectors == reader["boot_record_offset"] + 1)
if not reader_window_ok:
    fail (f"the reader's own window ({dump_sectors}) is not one sector past its own boot record "
          f"({reader['boot_record_offset']})")

# --- leg 3: the driver's prose makes number claims; check them against its numbers ---
claim_line = re.search (r"^\s*static const uint32_t kRingRamSlots = (\d+);\s*//([^\n]*)$",
                        driver_text, re.M)
if claim_line is None:
    fail ("the driver's kRingRamSlots line carries no trailing comment this script can read: the "
          "leg compares a claim to a number and has lost the claim")
else:
    depth = int (claim_line.group (1))
    claim = claim_line.group (2)
    checks = []
    if "deeper than either card ring" in claim:
        checks.append (depth >= max (driver["ring_card_slots"]))
    if "deeper than the driver's card ring" in claim:
        checks.append (depth >= driver["ring_card_slots"][0])
    if "shallower than the kernel" in claim:
        checks.append (depth < driver["ring_card_slots"][1])
    if not checks:
        fail (f"the driver's kRingRamSlots comment ({claim.strip ()!r}) makes no claim this leg can "
              "check against the ring table: the sentence is prose a later round will read as a "
              "number, and nothing here would notice if it stopped being true")
    elif not all (checks):
        fail (f"the driver's comment on kRingRamSlots says {depth} is {claim.strip ()} while its "
              f"own card rings are {driver['ring_card_slots']}: the comment and the table are one "
              "claim about one buffer, and a reader of the comment would size the staging ring "
              "against a depth it does not have")

# --- leg 4's third copy: what the host asks the card for ---
dd = sorted (set (int (m.group (1)) for m in re.finditer (
    r"dd if=(?:\$source|[^ \"]*logdump[^\"]*) bs=512 count=(\d+)", runner_text)))
if not dd:
    fail ("scripts/run-clover-unattended.sh dd's no sized logdump window: the read-back path's own "
          "capacity would be unreadable rather than wrong")
byte_asks = sorted (set (int (value) for line in runner_text.splitlines ()
                         if re.search (r"wc -c|\$size|expected", line)
                         for value in re.findall (r"\b(\d{6,})\b", line)))
if not byte_asks:
    fail ("scripts/run-clover-unattended.sh asserts no byte count for its window: a short dd would "
          "be a missing reading with nothing to say so")
if driver_window and dd and set (dd) != {driver_window}:
    fail (f"the host reads {dd} sectors while the driver's table needs {driver_window}: a short "
          "window is a missing reading, and the reader's byte assert turns it into an error that "
          "names neither partition nor offset")
if driver_window and byte_asks and driver_window * SECTOR not in byte_asks:
    fail (f"the host asserts byte counts {byte_asks} but the window is {driver_window * SECTOR}")

# --- leg 4: the writer's record-building lines, which leg 5 replicates ---
# Round341 moved the builder into SdhciLogCore.h, so these needles point there -- and the two lines that
# are about the card rather than about a record (which sector of the pair goes first) are read from the
# .cpp alone, because the layer has no business naming a write.
sectors_per_record = scalar (layer_code, "kRingSectorsPerRecord", "the layer")
k_record_header_words = scalar (layer_code, "kRecordHeaderWords", "the layer")
pinned_slots = scalar (layer_code, "kRingPinnedSlots", "the layer")
if re.search (r"return first_lba \+ kRingOffset\[which\] \+\s*"
              r"kRingSectorsPerRecord \* sdhci_ring_slot \(which, cursor, pin\);", layer_code) is None \
        or re.search (r"return floor \+ cursor % \(kRingCardSlots\[which\] - floor\);",
                      layer_code) is None \
        or re.search (r"const uint32_t floor = \(which == kRingDriver\) \? kRingPinnedSlots : 0u;",
                      layer_code) is None \
        or sectors_per_record != 2:
    fail ("the writer's record address is no longer first_lba + kRingOffset[r] + "
          "2 * sdhci_ring_slot (which, cursor, pin) with the rolling branch "
          "floor + cursor % (slots - floor) and that floor named only for the driver's ring: the "
          "arithmetic in this script is a claim about a formula that may have moved, and a moved "
          "formula is exactly what the roundtrip cannot see")
body_clamp = scalar (layer_code, "kRecordBodyBytes", "the layer")
shape = [
    ("the length field is clamped through the layer's own bound",
     r"return length > kRecordBodyBytes \? kRecordBodyBytes : length;"),
    ("the total is prefix plus body plus one",
     r"return kLogPrefixLength \+ body \+ 1u?;"),
    ("the sum runs over the prefix bytes", r"sum \+= \(uint32_t\) \(uint8_t\) kLogPrefix\[i\];"),
    # Round341's third finding: the sum has to run over the bytes as the record stores them, because
    # the reader recomputes over the sector. A needle here is the only thing that keeps the fix from
    # being a passing test that nobody pinned.
    ("the sum runs over the filtered body bytes",
     r"sum \+= \(uint32_t\) sdhci_record_byte \(\(uint8_t\) text\[i\]\);"),
    ("the sum closes with a newline", r"sum \+= \(uint32_t\) \(uint8_t\) '\\n';"),
    ("the text block filters to printable",
     r"\(c >= 0x20 && c < 0x7f\) \? c : \(uint8_t\) '\.'"),
]
for label, needle in shape:
    if not re.search (needle, layer_code):
        fail (f"the writer no longer builds a record the way this script replicates it: {label}")
card_order = [
    ("the header block is written first", r"if \(!writePioBlock \(lba, gRingBlock"),
    ("the text block is the next sector", r"if \(!writePioBlock \(lba \+ 1, gRingBlock"),
]
for label, needle in card_order:
    if not re.search (needle, driver_code):
        fail (f"CloverSdhci.cpp no longer writes the pair in this order: {label}")
if body_clamp is None:
    fail ("the layer declares no kRecordBodyBytes: the clamp's number would be this script's, "
          "not the writer's")
else:
    reader_clamps = [int (n) for n in re.findall (r"if body > (\d+):", reader_text)]
    if not reader_clamps:
        fail ("the reader no longer bounds a record's body with a literal, so nothing compares the "
              f"writer's {body_clamp} against the number the decoder believes")
    elif any (n != body_clamp for n in reader_clamps):
        fail (f"the writer clamps a body at {body_clamp} while the reader's own bounds say "
              f"{reader_clamps}: a record the writer thinks is legal the reader would refuse")

# The word positions come from the writer's own enum: a needle that typed 0..9 would still pass if the
# enum renumbered, and the reader unpacks by position, so a renumbered writer is read field-shifted.
positions = {name: int (value) for name, value in
             re.findall (r"\b(kWord[A-Za-z0-9]+)\s*=\s*(\d+)(?!\w)", layer_code)}
if sorted (positions.values ()) != list (range (k_record_header_words or 10)):
    fail (f"the layer's word-position enum is {sorted (positions.values ())}, expected 0..9 in some "
          "order: the reader unpacks ten words, so a renamed or renumbered position is a shift, not "
          "a rename")
words = {}
for name, expression in re.findall (r"header\[(kWord[A-Za-z0-9]+)\] = ([^;]+);", layer_code):
    if name not in positions:
        fail (f"the builder writes {name} but the enum declares no position for it: the reader "
              "unpacks by position, so an unnamed word is a field nobody reads")
        continue
    words[positions[name]] = expression.strip ()
want_words = {0: "kLogMagic0", 1: "kLogMagic1", 2: "lba", 3: "sdhci_record_total (body)",
              4: "sdhci_record_sum (text, body)", 6: "body",
              7: "slot", 8: "seq", 9: "boot_id"}
for index, expression in want_words.items ():
    if not words.get (index, "").startswith (expression):
        fail (f"header word {index} is no longer {expression} (it is "
              f"{words.get (index) !r}): the reader unpacks ten words in this order, and a reordered "
              "writer is decoded as a different field")
kind_line = re.search (r"return kind \? kind : \(\(which == kRingKernel\) \? (\d+)u? : (\d+)u?\);",
                       layer_code)
# Word 5 is no longer filled by an expression in the builder: the builder that fills all ten takes the
# kind as a parameter, and the ring's wrapper is what decides it. So the chain has three links, each
# pinned on its own line (a needle across two lines is what caught round328), and together they say the
# same thing the one line said before -- that a ring record's kind comes from the derivation and its slot
# from the cursor rule. A swap of the two *parameters* is not this leg's job: it is the model's, because
# the reader refuses a record whose slot word disagrees with where it was found and whose kind disagrees
# with the ring it was read from.
param_order = re.search (r"sdhci_build_words \(uint32_t \*header, uint32_t lba, uint32_t (\w+), "
                         r"uint32_t (\w+),", layer_code)
if param_order is None or (param_order.group (1), param_order.group (2)) != ("kind", "slot"):
    fail (f"the ten-word builder's third and fourth parameters are "
          f"{(param_order.groups () if param_order else None)} rather than kind, slot: words 5 and 7 are "
          "filled from them, so a rename that crosses the two moves every record's kind into its slot")
if not words.get (5, "").startswith ("kind"):
    fail (f"header word 5 is no longer the builder's kind parameter (it is {words.get (5) !r}): the "
          "reader files a record by that word and has no other evidence of who wrote it")
def layer_body (name):
    """The layer's text for one inline function, definition through its closing brace.

    The two ring derivations are arguments of a call the wrapper makes, and an argument list can be
    broken over lines -- a needle that read only the first line of the call would fail on a reflow that
    changed nothing, which is the mistake round328's own contract caught it making.
    """
    start = layer_code.find ("static inline void " + name + " (")
    if start < 0:
        return ""
    end = layer_code.find ("\n}", start)
    return layer_code[start:end if end >= 0 else len (layer_code)]


ring_body = layer_body ("sdhci_build_header")
for derivation in ("sdhci_build_words (", "sdhci_record_kind_word (kind, which)",
                   "sdhci_ring_slot (which, cursor, pin)"):
    if derivation not in ring_body:
        fail (f"the ring builder no longer passes {derivation} to the ten-word builder: the record's kind "
              "and slot would come from somewhere else, and the two literals that say which ring a record "
              "belongs to would be unread")

# --- leg 4c: round342's pinned slots, read from the three places that must say one number ---
# The count is one number with three homes (layer, reader, card) and the heads are a second table with
# two. A pinned row that the reader does not know about is a row the reader files as a rolling one, and
# a rolling record the layer does not know about is a record that lands in a pinned slot and is read as
# whichever pinned row that slot's head names -- which is a wrong answer that looks exactly like a
# reading. Both directions are checked here, and the driver's own declaration is the third voice: the
# r174 row prints the number it uses, so a card says which generation's arithmetic its own slots follow.
pinned_decl = re.findall (r"static const char \*const kPinHeads\[kRingPinnedSlots\] = \{(.*?)\};",
                          layer_code, re.S)
if pinned_slots is None:
    fail ("the layer declares no kRingPinnedSlots: the pinned count would be this script's number, not "
          "the writer's, and a ring read by a rule nobody wrote is the round319 silence on the way in")
elif len (pinned_decl) != 1:
    fail (f"the layer's pinned-head table is declared in {len (pinned_decl)} places, expected exactly "
          "one: two tables can disagree, and the pinned slot is the only place a reader looks")
else:
    table_names = [n.strip () for n in pinned_decl[0].split (",") if n.strip ()]
    head_literals = dict (re.findall (r'static const char (kPinHead\w+)\[\] = "([^"]*)";', layer_code))
    unnamed = [n for n in table_names if n not in head_literals]
    if unnamed:
        fail (f"the pinned table lists {unnamed}, which the layer declares no literal for: the slot "
              "would be named by an identifier nobody can match against the card's bytes")
        layer_heads = []
    else:
        layer_heads = [head_literals[n] for n in table_names]
        missing = sorted (set (head_literals) - set (table_names))
        if missing:
            fail (f"the layer declares pinned literals {missing} that no table lists: the rows would "
                  "be pinned to nothing, which is boot 120's absence with a name attached")
    if len (table_names) != pinned_slots:
        fail (f"the pinned table lists {len (table_names)} heads while kRingPinnedSlots is "
              f"{pinned_slots}: the count and the table are the same decision written twice, and a row "
              "with no head has no home")
    declared = re.search (r"sdhciSay \(\"CloverSdhci r174: verdict ring armed lba=%u slots=%u "
                          r"pinned=%u", driver_code)
    if declared is None:
        fail ("the driver no longer declares its pinned count on the card in the r174 row: a reader "
              "would apply this tree's rule to a card written under another one, and the disagreement "
              "would be invisible")
    if "kRingPinnedSlots);" not in driver_code:
        fail ("the r174 row's pinned column is no longer filled from kRingPinnedSlots: the declaration "
              "would be a typed number, which is a fourth copy of the count")
    reader_pinned = re.findall (r"^PINNED_SLOTS = (\d+)", reader_text, re.M)
    reader_heads = re.findall (r"^PINNED_HEADS = \[(.*?)\]", reader_text, re.S | re.M)
    if not reader_pinned:
        fail ("the reader declares no PINNED_SLOTS at module level: the walk of the driver ring would "
              "keep using seq modulo slots, which is round341's rule and stops being true the moment a "
              "slot is pinned")
    else:
        int_pinned = int (reader_pinned[0])
        if int_pinned != pinned_slots:
            fail (f"the reader pins {int_pinned} slots and the layer pins {pinned_slots}: the card's "
                  "first records would be read as a different row than the one written there")
        if layer_heads and len (reader_heads) != 1:
            fail (f"the reader's pinned-head list is declared in {len (reader_heads)} places, expected "
                  "one")
        elif layer_heads:
            r_heads = [h.strip ().strip ('"\'') for h in reader_heads[0].replace ("\n", " ").split (",")
                       if h.strip ()]
            if r_heads != layer_heads:
                fail (f"the reader names {r_heads} where the layer pins {layer_heads}: a row can be "
                      "filed in a slot the writer never assigned, or a pinned row can be read as a "
                      "rolling one and erased by the next lap")
panic_body = layer_body ("sdhci_build_panic_header")
for derivation in ("sdhci_build_words (", "kPanicKind"):
    if derivation not in panic_body:
        fail (f"the panic builder no longer passes {derivation} to the ten-word builder: this is the "
              "second producer of the same ten words, and the copy round341 found in CloverSdhci.cpp "
              "returns as soon as this stops being one builder with two entries")
if kind_line is None:
    fail ("the layer's word-5 derivation no longer decides the two ring kinds by literals, so "
          "nothing here can say which kind a record is written as")
unpack = re.findall (r"magic0, magic1, lba, (\w+), (\w+), kind, (\w+), (\w+), _?seq, boot = \\",
                     reader_text)
if len (unpack) < 2:
    fail (f"the reader unpacks the header in {len (unpack)} place(s), not the two the driver's "
          "ring and boot record are read from")
for total_field, sum_field, body_field, slot_field in unpack:
    if (total_field, sum_field, body_field, slot_field) not in (
            ("_total", "_sum", "_body", "_slot"), ("total", "checksum", "body", "slot_word")):
        fail (f"the reader names words 3,4,6,7 {total_field},{sum_field},{body_field},"
              f"{slot_field}: the names and the positions have to come from one table")

# The kinds each ring is asked with, read from the reader's own main path rather than typed here.
# Anchored to the main path's own assignment: the reader's --selftest contains the same two
# numbers in the same order, and an unanchored search is satisfied by whichever copy it reaches
# first -- which would let a round change the path people actually run and this leg never notice.
asked = re.findall (r"slots, base, kind, window = \(VERDICT_SLOTS, VERDICT_OFFSET, "
                    r"\((\d+), (\w+)\)", reader_text)
kernel_ask = re.findall (r"slots, base, kind, window = KERNEL_SLOTS, KERNEL_OFFSET, (\d+),",
                         reader_text)
ring_kinds = None
if len (asked) != 1 or len (kernel_ask) != 1:
    fail (f"the reader's main path asks the two rings in {len (asked)} and {len (kernel_ask)} "
          "place(s), not the one each this leg reads: the roundtrip leg's expects are those two "
          "lines' numbers, and the selftest repeats the same words with different authority")
else:
    second = asked[0][1]
    second = int (second) if second.isdigit () else reader_env.get (second)
    if second is None:
        fail (f"the reader asks the driver ring with a second kind named {asked[0][1]} that "
              "its own table does not resolve")
    else:
        ring_kinds = {0: (int (asked[0][0]), second), 1: (int (kernel_ask[0]),)}
        written = kind_line
        if written is None:
            fail ("the layer's word-5 derivation no longer names the two ring kinds by literals, so "
                  "nothing here can say which kind a record is written as")
        elif (int (written.group (1)), int (written.group (2))) != (ring_kinds[1][0],
                                                                    ring_kinds[0][0]):
            fail (f"the driver writes kind {written.group (1)} to the kernel ring and "
                  f"{written.group (2)} to its own while the reader asks them with "
                  f"{ring_kinds[1][0]} and {ring_kinds[0][0]}: the roundtrip's two legs pick the "
                  "kinds together, so they would agree with each other and not with the board")
        if second != driver["probe_kind"]:
            fail (f"the driver ring accepts kind {second} as its second kind while the driver "
                  f"calls its relayed probe kind {driver['probe_kind']}: the probe rows would be "
                  "named as a mismatch rather than read")

# --- leg 4b: the driver's own translation unit, which nothing here compiles ---
def unread_layer_locals (text):
    """([line, name] for each `const <type> name = sdhci_* (...);` nothing reads again, and the count).

    Round341's extraction left one of these behind -- the local that had held the body clamp, which
    `sdhci_build_header` applies itself -- and the board's -Werror build died on it at archive 11 while
    every leg of this script and of scripts/check-clover.py stayed green. Neither compiles
    CloverSdhci.cpp: the host model compiles the layer and the model, so a leftover in the driver's own
    translation unit is invisible unless someone asks, and this is that question. The scope is the
    block that encloses the declaration, attributed by brace depth (round338's rule), and unbalanced
    braces are reported rather than guessed at. Comment lines and trailing comments are dropped first:
    this round's own fifth finding was a table read out of the prose that restated it, and a use has to
    be a use in code.
    """
    parts = []
    offsets = [0]
    for line in text.splitlines ():
        cut = re.sub (r'"(?:[^"\\]|\\.)*"', '""', re.sub (r"'(?:[^'\\]|\\.)*'", "'x'", line))
        if re.match (r"\s*(//|/\*)", cut) or re.match (r"\s*\*", cut):
            cut = ""
        else:
            cut = cut.split ("//")[0]
        parts.append (cut)
        offsets.append (offsets[-1] + len (cut) + 1)
    code = "\n".join (parts)
    declaration = re.compile (r"^const\s+[A-Za-z_]\w*\s*\**\s*(\w+)\s*=\s*sdhci_\w+\s*\(.*\)$", re.S)
    blocks, open_blocks, declared, unread = {}, [], [], []
    boundary, block_id = 0, None
    for index, char in enumerate (code):
        if char == "{":
            block_id = len (blocks)
            blocks[block_id] = [index, None]
            open_blocks.append (block_id)
            boundary = index + 1
        elif char == "}":
            if not open_blocks:
                return declared, [], False
            blocks[open_blocks.pop ()][1] = index
            boundary = index + 1
        elif char == ";":
            statement = code[boundary:index].strip ()
            found = declaration.match (statement)
            if found and open_blocks:
                declared.append ((found.group (1), open_blocks[-1], boundary))
            boundary = index + 1
    if open_blocks:
        return declared, [], False
    for name, block, start in declared:
        while start < len (code) and code[start].isspace ():
            start += 1
        end = blocks[block][1]
        span = code[blocks[block][0]:end]
        if len (re.findall (rf"\b{re.escape (name)}\b", span)) <= 1:
            unread.append ((bisect.bisect_right (offsets, start), name))
    return declared, unread, True


locals_declared, locals_unread, locals_balanced = unread_layer_locals (driver_code)
if not locals_balanced:
    fail ("drivers/CloverSdhci/CloverSdhci.cpp's braces do not balance: this leg cannot say which "
          "block encloses a local, so it would report either nothing or the wrong things")
for number, name in locals_unread:
    fail (f"drivers/CloverSdhci/CloverSdhci.cpp:{number}: the local {name} is declared from a call "
          f"into the layer and never read again inside the block that declares it: the board's "
          "-Werror build stops there (archive 11) and neither this script nor the gate compiles "
          "this file, which is why the shape that broke the build was invisible until the build")

# --- leg 5: build records the way the driver does, read them the way the reader does ---
roundtrip = None
if all (value is not None for value in (driver["ring_offset"], driver["ring_card_slots"],
                                       reader["ring_offset"], reader["ring_card_slots"],
                                       ring_kinds)):
    spec = importlib.util.spec_from_file_location ("clover_reader", READER)
    reader_module = importlib.util.module_from_spec (spec)
    spec.loader.exec_module (reader_module)
    prefix = reader_module.PREFIX
    window_sectors = dump_sectors or driver_window
    data = bytearray (window_sectors * SECTOR)

    def write_record (which, cursor, seq, kind, text):
        """The driver's own arithmetic, applied to the driver's parsed numbers."""
        body = len (text) if len (text) <= 480 else 480
        lba = driver["ring_offset"][which] + 2 * (cursor % driver["ring_card_slots"][which])
        total = len (prefix) + body + 1
        # The driver sums the bytes it was handed and stores the filtered ones; for the ASCII rows
        # below the two agree, which is what the reader recomputes over.
        checksum = (sum (prefix) + sum (text[:body]) + 0x0a) & 0xFFFFFFFF
        header = struct.pack ("<10I", driver["log_magic0"], driver["log_magic1"], lba, total,
                              checksum, kind, body, cursor % driver["ring_card_slots"][which],
                              seq, 120)
        block = bytearray (prefix) + bytearray (
            (c if c == 0x0a or 0x20 <= c < 0x7f else 0x2e) for c in text[:body])
        block += bytearray (b"\n")
        block += bytearray (SECTOR - len (block))
        data[lba * SECTOR:lba * SECTOR + SECTOR] = header
        data[(lba + 1) * SECTOR:(lba + 1) * SECTOR + SECTOR] = block

    rows = [(0, 0, ring_kinds[0][0], b"clover r340: driver row"),
            (0, 1, ring_kinds[0][1], b"clover r304: probe row"),
            (1, 0, ring_kinds[1][0], b"clover r329: tee row"),
            (1, 1, ring_kinds[1][0], b"unixnu clover kernel row")]
    for index, (which, cursor, kind, text) in enumerate (rows):
        write_record (which, cursor, index, kind, text)

    decoded_kinds = []
    for which in (0, 1):
        found = reader_module.decode (bytes (data), 0, slots=reader["ring_card_slots"][which],
                                      base_sector=reader["ring_offset"][which],
                                      expect_kind=ring_kinds[which])
        written = sum (1 for entry in rows if entry[0] == which)
        if len (found) != written:
            fail (f"the reader found {len (found)} of the {written} rows written into ring "
                  f"{which} at the reader's own base {reader['ring_offset'][which]}: the two "
                  "tables say different things about where a ring starts, and a record outside the "
                  "window is not read as missing, only as never written")
        for slot, header, content, row_problems in found:
            decoded_kinds.append (header["kind"])
            if row_problems:
                fail (f"ring {which} slot {slot} decoded with problems {row_problems}: the writer "
                      "arithmetic in this script and the reader's checks disagree")
    if sorted (decoded_kinds) != sorted (entry[2] for entry in rows):
        fail (f"the decoded kinds {sorted (decoded_kinds)} are not the written kinds "
              f"{sorted (entry[2] for entry in rows)}: word 5 is the only field that says which "
              "ring a record came from, and the roundtrip would pass without it")

    # Two controls: each one a geometry this leg would otherwise be blind to.
    earlier = reader_module.decode (bytes (data), 0, slots=reader["ring_card_slots"][0],
                                    base_sector=reader["ring_offset"][0] - 2,
                                    expect_kind=ring_kinds[0])
    shifted = reader_module.decode (bytes (data), 0, slots=reader["ring_card_slots"][0],
                                    base_sector=reader["ring_offset"][0] + 1,
                                    expect_kind=ring_kinds[0])
    if earlier:
        fail (f"the records decode two sectors before the ring as well ({len (earlier)} rows): "
              "then a reader whose origin moved by one slot would still be satisfied, so the "
              "count above would prove nothing about where the ring starts")
    if shifted:
        fail (f"the records decode one sector after the ring as well ({len (shifted)} rows): a "
              "header never lands on an odd sector, so this is the writer's own parity to protect")
    roundtrip = (len (rows), len (earlier), len (shifted))

print (f"driver constants: ring_offset={driver['ring_offset']} "
       f"ring_card_slots={driver['ring_card_slots']} "
       f"flush_per_pass={driver['ring_flush_per_pass']} ring_ram_slots={driver['ring_ram_slots']} "
       f"verdict_text_bytes={driver['verdict_text_bytes']} "
       f"boot_record_offset={driver['boot_record_offset']} kinds=21,22,"
       f"{driver['probe_kind']},{driver['boot_kind']} "
       f"magics={shown (driver['log_magic0'], True)},{shown (driver['log_magic1'], True)} "
       f"report_slots={driver['report_slots']} "
       f"flight=entry {driver['flight_entry_offset']}/{driver['flight_entry_slots']} "
       f"beat {driver['flight_beat_offset']}/{driver['flight_beat_slots']} "
       f"deadline {driver['flight_deadline_offset']}/{driver['flight_deadline_slots']} "
       f"panic {driver['flight_panic_offset']}/{driver['flight_panic_slots']} "
       f"text {driver['flight_text_offset']}/{driver['flight_text_slots']} "
       f"drain {driver['flight_drain_offset']}/{driver['flight_drain_slots']} "
       f"kind {driver['flight_kind']} marker {shown (driver['flight_marker_magic'], True)}")
print (f"reader constants: ring_offset={reader['ring_offset']} "
       f"ring_card_slots={reader['ring_card_slots']} "
       f"boot_record_offset={reader['boot_record_offset']} dump_sectors={dump_sectors} "
       f"kinds={reader['probe_kind']},{reader['boot_kind']} "
       f"magics={shown (reader['log_magic0'], True)},{shown (reader['log_magic1'], True)}")
print (f"pairwise agreement: {agree} of {len (shared)} names both files declare; "
       f"driver-only {len (driver_only)} ({', '.join (driver_only)}); "
       f"reader-only {len (reader_only)} ({', '.join (reader_only)})")
print (f"region tiling: driver {'exact' if driver_tiled else 'BROKEN'} over "
       f"{driver_window} sectors, reader {'exact' if reader_tiled else 'BROKEN'} over "
       f"{dump_sectors} sectors; the reader's window is "
       + ("one sector past its own boot record" if reader_window_ok
          else "NOT one sector past its own boot record"))
print (f"dd window: driver needs {driver_window} sectors, reader demands {dump_sectors}, "
       f"host reads {dd} and asserts bytes {byte_asks}")
print (f"record shape: {len (shape)} layer lines + {len (card_order)} write lines, "
       f"{len (want_words) + 1} of {k_record_header_words} header words, "
       f"{len (unpack)} reader unpacks, rings asked with {ring_kinds}")
print (f"driver locals: {len (locals_declared)} bound from a layer call, {len (locals_unread)} "
       f"unread in their own block, braces balanced={locals_balanced}")
if roundtrip:
    print (f"roundtrip: {roundtrip[0]} records built from the writer's arithmetic read back by the "
           f"reader's decode() with problems=0; the same records two sectors earlier decode as "
           f"{roundtrip[1]} and one sector later as {roundtrip[2]}")

if problems:
    for item in problems:
        print (f"PROBLEM {item}", file=sys.stderr)
    print (f"sdhci geometry: {len (problems)} problem(s)", file=sys.stderr)
    sys.exit (1)
print ("sdhci geometry: OK")
