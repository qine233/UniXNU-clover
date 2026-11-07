#!/usr/bin/env python3
"""Negative tests for the #716, #718, #719, #720, #720b, #653 and #728 needles in scripts/check-clover.py.

Each case breaks one shape the gate now pins, runs the gate, and restores in a finally block: a
negative test that leaves its edit behind is worse than no negative test, because the next gate run
then judges a repository that is not the one under review (round333's own negative script left a
change behind exactly this way). The strings matched here carry the files' real indentation, and a
pattern that misses is an exception rather than a pass -- a mutation that applies to nothing would
otherwise report a green gate as "the needle works".

A case is either (name, path, old, new[, occurrence]) or (name, [(path, old, new[, occurrence]),
...]): round338's N15 needs two files broken at once, because either half alone is a legitimate
state the gate must not reject. `old = None` means "this file is not there", which is how the
archive's own existence gets judged rather than asserted. A list-form case may carry a third
element, a substring the gate's output has to contain: exit 1 alone only proves *something*
judged, and for an instrument whose job is to name what it saw, that is not the same claim.
"""
import subprocess
import sys
import traceback
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "scripts/run-clover-unattended.sh"
READER = REPO / "scripts/read-clover-log.py"
GEOMETRY = REPO / "scripts/test-clover-imem-geometry.py"
IMEM_ARCHIVE = REPO / "notes/sources/sdm660/imem-children.dts"
POWEROFF_ARCHIVE = REPO / "notes/sources/sdm660/msm-poweroff.c"
BOOT_STATS_ARCHIVE = REPO / "notes/sources/sdm660/boot_stats.c"
SDHCI = REPO / "drivers/CloverSdhci/CloverSdhci.cpp"
SDHCI_LAYER = REPO / "drivers/CloverSdhci/SdhciLogCore.h"
SDHCI_MODEL = REPO / "drivers/CloverSdhci/tests/clover_sdhci_host.cpp"
SDHCI_TEST = REPO / "scripts/test-clover-sdhci.py"
SDHCI_GEOMETRY = REPO / "scripts/test-clover-sdhci-geometry.py"
PANIC = REPO / "scripts/xnu-panic-screen.c"
PROBE = REPO / "package/CloverLegacyDisplayProbe.cpp"
HARNESS = REPO / "drivers/CloverPower/tests/clover_power_host.cpp"
GATE = REPO / "scripts/check-clover.py"
LOG = Path.home() / "clover-out/round343-column-names/negatives.log"

SILENT_AFTER = '''      timeline_close "neither-$(usb_landing_class)" "" ""
      return 2
    fi
    sleep 2'''

POLL_PRINTF = """    printf '%s %s %s %s %s %s\\n' "$now" "$(( now - TIMELINE_START ))" "${adb_now:--}" "${fb_now:--}" \\"""

import re as _re

def archive_block(child: str, source: int = 1) -> str:
    """One child block of the archived IMEM node, read off the archive instead of typed out.

    Round333's lesson applies to this file too: a match string typed from memory carries the wrong
    indentation and the case then tests nothing. The two SOURCE blocks indent differently (the
    board's tree nests one level deeper than the vendor include), so the block is chosen by source.
    """
    text = IMEM_ARCHIVE.read_text(encoding="utf-8")
    marks = [m.start() for m in _re.finditer(r"^=== SOURCE", text, _re.M)]
    window = text[marks[source - 1]:marks[source] if source < len(marks) else len(text)]
    found = _re.search(rf"\n[ \t]*{child}@[0-9a-fA-F]+ \{{.*?\n[ \t]*\}};\n", window, _re.S)
    if found is None:
        raise SystemExit(f"{IMEM_ARCHIVE} SOURCE {source} has no {child} block: the case would "
                         "mutate nothing and a green gate would be reported as the needle working")
    return found.group(0)


DLOAD_BOARD = archive_block("dload_type")
BOOT_STATS_BOARD = archive_block("boot_stats")
BOOT_STATS_NO_REG = _re.sub(r"\n[ \t]*reg = <[^>]*>;", "", BOOT_STATS_BOARD, count=1)
if BOOT_STATS_NO_REG == BOOT_STATS_BOARD:
    raise SystemExit("the boot_stats block has no reg to strip: N16 would test nothing")

CASES = [
    ("N1 classifier removed", SCRIPT,
     "usb_landing_class() {", "usb_landing_class_unused() {"),
    ("N2 bulk node no longer grepped", SCRIPT,
     "grep -qE 'QUSB_BULK|QPS6000|HS-USB.*9008'", "grep -qE 'NOTHING_MATCHES_THIS'"),
    ("N3 after-deadline left classless", SCRIPT, SILENT_AFTER,
     '      timeline_close neither "" ""\n      return 2\n    fi\n    sleep 2'),
    ("N4 verdict re-measures instead of quoting the close line",
     [(SCRIPT, 'closing=$(timeline_last_close)', 'closing=$(usb_landing_class)')],
     "of the 2 places that can end silent"),
    ("N5 reader loses one watch key", READER, '    "wait-adb": {', '    "wait-adb-renamed": {'),
    ("N6 reader stops running its timeline leg", READER, "    timeline_selftest()\n", ""),
    # Round337 (#718).
    ("N7 poll stops asking the class", SCRIPT,
     "now=$(date +%s); class=$(usb_landing_class)", "now=$(date +%s); class=no-node"),
    ("N8 an empty host answer becomes a quiet bus", SCRIPT,
     "printf 'ioreg-fail'", "printf 'no-node'"),
    ("N9 poll row drops the class column", SCRIPT,
     POLL_PRINTF, """    printf '%s %s %s %s %s\\n' "$now" "$(( now - TIMELINE_START ))" "${adb_now:--}" "${fb_now:--}" \\"""),
    ("N10 the second silent verdict stops quoting the file",
     [(SCRIPT, 'closing=$(timeline_last_close)', 'closing=$(usb_landing_class)', 2)],
     "of the 2 places that can end silent"),
    ("N11 unasked class reads as no-node", READER,
     'TIMELINE_CLASS_ABSENT = "not-asked-by-this-writer"', 'TIMELINE_CLASS_ABSENT = "no-node"'),
    ("N12 reader stops printing the per-poll class", READER,
     'class_note = "class-polls=" + " ".join(steps)', 'class_note = "klass-polls=" + " ".join(steps)'),
    # Round338 (#719). The gate runs the geometry script and compares what it prints against child
    # counts it derives from the archive itself, so the cases below break the map, the archive and
    # the independence of the two legs -- never a substring in somebody's comment.
    ("N13 the IMEM archive is not there", [(IMEM_ARCHIVE, None, None)]),
    ("N14 the board tree loses the download-mode cell",
     [(IMEM_ARCHIVE, DLOAD_BOARD, "")]),
    ("N15 the script compares the board tree with a copy of itself",
     [(IMEM_ARCHIVE, DLOAD_BOARD, ""),
      (GEOMETRY, '    vendor = parse_source(raw[marks[1]:], "vendor tree")', "    vendor = dict(board)")]),
    ("N16 a claimed cell loses its reg and the map stops being a map",
     [(IMEM_ARCHIVE, BOOT_STATS_BOARD, BOOT_STATS_NO_REG)]),
    # Round338's second leg: the restart_reason table. The gate counts the vendor's writes itself,
    # re-reads the driver's own constant, and refuses a report that quotes either from memory.
    ("N17 the vendor restart-reason source is not there",
     [(POWEROFF_ARCHIVE, None, None)],
     "notes/sources/sdm660/msm-poweroff.c is missing"),
    ("N18 the archived boot_stats.c turns out to write",
     [(BOOT_STATS_ARCHIVE, "static struct boot_stats __iomem *boot_stats;",
       "static struct boot_stats __iomem *boot_stats;\n\nstatic void injected_write(void)\n{\n"
       "\twritel(0, boot_stats);\n}")],
     "the archived boot_stats.c writes something"),
    ("N19 the report's write count is not the file's",
     [(GEOMETRY, '(0x7766[0-9a-fA-F]{4}|0x0)', '(0x7766[0-9a-fA-F]{4})')],
     "while the archived vendor file holds"),
    ("N20 the port's word moves back to the arm the vendor does not label it under",
     [(SDHCI, "static const uint32_t kRestartBootloader = 0x77665500u;",
       "static const uint32_t kRestartBootloader = 0x77665501u;")],
     "labels only"),
    # Round338's other instrument: a burst leg that dies has to say which operand differed. This
    # breaks one expected byte in the arbiter model and judges the GATE OUTPUT, not just its exit
    # code -- a red gate is not the same thing as a gate that names what it saw.
    ("N21 a wrong burst byte names itself",
     [(HARNESS, "    const uint8_t b3[2] = { 0xc0, 0x0b };",
       "    const uint8_t b3[2] = { 0xbf, 0x0b };", 2)],
     "v3 B3 through the PM660L APID: rc=0"),
    # Round339 (#720). Round338 left the naming and the geometry as readings; this round made them
    # requirements about the code, so each case below breaks a requirement rather than a string that
    # describes one. N20 is the nail the rename needed: the value the vendor labels `other` has to
    # turn the gate red by itself, not because a quote moved with it.
    ("N22 a second restart-reason name stays in the driver",
     [(SDHCI, "static const uint32_t kRestartBootloader = 0x77665500u;",
       "static const uint32_t kRestartBootloader = 0x77665500u;\n"
       "static const uint32_t kRestartLegacyFastboot = 0x77665501u;")],
     "restart-reason values"),
    ("N23 the two writers of the restart cell disagree on the word",
     [(PANIC, "#define BOOTLOADER_WORD 0x77665500u", "#define BOOTLOADER_WORD 0x77665502u")],
     "one column would then mean two different previous boots"),
    ("N24 the heartbeat counter moves back into a firmware-owned word",
     [(SDHCI, "  beat[0] = beat[0] + 1;", "  record[10] = record[10] + 1;")],
     "clover_imem_heartbeat writes record[10]"),
    ("N25 the arm-time control stops writing the sentinel",
     [(SDHCI, "  beat[1] = kResetBeatSentinel;", "  beat[1] = kResetBeatMagic;")],
     "CloverSdhci.cpp lacks 'beat[1] = kResetBeatSentinel"),
    ("N26 the restart cell is read only after it is marked",
     [(SDHCI, "  const uint32_t before = cell[0];\n  cell[0] = kRestartBootloader;",
       "  cell[0] = kRestartBootloader;\n  const uint32_t before = cell[0];")],
     "is read after it is written"),
    ("N27 the geometry instrument stops looking at the heartbeat cell",
     [(GEOMETRY, '    out["beat_write_words"] = sorted({int(m.group(1)) for m in '
                 'BEAT_WORDS.finditer(text)})',
       '    out["beat_write_words"] = []')],
     "no beat[N] store was found"),
    ("N28 the heartbeat cell moves onto boot_stats",
     [(SDHCI, "static const uint32_t kResetBeatOffset = 0x6e0;",
       "static const uint32_t kResetBeatOffset = 0x6b0;")],
     "free bytes start there"),
    ("N29 the zeroing loop goes back to a literal the comment supplied",
     [(SDHCI, "  for (unsigned int i = 0; i < kResetRecordWords; ++i) {",
       "  for (unsigned int i = 0; i < 20; ++i) {")],
     "sits inside the record's own span"),
    # Round339's second instrument, added while the suite was being run: the arbiter host model was
    # itself the thing going red, and this time it could say which of its own promises broke. Each case
    # below breaks one of the three halves of that answer -- the release in front of every DONE store,
    # the acquire in front of the reading of the record, and the record being the channel's own rather
    # than a word every serve writes. None of them changes what the model computes, so a green here
    # would mean the gate is not judging the publication order at all.
    ("N30 a DONE store loses its release",
     [(HARNESS, "  __atomic_thread_fence (__ATOMIC_RELEASE);\n  ch[kPmicArbStatus / 4] = kModelHandled "
                "| kPmicArbStatusDone;",
       "  ch[kPmicArbStatus / 4] = kModelHandled | kPmicArbStatusDone;")],
     "times and releases"),
    ("N31 the burst leg stops taking the reader's half of the pair",
     [(HARNESS, "__atomic_thread_fence (__ATOMIC_ACQUIRE);",
       "__atomic_thread_fence (__ATOMIC_SEQ_CST);")],
     "has no acquire"),
    ("N32 the channel's answer is recorded from the shared word again",
     [(HARNESS, "  servedSid[apid] = (int) sid;", "  servedSid[apid] = lastSid;")],
     "shared last-serve word"),
    # Round339b. Boot 120's own verdict sentence is the reason these three exist: it quoted the end of
    # the timeline, and the end of the timeline is a row of the USB tree the writer photographs under
    # the close line. The needle that used to guard that sentence pinned the *expression*, so the
    # expression could be wrong and the gate stayed green -- which is exactly what happened. The gate
    # now runs the shipped helper against synthetic timelines, and the cases below break what only a
    # run can see: the shape of the tail, the reset at each window header, and the helper's existence.
    # None of them touches a substring the ban searches for, so a green here would mean the probe is
    # not the thing judging.
    ("N33 the close is read off the last line of the file again",
     [(SCRIPT,
       '''  awk '/^# watch / { line = "" } NF == 5 && $5 ~ /^close:/ { line = $5 } END { if (line) print line }' \\
    "$TIMELINE" 2>/dev/null''',
       "  tail -n 1 \"$TIMELINE\" 2>/dev/null | awk '{print $5}'")],
     "timeline_last_close on close then the tree under it"),
    ("N34 the scan stops restarting at each window header",
     [(SCRIPT, '''awk '/^# watch / { line = "" } NF == 5''', '''awk 'NF == 5''')],
     "timeline_last_close on an older window's close"),
    ("N35 the helper that quotes the close is gone",
     [(SCRIPT, "timeline_last_close() {", "timeline_last_close_unused() {")],
     "no timeline_last_close() block the gate can run"),

    # #653's geometry agreement. Each of these is one leg of the comparison, and the expect is the
    # sentence only that leg prints: a red gate that came from any other leg would not say which of
    # the three files moved, which is the whole claim the round exists to make.
    # N36 and N41 name SdhciLogCore.h, not CloverSdhci.cpp: round341 moved both texts into the layer,
    # and the harness reported them as NOT TESTED rather than passing a mutation it never applied. A
    # needle whose subject moved is an instrument debt, so it has to be re-pinned in the same round.
    ("N36 the driver's ring table moves against the reader's copy",
     [(SDHCI_LAYER, "kRingOffset[kRingCount] = {64, 192}", "kRingOffset[kRingCount] = {64, 194}")],
     "the driver says [64, 194]"),
    ("N37 the reader's kernel ring shrinks under its own derived offsets",
     [(READER, "KERNEL_SLOTS = 512", "KERNEL_SLOTS = 256")],
     "the reader says [64, 256]"),
    ("N38 the host dd's a sector short of the geometry",
     [(SCRIPT, "by-name/logdump bs=512 count=1217", "by-name/logdump bs=512 count=1216")],
     "the host reads [1216, 1217] sectors"),
    ("N39 the reader's table is parsed out of the driver's file",
     [(SDHCI_GEOMETRY, 'READER = REPO / "scripts/read-clover-log.py"',
       'READER = REPO / "drivers/CloverSdhci/CloverSdhci.cpp"')],
     "declares no VERDICT_OFFSET"),
    ("N40 the roundtrip leg is stubbed and the script still exits 0",
     [(SDHCI_GEOMETRY, "roundtrip = None\nif all (value is not None",
       "roundtrip = None\nif False and all (value is not None")],
     "printed no 'roundtrip: 4 records'"),
    ("N41 the staging-depth comment claims a number the table does not give",
     [(SDHCI_LAYER, "// staging depth: deeper than the driver's card ring, shallower than the kernel's",
       "// staging depth; deeper than either card ring")],
     "kRingRamSlots says 128 is staging depth"),
    ("N42 the reader's main path asks the kernel ring for somebody else's kind",
     [(READER, "slots, base, kind, window = KERNEL_SLOTS, KERNEL_OFFSET, 22,",
       "slots, base, kind, window = KERNEL_SLOTS, KERNEL_OFFSET, 23,")],
     "the driver writes kind 22 to the kernel ring"),

    # Round341 (#653, the sixth pure layer). Four of these are the findings themselves put back: each
    # mutation restores the expression the round replaced, so a green gate would mean the model never
    # measured the difference between the two rules -- which is round330's rule that a negative has to
    # re-run the wrong code, not a synonym of it. The rest judge the split's own failure modes: a second
    # copy in the driver, a layer nobody calls, an instrument that stops printing its new column, and a
    # reader whose bound silently moves against the writer's clamp.
    ("N43 the checksum goes back to summing the bytes as they arrive",
     [(SDHCI_LAYER, "    sum += (uint32_t) sdhci_record_byte ((uint8_t) text[i]);",
       "    sum += (uint32_t) (uint8_t) text[i];")],
     "case filter sum_agrees=0"),
    ("N44 the relay's dropped= counts a position again",
     [(SDHCI_LAYER, "  return (uint32_t) (seq - consumed) >= kProbeSlots ? 1u : 0u;",
       "  return seq >= kProbeSlots ? 1u : 0u;")],
     "case relay drained_dropped=136"),
    ("N45 the tee's long line goes back to splitting and destroying",
     [(SDHCI_LAYER, "    return truncated ? kSdhciTeeEmitCut : kSdhciTeeEmit;",
       "    return truncated ? kSdhciTeeStore : kSdhciTeeEmit;")],
     "case tee_cut cut=0"),
    ("N46 the panic record's kind moves off the reader's expectation",
     [(SDHCI_LAYER, "static const uint32_t kPanicKind = 20;",
       "static const uint32_t kPanicKind = 21;")],
     "case panic kind=21"),
    ("N47 the driver keeps a second copy of the ring's slot table",
     [(SDHCI, "static const uint32_t kTeeCensusMax = 12;",
       "static const uint32_t kRingCardSlots[kRingCount] = {64, 512};\n"
       "static const uint32_t kTeeCensusMax = 12;")],
     "still carries 'static const uint32_t kRingCardSlots'"),
    ("N48 the panic sink's own copy of the byte sum comes back",
     [(SDHCI, "  sdhci_build_panic_header (gWriteBlock, lba, slot, maxSeq + 1, gBootId, text, length);",
       "  for (uint32_t i = 0; i < length; i++) { sum += static_cast<uint8_t> (text[i]); }\n"
       "  sdhci_build_panic_header (gWriteBlock, lba, slot, maxSeq + 1, gBootId, text, length);")],
     "still carries 'sum += static_cast<uint8_t> (text[i]);'"),
    ("N49 the panic path stops calling the layer",
     [(SDHCI, "  sdhci_build_panic_header (gWriteBlock, lba,",
       "  panicWriteWords (gWriteBlock, lba,")],
     "does not call 'sdhci_build_panic_header ('"),
    ("N50 the tee census stops printing its truncation count",
     [(SDHCI, '              " cut=%u bound=%u\\n",', '              "\\n",')],
     'lacks \'" cut=%u bound=%u'),
    ("N51 the reader's body bound moves against the writer's clamp",
     [(READER, "        if body > 480:", "        if body > 400:")],
     "the writer clamps a body at 480 while the reader's own bounds say"),
    ("N52 the reader's tee sample row is re-typed instead of derived",
     [(READER, '    for column in ("cut=", "bound="):', '    for column in ():')],
     'lacks \'for column in ("cut=", "bound="):\''),
    ("N53 the pure layer is not on disk at all",
     [(SDHCI_LAYER, None, None)],
     "SdhciLogCore.h is missing"),
    # N36 found this while it was being written, so it stays as a case: the geometry script's array
    # parser was unanchored and matched a comment two lines above the table that restates it. Only
    # un-anchoring the parser does not turn the gate red -- with the real table and the prose agreeing
    # the reading is still right -- so the second edit moves the prose alone. Red then means the leg
    # read a number out of a sentence, which is round328's shape (prose is not a number's source).
    ("N54 the ring table is parsed from the comment that quotes it, not the declaration",
     [(SDHCI_GEOMETRY,
       r'    found = re.search (rf"^\s*(?:static const \w+ )?\b{name}\s*(?:\[[^\]]*\])?\s*=\s*\{{([^}}]*)\}}",',
       r'    found = re.search (rf"\b{name}\s*(?:\[[^\]]*\])?\s*=\s*\{{([^}}]*)\}}",'),
      (SDHCI_LAYER, "kRingOffset = {64, 192}", "kRingOffset = {64, 193}")],
     "the driver says [64, 193]"),
    # Round341's sixth finding: the reader's byte filter and the writer's were different sets. The two
    # legs judge it two ways, so each needs its own case -- the mask comparison (behaviour, run) and the
    # ban on the old expression (a shape). N55 moves the reader's range one byte so only the mask can
    # see it; N56 puts the deleted expression back so the ban must be the one that says so; N57 renames
    # the model's row, because a leg that stopped printing must not look like a leg that disagrees.
    ("N55 the reader's byte range narrows against the writer's keep-set",
     [(READER, "byte == 0x0A or 0x20 <= byte < 0x7F", "byte == 0x0A or 0x21 <= byte < 0x7F")],
     "the reader and the writer disagree about 1 of 256 bytes"),
    ("N56 the reader goes back to keeping the control bytes the writer replaces",
     [(READER, "byte == 0x0A or 0x20 <= byte < 0x7F", "0x09 <= byte < 0x7F")],
     "still keeps 0x09 and 0x0b through 0x1f"),
    ("N57 the model stops printing its keep-set mask",
     [(SDHCI_MODEL, '"case keepset keepmask=%s kept=%u filler=%u\\n"',
       '"case keepset-renamed keepmask=%s kept=%u filler=%u\\n"')],
     "printed no case keepset"),
    # The defect the build caught and this suite did not: round341's extraction left the local that had
    # held the body clamp behind, -Werror stopped archive 11, and every case above stayed green because
    # neither the gate nor the geometry script compiles the driver's own translation unit. Leg 4b asks
    # the question, so N58 puts the dead local back -- the script has to name it, not merely fail -- and
    # N59 removes the line carrying its denominator, because a leg whose scan size nobody prints could
    # be scanning nothing and reporting clean.
    ("N58 the driver keeps a local from a layer call that nothing reads",
     [(SDHCI, "  const uint32_t lba = sdhci_ring_lba (gVerdictFirst, which, gRingCursor[which], pin);",
       "  const uint32_t body = sdhci_record_body (length);\n"
       "  const uint32_t lba = sdhci_ring_lba (gVerdictFirst, which, gRingCursor[which], pin);")],
     "the local body is declared from a call into the layer and never read again"),
    ("N59 the locals leg stops printing what it scanned",
     [(SDHCI_GEOMETRY, '"driver locals: {len (locals_declared)} bound from a layer call',
       '"driver localities: {len (locals_declared)} bound from a layer call')],
     "printed no driver locals line"),
    # Round342's registered cases. The instrument being judged is a slot rule, so each case breaks one
    # link and asks which leg notices. N60 renames a pinned head in the layer AND in the reader, so the
    # two declarations still agree and no textual leg can see it: the row the model pushes under its old
    # name no longer matches a head, lands rolling, and the pinned slot stays empty. Only the model's
    # read-back can say that. N68 renames it in the layer alone, which is the cross-file leg's own
    # disagreement -- the first version of N60 did this and was red for that leg, not for the model, so
    # the two shapes are registered separately rather than one case wearing the other's sentence.
    # N61 leaves the pins in place but lets the rolling modulus run through them, which is the quiet
    # direction (a pinned row is overwritten by a periodic one and both words still look legal). N62
    # moves the count away from the table, the one-number-three-homes leg's own disagreement.
    ("N60 a pinned head is renamed in both declarations, so only the card can see the row lose its home",
     [(SDHCI_LAYER, 'static const char kPinHeadMark[] = "clover r339: mark";',
       'static const char kPinHeadMark[] = "clover r339: markx";'),
      (READER, '    "clover r339: mark",', '    "clover r339: markx",')],
     "a pinned slot held no record"),
    ("N61 the rolling modulus runs through the pinned slots",
     [(SDHCI_LAYER, "  return floor + cursor % (kRingCardSlots[which] - floor);",
       "  return floor + cursor % kRingCardSlots[which];")],
     "this is a bound, a cursor rule or a checksum that the host model proves"),
    ("N62 the pinned count and the pinned table are two decisions",
     [(SDHCI_LAYER, "static const uint32_t kRingPinnedSlots = 4;",
       "static const uint32_t kRingPinnedSlots = 3;")],
     "the pinned table lists 4 heads while kRingPinnedSlots is 3"),
    # The same arithmetic seen from the reader's side: the pinned layout lives in four declarations
    # across two files, and N61/N62/N68 mutate the layer alone. None of them can make a reader whose
    # own count disagrees with the layer go red.
    ("N63 the reader's pinned count moves away from the layer's",
     [(READER, "PINNED_SLOTS = 4", "PINNED_SLOTS = 3")],
     "the pinned layout is four numbers, not one"),
    # The second half is boot 121's card: the reader used to open the slot the rule gives the arming
    # row and ask that row which rule the card follows. Each case below breaks one link of the
    # replacement and expects the gate to name that link -- the phrase is the gate's own sentence, so
    # a red that came from a traceback quoting the reader would not count.
    ("N64 the reader walks the ring with the pre-342 semantics that stop at the first gap",
     [(READER, "pinned=PINNED_SLOTS)", "pinned=None)")],
     "does not walk the ring with round342's own semantics"),
    ("N65 the reader picks the declaring row by its slot instead of its head",
     [(READER, 'content.startswith (head.encode ("latin-1"))', "slot == head_slot")],
     "does not identify the declaring row by its own head"),
    ("N66 the reader has no sentence for a card that holds no declaring row",
     [(READER, '"no r174 arming row lies anywhere in this ring, so this card declares nothing: "',
       '"the ring holds nothing that declares itself, so this card says nothing: "')],
     "has no sentence for a ring that holds no declaring row"),
    ("N67 the reader's ring-rule leg is written but never run",
     [(READER, "    card_pinned_selftest()\n", "")],
     "does not run its ring-rule leg"),
    ("N68 a pinned head is renamed in the layer alone, so the reader's table names a row nobody writes",
     [(SDHCI_LAYER, 'static const char kPinHeadMark[] = "clover r339: mark";',
       'static const char kPinHeadMark[] = "clover r339: markx";')],
     "the reader pins heads the layer does not"),
    # Round343 (#735). The reader's ring-rule leg above is an existence-and-shape test: it proves the
    # sentence exists and names both branches. It cannot prove the sentence is *printed*, and boot 122
    # is the card that showed the difference -- the criterion asked which declaration the read used and
    # silence satisfied it. These three break the printing, the fallback's source name and the fallback's
    # refusal to claim a count. Each expects the reader's own assertion sentence, which runs before the
    # shape pins, so a red from the pins instead would mean the behavioral leg did not judge.
    ("N69 the ring-rule sentence goes back to printing only on the branch it worries about",
     [(READER, "        print (ring_rule_sentence (pinned, why), file=sys.stderr)",
       "        if why:\n            print (ring_rule_sentence (pinned, why), file=sys.stderr)")],
     "the driver ring has to say which rule read it, on every read"),
    ("N70 the fallback names the card as its source, so an assumption reads as a declaration",
     [(READER, 'source = "this reader\'s fallback: the card declares nothing readable"',
       'source = "the card\'s own r174 arming row"')],
     "the reader would report its own assumption as the card's declaration"),
    ("N71 the fallback prints a pinned count this card never declared",
     [(READER, 'used = "round341 congruence (slot == seq % slots)"',
       'used = f"round341 congruence pinned={pinned} (slot == seq % slots)"')],
     "the fallback sentence claims a pinned count"),
    # Round343 (#732). Detection and the write were two decisions: the selectors answered with the first
    # usable row while every flash carried no -s and went wherever the environment named. The probe runs
    # the four functions against a synthesized bus, so these expect the fixture's own name -- an exit 1
    # from a shape pin instead would mean the probe never noticed the wrong board being answered for.
    ("N72 the fastboot selector goes back to first-match and ignores the declared serial",
     [(SCRIPT, '    awk -v want="$SERIAL" \'$2 == "fastboot" && (want == "" || $1 == want) {print $1; exit}\'',
       '    awk \'$2 == "fastboot" {print $1; exit}\'')],
     "board-selection probe [two boards, --serial names the one in recovery]"),
    ("N73 an ambiguous bus is no longer refused",
     [(SCRIPT, '    if [ "$count" -gt 1 ]; then', '    if [ "$count" -gt 99 ]; then')],
     "board-selection probe [two boards, nothing declared]"),
    # A variable merely assigned in this shell is still visible to this shell, which is how the first
    # draft of this fixture passed a mutation that broke the export. The probe reads EXPORT through an
    # exec'd child because adb and fastboot are exec'd children.
    ("N74 the resolved serial is assigned but not exported, so the tools never see it",
     [(SCRIPT, '    export ANDROID_SERIAL="$SERIAL"', '    ANDROID_SERIAL="$SERIAL"')],
     "board-selection probe [one board in fastboot, nothing declared]"),
    ("N75 a watch resolves the board only at startup, not on its own polls",
     [(SCRIPT, "    require_serial\n    a=$(adb_serial); f=$(fastboot_serial)",
       "    a=$(adb_serial); f=$(fastboot_serial)")],
     "watch_landing() does not resolve the board on its polls"),
    ("N76 the refusal is moved into a command substitution, where its exit code dies with the subshell",
     [(SCRIPT, "    found=$(fastboot_serial); a=$(adb_serial)",
       "    found=$(require_serial; fastboot_serial); a=$(adb_serial)")],
     "resolves the board inside a command substitution"),
    ("N77 the verdict leg discards the decoder's stderr, so the ring-rule sentence is never archived",
     [(SCRIPT, 'python3 "$decoder" --verdict "$OUT" 2>&1',
       'python3 "$decoder" --verdict "$OUT" 2>/dev/null')],
     "does not keep the decoder's stderr on the verdict leg"),
    ("N78 the top-level resolution is dropped, so the first write runs before any declaration",
     [(SCRIPT, 'require_serial\nsay "board:', 'say "board:')],
     "never resolves the board at top level"),
    # Round343 (#736). The swap row's column names are walked down to the offset that fills them, so
    # each link of that chain gets its own break: the declaration, the operand count, the operand a
    # column reads, the table's names, the table's numbers, the table's reach, the state row's three
    # memcpy/snprintf/order pins, and the three banned old names.
    ("N79 the swap row's offset enum is renamed, so no column can be tied to its storage",
     [(PROBE, "enum {kLatchOffset=5820,kCopyOffset=5824,kFcOffset=252,kCurOffset=5472,kListOffset=5480};",
       "enum {kzzLatchOffset=5820,kCopyOffset=5824,kFcOffset=252,kCurOffset=5472,kListOffset=5480};")],
     "offset chain is not declared"),
    ("N80 the swap row drops one operand, so the columns and the values under them are two lists",
     [(PROBE, "pre.queryTxt[0],pre.queryTxt[1],", "pre.queryTxt[0],")],
     "every conversion in this row takes one operand"),
    ("N81 the swap row's latch= and copy= operands are transposed, so latch= reads the copy buffer",
     [(PROBE, "pre.argTxt,pre.dispTxt[0],pre.dispTxt[1],pre.dispTxt[2],pre.dispTxt[3],pre.dispTxt[4],",
       "pre.argTxt,pre.dispTxt[1],pre.dispTxt[0],pre.dispTxt[2],pre.dispTxt[3],pre.dispTxt[4],")],
     "prints latch= from pre.dispTxt[1]"),
    ("N82 the offset table names an offset no column reads, so the table's order is not the row's",
     [(PROBE, "kDisplayOffsets[5]={kLatchOffset,kCopyOffset,kFcOffset,kCurOffset,kListOffset};",
       "kDisplayOffsets[5]={kLatchOffset,kCopyOffset,kZzOffset,kCurOffset,kListOffset};")],
     "names no column of the r343 swap row"),
    ("N83 kLatchOffset is off by one, so the column would name a word the writer never stored there",
     [(PROBE, "enum {kLatchOffset=5820,kCopyOffset=5824", "enum {kLatchOffset=5821,kCopyOffset=5824")],
     "the stock kernel's latch writer stores"),
    ("N84 the list= operand is repointed, so the table's fifth entry fills no operand of the row",
     [(PROBE, "pre.dispTxt[3],pre.dispTxt[4],", "pre.dispTxt[3],pre.queryTxt[0],")],
     "fills no operand of the r343 swap row"),
    ("N85 the state row's latch memcpy reads the copy offset, so latch= is named for an offset it drops",
     [(PROBE, "memcpy(&latchValue,object+kLatchOffset,4);", "memcpy(&latchValue,object+kCopyOffset,4);")],
     "does not fill latch= through"),
    ("N86 the state row prints copy= before fc= is swapped, so the two display rows disagree on order",
     [(PROBE, '"hot=%s s6=%s latch=%s copy=%s fc=%s vs=%u', '"hot=%s s6=%s latch=%s fc=%s copy=%s vs=%u')],
     "does not print latch= copy= fc= in that order"),
    ("N87 the state row's operands are latchTxt,fcTxt,copyTxt, so the names and buffers are two lists",
     [(PROBE, "s6Txt,latchTxt,copyTxt,fcTxt,", "s6Txt,latchTxt,fcTxt,copyTxt,")],
     "operands are not latchTxt,copyTxt,fcTxt"),
    ("N88 the swap row brings back done=%s, a word whose producer is a different offset",
     [(PROBE, "arg=%s latch=%s copy=%s fc=%s cur=%s", "arg=%s done=%s copy=%s fc=%s cur=%s")],
     "prints 'done=%s'"),
    ("N89 the swap row brings back presented=%s, the latch copied -- a second name for a column beside it",
     [(PROBE, "latch=%s copy=%s fc=%s cur=%s", "latch=%s presented=%s fc=%s cur=%s")],
     "prints 'presented=%s'"),
    ("N90 the swap row brings back frames=%s, which named the count the port never presented",
     [(PROBE, "fc=%s cur=%s list=%s pw=%s", "fc=%s frames=%s list=%s pw=%s")],
     "prints 'frames=%s'"),
]


def run_gate():
    result = subprocess.run([sys.executable, str(REPO / "scripts/check-clover.py")],
                            capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def first_failure(out):
    """The whole first red entry of a gate run, detail lines included.

    The arbiter leg prints its own sentence and then the tail of the model's output, so a report cut
    to one line says "a host model failed" and leaves the next round to guess which of its legs. The
    tail is the reading.
    """
    lines = out.splitlines()
    for index, line in enumerate(lines):
        if not line.startswith("[check-clover] "):
            continue
        if any(mark in line for mark in ("notice:", "clover ok", "host model: PASS")):
            continue
        detail = []
        for following in lines[index + 1:]:
            if following.startswith("[check-clover] "):
                break
            detail.append(following)
        return "\n      ".join(([line[:300]] + detail[-6:])[:7])
    return ""


def replace_nth(text, old, new, occurrence):
    """Mutate the nth copy of `old`, so a needle that counts occurrences can be judged red.

    Two silent verdict sentences quote the timeline today, so replacing only the first one leaves a
    file with one quote and one re-measurement -- a bool needle would still pass. The occurrence is
    counted, not guessed from the surrounding indentation.
    """
    start = -1
    for _ in range(occurrence):
        start = text.find(old, start + 1)
        if start < 0:
            return None
    return text[:start] + new + text[start + len(old):]


def main():
    LOG.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    problems = 0
    for case in CASES:
        name = case[0]
        edits = case[1] if isinstance(case[1], list) else [list(case[1:])]
        # Some cases are not satisfied by a red gate: round338 added an instrument whose whole job is
        # to name the operand it saw, so a red that came from anywhere else proves nothing. `expect`
        # is the substring that only that instrument can print.
        expect = case[2] if isinstance(case[1], list) and len(case) > 2 else None
        originals = {}
        applied = True
        for path, old, new, *rest in edits:
            occurrence = rest[0] if rest else 1
            originals[path] = path.read_text(encoding="utf-8") if path.is_file() else None
            if old is None:
                path.unlink(missing_ok=True)
                continue
            mutated = replace_nth(originals[path], old, new, occurrence)
            if mutated is None:
                lines.append(f"{name}: NOT TESTED -- {path.name} has no occurrence {occurrence} of "
                             f"{old[:60]!r}")
                problems += 1
                applied = False
                break
            path.write_text(mutated)
        if not applied:
            for path, text in originals.items():
                if text is not None:
                    path.write_text(text)
            continue
        try:
            code, out = run_gate()
            first = first_failure(out)
            if code == 0:
                problems += 1
                lines.append(f"{name}: GREEN (exit 0) -- the needle does not judge")
            elif expect and expect not in out:
                problems += 1
                lines.append(f"{name}: RED, but not for itself -- {expect!r} never appears in the "
                             "gate's output, so some other needle judged this case")
            else:
                lines.append(f"{name}: RED (exit {code})"
                             + (f" and named itself ({expect!r})" if expect else "")
                             + f"\n    {first}")
        except Exception as error:
            problems += 1
            lines.append(f"{name}: EXCEPTION {error}")
            traceback.print_exc()
        finally:
            for path, text in originals.items():
                if text is None:
                    path.unlink(missing_ok=True)
                else:
                    path.write_text(text)
        code, out = run_gate()
        # One boolean used to cover two different failures: a gate that stayed red after the sources
        # went back (a claim about this machine, or about a case that ran before it) and a file the
        # restore did not actually put back (a claim that this harness is lying about cleaning up).
        still_mutated = [path.name for path, text in originals.items()
                         if text is not None
                         and path.read_text(encoding="utf-8", errors="replace") != text]
        if code != 0 or still_mutated:
            problems += 1
            first = first_failure(out)
            files = ("still mutated: " + ", ".join(still_mutated)) if still_mutated \
                else "byte-identical"
            lines.append(f"    RESTORE BROKEN: gate exit={code}, restored files {files}"
                         + (f"\n      {first}" if code else ""))
        else:
            lines.append("    restored: gate exit=0, file byte-identical")
    LOG.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"negatives: {len(CASES)} cases, {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
