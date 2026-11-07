#!/usr/bin/env python3
"""Negative tests for the #716, #718 and #719 needles in scripts/check-clover.py.

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
HARNESS = REPO / "drivers/CloverPower/tests/clover_power_host.cpp"
GATE = REPO / "scripts/check-clover.py"
LOG = Path.home() / "clover-out/round338-sdam-facts/negatives.log"

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
    ("N4 verdict re-measures instead of quoting the close line", SCRIPT,
     'closing=$(tail -n 1 "$TIMELINE"', 'closing=$(usb_landing_class'),
    ("N5 reader loses one watch key", READER, '    "wait-adb": {', '    "wait-adb-renamed": {'),
    ("N6 reader stops running its timeline leg", READER, "    timeline_selftest()\n", ""),
    # Round337 (#718).
    ("N7 poll stops asking the class", SCRIPT,
     "now=$(date +%s); class=$(usb_landing_class)", "now=$(date +%s); class=no-node"),
    ("N8 an empty host answer becomes a quiet bus", SCRIPT,
     "printf 'ioreg-fail'", "printf 'no-node'"),
    ("N9 poll row drops the class column", SCRIPT,
     POLL_PRINTF, """    printf '%s %s %s %s %s\\n' "$now" "$(( now - TIMELINE_START ))" "${adb_now:--}" "${fb_now:--}" \\"""),
    ("N10 the second silent verdict stops quoting the file", SCRIPT,
     'closing=$(tail -n 1 "$TIMELINE"', 'closing=$(usb_landing_class', 2),
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
    ("N20 the port's word becomes the vendor's bootloader value",
     [(SDHCI, "static const uint32_t kRestartFastboot = 0x77665501u;",
       "static const uint32_t kRestartFastboot = 0x77665500u;"),
      (GATE, '"static const uint32_t kRestartFastboot = 0x77665501u;",',
       '"static const uint32_t kRestartFastboot = 0x77665500u;",')],
     "round338's disagreement is closed"),
    # Round338's other instrument: a burst leg that dies has to say which operand differed. This
    # breaks one expected byte in the arbiter model and judges the GATE OUTPUT, not just its exit
    # code -- a red gate is not the same thing as a gate that names what it saw.
    ("N21 a wrong burst byte names itself",
     [(HARNESS, "    const uint8_t b3[2] = { 0xc0, 0x0b };",
       "    const uint8_t b3[2] = { 0xbf, 0x0b };", 2)],
     "v3 B3 through the PM660L APID: rc=0"),
]


def run_gate():
    result = subprocess.run([sys.executable, str(REPO / "scripts/check-clover.py")],
                            capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


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
            first = next((line for line in out.splitlines()
                          if line.startswith("[check-clover] ")
                          and "notice:" not in line and "clover ok" not in line
                          and "host model: PASS" not in line), "")
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
                             + f"\n    {first[:240]}")
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
        if code != 0 or any(path.read_text(encoding="utf-8", errors="replace") != text
                            for path, text in originals.items() if text is not None):
            problems += 1
            lines.append(f"    RESTORE BROKEN: exit={code}")
        else:
            lines.append("    restored: gate exit=0, file byte-identical")
    LOG.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"negatives: {len(CASES)} cases, {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
